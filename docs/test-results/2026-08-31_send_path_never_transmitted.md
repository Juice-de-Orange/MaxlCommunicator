# Why the send path sent nothing — four bugs in a row

| | |
|---|---|
| Date | 2026-08-31, evening |
| Node | **A**, `0123ABCD4567EF01`; the trigger came from **B** |
| Sketch | 17, extended with diagnostic counters over the course of the evening |
| Initial finding | 100 000 messages offered, 0 accepted, `budget.used_ms = 0`, for 30 minutes |

Four bugs lay one behind the other on the same path. Each one on its own would have been
enough to keep the device silent, and **each one hid the next**. They are listed in the order
in which they were found.

## 0. The sketch could not say where it failed

Sketch 17 counted `queue.refused_budget` — and only the code `0x06`
(`ERR_BUDGET_EXHAUSTED`), which `Node::sendText` **never** returns: the budget refusal is per
frame and happens further down. The only instrumented value was the only one that could not
move.

Fixed: the sketch now reports the real error code, the queue's state distribution and five
counters from `app::Node` that name where a message gets stuck. Only after that could the rest
be found in an hour instead of a week.

## 1. The external flash was asleep, and the node correctly refused to send

```
RESULT flash.jedec_before_wake = 0xFFFFFF
RESULT flash.jedec             = 0xBA6015     (after 0xAB)
```

The ZD25WQ16B has no reset line, so a deep power-down survives every MCU reset.
Without flash there is no persistent frame counter, and `CLAUDE.md` §2.1 requires that a node
that cannot persist its counter **does not transmit**. The firmware did exactly the right
thing — it just could not tell anyone: `store.mounted = 0` was in the report, but
`sendText` reported `0x0A ERR_STORAGE`, and nobody read the two together.

**This overturns D12.** The decision recorded that the chip demonstrably does *not* enter deep
power-down; it named two possible explanations and could not choose between them.
It was the second one: `getJEDECID()` woke it up itself, which is why sketch 04 saw it answer.

Fixed in `hal::openExternalFlash()` — wake up, then open, in one place instead of six. The
candidate list used to be in the tree six times.

## 2. Messages that were in flight at power-off stayed that way forever

```
RESULT queue.depth        = 24
RESULT queue.pending      = 0
RESULT queue.in_flight    = 9      <- stranded
RESULT queue.undelivered  = 15
RESULT tx.promote_seen    = 0
```

`InFlight` means "the ARQ holds it" — and the ARQ lives in RAM. After a reset there is no
slot, no timer and no attempt counter, and `Node::promoteQueued` only looks at `Pending`.
These nine entries could **never** reach a final state again, permanently occupied the queue,
and every new message got `ERR_QUEUE_FULL`.

Gate 3.1 did not find this, although it sounds exactly like it: its twenty messages were still
`Pending`, so they had never been sent and could not get stranded.

Fixed in `MessageQueue::restore()`: `InFlight` becomes `Pending` again on restore, with the
attempt counter reset, and the revival is persisted immediately. §2.4 gives a frame three
attempts and then requires it to be **shown** as undeliverable, "not silently dropped" — an
entry that can reach neither state is exactly that silent disappearance. Host test:
`test_app.cpp`, "a message that was in flight when the power went is Pending again".

## 3. The duty-cycle budget was never charged

`link::Budget::recordTransmission()` did not appear in `firmware/src/` **a single time** — only
in unit tests and in the two-node simulation. The budget is written, persisted and tested,
and nobody connected it.

The consequence: `usedMs` stayed zero forever, the hourly account never filled up,
`earliestLegalTx` never moved — **the node would have transmitted beyond the 10 % without any
brake**, the limit `CLAUDE.md` §1.2 legally binds it to. §6 says it unmistakably: "Anything
that transmits goes through the budget tracker. There is no second path."

Fixed in `Node::onTransmitComplete()`, the only place that knows a transmission has really
finished. What is charged is the **computed** airtime from `link/airtime`, not the one
measured by the driver: the measured one lives on `Sx1262Radio` rather than on
`hal::IRadioLink` and would drag a driver type through the interface that D3 is meant to keep
clean — and gate 2.10 is exactly the comparison of the two numbers, which needs them separate.

## The state afterwards

```
RESULT tx.promote_seen     = 1
RESULT tx.requested        = 2
RESULT tx.refused_by_radio = 0
RESULT budget.used_ms      = 4352
RESULT budget.peak_used_ms = 4352
RESULT budget.wait_s       = 19
```

**2176 ms per frame.** `CLAUDE.md` §2.3 gives 2196 ms for SF9 at a 2 s sniff interval — the
computation agrees to within one percent with the table the document itself maintains.

`wait_s = 19` is the lockout from D9 that holds the node back. This makes the duty-cycle rule
actually effective for the first time in this project, and not just described.

## What this is not

**No proof that the radio signal arrives.** What is proven: `radio.transmit()` accepts the
buffer, the transmit confirmation comes back (otherwise the second transmission would have
been rejected as `Busy`), and the ARQ counts attempts and gives up after three. Whether there
was power on the antenna and whether a second node hears it, only gate 2.1 can tell.

**Gate 2.7 is not passed.** It requires a full hour and `peak_used_ms` against
360 000 ms. The run here is the precondition, not the gate.

## Left open

**16 undeliverable messages fill the queue, and nothing clears them.**
`reclaimDelivered()` only discards `Delivered`. A node without a peer thus ends up, after 24
messages, in a state in which it permanently accepts nothing more. That is compatible with
§2.4 — "Kept until the user sees it" — but a field device whose outbox walls itself up was not
the intention. This needs a decision, not a change on the side.
