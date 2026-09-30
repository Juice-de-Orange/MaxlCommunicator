# Phase 5 — The measurement scaffold, and the first run of the radio driver

| | |
|---|---|
| Date | 2026-08-31, 12:25 |
| Node | A (USB storage serial `0123ABCD4567EF01`) |
| Firmware | 0.1.0, bring-up sketch 15 |
| Environment | `bringup` |
| Measuring equipment | Serial output. **The actual measurement needs an ammeter and is still pending.** |
| Conditions | USB power, antenna attached. **Nothing was transmitted.** |

Not a gate. A tool that makes gates 0.4 and 5.1 to 5.4 measurable in a quarter of an hour
instead of an afternoon.

## What it does

Sketch 15 enters five states one after another and holds each for **thirty seconds**, round
and round forever. Before each state the LED blinks as many times as the state's number;
**during** the window it is dark, because a few milliamps of LED would swamp a
twenty-microamp measurement.

| Blinks | State | Expectation from `CLAUDE.md` §3.1 | Gate |
|---|---|---|---|
| 1 | `ACTIVE` — CPU busy, everything on | ~15 mA | 5.3 |
| 2 | `GNSS_FIX` — L76K powered and searching | ~40 mA | 5.4 |
| 3 | `IDLE_FLASH_AWAKE` — panel asleep, MCU asleep, **flash awake** | — | — |
| 4 | `DEEP_IDLE` — the same, **flash in power-down** | < 20 µA | 5.1 |
| 5 | `SNIFF` — SX1262 RxDutyCycle, 2 s, MCU asleep | table in §2.3 | 5.2 |

## States 3 and 4 are gate 0.4

They differ in **exactly one thing**: whether the external flash was given the deep
power-down command. The difference between the two readings **is** the flash's contribution
— i.e. what gate 0.4 requires and what **D12** could not settle from within the firmware.

D12 in essence: sketch 04 sent `0xB9` and the chip kept answering with its JEDEC ID
afterwards. There are two explanations for that — it never slept, or the query itself woke it
— and **they cannot be told apart from software.** An ammeter tells them apart in a single
reading, because a sleeping chip draws about 12 µA less than an awake one.

`CLAUDE.md` §3.0 hinges the `DEEP_IDLE` budget of under 20 µA on this. As long as it is open,
Phase 5 is built on an assumption.

## What is already settled in this run

```
RESULT flash.ready = 1
RESULT radio.ready = 1
RESULT hold_ms     = 30000
```

**`radio.ready = 1` is a first.** `hal/radio_sx1262` had been written for two sessions and
**never executed**. Here the SX1262 comes up in the rendezvous configuration from
`CLAUDE.md` §2.5 — SF9 / BW125 / CR4/5 on 869.575 MHz — and `startReceiveDutyCycle(2000, 8)`
runs.

**Nothing was transmitted.** This sketch calls exactly two radio methods,
`startReceiveDutyCycle` and `sleep`; `transmit` does not appear in it. The Phase 2 gates
still need a second device.

## How to measure

```bash
.venv/bin/python tools/bringup_run.py 15
# Unplug the cable. The device keeps running on battery and keeps blinking.
# Ammeter into the battery lead. Count blinks, read, wait.
```

**Measuring with the cable attached does not work.** USB draws current itself and holds the
peripheral latch via D5 — a reading on the cable is a reading of the wrong thing. The same
structure that D2 and D13 hinge on.

The device keeps running meanwhile, the cycle keeps running, and plugging back in brings the
serial output back.

## What is in the measurement that is not the state

Named honestly rather than puzzled over later:

- **The dead-man timer's task wakes four times per second** for a few microseconds.
  It deliberately stays armed — unattended is exactly the situation in which a hang is
  expensive. The contribution is small and real.
- **`delay()` is `vTaskDelay` on this core,** and gate 0.7 confirmed that the core's
  FreeRTOS has tickless idle. A blocked task therefore really lets the CPU stop instead of
  spinning. What is measured is thus the state in which the device actually spends its time
  — not a busy-wait loop.
- **The panel sleeps** and holds a static image. `CLAUDE.md` §1.6: e-paper keeps its image
  without power. A panel that redraws during a current measurement measures the redraw.

## What remains open

Everything. This is the tool, not the result. Gate 5.5 (breakdown per subsystem) and 5.6
(24 h, extrapolated to ≤ 2.38 mA) additionally need a long run — 5.6 is the phase's actual
gate, and if it fails, the sniff interval is the adjustment knob that §2.3 tabulates.
