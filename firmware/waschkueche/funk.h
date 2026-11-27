// Funkrahmen: 7 Byte Kopf (lesbar, aber geschuetzt), Nutzdaten AES-128-CCM verschluesselt, 4 Byte Pruefwert.
// Nonce = Richtung + Sender + Zaehler; der Zaehler steigt immer (NVS), deshalb nie dieselbe Nonce zweimal
// mit anderem Inhalt. Wiederholungen schicken exakt dieselben Bytes.
//
// Seit 0.5.0 (PROTO 0xA8, nicht mit aelteren Versionen mischbar):
//   Sendung  = Daten [+ Vibrationsblock, wenn der Beschleunigungssensor Werte hat]
//   Quittung = Quittung [+ Einstellungen, wenn die Pruefsumme des Senders nicht zu der des Gateways passt]
#pragma once
#include <Arduino.h>
#include <mbedtls/ccm.h>
#include "einstellungen.h"
#include "vibration.h"

namespace funk {

constexpr uint8_t PROTO = 0xA8;
enum Typ : uint8_t { DATEN = 1, QUITTUNG = 2 };
enum Richtung : uint8_t { HOCH = 1, RUNTER = 2 };   // HOCH = Waschkueche -> Wohnung
constexpr uint8_t FLAG_ERREICHBAR = 0x01;
constexpr size_t TAG = 4;
constexpr uint8_t BLOCK_VIBRATION = 0x01;       // Daten.bloecke: Vibrationsblock folgt
constexpr uint8_t BLOCK_EINSTELLUNGEN = 0x01;   // Quittung.bloecke: Einstellungen folgen
constexpr uint8_t SENSOR_AN = 0x01;             // Daten.sensor: Auswertung eingeschaltet
constexpr uint8_t SENSOR_DA = 0x02;             // Daten.sensor: Beschleunigungssensor antwortet

struct __attribute__((packed)) Kopf {
  uint8_t proto, typ, sender;
  uint32_t fcnt;
};
struct __attribute__((packed)) Maschine {
  uint8_t flags;
  uint16_t leistung_dW;   // 0,1 W
  uint32_t energie_Wh;    // Zaehlerstand des Shelly
};
struct __attribute__((packed)) Daten {
  uint32_t laufzeit_s;
  Maschine m[2];
  int8_t quittungRssi;    // was der Sender von der letzten Quittung gehoert hat
  int8_t quittungSnr4;    // SNR x4
  uint16_t verloren;      // Sendungen ohne Quittung seit Start
  uint16_t cfgCrc;        // Pruefsumme der Einstellungen, mit denen der Sender gerade arbeitet
  uint8_t bloecke;        // BLOCK_*
  uint8_t sensor;         // SENSOR_*
};
struct __attribute__((packed)) Quittung {
  int16_t rssi10;         // wie das Gateway diese Sendung gehoert hat
  int16_t snr10;
  uint8_t bloecke;        // BLOCK_*
  uint8_t reserve;
};
// Groesste Nutzdaten je Richtung, in dieser Reihenfolge auf dem Funk
struct __attribute__((packed)) Sendung {
  Daten d;
  vib::Block v;
};
struct __attribute__((packed)) Antwort {
  Quittung q;
  cfg::Einstellungen e;
};
static_assert(sizeof(Daten) == 26 && sizeof(Sendung) == 51 && sizeof(Quittung) == 6 && sizeof(Antwort) == 18, "Rahmenlaengen");
constexpr size_t LEN_DATEN = sizeof(Kopf) + sizeof(Daten) + TAG;          // 37
constexpr size_t LEN_DATEN_MAX = sizeof(Kopf) + sizeof(Sendung) + TAG;    // 62 mit Vibrationsblock
constexpr size_t LEN_QUITTUNG = sizeof(Kopf) + sizeof(Quittung) + TAG;    // 17
constexpr size_t LEN_QUITTUNG_MAX = sizeof(Kopf) + sizeof(Antwort) + TAG; // 29 mit Einstellungen
constexpr size_t LEN_MAX = LEN_DATEN_MAX;

inline mbedtls_ccm_context ccm;
inline bool schluesselOk = false;

inline int hexWert(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

inline bool begin(const char* hex) {
  uint8_t key[16];
  if (!hex || strlen(hex) != 32) return false;
  for (int i = 0; i < 16; i++) {
    int h = hexWert(hex[2 * i]), l = hexWert(hex[2 * i + 1]);
    if (h < 0 || l < 0) return false;
    key[i] = (uint8_t)(h << 4 | l);
  }
  mbedtls_ccm_init(&ccm);
  schluesselOk = mbedtls_ccm_setkey(&ccm, MBEDTLS_CIPHER_ID_AES, key, 128) == 0;
  memset(key, 0, sizeof(key));
  return schluesselOk;
}

inline void nonce(uint8_t n[13], Richtung r, uint8_t sender, uint32_t fcnt) {
  memset(n, 0, 13);
  n[0] = r;
  n[1] = sender;
  memcpy(n + 2, &fcnt, 4);
}

// Schreibt Kopf + verschluesselte Nutzdaten + Pruefwert nach buf. Gibt die Laenge zurueck, 0 bei Fehler.
inline size_t packe(uint8_t* buf, Typ typ, Richtung r, uint8_t sender, uint32_t fcnt, const void* daten, size_t len) {
  if (!schluesselOk) return 0;
  Kopf k{PROTO, typ, sender, fcnt};
  memcpy(buf, &k, sizeof(k));
  uint8_t n[13];
  nonce(n, r, sender, fcnt);
  if (mbedtls_ccm_encrypt_and_tag(&ccm, len, n, sizeof(n), buf, sizeof(k), (const uint8_t*)daten, buf + sizeof(k),
                                  buf + sizeof(k) + len, TAG) != 0)
    return 0;
  return sizeof(k) + len + TAG;
}

// Wie entpacke, aber die Nutzdaten duerfen kurz (ohne Block) oder lang (mit Block) sein. len = empfangene Laenge.
inline bool entpackeVar(const uint8_t* buf, size_t buflen, Typ typ, Richtung r, Kopf& k, void* daten, size_t kurz, size_t lang,
                        size_t& len) {
  if (!schluesselOk || buflen < sizeof(Kopf) + TAG) return false;
  len = buflen - sizeof(Kopf) - TAG;
  if (len != kurz && len != lang) return false;
  memcpy(&k, buf, sizeof(k));
  if (k.proto != PROTO || k.typ != typ) return false;
  uint8_t n[13];
  nonce(n, r, k.sender, k.fcnt);
  return mbedtls_ccm_auth_decrypt(&ccm, len, n, sizeof(n), buf, sizeof(k), buf + sizeof(k), (uint8_t*)daten,
                                  buf + sizeof(k) + len, TAG) == 0;
}

// Prueft und entschluesselt. false bei falscher Laenge, fremdem Rahmen oder falschem Pruefwert.
inline bool entpacke(const uint8_t* buf, size_t buflen, Typ typ, Richtung r, Kopf& k, void* daten, size_t len) {
  if (!schluesselOk || buflen != sizeof(Kopf) + len + TAG) return false;
  memcpy(&k, buf, sizeof(k));
  if (k.proto != PROTO || k.typ != typ) return false;
  uint8_t n[13];
  nonce(n, r, k.sender, k.fcnt);
  return mbedtls_ccm_auth_decrypt(&ccm, len, n, sizeof(n), buf, sizeof(k), buf + sizeof(k), (uint8_t*)daten,
                                  buf + sizeof(k) + len, TAG) == 0;
}

// Sendezeit der letzten Stunde in 60 Minuten-Faechern
struct Sendezeit {
  uint32_t ms[60] = {0};
  uint32_t minute[60] = {0};
  void buche(uint32_t dauerMs) {
    uint32_t m = millis() / 60000;
    int i = m % 60;
    if (minute[i] != m) { minute[i] = m; ms[i] = 0; }
    ms[i] += dauerMs;
  }
  uint32_t letzteStunde() const {
    uint32_t m = millis() / 60000, summe = 0;
    for (int i = 0; i < 60; i++)
      if (m - minute[i] < 60) summe += ms[i];
    return summe;
  }
};

}  // namespace funk
