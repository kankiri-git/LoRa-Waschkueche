// Einstellungen des Senders, die sich aus der Wohnung per LoRa aendern lassen (ohne Besuch in der Waschkueche).
// Das Gateway haelt die gewuenschten Werte, der Sender meldet in jeder Sendung die Pruefsumme seiner Werte.
// Weichen sie ab, haengt das Gateway die Einstellungen an die naechste Quittung. Funkparameter (Frequenz, SF,
// Leistung) stehen bewusst nicht hier: ein falscher Wert wuerde die Strecke kappen, ueber die man ihn korrigiert.
// Reines C++ ohne Arduino, damit es sich am Rechner testen laesst (test/test_vibration.cpp).
#pragma once
#include <stdint.h>
#include <string.h>

namespace cfg {

constexpr uint8_t SCHALTER_SENSOR = 0x01;      // Beschleunigungssensor auswerten und mitsenden
constexpr uint8_t SCHALTER_VIB_AKTIV = 0x02;   // Vibration ab vib_aktiv_mg zaehlt wie eine laufende Maschine
constexpr uint8_t SCHALTER_MASKE = 0x03;

struct __attribute__((packed)) Einstellungen {
  uint16_t senden_aktiv_s;   // Sendetakt, solange eine Maschine laeuft
  uint16_t senden_ruhe_s;    // Sendetakt sonst (Lebenszeichen)
  uint16_t aktiv_dW;         // Leistung in 0,1 W, ab der eine Maschine als laufend gilt
  uint8_t abfrage_s;         // Abstand zwischen zwei Shelly-Abfragen (abwechselnd, je Shelly also das Doppelte)
  uint8_t anzeige_s;         // Display und LED nach so vielen Sekunden aus
  uint8_t schalter;          // SCHALTER_*
  uint8_t vib_aktiv_mg;      // Schwelle fuer SCHALTER_VIB_AKTIV (Gesamt-RMS in mg)
  uint8_t reserve[2];
};
static_assert(sizeof(Einstellungen) == 12, "Einstellungen muss 12 Byte lang sein");

inline Einstellungen standard() {
  Einstellungen e;
  e.senden_aktiv_s = 30;
  e.senden_ruhe_s = 300;
  e.aktiv_dW = 50;
  e.abfrage_s = 5;
  e.anzeige_s = 60;
  e.schalter = SCHALTER_SENSOR;
  e.vib_aktiv_mg = 30;
  e.reserve[0] = e.reserve[1] = 0;
  return e;
}

template <typename T> inline T grenze(long v, long lo, long hi) { return (T)(v < lo ? lo : (v > hi ? hi : v)); }

// Bringt jeden Wert in seinen erlaubten Bereich. Sender und Gateway rechnen gleich, sonst stimmt die Pruefsumme nie.
inline void begrenzen(Einstellungen& e) {
  e.senden_aktiv_s = grenze<uint16_t>(e.senden_aktiv_s, 10, 600);
  e.senden_ruhe_s = grenze<uint16_t>(e.senden_ruhe_s, 60, 900);    // Alarm auf dem Server nach 30 min ohne Sendung
  if (e.senden_ruhe_s < e.senden_aktiv_s) e.senden_ruhe_s = e.senden_aktiv_s;
  e.aktiv_dW = grenze<uint16_t>(e.aktiv_dW, 10, 20000);            // 1 W .. 2 kW
  e.abfrage_s = grenze<uint8_t>(e.abfrage_s, 2, 60);
  e.anzeige_s = grenze<uint8_t>(e.anzeige_s, 10, 250);
  e.schalter &= SCHALTER_MASKE;
  e.vib_aktiv_mg = grenze<uint8_t>(e.vib_aktiv_mg, 5, 250);
  e.reserve[0] = e.reserve[1] = 0;
}

// CRC-16/CCITT-FALSE
inline uint16_t crc16(const void* daten, size_t len) {
  const uint8_t* p = (const uint8_t*)daten;
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)p[i] << 8;
    for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

inline uint16_t pruefsumme(const Einstellungen& e) { return crc16(&e, sizeof(e)); }

inline bool gleich(const Einstellungen& a, const Einstellungen& b) { return memcmp(&a, &b, sizeof(a)) == 0; }

}  // namespace cfg
