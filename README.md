# Placa ESP32-C3 + SX1278 (LoRa 433 MHz) — Version2

Bring-up y validación de la revisión **Version2** de la placa.
Módulos: **ESP32-C3-WROOM-02-N4** (4 MB) + módulo **SX1278** de 433 MHz con conector u.FL.

**Estado: hardware validado.** 5 unidades fabricadas (3 + 2), todas funcionando.
Enlace LoRa bidireccional confirmado entre dos placas.

---

## Pinout

Verificado de dos formas independientes: extraído del netlist de KiCad
(`Version2.kicad_pcb`) y confirmado con firmware que corre en la placa.

| Señal | GPIO | Pad del módulo | Nota |
|---|---|---|---|
| LoRa SCK | **10** | 10 | |
| LoRa MISO | **6** | 5 | |
| LoRa MOSI | **7** | 6 | |
| LoRa NSS | **8** | 7 | strapping pin — ver ERRATA #4 |
| LoRa RESET | **5** | 4 | |
| LoRa DIO0 | **4** | 3 | conectado, permite usar interrupciones |
| UART TX | **21** | 12 | compartido con el LED — ver ERRATA #1 |
| UART RX | **20** | 11 | |
| Botón RESET | EN | 2 | |
| Botón BOOT | **0** | 18 | no es strapping pin — ver ERRATA #2 |
| Pull-up 10k (R10) | 2 | 16 | strapping, correcto |
| Pull-up 10k (R11) | 8 | 7 | strapping, correcto |

**GPIO libres:** 1, 3, 9, 18, 19
**DIO1–DIO5 del SX1278:** sin conectar

```cpp
#define PIN_SCK   10
#define PIN_MISO   6
#define PIN_MOSI   7
#define PIN_NSS    8
#define PIN_RST    5
#define PIN_DIO0   4
```

---

## Estado de validación

| Qué | Estado | Evidencia |
|---|---|---|
| Regulador 3.3 V (AMS1117) | OK | 3.3 V medidos, sin cortos |
| Arranque del ESP32-C3 | OK | mensaje de la ROM en UART |
| Flash 4 MB | OK | `flash-id` y sketches corriendo |
| UART bidireccional | OK | consola + flasheo exitoso |
| Modo programación | OK | `boot:0x5 (DOWNLOAD(USB/UART0/1))` |
| WiFi + estabilidad de fuente | OK | 6 redes, sin brownout en el pico de ~350 mA |
| SPI hacia el SX1278 | OK | `RegVersion = 0x12` estable |
| **Enlace LoRa bidireccional** | **OK** | PING/PONG entre 2 placas y test 14 del banco |
| **Test de producción completo** | **OK** | **14/14** en las unidades probadas |

Valores de referencia de placa sana: WiFi **-48 a -51 dBm** con 10-14 redes,
piso de ruido LoRa **-123 dBm**, frecuencia por defecto **434.0 MHz**.
Ver la tabla completa en [ERRATA.md](ERRATA.md).

### Todavía sin validar

- Alcance real (requiere antenas u.FL → SMA, aún no disponibles)
- Consumo en régimen y a batería
- Comportamiento térmico bajo carga sostenida

> **Nota sobre el -124 dBm:** la sensibilidad del SX1278 en SF7/BW125 es -123 dBm.
> El enlace funcionó **en el límite físico del chip**, sin antena y a 2 dBm, con
> ~8% de paquetes exitosos. Eso no es una falla: es el resultado esperado con los
> conectores u.FL al aire. Con antenas se espera -40 a -50 dBm a corta distancia.

---

## Cómo flashear

El programador usado es un **PL2303 sin DTR/RTS**, así que el modo descarga se
entra a mano.

### Cableado (TX y RX van cruzados)

```
PL2303              J2
------              --------------
GND     --------->  GND   (pin 1)
+5V     --------->  5V    (pin 2)
RXD     <---------  TX    (pin 3)
TXD     --------->  RX    (pin 4)
(nada)              RST   (pin 5)
(nada)              DTR   (pin 6)
```

### Configuración de Arduino IDE

| Opción | Valor |
|---|---|
| Board | ESP32C3 Dev Module |
| **USB CDC On Boot** | **Disabled** |
| Flash Mode | DIO |
| Flash Size | 4MB (32Mb) |
| Upload Speed | 921600 (probado y estable) |

### Secuencia

```
1. Mantener BOOT apretado
2. Apretar y soltar RESET
3. Soltar BOOT
4. Verificar en el monitor: boot:0x... (DOWNLOAD(USB/UART0/1))
5. Cerrar el Monitor Serie
6. Upload
7. Apretar RESET a mano (no hay RTS conectado)
```

**Indicador infalible:** el `boot:0x...` del mensaje de arranque.
`DOWNLOAD` = listo para flashear. `SPI_FAST_FLASH_BOOT` = no entraste.

---

## Sketches

| Carpeta | Qué prueba | Cuándo usarlo |
|---|---|---|
| `00_test_produccion/` | **14 pruebas automáticas con veredicto APROBADA / RECHAZADA** y línea CSV para planilla | **El de todos los días.** Es el banco de pruebas del lote. Ver la calibración de umbrales en ERRATA.md antes de correr una tanda |
| `04_test_lucas/` | Lee `RegVersion` del SX1278 por SPI | Primer test de una placa nueva si querés algo mínimo. 70 líneas, sin librerías |
| `01_smoke_test/` | CPU, flash, UART, LED, escaneo de WiFi | Validación general. El escaneo de WiFi es el que fuerza el pico de 350 mA y detecta problemas de fuente |
| `05_buscar_led/` | Versión corta del anterior | Cuando solo querés el estrés de fuente sin todo lo demás |
| `02_scan_sx1278/` | Encuentra los pines SPI por fuerza bruta | Si alguna vez hay una revisión nueva y no se conoce el pinout. Con el comando `v` verifica un pinout conocido en 2 segundos |
| `03_lora_pingpong/` | **Enlace de radio real entre 2 placas** | El único test que valida el amplificador, el cristal y el camino de antena |

> `04_test_lucas.ino` está **sin modificar**, tal como fue escrito originalmente.
> Se conserva así a propósito: es la referencia contra la que se comparó todo lo demás.

### Ping-pong: cómo se corre

El **mismo sketch** va en las dos placas. Las dos arrancan como RESPONDEDOR.
En la que tiene consola se manda `a` y pasa a EMISOR.

```
a  -> pasar a EMISOR        s  -> estadísticas
b  -> pasar a RESPONDEDOR   r  -> medir piso de ruido
t<n> -> potencia TX (2-17)  f<n> -> spreading factor (7-12)
d  -> cazar el pin DIO0
```

**Sin antena: 2 dBm y las placas a 5-10 cm.** No subir la potencia: el PA se
degrada transmitiendo contra un circuito abierto, y la degradación es silenciosa.

---

## Documentos

- **[ERRATA.md](ERRATA.md)** — los 7 defectos encontrados y qué cambiar en la v2
- **[00_GUIA_BRINGUP.md](00_GUIA_BRINGUP.md)** — guía completa de bring-up con mediciones
- **[PASOS_TEST_LUCAS.md](PASOS_TEST_LUCAS.md)** — procedimiento paso a paso con hitos

---

## Pendientes

- [ ] Comprar pigtails u.FL → SMA y antenas de 433 MHz (×5)
- [ ] Medir alcance real al aire libre
- [ ] Adaptador USB-TTL con DTR/RTS (CP2102 o CH340) para reset automático
- [ ] Regenerar los gerbers desde Version2 — ver ERRATA #7
- [ ] Aplicar los cambios de ERRATA.md a la v3
