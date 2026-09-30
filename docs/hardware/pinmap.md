# Pin map — LilyGO T-Echo (nRF52840)

As of 2026-08-30. Compiled from four sources, **not yet confirmed on the device**.
The "confirmed" column is filled in by Phase 0 (Gate 0.1), each with sketch and date.

## Sources

| Key | Source | Nature | Licence |
|---|---|---|---|
| **V** | LilyGO `examples/Factory/utilities.h` | vendor firmware | — |
| **C** | `cfr34k/t-echo-lora-aprs` `config/pinout.h` | bare metal, from the schematic | MIT |
| **M** | Meshtastic `variants/nrf52840/t-echo/variant.h` | third-party implementation | LGPL-2.1 |
| **S** | LilyGO `schematic/T-Echo_Schematic.pdf` (Altium, 2021-06-23) | **vendor schematic** | — |

**V and C are identical in every pin they share.** M differs in three places (see below).

Arduino pin number = absolute nRF number (`g_ADigitalPinMap` is the identity):
`P0.xx -> xx`, `P1.yy -> 32+yy`. P0.00/P0.01 are reserved for the LFXO.

---

## 1. READ FIRST: there are two hardware revisions

LilyGO has a revision switch in `examples/GPS/utilities.h` and `examples/A7682/*/utilities.h`:

```c
#if defined(VERSION_1)
#define GreenLed_Pin  _PINNUM(0,13)
#define RedLed_Pin    _PINNUM(0,14)
#define BlueLed_Pin   _PINNUM(0,15)
#else
#define GreenLed_Pin  _PINNUM(1,1)
#define RedLed_Pin    _PINNUM(1,3)
#define BlueLed_Pin   _PINNUM(0,14)
#endif
```

On the current revision **P1.01 and P1.03 are LEDs** — but on `VERSION_1` hardware the
same pins are **ePaper MISO (P1.03)** and **LoRa DIO0 (P1.01)**. Driving them push-pull there
causes bus contention.

**This is not a theoretical risk.** Meshtastic PR #3051 moved the LEDs to P1.03/P1.01;
PR #3304 reverted it with the reasoning *"the revision pin macros are causing
boot-loop in a sub-set of T-Echos"*. A *subset* of the devices — so both revisions are in circulation.

> **Rule for Phase 0: P1.01 and P1.03 are not touched until the revision of this
> device has been determined.** The revision-safe status LED is **P0.14** — it is an LED in *both*
> revisions (only the colour differs) and cannot damage anything.

---

## 2. P0.13 is the enable of the 3.3 V regulator — never write LOW

| Source | P0.13 |
|---|---|
| **S** (schematic) | net **`PWR_EN`** -> via `Rw = 10K` to **pin 3 (EN) of U11 = AP2112K-3.3V**, output `VDD3V3` |
| **V** | `Power_Enable1_Pin` |
| **C** | `PIN_REG_EN` — *"enable pin of the 3.3V regulator"* |
| **M** | `LED_RED` (via `PIN_LED3`) <- **this is the VERSION_1 assignment** |

The schematic decides: P0.13 is a regulator enable.

**Why everything runs anyway:** the EN node has a **pull-up `R37 = 100K`** to the
LDO input rail; the AP2112 has only a 3 MOhm internal pull-down. With P0.13 not driven,
EN is about 0.97 * V_IN — **the regulator is on without any firmware involvement.** Meshtastic
does not switch it on "by accident"; it was never off.

**Why it is dangerous nonetheless:** `digitalWrite(P0.13, LOW)` divides down via 10K/100K:

| V_IN | EN voltage | AP2112 `V_IL(max)` = 0.4 V |
|---|---|---|
| 4.2 V (battery full) | 0.38 V | **regulator OFF** |
| 3.7 V (battery nominal) | 0.34 V | **regulator OFF** |
| 5.0 V (USB) | 0.45 V | grey zone |

And in the default assembly the nRF52840 itself hangs off this rail
(`VDD3V3 -L8- VDD_nRF -RJ (0R)- VDDH`). An `ledOn(LED_RED)` would therefore not be a "LoRa off"
situation but the **self-shutdown of the entire device**.

> **Rule: P0.13 is configured as `OUTPUT` and permanently driven `HIGH`. Never LOW,
> never through an LED API.** This is safe under both hypotheses: as a regulator enable, HIGH
> is "on"; as a VERSION_1 LED (common anode), HIGH is "off".

## 3. Power rails

| Net | Pin | Function | Source |
|---|---|---|---|
| `PWR_EN` | **P0.13** | enable AP2112K-3.3V (`VDD3V3`) | S, V, C |
| `PWR_ON` | **P0.12** | battery latch + peripheral switch (`VDD_POWR` via Q8) | S, V, C |

From the schematic, more precise than any firmware source:

- `VDD3V3 -L7 (FB0603 600R)- VDD_LORA` -> **the LoRa module hangs directly off the regulator**, without Q8.
- E-paper, GNSS, BME280 and LEDs hang off `VDD_POWR`, which is switched from `VDD3V3`
  through **Q8 (SI2301)**.
- P0.12 goes via D6 (1N4148) to a node with **R30 = 100K to GND** (the external pull-down
  mentioned by cfr34k) and D5 from `VBUS`, from there via R28 to the base of Q5, which
  pulls the gates of **Q6** (main switch BAT -> system) and Q8. P0.12 is therefore the
  **battery latch, OR-ed with USB VBUS** — more than just a "peripheral enable".

**Consequence for CLAUDE.md 3.1:** LoRa RX is possible in principle while e-paper, GNSS
and sensor are unpowered. That is the lever with which cfr34k reaches about 100 uA standby.

Open question: cfr34k contradicts itself. `pinout.h` comments P0.12 with *"Must be set
to enable LoRa, GPS, LEDs and Flash"*, but `periph_pwr.c` requests only the regulator for LoRa
(`PERIPH_PWR_FLAG_LORA: return MODULE_FLAG_3V3_REG;`). **Whether LoRa runs without `PWR_ON` is to be
measured in Phase 0** — it decides the power budget.

## 4. Pin table

Source legend: V=vendor header, C=cfr34k, M=Meshtastic, S=schematic.

### Power

| Signal | Pin | Arduino | Source | confirmed |
|---|---|---|---|---|
| `REG_EN` (3V3 regulator) | P0.13 | 13 | S,V,C | |
| `PWR_ON` (peripherals/latch) | P0.12 | 12 | S,V,C | |
| E-paper frontlight | P1.11 | 43 | V,C,M | |

### SX1262 (SPI0) — all sources identical

| Signal | Pin | Arduino | confirmed |
|---|---|---|---|
| CS / NSS | P0.24 | 24 | |
| SCK | P0.19 | 19 | |
| MOSI | P0.22 | 22 | |
| MISO | P0.23 | 23 | |
| RESET | P0.25 | 25 | |
| BUSY | P0.17 | 17 | |
| DIO1 | P0.20 | 20 | |
| DIO3 (TCXO 1.8 V) | P0.21 | 21 | |
| DIO2 | — | — | module-internal as RF switch, not an MCU pin |

### E-paper (SPI1) — GDEH0154D67 / SSD1681, 200x200

| Signal | Pin | Arduino | Source | confirmed |
|---|---|---|---|---|
| CS | P0.30 | 30 | all | |
| DC | P0.28 | 28 | all | |
| RST | P0.02 | 2 | all | |
| BUSY | P0.03 | 3 | all | |
| SCK | P0.31 | 31 | all | |
| MOSI | P0.29 | 29 | all | |
| **MISO** | **P1.06** | 38 | **V,C** | M has P1.07 here as a `FIXME` dummy |

### External flash (dedicated QSPI, no bus sharing)

| Signal | Pin | Arduino | confirmed |
|---|---|---|---|
| SCK | P1.14 | 46 | |
| CS | P1.15 | 47 | |
| IO0 / MOSI | P1.12 | 44 | |
| IO1 / MISO | P1.13 | 45 | |
| IO2 / WP | P0.07 | 7 | |
| IO3 / HOLD | P0.05 | 5 | |

### GNSS L76K (UART, 9600 Bd)

| Signal | Pin | Arduino | confirmed |
|---|---|---|---|
| **nRF RX** (from GPS) | **P1.09** | 41 | |
| **nRF TX** (to GPS) | **P1.08** | 40 | |
| Wakeup / standby | P1.02 | 34 | |
| Reset (active low) | P1.05 | 37 | |
| PPS | P1.04 | 36 | |

> Meshtastic's comments on `GPS_TX_PIN`/`GPS_RX_PIN` claim the opposite of its own
> defines. The defines are correct, confirmed by V (`SerialGPS.setPins(Gps_Rx_Pin, ...)`) and
> C (`uart_config.pselrxd = PIN_GPS_RX` with `PIN_GPS_RX = P1.09`).

### I2C (SDA P0.26 / SCL P0.27)

| Device | Address | Source | confirmed |
|---|---|---|---|
| BME280 | `0x77` | V, C | 0x76 if SDO=GND — the scan decides |
| PCF8563 RTC | `0x51` | M | INT on P0.16, open-drain |

### Controls and ADC

| Signal | Pin | Arduino | Note |
|---|---|---|---|
| User button | P1.10 | 42 | active low, pull-up |
| Touch TTP223 | P0.11 | 11 | **Active HIGH — decided on 2026-08-31, see below.** Idle level LOW, a touch drives HIGH. Electrode: front, upper ledge next to the LILYGO acrylic panel. |
| Button 2 | P0.18 | 18 | **= nRESET.** See section 5. |
| Battery ADC | P0.04 | 4 (A0) | AIN2, external divider **2:1** |

### 4.1 The touch pad — where it sits and why it is active HIGH

Both were open until 2026-08-31, and the earlier reading in this document was wrong.

**Where.** LilyGO's own pin map graphic (`image/T-ECHO.jpg` in the repo `Xinyuan-LilyGO/T-Echo`)
leads "Touch / P0.11" with a line to the **front side, upper ledge directly next to the
black acrylic logo panel**, above the display. Meshtastic and cfr34k's manual both
only say "top". There is no marking on the housing; the logo panel is the only
landmark.

The electrode is **not a copper pad on the board**. In the schematic, input `I`
of U6 is connected via `TP_LED` and C42 (10 pF) to `P3` — a single-pin part that is
recognisable on LilyGO's board photos as a bent spring contact. A spring usually couples to
the housing surface; **with the cover removed** only the spring itself remains as the electrode,
with correspondingly small reach. Not proven, but the obvious explanation if
a touch on the opened device triggers nothing.

**Why active HIGH.** In the schematic, **TOG (pin 6) and AHLB (pin 4) of U6 (TTP223-BA6)
are not connected**. The datasheet gives both an internal pull-low, and the
combination `TOG=0, AHLB=0` means *direct mode, CMOS active high output*: idle level LOW,
touch drives HIGH, momentary rather than latching, push-pull.

Four independent pieces of evidence for this: the schematic, the datasheet, LilyGO's own pin table
("High level active") and LilyGO's `Factory.ino`, which uses `INPUT_PULLDOWN` with `RISING`.

**And the measurement fits.** Sketch 09 reads P0.11 with the internal pull-up enabled
and still reads **LOW** (`touch.with_pullup = 0`): the output actively drives against the
13 kOhm. The chip is fitted, powered and healthy.

That Meshtastic (`BUTTON_TOUCH_ACTIVE_LOW true`) and cfr34k (`APP_BUTTON_ACTIVE_LOW`) treat it as
active low is therefore no counter-evidence: with a push-pull output, active-low code also
works, it just reports the press on **release**. Moreover, Meshtastic's define is read
nowhere in their `src/`.

**Where the earlier HIGH measurement came from is open.** Sketch 11 measured the idle level twice as
HIGH, with the same `pinMode(..., INPUT)` configuration as sketch 09. Against an
actively driven LOW output that is not possible. The contradiction is not resolved,
but it does not change the decision: schematic, datasheet and the pull-up cross-check all
point in the same direction.

**Three quirks of the chip that affect every test protocol:**

- **0.5 s stabilisation after power-on**, during which the sensor reports nothing. Keep your finger
  off during boot.
- **Self-calibration about every 4 s** without touch. A resting finger is
  calibrated away; so pause at least four seconds between attempts.
- **Cs = C42 = 10 pF**, i.e. a deliberately reduced sensitivity. Place your finger flat and
  hold for one to two seconds, do not tap.

**Known hardware bug that will still hit us:** on some T-Echos a
LoRa transmission triggers the touch. Meshtastic therefore locks the button during TX
(`nicheGraphics.h`, `ButtonThread.cpp`). As soon as `link/` really transmits, the same
lock belongs in `ui/` — otherwise the device flips the screen on every frame.

### LEDs (common anode on `VDD_POWR`, i.e. active low)

| Pin | Arduino | current revision | VERSION_1 | schematic net label |
|---|---|---|---|---|
| **P0.14** | 14 | LED (blue) | LED (red) | `P0.14_Red` |
| P1.03 | 35 | LED (red) | **ePaper MISO** | `P1.03_Green` |
| P1.01 | 33 | LED (green) | **LoRa DIO0** | `P1.01_Blue` |
| P0.15 | 15 | no function | LED (blue) | `P0.15_LR` -> U10 pin 6 (NC) |

**The colour assignment is disputed.** The schematic net labels (`P1.03_Green`, `P0.14_Red`,
`P1.01_Blue`) contradict LilyGO's own firmware header. The text block in the schematic is
moreover internally broken (it names P0.14 twice). The empirical test in Meshtastic PR #5326
("Pin 0.14 blinked blue ... green led successfully on Pin P1.01") supports the firmware header.
**Settle the colour in Gate 0.1 by looking** — the set of pins is fixed, only the colour is not.

**P0.15 is not unconnected**, as is often claimed: the net `P0.15_LR` runs to pin 6
of the SX1262 module, which is listed as NC there. In the A7682 variant it is `A7682_PWR`.
**Do not use, leave as `INPUT`.**

## 5. Button 2 / P0.18 — we do not touch it

`CLAUDE.md` 1.7 calls it "hard-wired to nRESET and is not available to the application".
The schematic confirms the net `REST`. Meshtastic nevertheless uses it as a GPIO via UICR
`PSELRESET`.

**We follow the spec.** Whoever reconfigures `PSELRESET` loses double-reset access to the
bootloader — without an SWD probe, our only lifeline. In this project **no
UICR register is written**: `PSELRESET`, `APPROTECT` and `REGOUT0` remain untouched
(`REGOUT0` has already been set correctly to 3.0 V by the bootloader).

## 6. What Phase 0 has to settle

| # | Question | Sketch |
|---|---|---|
| 1 | **Hardware revision** (section 1) — only then touch P1.01/P1.03 | `01_blink` via P0.14, then `06_epaper` |
| 2 | LED colour assignment (section 4) | `01_blink` |
| 3 | Does LoRa run with `REG_EN` without `PWR_ON`? (section 3) | `03_rails` |
| 4 | Battery divider permanently on VBAT or switched? | **half answered**: not on VBAT alone — 4807/4821 mV on the cable, see decision D13. Needs a run without USB |
| 5 | Touch polarity P0.11 | **answered: active HIGH.** Schematic, datasheet and measurement, see section 4.1 |
| 6 | JEDEC ID: `0xBA6015` (ZD25WQ16B) or `0xC22815` (MX25R1635F)? | `05_extflash` |
| 7 | BME280 on 0x76 or 0x77? | `02_i2c_scan` |
| 8 | TCXO fitted? (`begin()` with 1.8 V vs. 0.0) | `09_radio_probe` |
| 9 | RTC INT: external pull-up present? | `07_rtc` |
| 10 | Does the RTC have its own backup supply? | `07_rtc` |
