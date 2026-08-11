# ERRATA — Version2

Defectos encontrados durante el bring-up, con la evidencia que los sustenta y el
cambio concreto para la próxima revisión.

**Ninguno impide que la placa funcione.** Las unidades pasan el test de
producción **14/14**, incluido el enlace de radio verificado entre dos placas.
Esto es la lista de lo que hay que arreglar antes de fabricar de nuevo.

Severidad: **A** = arreglar sí o sí · **B** = arreglar si se puede · **C** = cosmético

| # | Defecto | Sev | Estado |
|---|---|---|---|
| 1 | LED sobre la línea TX del UART | **A** | Confirmado |
| 2 | GPIO9 sin conectar; botón BOOT a GPIO0 | **A** | Confirmado + anomalía abierta |
| 3 | Pad térmico (EP) sin conectar a masa | **B** | Confirmado, impacto menor al estimado |
| 4 | NSS sobre un strapping pin | **B** | Confirmado |
| 5 | Desacoplo insuficiente (C9) | **B** | Confirmado, sin impacto medible |
| 6 | Sin protección en la alimentación | **B** | Confirmado |
| 7 | Carpeta `production/` desactualizada | **A** | Operativo |

---

## #1 — El LED está sobre la línea TX del UART · **A**

**Qué pasa.** La red `BuildinLEd` quedó conectada al pad 12 del módulo, que es
**TXD (GPIO21)**. Cadena completa: `GPIO21 → R2 (100 Ω) → LED1 → GND`.

**Consecuencia.** El TX de un UART reposa en alto, así que el LED queda
**encendido en forma permanente** y titila cuando sale data serie.
**No se puede controlar por software**: manejarlo destruye la consola.
Además le chupa ~13 mA continuos a una línea de señal, lo cual es relevante
si el producto alguna vez va a batería.

**Evidencia.**
```
U1 pad 12 (TXD_12) -> net "BuildinLEd"
R2.1 -> BuildinLEd    R2.2 -> Net-(LED1-+)    LED1.1 -> Net-(LED1-+)    LED1.2 -> GND
```
El mismo error existía en la revisión anterior (`PrimerPrueba`):
`U3 pad 31 (IO21/TXD) -> BUILDINLED`. Se arrastra desde el primer prototipo.

**Causa raíz.** En el esquemático, la etiqueta global `BuildinLEd` quedó colocada
encima del cable de TX. KiCad no lo reporta como error: es una red válida con dos
nombres.

**Fix v3.** Mover la red a **GPIO3** (pad 15, libre). Verificar con "Resaltar red"
que la etiqueta no toque ningún otro cable.

> **Defecto de ensamblado adicional, confirmado:** en las placas recibidas el LED
> está montado con la polaridad invertida, así que ni siquiera enciende.
> **No repararlo:** aunque se corrija la orientación sigue sin ser controlable,
> y no justifica calentar la placa. Se resuelve en la v3 con el cambio de red.

---

## #2 — GPIO9 sin conectar; el botón BOOT va a GPIO0 · **A**

**Qué pasa.** El pin de arranque del ESP32-C3 es **GPIO9**, y está sin conectar.
El botón BOOT (SW1) y el transistor de auto-reset (Q3) van a **GPIO0**, que en el
C3 es un GPIO común sin ninguna función de strapping.

**Evidencia del netlist.**
```
U1 pad 8  (IO9_8)  -> unconnected-(U1-IO9-Pad8)
U1 pad 18 (IO0_18) -> net "GPIO0"   (SW1 + Q3 + R6)
```

### La anomalía, con evidencia nueva

Según los archivos ese botón **no debería** poder meter el chip en modo descarga.
En la práctica **funciona**, de forma repetible, en las 5 placas:
`boot:0x5 (DOWNLOAD(USB/UART0/1))` después de la secuencia BOOT+RESET.

Y apareció un dato más, en dirección opuesta: el firmware de test lee `GPIO0`
para elegir el modo baliza, y **apretando ese mismo botón el pin nunca baja**.
Hubo que agregar un comando por serie porque el botón no se detectaba.

Las dos observaciones juntas apuntan a lo mismo: **el botón se comporta como si
estuviera físicamente en GPIO9, no en GPIO0.** Eso explicaría que el modo
descarga funcione y que `digitalRead(0)` no lo vea.

> **Cuidado con esta conclusión.** La prueba del botón puede haber corrido sobre
> un firmware viejo, así que la evidencia está contaminada. No darla por cerrada.

**Cómo se resuelve, definitivo, 2 minutos.** Multímetro en continuidad, con el
botón SW1 apretado:

| Una punta | Otra punta | Si pita |
|---|---|---|
| pata de SW1 | **pad 8** del módulo (IO9) | El botón está en GPIO9. El netlist no refleja la placa |
| pata de SW1 | **pad 18** del módulo (IO0) | El netlist tiene razón y la anomalía sigue abierta |

**Por qué importa igual.** Lo que funciona sin que se sepa por qué es lo que
después deja de funcionar sin que se sepa por qué: otro lote de módulos, otra
revisión de silicio, otra temperatura.

**Fix v3.** Cablear SW1 y el colector de Q3 a **GPIO9** (pad 8) de forma explícita.
Renombrar la red `GPIO0` a **`BOOT_IO9`** — un nombre que miente cuesta horas.

---

## #3 — El pad térmico (EP) del ESP32 no está conectado a masa · **B**

> **Corregido respecto de la versión anterior de este documento.** Estaba
> clasificado como **A** por sospecha de que degradaba el RF. **Los datos no lo
> respaldan** y baja a **B**.

**Qué pasa.** El pad 19 (EP) del módulo, que es su pad de masa y disipación,
quedó sin red. El módulo se aterriza únicamente por el pad 9.

**Evidencia.**
```
U1 pad 19 (EP_19) -> unconnected-(U1-EP-Pad19)
```

**Impacto real: menor al estimado.** La hipótesis inicial era que explicaba un
WiFi flojo (-71 dBm en la primera medición). Las corridas posteriores dieron de
forma consistente **-48 a -51 dBm con 10 a 14 redes**, que son valores sanos.
El -71 fue distancia u orientación, no la placa. El piso de ruido del SX1278
también da normal: **-123 dBm**.

**Sigue siendo un defecto** de disipación y de retorno de masa, y hay que
arreglarlo. Pero no está limitando el rendimiento actual.

**Fix v3.** Conectar EP a GND con un array de vías térmicas (mínimo 4).

---

## #4 — NSS del SX1278 sobre un strapping pin · **B**

**Qué pasa.** `NSS` está en **GPIO8**, que debe estar en ALTO al soltar el reset o
el ESP32-C3 no arranca.

**Por qué funciona igual.** NSS reposa en alto (que es justo lo que GPIO8 necesita)
y R11 (10 kΩ) lo mantiene arriba antes de que el firmware tome el pin.

**Por qué es frágil.** No queda margen. Cualquier cosa que tire GPIO8 a masa
durante el reset —una punta de multímetro apoyada, un módulo LoRa con la entrada
dañada, flux conductivo— deja la placa sin arrancar, con un síntoma que parece un
ESP32 muerto y no lo es.

**Fix v3.** Mover NSS a **GPIO1** (pad 17, libre).

---

## #5 — Desacoplo insuficiente en el ESP32 · **B**

**Qué pasa.** `C9 = 10 nF` donde la nota de aplicación de Espressif pide **10 µF**.
Casi seguro un typo. El módulo queda con 100 nF + 10 nF y sin capacitor de masa.

**Impacto medido: ninguno.** El escaneo de WiFi (pico de ~350 mA) corrió completo
en todas las placas probadas, sin brownout ni resets. El bulk del regulador
(C3, 10 µF) alcanza a cubrirlo.

**Fix v3.** `C9 → 10 µF`, ubicado lo más cerca posible del pad 1 del módulo.

---

## #6 — Sin protección en la entrada de alimentación · **B**

**Qué pasa.** `P1` entra directo al AMS1117. No hay protección contra inversión de
polaridad ni contra sobretensión.

**Consecuencia.** Un cable invertido mata el regulador y probablemente lo que
venga después. En una bornera a tornillo que va a manipular gente distinta, y con
30 unidades en circulación, es cuestión de tiempo.

**Fix v3.** Schottky en serie (SS34 o similar) o un P-MOSFET para no perder los
0.3 V de caída, más un TVS a masa. Y marca de polaridad en la serigrafía.

---

## #7 — La carpeta `production/` es de la revisión vieja · **A (operativo)**

**Qué pasa.** `production/` contiene los gerbers y el netlist IPC de
**`PrimerPrueba`**, la revisión anterior, que usaba el footprint
**ESP32-C3-MINI-1 de 53 pads**. Las placas actuales llevan un
**WROOM-02 de 19 pads**: footprints físicamente incompatibles.

**Evidencia.** En `production/netlist.ipc` el ESP32 figura como `U3` con 53 pads,
y no existen R10 ni R11 (los pull-ups de strapping que sí tiene Version2).

**Consecuencia.** Si se manda esa carpeta a fabricar, llegan placas para un módulo
que no se tiene. Con un pedido de 30 unidades, es un error caro.

**Fix inmediato.** Borrar `production/` o regenerar los gerbers desde
`Version2.kicad_pcb` **antes de pedir cualquier cosa**. Ya está en `.gitignore`.

---

# Datos de referencia del lote

Valores de placas conocidas buenas. Sirven como línea de base: en un lote de 30,
**lo que importa es la dispersión, no el valor absoluto.** Una unidad que se sale
del rango tiene un problema aunque pase todos los umbrales.

| Medición | Rango observado | Qué significa salirse |
|---|---|---|
| WiFi, mejor RSSI | **-48 a -51 dBm** | 15 dB peor que el resto → antena PCB tapada o soldadura del módulo |
| WiFi, redes vistas | 10 a 14 | Muy pocas → mismo problema |
| Piso de ruido LoRa | **-123 dBm** | Muy alto → interferencia o ruido de fuente. Muy bajo → front-end dañado |
| Frecuencia por defecto | ~434.0 MHz | Otra cosa → módulo de otra banda o que no resetea |
| Rail de 3.3 V | 3.25 – 3.35 V | Regulador |
| Enlace con la baliza | ver abajo | — |

## Calibración del banco de pruebas

**Sin antenas, la distancia a la baliza domina el resultado.** Medido:

| Distancia | RSSI | Utilidad |
|---|---|---|
| ~50 cm | **-124 dBm** | Inservible. Está en el límite de sensibilidad del chip (-123 dBm en SF7/BW125): ~8% de paquetes pasan, y una placa buena falla al azar |
| 5 a 10 cm | ~-105 dBm | Usable |

**Procedimiento antes de correr un lote:**

1. Poner la baliza a 5-10 cm y dejarla fija ahí
2. Correr el test en varias placas ya validadas y anotar el RSSI del enlace
3. Poner `RSSI_ENLACE_MIN` unos **10 dB por debajo del peor** de esos valores
4. Verificar que el detalle diga **`1i`** (un solo intento). Si necesita más de
   uno, el enlace está al límite y el test no discrimina

Con el umbral pegado a la sensibilidad, el test mide suerte, no calidad.
Una placa con el amplificador degradado da 15 a 20 dB peor que una sana: **ese**
es el margen que hay que poder detectar.

> Cuando lleguen los pigtails u.FL → SMA y las antenas, repetir la calibración.
> Los números van a mejorar mucho y los umbrales hay que rehacerlos.

---

# Lista de cambios para la v3

## Obligatorios

- [ ] `BuildinLEd`: GPIO21 (TX) → **GPIO3**
- [ ] Botón BOOT y Q3: GPIO0 → **GPIO9**. Renombrar la red a `BOOT_IO9`
- [ ] Pad térmico EP → **GND** con vías térmicas
- [ ] Regenerar los gerbers desde Version2

## Recomendados

- [ ] NSS: GPIO8 → **GPIO1**
- [ ] `C9`: 10 nF → **10 µF**
- [ ] Schottky + TVS en `P1`, y marca de polaridad en la serigrafía
- [ ] Keep-out de la antena PCB: sin cobre en ninguna capa

## Mejoras

- [ ] **Conector USB-C en GPIO18/19.** El ESP32-C3 tiene USB-Serial-JTAG nativo y
      esos dos pines están libres. Elimina el adaptador TTL para siempre. Con 30
      unidades a flashear, esto solo ya paga el rediseño
- [ ] Puntos de test (pads de 1 mm) en 3V3, GND, EN, IO9 y el bus SPI
- [ ] Serigrafía que diga cuál botón es **BOOT** y cuál **RESET**
- [ ] Evaluar reemplazar el AMS1117 por un LDO de bajo Iq o un buck, si va a
      batería (el AMS1117 consume ~5 mA solo por existir)

## Pinout propuesto para la v3

| Señal | GPIO actual | GPIO propuesto |
|---|---|---|
| LoRa SCK | 10 | 10 |
| LoRa MISO | 6 | 6 |
| LoRa MOSI | 7 | 7 |
| LoRa NSS | **8** | **1** |
| LoRa RESET | 5 | 5 |
| LoRa DIO0 | 4 | 4 |
| LED | **21 (TX)** | **3** |
| Botón BOOT | **0** | **9** |
| USB D− / D+ | — | **18 / 19** |
