# Phase 1 — The drivers on the device, and gate 1.1

| | |
|---|---|
| Date | 2026-08-31, 09:20–09:22 |
| Node | A (USB storage serial `0123ABCD4567EF01`) |
| Firmware | 0.1.0, bring-up sketch 11 via the `hal/` and `ui/` implementations that ship |
| Environment | `bringup` |
| Measuring equipment | Serial output; GNSS time against the NTP-synchronised computer clock |
| Conditions | USB power, indoors with a view through a window, antenna attached. Nothing was transmitted. |

## Result: Gate 1.1 PASS. Five more peripherals answer through the drivers.

Sketch 11 runs `hal::DisplaySsd1681`, `hal::Bme280Sensor`, `hal::BatteryAdc`,
`hal::GpioInputs`, `hal::StatusLed` and `hal::GnssL76k` — and draws real screens onto the
real panel with `ui::renderScreen`. No throwaway code: it is the same path the application
takes.

| Check | Expected | Measured | Result |
|---|---|---|---|
| **Gate 1.1** partial refresh, 20 in a row | each < 400 ms | **worst 324 ms**, mean 323 | **PASS** |
| Full refresh | ~2 s per `CLAUDE.md` §1.6 | **4410 ms** | see D14 |
| BME280 answers and identifies itself | chip ID 0x60 | **0x60 at 0x77** | PASS |
| Sensor values plausible | indoors | 27.34 °C · 45.06 % · 97056 Pa | plausible, gate 1.3 open |
| GNSS fix via `hal::GnssL76k` | a fix | **after 3725 ms**, 7 satellites | PASS |
| GNSS position | valid fix | **valid fix** (coordinates deliberately omitted) | plausible |
| **GNSS time against NTP** | to the second | **matches** — see below | PASS |
| NMEA sentences rejected | few | **0 of 40** | PASS |
| Battery ADC | 3.3–4.2 V | **4821 mV** | see D13 |
| Touch resting level | unknown | **HIGH** | see below |

## The clock recovery path is proven

`CLAUDE.md` §1.2: *"If the RTC time is not valid on boot, the device starts fully
transmit-blocked until time is re-established over BLE or GNSS."* The GNSS half of this
sentence has now been measured against real satellite data:

```
RESULT gnss.has_time = 1
RESULT gnss.unix     = 1788160871      = 2026-08-31 07:21:11 UTC
computer clock (NTP)                    = 2026-08-31 07:21:27 UTC
```

The 16 seconds are the time between the module's last RMC and the `date` call.
This confirms the RMC parsing, the `ddmmyy` conversion and `daysFromCivil` against an
independent reference — not just against the fixtures in the host tests.

`gnss.sentences_rejected = 0` with 40 accepted sentences: the checksum check rejects nothing
valid, and no fragment got through after the reset pulse.

## Why gate 1.1 failed on the first attempt

The first run measured **471 ms** and therefore failed. Not chance and not noise:

| What was refreshed | Duration |
|---|---|
| whole panel, 200×200, as a partial window | **471 ms** |
| 152×24 window (sketch 08) | **323 ms** |
| changed area, ~48×8 (this run) | **324 ms** |

`hal::DisplaySsd1681::present()` initially always pushed the whole area to the controller as
a partial window. That is a correct measurement of the wrong thing: no screen ever changes
200×200 pixels at once.

So `hal::Canvas::diffBounds()` was added — it returns the rectangle in which two canvases
differ, rounded outwards to byte columns (the SSD1681 addresses its RAM byte-wise; a window
in the middle of a byte is silently widened anyway). `ui::Ui::render()` passes on exactly
this rectangle. One pass over the buffers answers both questions — *whether* something has
changed and *where*.

This is not just about the gate: every bit of area saved is current, which `CLAUDE.md` §5
needs for 2.38 mA on average.

## Two findings that close open questions

**1. The touch pad rests HIGH.** `docs/hardware/pinmap.md` §4 listed the polarity of
P0.11 as disputed: LilyGO's header says active high, Meshtastic sets `ACTIVE_LOW true` with a
comment saying the opposite. Measured over 16 samples, with no finger nearby:

```
RESULT inputs.touch_resting_level  = 1     (HIGH)
RESULT inputs.button_resting_level = 1     (HIGH, expected: active low with pull-up)
```

If the pad rests HIGH, it is **active low** — Meshtastic's define is right, the comment next
to it and LilyGO's header are not. `hal::GpioInputs` stays on the "active high" default for
now, because a single resting level without a cross-check is no proof:
**sketch 09 decides**, by a press demonstrably inverting the level. Until then the
consequence of a mistake is a pad that does nothing — visible and harmless — and not a device
that switches screens on its own.

**2. The battery value confirms D13.** 4821 mV here, 4807 mV in sketch 05, measured with a
different ADC reference. The consistency suggests that the divider really does hang on a
USB-fed node and not on the cell.

## Observations

**A silent hang that cost 140 seconds.** The first run gave no output at all — device on
USB, port present, nothing on the line. Cause:
`hal::GnssL76k::begin()` calls `powerOff()`, and that called `Serial1.end()` on a UART that
had never been opened. `Uart::end()` in the core does

```cpp
while (!(nrfUart->EVENTS_TXSTOPPED && nrfUart->EVENTS_RXTO)) yield();
```

and a disabled UARTE never raises either of the two events. The `yield()` keeps FreeRTOS
running — that is why USB stayed up and the device looked healthy from the host, while the
loop task stood still forever.

Two consequences, both recorded:

- `hal::GnssL76k` remembers whether the UART is open. That is not a precaution but a
  necessity: `CLAUDE.md` §1.5 requires the module to be **off by default**, so `begin()`
  calls `powerOff()` — exactly the case that hangs. Every device would have stalled at
  start-up as soon as the driver is wired into the application.
- **The dead-man timer does not rescue this.** `service()` is called from `loop()`; a
  `loop()` that does not return never checks it. `deadman.h` now says so.
  Running it from its own FreeRTOS task would cover this and was deliberately *not* done on
  the side — it changes the recovery path that every unattended run depends on.

Sketch 11 now prints progress lines before the report frame. The collector ignores
everything before `begin`, so they cost nothing — and they are the difference between a
diagnosis in seconds and two minutes of staring at a silent port.

## What remains open

- **Gate 1.2** (ghosting): after 20 partials and a full refresh the panel holds a
  `PEERS` screen. Needs eyes and a photo.
- **Gate 1.3** (BME280 against a reference instrument), **1.4/1.5** (button presses),
  **1.6** (ADC against a multimeter, and only meaningful without the USB cable), **1.7**
  second half (current draw with and without GNSS).
