// Hardware des M5Stack Unit C6L (U202): ESP32-C6 + SX1262 + 0,66"-OLED.
// Pinbelegung und Initialisierung nach Meshtastic
// variants/esp32c6/m5stack_unitc6l/variant.h + variant.cpp und src/input/i2cButton.cpp.
#pragma once
#include <Arduino.h>
#include <Wire.h>

namespace c6l {

// I2C (nur fuer den IO-Expander)
constexpr int PIN_SDA = 10;
constexpr int PIN_SCL = 8;

// SPI, gemeinsam fuer LoRa und OLED
constexpr int PIN_SCK = 20;
constexpr int PIN_MISO = 22;
constexpr int PIN_MOSI = 21;

// SX1262: Reset haengt am IO-Expander (P7), DIO2 schaltet die Antenne, TCXO 3,0 V an DIO3
constexpr int LORA_CS = 23;
constexpr int LORA_DIO1 = 7;
constexpr int LORA_BUSY = 19;
constexpr float LORA_TCXO_V = 3.0f;

// SSD1306 64x48 an SPI
constexpr int OLED_CS = 6;
constexpr int OLED_DC = 18;
constexpr int OLED_RST = 15;

constexpr int PIN_LED = 2;      // WS2812C
constexpr int PIN_BUZZER = 11;

// IO-Expander PI4IOE5V6408
// P0 = Taster (low = gedrueckt), P5 = LNA an, P6 = Antennenschalter an, P7 = LoRa-Reset (high = laeuft)
constexpr uint8_t IOX_ADDR = 0x43;
constexpr uint8_t REG_CHIP_RESET = 0x01;
constexpr uint8_t REG_IO_DIR = 0x03;
constexpr uint8_t REG_OUT_SET = 0x05;
constexpr uint8_t REG_OUT_H_IM = 0x07;
constexpr uint8_t REG_IN_DEF_STA = 0x09;
constexpr uint8_t REG_PULL_EN = 0x0B;
constexpr uint8_t REG_PULL_SEL = 0x0D;
constexpr uint8_t REG_IN_STA = 0x0F;
constexpr uint8_t REG_INT_MASK = 0x11;
constexpr uint8_t REG_IRQ_STA = 0x13;

inline void ioxWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(IOX_ADDR);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

inline uint8_t ioxRead(uint8_t reg) {
  Wire.beginTransmission(IOX_ADDR);
  Wire.write(reg);
  Wire.endTransmission();
  Wire.requestFrom((int)IOX_ADDR, 1);
  return Wire.available() ? Wire.read() : 0xFF;
}

// Expander einrichten, SX1262 einmal sauber zuruecksetzen, Antennenschalter und LNA einschalten.
// Gibt false zurueck, wenn der Expander nicht antwortet.
inline bool begin() {
  pinMode(LORA_CS, OUTPUT);
  digitalWrite(LORA_CS, HIGH);
  pinMode(OLED_CS, OUTPUT);
  digitalWrite(OLED_CS, HIGH);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  Wire.beginTransmission(IOX_ADDR);
  if (Wire.endTransmission() != 0) return false;

  ioxWrite(REG_CHIP_RESET, 0xFF);
  delay(10);
  (void)ioxRead(REG_CHIP_RESET);
  delay(10);
  ioxWrite(REG_IO_DIR, 0b11100000);      // P5, P6, P7 Ausgaenge
  delay(10);
  ioxWrite(REG_OUT_H_IM, 0b00011100);    // P2..P4 hochohmig
  delay(10);
  ioxWrite(REG_PULL_SEL, 0b11100011);    // Pull-ups
  delay(10);
  ioxWrite(REG_PULL_EN, 0b11100011);
  delay(10);
  ioxWrite(REG_IN_DEF_STA, 0b00000011);
  delay(10);
  ioxWrite(REG_INT_MASK, 0b11111100);
  delay(10);
  ioxWrite(REG_OUT_SET, 0b00000000);     // SX1262 im Reset
  delay(10);
  ioxWrite(REG_OUT_SET, 0b11100000);     // Reset frei, Antennenschalter + LNA an
  delay(20);
  (void)ioxRead(REG_IRQ_STA);            // Interrupt-Flag loeschen
  return true;
}

inline bool buttonDown() {
  return (ioxRead(REG_IN_STA) & 0x01) == 0;
}

}  // namespace c6l
