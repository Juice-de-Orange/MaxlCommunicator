# Device stack — integration, checked on the host

| | |
|---|---|
| Date | 2026-08-31 |
| Node | none — only flash, clock and BLE link are fakes |
| Environment | Host build in `gcc:14` |

## Why this is here

Until this night, `link/`, `app/`, `ble/` and `hal/` were **four sets of modules that had
never met each other.** Each of them compiled on its own and was tested — and that is not the
same as: the interfaces fit together. An interface that nobody implements is a guess about
what is needed.

`app/node` implements `ble::IHost` and wires up counter, budget, queue, journal, peer table
and scheduler. The test in `firmware/test/unit/test_node_integration.cpp` pushes real chunks
from a phone into it and checks what ends up in flash at the other end.

**133 test cases** in total (122 before), 406,952 assertions, 0 failures.

## Two real bugs that no single layer would have shown

### 1. The frame counter started at 0 — and made the first journal entry unreachable

`docs/bridge-protocol.md` §3: the phone asks `GET_QUEUE(sinceCounter)` for "everything
after the last counter it durably stored". A phone that has nothing yet sends **0**.
And the bridge already documents its own storage like this:

> `highWaterMark()` — *the highest counter this store durably holds, **or 0 if it holds
> nothing***

A device whose first counter actually is 0 therefore has **exactly one journal entry that no
client can ever fetch** — the first one. Each side on its own was consistent; only together
did they produce a contradiction.

Fixed: the counter starts at 1. Zero means "none" everywhere.

### 2. `FACTORY_RESET` erased the frame counter

The first version erased all regions. `CLAUDE.md` §2.1: *"Monotonic per device, never
reset, never reused."*

What that would have meant, in plain words: **a user who taps "factory reset" would have
silently broken the cryptography of every connection of the device** — the counter back at
the start, the same CCM nonces again, against a peer that still knows the old ones.

Fixed: the counter region is excluded.

## What the test plays through

| Check | |
|---|---|
| Text from the phone lands in the persistent queue and survives a restart | ✓ |
| Accepted without a network key? No — `ERR_NO_KEY`, and the queue stays empty | ✓ |
| Without valid time: send-blocked, `ERR_NO_TIME`, and the status flag says so | ✓ |
| The status body is the one from §4 — byte for byte, including the g3 limit 360000 | ✓ |
| Config TLVs there and back, with a report on what was not applied | ✓ |
| An out-of-range value is reported, the others still take effect | ✓ |
| Received frames become journal entries, `GET_QUEUE` returns them, `ACK_QUEUE` releases them | ✓ |
| Journal counters are strictly monotonic over 20 entries | ✓ |
| `FACTORY_RESET` clears everything except the counter | ✓ |
| `LINK_TEST` with an exhausted budget: `ERR_BUDGET_EXHAUSTED`, **not** queued | ✓ |
| `GET_BUDGET` returns the release time the UI needs for "waiting until HH:MM" | ✓ |

## What is still missing

The radio loop. It belongs with a working `hal/radio_sx1262` and two devices pointing at each
other — writing it now would mean building against an interface that has never exercised
anything.
