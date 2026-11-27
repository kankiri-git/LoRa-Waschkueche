// Waschkueche: feste Einstellungen. Geheimes steht in secrets.h (Vorlage secrets.h.example).
#pragma once
#include <Arduino.h>

#define FW_VERSION "0.5.1"
#define OLED_DREHUNG U8G2_R2   // R0 steht auf dem Kopf

// ---- Funk (beide Geraete gleich) ----
// 869,525 MHz im Band 869,40-869,65 MHz: max. 500 mW ERP, 10 % Sendezeit.
// Reichweitentest im Original-Aufbau (Waschkueche in einem anderen Haus, rund vier Stockwerke): im freien Raum der
// Waschkueche -61..-66 dBm, SNR +1,5..+5 dB; SF10 schafft bis -15 dB, also rund 16-19 dB Reserve. Zwischen den
// Geraetetuermen nur -84 dBm / -10 dB: dort nicht montieren. Vorher mit firmware/lora-test selbst messen.
constexpr float FREQ_MHZ = 869.525f;
constexpr float BW_KHZ = 125.0f;
constexpr uint8_t SF = 10;
constexpr uint8_t CR = 5;                 // 4/5
constexpr int8_t TX_DBM = 22;
constexpr uint16_t PREAMBEL = 12;
constexpr uint8_t SYNC_WORD = 0x12;       // privat; Meshtastic 0x2B, LoRaWAN 0x34
constexpr uint8_t SENDER_ID = 1;

// ---- Sender in der Waschkueche (WASC) ----
// Name und Passwort des eigenen WLANs nur fuer die zwei Shellys stehen in secrets.h (AP_SSID, AP_PASS).
#define AP_KANAL 6
constexpr uint8_t AP_IP[4] = {192, 168, 4, 1};
constexpr int MASCHINEN = 2;
constexpr const char* MASCHINE_NAME[MASCHINEN] = {"waschmaschine", "trockner"};
constexpr const char* MASCHINE_KURZ[MASCHINEN] = {"WM", "TR"};
constexpr const char* SHELLY_IP[MASCHINEN] = {"192.168.4.201", "192.168.4.202"};  // im Shelly fest einstellen (DHCP verteilt .2-.101)
// Sendetakt (30 s / 5 min), Schwelle "laeuft" (5 W), Abfragetakt der Shellys (je 10 s), Anzeigedauer (1 min) und
// der Beschleunigungssensor stehen in einstellungen.h: sie lassen sich aus der Wohnung per LoRa aendern.
constexpr uint8_t ABFRAGE_FEHLER_BIS_WEG = 3;
constexpr uint32_t SENDEN_MIN_ABSTAND_MS = 10000; // bei Zustandswechsel sofort, aber nicht oefter
constexpr uint8_t VERSUCHE = 4;                   // 1 + 3 Wiederholungen, wenn keine Quittung kommt
constexpr float SENDEZEIT_MAX = 0.06f;            // eigene Grenze je Stunde (erlaubt 10 %)
// Grove-Port (PORT.A) fuer den M5Stack Unit Accel (ADXL345) an der Seitenwand der Waschmaschine
constexpr int GROVE_SDA = 5;                      // gelb
constexpr int GROVE_SCL = 4;                      // weiss

// ---- Gateway in der Wohnung (WOHN) ----
#define HOSTNAME "waschkueche-wohn"       // im Heimnetz meist als http://waschkueche-wohn.local/ erreichbar
// Adresse im Heimnetz: Vorgabe DHCP (der Router vergibt sie; am besten dort fest dem Geraet zuordnen).
// Wer die Adresse lieber im Geraet festlegt: WOHN_FESTE_IP = true und die vier Zeilen darunter an das eigene
// Netz anpassen (Beispielwerte fuer ein FRITZ!Box-Netz 192.168.178.x; die Adresse ausserhalb des DHCP-Bereichs
// des Routers waehlen).
constexpr bool WOHN_FESTE_IP = false;
constexpr uint8_t WOHN_IP[4] = {192, 168, 178, 60};
constexpr uint8_t WOHN_GW[4] = {192, 168, 178, 1};
constexpr uint8_t WOHN_MASKE[4] = {255, 255, 255, 0};
constexpr uint8_t WOHN_DNS[4] = {192, 168, 178, 1};
#define MQTT_BASIS "sensors/waschkueche"
#define MQTT_CLIENT "waschkueche-wohn"
constexpr uint32_t DIAG_MS = 60000;               // Lebenszeichen ans MQTT, auch ohne Funk
