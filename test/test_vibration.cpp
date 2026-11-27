// Test am Rechner fuer die Teile der Firmware ohne Arduino:
//   g++ -std=c++17 -O2 -Wall -Wextra -o /tmp/test_vibration test/test_vibration.cpp && /tmp/test_vibration
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <random>
#include "../firmware/waschkueche/einstellungen.h"
#include "../firmware/waschkueche/vibration.h"

static int16_t x[vib::N], y[vib::N], z[vib::N];
static std::mt19937 zufall(1);

// mg -> LSB, mit Erdbeschleunigung auf z und etwas Rauschen wie beim ADXL345
static void signal(float hz, float ampMg, float rauschMg, float phase = 0.3f, float hz2 = 0, float amp2Mg = 0) {
  std::normal_distribution<float> rauschen(0.0f, rauschMg > 0 ? rauschMg : 1e-9f);
  for (int n = 0; n < vib::N; n++) {
    float t = n / vib::FS;
    float s = ampMg * sinf(2 * (float)M_PI * hz * t + phase) + amp2Mg * sinf(2 * (float)M_PI * hz2 * t);
    x[n] = (int16_t)lroundf((s + (rauschMg > 0 ? rauschen(zufall) : 0)) / vib::MG_JE_LSB);
    y[n] = (int16_t)lroundf((rauschMg > 0 ? rauschen(zufall) : 0) / vib::MG_JE_LSB);
    z[n] = (int16_t)lroundf((1000.0f + (rauschMg > 0 ? rauschen(zufall) : 0)) / vib::MG_JE_LSB);
  }
}

static bool nah(float a, float b, float rel) { return fabsf(a - b) <= rel * fabsf(b); }
#define PRUEFE(bed) do { if (!(bed)) { printf("FEHLER Zeile %d: %s\n", __LINE__, #bed); fehler++; } } while (0)

int main() {
  int fehler = 0;

  // --- Bandgrenzen ---
  vib::tabellen();
  for (int b = 0; b < vib::BAENDER; b++) {
    PRUEFE(vib::bandVon[b] < vib::bandBis[b]);
    if (b) PRUEFE(vib::bandVon[b] == vib::bandBis[b - 1]);
  }
  PRUEFE(vib::bandVon[0] == 2 && vib::bandBis[vib::BAENDER - 1] <= vib::N / 2);

  // --- Schleudern 1320 U/min = 22 Hz, 300 mg Amplitude, Rauschen 4 mg je Achse ---
  signal(22.0f, 300.0f, 4.0f);
  vib::Rahmen r = vib::rechne(x, y, z);
  printf("22 Hz/300 mg: rms %.1f spitze %.1f dreh %.2f Hz %.1f mg lage %.0f %.0f %.0f\n", r.rms, r.spitze, r.drehHz, r.dreh,
         r.lage[0], r.lage[1], r.lage[2]);
  PRUEFE(nah(r.rms, 300.0f / sqrtf(2), 0.03f));
  PRUEFE(nah(r.drehHz, 22.0f, 0.01f));
  PRUEFE(nah(r.dreh, 300.0f / sqrtf(2), 0.03f));
  PRUEFE(nah(sqrtf(r.band[6]), 300.0f / sqrtf(2), 0.03f));          // Band 20-24 Hz
  PRUEFE(sqrtf(r.band[3]) < 5.0f && sqrtf(r.band[9]) < 5.0f);
  PRUEFE(nah(r.lage[2], 1000.0f, 0.01f) && fabsf(r.lage[0]) < 5 && fabsf(r.lage[1]) < 5);
  PRUEFE(r.spitze > 290 && r.spitze < 330);

  // --- nur Rauschen: Gesamtwert ~ sqrt(3)*4 mg, je Band wenig ---
  signal(0, 0, 4.0f);
  r = vib::rechne(x, y, z);
  printf("Rauschen 4 mg: rms %.2f, Baender", r.rms);
  for (int b = 0; b < vib::BAENDER; b++) printf(" %.2f", sqrtf(r.band[b]));
  printf(", dreh %.2f mg\n", r.dreh);
  PRUEFE(nah(r.rms, 4.0f * sqrtf(3), 0.15f));
  for (int b = 0; b < vib::BAENDER; b++) PRUEFE(sqrtf(r.band[b]) < 4.0f);

  // --- Trockner 0,85 Hz (51 U/min) 10 mg plus Waschmaschine 13,3 Hz (800 U/min) 80 mg ---
  signal(13.33f, 80.0f, 3.0f, 0.0f, 0.85f, 10.0f);
  r = vib::rechne(x, y, z);
  printf("0,85 Hz/10 mg + 13,3 Hz/80 mg: band0 %.2f band4 %.2f dreh %.2f Hz %.1f mg\n", sqrtf(r.band[0]), sqrtf(r.band[4]), r.drehHz, r.dreh);
  PRUEFE(nah(r.drehHz, 13.33f, 0.01f));
  PRUEFE(nah(sqrtf(r.band[4]), 80.0f / sqrtf(2), 0.05f));           // Band 12-16 Hz
  PRUEFE(sqrtf(r.band[0]) > 4.0f && sqrtf(r.band[0]) < 8.5f);       // 10 mg Amplitude = 7,1 mg effektiv, ein Teil liegt unter 0,78 Hz

  // --- 50 Hz Pumpe 20 mg ---
  signal(50.0f, 20.0f, 3.0f);
  r = vib::rechne(x, y, z);
  PRUEFE(nah(sqrtf(r.band[9]), 20.0f / sqrtf(2), 0.08f));
  PRUEFE(r.dreh < 5.0f);                                            // keine Linie im Schleuderbereich

  // --- Fenster: Mittelung und Block ---
  vib::Fenster f;
  signal(22.0f, 300.0f, 4.0f);
  f.dazu(vib::rechne(x, y, z));
  signal(8.0f, 600.0f, 4.0f);
  f.dazu(vib::rechne(x, y, z));
  signal(0, 0, 4.0f);
  f.dazu(vib::rechne(x, y, z));
  vib::Block b = f.block(true);
  float rmsSoll = sqrtf((300.0f * 300.0f / 2 + 600.0f * 600.0f / 2) / 3);
  printf("Block: rahmen %u rms %.1f max %.1f spitze %u dreh %.2f Hz %.1f mg band6 %.1f lage %d %d %d\n", b.rahmen, b.rms_mg10 / 10.0f,
         b.rms_max_mg10 / 10.0f, b.spitze_mg, b.dreh_hz4 / 4.0f, vib::mg(b.dreh_db2), vib::mg(b.band_db2[6]), b.lage[0], b.lage[1], b.lage[2]);
  PRUEFE(b.rahmen == 3 && b.zustand == vib::ZUSTAND_OK);
  PRUEFE(nah(b.rms_mg10 / 10.0f, rmsSoll, 0.03f));
  PRUEFE(nah(b.rms_max_mg10 / 10.0f, 600.0f / sqrtf(2), 0.03f));
  PRUEFE(b.dreh_hz4 == 32);                                         // 8 Hz, dort war die Linie am staerksten
  PRUEFE(nah(vib::mg(b.dreh_db2), 600.0f / sqrtf(2), 0.06f));
  PRUEFE(nah(vib::mg(b.band_db2[6]), 300.0f / sqrtf(2) / sqrtf(3), 0.08f));   // ueber drei Rahmen gemittelt
  PRUEFE(b.lage[2] == 64 && b.lage[0] == 0);
  f.leeren();
  b = f.block(false);
  PRUEFE(b.rahmen == 0 && b.zustand == 0 && b.rms_mg10 == 0);

  // --- db2 ---
  PRUEFE(vib::db2(0) == 0 && vib::db2(0.01f) == 0 && vib::db2(1.0f) == 80 && vib::db2(1000.0f) == 200 && vib::db2(1e9f) == 255);
  for (float m : {0.05f, 0.7f, 12.0f, 345.0f, 9000.0f}) PRUEFE(nah(vib::mg(vib::db2(m)), m, 0.03f));

  // --- Einstellungen ---
  cfg::Einstellungen e = cfg::standard(), e2 = e;
  cfg::begrenzen(e2);
  PRUEFE(cfg::gleich(e, e2));                                       // Standard liegt im erlaubten Bereich
  PRUEFE(cfg::crc16("123456789", 9) == 0x29B1);
  e2.senden_aktiv_s = 3;
  e2.senden_ruhe_s = 5000;
  e2.aktiv_dW = 0;
  e2.abfrage_s = 0;
  e2.anzeige_s = 0;
  e2.schalter = 0xFF;
  e2.vib_aktiv_mg = 0;
  e2.reserve[0] = 7;
  cfg::begrenzen(e2);
  PRUEFE(e2.senden_aktiv_s == 10 && e2.senden_ruhe_s == 900 && e2.aktiv_dW == 10 && e2.abfrage_s == 2 && e2.anzeige_s == 10);
  PRUEFE(e2.schalter == cfg::SCHALTER_MASKE && e2.vib_aktiv_mg == 5 && e2.reserve[0] == 0);
  PRUEFE(cfg::pruefsumme(e) != cfg::pruefsumme(e2));
  e2 = e;
  e2.senden_aktiv_s = 600;
  e2.senden_ruhe_s = 60;
  cfg::begrenzen(e2);
  PRUEFE(e2.senden_ruhe_s == 600);

  printf(fehler ? "%d FEHLER\n" : "alle Pruefungen bestanden\n", fehler);
  return fehler ? 1 : 0;
}
