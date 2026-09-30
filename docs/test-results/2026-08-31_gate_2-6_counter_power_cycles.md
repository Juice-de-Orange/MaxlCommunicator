# Gate 2.6 — 50 forced power cycles, counter never repeats or goes backwards

| | |
|---|---|
| Date | 2026-08-31, evening |
| Node | **B**, `89EF45670123ABCD` |
| Sketch | 18, `firmware/src/bringup/sketch_18_counter.cpp` |
| Firmware | `maxl-bringup-0.1.0-ga69f2ef` |
| Clock | valid — set on this board the same evening, `RTC_REF_UNIX=1788192819` |
| Antenna | fitted, confirmed before the run |

**Result: `gate_2_6.pass = 0`.** The run completed all 50 cycles; it did not time out.

```
RESULT log.cycles              = 50
RESULT log.mid_transmit_resets = 10
RESULT log.first_counter       = 0
RESULT log.last_counter        = 49
RESULT log.never_repeated      = 0        <- the failing one
RESULT log.never_backwards     = 1
RESULT gate_2_6.pass           = 0
```

## What the numbers actually show

The host caught 16 of the 50 boot reports -- the rest fell between port reopenings, which
is expected for a sketch that resets itself. In **every one of those 16**, the counter did
not move during the cycle:

```
cycle =  6   counter.at_boot =  6   counter.after_draws =  6
cycle = 12   counter.at_boot = 12   counter.after_draws = 12
cycle = 20   counter.at_boot = 20   counter.after_draws = 20
...
cycle = 49   counter.at_boot = 49   counter.after_draws = 49
cycle = 50   counter.at_boot = 49   counter.after_draws = 49
```

Boots in which a draw happened: **0 of 16.**

Two things follow, and the second is the larger one.

### 1. The failing criterion is an artefact of the last boot

`verify()` flags a repeat when an entry's `at_boot` is at or below the highest value already
seen (`sketch_18_counter.cpp:184`). The final boot is the verification boot: it reads the
counter, draws nothing, and reads the same value the previous cycle ended on -- 49 against a
`highestSeen` of 49. That trips the test.

Across the 50 cycles the counter went 0 → 49, one per cycle, monotonically, with
`never_backwards = 1`. **Nothing in this run is evidence of counter reuse.** What the gate
is meant to catch did not happen; what it caught is its own final boot.

### 2. Nothing was transmitted, and that is the real finding

`app::Node::buildFrame` returns 0 without a counter -- `counter_.next()` failing is a frame
that must not go (`node.cpp:550`, and `CLAUDE.md` §2.1: the counter is what makes nonce
reuse impossible). A cycle in which no counter is drawn is a cycle in which no frame was
built.

The one counter per cycle that *is* consumed is accounted for: the sketch appends a journal
entry per cycle, and journal entries draw from the same monotonic supply (D10).

So this run put **nothing on the air**, and the project's claim that sketches 17 and 18 are
"the first time this device really transmits" is not yet borne out for 18.

**Two candidates, and the device is needed to choose between them:**

1. The draws are refused -- `transmitAllowed()`, the rolling hour, or the per-frame lockout
   of §1.2. The clock is valid on this board, so `ERR_NO_TIME` is not it.
2. The sketch reports a peek rather than the value after drawing, in which case frames may
   have gone out and only the instrumentation is wrong.

Candidate 1 predicts a budget that stays at zero; candidate 2 predicts one that climbs.
`budget.used_ms` in sketch 17, running on this board as this is written, distinguishes them
without any new code.

## Not done

The gate is **not** re-run and **not** claimed. Both the check's last-boot condition and the
absent draws have to be settled first, and the second one is the one that matters -- a
transmit path that never transmits would make gates 2.7 and 2.10 meaningless in the same way.
