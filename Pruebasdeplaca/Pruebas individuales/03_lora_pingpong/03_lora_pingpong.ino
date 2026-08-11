/*
 * ============================================================================
 *  TEST 3 - ENLACE LoRa REAL ENTRE PLACAS (ping / pong)
 *  ESP32-C3-WROOM-02 + SX1278 (Ra-02) @ 433 MHz
 * ============================================================================
 *
 *  >>>>>>>>>>>>>>>>>>>>>>>  LEER ANTES DE ENCHUFAR  <<<<<<<<<<<<<<<<<<<<<<<<<
 *
 *  NUNCA TRANSMITAS SIN ANTENA. La potencia reflejada puede danar el
 *  amplificador del SX1278. Antes de correr esto, cada placa que vaya a
 *  transmitir necesita algo en el pad ANT:
 *
 *      - un cable rigido de 16.4 cm  (lambda/4 a 433 MHz), o
 *      - un pigtail IPEX -> SMA con antena de 433 MHz
 *
 *  Arranca con potencia baja (2 dBm). Con las placas a 50 cm eso sobra.
 *  Subir la potencia con las placas pegadas satura el receptor y te da
 *  peores resultados, no mejores.
 *
 *  >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
 *
 *  LIBRERIA NECESARIA:
 *    Arduino IDE -> Herramientas -> Administrar Bibliotecas -> buscar "LoRa"
 *    -> instalar "LoRa" de Sandeep Mistry
 *
 *  COMO SE USA (con un solo programador TTL, que es tu caso):
 *    1. Flashea este MISMO sketch en las 3 placas.
 *    2. Todas arrancan como RESPONDEDOR: escuchan y contestan solas,
 *       sin necesidad de serial. El LED parpadea con cada paquete recibido.
 *    3. Deja el TTL conectado a UNA sola placa y manda 'a' por serial:
 *       esa pasa a EMISOR y empieza a mandar PING.
 *    4. Mira las estadisticas en el monitor.
 *
 *  ARDUINO IDE: Board = ESP32C3 Dev Module | USB CDC On Boot = Disabled | 115200
 * ============================================================================
 */

#include <SPI.h>
#include <LoRa.h>

// ############################################################################
// #  PINOUT REAL - extraido del codigo de Lucas que ya anda en la placa      #
// ############################################################################
#define PIN_SCK     10
#define PIN_MISO    6
#define PIN_MOSI    7
#define PIN_NSS     8      // OJO: GPIO8 es strapping pin. Ver nota abajo.
#define PIN_RST     5
#define PIN_DIO0    4      // CONFIRMADO en el netlist de KiCad (SX1.DIO0 -> U1 IO4)

// EL LED NO SE PUEDE USAR. Esta cableado a GPIO21, que es el TX del UART.
// Como el TX reposa en alto, el LED esta PRENDIDO FIJO siempre y titila cuando
// sale data serie. No hay forma de controlarlo sin romper la consola.
#define PIN_LED     -1     // dejalo en -1. No lo cambies.
// ############################################################################
//
// NOTA SOBRE GPIO8 = NSS
// GPIO8 es un strapping pin del ESP32-C3: tiene que estar en ALTO al soltar el
// reset o el chip no arranca. Funciona porque:
//   - NSS reposa en alto (que es justo lo que GPIO8 necesita)
//   - R11 (10k a 3V3) lo mantiene arriba antes de que el ESP32 lo maneje
// Pero es fragil: cualquier cosa que tire GPIO8 a masa durante el reset deja la
// placa sin arrancar. GPIO4 y GPIO9 estan libres. Para la v2, move NSS a GPIO4.
//
// ############################################################################

#define BAUD          115200
#define FRECUENCIA    433E6      // SX1278 = banda de 433 MHz
#define SYNC_WORD     0x34       // cualquier valor != 0x34 default; que sea igual en las 3
#define TX_POWER_INI  2          // dBm. Rango PA_BOOST: 2..17
#define PERIODO_PING  1500       // ms entre PINGs
#define TIMEOUT_PONG  1200       // ms de espera de respuesta

enum Rol { RESPONDEDOR, EMISOR };
Rol rol = RESPONDEDOR;

uint16_t miId = 0;
uint32_t seq = 0;
int txPower = TX_POWER_INI;
int sf = 7;

// estadisticas (solo utiles en modo EMISOR)
uint32_t enviados = 0, contestados = 0;
long rssiAcum = 0; int rssiN = 0;
int rssiMejor = -200, rssiPeor = 0;

unsigned long tUltimoPing = 0;
bool esperandoPong = false;
unsigned long tEnvio = 0;

// ----------------------------------------------------------------- utilidades

void blink(int n, int ms) {
  if (PIN_LED < 0) return;
  pinMode(PIN_LED, OUTPUT);
  for (int i = 0; i < n; i++) {
    digitalWrite(PIN_LED, HIGH); delay(ms);
    digitalWrite(PIN_LED, LOW);  delay(ms);
  }
}

void enviar(const String &msg) {
  LoRa.beginPacket();
  LoRa.print(msg);
  LoRa.endPacket();          // bloqueante: vuelve cuando termino el TX
  LoRa.receive();            // vuelvo a modo escucha
}

String campo(const String &s, int n) {
  int ini = 0, cnt = 0;
  for (int i = 0; i <= (int)s.length(); i++) {
    if (i == (int)s.length() || s.charAt(i) == ';') {
      if (cnt == n) return s.substring(ini, i);
      cnt++;
      ini = i + 1;
    }
  }
  return "";
}

// ------------------------------------------------------- cazador de DIO0
//
// DIO0 no aparece en el codigo de Lucas, asi que no sabemos en que pin esta.
// Este cazador es 100% SEGURO: pone los pines candidatos como ENTRADA y solo
// los lee. Nunca maneja nada, asi que no puede pelearse con una salida del
// SX1278 ni con nada mas.
//
// Como funciona: DIO0 se pone en ALTO cuando el radio termina de recibir un
// paquete (RxDone) y vuelve a BAJO cuando parsePacket() limpia las banderas.
// Muestreamos justo antes de parsePacket() en cada vuelta del loop, asi que
// atrapamos ese pulso.

const int DIO_CAND[] = {0, 1, 2, 3, 4, 9, 18, 19};   // los que quedan libres
const int N_DIO = sizeof(DIO_CAND) / sizeof(DIO_CAND[0]);

bool cazandoDio0 = false;
uint16_t dioVistoAlto = 0;    // bit por candidato
uint32_t dioMuestras = 0;

void iniciarCazaDio0() {
  for (int i = 0; i < N_DIO; i++) pinMode(DIO_CAND[i], INPUT);
  dioVistoAlto = 0;
  dioMuestras = 0;
  cazandoDio0 = true;
  Serial.println();
  Serial.println(F("CAZA DE DIO0 ACTIVADA (solo lectura, riesgo cero)."));
  Serial.println(F("Necesita trafico: pone esta placa o la otra a mandar PING."));
  Serial.println(F("Despues de unos 20 paquetes, manda 'd' otra vez para ver el resultado."));
}

inline void muestrearDio() {
  if (!cazandoDio0) return;
  for (int i = 0; i < N_DIO; i++) {
    if (digitalRead(DIO_CAND[i]) == HIGH) dioVistoAlto |= (1 << i);
  }
  dioMuestras++;
}

void reporteDio0() {
  Serial.println();
  Serial.println(F("=============== RESULTADO CAZA DE DIO0 ==============="));
  Serial.printf("  Muestras tomadas: %lu\n", (unsigned long)dioMuestras);
  Serial.println();
  int candidatos = 0;
  for (int i = 0; i < N_DIO; i++) {
    bool alto = dioVistoAlto & (1 << i);
    Serial.printf("  GPIO %-2d  ->  %s\n", DIO_CAND[i],
                  alto ? "estuvo en ALTO   <<< candidato" : "siempre en bajo");
    if (alto) candidatos++;
  }
  Serial.println();
  if (candidatos == 0) {
    Serial.println(F("  Ningun pin se movio. O DIO0 no esta conectado en esta placa,"));
    Serial.println(F("  o no hubo suficiente trafico. Genera mas paquetes y repeti."));
  } else if (candidatos == 1) {
    Serial.println(F("  Un solo candidato: ese es DIO0 con mucha probabilidad."));
    Serial.println(F("  Cargalo en PIN_DIO0 arriba y confirmalo contra el esquematico."));
  } else {
    Serial.println(F("  Varios pines en alto. Algunos tienen pull-up (GPIO2 con R10,"));
    Serial.println(F("  GPIO9 interno, GPIO0 con R6): esos son falsos positivos, siempre"));
    Serial.println(F("  estan altos. El que te interesa es uno que NO tenga pull-up."));
    Serial.println(F("  La forma definitiva de saberlo sigue siendo el esquematico."));
  }
  Serial.println(F("======================================================"));
}

// ---------------------------------------------------------------------- ayuda

void ayuda() {
  Serial.println();
  Serial.println(F("------------------------- COMANDOS -------------------------"));
  Serial.println(F("  a     -> pasar a EMISOR (manda PING cada 1.5 s)"));
  Serial.println(F("  b     -> pasar a RESPONDEDOR (escucha y contesta)"));
  Serial.println(F("  s     -> mostrar estadisticas"));
  Serial.println(F("  z     -> borrar estadisticas"));
  Serial.println(F("  t<n>  -> potencia TX en dBm, 2 a 17.  ej: t2   t17"));
  Serial.println(F("  f<n>  -> spreading factor, 7 a 12.    ej: f7   f12"));
  Serial.println(F("  r     -> medir piso de ruido durante 3 s"));
  Serial.println(F("  d     -> cazar el pin DIO0 (1a vez arranca, 2a vez muestra)"));
  Serial.println(F("  ?     -> esta ayuda"));
  Serial.println(F("------------------------------------------------------------"));
}

void estadisticas() {
  Serial.println();
  Serial.println(F("=================== ESTADISTICAS ==================="));
  Serial.printf("  Mi ID .................. %04X\n", miId);
  Serial.printf("  Rol .................... %s\n", rol == EMISOR ? "EMISOR" : "RESPONDEDOR");
  Serial.printf("  Frecuencia ............. %.1f MHz\n", FRECUENCIA / 1e6);
  Serial.printf("  Potencia TX ............ %d dBm\n", txPower);
  Serial.printf("  Spreading factor ....... SF%d\n", sf);
  Serial.println();
  Serial.printf("  PING enviados .......... %lu\n", (unsigned long)enviados);
  Serial.printf("  PONG recibidos ......... %lu\n", (unsigned long)contestados);
  if (enviados > 0) {
    float perdida = 100.0 * (enviados - contestados) / (float)enviados;
    Serial.printf("  Perdida de paquetes .... %.1f %%\n", perdida);
    if (perdida < 2)       Serial.println(F("     -> enlace EXCELENTE"));
    else if (perdida < 10) Serial.println(F("     -> enlace bueno"));
    else if (perdida < 30) Serial.println(F("     -> enlace flojo: mira antena y potencia"));
    else                   Serial.println(F("     -> enlace MALO: revisa antenas y distancia"));
  }
  if (rssiN > 0) {
    Serial.println();
    Serial.printf("  RSSI promedio .......... %ld dBm\n", rssiAcum / rssiN);
    Serial.printf("  RSSI mejor / peor ...... %d / %d dBm\n", rssiMejor, rssiPeor);
    Serial.println();
    Serial.println(F("  Referencia de RSSI:"));
    Serial.println(F("    -30 a -60  placas muy cerca, normal en el banco"));
    Serial.println(F("    -60 a -90  enlace sano"));
    Serial.println(F("    -90 a -110 al limite"));
    Serial.println(F("    peor a -120 no vas a recibir nada"));
  }
  Serial.println(F("===================================================="));
}

void pisoDeRuido() {
  Serial.println();
  Serial.println(F("Midiendo piso de ruido 3 s (sin transmitir)..."));
  LoRa.receive();
  int minR = 0, maxR = -200; long acum = 0; int n = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < 3000) {
    int r = LoRa.rssi();
    if (r < minR) minR = r;
    if (r > maxR) maxR = r;
    acum += r; n++;
    delay(10);
  }
  Serial.printf("  Piso de ruido: min %d / prom %ld / max %d dBm  (%d muestras)\n",
                minR, n ? acum / n : 0, maxR, n);
  Serial.println(F("  Tipico: entre -110 y -125 dBm. Si esta mucho mas arriba,"));
  Serial.println(F("  tenes interferencia o ruido de la propia fuente switching."));
}

// ---------------------------------------------------------------------- setup

void setup() {
  Serial.begin(BAUD);
  delay(400);

  Serial.println();
  Serial.println();
  Serial.println(F("############################################################"));
  Serial.println(F("#   TEST 3 - ENLACE LoRa (ping / pong)                      #"));
  Serial.println(F("############################################################"));
  Serial.println();
  Serial.println(F("  !! SIN ANTENA NO TRANSMITAS !!"));
  Serial.println();

  uint64_t mac = ESP.getEfuseMac();
  miId = (uint16_t)(mac & 0xFFFF);
  Serial.printf("Mi ID (de la MAC): %04X\n", miId);

  Serial.printf("Pines: SCK=%d MISO=%d MOSI=%d NSS=%d RST=%d DIO0=%d\n",
                PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS, PIN_RST, PIN_DIO0);

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);
  LoRa.setPins(PIN_NSS, PIN_RST, PIN_DIO0);

  if (!LoRa.begin(FRECUENCIA)) {
    Serial.println();
    Serial.println(F("############################################################"));
    Serial.println(F("#  ERROR: el SX1278 no arranca                              #"));
    Serial.println(F("############################################################"));
    Serial.println(F("Los pines de arriba estan mal, o el modulo no tiene 3.3V."));
    Serial.println(F("Volve al TEST 2 y corre el escaneo."));
    while (true) { blink(1, 100); delay(700); }
  }

  LoRa.setTxPower(txPower, PA_OUTPUT_PA_BOOST_PIN);   // el Ra-02 usa PA_BOOST
  LoRa.setSpreadingFactor(sf);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setPreambleLength(8);
  LoRa.setSyncWord(SYNC_WORD);
  LoRa.enableCrc();
  LoRa.receive();

  Serial.println();
  Serial.println(F(">>> SX1278 INICIALIZADO OK <<<"));
  Serial.printf("    %.1f MHz | SF%d | BW 125 kHz | CR 4/5 | %d dBm | sync 0x%02X\n",
                FRECUENCIA / 1e6, sf, txPower, SYNC_WORD);
  Serial.println();
  Serial.println(F("Arranco como RESPONDEDOR (escucho y contesto)."));
  Serial.println(F("Manda 'a' para que ESTA placa sea la que manda PING."));
  ayuda();

  blink(3, 120);
  Serial.print(F("\n> "));
}

// -------------------------------------------------------------- recepcion

void procesarPaquete(int tam) {
  String msg = "";
  while (LoRa.available()) msg += (char)LoRa.read();

  int rssi = LoRa.packetRssi();
  float snr = LoRa.packetSnr();
  long ferr  = LoRa.packetFrequencyError();

  String tipo = campo(msg, 0);
  String src  = campo(msg, 1);
  String nseq = campo(msg, 2);

  blink(1, 30);

  if (tipo == "PING" && rol == RESPONDEDOR) {
    Serial.printf("[RX] PING de %s  seq=%s  RSSI=%d dBm  SNR=%.1f dB  dErr=%ld Hz\n",
                  src.c_str(), nseq.c_str(), rssi, snr, ferr);
    delay(30);   // pequena espera para que el otro alcance a pasar a RX
    String r = "PONG;" + String(miId, HEX) + ";" + nseq + ";" +
               String(rssi) + ";" + String(snr, 1);
    enviar(r);
    Serial.printf("[TX] PONG a %s  seq=%s\n", src.c_str(), nseq.c_str());
  }
  else if (tipo == "PONG" && rol == EMISOR) {
    contestados++;
    esperandoPong = false;
    unsigned long rtt = millis() - tEnvio;

    rssiAcum += rssi; rssiN++;
    if (rssi > rssiMejor) rssiMejor = rssi;
    if (rssi < rssiPeor)  rssiPeor  = rssi;

    Serial.printf("  <- PONG de %s  seq=%s | ida: RSSI %s dBm SNR %s dB | vuelta: RSSI %d dBm SNR %.1f dB | RTT %lu ms\n",
                  src.c_str(), nseq.c_str(),
                  campo(msg, 3).c_str(), campo(msg, 4).c_str(),
                  rssi, snr, rtt);

    if (enviados % 10 == 0) {
      Serial.printf("     [%lu/%lu recibidos, %.0f%% de perdida]\n",
                    (unsigned long)contestados, (unsigned long)enviados,
                    100.0 * (enviados - contestados) / (float)enviados);
    }
  }
  else if (tipo == "PING" && rol == EMISOR) {
    Serial.printf("[RX] PING de otro emisor (%s). Hay 2 placas en modo 'a'.\n", src.c_str());
  }
  else {
    Serial.printf("[RX] paquete raro (%d bytes): %s\n", tam, msg.c_str());
  }
}

// ------------------------------------------------------------------- comandos

void comandos() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd.length() == 0) return;

  char c = cmd.charAt(0);

  if (c == 'a') {
    rol = EMISOR;
    Serial.println(F("\n>>> Ahora soy EMISOR. Empiezo a mandar PING."));
    Serial.println(F(">>> ASEGURATE DE TENER ANTENA CONECTADA."));
    tUltimoPing = 0;
  }
  else if (c == 'b') {
    rol = RESPONDEDOR;
    LoRa.receive();
    Serial.println(F("\n>>> Ahora soy RESPONDEDOR. Escucho y contesto."));
  }
  else if (c == 's') estadisticas();
  else if (c == 'z') {
    enviados = contestados = 0; rssiAcum = 0; rssiN = 0;
    rssiMejor = -200; rssiPeor = 0;
    Serial.println(F("Estadisticas borradas."));
  }
  else if (c == 't') {
    int n = cmd.substring(1).toInt();
    if (n < 2 || n > 17) Serial.println(F("Potencia valida con PA_BOOST: 2 a 17 dBm."));
    else {
      txPower = n;
      LoRa.setTxPower(txPower, PA_OUTPUT_PA_BOOST_PIN);
      Serial.printf("Potencia TX = %d dBm\n", txPower);
      if (n > 10) Serial.println(F("OJO: con las placas a menos de 1 m esto satura el RX."));
    }
  }
  else if (c == 'f') {
    int n = cmd.substring(1).toInt();
    if (n < 7 || n > 12) Serial.println(F("SF valido: 7 a 12. SF alto = mas alcance, mas lento."));
    else {
      sf = n;
      LoRa.setSpreadingFactor(sf);
      LoRa.receive();
      Serial.printf("SF = %d  (acordate de ponerlo IGUAL en las otras placas)\n", sf);
    }
  }
  else if (c == 'r') pisoDeRuido();
  else if (c == 'd') {
    if (!cazandoDio0) iniciarCazaDio0();
    else              reporteDio0();
  }
  else ayuda();

  Serial.print(F("\n> "));
}

// ----------------------------------------------------------------------- loop

void loop() {
  comandos();

  muestrearDio();                  // antes de parsePacket: ahi vive el pulso
  int tam = LoRa.parsePacket();
  if (tam > 0) procesarPaquete(tam);

  if (rol == EMISOR && millis() - tUltimoPing >= PERIODO_PING) {
    tUltimoPing = millis();

    if (esperandoPong) {
      Serial.printf("  !! seq=%lu SIN RESPUESTA (timeout de %d ms)\n",
                    (unsigned long)seq, TIMEOUT_PONG);
    }

    seq++;
    enviados++;
    String m = "PING;" + String(miId, HEX) + ";" + String(seq);
    Serial.printf("-> PING seq=%lu ...\n", (unsigned long)seq);
    enviar(m);
    tEnvio = millis();
    esperandoPong = true;
  }

  delay(2);
}
