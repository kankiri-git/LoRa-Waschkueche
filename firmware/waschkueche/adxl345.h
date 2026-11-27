// M5Stack Unit Accel (U056, ADXL345, I2C 0x53) am Grove-Port des Unit C6L (gelb = G5 = SDA, weiss = G4 = SCL).
// Der ESP32-C6 hat nur einen I2C-Baustein, und der haengt am IO-Expander (Taster, LoRa-Reset). Der Sensor laeuft
// deshalb ueber ein einfaches Software-I2C auf den Grove-Pins, rund 100 kHz, damit auch 2 m Kabel gehen.
// Den Takt treibt der C6L aktiv (der ADXL345 haelt ihn nie fest), die Datenleitung ist Open-Drain.
// Laut Schaltplan hat der Unit Accel Pegelwandler (2x AO3400A) und je 4,7 kOhm Pull-up an beiden Seiten; die
// Datenleitung steigt damit auch an 2 m Kabel schnell genug. Beide Pins treiben mit der kleinsten Staerke,
// damit die Flanken am langen Kabel nicht klingeln und nicht auf die Nachbarader uebersprechen.
// Der ADXL345 sammelt die Messwerte selbst in seinem Puffer (32 Werte), eine eigene Aufgabe holt sie alle 40 ms
// ab und fuellt Rahmen fuer vibration.h. Ohne Sensor passiert nichts, er wird auch spaeter noch erkannt.
#pragma once
#include <Arduino.h>
#include "driver/gpio.h"
#include "vibration.h"

namespace adxl {

constexpr uint8_t ADDR = 0x53;
constexpr uint8_t REG_DEVID = 0x00;       // liest 0xE5
constexpr uint8_t REG_BW_RATE = 0x2C;
constexpr uint8_t REG_POWER_CTL = 0x2D;
constexpr uint8_t REG_INT_ENABLE = 0x2E;
constexpr uint8_t REG_DATA_FORMAT = 0x31;
constexpr uint8_t REG_DATAX0 = 0x32;
constexpr uint8_t REG_FIFO_CTL = 0x38;
constexpr uint8_t REG_FIFO_STATUS = 0x39;
constexpr uint8_t RATE_200HZ = 0x0B;
constexpr uint32_t HALB_US = 4;           // halbe Taktzeit, mit Aufrufzeit rund 100 kHz

inline gpio_num_t sda = GPIO_NUM_NC, scl = GPIO_NUM_NC;

inline void halb() { delayMicroseconds(HALB_US); }
inline void sdaAuf(int v) { gpio_set_level(sda, v); }
inline void sclAuf(int v) { gpio_set_level(scl, v); }

inline void start() {            // auch als wiederholter Start mitten in einer Uebertragung
  sdaAuf(1); halb();
  sclAuf(1); halb();
  sdaAuf(0); halb();
  sclAuf(0); halb();
}
inline void stopp() {
  sdaAuf(0); halb();
  sclAuf(1); halb();
  sdaAuf(1); halb();
}
// true, wenn der Baustein bestaetigt
inline bool sendeByte(uint8_t b) {
  for (int i = 7; i >= 0; i--) {
    sdaAuf((b >> i) & 1); halb();
    sclAuf(1); halb();
    sclAuf(0);
  }
  sdaAuf(1); halb();
  sclAuf(1); halb();
  bool ack = gpio_get_level(sda) == 0;
  sclAuf(0);
  return ack;
}
inline uint8_t liesByte(bool weiter) {
  uint8_t b = 0;
  sdaAuf(1);
  for (int i = 7; i >= 0; i--) {
    halb();
    sclAuf(1); halb();
    if (gpio_get_level(sda)) b |= 1 << i;
    sclAuf(0);
  }
  sdaAuf(weiter ? 0 : 1); halb();
  sclAuf(1); halb();
  sclAuf(0);
  sdaAuf(1);
  return b;
}

inline bool schreibe(uint8_t reg, uint8_t wert) {
  start();
  bool ok = sendeByte(ADDR << 1) && sendeByte(reg) && sendeByte(wert);
  stopp();
  return ok;
}
inline bool lies(uint8_t reg, uint8_t* ziel, int n) {
  start();
  bool ok = sendeByte(ADDR << 1) && sendeByte(reg);
  if (ok) {
    start();
    ok = sendeByte((ADDR << 1) | 1);
    if (ok)
      for (int i = 0; i < n; i++) ziel[i] = liesByte(i + 1 < n);
  }
  stopp();
  return ok;
}

// Pins einrichten, Bus freitakten, Sensor suchen und auf 200 Hz, +-16 g, volle Aufloesung, Puffer "Stream" stellen
inline bool begin(int pinSda, int pinScl) {
  sda = (gpio_num_t)pinSda;
  scl = (gpio_num_t)pinScl;
  pinMode(pinSda, OUTPUT_OPEN_DRAIN | PULLUP);
  pinMode(pinScl, OUTPUT);
  gpio_set_drive_capability(sda, GPIO_DRIVE_CAP_0);
  gpio_set_drive_capability(scl, GPIO_DRIVE_CAP_0);
  sdaAuf(1);
  sclAuf(1);
  delayMicroseconds(50);
  for (int i = 0; i < 9 && gpio_get_level(sda) == 0; i++) {   // haengender Baustein: Takt geben, bis er SDA loslaesst
    sclAuf(0); halb();
    sclAuf(1); halb();
  }
  sclAuf(0); halb();                                           // sauberes Stopp: SDA erst bei Takt unten herunterziehen
  stopp();
  uint8_t id = 0;
  if (!lies(REG_DEVID, &id, 1) || id != 0xE5) return false;
  bool ok = schreibe(REG_POWER_CTL, 0x00);          // Standby
  ok = ok && schreibe(REG_INT_ENABLE, 0x00);
  ok = ok && schreibe(REG_BW_RATE, RATE_200HZ);
  ok = ok && schreibe(REG_DATA_FORMAT, 0x0B);       // volle Aufloesung (3,9 mg je Schritt), +-16 g
  ok = ok && schreibe(REG_FIFO_CTL, 0x00);          // Puffer leeren
  ok = ok && schreibe(REG_FIFO_CTL, 0x80 | 16);     // Stream: die neuesten 32 Werte
  ok = ok && schreibe(REG_POWER_CTL, 0x08);         // messen
  return ok;
}

inline void aus() {
  if (sda != GPIO_NUM_NC) schreibe(REG_POWER_CTL, 0x00);
}

// Anzahl wartender Messwerte (0..32), -1 bei I2C-Fehler
inline int wartend() {
  uint8_t s = 0;
  if (!lies(REG_FIFO_STATUS, &s, 1)) return -1;
  return s & 0x3F;
}

inline bool liesWert(int16_t& x, int16_t& y, int16_t& z) {
  uint8_t d[6];
  if (!lies(REG_DATAX0, d, 6)) return false;
  x = (int16_t)(d[0] | (d[1] << 8));
  y = (int16_t)(d[2] | (d[3] << 8));
  z = (int16_t)(d[4] | (d[5] << 8));
  delayMicroseconds(6);                              // der Puffer braucht 5 us bis zum naechsten Wert
  return true;
}

// ---- Abtasten: zwei Rahmen im Wechsel, die Aufgabe fuellt einen, loop() rechnet den anderen aus ----
struct Puffer {
  int16_t x[vib::N], y[vib::N], z[vib::N];
};
inline Puffer puffer[2];
inline volatile int schreibt = 0;          // in diesen Puffer schreibt die Aufgabe
inline volatile int fertig = -1;           // dieser Puffer wartet auf loop(), -1 = keiner
inline volatile bool an = false;           // von loop() gesetzt: Sensor auswerten
inline volatile bool da = false;           // Sensor antwortet
inline volatile bool verworfen = false;    // seit dem letzten Abholen gingen Werte verloren
inline volatile bool fehler = false;       // seit dem letzten Abholen gab es I2C-Fehler
inline volatile uint32_t fehlerZahl = 0, verworfenZahl = 0, rahmenZahl = 0;
inline int pinSda = -1, pinScl = -1;

inline void aufgabe(void*) {
  int fuell = 0, fehlerFolge = 0;
  uint32_t naechsteSuche = 0;
  for (;;) {
    if (!an) {
      if (da) {
        aus();
        da = false;
      }
      fuell = 0;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (!da) {
      if ((int32_t)(millis() - naechsteSuche) >= 0) {
        naechsteSuche = millis() + 30000;             // spaeter angesteckter Sensor wird nach hoechstens 30 s erkannt
        da = begin(pinSda, pinScl);
        fuell = 0;
        fehlerFolge = 0;
      }
      if (!da) {
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;
      }
    }
    int n = wartend();
    if (n < 0) {
      fehler = true;
      fehlerZahl = fehlerZahl + 1;
      fuell = 0;                                       // Luecke: Rahmen neu beginnen
      if (++fehlerFolge >= 25) da = false;             // 1 s keine Antwort: Sensor weg, neu suchen
      vTaskDelay(pdMS_TO_TICKS(40));
      continue;
    }
    if (n >= 32) {                                     // Puffer war voll: Werte fehlen
      verworfen = true;
      verworfenZahl = verworfenZahl + 1;
      fuell = 0;
    }
    Puffer& p = puffer[schreibt];
    for (int i = 0; i < n; i++) {
      int16_t x, y, z;
      if (!liesWert(x, y, z)) {
        fehler = true;
        fehlerZahl = fehlerZahl + 1;
        fuell = 0;
        fehlerFolge++;
        break;
      }
      fehlerFolge = 0;
      p.x[fuell] = x;
      p.y[fuell] = y;
      p.z[fuell] = z;
      if (++fuell >= vib::N) {
        fuell = 0;
        if (fertig < 0) {                              // loop() ist mit dem anderen Puffer durch
          fertig = schreibt;
          schreibt = schreibt ^ 1;
          rahmenZahl = rahmenZahl + 1;
        } else {
          verworfen = true;                            // loop() kam nicht nach: diesen Rahmen ueberschreiben
          verworfenZahl = verworfenZahl + 1;
        }
        break;                                         // Rest beim naechsten Durchlauf in den neuen Puffer
      }
    }
    vTaskDelay(pdMS_TO_TICKS(40));
  }
}

inline void starte(int pinSdaNeu, int pinSclNeu) {
  pinSda = pinSdaNeu;
  pinScl = pinSclNeu;
  xTaskCreate(aufgabe, "adxl", 4096, nullptr, 2, nullptr);
}

}  // namespace adxl
