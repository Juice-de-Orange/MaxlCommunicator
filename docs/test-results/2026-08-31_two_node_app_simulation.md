# Two complete nodes over a simulated channel

| | |
|---|---|
| Date | 2026-08-31 |
| Node | none — radio and channel are fakes |
| Environment | Host build in `gcc:14`, `firmware/test/sim/two_node_app.cpp` |

## Result: 16 checks, 0 failures — **no gate claimed**

`sim/two_node.cpp` has been running the `link/` layer through since session 2. This
simulation runs the layer above it: **two `app::Node` instances** with their own key store,
their own queue, their own journal and their own counter, each with a GATT server in front,
the same channel in between.

A message is written into node A's GATT characteristic, as a phone would do it. The check is
whether it arrives in node B's journal — ready for B's phone to fetch.
Everything in between is the shipping code: real cryptography, real replay window,
real budget tracker.

| Scenario | Observation |
|---|---|
| Clean link | A sends 4×, 0 MIC failures at B, the text is in B's journal, B knows A |
| B has a **different** network key | 4 MIC failures at B, **0** events in the application, A reports no delivery |
| A has **no key at all** | `transmitAllowed()` false, **0 transmissions**, the message is rejected instead of queued |
| A's clock is invalid | send-blocked, **0 transmissions** — without time no reconstructible budget |
| A frame is intercepted and replayed 20× | **23 rejections**, 0 additional events in the application |

## What this does **not** prove

Anything about an SX1262. The radio is a fake and the channel is arithmetic.
`docs/test-plan.md` Phase 2 requires two devices on the workbench, and no simulation is a
substitute for that. What it achieves: that the time at the workbench goes into radio problems
and not into a bug that a laptop could have found.

## A bug in the test that almost became a passed test

The first version of the replay scenario replayed `a.radio->lastFrame`. `lastFrame` is a tap
on the **receive** path — so that was the ACK that A got from B, addressed to A. B would have
discarded it at the destination address check, long before the replay window ever saw it.
The test would have been green and would have shown nothing.

That is why what B actually received is now replayed, and compared against the number of
rejections **before** the attack.
