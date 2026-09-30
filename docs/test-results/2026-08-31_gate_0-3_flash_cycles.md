# Phase 0 — Gate 0.3 External flash: JEDEC ID and 100 write/reboot cycles

| | |
|---|---|
| Date | 2026-08-31 |
| Node | A (USB storage serial `0123ABCD4567EF01`) |
| Firmware | 0.1.0, `338581f`, bring-up sketch 04 |
| Environment | `bringup` |
| Measuring equipment | Serial output via `tools/bringup_run.py`; raw data under `raw/` |
| Conditions | USB power, antenna attached, indoors. Sketch 04 never transmits. |

## Result: PASS — both halves, with a finding that is not part of the gate

Gate 0.3 requires: *"Matches ZD25WQ16B; LittleFS mounts, survives 100 write/reboot
cycles."* The mount via the real `hal::BlockStoreLittleFs` is sketch 10 and has a report of
its own; this one is about identification and cycles.

| Check | Expected | Measured | Result |
|---|---|---|---|
| JEDEC ID | `0xBA6015` (ZD25WQ16B) | **`0xBA6015`** | PASS |
| Size | 2 097 152 bytes | **2 097 152** | PASS |
| Write/reboot cycles | 100 | **100 / 100** | PASS |
| Record survives every reset | yes, every time | no loss | PASS |
| Pattern round trip at the end | 0 mismatched bytes | **0** | PASS |
| Deep power-down | chip goes silent | **keeps answering** | see below |

The second half, sketch 10 via `hal::LittleFsBlockStore` — i.e. via the implementation that
also ships:

| Check | Expected | Measured | Result |
|---|---|---|---|
| LittleFS mounts | yes | **yes** | PASS |
| Volume | 2 097 152 bytes | **2 097 152** | PASS |
| All four regions round-trip | append, read back, count | **yes** | PASS |
| `replaceAll` stays atomic, 8× | region holds exactly 1 record afterwards | **yes** | PASS |

```
RESULT clock.present            = 1
RESULT clock.vl_at_boot         = 0
RESULT flash.begin              = 1
RESULT flash.jedec              = 0xBA6015
RESULT littlefs.mounted         = 1
RESULT littlefs.volume_bytes    = 2097152
RESULT regions.round_trip       = 1
RESULT counter.replace_all_x8   = 1
VERDICT pass -- LittleFS mounts on the ZD25WQ16B and all four regions round-trip
```

**The journal region had never been checked before this run.** `exerciseRegion` used
64-byte buffers, `app::kJournalRecordBytes` is 70 — so the region fell into the
"skipping the round trip" branch every time, and that line was printed outside the report
frame. The region through which every event reaches the phone thus had zero test coverage,
with a report that looked like a success. Buffers are now 96 bytes, and the round trip really
runs.

The previous session reached 78 of 100 cycles and stopped because the host's xHCI port gave
up. After re-plugging, the sketch completed the remaining 22 cycles **on its own** — the
counter is stored in flash, not in RAM, precisely for this.

## Raw data

```
RESULT flash.jedec_raw        = 0xBA6015
RESULT flash.part             = ZD25WQ16B
RESULT flash.size_bytes       = 2097152
RESULT cycle.count            = 100
RESULT cycle.target           = 100
RESULT pattern.round_trip     = 1
RESULT pattern.mismatched_bytes = 0
RESULT dpd.jedec_while_asleep = 0xBA6015
RESULT dpd.jedec_after_wake   = 0xBA6015
RESULT dpd.entered            = 0
RESULT dpd.woke               = 1
VERDICT pass -- JEDEC id, 100 write/reboot cycles and a pattern round trip
```

In full in `raw/2026-08-31_0835_sketch04_final.log`.

## Observations

**1. The deep power-down is not proven.** `dpd.entered = 0`: after the command `0xB9` the chip
keeps answering with its JEDEC ID. `CLAUDE.md` §3.0 hinges the `DEEP_IDLE` budget of
**< 20 µA** on this chip not drawing its ~12 µA standby current. Whether it does not sleep or
whether the measurement wakes it cannot be told apart from within the firmware — many SPI NOR
chips leave deep power-down on *any* command. In detail in
`docs/decisions/0001-open-decisions.md` **D12**. The ammeter decides, i.e. **gate 0.4**, and
that makes it a blocking prerequisite for Phase 5.

**2. The final report could not be collected.** The sketch passed the gate on the device, but
printed the closing block without `MAXL-BRINGUP 04 begin` / `end`.
`tools/bringup_run.py` collects exactly between these two markers, never found an end and
reported "no complete report cycle" after 900 s — for a passed gate. The result was on the
line the whole time. The frame has been added.

This is the second trap of the same kind in this sketch (the first was the boot report before
USB enumeration, see gate 0.5). Both times a working device looked like a dead one because the
*output* did not arrive. When writing a bring-up sketch, the reporting path has to be checked
just as much as what it reports on.

**3. The collector does not survive the device's reset.** Sketch 04 resets in every cycle; the
port disappears and comes back, the open file descriptor in `bringup_run.py` is left dead and
delivers empty lines until the timeout. For sketch 04 the right procedure is therefore: flash,
let the device run on its own, then fetch the final report with `--no-flash`. This does not
apply to all the other sketches, which do not reset.

## Deviations from the specification

None. The LittleFS mount is deliberately not here but in sketch 10, via the implementation
that also ships — a second, throwaway mount would have proven nothing about the shipped one.
