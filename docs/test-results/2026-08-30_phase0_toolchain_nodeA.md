# Phase 0 — Toolchain bring-up (step 1)

| | |
|---|---|
| Date | 2026-08-30 |
| Node | A (USB storage serial `0123ABCD4567EF01`) |
| Firmware | 0.1.0, build before the first commit (`gnogit`) |
| Environment | `debug` |
| Conditions | USB power, no battery operation, antenna not relevant (no radio access) |

## Result: PARTIAL

| Check | Result |
|---|---|
| `pio run -e debug` | **PASS** |
| RAM usage | 8352 B of 248832 B (3.4 %) |
| Flash usage | 56416 B of 815104 B (6.9 %) |
| Flash upper limit matches `nrf52840_s140_v6.ld` (`0xED000-0x26000 = 0xC7000`) | **PASS** |
| Upload via `nrfutil` / COM8 / 1200 bps touch | **PASS** — `Device programmed.` |
| Application USB CDC enumerates | **FAIL** — no new COM port after 20 s |

## Observation

After flashing, the device disappears from USB completely: no VID 239A, no drive `E:`, no
faulty PnP device. So the application is running (the bootloader has exited), but it does not
bring up USB.

The bootloader is intact — a double reset brings `TECHOBOOT` back.

## Recovery path: VERIFIED (corresponds to step 1b of the Phase 0 sequence)

Double-clicking the **top left** button (P0.18) reliably brings the device back:
drive `E:` with `TECHOBOOT` reappears, CDC shows up as COM8, `INFO_UF2.TXT`
reads unchanged. This proves — rather than merely assumes — that an application that does
not bring up USB does not make the device unreachable.

Button assignment according to the LilyGO README, confirmed on the device:

| Button | Pin | Function |
|---|---|---|
| top left | P0.18 | once = reset, **twice = bootloader (DFU)** |
| bottom left | P1.10 | user button, active low |

## Three build hurdles solved along the way

1. `-Werror` fired in Nordic's SoftDevice headers: their `SVCALL` macro leaves parameters
   unused. → `-Wno-unused-parameter`.
2. `-Wundef` fired in `nrfx` and `FreeRTOSConfig.h`. → removed for now; the clean
   solution is to pass the core includes as `-isystem` instead of `-I` (TODO in the ini).
3. The linker could not find `Adafruit_USBD_CDC`: core 1.7.0 does not ship its own
   TinyUSB implementation. → `adafruit/Adafruit TinyUSB Library` as `lib_deps`.
   This library pulls in SdFat, which needs an `SS` define in the variant.

Hurdle 3 is also the main suspect for the USB failure: the pinned version 3.4.4
is considerably newer than core 1.7.0 (May 2024).

## Next step

Pin the TinyUSB library down to a 2.x version that matches the core, then flash again.
As a cross-check, a blink-only image on **P0.14** — if it blinks, only USB is affected.
