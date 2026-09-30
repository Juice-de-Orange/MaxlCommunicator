# Phase 0 — Gate 0.3 External flash

| | |
|---|---|
| Date | 2026-08-31 |
| Node | A (USB storage serial `0123ABCD4567EF01`) |
| Firmware | 0.1.0, `8eb31e3`, bring-up sketch 04 |
| Environment | `bringup` |
| Measuring equipment | Adafruit_SPIFlash 5.x via the QSPI transport, serial log |
| Conditions | USB power, battery permanently installed and charging |
| Raw data | `raw_2026-08-31_gate_0-3_flash_cycles.log` |

## Result: PARTIAL

Gate 0.3 requires: *"Matches ZD25WQ16B; LittleFS mounts, survives 100 write/reboot cycles."*
Three parts, two of them done.

| Check | Expected | Measured | Result |
|---|---|---|---|
| JEDEC ID | ZD25WQ16B = `0xBA6015` | **`0xBA6015`** | PASS |
| Chip size | 2 MiB | **2,097,152 B** | PASS |
| `Adafruit_SPIFlash::begin()` | true | true (with explicit descriptors) | PASS |
| Write/reboot cycles | 100 | **78** — aborted externally, see below | PARTIAL |
| Record survives every reset | yes, 78 of 78 | **yes, 78 of 78** | PASS |
| LittleFS mounts | yes | **not checked** — belongs to `hal/block_store_littlefs` | open |
| Deep power-down enters/exits | yes | **not reached** (only runs after cycle 100) | open |

## Raw data

```
RESULT flash.begin = 1
RESULT flash.jedec_raw = 0xBA6015
RESULT flash.jedec = 0xBA6015
RESULT flash.part = ZD25WQ16B
RESULT flash.size_bytes = 2097152
RESULT cycle.record_valid = 1
```

The cycle test verifies the previous boot's record **before** each write. Up to cycle 78 not
a single one was lost or corrupted; a failure would have printed
`VERDICT fail -- the record written before the last reset did not survive it`
and stopped the run. That did not happen.

## Observations — why it stopped at 78

Not the chip and not the firmware. The **host's xHCI controller** gave up after about 80
rapid re-enumerations:

```
usb 1-1: new full-speed USB device number 102
cdc_acm 1-1:1.0: ttyACM0: USB ACM device
usb 1-1: USB disconnect, device number 102        <- same second
usb 1-1: new full-speed USB device number 103
usb 1-1: Device not responding to setup address.
usb 1-1: device not accepting address 103, error -71
usb 1-1: WARN: invalid context state for evaluate context command
```

After that the port is `not attached` and the kernel does not try again. A port restart via
`sysfs` would need root, and no password was available at night.

Two bugs in sketch 04 led to this, both fixed:

1. **The visibility window ran from boot instead of from USB readiness.** Enumeration itself
   takes a few seconds, so the window had often already expired by the time the host was
   done — the device then reset itself milliseconds after the port appeared, again and
   again.
2. **No clean detach before the reset.** Disappearing in the middle of the host's setup
   sequence is exactly what produces `error -71`. Now `TinyUSBDevice.detach()` with a 250 ms
   pause before `NVIC_SystemReset()`.

## What is missing to finish

1. Unplug the USB cable once and plug it back in (fixes the port).
2. `tools/bringup_run.py 4` again — the counter is stored **in flash**, the run resumes at
   78 and only needs the remaining cycles.
3. LittleFS mount via `hal/block_store_littlefs`.
