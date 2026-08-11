#include <SPI.h>

// Pines ESP32-C3 -> SX1278
#define LORA_RST  5
#define LORA_MISO 6
#define LORA_MOSI 7
#define LORA_NSS  8
#define LORA_SCK  10

void writeRegister(uint8_t address, uint8_t value) {
  digitalWrite(LORA_NSS, LOW);

  SPI.transfer(address | 0x80);  // Bit 7 = escritura
  SPI.transfer(value);

  digitalWrite(LORA_NSS, HIGH);
}

uint8_t readRegister(uint8_t address) {
  digitalWrite(LORA_NSS, LOW);

  SPI.transfer(address & 0x7F);  // Bit 7 = lectura
  uint8_t value = SPI.transfer(0x00);

  digitalWrite(LORA_NSS, HIGH);

  return value;
}

void resetSX1278() {
  pinMode(LORA_RST, OUTPUT);

  digitalWrite(LORA_RST, LOW);
  delay(10);

  digitalWrite(LORA_RST, HIGH);
  delay(10);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("=== TEST SX1278 ===");

  pinMode(LORA_NSS, OUTPUT);
  digitalWrite(LORA_NSS, HIGH);

  // SPI: SCK, MISO, MOSI, SS
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);

  resetSX1278();

  uint8_t version = readRegister(0x42);

  Serial.print("RegVersion = 0x");

  if (version < 0x10)
    Serial.print("0");

  Serial.println(version, HEX);

  if (version == 0x12) {
    Serial.println("SX1278 DETECTADO!");
  }
  else {
    Serial.println("No se detecto un SX1278.");
    Serial.println("Revisa alimentacion, SPI y NSS.");
  }
}

void loop() {
  delay(1000);

  uint8_t version = readRegister(0x42);

  Serial.print("Version: 0x");
  Serial.println(version, HEX);
}
