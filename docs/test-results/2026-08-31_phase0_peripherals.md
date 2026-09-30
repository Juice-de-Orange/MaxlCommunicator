# Phase 0 — Peripheral run (gate 0.1) and what came out of it

| | |
|---|---|
| Date | 2026-08-31, 08:35–08:52 |
| Node | A (USB storage serial `0123ABCD4567EF01`) |
| Firmware | 0.1.0, `338581f`, bring-up sketches 02–08 |
| Environment | `bringup` |
| Measuring equipment | `tools/morning.sh`, raw output under `raw/2026-08-31_0835_*` |
| Conditions | USB power, antenna attached, indoors. No sketch of this phase transmits. |

## Result: Gate 0.1 PARTIAL — four of six peripherals confirmed

Gate 0.1 requires: *"Every pin in `hal/board.h` exercised by a minimal sketch — each
peripheral responds."*

| Sketch | Peripheral | Verdict | Key point |
|---|---|---|---|
| 02 | I2C: BME280, PCF8563 | **pass** | 0x77 chip ID 0x60, 0x51 VL flag 0 |
| 03 | RTC commands | (no verdict) | command-driven, see gate 0.5 |
| 04 | External flash | **pass** | own report, gate 0.3 |
| 05 | Battery ADC | **fail** | 4807 mV — see D13 |
| 06 | GNSS L76K | **inconclusive** | module answers, fix valid |
| 07 | SX1262 | **pass** | BUSY handshake, sync word `0x1424` |
| 08 | E-paper | (not collectable) | test image is shown on the panel |

## Raw data

**Sketch 07 — SX1262, read-only:**

```
VERDICT pass -- BUSY handshakes and the sync word register reads its reset value
RESULT reg.expected = 0x1424
```

The radio chip answers over raw SPI, the BUSY handshake works, and the sync word register
returns its reset value. **Nothing was transmitted** — the sketch does not contain a single
command that puts the SX1262 into TX.

**Sketch 06 — GNSS, and this is the surprise:**

```
RESULT gnss.bytes                        = 4391
RESULT gnss.sentences                    = 79
RESULT gnss.gga                          = 6
RESULT gnss.gsv                          = 30
RESULT gnss.rmc                          = 6
RESULT gnss.satellites_in_view           = 11
RESULT gnss.fix_valid                    = 1
RESULT gnss.bytes_while_reset_asserted   = 0
RESULT gnss.bytes_after_release          = 1207
```

**Eleven satellites in view and a valid fix — indoors.** This means the `hal/nmea`
implementation can be checked not only against fixtures but against real sentences from this
module; sketch 11 does exactly that.

`gnss.bytes_while_reset_asserted = 0` is the second important value: **the reset pin really
silences the module.** `hal::GnssL76k::powerOff()` therefore holds `PIN_GPS_RESET` LOW and
closes Serial1 — there is no separate power switch for the GNSS on this board.
Whether "silent" also means "unpowered" is not answered by this. That is the second half of
gate 1.7 and needs an ammeter.

**Sketch 05 — battery:** failed, with 4807 mV. In detail in
`docs/decisions/0001-open-decisions.md` **D13**: on the cable the divider probably does not
measure the cell but the latch node that VBUS feeds via D5 — the same structure that already
showed up with the RTC (D2). Two findings, one cause.

## Observations

**1. Three of seven sketches reported exactly once.** Sketches 04, 08 and 10 carried out their
measurement, printed the report and set a `g_done` flag. The report therefore existed for one
loop iteration — and that lies **before** the moment `tools/bringup_run.py` opens the port
(it deliberately waits 1.5 s after the device node appears, because opening earlier throws an
IO error).

Result: three passed checks behind three timeouts. `bringup/common.h` states the contract in
its first paragraph — *"a report cycle that repeats so a late reader still sees the whole
run"* — and the three sketches broke it.

All three have been rebuilt: the **work** still runs exactly once (sketch 10 writes to flash,
sketch 08 runs twenty partial refreshes), the **report** repeats.

**2. The journal region was never checked in sketch 10.** `exerciseRegion` had 64-byte
buffers, `app::kJournalRecordBytes` is 70. So on every run the region fell into the
"record is 70 bytes, skipping the round trip" branch — and that line was printed outside the
report frame, where nobody saw it. The region through which every event reaches the phone was
never tested. Buffers are now 96 bytes.

**3. An own mistake that cost the first run.** While `morning.sh` was running, new files were
written to `firmware/src/hal/`. PlatformIO compiles all of `src/`, so a bug in them (`size_t`
without `<stddef.h>`) broke the build of **every following sketch** — 10, 02, 03, 05, 06, 07,
08 all failed at the compiler, not at the hardware. **While a bring-up run is in progress,
`firmware/src/` is not touched.** `firmware/test/` and `docs/` are safe.

## Deviations from the specification

Gate 0.1 stays open until `hal/board.h` is settled and sketch 09 has confirmed the inputs.
Gate 0.2 depends on D1 criterion 2, and that is a look at the panel.
