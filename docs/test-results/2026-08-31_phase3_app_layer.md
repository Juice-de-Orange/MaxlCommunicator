# Phase 3 — Application layer, host-tested

| | |
|---|---|
| Date | 2026-08-31 |
| Node | none — the layer talks exclusively through `hal/` interfaces |
| Environment | Host build in `gcc:14` (`firmware/tools/hosttest.sh`) |

## Result: gates met in simulation, **not proven on hardware**

`CLAUDE.md` §5 requires that a phase only begins once the previous one is confirmed on real
hardware. Phase 0 is not. This layer is therefore **written and host-checked, but no gate is
listed as passed** — the same separation as with `link/` in session 2.

| Gate | Check | Simulation |
|---|---|---|
| 3.1 | 20 messages across a reboot: all present, order preserved, no duplicates | met |
| 3.1 | Delivery state survives the reboot as well | met |
| 3.2 | Full queue: the oldest **delivered** one is dropped, otherwise rejection with a message | met |
| 3.2 | An undelivered message is **never** displaced | met |
| 3.3 | Journal wraparound: acknowledged entries correctly released | met |
| 3.3 | Full journal without acknowledgement: the oldest is dropped, and the loss is counted | met |
| 3.4 | Telemetry interval over 6 h within 10 % | met |

**110 test cases** in total (98 before), 405,860 assertions, 0 failures.
`check_no_alloc.py`: 14 object files in `link/` and `app/`, **allowlist still empty**.

## Observations

**The queue is one record, not many.** `hal::IBlockStore::replaceAll` is documented as
atomic; `erase()` plus a series of `append()` calls has a window in which the region is
empty. Gate 3.1 requires that 20 messages survive a reboot — not that they survive a reboot
outside this window. 24 × 62 + 12 = 1500 bytes on a 2 MiB chip.

**Only delivered messages are displaced.** `CLAUDE.md` §2.4 says an undelivered message is
"not silently dropped" — displacing it to make room for a newer one is exactly that. If
nothing delivered is there, the new message is **rejected**, and the user sees a rejection.

**Only `acknowledge()` frees journal space, `fetch()` does not.** A phone that dies between
`GET_QUEUE` and `ACK_QUEUE` costs a repeated transfer and never an event. A test case of its
own, because exactly this order is justified in `docs/bridge-protocol.md` §3.

**The scheduler keeps its phase.** `nextAt += interval`, not `nextAt = now + interval`.
Over the 36 triggers that gate 3.4 measures, the second form accumulates every delay of the
loop. Tested with a loop that is deliberately never on time.

Two cases that no gate requires and that are in anyway, because otherwise they would only
show up in the field:

- **No catching up after a sleep pause.** An hour of sleep triggers **once**, not sixty
  times — otherwise the whole hourly budget would be spent on backlog.
- **The `millis()` wraparound after 49 days.** A node that is supposed to run for two weeks
  per charge experiences it on the shelf. A `now >= nextAt` stops triggering forever after
  that.

## What is missing

The wiring to `link/` — the layer is written, but not called from anywhere yet.
That belongs to an `app/` main loop, and that needs `hal/board.h`, i.e. Phase 0.
