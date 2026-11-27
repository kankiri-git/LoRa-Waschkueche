// Waschkueche: LoRa-Reichweitentest fuer zwei M5Stack Unit C6L.
// Reines LoRa P2P, kein LoRaWAN, kein Meshtastic. Eine Firmware fuer beide Geraete.
//
// Rollen (umschalten: Taster 3 s halten, wird gespeichert):
//   WOHN  Basis in der Wohnung. Hoert zu, antwortet auf jeden Ping, haengt im WLAN
//         (Statusseite http://waschkueche-wohn.local/, OTA-Update). Vorgabe nach dem Flashen.
//   WASC  Sender in der Waschkueche. Schickt regelmaessig einen Ping, zeigt beide Richtungen an. Kein WLAN.
// Taster kurz an WASC: neuer Messpunkt (Statistik auf null, WOHN legt eine neue Zeile an).
//
// Funk: 869,525 MHz im Band 869,40-869,65 MHz (max. 500 mW ERP, 10 % Sendezeit),
// SF12, 125 kHz, CR 4/5, 22 dBm. Jedes Geraet sendet hoechstens etwa 5 % der Zeit.
//
// Anzeige WASC:  hin = Waschkueche -> Wohnung (gemessen bei WOHN), zur = Wohnung -> Waschkueche.
//                Werte: RSSI in dBm, SNR in dB. Unten: schnellster Spreizfaktor mit 5 dB Reserve.
//
// WLAN-Zugang in secrets.h (Vorlage secrets.h.example). Ohne secrets.h laeuft WOHN ohne WLAN.

#include <Arduino.h>
#include <SPI.h>
#include <Preferences.h>
#include <RadioLib.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <time.h>
#include "c6l_hw.h"

#if __has_include("secrets.h")
#include "secrets.h"
#define HAT_WLAN 1
#else
#define HAT_WLAN 0
#endif

#define FW_VERSION "0.2.1"
#define OLED_DREHUNG U8G2_R2   // 0.1.0 stand auf dem Kopf (R0)
#define HOSTNAME "waschkueche-wohn"
#define ZEITZONE "CET-1CEST,M3.5.0,M10.5.0/3"

// ---- Funkparameter ----
constexpr float FREQ_MHZ = 869.525f;
constexpr float BW_KHZ = 125.0f;
constexpr uint8_t SF = 12;
constexpr uint8_t CR = 5;              // 4/5
constexpr int8_t TX_DBM = 22;
constexpr uint16_t PREAMBEL = 16;
constexpr uint8_t SYNC_WORD = 0x12;    // privat; Meshtastic nutzt 0x2B, LoRaWAN 0x34
constexpr uint32_t PING_MIN_MS = 20000;
constexpr uint32_t SENDEZEIT_FAKTOR = 20;  // Abstand >= 20 x Sendedauer -> <= 5 % Sendezeit
constexpr float RESERVE_DB = 5.0f;
constexpr int16_t KEIN_WERT = INT16_MIN;

enum Rolle : uint8_t { WOHN = 0, WASC = 1 };
enum Typ : uint8_t { PING = 1, PONG = 2 };

// 15 Byte kosten bei SF12 genauso viel Sendezeit wie 13, deshalb passen die Zusatzfelder umsonst hinein.
struct __attribute__((packed)) Paket {
  uint8_t magic[2];   // 'W','K'
  uint8_t typ;        // PING / PONG
  uint8_t sf;
  uint16_t seq;
  int16_t rssi10;     // PONG: RSSI des Pings bei WOHN x10.  PING: RSSI der letzten Antwort bei WASC x10
  int16_t snr10;      // PONG: SNR des Pings bei WOHN x10.   PING: SNR der letzten Antwort bei WASC x10 (KEIN_WERT)
  uint8_t punkt;      // PING: Messpunkt-Nummer bei WASC
  uint16_t okZur;     // PING: Antworten, die WASC an diesem Messpunkt bekommen hat
  uint16_t gesendet;  // PING: Pings, die WASC an diesem Messpunkt gesendet hat (mit diesem)
};
static_assert(sizeof(Paket) == 15, "Paketgroesse");

SX1262 radio = new Module(c6l::LORA_CS, c6l::LORA_DIO1, RADIOLIB_NC, c6l::LORA_BUSY, SPI);
U8G2_SSD1306_64X48_ER_F_4W_HW_SPI oled(OLED_DREHUNG, c6l::OLED_CS, c6l::OLED_DC, c6l::OLED_RST);
Preferences prefs;
WebServer web(80);

Rolle rolle = WOHN;
bool expanderOk = false;
int funkStatus = RADIOLIB_ERR_NONE;
volatile bool dio1 = false;
bool sendetGerade = false;
uint32_t sendedauerMs = 0;
uint32_t pingAbstandMs = PING_MIN_MS;
uint32_t pongWarteMs = 3000;
bool otaLaeuft = false;
bool netzGestartet = false;

struct Richtung {
  bool da = false;
  float rssi = 0, snr = 0, snrMin = 99, snrSumme = 0;
  uint32_t n = 0;
};

struct Stat {
  uint32_t gesendet = 0, ok = 0, weg = 0, kaputt = 0;
  Richtung hin;       // Waschkueche -> Wohnung
  Richtung zur;       // Wohnung -> Waschkueche
  uint16_t letzteSeq = 0;
  bool seqDa = false;
  uint32_t letzterEmpfangMs = 0;
};
Stat st;

// nur WASC
uint16_t seq = 0;
uint8_t punktNr = 1;
bool wartetAufPong = false;
uint32_t pongBisMs = 0;
uint32_t naechsterPingMs = 3000;
bool zurNeu = false;
int16_t zurRssi10 = 0, zurSnr10 = KEIN_WERT;

// nur WOHN: Messpunkte und Protokoll fuer die Statusseite
// Zaehler von WASC (gesendet, okZur) laufen je Messpunkt hoch. gesendet0/okZur0 sind ihr Stand vor dem ersten
// Ping, den WOHN in diesem Eintrag gesehen hat; so stimmen die Quoten auch nach einem Neustart von WOHN.
struct Punkt {
  uint8_t nr = 0;
  time_t beginn = 0, ende = 0;
  uint32_t beginnMs = 0, endeMs = 0, maxLueckeMs = 0;
  uint16_t empfangen = 0, gesendetWasc = 0, okZur = 0, gesendet0 = 0, okZur0 = 0, kaputt = 0;
  float hinRssiSumme = 0, hinSnrSumme = 0, hinSnrMin = 99;
  float zurRssiSumme = 0, zurSnrSumme = 0, zurSnrMin = 99;
  uint16_t nZur = 0;
};
constexpr int MAX_PUNKTE = 24;
Punkt punkte[MAX_PUNKTE];
int punkteAnzahl = 0;   // gesamt; Index = (anzahl - 1) % MAX_PUNKTE ist der aktuelle

// Dasselbe je Stunde und Messpunkt, fuer den Dauertest ueber Nacht
struct Stunde {
  uint32_t schluessel = 0;
  uint8_t punkt = 0;
  bool ersterDesPunkts = false;
  time_t beginn = 0;
  uint32_t beginnMs = 0, maxLueckeMs = 0;
  uint16_t empfangen = 0, gesendetStart = 0, gesendetLetzt = 0, okStart = 0, okLetzt = 0, kaputt = 0;
  float hinRssiSumme = 0, hinSnrSumme = 0, hinSnrMin = 99, zurSnrMin = 99;
  uint16_t nZur = 0;
};
constexpr int MAX_STUNDEN = 48;
Stunde stunden[MAX_STUNDEN];
int stundenAnzahl = 0;

struct LogZeile {
  time_t t;
  uint32_t ms;
  char text[52];
};
constexpr int MAX_LOG = 160;
LogZeile protokoll[MAX_LOG];
int logAnzahl = 0;

uint32_t ledAusMs = 0;
char hinweis[16] = "";
uint32_t hinweisBisMs = 0;

void IRAM_ATTR aufDio1() { dio1 = true; }

// ---------------------------------------------------------------- Hilfen

void led(uint8_t r, uint8_t g, uint8_t b, uint32_t ms) {
  rgbLedWrite(c6l::PIN_LED, r, g, b);
  ledAusMs = ms ? millis() + ms : 0;
}

void zeigeHinweis(const char* text, uint32_t ms = 2000) {
  strncpy(hinweis, text, sizeof(hinweis) - 1);
  hinweis[sizeof(hinweis) - 1] = 0;
  hinweisBisMs = millis() + ms;
}

void merke(Richtung& r, float rssi, float snr) {
  r.da = true;
  r.rssi = rssi;
  r.snr = snr;
  if (snr < r.snrMin) r.snrMin = snr;
  r.snrSumme += snr;
  r.n++;
}

// Schnellster Spreizfaktor, der mit der schlechtesten SNR noch RESERVE_DB Luft hat.
// SX1262-Grenze: SF7 -7,5 dB ... SF12 -20 dB (2,5 dB je Stufe). >12 = zu knapp.
int sfFuer(float snrMin) {
  int sf = (int)ceilf(4.0f + (RESERVE_DB - snrMin) / 2.5f);
  return sf < 7 ? 7 : sf;
}

int sfMoeglich() {   // 0 = noch keine Daten
  bool daten = false;
  float m = 99;
  if (st.hin.da) { daten = true; m = min(m, st.hin.snrMin); }
  if (st.zur.da) { daten = true; m = min(m, st.zur.snrMin); }
  return daten ? sfFuer(m) : 0;
}

const char* rollenName(Rolle r) { return r == WASC ? "WASC" : "WOHN"; }

bool zeitGueltig() { return time(nullptr) > 1700000000; }

void zeitText(time_t t, uint32_t ms, char* buf, size_t n) {
  if (t > 1700000000) {
    struct tm lt;
    localtime_r(&t, &lt);
    strftime(buf, n, "%d.%m. %H:%M:%S", &lt);
  } else {
    snprintf(buf, n, "+%lus", (unsigned long)(ms / 1000));
  }
}

void logge(const char* fmt, ...) {
  LogZeile& z = protokoll[logAnzahl % MAX_LOG];
  z.t = zeitGueltig() ? time(nullptr) : 0;
  z.ms = millis();
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(z.text, sizeof(z.text), fmt, ap);
  va_end(ap);
  logAnzahl++;
}

Punkt* aktuellerPunkt() { return punkteAnzahl ? &punkte[(punkteAnzahl - 1) % MAX_PUNKTE] : nullptr; }
Stunde* aktuelleStunde() { return stundenAnzahl ? &stunden[(stundenAnzahl - 1) % MAX_STUNDEN] : nullptr; }
unsigned hinGesendet(const Punkt& a) { return (uint16_t)(a.gesendetWasc - a.gesendet0); }
unsigned zurOk(const Punkt& a) { return (uint16_t)(a.okZur - a.okZur0); }
unsigned zurBasis(const Punkt& a) { return a.empfangen ? a.empfangen - 1 : 0; }
unsigned hinGesendet(const Stunde& h) { return (uint16_t)(h.gesendetLetzt - h.gesendetStart); }
unsigned zurOk(const Stunde& h) { return (uint16_t)(h.okLetzt - h.okStart); }
unsigned zurBasis(const Stunde& h) { return h.empfangen - (h.ersterDesPunkts && h.empfangen ? 1 : 0); }

// ---------------------------------------------------------------- Funk

void starteEmpfang() {
  dio1 = false;
  int s = radio.startReceive();
  if (s != RADIOLIB_ERR_NONE) Serial.printf("# startReceive Fehler %d\n", s);
}

void sende(const Paket& p) {
  dio1 = false;
  sendetGerade = true;
  int s = radio.startTransmit((uint8_t*)&p, sizeof(p));
  if (s != RADIOLIB_ERR_NONE) {
    Serial.printf("# startTransmit Fehler %d\n", s);
    sendetGerade = false;
    starteEmpfang();
  }
}

Paket neuesPaket(Typ typ, uint16_t s) {
  Paket p{};
  p.magic[0] = 'W';
  p.magic[1] = 'K';
  p.typ = typ;
  p.sf = SF;
  p.seq = s;
  p.snr10 = KEIN_WERT;
  return p;
}

// WOHN: Ping in den Messpunkt-Verlauf eintragen
void bucheMesspunkt(const Paket& p, float rssi, float snr) {
  Punkt* a = aktuellerPunkt();
  bool neu = !a || a->nr != p.punkt || (st.seqDa && p.seq <= st.letzteSeq);
  if (neu) {
    if (a) st = Stat();   // Anzeige folgt dem Messpunkt von WASC
    a = &punkte[punkteAnzahl % MAX_PUNKTE];
    *a = Punkt();
    a->nr = p.punkt;
    a->beginn = zeitGueltig() ? time(nullptr) : 0;
    a->beginnMs = millis();
    a->gesendet0 = p.gesendet ? p.gesendet - 1 : 0;
    a->okZur0 = p.okZur;
    punkteAnzahl++;
    logge("Messpunkt %u beginnt", (unsigned)p.punkt);
    Serial.printf("WOHN,MESSPUNKT,%u\n", (unsigned)p.punkt);
  }
  uint32_t jetzt = millis();
  bool ersterPing = a->empfangen == 0;
  uint32_t luecke = ersterPing ? 0 : jetzt - a->endeMs;
  if (luecke > a->maxLueckeMs) a->maxLueckeMs = luecke;
  uint16_t gesVorher = ersterPing ? a->gesendet0 : a->gesendetWasc;
  uint16_t okVorher = ersterPing ? a->okZur0 : a->okZur;

  uint32_t schluessel = zeitGueltig() ? (uint32_t)(time(nullptr) / 3600) : jetzt / 3600000UL;
  Stunde* h = aktuelleStunde();
  if (!h || neu || h->schluessel != schluessel || h->punkt != p.punkt) {
    h = &stunden[stundenAnzahl % MAX_STUNDEN];
    *h = Stunde();
    h->schluessel = schluessel;
    h->punkt = p.punkt;
    h->ersterDesPunkts = ersterPing;
    h->beginn = zeitGueltig() ? time(nullptr) : 0;
    h->beginnMs = jetzt;
    h->gesendetStart = gesVorher;
    h->okStart = okVorher;
    stundenAnzahl++;
  }
  if (luecke > h->maxLueckeMs) h->maxLueckeMs = luecke;
  h->empfangen++;
  h->gesendetLetzt = p.gesendet;
  h->okLetzt = p.okZur;
  h->hinRssiSumme += rssi;
  h->hinSnrSumme += snr;
  if (snr < h->hinSnrMin) h->hinSnrMin = snr;
  if (p.snr10 != KEIN_WERT) {
    float zs = p.snr10 / 10.0f;
    if (zs < h->zurSnrMin) h->zurSnrMin = zs;
    h->nZur++;
  }

  a->ende = zeitGueltig() ? time(nullptr) : 0;
  a->endeMs = jetzt;
  a->empfangen++;
  a->gesendetWasc = p.gesendet;
  a->okZur = p.okZur;
  a->hinRssiSumme += rssi;
  a->hinSnrSumme += snr;
  if (snr < a->hinSnrMin) a->hinSnrMin = snr;
  if (p.snr10 != KEIN_WERT) {
    float zr = p.rssi10 / 10.0f, zs = p.snr10 / 10.0f;
    a->zurRssiSumme += zr;
    a->zurSnrSumme += zs;
    if (zs < a->zurSnrMin) a->zurSnrMin = zs;
    a->nZur++;
    merke(st.zur, zr, zs);
  }
}

void verarbeite(const Paket& p, float rssi, float snr) {
  if (rolle == WOHN && p.typ == PING) {
    bucheMesspunkt(p, rssi, snr);
    if (st.seqDa && p.seq > st.letzteSeq + 1) st.weg += p.seq - st.letzteSeq - 1;
    st.letzteSeq = p.seq;
    st.seqDa = true;
    st.ok++;
    st.letzterEmpfangMs = millis();
    merke(st.hin, rssi, snr);
    led(0, 0, 40, 300);
    if (p.snr10 != KEIN_WERT) {
      logge("#%u P%u hin %ld/%+.1f zur %ld/%+.1f", (unsigned)p.seq, (unsigned)p.punkt, lroundf(rssi), snr,
            lroundf(p.rssi10 / 10.0f), p.snr10 / 10.0f);
    } else {
      logge("#%u P%u hin %ld/%+.1f zur --", (unsigned)p.seq, (unsigned)p.punkt, lroundf(rssi), snr);
    }
    Serial.printf("WOHN,PING,%u,%u,%.0f,%.2f,%.1f,%.1f,%u,%u\n", (unsigned)p.seq, (unsigned)p.punkt, rssi, snr,
                  p.snr10 != KEIN_WERT ? p.rssi10 / 10.0f : NAN, p.snr10 != KEIN_WERT ? p.snr10 / 10.0f : NAN,
                  (unsigned)p.okZur, (unsigned)p.gesendet);
    Paket a = neuesPaket(PONG, p.seq);
    a.rssi10 = (int16_t)lroundf(rssi * 10);
    a.snr10 = (int16_t)lroundf(snr * 10);
    delay(20);
    sende(a);
    return;
  }
  if (rolle == WASC && p.typ == PONG && wartetAufPong && p.seq == seq) {
    wartetAufPong = false;
    st.ok++;
    st.letzterEmpfangMs = millis();
    merke(st.zur, rssi, snr);
    merke(st.hin, p.rssi10 / 10.0f, p.snr10 / 10.0f);
    zurRssi10 = (int16_t)lroundf(rssi * 10);
    zurSnr10 = (int16_t)lroundf(snr * 10);
    zurNeu = true;
    led(0, 40, 0, 400);
    Serial.printf("WASC,PONG,%u,hin,%.1f,%.2f,zur,%.0f,%.2f\n", (unsigned)p.seq, p.rssi10 / 10.0f, p.snr10 / 10.0f, rssi, snr);
    return;
  }
  Serial.printf("# fremdes/unerwartetes Paket typ=%u seq=%u rssi=%.0f snr=%.2f\n", (unsigned)p.typ, (unsigned)p.seq, rssi, snr);
}

void funkSchleife() {
  if (funkStatus != RADIOLIB_ERR_NONE || otaLaeuft) return;

  if (dio1) {
    dio1 = false;
    if (sendetGerade) {
      radio.finishTransmit();
      sendetGerade = false;
      if (rolle == WASC) {
        wartetAufPong = true;
        pongBisMs = millis() + pongWarteMs;
      }
      starteEmpfang();
    } else {
      size_t len = radio.getPacketLength();
      Paket p{};
      int s = radio.readData((uint8_t*)&p, sizeof(p));
      float rssi = radio.getRSSI();
      float snr = radio.getSNR();
      if (s == RADIOLIB_ERR_NONE && len == sizeof(Paket) && p.magic[0] == 'W' && p.magic[1] == 'K') {
        verarbeite(p, rssi, snr);
      } else if (s == RADIOLIB_ERR_NONE && len >= 2 && p.magic[0] == 'W' && p.magic[1] == 'K') {
        Serial.printf("# Paket einer anderen Firmware-Version (%u Byte), beide Geraete gleich flashen\n", (unsigned)len);
        zeigeHinweis("andere FW!", 4000);
      } else if (s == RADIOLIB_ERR_CRC_MISMATCH) {
        st.kaputt++;
        Serial.printf("# Paket mit CRC-Fehler rssi=%.0f snr=%.2f\n", rssi, snr);
        if (rolle == WOHN) {
          logge("CRC-Fehler %ld/%+.1f", lroundf(rssi), snr);
          if (Punkt* a = aktuellerPunkt()) a->kaputt++;
          if (Stunde* h = aktuelleStunde()) h->kaputt++;
        }
      } else if (s != RADIOLIB_ERR_NONE) {
        Serial.printf("# readData Fehler %d\n", s);
      }
      if (!sendetGerade) starteEmpfang();
    }
  }

  if (rolle != WASC) return;
  uint32_t jetzt = millis();
  if (wartetAufPong && (int32_t)(jetzt - pongBisMs) > 0) {
    wartetAufPong = false;
    st.weg++;
    led(40, 0, 0, 400);
    Serial.printf("WASC,VERLOREN,%u\n", (unsigned)seq);
  }
  if (!sendetGerade && !wartetAufPong && (int32_t)(jetzt - naechsterPingMs) >= 0) {
    seq++;
    st.gesendet++;
    naechsterPingMs = jetzt + pingAbstandMs;
    Paket p = neuesPaket(PING, seq);
    p.punkt = punktNr;
    p.okZur = (uint16_t)st.ok;
    p.gesendet = (uint16_t)st.gesendet;
    if (zurNeu) {   // nur die Antwort auf den vorigen Ping melden, nichts doppelt
      p.rssi10 = zurRssi10;
      p.snr10 = zurSnr10;
      zurNeu = false;
    }
    sende(p);
  }
}

// ---------------------------------------------------------------- Taster

void tasterSchleife() {
  static bool war = false;
  static bool langErledigt = false;
  static uint32_t seit = 0;
  static uint32_t letzteAbfrage = 0;
  if (!expanderOk || millis() - letzteAbfrage < 20) return;
  letzteAbfrage = millis();

  bool unten = c6l::buttonDown();
  if (unten && !war) {
    seit = millis();
    langErledigt = false;
  }
  if (unten && !langErledigt && millis() - seit >= 1000) zeigeHinweis("halten=Rolle", 300);
  if (unten && !langErledigt && millis() - seit >= 3000) {
    langErledigt = true;
    rolle = rolle == WASC ? WOHN : WASC;
    prefs.putUChar("rolle", rolle);
    Serial.printf("# Rolle jetzt %s, Neustart\n", rollenName(rolle));
    oled.clearBuffer();
    oled.setFont(u8g2_font_6x10_tf);
    oled.drawStr(0, 12, "Rolle:");
    oled.drawStr(0, 28, rollenName(rolle));
    oled.sendBuffer();
    delay(1500);
    ESP.restart();
  }
  if (!unten && war && !langErledigt && millis() - seit >= 30) {
    st = Stat();
    zurNeu = false;
    if (rolle == WASC) {
      punktNr = punktNr >= 250 ? 1 : punktNr + 1;
      char h[16];
      snprintf(h, sizeof(h), "Punkt %u", (unsigned)punktNr);
      zeigeHinweis(h);
    } else {
      zeigeHinweis("Anzeige neu");
    }
    Serial.printf("%s,MESSPUNKT_NEU,%u\n", rollenName(rolle), (unsigned)punktNr);
  }
  war = unten;
}

// ---------------------------------------------------------------- Anzeige

void zeile(int nr, const char* text) { oled.drawStr(0, 7 + nr * 8, text); }

void urteil(char* buf, size_t n) {
  int sf = sfMoeglich();
  if (sf == 0) snprintf(buf, n, "%s", (st.gesendet || rolle == WOHN) ? "kein Empfang" : "warte ...");
  else if (sf > 12) snprintf(buf, n, "zu knapp");
  else snprintf(buf, n, "SF%d geht", sf);
}

void wlanText(char* buf, size_t n) {
  if (!HAT_WLAN) snprintf(buf, n, "kein WLAN-Key");
  else if (WiFi.status() == WL_CONNECTED) snprintf(buf, n, "WLAN .%u", (unsigned)WiFi.localIP()[3]);
  else snprintf(buf, n, "WLAN sucht");
}

void anzeigen() {
  char z[6][16];
  memset(z, 0, sizeof(z));

  if (otaLaeuft) {
    snprintf(z[0], 16, "Update");
    snprintf(z[1], 16, "laeuft ...");
  } else if (!expanderOk || funkStatus != RADIOLIB_ERR_NONE) {
    snprintf(z[0], 16, "%s %s", rollenName(rolle), FW_VERSION);
    snprintf(z[1], 16, "%s", expanderOk ? "IO ok" : "IO Fehler");
    snprintf(z[2], 16, "LoRa %d", funkStatus);
    snprintf(z[3], 16, "siehe USB-Log");
    if (rolle == WOHN) wlanText(z[5], 16);
  } else if (rolle == WASC) {
    snprintf(z[0], 16, "WASC #%u P%u", (unsigned)seq, (unsigned)punktNr);
    if (st.hin.da) snprintf(z[1], 16, "hin%5.0f%+4.0f", st.hin.rssi, st.hin.snr);
    else snprintf(z[1], 16, "hin   --");
    if (st.zur.da) snprintf(z[2], 16, "zur%5.0f%+4.0f", st.zur.rssi, st.zur.snr);
    else snprintf(z[2], 16, "zur   --");
    snprintf(z[3], 16, "ok %lu/%lu", (unsigned long)st.ok, (unsigned long)st.gesendet);
    float m = min(st.hin.snrMin, st.zur.snrMin);
    if (m < 90) snprintf(z[4], 16, "min%+5.1fdB", m);
    else snprintf(z[4], 16, "SF%u %.1f", (unsigned)SF, FREQ_MHZ);
    urteil(z[5], 16);
  } else {
    Punkt* a = aktuellerPunkt();
    if (st.seqDa && a) snprintf(z[0], 16, "WOHN #%u P%u", (unsigned)st.letzteSeq, (unsigned)a->nr);
    else snprintf(z[0], 16, "WOHN %s", FW_VERSION);
    if (st.hin.da) {
      snprintf(z[1], 16, "vor %lus", (unsigned long)((millis() - st.letzterEmpfangMs) / 1000));
      snprintf(z[2], 16, "%.0f dBm", st.hin.rssi);
      snprintf(z[3], 16, "SNR %+.1f", st.hin.snr);
      snprintf(z[4], 16, "ok %lu weg %lu", (unsigned long)st.ok, (unsigned long)st.weg);
      if ((millis() / 4000) % 2) wlanText(z[5], 16);
      else urteil(z[5], 16);
    } else {
      snprintf(z[1], 16, "warte auf");
      snprintf(z[2], 16, "Ping ...");
      snprintf(z[4], 16, "SF%u", (unsigned)SF);
      wlanText(z[5], 16);
    }
  }
  if (hinweisBisMs && (int32_t)(millis() - hinweisBisMs) < 0) snprintf(z[5], 16, "%s", hinweis);

  oled.clearBuffer();
  oled.setFont(u8g2_font_5x8_tf);
  for (int i = 0; i < 6; i++) zeile(i, z[i]);
  oled.sendBuffer();
}

// ---------------------------------------------------------------- Statusseite (nur WOHN)

String punktJson(const Punkt& a) {
  char b[460];
  float m = a.nZur ? min(a.hinSnrMin, a.zurSnrMin) : a.hinSnrMin;
  snprintf(b, sizeof(b),
           "{\"nr\":%u,\"beginn\":%lld,\"ende\":%lld,\"dauer_s\":%lu,\"hin_empfangen\":%u,\"hin_gesendet\":%u,"
           "\"hin_rssi_mittel\":%.1f,\"hin_snr_mittel\":%.1f,\"hin_snr_min\":%.1f,\"zur_ok\":%u,\"zur_gesendet\":%u,"
           "\"zur_n\":%u,\"zur_rssi_mittel\":%.1f,\"zur_snr_mittel\":%.1f,\"zur_snr_min\":%.1f,\"max_luecke_s\":%lu,"
           "\"crc_fehler\":%u,\"sf_hin\":%d,\"sf_beide\":%d}",
           (unsigned)a.nr, (long long)a.beginn, (long long)a.ende, (unsigned long)((a.endeMs - a.beginnMs) / 1000),
           (unsigned)a.empfangen, hinGesendet(a), a.empfangen ? a.hinRssiSumme / a.empfangen : 0.0f,
           a.empfangen ? a.hinSnrSumme / a.empfangen : 0.0f, a.empfangen ? a.hinSnrMin : 0.0f, zurOk(a), zurBasis(a),
           (unsigned)a.nZur, a.nZur ? a.zurRssiSumme / a.nZur : 0.0f, a.nZur ? a.zurSnrSumme / a.nZur : 0.0f,
           a.nZur ? a.zurSnrMin : 0.0f, (unsigned long)(a.maxLueckeMs / 1000), (unsigned)a.kaputt,
           a.empfangen ? sfFuer(a.hinSnrMin) : 0, a.empfangen ? sfFuer(m) : 0);
  return String(b);
}

String stundeJson(const Stunde& h) {
  char b[360];
  snprintf(b, sizeof(b),
           "{\"beginn\":%lld,\"punkt\":%u,\"hin_empfangen\":%u,\"hin_gesendet\":%u,\"hin_rssi_mittel\":%.1f,"
           "\"hin_snr_mittel\":%.1f,\"hin_snr_min\":%.1f,\"zur_ok\":%u,\"zur_gesendet\":%u,\"zur_snr_min\":%.1f,"
           "\"max_luecke_s\":%lu,\"crc_fehler\":%u}",
           (long long)h.beginn, (unsigned)h.punkt, (unsigned)h.empfangen, hinGesendet(h),
           h.empfangen ? h.hinRssiSumme / h.empfangen : 0.0f, h.empfangen ? h.hinSnrSumme / h.empfangen : 0.0f,
           h.empfangen ? h.hinSnrMin : 0.0f, zurOk(h), zurBasis(h), h.nZur ? h.zurSnrMin : 0.0f,
           (unsigned long)(h.maxLueckeMs / 1000), (unsigned)h.kaputt);
  return String(b);
}

void seiteStatus() {
  String s;
  s.reserve(1200 + MAX_PUNKTE * 480 + MAX_STUNDEN * 380);
  char b[200];
  snprintf(b, sizeof(b),
           "{\"fw\":\"%s\",\"rolle\":\"%s\",\"uptime_s\":%lu,\"wlan_rssi\":%d,\"lora\":%d,\"letzter_empfang_vor_s\":%ld,",
           FW_VERSION, rollenName(rolle), (unsigned long)(millis() / 1000), (int)WiFi.RSSI(), funkStatus,
           st.letzterEmpfangMs ? (long)((millis() - st.letzterEmpfangMs) / 1000) : -1L);
  s += b;
  s += "\"punkte\":[";
  int von = punkteAnzahl > MAX_PUNKTE ? punkteAnzahl - MAX_PUNKTE : 0;
  for (int i = von; i < punkteAnzahl; i++) {
    if (i > von) s += ",";
    s += punktJson(punkte[i % MAX_PUNKTE]);
  }
  s += "],\"stunden\":[";
  von = stundenAnzahl > MAX_STUNDEN ? stundenAnzahl - MAX_STUNDEN : 0;
  for (int i = von; i < stundenAnzahl; i++) {
    if (i > von) s += ",";
    s += stundeJson(stunden[i % MAX_STUNDEN]);
  }
  s += "]}";
  web.send(200, "application/json", s);
}

void seiteLog() {
  String s;
  s.reserve(MAX_LOG * 72);
  int von = logAnzahl > MAX_LOG ? logAnzahl - MAX_LOG : 0;
  char zeit[24];
  for (int i = von; i < logAnzahl; i++) {
    const LogZeile& z = protokoll[i % MAX_LOG];
    zeitText(z.t, z.ms, zeit, sizeof(zeit));
    s += zeit;
    s += "  ";
    s += z.text;
    s += "\n";
  }
  web.send(200, "text/plain; charset=utf-8", s);
}


void prozent(char* buf, size_t n, unsigned teil, unsigned ganz) {
  if (!ganz) snprintf(buf, n, "–");
  else snprintf(buf, n, "%u/%u (%u %%)", teil, ganz, (unsigned)((teil * 100 + ganz / 2) / ganz));
}

void urteilText(char* buf, size_t n, float snrMin) {
  int sf = sfFuer(snrMin);
  if (sf > 12) snprintf(buf, n, "zu knapp");
  else snprintf(buf, n, "SF%d geht", sf);
}

void seiteHaupt() {
  String s;
  s.reserve(7000 + MAX_PUNKTE * 420 + MAX_STUNDEN * 300);
  s += F("<!doctype html><html lang=de><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
         "<meta http-equiv=refresh content=30><title>Waschküche LoRa</title><style>"
         "body{font:15px system-ui,sans-serif;margin:16px;background:#fff;color:#111}"
         "@media(prefers-color-scheme:dark){body{background:#111;color:#eee}td,th{border-color:#444!important}}"
         "table{border-collapse:collapse;margin:8px 0;font-variant-numeric:tabular-nums}"
         "td,th{border-bottom:1px solid #ccc;padding:4px 8px;text-align:right;white-space:nowrap}"
         "th:first-child,td:first-child{text-align:left}.w{overflow-x:auto}pre{font-size:13px;white-space:pre-wrap}"
         "</style></head><body><h2>Waschküche LoRa-Test</h2>");
  char b[300];
  snprintf(b, sizeof(b), "<p>Firmware %s, Rolle %s, läuft seit %lu min, WLAN %d dBm, ", FW_VERSION, rollenName(rolle),
           (unsigned long)(millis() / 60000), (int)WiFi.RSSI());
  s += b;
  if (st.letzterEmpfangMs) snprintf(b, sizeof(b), "letzter Ping vor %lu s.</p>", (unsigned long)((millis() - st.letzterEmpfangMs) / 1000));
  else snprintf(b, sizeof(b), "noch kein Ping empfangen.</p>");
  s += b;

  s += F("<h3>Messpunkte</h3><div class=w><table><tr><th>Punkt</th><th>Beginn</th><th>Dauer</th>"
         "<th>hin angekommen</th><th>hin RSSI Ø</th><th>hin SNR Ø / min</th>"
         "<th>zurück angekommen</th><th>zurück RSSI Ø</th><th>zurück SNR Ø / min</th>"
         "<th>längste Pause</th><th>fremd/CRC</th><th>Urteil</th></tr>");
  int von = punkteAnzahl > MAX_PUNKTE ? punkteAnzahl - MAX_PUNKTE : 0;
  for (int i = punkteAnzahl - 1; i >= von; i--) {
    const Punkt& a = punkte[i % MAX_PUNKTE];
    char zeit[24], hin[32], zur[32], urt[24];
    zeitText(a.beginn, a.beginnMs, zeit, sizeof(zeit));
    prozent(hin, sizeof(hin), a.empfangen, hinGesendet(a));
    prozent(zur, sizeof(zur), zurOk(a), zurBasis(a));
    urteilText(urt, sizeof(urt), a.nZur ? min(a.hinSnrMin, a.zurSnrMin) : a.hinSnrMin);
    snprintf(b, sizeof(b), "<tr><td>%u</td><td>%s</td><td>%lu min</td><td>%s</td><td>%ld dBm</td><td>%+.1f / %+.1f dB</td>",
             (unsigned)a.nr, zeit, (unsigned long)((a.endeMs - a.beginnMs) / 60000), hin,
             a.empfangen ? lroundf(a.hinRssiSumme / a.empfangen) : 0L, a.empfangen ? a.hinSnrSumme / a.empfangen : 0.0f,
             a.hinSnrMin);
    s += b;
    if (a.nZur) {
      snprintf(b, sizeof(b), "<td>%s</td><td>%ld dBm</td><td>%+.1f / %+.1f dB</td>", zur, lroundf(a.zurRssiSumme / a.nZur),
               a.zurSnrSumme / a.nZur, a.zurSnrMin);
    } else {
      snprintf(b, sizeof(b), "<td>%s</td><td>–</td><td>–</td>", zur);
    }
    s += b;
    snprintf(b, sizeof(b), "<td>%lu s</td><td>%u</td><td>%s</td></tr>", (unsigned long)(a.maxLueckeMs / 1000),
             (unsigned)a.kaputt, urt);
    s += b;
  }
  if (!punkteAnzahl) s += F("<tr><td colspan=12>noch keine Messpunkte</td></tr>");
  s += F("</table></div><p>Urteil: schnellster Spreizfaktor, der bei der schlechtesten SNR noch 5 dB Reserve hat. "
         "Längste Pause: größter Abstand zwischen zwei empfangenen Pings (normal etwa 28 s). "
         "Neuer Messpunkt: Taster an WASC kurz drücken.</p>");

  s += F("<h3>Stunden</h3><div class=w><table><tr><th>Stunde</th><th>Punkt</th><th>hin angekommen</th>"
         "<th>hin RSSI Ø</th><th>hin SNR Ø / min</th><th>zurück angekommen</th><th>zurück SNR min</th>"
         "<th>längste Pause</th><th>fremd/CRC</th><th>Urteil</th></tr>");
  von = stundenAnzahl > MAX_STUNDEN ? stundenAnzahl - MAX_STUNDEN : 0;
  for (int i = stundenAnzahl - 1; i >= von; i--) {
    const Stunde& h = stunden[i % MAX_STUNDEN];
    char zeit[24], hin[32], zur[32], urt[24], zsnr[16];
    if (h.beginn > 1700000000) {
      struct tm lt;
      localtime_r(&h.beginn, &lt);
      strftime(zeit, sizeof(zeit), "%d.%m. %H:00", &lt);
    } else {
      snprintf(zeit, sizeof(zeit), "+%lu h", (unsigned long)(h.beginnMs / 3600000UL));
    }
    prozent(hin, sizeof(hin), h.empfangen, hinGesendet(h));
    prozent(zur, sizeof(zur), zurOk(h), zurBasis(h));
    urteilText(urt, sizeof(urt), h.nZur ? min(h.hinSnrMin, h.zurSnrMin) : h.hinSnrMin);
    if (h.nZur) snprintf(zsnr, sizeof(zsnr), "%+.1f dB", h.zurSnrMin);
    else snprintf(zsnr, sizeof(zsnr), "–");
    snprintf(b, sizeof(b),
             "<tr><td>%s</td><td>%u</td><td>%s</td><td>%ld dBm</td><td>%+.1f / %+.1f dB</td><td>%s</td><td>%s</td>"
             "<td>%lu s</td><td>%u</td><td>%s</td></tr>",
             zeit, (unsigned)h.punkt, hin, h.empfangen ? lroundf(h.hinRssiSumme / h.empfangen) : 0L,
             h.empfangen ? h.hinSnrSumme / h.empfangen : 0.0f, h.hinSnrMin, zur, zsnr,
             (unsigned long)(h.maxLueckeMs / 1000), (unsigned)h.kaputt, urt);
    s += b;
  }
  if (!stundenAnzahl) s += F("<tr><td colspan=10>noch keine Daten</td></tr>");
  s += F("</table></div><h3>Letzte Pakete</h3><pre>");
  int vonLog = logAnzahl > 40 ? logAnzahl - 40 : 0;
  char zeit[24];
  for (int i = logAnzahl - 1; i >= vonLog; i--) {
    const LogZeile& z = protokoll[i % MAX_LOG];
    zeitText(z.t, z.ms, zeit, sizeof(zeit));
    s += zeit;
    s += "  ";
    s += z.text;
    s += "\n";
  }
  s += F("</pre><p><a href=/api/status>/api/status</a> · <a href=/api/log>/api/log</a></p></body></html>");
  web.send(200, "text/html; charset=utf-8", s);
}

// ---------------------------------------------------------------- WLAN (nur WOHN)

void starteWlan() {
#if HAT_WLAN
  WiFi.setHostname(HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.println("# WLAN verbinde ...");
#endif
}

void netzSchleife() {
#if HAT_WLAN
  if (rolle != WOHN) return;
  static uint32_t letzterVersuch = 0;
  bool verbunden = WiFi.status() == WL_CONNECTED;
  if (verbunden && !netzGestartet) {
    netzGestartet = true;
    Serial.printf("# WLAN verbunden, IP %s, RSSI %d dBm, http://%s.local/\n", WiFi.localIP().toString().c_str(),
                  (int)WiFi.RSSI(), HOSTNAME);
    logge("WLAN verbunden, IP %s", WiFi.localIP().toString().c_str());
    configTzTime(ZEITZONE, "pool.ntp.org", "time.cloudflare.com");
    web.on("/", seiteHaupt);
    web.on("/api/status", seiteStatus);
    web.on("/api/log", seiteLog);
    web.onNotFound([] { web.send(404, "text/plain", "nicht gefunden"); });
    web.begin();
    ArduinoOTA.setHostname(HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASS);
    ArduinoOTA.onStart([] {
      otaLaeuft = true;
      radio.standby();
      led(30, 0, 30, 0);
      anzeigen();
    });
    ArduinoOTA.onEnd([] { led(0, 0, 0, 0); });
    ArduinoOTA.onError([](ota_error_t e) {
      Serial.printf("# OTA Fehler %u, Neustart\n", (unsigned)e);
      delay(500);
      ESP.restart();
    });
    ArduinoOTA.begin();
  }
  if (!verbunden && millis() - letzterVersuch > 60000) {
    letzterVersuch = millis();
    WiFi.reconnect();
  }
  if (netzGestartet) {
    ArduinoOTA.handle();
    if (!otaLaeuft) web.handleClient();
  }
#endif
}

// ---------------------------------------------------------------- Start

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);   // ohne USB-Host (Powerbank) nie blockieren
#endif
  delay(300);

  prefs.begin("wk", false);
  rolle = prefs.getUChar("rolle", WOHN) == WASC ? WASC : WOHN;

  expanderOk = c6l::begin();
  SPI.begin(c6l::PIN_SCK, c6l::PIN_MISO, c6l::PIN_MOSI, -1);
  oled.begin();
  oled.setContrast(130);
  oled.clearBuffer();
  oled.setFont(u8g2_font_5x8_tf);
  zeile(0, "Waschkueche");
  zeile(1, "LoRa-Test");
  zeile(2, FW_VERSION);
  zeile(4, "Rolle:");
  zeile(5, rollenName(rolle));
  oled.sendBuffer();
  led(10, 10, 10, 800);

  if (rolle == WOHN) starteWlan();
  else WiFi.mode(WIFI_OFF);

  funkStatus = radio.begin(FREQ_MHZ, BW_KHZ, SF, CR, SYNC_WORD, TX_DBM, PREAMBEL, c6l::LORA_TCXO_V, false);
  if (funkStatus == RADIOLIB_ERR_NONE) {
    radio.setDio2AsRfSwitch(true);
    radio.setCurrentLimit(140.0);
    radio.setRxBoostedGainMode(true);
    radio.setDio1Action(aufDio1);
    sendedauerMs = radio.getTimeOnAir(sizeof(Paket)) / 1000;
    pingAbstandMs = max(PING_MIN_MS, SENDEZEIT_FAKTOR * sendedauerMs);
    pongWarteMs = sendedauerMs + 2500;
    starteEmpfang();
  }

  Serial.printf("# Waschkueche LoRa-Test %s, Rolle %s, IO %s, LoRa-Start %d, WLAN-Key %s\n", FW_VERSION, rollenName(rolle),
                expanderOk ? "ok" : "FEHLER", funkStatus, HAT_WLAN ? "ja" : "nein");
  Serial.printf("# %.3f MHz, SF%u, BW %.0f kHz, %d dBm, Sendedauer %lu ms, Ping alle %lu ms\n", FREQ_MHZ, (unsigned)SF, BW_KHZ,
                (int)TX_DBM, (unsigned long)sendedauerMs, (unsigned long)pingAbstandMs);
  Serial.println("# WASC,PONG,seq,hin,rssi,snr,zur,rssi,snr | WASC,VERLOREN,seq | WOHN,PING,seq,punkt,rssi,snr,zur_rssi,zur_snr,ok,gesendet");
  delay(1500);
}

void loop() {
  netzSchleife();
  funkSchleife();
  tasterSchleife();
  if (ledAusMs && (int32_t)(millis() - ledAusMs) >= 0) led(0, 0, 0, 0);
  static uint32_t letzteAnzeige = 0;
  if (millis() - letzteAnzeige >= 250) {
    letzteAnzeige = millis();
    anzeigen();
  }
  static uint32_t letzterStatus = 0;
  if (millis() - letzterStatus >= 10000) {   // Lebenszeichen fuer den USB-Log
    letzterStatus = millis();
    Serial.printf("# status fw=%s rolle=%s io=%s lora=%d wlan=%s ip=%s ok=%lu weg=%lu kaputt=%lu\n", FW_VERSION,
                  rollenName(rolle), expanderOk ? "ok" : "FEHLER", funkStatus,
                  rolle == WOHN && WiFi.status() == WL_CONNECTED ? "ok" : "nein",
                  rolle == WOHN && WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "-",
                  (unsigned long)st.ok, (unsigned long)st.weg, (unsigned long)st.kaputt);
  }
  delay(2);
}
