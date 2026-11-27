// Waschkueche: Waschmaschine und Trockner in einer Waschkueche ohne Netz per LoRa melden.
// Ein Quelltext fuer beide M5Stack Unit C6L. Die Rolle steht beim Bauen fest (rolle.h, legt
// skripte/waschkueche-flashen.sh an), damit das Geraet in der Waschkueche (fuer alle Parteien zugaenglich)
// weder Heim-WLAN noch MQTT-Zugang enthaelt. Per Taster laesst sich die Rolle nicht mehr wechseln.
//
//   WASC  Sender in der Waschkueche. Spannt ein eigenes WLAN (AP_SSID) fuer zwei Shelly Plug PM Gen3 auf,
//         fragt sie alle 10 s ab und funkt Leistung und Zaehlerstand verschluesselt in die Wohnung:
//         alle 30 s, solange eine Maschine laeuft, sonst alle 5 min, bei Zustandswechsel sofort.
//         Jede Sendung wird quittiert, sonst bis zu dreimal wiederholt.
//         Am Grove-Port optional ein M5Stack Unit Accel (ADXL345) an der Seitenwand der Waschmaschine:
//         der Sender wertet die Vibration selbst aus (vibration.h) und haengt 25 Byte Kennzahlen an.
//         Statusseite und Update: in diesem WLAN http://192.168.4.1/ und /update.
//   WOHN  Gateway am Fenster. Empfaengt, quittiert, schickt alles per MQTT an den Server
//         (sensors/waschkueche/...), Statusseite http://waschkueche-wohn.local/, Update per WLAN (OTA).
//         Haelt die Einstellungen des Senders (Sendetakt, Schwellen, Sensor; Seite /einstellungen oder MQTT
//         .../set) und schickt sie mit der Quittung hinunter, wenn der Sender andere meldet.
//
// "Fertig" erkennt der Server aus der Leistung (server/fertigmeldung), nicht dieses Geraet.
//
// Was im Speicher steht (lesbar fuer jeden mit USB-Kabel):
//   WASC  LoRa-Schluessel, Passwort des Shelly-WLANs (AP_PASS), eigenes Update-Passwort OTA_PASS_WASC
//   WOHN  LoRa-Schluessel, Heim-WLAN, OTA_PASS, MQTT-Zugang

#include <Arduino.h>
#include <SPI.h>
#include <Preferences.h>
#include <RadioLib.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <HTTPUpdateServer.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include "c6l_hw.h"
#include "config.h"
#include "einstellungen.h"
#include "vibration.h"
#include "adxl345.h"
#include "funk.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "secrets.h fehlt: Vorlage secrets.h.example oder skripte/waschkueche-secrets.sh"
#endif

#ifndef AP_SSID
#error "AP_SSID fehlt in secrets.h (Name des WLANs fuer die Shellys, siehe secrets.h.example)"
#endif

#if __has_include("rolle.h")
#include "rolle.h"
#endif
#if defined(ROLLE_WASC) == defined(ROLLE_WOHN)
#error "Rolle fehlt: rolle.h mit #define ROLLE_WASC oder #define ROLLE_WOHN (legt skripte/waschkueche-flashen.sh an)"
#endif

#if defined(ROLLE_WASC) && !defined(OTA_PASS_WASC)
#error "OTA_PASS_WASC fehlt in secrets.h: bash skripte/waschkueche-secrets.sh ergaenzt es"
#endif

// Der Sender startet seinen Sendezaehler nach dem Loeschen aus der Bauzeit (Sekunden seit 1.1.2026, siehe starteSender).
#define ZAEHLER_EPOCHE 1767225600UL
#ifdef ROLLE_WASC
#ifndef BAU_ZEIT
#error "BAU_ZEIT fehlt in rolle.h (legt skripte/waschkueche-flashen.sh an: Sekunden seit 1970 aus date +%s, mit UL)"
#endif
static_assert(BAU_ZEIT > ZAEHLER_EPOCHE, "BAU_ZEIT liegt vor 2026: Uhr des Rechners pruefen");
#endif

#if defined(ROLLE_WOHN) && defined(MQTT_HOST) && defined(MQTT_PASS)
#define HAT_MQTT 1
#else
#define HAT_MQTT 0
#endif

enum Rolle : uint8_t { WOHN = 0, WASC = 1 };
#ifdef ROLLE_WASC
constexpr Rolle rolle = WASC;
#else
constexpr Rolle rolle = WOHN;
#endif

SX1262 radio = new Module(c6l::LORA_CS, c6l::LORA_DIO1, RADIOLIB_NC, c6l::LORA_BUSY, SPI);
U8G2_SSD1306_64X48_ER_F_4W_HW_SPI oled(OLED_DREHUNG, c6l::OLED_CS, c6l::OLED_DC, c6l::OLED_RST);
Preferences prefs;
WebServer web(80);
HTTPUpdateServer updater;
WiFiClient netz;
PubSubClient mqtt(netz);

bool expanderOk = false;
int funkStatus = RADIOLIB_ERR_NONE;
volatile bool dio1 = false;
bool sendetGerade = false;
uint32_t txStartMs = 0, txEndeMs = 0;
uint32_t toaDatenMaxMs = 0, toaQuittungMs = 0, toaQuittungMaxMs = 0;
funk::Sendezeit sendezeit;
bool otaLaeuft = false;
uint32_t ledAusMs = 0;
char hinweis[16] = "";
uint32_t hinweisBisMs = 0;
bool anzeigeAus = false;          // nur WASC: Display und LED aus, bis jemand den Taster drueckt
uint32_t letzterTastendruckMs = 0;
// WASC: damit arbeitet der Sender. WOHN: so soll der Sender arbeiten (wird per Quittung hinuntergeschickt).
cfg::Einstellungen einst = cfg::standard();

// ---- Sender ----
struct Messung {
  bool erreichbar = false;
  uint8_t fehler = 0;
  float leistung = 0, energie = 0;
  uint32_t zeitMs = 0;
};
Messung mess[MASCHINEN];
int naechsteAbfrage = 0;
uint32_t letzteAbfrageMs = 0;
uint32_t fcnt = 0;
uint8_t rahmen[funk::LEN_MAX];
size_t rahmenLen = 0;
uint32_t rahmenToaMs = 0;
uint8_t versuch = 0;
bool wartetAufQuittung = false;
uint32_t quittungBisMs = 0, wiederholenAbMs = 0;
uint32_t letztesSendenMs = 0;
bool sendungOffen = false;
bool gesendetAktiv[MASCHINEN] = {false, false}, gesendetErreichbar[MASCHINEN] = {false, false};
bool erstesSenden = true;
uint32_t okZahl = 0, verloren = 0;
float quittungRssi = 0, quittungSnr = 0;      // was das Gateway gehoert hat
float eigenRssi = 0, eigenSnr = 0;            // wie wir die Quittung gehoert haben
uint32_t letzteQuittungMs = 0;
bool sofortSenden = false;
vib::Fenster vibFenster;                      // Vibration seit der letzten Sendung
float vibRms = 0, vibDrehHz = 0, vibDreh = 0; // letzter Rahmen (2,56 s), mg und Hz
uint32_t vibRahmenMs = 0;
vib::Block vibGesendet{};                     // zuletzt gesendeter Block, fuer die Statusseite
bool vibGesendetDa = false;

// ---- Gateway ----
uint32_t letzterFcnt = 0, gespeicherterFcnt = 0;
bool fcntDa = false;
uint8_t letzteQuittung[funk::LEN_QUITTUNG_MAX];
size_t letzteQuittungLen = 0;
funk::Daten zuletzt{};
bool zuletztDa = false;
vib::Block zuletztVib{};
bool zuletztVibDa = false;
uint32_t zuletztVibMs = 0;
uint32_t einstGesendet = 0;                   // so oft gingen Einstellungen mit einer Quittung hinunter
uint32_t zuletztMs = 0;
float zuletztRssi = 0, zuletztSnr = 0;
uint32_t empfangen = 0, doppelt = 0, ungueltig = 0, crcFehler = 0, fcntLuecken = 0;
uint32_t veraltet = 0, veraltetMs = 0;        // echte Pakete mit altem Zaehler (wiederholt abgespielt oder Sender falsch gebaut)
uint32_t letzteDiagMs = 0, letzterMqttVersuchMs = 0;
bool netzGestartet = false;

void IRAM_ATTR aufDio1() { dio1 = true; }

// ================================================================ Hilfen

void led(uint8_t r, uint8_t g, uint8_t b, uint32_t ms) {
  if (anzeigeAus && (r || g || b)) return;   // in der Waschkueche nicht blinken, solange das Display aus ist
  rgbLedWrite(c6l::PIN_LED, r, g, b);
  ledAusMs = ms ? millis() + ms : 0;
}

void zeigeHinweis(const char* text, uint32_t ms = 2000) {
  snprintf(hinweis, sizeof(hinweis), "%s", text);
  hinweisBisMs = millis() + ms;
}

const char* rollenName(Rolle r) { return r == WASC ? "WASC" : "WOHN"; }

uint32_t toaMs(size_t len) { return (uint32_t)(radio.getTimeOnAir(len) / 1000) + 1; }

void ladeEinstellungen() {
  cfg::Einstellungen e;
  if (prefs.getBytesLength("einst") == sizeof(e) && prefs.getBytes("einst", &e, sizeof(e)) == sizeof(e)) {
    cfg::begrenzen(e);
    einst = e;
  }
}

void speichereEinstellungen() { prefs.putBytes("einst", &einst, sizeof(einst)); }

String einstJson() {
  char b[220];
  snprintf(b, sizeof(b),
           "{\"senden_aktiv_s\":%u,\"senden_ruhe_s\":%u,\"aktiv_w\":%.1f,\"abfrage_s\":%u,\"anzeige_s\":%u,\"sensor\":%d,"
           "\"vib_aktiv\":%d,\"vib_aktiv_mg\":%u,\"crc\":%u}",
           (unsigned)einst.senden_aktiv_s, (unsigned)einst.senden_ruhe_s, einst.aktiv_dW / 10.0f, (unsigned)einst.abfrage_s,
           (unsigned)einst.anzeige_s, (einst.schalter & cfg::SCHALTER_SENSOR) ? 1 : 0,
           (einst.schalter & cfg::SCHALTER_VIB_AKTIV) ? 1 : 0, (unsigned)einst.vib_aktiv_mg, (unsigned)cfg::pruefsumme(einst));
  return String(b);
}

void starteEmpfang() {
  dio1 = false;
  int s = radio.startReceive();
  if (s != RADIOLIB_ERR_NONE) Serial.printf("# startReceive Fehler %d\n", s);
}

bool sende(const uint8_t* buf, size_t len, uint32_t dauerMs) {
  dio1 = false;
  int s = radio.startTransmit(buf, len);
  if (s != RADIOLIB_ERR_NONE) {
    Serial.printf("# startTransmit Fehler %d\n", s);
    starteEmpfang();
    return false;
  }
  sendetGerade = true;
  txStartMs = millis();
  sendezeit.buche(dauerMs);
  return true;
}

// ================================================================ Sender (WASC)

bool frageShelly(int i) {
  HTTPClient http;
  http.setConnectTimeout(800);
  http.setTimeout(1500);
  String url = String("http://") + SHELLY_IP[i] + "/rpc/Shelly.GetStatus";
  if (!http.begin(netz, url)) return false;
  int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();
  JsonDocument filter;
  filter["pm1:0"]["apower"] = true;
  filter["pm1:0"]["aenergy"]["total"] = true;
  filter["switch:0"]["apower"] = true;
  filter["switch:0"]["aenergy"]["total"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return false;
  JsonVariant k = doc["pm1:0"];
  if (k.isNull()) k = doc["switch:0"];   // falls spaeter ein Shelly mit Relais dazukommt
  if (k.isNull() || !k["apower"].is<float>()) return false;
  mess[i].leistung = k["apower"].as<float>();
  mess[i].energie = k["aenergy"]["total"] | mess[i].energie;
  return true;
}

void abfrageSchleife() {
  if (wartetAufQuittung || sendetGerade || millis() - letzteAbfrageMs < einst.abfrage_s * 1000UL) return;
  letzteAbfrageMs = millis();
  int i = naechsteAbfrage;
  naechsteAbfrage = (naechsteAbfrage + 1) % MASCHINEN;
  bool ok = frageShelly(i);
  Messung& m = mess[i];
  if (ok) {
    m.fehler = 0;
    m.erreichbar = true;
    m.zeitMs = millis();
  } else if (++m.fehler >= ABFRAGE_FEHLER_BIS_WEG) {
    if (m.erreichbar) Serial.printf("# %s nicht erreichbar\n", MASCHINE_NAME[i]);
    m.erreichbar = false;
    m.leistung = 0;
  }
}

bool aktiv(int i) { return mess[i].erreichbar && mess[i].leistung * 10.0f > einst.aktiv_dW; }

bool sensorAn() { return (einst.schalter & cfg::SCHALTER_SENSOR) != 0; }

// Vibration zaehlt nur dann wie eine laufende Maschine, wenn das in den Einstellungen eingeschaltet ist
bool vibAktiv() {
  return sensorAn() && (einst.schalter & cfg::SCHALTER_VIB_AKTIV) && adxl::da && millis() - vibRahmenMs < 10000 &&
         vibRms >= einst.vib_aktiv_mg;
}

// Fertigen Rahmen des Beschleunigungssensors auswerten (rund 40 ms Rechenzeit je 2,56 s)
void vibSchleife() {
  adxl::an = sensorAn();
  if (sendetGerade) return;        // nach dem Senden muss der Empfang sofort an sein: Rechnen wartet solange
  int i = adxl::fertig;
  if (i < 0) return;
  const adxl::Puffer& p = adxl::puffer[i];
  vib::Rahmen r = vib::rechne(p.x, p.y, p.z);
  adxl::fertig = -1;
  vibFenster.dazu(r);
  vibRms = r.rms;
  vibDrehHz = r.drehHz;
  vibDreh = r.dreh;
  vibRahmenMs = millis();
}

void baueRahmen() {
  funk::Sendung s{};
  funk::Daten& d = s.d;
  d.laufzeit_s = millis() / 1000;
  for (int i = 0; i < MASCHINEN; i++) {
    d.m[i].flags = mess[i].erreichbar ? funk::FLAG_ERREICHBAR : 0;
    float w = mess[i].leistung < 0 ? 0 : mess[i].leistung;
    d.m[i].leistung_dW = (uint16_t)min(65535.0f, roundf(w * 10));
    d.m[i].energie_Wh = (uint32_t)(mess[i].energie > 0 ? lroundf(mess[i].energie) : 0);
    gesendetAktiv[i] = aktiv(i);
    gesendetErreichbar[i] = mess[i].erreichbar;
  }
  d.quittungRssi = (int8_t)constrain(lroundf(eigenRssi), -128, 127);
  d.quittungSnr4 = (int8_t)constrain(lroundf(eigenSnr * 4), -128, 127);
  d.verloren = (uint16_t)min<uint32_t>(verloren, 65535);
  d.cfgCrc = cfg::pruefsumme(einst);
  bool sensorDa = adxl::da;
  d.sensor = (sensorAn() ? funk::SENSOR_AN : 0) | (sensorDa ? funk::SENSOR_DA : 0);
  size_t len = sizeof(funk::Daten);
  if (sensorAn() && (sensorDa || vibFenster.rahmen)) {
    vibFenster.verworfen = adxl::verworfen;
    vibFenster.fehler = adxl::fehler;
    adxl::verworfen = false;
    adxl::fehler = false;
    s.v = vibFenster.block(sensorDa);
    vibFenster.leeren();
    vibGesendet = s.v;
    vibGesendetDa = true;
    d.bloecke |= funk::BLOCK_VIBRATION;
    len = sizeof(funk::Sendung);
  }
  fcnt++;
  if (fcnt % 256 == 0) prefs.putUInt("fcnt", fcnt);
  rahmenLen = funk::packe(rahmen, funk::DATEN, funk::HOCH, SENDER_ID, fcnt, &s, len);
  rahmenToaMs = toaMs(rahmenLen);
}

void senderSchleife() {
  uint32_t jetzt = millis();
  if (wartetAufQuittung && (int32_t)(jetzt - quittungBisMs) > 0) {
    wartetAufQuittung = false;
    if (versuch < VERSUCHE) {
      wiederholenAbMs = jetzt + 800 + esp_random() % 1700;
      Serial.printf("# keine Quittung fuer %lu, Versuch %u\n", (unsigned long)fcnt, (unsigned)versuch + 1);
    } else {
      verloren++;
      sendungOffen = false;
      letztesSendenMs = jetzt;
      led(40, 0, 0, 500);
      Serial.printf("WASC,VERLOREN,%lu\n", (unsigned long)fcnt);
    }
  }
  if (sendetGerade || wartetAufQuittung) return;

  // offene Sendung wiederholen (dieselben Bytes)
  if (sendungOffen) {
    if ((int32_t)(jetzt - wiederholenAbMs) < 0) return;
    if (sendezeit.letzteStunde() + rahmenToaMs > SENDEZEIT_MAX * 3600000UL) return;
    versuch++;
    sende(rahmen, rahmenLen, rahmenToaMs);
    return;
  }

  bool wechsel = erstesSenden;
  bool irgendwasAktiv = false;
  for (int i = 0; i < MASCHINEN; i++) {
    if (aktiv(i) != gesendetAktiv[i] || mess[i].erreichbar != gesendetErreichbar[i]) wechsel = true;
    if (aktiv(i)) irgendwasAktiv = true;
  }
  if (vibAktiv()) irgendwasAktiv = true;
  uint32_t abstand = (irgendwasAktiv ? einst.senden_aktiv_s : einst.senden_ruhe_s) * 1000UL;
  uint32_t seit = jetzt - letztesSendenMs;
  bool faellig = seit >= abstand || ((wechsel || sofortSenden) && seit >= SENDEN_MIN_ABSTAND_MS);
  if (erstesSenden && jetzt < 15000) faellig = false;   // erst die Shellys einmal abfragen
  if (!faellig) return;
  if (sendezeit.letzteStunde() + toaDatenMaxMs > SENDEZEIT_MAX * 3600000UL) return;
  baueRahmen();
  if (!rahmenLen) {
    Serial.println("# Rahmen konnte nicht verschluesselt werden");
    letztesSendenMs = jetzt;
    return;
  }
  erstesSenden = false;
  sofortSenden = false;
  sendungOffen = true;
  versuch = 1;
  sende(rahmen, rahmenLen, rahmenToaMs);
}

void senderEmpfang(const uint8_t* buf, size_t len, float rssi, float snr) {
  funk::Kopf k;
  funk::Antwort a{};
  size_t n = 0;
  if (!funk::entpackeVar(buf, len, funk::QUITTUNG, funk::RUNTER, k, &a, sizeof(funk::Quittung), sizeof(funk::Antwort), n) ||
      k.sender != SENDER_ID) {
    Serial.printf("# fremdes Paket %u Byte rssi=%.0f snr=%.1f\n", (unsigned)len, rssi, snr);
    return;
  }
  if (!sendungOffen || k.fcnt != fcnt) return;   // alte Quittung
  const funk::Quittung& q = a.q;
  wartetAufQuittung = false;
  sendungOffen = false;
  letztesSendenMs = millis();
  letzteQuittungMs = millis();
  okZahl++;
  quittungRssi = q.rssi10 / 10.0f;
  quittungSnr = q.snr10 / 10.0f;
  eigenRssi = rssi;
  eigenSnr = snr;
  led(0, 30, 0, 300);
  Serial.printf("WASC,OK,%lu,versuch,%u,hin,%.1f,%.1f,zur,%.0f,%.1f\n", (unsigned long)fcnt, (unsigned)versuch,
                quittungRssi, quittungSnr, rssi, snr);
  // Das Gateway haengt Einstellungen an, wenn unsere Pruefsumme nicht zu seiner passt
  if ((q.bloecke & funk::BLOCK_EINSTELLUNGEN) && n == sizeof(funk::Antwort)) {
    cfg::Einstellungen e = a.e;
    cfg::begrenzen(e);
    if (!cfg::gleich(e, einst)) {
      einst = e;
      speichereEinstellungen();
      sofortSenden = true;                       // bald mit der neuen Pruefsumme bestaetigen
      zeigeHinweis("Einst. neu", 4000);
      Serial.printf("# neue Einstellungen aus der Wohnung: %s\n", einstJson().c_str());
    }
  }
}

void seiteSender() {
  String s;
  s.reserve(4000);
  s += F("<!doctype html><html lang=de><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
         "<meta http-equiv=refresh content=10><title>Waschküche Sender</title><style>body{font:15px system-ui,sans-serif;margin:16px}"
         "td,th{padding:4px 8px;border-bottom:1px solid #ccc;text-align:right}th:first-child,td:first-child{text-align:left}"
         "</style></head><body><h2>Waschküche Sender</h2><table><tr><th>Maschine</th><th>Shelly</th><th>Leistung</th><th>Zähler</th></tr>");
  char b[320];
  for (int i = 0; i < MASCHINEN; i++) {
    snprintf(b, sizeof(b), "<tr><td>%s</td><td>%s %s</td><td>%.1f W</td><td>%.0f Wh</td></tr>", MASCHINE_NAME[i], SHELLY_IP[i],
             mess[i].erreichbar ? "ok" : "weg", mess[i].leistung, mess[i].energie);
    s += b;
  }
  snprintf(b, sizeof(b), "</table><p>Firmware %s, läuft seit %lu min, %d Geräte im WLAN %s.</p>", FW_VERSION,
           (unsigned long)(millis() / 60000), (int)WiFi.softAPgetStationNum(), AP_SSID);
  s += b;
  snprintf(b, sizeof(b), "<p>Funk: %lu quittiert, %lu verloren, Zähler %lu, Sendezeit letzte Stunde %.2f %%.</p>",
           (unsigned long)okZahl, (unsigned long)verloren, (unsigned long)fcnt, sendezeit.letzteStunde() / 36000.0f);
  s += b;
  if (letzteQuittungMs) {
    snprintf(b, sizeof(b), "<p>Letzte Quittung vor %lu s. Wohnung hört uns mit %.0f dBm / %+.1f dB, wir hören sie mit %.0f dBm / %+.1f dB.</p>",
             (unsigned long)((millis() - letzteQuittungMs) / 1000), quittungRssi, quittungSnr, eigenRssi, eigenSnr);
    s += b;
  }
  if (!sensorAn()) {
    s += F("<p>Beschleunigungssensor: ausgeschaltet.</p>");
  } else if (!adxl::da) {
    s += F("<p>Beschleunigungssensor: nicht gefunden (Unit Accel am Grove-Port?).</p>");
  } else {
    snprintf(b, sizeof(b), "<p>Beschleunigungssensor: Vibration %.1f mg, stärkste Linie %.1f Hz (%.0f U/min) mit %.1f mg, vor %lu s. "
                           "Rahmen %lu, verworfen %lu, I2C-Fehler %lu.</p>",
             vibRms, vibDrehHz, vibDrehHz * 60, vibDreh, (unsigned long)((millis() - vibRahmenMs) / 1000),
             (unsigned long)adxl::rahmenZahl, (unsigned long)adxl::verworfenZahl, (unsigned long)adxl::fehlerZahl);
    s += b;
    if (vibGesendetDa && vibGesendet.rahmen) {
      s += F("<p>Zuletzt gesendet, je Band in mg:");
      for (int i = 0; i < vib::BAENDER; i++) {
        snprintf(b, sizeof(b), " %g–%g Hz %.1f%s", vib::BAND_HZ[i], vib::BAND_HZ[i + 1], vib::mg(vibGesendet.band_db2[i]),
                 i + 1 < vib::BAENDER ? "," : ".</p>");
        s += b;
      }
    }
  }
  s += "<p>Einstellungen (aus der Wohnung): " + einstJson() + "</p>";
  s += F("<p><a href=/update>Firmware aktualisieren</a></p></body></html>");
  web.send(200, "text/html; charset=utf-8", s);
}

void starteSender() {
#ifdef ROLLE_WASC
  WiFi.mode(WIFI_AP);
  IPAddress ip(AP_IP[0], AP_IP[1], AP_IP[2], AP_IP[3]);
  WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASS, AP_KANAL, 0, 4);
  web.on("/", seiteSender);
  updater.setup(&web, "/update", "admin", OTA_PASS_WASC);
  web.begin();
  if (prefs.isKey("fcnt")) {
    fcnt = prefs.getUInt("fcnt", 0) + 256;   // nie einen alten Zaehler wiederverwenden
  } else {
    // Frisch geloeschtes Geraet: Start aus der Bauzeit (Sekunden seit 2026). Der Zaehler waechst hoechstens um
    // 1 je 10 s (SENDEN_MIN_ABSTAND_MS), ein spaeter gebautes Image startet deshalb ueber allem, was dieses
    // Geraet mit demselben Schluessel je gesendet hat. So wiederholt sich keine Nonce, und das Gateway muss
    // seinen Zaehler nie von selbst zuruecksetzen (alte Pakete bleiben ausgesperrt).
    fcnt = (uint32_t)(BAU_ZEIT - ZAEHLER_EPOCHE);
  }
  prefs.putUInt("fcnt", fcnt);
  adxl::an = sensorAn();
  adxl::starte(GROVE_SDA, GROVE_SCL);
  Serial.printf("# Sender: WLAN %s auf %s, Shellys %s / %s, Zaehler ab %lu\n", AP_SSID, ip.toString().c_str(), SHELLY_IP[0],
                SHELLY_IP[1], (unsigned long)fcnt);
  Serial.printf("# Einstellungen %s\n", einstJson().c_str());
#endif
}

// ================================================================ Gateway (WOHN)

void mqttVeroeffentlichen(const char* unterthema, const String& json, bool behalten = false) {
#if HAT_MQTT
  if (!mqtt.connected()) return;
  String t = String(MQTT_BASIS "/") + unterthema;
  mqtt.publish(t.c_str(), json.c_str(), behalten);
#endif
}

String diagJson() {
  char b[600];
  snprintf(b, sizeof(b),
           "{\"fw\":\"%s\",\"alter_s\":%ld,\"rssi\":%.1f,\"snr\":%.1f,\"fcnt\":%lu,\"sender_laufzeit_s\":%lu,"
           "\"quittung_rssi\":%d,\"quittung_snr\":%.2f,\"verloren\":%u,\"empfangen\":%lu,\"doppelt\":%lu,\"ungueltig\":%lu,"
           "\"crc_fehler\":%lu,\"luecken\":%lu,\"gw_laufzeit_s\":%lu,\"wlan_rssi\":%d,"
           "\"einst_ok\":%d,\"einst_gesendet\":%lu,\"sensor_an\":%d,\"sensor_da\":%d,\"veraltet\":%lu}",
           FW_VERSION, zuletztDa ? (long)((millis() - zuletztMs) / 1000) : -1L, zuletztRssi, zuletztSnr,
           (unsigned long)letzterFcnt, (unsigned long)(zuletztDa ? zuletzt.laufzeit_s : 0),
           zuletztDa ? (int)zuletzt.quittungRssi : 0, zuletztDa ? zuletzt.quittungSnr4 / 4.0f : 0.0f,
           zuletztDa ? (unsigned)zuletzt.verloren : 0u, (unsigned long)empfangen, (unsigned long)doppelt,
           (unsigned long)ungueltig, (unsigned long)crcFehler, (unsigned long)fcntLuecken, (unsigned long)(millis() / 1000),
           (int)WiFi.RSSI(), (zuletztDa && zuletzt.cfgCrc == cfg::pruefsumme(einst)) ? 1 : 0, (unsigned long)einstGesendet,
           (zuletztDa && (zuletzt.sensor & funk::SENSOR_AN)) ? 1 : 0, (zuletztDa && (zuletzt.sensor & funk::SENSOR_DA)) ? 1 : 0,
           (unsigned long)veraltet);
  return String(b);
}

// Vibrationsblock der letzten Sendung, Werte in mg. b0..b11 = Frequenzbaender (vib::BAND_HZ in vibration.h).
String vibJson() {
  const vib::Block& v = zuletztVib;
  char b[560];
  int n = snprintf(b, sizeof(b),
                   "{\"ok\":%d,\"rahmen\":%u,\"verworfen\":%d,\"fehler\":%d,\"rms\":%.1f,\"rms_max\":%.1f,\"spitze\":%u,"
                   "\"dreh_hz\":%.2f,\"dreh_upm\":%u,\"dreh\":%.2f,\"lage_x\":%.2f,\"lage_y\":%.2f,\"lage_z\":%.2f",
                   (v.zustand & vib::ZUSTAND_OK) ? 1 : 0, (unsigned)v.rahmen, (v.zustand & vib::ZUSTAND_VERWORFEN) ? 1 : 0,
                   (v.zustand & vib::ZUSTAND_FEHLER) ? 1 : 0, v.rms_mg10 / 10.0f, v.rms_max_mg10 / 10.0f, (unsigned)v.spitze_mg,
                   v.dreh_hz4 / 4.0f, (unsigned)v.dreh_hz4 * 15u, vib::mg(v.dreh_db2), v.lage[0] / 64.0f, v.lage[1] / 64.0f,
                   v.lage[2] / 64.0f);
  for (int i = 0; i < vib::BAENDER && n > 0 && n < (int)sizeof(b) - 20; i++)
    n += snprintf(b + n, sizeof(b) - n, ",\"b%d\":%.2f", i, vib::mg(v.band_db2[i]));
  if (n > 0 && n < (int)sizeof(b) - 2) snprintf(b + n, sizeof(b) - n, "}");
  return String(b);
}

// Gewuenschte Einstellungen des Senders uebernehmen (Gateway). Sie gehen mit der naechsten Quittung hinunter.
void setzeEinstellungen(cfg::Einstellungen e, const char* woher) {
  cfg::begrenzen(e);
  if (cfg::gleich(e, einst)) return;
  einst = e;
  speichereEinstellungen();
  Serial.printf("# Einstellungen geaendert (%s): %s\n", woher, einstJson().c_str());
  mqttVeroeffentlichen("einstellungen", einstJson(), true);
}

String maschineJson(int i) {
  char b[120];
  const funk::Maschine& m = zuletzt.m[i];
  snprintf(b, sizeof(b), "{\"power\":%.1f,\"energy\":%lu,\"online\":%s}", m.leistung_dW / 10.0f, (unsigned long)m.energie_Wh,
           (m.flags & funk::FLAG_ERREICHBAR) ? "true" : "false");
  return String(b);
}

void gatewayEmpfang(const uint8_t* buf, size_t len, float rssi, float snr) {
  funk::Kopf k;
  funk::Sendung s{};
  size_t n = 0;
  if (!funk::entpackeVar(buf, len, funk::DATEN, funk::HOCH, k, &s, sizeof(funk::Daten), sizeof(funk::Sendung), n) ||
      k.sender != SENDER_ID) {
    ungueltig++;
    Serial.printf("# fremdes/ungueltiges Paket %u Byte rssi=%.0f snr=%.1f\n", (unsigned)len, rssi, snr);
    return;
  }
  const funk::Daten& d = s.d;
  bool mitVib = (d.bloecke & funk::BLOCK_VIBRATION) && n == sizeof(funk::Sendung);
  if (fcntDa && k.fcnt == letzterFcnt && letzteQuittungLen) {   // Quittung ging verloren: dieselbe nochmal
    doppelt++;
    delay(20);
    sende(letzteQuittung, letzteQuittungLen, toaMs(letzteQuittungLen));
    return;
  }
  if (fcntDa && k.fcnt < letzterFcnt) {
    // Echt, aber alt: jemand spielt ein aufgezeichnetes Paket wieder ab, oder der Sender wurde geloescht und mit
    // falscher Uhr gebaut. Nie von selbst zuruecksetzen; dafuer gibt es "Zaehler vergessen" auf /einstellungen.
    veraltet++;
    veraltetMs = millis();
    Serial.printf("# alter Zaehler %lu < %lu, verworfen\n", (unsigned long)k.fcnt, (unsigned long)letzterFcnt);
    return;
  }
  // Luecken nur zaehlen, wenn der Sender durchlief: nach seinem Neustart springt der Zaehler absichtlich
  bool senderNeu = !zuletztDa || d.laufzeit_s < zuletzt.laufzeit_s;
  if (fcntDa && !senderNeu && k.fcnt > letzterFcnt + 1 && k.fcnt - letzterFcnt < 1000) fcntLuecken += k.fcnt - letzterFcnt - 1;
  letzterFcnt = k.fcnt;
  fcntDa = true;
  if (letzterFcnt - gespeicherterFcnt >= 64) {
    gespeicherterFcnt = letzterFcnt;
    prefs.putUInt("gwfcnt", gespeicherterFcnt);
  }

  // erst quittieren, dann veroeffentlichen. Arbeitet der Sender mit anderen Einstellungen, gehen sie mit.
  funk::Antwort a{};
  a.q.rssi10 = (int16_t)lroundf(rssi * 10);
  a.q.snr10 = (int16_t)lroundf(snr * 10);
  size_t alen = sizeof(funk::Quittung);
  if (d.cfgCrc != cfg::pruefsumme(einst)) {
    a.q.bloecke |= funk::BLOCK_EINSTELLUNGEN;
    a.e = einst;
    alen = sizeof(funk::Antwort);
    einstGesendet++;
  }
  letzteQuittungLen = funk::packe(letzteQuittung, funk::QUITTUNG, funk::RUNTER, k.sender, k.fcnt, &a, alen);
  delay(20);
  if (letzteQuittungLen) sende(letzteQuittung, letzteQuittungLen, toaMs(letzteQuittungLen));

  zuletzt = d;
  zuletztDa = true;
  zuletztMs = millis();
  zuletztRssi = rssi;
  zuletztSnr = snr;
  empfangen++;
  led(0, 0, 30, 300);
  Serial.printf("WOHN,DATEN,%lu,%.0f,%.1f,wm,%.1f,%lu,tr,%.1f,%lu\n", (unsigned long)k.fcnt, rssi, snr,
                d.m[0].leistung_dW / 10.0f, (unsigned long)d.m[0].energie_Wh, d.m[1].leistung_dW / 10.0f,
                (unsigned long)d.m[1].energie_Wh);
  for (int i = 0; i < MASCHINEN; i++) mqttVeroeffentlichen(MASCHINE_NAME[i], maschineJson(i));
  if (mitVib) {
    zuletztVib = s.v;
    zuletztVibDa = true;
    zuletztVibMs = millis();
    Serial.printf("WOHN,VIB,%lu,%s\n", (unsigned long)k.fcnt, vibJson().c_str());
    if (s.v.rahmen) mqttVeroeffentlichen("vibration", vibJson());   // ohne ausgewerteten Rahmen keine Nullen ins MQTT
  }
  mqttVeroeffentlichen("diag", diagJson());
  letzteDiagMs = millis();
}

void seiteGateway() {
  String s;
  s.reserve(3000);
  s += F("<!doctype html><html lang=de><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
         "<meta http-equiv=refresh content=15><title>Waschküche</title><style>"
         "body{font:15px system-ui,sans-serif;margin:16px;background:#fff;color:#111}"
         "@media(prefers-color-scheme:dark){body{background:#111;color:#eee}td,th{border-color:#444!important}}"
         "td,th{padding:4px 8px;border-bottom:1px solid #ccc;text-align:right}th:first-child,td:first-child{text-align:left}"
         "</style></head><body><h2>Waschküche</h2>");
  char b[260];
  if (zuletztDa) {
    s += F("<table><tr><th>Maschine</th><th>Steckdose</th><th>Leistung</th><th>Zähler</th></tr>");
    for (int i = 0; i < MASCHINEN; i++) {
      const funk::Maschine& m = zuletzt.m[i];
      snprintf(b, sizeof(b), "<tr><td>%s</td><td>%s</td><td>%.1f W</td><td>%lu Wh</td></tr>", MASCHINE_NAME[i],
               (m.flags & funk::FLAG_ERREICHBAR) ? "ok" : "nicht erreichbar", m.leistung_dW / 10.0f,
               (unsigned long)m.energie_Wh);
      s += b;
    }
    s += F("</table>");
    snprintf(b, sizeof(b), "<p>Letzte Sendung vor %lu s, Zähler %lu, Empfang %.0f dBm / %+.1f dB. Sender läuft seit %lu min, %u ohne Quittung.</p>",
             (unsigned long)((millis() - zuletztMs) / 1000), (unsigned long)letzterFcnt, zuletztRssi, zuletztSnr,
             (unsigned long)(zuletzt.laufzeit_s / 60), (unsigned)zuletzt.verloren);
    s += b;
  } else {
    s += F("<p>Noch keine Sendung aus der Waschküche empfangen.</p>");
  }
  if (zuletztDa && !(zuletzt.sensor & funk::SENSOR_AN)) {
    s += F("<p>Beschleunigungssensor: ausgeschaltet.</p>");
  } else if (zuletztDa && !(zuletzt.sensor & funk::SENSOR_DA)) {
    s += F("<p>Beschleunigungssensor: am Sender nicht gefunden.</p>");
  } else if (zuletztVibDa) {
    const vib::Block& v = zuletztVib;
    snprintf(b, sizeof(b), "<p>Vibration (vor %lu s, %u Rahmen): %.1f mg im Mittel, höchstens %.1f mg, Spitze %u mg. "
                           "Stärkste Linie %.2f Hz (%u U/min) mit %.1f mg.</p><p>Je Band in mg:",
             (unsigned long)((millis() - zuletztVibMs) / 1000), (unsigned)v.rahmen, v.rms_mg10 / 10.0f, v.rms_max_mg10 / 10.0f,
             (unsigned)v.spitze_mg, v.dreh_hz4 / 4.0f, (unsigned)v.dreh_hz4 * 15u, vib::mg(v.dreh_db2));
    s += b;
    for (int i = 0; i < vib::BAENDER; i++) {
      snprintf(b, sizeof(b), " %g–%g Hz %.1f%s", vib::BAND_HZ[i], vib::BAND_HZ[i + 1], vib::mg(v.band_db2[i]),
               i + 1 < vib::BAENDER ? "," : ".</p>");
      s += b;
    }
  }
  if (zuletztDa) {
    s += (zuletzt.cfgCrc == cfg::pruefsumme(einst)) ? F("<p>Einstellungen: vom Sender übernommen. ")
                                                    : F("<p>Einstellungen: gehen mit der nächsten Quittung zum Sender. ");
    s += F("<a href=/einstellungen>ändern</a></p>");
  }
  snprintf(b, sizeof(b), "<p>Gateway %s, läuft seit %lu min, WLAN %d dBm, MQTT %s. Empfangen %lu, doppelt %lu, ungültig %lu, CRC-Fehler %lu, Lücken %lu, alter Zähler %lu.</p>",
           FW_VERSION, (unsigned long)(millis() / 60000), (int)WiFi.RSSI(),
           HAT_MQTT ? (mqtt.connected() ? "verbunden" : "getrennt") : "nicht eingerichtet", (unsigned long)empfangen,
           (unsigned long)doppelt, (unsigned long)ungueltig, (unsigned long)crcFehler, (unsigned long)fcntLuecken,
           (unsigned long)veraltet);
  s += b;
  if (veraltet && millis() - veraltetMs < 3600000UL)
    s += F("<p><b>Sendungen mit altem Zähler verworfen.</b> Nach dem Löschen des Senders: auf der Seite Einstellungen „Zähler vergessen“.</p>");
  s += F("<p><a href=/einstellungen>Einstellungen des Senders</a> · <a href=/api/status>/api/status</a></p></body></html>");
  web.send(200, "text/html; charset=utf-8", s);
}

// Formular fuer die Einstellungen des Senders. Gespeichert wird im Gateway, uebertragen mit der naechsten Quittung.
void seiteEinstellungen() {
  String s;
  s.reserve(3200);
  s += F("<!doctype html><html lang=de><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
         "<title>Waschküche Einstellungen</title><style>"
         "body{font:15px system-ui,sans-serif;margin:16px;background:#fff;color:#111;max-width:520px}"
         "@media(prefers-color-scheme:dark){body{background:#111;color:#eee}input{background:#222;color:#eee;border:1px solid #555}}"
         "label{display:block;margin:12px 0 2px}input[type=number]{width:7em;font:inherit;padding:4px}small{opacity:.75}"
         "button{font:inherit;padding:8px 16px;margin-top:16px}</style></head><body><h2>Einstellungen des Senders</h2>");
  char b[420];
  if (zuletztDa)
    s += (zuletzt.cfgCrc == cfg::pruefsumme(einst))
           ? F("<p>Der Sender arbeitet mit diesen Werten.</p>")
           : F("<p><b>Noch nicht beim Sender.</b> Die Werte gehen mit der nächsten Quittung hinunter (spätestens nach dem nächsten Lebenszeichen).</p>");
  else s += F("<p>Seit dem Start des Gateways kam noch keine Sendung. Die Werte gehen mit der ersten Quittung hinunter.</p>");
  s += F("<form method=post action=/einstellungen>");
  snprintf(b, sizeof(b), "<label>Sendetakt, solange eine Maschine läuft (s)</label><input type=number name=senden_aktiv_s min=10 max=600 value=%u> "
                         "<small>10–600, Standard 30</small>", (unsigned)einst.senden_aktiv_s);
  s += b;
  snprintf(b, sizeof(b), "<label>Sendetakt in Ruhe, Lebenszeichen (s)</label><input type=number name=senden_ruhe_s min=60 max=900 value=%u> "
                         "<small>60–900, Standard 300. Der Server meldet nach 30 min ohne Sendung.</small>", (unsigned)einst.senden_ruhe_s);
  s += b;
  snprintf(b, sizeof(b), "<label>Maschine läuft ab (W)</label><input type=number name=aktiv_w min=1 max=2000 step=0.1 value=%.1f> "
                         "<small>1–2000, Standard 5</small>", einst.aktiv_dW / 10.0f);
  s += b;
  snprintf(b, sizeof(b), "<label>Abstand zwischen zwei Shelly-Abfragen (s)</label><input type=number name=abfrage_s min=2 max=60 value=%u> "
                         "<small>2–60, Standard 5 (jede Steckdose alle 10 s)</small>", (unsigned)einst.abfrage_s);
  s += b;
  snprintf(b, sizeof(b), "<label>Display und LED aus nach (s)</label><input type=number name=anzeige_s min=10 max=250 value=%u> "
                         "<small>10–250, Standard 60</small>", (unsigned)einst.anzeige_s);
  s += b;
  snprintf(b, sizeof(b), "<label><input type=checkbox name=sensor value=1%s> Beschleunigungssensor auswerten</label>"
                         "<label><input type=checkbox name=vib_aktiv value=1%s> Vibration zählt wie eine laufende Maschine (schneller Sendetakt)</label>",
           (einst.schalter & cfg::SCHALTER_SENSOR) ? " checked" : "", (einst.schalter & cfg::SCHALTER_VIB_AKTIV) ? " checked" : "");
  s += b;
  snprintf(b, sizeof(b), "<label>… ab einer Vibration von (mg)</label><input type=number name=vib_aktiv_mg min=5 max=250 value=%u> "
                         "<small>5–250, Standard 30. Der Sensor rauscht in Ruhe mit etwa 7 mg.</small>", (unsigned)einst.vib_aktiv_mg);
  s += b;
  s += F("<br><button type=submit>Speichern und senden</button></form>");
  snprintf(b, sizeof(b), "<h3>Sendezähler</h3><p>Letzter Zähler %lu, mit altem Zähler verworfen: %lu. <small>Das Gateway nimmt nur steigende "
                         "Zähler an. Nur wenn der Sender nach dem Löschen nicht mehr angenommen wird:</small></p>"
                         "<form method=post action=/zaehler><button type=submit>Zähler vergessen</button></form>",
           (unsigned long)letzterFcnt, (unsigned long)veraltet);
  s += b;
  s += F("<p><a href=/>zurück</a></p></body></html>");
  web.send(200, "text/html; charset=utf-8", s);
}

// Nur die eigenen Formulare duerfen speichern: eine fremde Webseite im Browser koennte sonst blind hierher posten.
bool eigeneHerkunft() {
  String herkunft = web.header("Origin");
  if (!herkunft.length() || herkunft == "http://" + web.hostHeader()) return true;
  web.send(403, "text/plain", "fremde Herkunft");
  return false;
}

// Gateway vergisst den Sendezaehler und nimmt die naechste echte Sendung mit jedem Zaehlerstand an
void zaehlerVergessen() {
  if (!eigeneHerkunft()) return;
  fcntDa = false;
  letzterFcnt = gespeicherterFcnt = 0;
  letzteQuittungLen = 0;
  veraltet = 0;
  prefs.remove("gwfcnt");
  Serial.println("# Sendezaehler vergessen (Webseite)");
  web.sendHeader("Location", "/einstellungen");
  web.send(303, "text/plain", "");
}

void einstellungenSpeichern() {
  if (!eigeneHerkunft()) return;
  cfg::Einstellungen e = einst;
  if (web.hasArg("senden_aktiv_s")) e.senden_aktiv_s = (uint16_t)constrain(web.arg("senden_aktiv_s").toInt(), 0L, 65535L);
  if (web.hasArg("senden_ruhe_s")) e.senden_ruhe_s = (uint16_t)constrain(web.arg("senden_ruhe_s").toInt(), 0L, 65535L);
  if (web.hasArg("aktiv_w")) e.aktiv_dW = (uint16_t)constrain(lroundf(web.arg("aktiv_w").toFloat() * 10), 0L, 65535L);
  if (web.hasArg("abfrage_s")) e.abfrage_s = (uint8_t)constrain(web.arg("abfrage_s").toInt(), 0L, 255L);
  if (web.hasArg("anzeige_s")) e.anzeige_s = (uint8_t)constrain(web.arg("anzeige_s").toInt(), 0L, 255L);
  if (web.hasArg("vib_aktiv_mg")) e.vib_aktiv_mg = (uint8_t)constrain(web.arg("vib_aktiv_mg").toInt(), 0L, 255L);
  // Haken, die nicht gesetzt sind, schickt der Browser nicht mit
  e.schalter = (web.hasArg("sensor") ? cfg::SCHALTER_SENSOR : 0) | (web.hasArg("vib_aktiv") ? cfg::SCHALTER_VIB_AKTIV : 0);
  setzeEinstellungen(e, "Webseite");
  web.sendHeader("Location", "/einstellungen");
  web.send(303, "text/plain", "");
}

// MQTT sensors/waschkueche/set: JSON mit einem oder mehreren Werten, Namen wie in .../einstellungen
void mqttEmpfang(char* thema, uint8_t* daten, unsigned int len) {
  if (strcmp(thema, MQTT_BASIS "/set") != 0) return;
  JsonDocument doc;
  if (deserializeJson(doc, daten, len) || !doc.is<JsonObject>()) {
    Serial.println("# MQTT set: kein JSON-Objekt");
    return;
  }
  cfg::Einstellungen e = einst;
  if (doc["senden_aktiv_s"].is<float>()) e.senden_aktiv_s = (uint16_t)constrain(doc["senden_aktiv_s"].as<long>(), 0L, 65535L);
  if (doc["senden_ruhe_s"].is<float>()) e.senden_ruhe_s = (uint16_t)constrain(doc["senden_ruhe_s"].as<long>(), 0L, 65535L);
  if (doc["aktiv_w"].is<float>()) e.aktiv_dW = (uint16_t)constrain(lroundf(doc["aktiv_w"].as<float>() * 10), 0L, 65535L);
  if (doc["abfrage_s"].is<float>()) e.abfrage_s = (uint8_t)constrain(doc["abfrage_s"].as<long>(), 0L, 255L);
  if (doc["anzeige_s"].is<float>()) e.anzeige_s = (uint8_t)constrain(doc["anzeige_s"].as<long>(), 0L, 255L);
  if (doc["vib_aktiv_mg"].is<float>()) e.vib_aktiv_mg = (uint8_t)constrain(doc["vib_aktiv_mg"].as<long>(), 0L, 255L);
  if (!doc["sensor"].isNull()) e.schalter = doc["sensor"].as<int>() ? (e.schalter | cfg::SCHALTER_SENSOR) : (e.schalter & ~cfg::SCHALTER_SENSOR);
  if (!doc["vib_aktiv"].isNull())
    e.schalter = doc["vib_aktiv"].as<int>() ? (e.schalter | cfg::SCHALTER_VIB_AKTIV) : (e.schalter & ~cfg::SCHALTER_VIB_AKTIV);
  setzeEinstellungen(e, "MQTT");
}

void seiteGatewayJson() {
  String s = "{\"diag\":" + diagJson();
  if (zuletztDa) {
    for (int i = 0; i < MASCHINEN; i++) s += String(",\"") + MASCHINE_NAME[i] + "\":" + maschineJson(i);
  }
  if (zuletztVibDa) s += ",\"vibration\":" + vibJson();
  s += ",\"einstellungen\":" + einstJson() + "}";
  web.send(200, "application/json", s);
}

void starteGateway() {
#ifdef ROLLE_WOHN
  gespeicherterFcnt = letzterFcnt = prefs.getUInt("gwfcnt", 0);
  fcntDa = letzterFcnt > 0;
  WiFi.setHostname(HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  // feste Adresse nur, wenn in config.h eingeschaltet (sonst DHCP); ohne DNS-Angabe bliebe die Namensaufloesung leer
  if (WOHN_FESTE_IP)
    WiFi.config(IPAddress(WOHN_IP[0], WOHN_IP[1], WOHN_IP[2], WOHN_IP[3]), IPAddress(WOHN_GW[0], WOHN_GW[1], WOHN_GW[2], WOHN_GW[3]),
                IPAddress(WOHN_MASKE[0], WOHN_MASKE[1], WOHN_MASKE[2], WOHN_MASKE[3]),
                IPAddress(WOHN_DNS[0], WOHN_DNS[1], WOHN_DNS[2], WOHN_DNS[3]));
  WiFi.begin(WIFI_SSID, WIFI_PASS);
#if HAT_MQTT
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(896);
  mqtt.setKeepAlive(30);
  mqtt.setCallback(mqttEmpfang);
#endif
#endif
}

void gatewaySchleife() {
#ifdef ROLLE_WOHN
  bool verbunden = WiFi.status() == WL_CONNECTED;
  if (verbunden && !netzGestartet) {
    netzGestartet = true;
    Serial.printf("# WLAN verbunden, IP %s, RSSI %d dBm\n", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
    web.on("/", seiteGateway);
    web.on("/api/status", seiteGatewayJson);
    web.on("/einstellungen", HTTP_GET, seiteEinstellungen);
    web.on("/einstellungen", HTTP_POST, einstellungenSpeichern);
    web.on("/zaehler", HTTP_POST, zaehlerVergessen);
    static const char* kopfzeilen[] = {"Origin"};
    web.collectHeaders(kopfzeilen, 1);
    web.begin();
    ArduinoOTA.setHostname(HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASS);
    ArduinoOTA.onStart([] {
      otaLaeuft = true;
      radio.standby();
    });
    ArduinoOTA.onError([](ota_error_t e) {
      Serial.printf("# OTA Fehler %u, Neustart\n", (unsigned)e);
      delay(500);
      ESP.restart();
    });
    ArduinoOTA.begin();
  }
  if (!netzGestartet) return;
  ArduinoOTA.handle();
  if (otaLaeuft) return;
#if HAT_MQTT
  if (verbunden && !mqtt.connected() && millis() - letzterMqttVersuchMs > 10000) {
    letzterMqttVersuchMs = millis();
    String lwt = MQTT_BASIS "/status";
    if (mqtt.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASS, lwt.c_str(), 1, true, "offline")) {
      mqtt.publish(lwt.c_str(), "online", true);
      mqtt.subscribe(MQTT_BASIS "/set");
      mqttVeroeffentlichen("einstellungen", einstJson(), true);
      Serial.println("# MQTT verbunden");
    } else {
      Serial.printf("# MQTT Fehler %d\n", mqtt.state());
    }
  }
  mqtt.loop();
  if (millis() - letzteDiagMs >= DIAG_MS) {
    letzteDiagMs = millis();
    mqttVeroeffentlichen("diag", diagJson());
  }
#endif
#endif
}

// ================================================================ Funk

void funkSchleife() {
  if (funkStatus != RADIOLIB_ERR_NONE || otaLaeuft || !dio1) return;
  dio1 = false;
  if (sendetGerade) {
    radio.finishTransmit();
    sendetGerade = false;
    txEndeMs = millis();
    if (rolle == WASC && sendungOffen) {
      wartetAufQuittung = true;
      quittungBisMs = millis() + toaQuittungMaxMs + 1000;
    }
    starteEmpfang();
    return;
  }
  uint8_t buf[96];
  size_t len = radio.getPacketLength();
  int s = radio.readData(buf, min(len, sizeof(buf)));
  float rssi = radio.getRSSI();
  float snr = radio.getSNR();
  if (s == RADIOLIB_ERR_NONE && len <= sizeof(buf)) {
    if (rolle == WASC) senderEmpfang(buf, len, rssi, snr);
    else gatewayEmpfang(buf, len, rssi, snr);
  } else if (s == RADIOLIB_ERR_CRC_MISMATCH) {
    crcFehler++;
  } else if (s != RADIOLIB_ERR_NONE) {
    Serial.printf("# readData Fehler %d\n", s);
  }
  if (!sendetGerade) starteEmpfang();
}

// ================================================================ Taster und Anzeige

void tasterSchleife() {
  static bool war = false, langErledigt = false;
  static uint32_t seit = 0, letzteAbfrage = 0;
  if (!expanderOk || millis() - letzteAbfrage < 20) return;
  letzteAbfrage = millis();
  if (sendetGerade || millis() - txEndeMs < 300) return;   // waehrend und kurz nach dem Senden nicht lesen
  bool unten = c6l::buttonDown();
  if (unten && !war) {
    seit = millis();
    // erster Druck bei dunklem Display weckt nur (sendet nichts)
    langErledigt = anzeigeAus;
    letzterTastendruckMs = millis();
    if (anzeigeAus) {
      anzeigeAus = false;
      oled.setPowerSave(0);
    }
  }
  if (!unten && war && !langErledigt && millis() - seit >= 50) {
    if (rolle == WASC) {
      sofortSenden = true;
      zeigeHinweis("sende ...");
    } else {
      char h[16];
      snprintf(h, sizeof(h), "IP .%u", (unsigned)WiFi.localIP()[3]);
      zeigeHinweis(h, 4000);
    }
  }
  war = unten;
}

void zeile(int nr, const char* text) { oled.drawStr(0, 7 + nr * 8, text); }

void anzeigen() {
  if (rolle == WASC && !anzeigeAus && millis() - letzterTastendruckMs >= einst.anzeige_s * 1000UL && !otaLaeuft) {
    anzeigeAus = true;
    led(0, 0, 0, 0);
    oled.clearBuffer();
    oled.sendBuffer();
    oled.setPowerSave(1);
  }
  if (anzeigeAus) return;
  char z[6][16];
  memset(z, 0, sizeof(z));
  if (otaLaeuft) {
    snprintf(z[0], 16, "Update");
    snprintf(z[1], 16, "laeuft ...");
  } else if (!expanderOk || funkStatus != RADIOLIB_ERR_NONE || !funk::schluesselOk) {
    snprintf(z[0], 16, "%s %s", rollenName(rolle), FW_VERSION);
    snprintf(z[1], 16, "%s", expanderOk ? "IO ok" : "IO Fehler");
    snprintf(z[2], 16, "LoRa %d", funkStatus);
    snprintf(z[3], 16, "%s", funk::schluesselOk ? "Schluessel ok" : "Schluessel?");
  } else if (rolle == WASC) {
    snprintf(z[0], 16, "WASC %s", FW_VERSION);
    for (int i = 0; i < MASCHINEN; i++) {
      if (mess[i].erreichbar) snprintf(z[1 + i], 16, "%s %5.0fW", MASCHINE_KURZ[i], mess[i].leistung);
      else snprintf(z[1 + i], 16, "%s  --", MASCHINE_KURZ[i]);
    }
    if (letzteQuittungMs) snprintf(z[3], 16, "ok %.0f/%+.0f", quittungRssi, quittungSnr);
    else snprintf(z[3], 16, "noch kein ok");
    snprintf(z[4], 16, "weg %lu", (unsigned long)verloren);
    // unterste Zeile im Wechsel: Geraete im WLAN und, mit Sensor, die Vibration
    if (sensorAn() && adxl::da && (millis() / 2000) % 2) snprintf(z[5], 16, "Vib %.0fmg", vibRms);
    else if (sensorAn() && !adxl::da && (millis() / 2000) % 2) snprintf(z[5], 16, "kein Sensor");
    else snprintf(z[5], 16, "AP %d Geraete", (int)WiFi.softAPgetStationNum());
  } else {
    snprintf(z[0], 16, "WOHN %s", FW_VERSION);
    if (zuletztDa) {
      for (int i = 0; i < MASCHINEN; i++) {
        if (zuletzt.m[i].flags & funk::FLAG_ERREICHBAR)
          snprintf(z[1 + i], 16, "%s %5.0fW", MASCHINE_KURZ[i], zuletzt.m[i].leistung_dW / 10.0f);
        else snprintf(z[1 + i], 16, "%s  --", MASCHINE_KURZ[i]);
      }
      snprintf(z[3], 16, "vor %lus", (unsigned long)((millis() - zuletztMs) / 1000));
      snprintf(z[4], 16, "%.0f/%+.1f", zuletztRssi, zuletztSnr);
    } else {
      snprintf(z[1], 16, "warte auf");
      snprintf(z[2], 16, "Waschkueche");
    }
    if (WiFi.status() != WL_CONNECTED) snprintf(z[5], 16, "WLAN sucht");
    else if (HAT_MQTT && !mqtt.connected()) snprintf(z[5], 16, "MQTT weg");
    else snprintf(z[5], 16, "%s", HAT_MQTT ? "MQTT ok" : "ohne MQTT");
  }
  if (hinweisBisMs && (int32_t)(millis() - hinweisBisMs) < 0) snprintf(z[5], 16, "%s", hinweis);
  oled.clearBuffer();
  oled.setFont(u8g2_font_5x8_tf);
  for (int i = 0; i < 6; i++) zeile(i, z[i]);
  oled.sendBuffer();
}

// ================================================================ Start

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);
#endif
  delay(300);
  prefs.begin("wk", false);
  ladeEinstellungen();

  expanderOk = c6l::begin();
  SPI.begin(c6l::PIN_SCK, c6l::PIN_MISO, c6l::PIN_MOSI, -1);
  oled.begin();
  oled.setContrast(130);
  oled.clearBuffer();
  oled.setFont(u8g2_font_5x8_tf);
  zeile(0, "Waschkueche");
  zeile(1, FW_VERSION);
  zeile(3, "Rolle:");
  zeile(4, rollenName(rolle));
  oled.sendBuffer();
  led(10, 10, 10, 800);

  if (!funk::begin(LORA_KEY_HEX)) Serial.println("# LORA_KEY_HEX fehlt oder ist kein 32-stelliger Hex-Wert");

  funkStatus = radio.begin(FREQ_MHZ, BW_KHZ, SF, CR, SYNC_WORD, TX_DBM, PREAMBEL, c6l::LORA_TCXO_V, false);
  if (funkStatus == RADIOLIB_ERR_NONE) {
    radio.setDio2AsRfSwitch(true);
    radio.setCurrentLimit(140.0);
    radio.setRxBoostedGainMode(true);
    radio.setDio1Action(aufDio1);
    toaDatenMaxMs = toaMs(funk::LEN_DATEN_MAX);
    toaQuittungMs = toaMs(funk::LEN_QUITTUNG);
    toaQuittungMaxMs = toaMs(funk::LEN_QUITTUNG_MAX);
    starteEmpfang();
  }

  if (rolle == WASC) starteSender();
  else starteGateway();

  Serial.printf("# Waschkueche %s, Rolle %s, IO %s, LoRa %d, Schluessel %s, MQTT %s\n", FW_VERSION, rollenName(rolle),
                expanderOk ? "ok" : "FEHLER", funkStatus, funk::schluesselOk ? "ok" : "FEHLT", HAT_MQTT ? "ja" : "nein");
  Serial.printf("# %.3f MHz SF%u BW %.0f kHz %d dBm, Sendedauer Daten %lu ms (mit Vibration %lu ms), Quittung %lu ms (mit Einstellungen %lu ms)\n",
                FREQ_MHZ, (unsigned)SF, BW_KHZ, (int)TX_DBM, (unsigned long)toaMs(funk::LEN_DATEN), (unsigned long)toaDatenMaxMs,
                (unsigned long)toaQuittungMs, (unsigned long)toaQuittungMaxMs);
}

void loop() {
  funkSchleife();
  if (rolle == WASC) {
    vibSchleife();
    funkSchleife();
    abfrageSchleife();
    funkSchleife();
    if (funk::schluesselOk) senderSchleife();
  } else {
    gatewaySchleife();
  }
  if (!otaLaeuft && (rolle == WASC || netzGestartet)) web.handleClient();
  tasterSchleife();
  if (ledAusMs && (int32_t)(millis() - ledAusMs) >= 0) led(0, 0, 0, 0);
  static uint32_t letzteAnzeige = 0;
  if (millis() - letzteAnzeige >= 500) {
    letzteAnzeige = millis();
    anzeigen();
  }
  static uint32_t letzterStatus = 0;
  if (millis() - letzterStatus >= 30000) {
    letzterStatus = millis();
    Serial.printf("# status fw=%s rolle=%s lora=%d ok=%lu weg=%lu empfangen=%lu sendezeit=%.2f%%\n", FW_VERSION,
                  rollenName(rolle), funkStatus, (unsigned long)okZahl, (unsigned long)verloren, (unsigned long)empfangen,
                  sendezeit.letzteStunde() / 36000.0f);
  }
  delay(2);
}
