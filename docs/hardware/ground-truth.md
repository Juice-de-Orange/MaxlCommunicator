# Hardware Ground Truth

Measured directly on the connected device and on the host, not taken from datasheets.
Collected on **2026-08-30**, before the first line of firmware was written.

Tool: `tools/uf2_inspect.py` (reads the bootloader's `CURRENT.UF2` and reconstructs
the complete flash content from it).

---

## 1. Device identity

From `E:\INFO_UF2.TXT` (the UF2 bootloader exposes it as a file):

```
UF2 Bootloader 0.6.1-2-g1224915 lib/nrfx (v2.0.0) lib/tinyusb (0.10.1-293-gaf8e5a90)
                                lib/uf2 (remotes/origin/configupdate-9-gadbb8c7)
Model: LilyGo T-Echo
Board-ID: nRF52840-TEcho-v1
SoftDevice: S140 version 6.1.1
Date: Oct 13 2021
```

| Fact | Value |
|---|---|
| Model | LilyGo T-Echo (original — not Plus, not Lite) |
| Board ID | `nRF52840-TEcho-v1` |
| Bootloader | UF2 0.6.1-2-g1224915, Oct 13 2021 (matches the Meshtastic version) |
| USB | VID `0x239A`, PID `0x0029` |
| CDC | COM8 |
| MSC | drive `E:`, label `TECHOBOOT` |
| USB storage serial number | **`0123ABCD4567EF01`** |

> ⚠️ **The FAT volume serial number is `0042-0042` and hard-coded in the bootloader** — it is
> identical on *every* T-Echo, as is the label `TECHOBOOT`. Two connected devices
> can be told apart **only** by the USB storage serial number (derived from the
> nRF52840 DEVICEID). See `firmware/nodes.ini`.

## 2. SoftDevice and flash usage

SoftDevice information struct at `0x3000` (= MBR_SIZE + 0x2000), magic `0x51B1E5DB` **OK**:

| Field | Value |
|---|---|
| `SD_ID` | 140 → S140 |
| `SD_VERSION` | 6001001 → **v6.1.1** |
| `SD_FWID` | `0x00B6` (read as `0xFFFF00B6`) |
| `SD_SIZE` | `0x26000` (152 KiB) → **application base `0x26000`** |

Occupied flash pages (4 KiB granularity, `0xFF` = empty):

```
0x001000-0x026000  USED     148 KiB   SoftDevice S140 6.1.1
0x026000-0x0CB000  empty    660 KiB   <- NO application
0x0CB000-0x0CF000  USED      16 KiB   remnants of an old firmware
0x0CF000-0x0EA000  empty    108 KiB
```

**No application is flashed.** That is why the device sits in the bootloader permanently —
not because someone double-pressed reset.

The 16 KiB block at `0x0CB000` contains newlib strings with the build path
`/Host/home/ilg/Work/arm-none-eabi-gcc-9.3.1-1.1/...` (xPack GNU Arm Embedded GCC 9.3.1).
It is never executed — the reset vector is at `0x26000` and is empty — but it can be cleaned up
with the first own flash.

## 3. Consequence: no bootloader update needed

The Adafruit nRF52 Arduino core **1.7.0** (bundled in `platform-nordicnrf52@10.11.0`) builds
against S140 6.1.1 for **every** nRF52840 board:

- `boards.txt` @ tag 1.7.0: `sd_version=6.1.1`, `sd_fwid=0x00B6`, `ldscript=nrf52840_s140_v6.ld`
- Linker script: `FLASH ORIGIN = 0x26000, LENGTH = 0xED000 - 0x26000` (= `0xC7000` = 815104 B)
- Only the nRF52833 board (`pca10100`) uses S140 7.3.0.

The device reports exactly `SD_FWID 0x00B6` and `SD_SIZE 0x26000`. **Exact match.**

In any case there is **no** T-Echo bootloader with S140 7.3.0 (checked: Adafruit
`src/boards/`, bootloader Makefile, Meshtastic `bin/`, OTAFIX release assets). The bootloader
is not touched — that is also the only step that would be dangerous without an SWD probe.

Complete flash map (from the linker script and bootloader sources):

```
0x000000-0x001000  MBR
0x001000-0x026000  SoftDevice S140 6.1.1
0x026000-0x0ED000  Application            (815104 B = 796 KiB)
0x0ED000-0x0F4000  App data reserve
0x0F4000-0x0FE000  Bootloader
0x0FE000-0x0FF000  MBR parameters
0x0FF000-0x100000  Bootloader settings
```

> The flash dump ends at `0x0EA000`, not at `0x0ED000`. That is the bootloader's dump limit,
> **not** the app limit. **Verify against the map file in Gate 0.6.**

## 4. Host environment

| Present | Missing |
|---|---|
| Arduino CLI 1.5.1 (only `arduino:avr`, `esp32`) | **PlatformIO** (`~/.platformio` empty) |
| Node v24.19.0, npm 11.17.0 | **SWD probe** and all SWD tools |
| Python 3.13.13, uv 0.11.21 | **`gcc` on the host** → `[env:native]` unit tests do not run locally (CI/Docker) |
| Git 2.55.0, `gh` authenticated | `adafruit-nrfutil` (PlatformIO pulls it in itself) |
| Docker 29.7.2 | |

`LongPathsEnabled = 1` — the well-known PlatformIO Windows trap does not apply here.

⚠️ `CLAUDE.md` §0.1 pins **Node 22 LTS**; Node 24 is installed. Only affects Phase 6/7.

## 5. Open measurement points for Phase 0 — as of 2026-08-31

These questions arose from the source research and can only be settled on the device.
Three are answered, three are not, and one is half answered.

| # | Question | Status |
|---|---|---|
| 1 | **Hardware revision** (`pinmap.md` §1): are P1.01/P1.03 LEDs or ePaper MISO/LoRa DIO0? | **open.** Worked around rather than answered: P0.14 is an LED on both revisions and the only pin this project drives. `hal/board.cpp` pins that down with a `static_assert`. |
| 2 | Does LoRa run with `REG_EN` alone, without `PWR_ON`? | **open.** Decides whether `SNIFF` (§3.1) works with the peripherals switched off — and with it the power budget. Needs the ammeter, i.e. the same bench session as Gate 0.4. |
| 3 | Is the battery divider permanently connected to VBAT? | **half.** No, at least not to VBAT alone: 4807 and 4821 mV on the cable, measured twice independently. Probably the latch node that VBUS feeds through D5. See **D13**. |
| 4 | JEDEC ID of the external flash | **answered: `0xBA6015` = ZD25WQ16B**, 2 097 152 bytes. Sketch 04, 100 reboot cycles without data loss; LittleFS mounts on it (sketch 10). |
| 5 | TCXO fitted? | **open.** Sketch 07 talks to the SX1262 over raw SPI and does not use RadioLib's `begin()` — the question only arises in Phase 2. |
| 6 | Does the RTC have its own backup supply? | **open.** Not measurable on the cable: `PWROFF` pulls `PIN_PWR_ON` LOW and the PCF8563 keeps answering, because VBUS holds the same node through D5. See **D2**. |

**Two of them — 3 and 6 — are answered by the same run: device on battery, no USB cable.** And
Gate 1.6 along with them.

### What was added on the device

| Fact | Value | Evidence |
|---|---|---|
| BME280 | address `0x77`, chip ID **`0x60`** (not `0x58`, so with humidity sensor) | sketch 02, 11 |
| PCF8563 | address `0x51`, VL flag 0 | sketch 02, 03 |
| External flash | `0xBA6015`, 2 MiB, **deep power-down not demonstrable** | sketch 04, see **D12** |
| SX1262 | BUSY handshake, sync word `0x1424` (reset value) | sketch 07, read-only |
| E-paper | 200 × 200, partial refresh **324 ms**, full **4410 ms** | sketch 08, 11, see **D14** |
| GNSS L76K | fix in 3.7 s, 11 satellites in view **indoors**; reset pin silences it (0 bytes) | sketch 06, 11 |
| GNSS time | confirmed against NTP to the second | sketch 11 |
| Touch pad P0.11 | idle level **HIGH** → active low; cross-check with a press pending | sketch 11 |
| Tickless idle in the core | present | Gate 0.7 |
| Flash and RAM budget | 671 KiB reserve, 218 KiB RAM free (without `ui/`) | Gate 0.6 |

The BME280's chip ID is more than a tick box: `0x58` would have been a BMP280 without a humidity sensor,
and the field would have gone out silently as zero in every `TELEMETRY` frame.
`hal::Bme280Sensor` therefore checks it on every `begin()` and reports `humidityValid = false`
instead of delivering a zero that looks like a measurement.
