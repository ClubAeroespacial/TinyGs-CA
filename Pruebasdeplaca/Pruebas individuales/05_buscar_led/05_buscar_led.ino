/*
 * BUSCAR EL LED + ESTRES DE LA FUENTE
 * ESP32-C3 Dev Module | USB CDC On Boot = Disabled | Monitor a 115200
 *
 * 1) Parpadea GPIO 18 y despues GPIO 19. MIRA LA PLACA.
 * 2) Escanea WiFi (pico de ~350 mA): si no se resetea, la fuente aguanta.
 *
 * Si no prende en 18 ni 19, escribi  p1  p3  p0  p4  y ENTER.
 */

#include <WiFi.h>
#include "esp_system.h"

void blink(int pin, int n) {
  pinMode(pin, OUTPUT);
  for (int i = 0; i < n; i++) {
    digitalWrite(pin, HIGH); delay(250);
    digitalWrite(pin, LOW);  delay(250);
  }
  pinMode(pin, INPUT);          // lo dejo en alta impedancia
}

void buscarLed() {
  Serial.println();
  Serial.println("--- BUSCANDO EL LED: MIRA LA PLACA ---");
  Serial.println(">>> GPIO 18 ... 6 parpadeos");
  blink(18, 6);
  delay(1500);
  Serial.println(">>> GPIO 19 ... 6 parpadeos");
  blink(19, 6);
  Serial.println("En cual se prendio?  (repetir = 'l')");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== TEST LED + FUENTE ===");

  esp_reset_reason_t r = esp_reset_reason();
  Serial.print("Motivo del reset: ");
  Serial.print((int)r);
  if (r == ESP_RST_BROWNOUT) Serial.print("  <<< BROWNOUT: LA FUENTE SE CAYO");
  if (r == ESP_RST_POWERON)  Serial.print("  (encendido normal)");
  if (r == ESP_RST_EXT)      Serial.print("  (boton de reset)");
  Serial.println();

  uint64_t m = ESP.getEfuseMac();
  Serial.printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                (uint8_t)(m), (uint8_t)(m >> 8), (uint8_t)(m >> 16),
                (uint8_t)(m >> 24), (uint8_t)(m >> 32), (uint8_t)(m >> 40));

  buscarLed();

  Serial.println();
  Serial.println("--- ESTRES DE LA FUENTE: escaneo WiFi ---");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(150);
  int n = WiFi.scanNetworks();
  Serial.printf("Redes encontradas: %d\n", n);
  for (int i = 0; i < n && i < 8; i++)
    Serial.printf("   %-24s %d dBm\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i));
  WiFi.scanDelete();
  WiFi.mode(WIFI_OFF);

  Serial.println();
  Serial.println("Si llegaste hasta aca SIN que la placa se resetee, la fuente");
  Serial.println("aguanta el pico y el riesgo del desacoplo (C9) queda cerrado.");
  Serial.println();
  Serial.println("Comandos:  l = repetir busqueda del LED   p<n> = probar un GPIO");
}

void loop() {
  if (!Serial.available()) { delay(20); return; }
  String s = Serial.readStringUntil('\n');
  s.trim();
  if (s.length() == 0) return;

  if (s.charAt(0) == 'l') {
    buscarLed();
  } else if (s.charAt(0) == 'p') {
    int n = s.substring(1).toInt();
    if ((n >= 11 && n <= 17) || n == 20 || n == 21) {
      Serial.println("Ese pin no: 11-17 es la flash interna, 20/21 es el UART.");
    } else if (n < 0 || n > 21) {
      Serial.println("Validos: 0-10, 18, 19.");
    } else {
      Serial.printf("Parpadeando GPIO %d ... mira la placa\n", n);
      blink(n, 8);
    }
  }
}
