// Auswertung des Beschleunigungssensors (M5Stack Unit Accel, ADXL345) an der Seitenwand der Waschmaschine.
// Rohdaten (200 Messwerte je Sekunde und Achse) passen nicht durch LoRa. Der Sender rechnet deshalb selbst:
// je 2,56 s ein Rahmen mit 512 Werten je Achse, daraus Gesamt-Vibration, Spitze, staerkste Linie im
// Schleuderbereich und zwoelf Frequenzbaender. Ueber die Zeit zwischen zwei Sendungen wird gemittelt und das
// Ergebnis als 25-Byte-Block an die Sendung gehaengt. Daraus laesst sich spaeter auf dem Server das Programm erkennen.
// Reines C++ ohne Arduino, damit es sich am Rechner testen laesst (test/test_vibration.cpp).
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

namespace vib {

constexpr int N = 512;                    // Werte je Rahmen
constexpr float FS = 200.0f;              // Abtastrate in Hz
constexpr float MG_JE_LSB = 3.9f;         // ADXL345, volle Aufloesung
constexpr int BAENDER = 12;
// Bandgrenzen in Hz. Trommel beim Waschen und im Trockner um 0,8 Hz; Schleudern 400-1600 U/min = 6,7-26,7 Hz
// (Baender 2-7 je 240 U/min breit); 45-55 Hz Netzbrumm, Pumpen, Trocknermotor.
constexpr float BAND_HZ[BAENDER + 1] = {0.4f, 1.6f, 4, 8, 12, 16, 20, 24, 28, 45, 55, 75, 95};
constexpr float DREH_VON_HZ = 3.0f;       // staerkste Linie wird zwischen 180 und 1800 U/min gesucht
constexpr float DREH_BIS_HZ = 30.0f;

constexpr uint8_t ZUSTAND_OK = 0x01;        // Sensor antwortet
constexpr uint8_t ZUSTAND_VERWORFEN = 0x02; // im Fenster gingen Messwerte verloren (Rahmen verworfen)
constexpr uint8_t ZUSTAND_FEHLER = 0x04;    // im Fenster gab es I2C-Fehler

// So geht es ueber Funk (Werte in db2: 0,5-dB-Schritte ueber 0,01 mg, siehe db2()/mg())
struct __attribute__((packed)) Block {
  uint8_t rahmen;          // ausgewertete Rahmen im Fenster (hoechstens 255), 0 = keine Daten
  uint8_t zustand;         // ZUSTAND_*
  uint16_t rms_mg10;       // Gesamt-Vibration (ohne Erdbeschleunigung), Mittel ueber das Fenster, 0,1 mg
  uint16_t rms_max_mg10;   // staerkster Rahmen im Fenster
  uint16_t spitze_mg;      // hoechster Einzelwert im Fenster, mg
  uint8_t dreh_hz4;        // staerkste Linie 3-30 Hz in 0,25 Hz (mal 15 = U/min), aus dem Rahmen, in dem sie am staerksten war
  uint8_t dreh_db2;        // ihre Staerke
  uint8_t band_db2[BAENDER];  // Vibration je Frequenzband, Mittel ueber das Fenster
  int8_t lage[3];          // Erdbeschleunigung je Achse in 1/64 g (zeigt, ob der Sensor verrutscht oder abgefallen ist)
};
static_assert(sizeof(Block) == 25, "Vibrationsblock muss 25 Byte lang sein");

inline uint8_t db2(float mg) {
  if (!(mg > 0.01f)) return 0;
  float v = 40.0f * log10f(mg / 0.01f);
  return v >= 255.0f ? 255 : (uint8_t)lroundf(v);
}
inline float mg(uint8_t b) { return b == 0 ? 0.0f : 0.01f * powf(10.0f, b / 40.0f); }

// ---- Tabellen und Arbeitsspeicher (einmal, nicht auf dem Stack) ----
inline float twRe[N / 2], twIm[N / 2];    // cos/sin(2 pi k / N)
inline float arbRe[N], arbIm[N];
inline float leistung[N / 2];             // Summe der drei Achsen, |X|^2 je Linie
inline int bandVon[BAENDER], bandBis[BAENDER];   // Linien [von, bis)
inline int drehVon = 0, drehBis = 0;
inline bool tabellenDa = false;

inline void tabellen() {
  if (tabellenDa) return;
  for (int k = 0; k < N / 2; k++) {
    float w = 2.0f * (float)M_PI * k / N;
    twRe[k] = cosf(w);
    twIm[k] = sinf(w);
  }
  for (int b = 0; b < BAENDER; b++) {
    bandVon[b] = (int)ceilf(BAND_HZ[b] * N / FS - 1e-3f);
    bandBis[b] = (int)ceilf(BAND_HZ[b + 1] * N / FS - 1e-3f);
    if (bandVon[b] < 1) bandVon[b] = 1;
    if (bandBis[b] > N / 2) bandBis[b] = N / 2;
  }
  drehVon = (int)ceilf(DREH_VON_HZ * N / FS);
  drehBis = (int)floorf(DREH_BIS_HZ * N / FS);
  tabellenDa = true;
}

inline void fft(float* re, float* im) {
  for (int i = 1, j = 0; i < N; i++) {
    int bit = N >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (int len = 2; len <= N; len <<= 1) {
    int halb = len >> 1, schritt = N / len;
    for (int i = 0; i < N; i += len) {
      for (int k = 0; k < halb; k++) {
        float wr = twRe[k * schritt], wi = -twIm[k * schritt];
        int a = i + k, b = a + halb;
        float vr = re[b] * wr - im[b] * wi;
        float vi = re[b] * wi + im[b] * wr;
        re[b] = re[a] - vr;
        im[b] = im[a] - vi;
        re[a] += vr;
        im[a] += vi;
      }
    }
  }
}

// Ergebnis eines Rahmens, alles in mg
struct Rahmen {
  float rms = 0, spitze = 0;
  float band[BAENDER] = {0};      // Leistung (mg^2) je Band
  float drehHz = 0, dreh = 0;     // staerkste Linie 3-30 Hz und ihr Effektivwert
  float lage[3] = {0, 0, 0};      // Mittelwert je Achse in mg
};

// Mit Hann-Fenster gilt fuer ein Band: Varianz = 2 * Summe |X[k]|^2 / (0,375 * N^2)
constexpr float SKALA = 2.0f / (0.375f * N * N);

inline Rahmen rechne(const int16_t x[N], const int16_t y[N], const int16_t z[N]) {
  tabellen();
  const int16_t* a[3] = {x, y, z};
  Rahmen r;
  float mittel[3];
  for (int c = 0; c < 3; c++) {
    long s = 0;
    for (int n = 0; n < N; n++) s += a[c][n];
    mittel[c] = (float)s / N;
    r.lage[c] = mittel[c] * MG_JE_LSB;
  }
  float summe = 0, spitze2 = 0;
  for (int n = 0; n < N; n++) {
    float v2 = 0;
    for (int c = 0; c < 3; c++) {
      float d = a[c][n] - mittel[c];
      v2 += d * d;
    }
    summe += v2;
    if (v2 > spitze2) spitze2 = v2;
  }
  r.rms = sqrtf(summe / N) * MG_JE_LSB;
  r.spitze = sqrtf(spitze2) * MG_JE_LSB;

  memset(leistung, 0, sizeof(leistung));
  for (int c = 0; c < 3; c++) {
    for (int n = 0; n < N; n++) {
      float hann = 0.5f - 0.5f * (n < N / 2 ? twRe[n] : -twRe[n - N / 2]);
      arbRe[n] = (a[c][n] - mittel[c]) * hann;
      arbIm[n] = 0;
    }
    fft(arbRe, arbIm);
    for (int k = 1; k < N / 2; k++) leistung[k] += arbRe[k] * arbRe[k] + arbIm[k] * arbIm[k];
  }
  const float mg2 = SKALA * MG_JE_LSB * MG_JE_LSB;
  for (int b = 0; b < BAENDER; b++) {
    float s = 0;
    for (int k = bandVon[b]; k < bandBis[b]; k++) s += leistung[k];
    r.band[b] = s * mg2;
  }
  int kmax = drehVon;
  for (int k = drehVon; k <= drehBis; k++)
    if (leistung[k] > leistung[kmax]) kmax = k;
  float p = leistung[kmax - 1] + leistung[kmax] + leistung[kmax + 1];
  if (p > 0) {
    float schwerpunkt = ((kmax - 1) * leistung[kmax - 1] + kmax * leistung[kmax] + (kmax + 1) * leistung[kmax + 1]) / p;
    r.drehHz = schwerpunkt * FS / N;
    r.dreh = sqrtf(p * mg2);
  }
  return r;
}

// Sammelt die Rahmen zwischen zwei Sendungen
struct Fenster {
  uint32_t rahmen = 0;
  float summeRms2 = 0, maxRms = 0, spitze = 0;
  float band[BAENDER] = {0};
  float drehHz = 0, dreh = 0;
  float lage[3] = {0, 0, 0};
  bool verworfen = false, fehler = false;

  void dazu(const Rahmen& r) {
    rahmen++;
    summeRms2 += r.rms * r.rms;
    if (r.rms > maxRms) maxRms = r.rms;
    if (r.spitze > spitze) spitze = r.spitze;
    for (int b = 0; b < BAENDER; b++) band[b] += r.band[b];
    if (r.dreh > dreh) {
      dreh = r.dreh;
      drehHz = r.drehHz;
    }
    for (int c = 0; c < 3; c++) lage[c] += r.lage[c];
  }

  Block block(bool sensorOk) const {
    Block b;
    memset(&b, 0, sizeof(b));
    b.zustand = (sensorOk ? ZUSTAND_OK : 0) | (verworfen ? ZUSTAND_VERWORFEN : 0) | (fehler ? ZUSTAND_FEHLER : 0);
    if (!rahmen) return b;
    b.rahmen = rahmen > 255 ? 255 : (uint8_t)rahmen;
    float rms = sqrtf(summeRms2 / rahmen);
    b.rms_mg10 = (uint16_t)fminf(65535.0f, roundf(rms * 10));
    b.rms_max_mg10 = (uint16_t)fminf(65535.0f, roundf(maxRms * 10));
    b.spitze_mg = (uint16_t)fminf(65535.0f, roundf(spitze));
    b.dreh_hz4 = (uint8_t)fminf(255.0f, roundf(drehHz * 4));
    b.dreh_db2 = db2(dreh);
    for (int i = 0; i < BAENDER; i++) b.band_db2[i] = db2(sqrtf(band[i] / rahmen));
    for (int c = 0; c < 3; c++) {
      float g64 = roundf(lage[c] / rahmen * 64.0f / 1000.0f);
      b.lage[c] = (int8_t)(g64 < -127 ? -127 : (g64 > 127 ? 127 : g64));
    }
    return b;
  }

  void leeren() { *this = Fenster(); }
};

}  // namespace vib
