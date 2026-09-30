# Phase 0 — Gate 0.5 RTC across a reset

| | |
|---|---|
| Date | 2026-08-31 |
| Node | A (USB storage serial `0123ABCD4567EF01`) |
| Firmware | 0.1.0, `8eb31e3`, bring-up sketch 03 |
| Environment | `bringup` |
| Measuring equipment | System clock of the development computer via `tools/rtc_test.py` |
| Conditions | USB power, battery permanently installed and charging, indoors |

## Result: PARTIAL — first half PASS, second half running

Gate 0.5 requires two things: *"RTC keeps time across reset and across battery-only
operation"* and *"Drift recorded over 24 h; < 5 s/day"*.

| Check | Expected | Measured | Result |
|---|---|---|---|
| PCF8563 answers on 0x51 | ACK | ACK | PASS |
| Set and read back immediately | identical | identical | PASS |
| Time survives `NVIC_SystemReset()` | deviation ≤ 1 s (chip resolution) | **−0.10 s** | PASS |
| Reset really was a software reset | RESETREAS bit 2 (SREQ) | `0x00000004` | PASS |
| VL flag after the reset | 0 (chip trusts its time) | **0** | PASS |
| Drift over 24 h | < 5 s/day | running — reference see below | open |
| Battery-only operation | time survives | **not testable**, see below | open |

## Raw data

```
RESULT boot.after_soft_reset = 1
RESULT boot.reset_reason = 0x00000004
RESULT rtc.present = 1
RESULT rtc.vl_flag = 0
set to          1788126105
read back       1788126117
on the host     12.10 s elapsed
offset          -0.10 s
```

Reference for the drift measurement:

```
RTC_REF_UNIX=1788126105
RTC_REF_HOST=1788126105.632
```

To be evaluated with
`tools/rtc_test.py read --ref-unix 1788126105 --ref-host 1788126105.632`.
The clock keeps running independently of the MCU, across every flash too — it just must
**not be set again** until the evaluation.

## Observations

Three things first gave convincingly wrong answers and are therefore recorded here:

1. **The boot report got lost.** It was printed from `setup()`, i.e. before USB
   enumerates. That made the reset part of the gate look like a dead device. The report
   is now only printed once the host can receive it.
2. **`GPREGRET2` does not survive the reset here** — it comes back as 0. The
   UF2 bootloader runs before every application start and clears it. `RESETREAS` is set
   by the hardware and needs nobody's cooperation; that is the right signal.
3. **A timestamp at the end of a fixed read window** makes the device appear slow by half the
   window width. The −1.6 s of the first run were that, not the clock.

## Deviations from the specification

The second half of the gate — *"across battery-only operation"* — cannot be tested on this
setup, and that is a property of the hardware, not the firmware.
`PWROFF` drives `PIN_PWR_ON` (P0.12) LOW, and the PCF8563 still keeps answering:

```
RESULT rtc.acks_with_pwr_on_low = 1
RESULT rtc.acks_after_restore = 1
RESULT rtc.vl_after_rail_cut = 0
```

This matches the schematic: VBUS reaches the same latch node via D5 as long as the USB cable
is plugged in, so `VDD_POWR` never drops at all. This confirms the schematic reading from
`docs/hardware/pinmap.md` §3 — and **open decision D2 stays open**: it needs a run without
the cable.
