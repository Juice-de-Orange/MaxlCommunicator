# Phase 0 — A stuck I2C bus takes the whole image down with it, silently

| | |
|---|---|
| Date | 2026-09-01 |
| Node | B (`89EF45670123ABCD`, COM9) and A (`0123ABCD4567EF01`, COM8) as control |
| Firmware | `maxl-bringup-0.1.0-g0582325`, before that `gc10bed3` and `gb277262` |
| Environment | `bringup` |
| Measuring equipment | Serial capture, USB enumeration, LED by eye |
| Conditions | Both boards on USB, antennas fitted, cell inserted |

## Result: PASS — cause found, fixed and confirmed on the device

| Check | Expected | Measured | Result |
|---|---|---|---|
| Bring-up 09 on B (no I2C, no flash) | prints | prints completely | PASS |
| Bring-up 02 on B (I2C only), **before** the fix | prints | 0 bytes in 28 s | FAIL |
| Bring-up 02 on B, **after** `hal::recoverI2cBus` | both devices answer | 0x51 + 0x77, `i2c.count = 2` | PASS |
| BME280 chip ID on B | 0x60 | 0x60 | PASS |
| PCF8563 on B | answers, time valid | `rtc.vl_flag = 0`, `time_trusted = 1` | PASS |
| Dead-man rescue from the hang | jumps into the bootloader | TECHOBOOT after ~15 min, reproduced twice | PASS |

## What happened

Node B went completely silent in the evening and stayed that way across **ten reflashes,
three physical replugs and an image from before all of the day's changes** (`c10bed3`).
The board enumerated as a CDC composite device throughout, could be flashed throughout,
and did not output a single byte.

Isolation settled it: bring-up 09 touches neither I2C nor the external flash and printed
normally. Bring-up 02 is I2C and nothing else, and it hung. The same board, a few minutes
apart.

I2C has no reset. A slave interrupted in the middle of a byte keeps holding SDA low and waits
for the clocks that would finish its transfer; TWIM on the nRF52 does not run into a timeout
in that case, so `Wire.begin()` never returns.

## Why this was so hard to see

Three things together led astray, and all three generalise:

1. **Every bring-up sketch only prints from `loop()`.** A *failure* in `setup()` is reported
   (`store.mounted = 0`, `radio.ready = 0`), a *hang* produces nothing at all. Silence is the
   only state the instrumentation cannot describe.
2. **The board enumerates and flashes anyway.** The core runs TinyUSB in a task of its own,
   independent of `loop()`. "It shows up on the bus" is no evidence that the firmware is
   running — an hour of diagnosis went into assuming exactly that.
3. **A power cycle does not help.** The PCF8563 has a backup supply — it has to, otherwise
   gate 0.5 could not pass — so unplugging the USB cable leaves untouched precisely the device
   that is most likely to be holding the bus.

## The fix

`hal::recoverI2cBus` (`firmware/src/hal/i2c_recover.{h,cpp}`) does what the I2C specification
and NXP's AN10216 prescribe: clock the bus by hand until the slave releases SDA, then a STOP
so that it finds its way back to the idle state. Open-drain throughout — actively driving high
against a slave that is still holding low would be a short circuit.

It runs in `commonSetup()`, before anything that can touch `Wire`. Not in the individual
sketches: the one that forgets it is exactly the one you can no longer reach afterwards.
Bring-up 19 reports `i2c.was_stuck`, because a bus that had to be freed is a fact about the
hardware and not a side note.

## Observations that were not part of the gate

- **The dead-man rescue is thereby proven on the real device**, twice and independently: the
  board brought itself back into the bootloader from a hang, without anyone having to press a
  button. That is the purpose sketch 14 was built for, here under unintended conditions.
- **The dead man proves less than it seems to prove.** It is only poked when
  `g_usbEverReady` (`common.cpp:60-64`). Its firing therefore only means that
  `TinyUSBDevice.mounted()` was never true — it does not separate "`setup()` hangs" from
  "the loop runs, but USB never becomes ready". The dark LED was the second, independent
  observation that settled it.
- **Bring-up 00 and 01 had not built since `664fa94`** (`"deadman.h"` instead of
  `"hal/deadman.h"`). Of all images, the two minimal rescue images were the ones that could
  not be compiled. Fixed.
- **`MAXL_SETUP_TRACE`** was built for exactly this case and was useless here: a device that
  is not mounted discards the bytes. It is good for a hang *after* enumeration, not before
  it. On node A it works.

## Raw data

Serial captures are not in the repo (`.gitignore`: `docs/test-results/raw/`).
Key lines from node B after the fix:

```
MAXL-BRINGUP 02 begin
RESULT i2c.addr = 0x51
RESULT i2c.addr = 0x77
RESULT i2c.count = 2
RESULT bme.chipid = 0x60 (BME280)
RESULT rtc.vl_flag = 0
RESULT rtc.time_trusted = 1
VERDICT pass -- BME280 and PCF8563 both answer and identify
```
