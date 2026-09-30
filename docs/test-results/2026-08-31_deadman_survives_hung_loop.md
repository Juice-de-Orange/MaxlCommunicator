# Infrastructure — The dead-man timer survives a hung `loop()`

| | |
|---|---|
| Date | 2026-08-31, 11:40 |
| Node | A (USB storage serial `0123ABCD4567EF01`) |
| Firmware | 0.1.0, bring-up sketch 14 |
| Environment | `bringup` |
| Measuring equipment | `lsusb -d 239a:0029`, every two seconds |
| Conditions | USB power. No manual intervention during the run. |

Not a gate from `docs/test-plan.md` — but the prerequisite for flashing unattended.
Without a working way back into the bootloader, every failed attempt needs a thumb on the
reset button.

## Result: PASS

| Time since flash | USB reports |
|---|---|
| 2 s … 32 s | `Adafruit MaxlCommunicator` — the application is running |
| from 34 s | **`Adafruit T-Echo v1` — the bootloader** |

Sketch 14 deliberately hangs its `loop()` at 12 s; the timer was set to 20 s. A trip at
about 32 s was expected; the device was back in the bootloader at 34 s. **Without
intervention.**

## What would have happened before

Nothing. That is exactly the point.

The old timer checked its deadline in `service()`, and `service()` was called from `loop()`.
A `loop()` that does not return never checks anything. The device would have stayed on the
bus as `Adafruit MaxlCommunicator` forever — enumerated, silent, looking healthy from the
host.

**This is not a hypothesis.** It happened exactly like this the same morning:
`hal::GnssL76k::begin()` called `Serial1.end()` on a UART that had never been opened, and the
core does this there

```cpp
while (!(nrfUart->EVENTS_TXSTOPPED && nrfUart->EVENTS_RXTO)) yield();
```

A disabled UARTE never raises either of the two events. The `yield()` kept FreeRTOS running
— USB stayed up, the host saw a healthy device, and for 140 seconds not a single byte arrived.
The timer was armed the whole time and never got around to looking at itself.

Sketch 14 recreates the same shape: an endless loop with `yield()` on every iteration,
so the scheduler keeps running and everything except this task stays healthy.

## How it is built now

The timer runs in its own FreeRTOS task at **`TASK_PRIO_NORMAL`** — above the loop task
(`TASK_PRIO_LOW`), so that a spinning loop cannot starve it, and below the Bluefruit task, so
that it never delays the radio stack. Between checks it blocks on `vTaskDelay`; it costs one
kilobyte of stack and a few microseconds four times per second.

The shared variables are `volatile uint32_t` without a mutex: an aligned 32-bit access is a
single instruction on Cortex-M and cannot be half-read. The worst interleaving is a `poke()`
seen one cycle too late — meaningless against a deadline in the tens of seconds.

`service()` stays and has no effect in the normal case. It only kicks in if the task **could
not be created**; then it is the only check there is, and `taskRunning()` tells you that you
are in this degraded state. A silent fallback to the old coverage would be worse than the old
coverage.

## Cross-check: no false trips

Directly afterwards, sketch 12 — the whole stack — ran for thirty seconds without a restart,
with `refresh.skipped` at 964 and a timer of 300 s. A watchdog that kills healthy runs would
not be progress.

## What is still not covered

- **A crash before `setup()`, before `arm()` is reached.** Only the button helps there.
- **A hang with interrupts masked**, or one that takes the scheduler down with it. Then no
  task runs any more, this one included. Only the nRF52840's hardware watchdog helps against
  that, and it belongs to Phase 5, because it can no longer be stopped once it is running.
- **The way back is DFU, not a restart.** For bring-up that is right: a host is standing next
  to it and the next image wants to be flashed. For a device in the field it would be exactly
  wrong — it would go into the bootloader and wait there until someone connects a computer.
  The application therefore needs a *reset*, not a DFU jump.
