/*
 * ============================================================================
 *  TEST DE PRODUCCION - Placa ESP32-C3-WROOM-02 + SX1278 (433 MHz)
 *  Revision de placa: Version2
 * ============================================================================
 *
 *  Corre 14 pruebas solo y da un veredicto: APROBADA o RECHAZADA.
 *  No tiene comandos. Flasheas, apretas RESET, y leer el resultado.
 *  Cada falla te dice que medir.
 *
 *  DURACION: ~25 segundos por placa.
 *
 * ----------------------------------------------------------------------------
 *  DOS MODOS, se eligen con el boton BOOT al arrancar
 * ----------------------------------------------------------------------------
 *
 *  MODO TEST (normal)
 *      Arrancar sin tocar nada y esperar 3 segundos. Corre las 14 pruebas.
 *
 *  MODO BALIZA (una sola placa, la "patron")
 *      Al arrancar hay una ventana de 3 segundos, avisada por el serial.
 *      Apretar BOOT, o escribir  b  y ENTER en el Monitor. Cualquiera sirve.
 *
 *      QUEDA GUARDADO EN FLASH. Podes desconectar el TTL y alimentarla aparte:
 *      va a arrancar como baliza sola cada vez, sin que le digas nada.
 *
 *      Para devolverla a modo test: apretar BOOT o escribir  t  en la ventana.
 *      (Tambien se borra flasheando con "Erase All Flash Before Upload".)
 *
 *      La placa queda contestando pings para siempre. Dejala encendida en el
 *      banco: es contra ella que se prueba el enlace de radio de las demas.
 *
 *      OJO: la baliza tiene que estar a 5-10 cm de la placa bajo prueba
 *      mientras no haya antenas. A 50 cm el enlace queda en el limite de
 *      sensibilidad y el test 14 falla al azar en placas buenas.
 *
 *      Sin baliza el test corre igual, pero la prueba 14 queda SALTEADA y el
 *      veredicto lo aclara. El enlace de RF es lo unico que una placa no puede
 *      verificar sola.
 *
 * ----------------------------------------------------------------------------
 *  FLUJO PARA UN LOTE
 * ----------------------------------------------------------------------------
 *      1. Preparar UNA placa patron en modo baliza. Se hace una sola vez
 *      2. Por cada placa: conectar TTL -> flashear -> RESET -> leer veredicto
 *      3. Copiar la linea que empieza con CSV, a la planilla
 *
 *  ANTENA: el test transmite 3 paquetes cortos a 2 dBm. Sin antena el riesgo
 *  al amplificador es bajo a esa potencia. Si tenes antenas, mejor ponerlas.
 *
 *  ARDUINO IDE: ESP32C3 Dev Module | USB CDC On Boot = Disabled | 115200
 *  LIBRERIA: "LoRa" de Sandeep Mistry
 * ============================================================================
 */

#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <Preferences.h>
#include "esp_system.h"

Preferences prefs;   // guarda el modo baliza en flash, sobrevive al corte de energia

// ------------------------------------------------------------------- pines
#define PIN_SCK      10
#define PIN_MISO      6
#define PIN_MOSI      7
#define PIN_NSS       8
#define PIN_RST       5
#define PIN_DIO0      4
#define PIN_BOOTBTN   0     // el boton BOOT. Sirve para elegir el modo baliza

// --------------------------------------------------------------- parametros
#define FRECUENCIA        433E6
#define SYNC_WORD         0x34
#define TX_POWER          2       // dBm. NO subir sin antena
#define MIN_REDES_WIFI    1
#define RSSI_ENLACE_MIN   -125    // por debajo de esto el enlace no sirve

// registros del SX127x
#define REG_FIFO_ADDR_PTR 0x0D
#define REG_FRF_MSB       0x06
#define REG_IRQ_FLAGS     0x12
#define REG_VERSION       0x42
#define IRQ_TX_DONE       0x08
#define SX127X_VERSION    0x12

// ------------------------------------------------------------------ estado
#define N_TESTS 14
struct Resultado {
  const char *nombre;
  int  estado;          // 0 = falla, 1 = ok, 2 = salteado
  char detalle[40];
  const char *ayuda;
};
Resultado R[N_TESTS];
int idx = 0;

char macStr[20] = "";
int  wifiRedes = 0, wifiMejorRssi = -200, enlaceRssi = -200, pisoRuido = 0;
uint32_t flashSize = 0;

volatile bool txDoneFlag = false;
void IRAM_ATTR onTxDoneISR() { txDoneFlag = true; }

// --------------------------------------------------------------- utilidades

void anota(const char *nombre, int estado, const char *detalle, const char *ayuda) {
  if (idx >= N_TESTS) return;
  R[idx].nombre = nombre;
  R[idx].estado = estado;
  snprintf(R[idx].detalle, sizeof(R[idx].detalle), "%s", detalle);
  R[idx].ayuda = ayuda;
  const char *m = (estado == 1) ? "OK" : (estado == 2) ? "SALTEADO" : "FALLA";
  Serial.printf("[%02d] %-32s %-22s %s\n", idx + 1, nombre, detalle, m);
  if (estado == 0 && ayuda) Serial.printf("     -> %s\n", ayuda);
  idx++;
}

// SPI crudo, antes de que la libreria tome el control
uint8_t leerReg(uint8_t addr) {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_NSS, LOW);  delayMicroseconds(2);
  SPI.transfer(addr & 0x7F);
  uint8_t v = SPI.transfer(0x00);
  digitalWrite(PIN_NSS, HIGH); SPI.endTransaction();
  delayMicroseconds(2);
  return v;
}

void escribirReg(uint8_t addr, uint8_t val) {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_NSS, LOW);  delayMicroseconds(2);
  SPI.transfer(addr | 0x80);
  SPI.transfer(val);
  digitalWrite(PIN_NSS, HIGH); SPI.endTransaction();
  delayMicroseconds(2);
}

// Espera hasta 3 s a que pase alguna de estas dos cosas:
//   - se aprieta el boton BOOT
//   - llega la letra indicada por el puerto serie
// Devuelve true si ocurrio alguna. Imprime puntos como cuenta regresiva.
bool esperarEntrada(char letra) {
  unsigned long t0 = millis();
  int puntos = 0;
  while (millis() - t0 < 3000) {
    if (digitalRead(PIN_BOOTBTN) == LOW) return true;
    if (Serial.available()) {
      char c = Serial.read();
      if (c == letra || c == (char)(letra - 32)) return true;   // acepta mayuscula
    }
    if ((int)((millis() - t0) / 500) > puntos) { puntos++; Serial.print('.'); }
    delay(10);
  }
  return false;
}

// =========================================================== MODO BALIZA

void modoBaliza() {
  Serial.println();
  Serial.println(F("############################################################"));
  Serial.println(F("#            M O D O   B A L I Z A                          #"));
  Serial.println(F("############################################################"));
  Serial.println();
  Serial.println(F("Esta placa queda contestando pings de las que se estan"));
  Serial.println(F("probando. Dejala encendida y no la toques."));
  Serial.println(F("Para volver al modo test: RESET sin apretar BOOT."));
  Serial.println();

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);
  LoRa.setPins(PIN_NSS, PIN_RST, PIN_DIO0);
  if (!LoRa.begin(FRECUENCIA)) {
    Serial.println(F("ERROR: el SX1278 de la BALIZA no arranca."));
    Serial.println(F("Usa otra placa como patron, esta no sirve."));
    while (true) delay(1000);
  }
  LoRa.setTxPower(TX_POWER, PA_OUTPUT_PA_BOOST_PIN);
  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setSyncWord(SYNC_WORD);
  LoRa.enableCrc();
  LoRa.receive();

  Serial.println(F(">>> BALIZA ACTIVA. Esperando placas...\n"));
  unsigned long n = 0;
  while (true) {
    if (LoRa.parsePacket() > 0) {
      String m = "";
      while (LoRa.available()) m += (char)LoRa.read();
      if (m.startsWith("TEST?")) {
        int rssi = LoRa.packetRssi();
        delay(25);
        LoRa.beginPacket();
        LoRa.print("BALIZA;");
        LoRa.print(rssi);
        LoRa.endPacket();
        LoRa.receive();
        n++;
        Serial.printf("  ping #%lu contestado  (la escuche a %d dBm)\n", n, rssi);
      }
    }
    delay(2);
  }
}

// ============================================================ LAS PRUEBAS

void t01_chip() {
  const char *modelo = ESP.getChipModel();
  char d[40];
  snprintf(d, sizeof(d), "%s rev %d", modelo, (int)ESP.getChipRevision());
  bool ok = (strstr(modelo, "ESP32-C3") != NULL);
  anota("Modelo de chip", ok, d,
        "El modulo soldado no es un ESP32-C3. Revisar que se pidio al ensamblador.");
}

void t02_flash() {
  flashSize = ESP.getFlashChipSize();
  char d[40];
  snprintf(d, sizeof(d), "%lu MB", (unsigned long)(flashSize / 1048576UL));
  anota("Flash de 4 MB", flashSize == 4194304UL, d,
        "No es un modulo N4 de 4 MB, o el Flash Size del IDE esta mal puesto.");
}

void t03_reset() {
  esp_reset_reason_t r = esp_reset_reason();
  const char *nom = "OTRO";
  int estado = 1;
  const char *ayuda = NULL;
  switch (r) {
    case ESP_RST_POWERON: nom = "POWERON"; break;
    case ESP_RST_EXT:     nom = "EXT (boton)"; break;
    case ESP_RST_SW:      nom = "SW"; break;
    case ESP_RST_BROWNOUT:
      nom = "BROWNOUT"; estado = 0;
      ayuda = "LA FUENTE SE CAYO. Revisar el regulador, C3 y C9, o usar una fuente de 1 A.";
      break;
    case ESP_RST_PANIC:
      nom = "PANIC"; estado = 0;
      ayuda = "El firmware crasheo en la corrida anterior. Repetir el test.";
      break;
    default: break;
  }
  anota("Motivo del ultimo reset", estado, nom, ayuda);
}

void t04_sx1278_presente() {
  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_RST, LOW);  delay(10);
  digitalWrite(PIN_RST, HIGH); delay(20);

  pinMode(PIN_NSS, OUTPUT);
  digitalWrite(PIN_NSS, HIGH);
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);

  uint8_t v = 0; bool ok = true;
  for (int i = 0; i < 5; i++) {
    v = leerReg(REG_VERSION);
    if (v != SX127X_VERSION) { ok = false; break; }
  }
  char d[40]; snprintf(d, sizeof(d), "RegVersion 0x%02X", v);
  const char *ayuda =
    (v == 0x00) ? "Lei 0x00: el MISO no llega, o el modulo LoRa no tiene 3.3 V. Medir su VCC."
  : (v == 0xFF) ? "Lei 0xFF: MISO flotando. Tipico de soldadura fria en el modulo LoRa."
                : "Revisar soldadura del modulo LoRa, su VCC (3.3 V) y su GND.";
  anota("SX1278 presente", ok, d, ayuda);
}

// Prueba de escritura: se escribe un patron y se relee.
//
// Se usa RegFrfMid (0x07) y NO RegFifoAddrPtr (0x0D). Motivo: en este punto el
// SX1278 todavia esta en modo FSK (arranca asi tras el reset, LoRa recien se
// activa en el test 08). En modo FSK el registro 0x0D es RegRxConfig, que tiene
// bits de disparo que siempre releen 0 -> el patron nunca coincide y el test
// falla aunque el MOSI este perfecto.
//
// RegFrfMid es R/W puro de 8 bits en los dos modos. Se guarda y se restaura,
// y ademas el reset del test 06 lo devuelve al valor de fabrica.
void t05_escritura_spi() {
  const uint8_t REG_FRF_MID = 0x07;
  uint8_t orig = leerReg(REG_FRF_MID);
  const uint8_t pat[] = {0x55, 0xAA, 0x3C};
  int okc = 0;
  for (int i = 0; i < 3; i++) {
    escribirReg(REG_FRF_MID, pat[i]);
    if (leerReg(REG_FRF_MID) == pat[i]) okc++;
  }
  escribirReg(REG_FRF_MID, orig);
  char d[40]; snprintf(d, sizeof(d), "%d/3 patrones", okc);
  anota("Escritura SPI (MOSI)", okc == 3, d,
        "El MOSI no llega bien al modulo LoRa. Revisar esa pista y su soldadura. "
        "Ojo: si el test 04 paso, el MOSI funciona -> sospechar del test antes que de la placa.");
}

void t06_pin_reset() {
  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_RST, LOW);  delay(15);
  uint8_t enReset = leerReg(REG_VERSION);
  digitalWrite(PIN_RST, HIGH); delay(20);
  uint8_t fuera = leerReg(REG_VERSION);
  bool ok = (enReset != SX127X_VERSION) && (fuera == SX127X_VERSION);
  char d[40]; snprintf(d, sizeof(d), "0x%02X -> 0x%02X", enReset, fuera);
  anota("Pin RESET del SX1278", ok, d,
        "El RESET no llega al modulo LoRa. Revisar la pista de GPIO5.");
}

void t07_frecuencia_default() {
  uint32_t frf = ((uint32_t)leerReg(0x06) << 16) |
                 ((uint32_t)leerReg(0x07) << 8)  |
                  (uint32_t)leerReg(0x08);
  double mhz = (double)frf * 32.0 / 524288.0;
  char d[40]; snprintf(d, sizeof(d), "%.1f MHz", mhz);
  bool ok = (mhz > 400.0 && mhz < 470.0);
  anota("Frecuencia por defecto", ok, d,
        "Tras el reset deberia dar ~434 MHz. Si no, el chip no resetea bien "
        "o no es un SX1278 de banda de 433.");
}

bool t08_modem() {
  SPI.end();
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);
  LoRa.setPins(PIN_NSS, PIN_RST, PIN_DIO0);
  bool ok = LoRa.begin(FRECUENCIA);
  if (ok) {
    LoRa.setTxPower(TX_POWER, PA_OUTPUT_PA_BOOST_PIN);
    LoRa.setSpreadingFactor(7);
    LoRa.setSignalBandwidth(125E3);
    LoRa.setCodingRate4(5);
    LoRa.setSyncWord(SYNC_WORD);
    LoRa.enableCrc();
  }
  anota("Modem LoRa a 433 MHz", ok, ok ? "configurado" : "no arranca",
        "LoRa.begin() fallo aunque el SPI responde. Modulo dañado o de otra banda.");
  return ok;
}

// Se transmite una sola vez y se miden DOS cosas independientes:
//
//   TX COMPLETO -> el bit TxDone del registro REG_IRQ_FLAGS, leido por SPI.
//                  No depende de DIO0 para nada.
//   DIO0 OK     -> la interrupcion en GPIO4 disparo la ISR.
//
// Detalle: si DIO0 funciona, la ISR de la propia libreria limpia REG_IRQ_FLAGS
// antes de que lleguemos a leerlo. Por eso "TX completo" acepta cualquiera de
// las dos evidencias: si vimos la interrupcion, la transmision termino si o si.
//
// No se usa LoRa.endPacket() bloqueante a proposito: si el modem no completa,
// esa llamada se cuelga para siempre y el test queda congelado sin decir nada.
void t09_t10_tx_y_dio0() {
  int txOk = 0, dioOk = 0;
  LoRa.onTxDone(onTxDoneISR);          // esto mapea DIO0 a TxDone y engancha la ISR

  for (int i = 0; i < 3; i++) {
    txDoneFlag = false;
    escribirReg(REG_IRQ_FLAGS, 0xFF);  // limpio banderas previas
    LoRa.beginPacket();
    LoRa.print("TESTPROD");
    LoRa.endPacket(true);              // async: no bloquea

    bool completo = false;
    unsigned long t0 = millis();
    while (millis() - t0 < 1500) {
      if (txDoneFlag) { completo = true; break; }
      if (leerReg(REG_IRQ_FLAGS) & IRQ_TX_DONE) { completo = true; break; }
      delay(5);
    }
    if (completo)   txOk++;
    if (txDoneFlag) dioOk++;

    escribirReg(REG_IRQ_FLAGS, 0xFF);
    delay(60);
  }
  LoRa.onTxDone(NULL);
  LoRa.receive();

  char d1[40]; snprintf(d1, sizeof(d1), "%d/3 paquetes", txOk);
  anota("Transmision (TxDone)", txOk == 3, d1,
        "El modem no termina de transmitir. Cristal del modulo LoRa o alimentacion.");

  char d2[40]; snprintf(d2, sizeof(d2), "%d/3 interrupciones", dioOk);
  anota("DIO0 en GPIO4", dioOk >= 2, d2,
        "DIO0 no genera interrupcion. Revisar la pista de GPIO4 al modulo LoRa.");
}

void t11_piso_ruido() {
  LoRa.receive();
  long acum = 0; int n = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < 600) { acum += LoRa.rssi(); n++; delay(5); }
  pisoRuido = n ? (int)(acum / n) : 0;
  char d[40]; snprintf(d, sizeof(d), "%d dBm", pisoRuido);
  bool ok = (pisoRuido < -85 && pisoRuido > -145);
  anota("Piso de ruido", ok, d,
        "Fuera de rango. Si esta muy alto hay interferencia o ruido de la fuente; "
        "si esta muy bajo el front-end de RF puede estar dañado.");
}

void t12_t13_wifi() {
  esp_reset_reason_t antes = esp_reset_reason();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(150);
  wifiRedes = WiFi.scanNetworks();
  for (int i = 0; i < wifiRedes; i++)
    if (WiFi.RSSI(i) > wifiMejorRssi) wifiMejorRssi = WiFi.RSSI(i);
  WiFi.scanDelete();
  WiFi.mode(WIFI_OFF);

  char d[40];
  if (wifiRedes > 0) snprintf(d, sizeof(d), "%d redes, max %d dBm", wifiRedes, wifiMejorRssi);
  else               snprintf(d, sizeof(d), "0 redes");
  anota("WiFi (RF + fuente)", wifiRedes >= MIN_REDES_WIFI, d,
        "Sin redes. Puede ser que no haya WiFi cerca, o que la antena PCB del "
        "modulo este tapada por cobre. Comparar contra otra placa del lote.");

  // si hubiera habido brownout, la placa se habria reiniciado y no llegariamos aca
  anota("Estabilidad de fuente", antes != ESP_RST_BROWNOUT, "sin brownout",
        "La placa se reinicio durante el pico de WiFi. Fuente insuficiente o desacoplo.");
}

void t14_enlace() {
  LoRa.receive();
  bool ok = false;
  int rssiRemoto = 0;

  for (int intento = 0; intento < 5 && !ok; intento++) {
    LoRa.beginPacket();
    LoRa.print("TEST?");
    LoRa.endPacket();
    LoRa.receive();

    unsigned long t0 = millis();
    while (millis() - t0 < 800) {
      if (LoRa.parsePacket() > 0) {
        String m = "";
        while (LoRa.available()) m += (char)LoRa.read();
        if (m.startsWith("BALIZA")) {
          enlaceRssi = LoRa.packetRssi();
          int c = m.indexOf(';');
          if (c > 0) rssiRemoto = m.substring(c + 1).toInt();
          ok = true;
          break;
        }
      }
      delay(2);
    }
  }

  if (!ok) {
    anota("Enlace de radio con la baliza", 2, "sin baliza", NULL);
    return;
  }
  char d[40]; snprintf(d, sizeof(d), "%d dBm (ella %d)", enlaceRssi, rssiRemoto);
  anota("Enlace de radio con la baliza", enlaceRssi > RSSI_ENLACE_MIN, d,
        "Senal por debajo del limite. Acercar las placas, o el amplificador de "
        "esta unidad esta degradado. Comparar contra otra placa del lote.");
}

// ============================================================== VEREDICTO

void veredicto() {
  int ok = 0, falla = 0, salt = 0;
  int primeraFalla = -1;
  for (int i = 0; i < idx; i++) {
    if (R[i].estado == 1) ok++;
    else if (R[i].estado == 2) salt++;
    else { falla++; if (primeraFalla < 0) primeraFalla = i; }
  }

  Serial.println();
  Serial.println(F("============================================================"));
  if (falla == 0 && salt == 0) {
    Serial.printf("   >>>  PLACA APROBADA   -   %d/%d  <<<\n", ok, idx);
  } else if (falla == 0) {
    Serial.printf("   >>>  APROBADA CON RESERVA   -   %d/%d  <<<\n", ok, idx);
    Serial.println(F("   El enlace de radio no se verifico: no habia baliza."));
    Serial.println(F("   Prepara una placa patron y repeti para aprobarla del todo."));
  } else {
    Serial.printf("   >>>  PLACA RECHAZADA   -   %d/%d  <<<\n", ok, idx);
    Serial.println();
    Serial.println(F("   Fallo:"));
    for (int i = 0; i < idx; i++)
      if (R[i].estado == 0) Serial.printf("     [%02d] %s  (%s)\n", i + 1, R[i].nombre, R[i].detalle);
    Serial.println();
    Serial.printf("   Empeza por la [%02d]: las fallas posteriores suelen ser\n", primeraFalla + 1);
    Serial.println(F("   consecuencia de la primera, no problemas independientes."));
  }
  Serial.println(F("============================================================"));

  // una linea para pegar en la planilla
  Serial.println();
  Serial.println(F("--- copiar a la planilla ---"));
  char rssiEnlaceTxt[12];
  if (enlaceRssi <= -200) snprintf(rssiEnlaceTxt, sizeof(rssiEnlaceTxt), "NA");
  else                    snprintf(rssiEnlaceTxt, sizeof(rssiEnlaceTxt), "%d", enlaceRssi);
  Serial.printf("CSV,%s,%s,%d,%d,%d,%d,%d,%s\n",
                macStr,
                (falla == 0 ? (salt == 0 ? "PASS" : "PASS_SIN_RF") : "FAIL"),
                ok, idx, wifiRedes, wifiMejorRssi, pisoRuido, rssiEnlaceTxt);
  Serial.println(F("(mac, resultado, ok, total, redes, rssi_wifi, piso_ruido, rssi_enlace)"));
  Serial.println();
}

// ================================================================= SETUP

void setup() {
  Serial.begin(115200);
  delay(600);

  // ---- ventana para elegir MODO BALIZA -------------------------------------
  // No hace falta atinarle a ningun instante: alcanza con apretar BOOT en
  // cualquier momento durante estos 3 segundos. Tampoco molesta mantenerlo
  // apretado desde antes del reset: cualquiera de las dos formas funciona.
  pinMode(PIN_BOOTBTN, INPUT_PULLUP);
  delay(20);
  prefs.begin("prodtest", false);
  bool guardadaComoBaliza = prefs.getBool("baliza", false);
  Serial.println();

  if (guardadaComoBaliza) {
    // Ya quedo marcada como baliza en una sesion anterior. Arranca sola.
    Serial.println(F("###  Esta placa esta GUARDADA COMO BALIZA."));
    Serial.println(F("###  Para devolverla a modo test: apreta BOOT, o escribi  t  y ENTER."));
    Serial.print(F("Tenes 3 segundos:  "));
    bool salir = esperarEntrada('t');
    Serial.println();
    if (salir) {
      prefs.putBool("baliza", false);
      prefs.end();
      Serial.println(F(">>> Marca de BALIZA borrada. Vuelve a ser una placa de test."));
    } else {
      prefs.end();
      modoBaliza();
      return;
    }
  } else {
    Serial.println(F("###  MODO BALIZA: apreta el boton BOOT,"));
    Serial.println(F("###  o escribi   b   y ENTER en el Monitor Serie."));
    Serial.print(F("Tenes 3 segundos. Si no, arranca el test:  "));
    bool activar = esperarEntrada('b');
    Serial.println();
    if (activar) {
      prefs.putBool("baliza", true);
      prefs.end();
      Serial.println(F(">>> Guardada como BALIZA en la flash."));
      Serial.println(F(">>> Ya podes desconectar el TTL y alimentarla aparte:"));
      Serial.println(F(">>> va a arrancar como baliza sola, sin que le digas nada."));
      modoBaliza();
      return;
    }
    prefs.end();
    Serial.println(F("(GPIO0 se mantuvo en ALTO: no se apreto BOOT, o ese boton no esta en GPIO0)"));
  }

  uint64_t m = ESP.getEfuseMac();
  snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X",
           (uint8_t)(m), (uint8_t)(m >> 8), (uint8_t)(m >> 16),
           (uint8_t)(m >> 24), (uint8_t)(m >> 32), (uint8_t)(m >> 40));

  Serial.println();
  Serial.println(F("============================================================"));
  Serial.println(F("  TEST DE PRODUCCION - ESP32-C3 + SX1278 - Version2"));
  Serial.println(F("============================================================"));
  Serial.printf("  MAC de la placa : %s\n", macStr);
  Serial.printf("  Firmware        : %s %s\n", __DATE__, __TIME__);
  Serial.println(F("------------------------------------------------------------"));
  Serial.println();

  t01_chip();
  t02_flash();
  t03_reset();
  t04_sx1278_presente();
  t05_escritura_spi();
  t06_pin_reset();
  t07_frecuencia_default();

  if (t08_modem()) {
    t09_t10_tx_y_dio0();
    t11_piso_ruido();
  } else {
    anota("Transmision (TxDone)", 0, "no se pudo", "El modem no arranco.");
    anota("DIO0 en GPIO4",        0, "no se pudo", "El modem no arranco.");
    anota("Piso de ruido",        0, "no se pudo", "El modem no arranco.");
  }

  t12_t13_wifi();

  if (idx < N_TESTS) {
    // el WiFi apaga el radio: hay que reconfigurarlo antes del enlace
    LoRa.setPins(PIN_NSS, PIN_RST, PIN_DIO0);
    if (LoRa.begin(FRECUENCIA)) {
      LoRa.setTxPower(TX_POWER, PA_OUTPUT_PA_BOOST_PIN);
      LoRa.setSpreadingFactor(7);
      LoRa.setSignalBandwidth(125E3);
      LoRa.setCodingRate4(5);
      LoRa.setSyncWord(SYNC_WORD);
      LoRa.enableCrc();
      t14_enlace();
    } else {
      anota("Enlace de radio con la baliza", 0, "modem caido",
            "El modem no volvio despues del WiFi. Posible problema de alimentacion.");
    }
  }

  veredicto();

  Serial.println(F("Para probar la placa siguiente: desconecta, conecta la otra,"));
  Serial.println(F("flashea y apreta RESET. No hace falta cambiar nada."));
}

void loop() { delay(1000); }