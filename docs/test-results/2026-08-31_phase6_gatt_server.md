# Phase 6 — GATT server, host-checked

| | |
|---|---|
| Date | 2026-08-31 |
| Node | none — the BLE layer sits behind `hal::IBleTransport` (D3) |
| Environment | Host build in `gcc:14` |

## Result: four gates met in simulation, **none claimed**

`docs/test-plan.md` Phase 6 has nine gates. Four of them depend on protocol logic and not on
the radio, and those have been played through here without a device:

| Gate | Check | Simulation |
|---|---|---|
| 6.3 | Unbonded `PROVISION_KEY` → `ERR_NOT_AUTHORISED`, **key unchanged** | met |
| 6.4 | Unbonded `GET_INFO` / `GET_STATUS` / `GET_BUDGET` → successful | met |
| 6.5 | Chunk a large message and reassemble it, at MTU 23 **and** 247 | met |
| 6.6 | A middle chunk is lost → cleanly discarded, **nothing partially applied** | met |

**Not listed as passed.** 6.1, 6.2, 6.7 and 6.8 need a device with a running GATT server; a
gate list with four ticks and the footnote "against a fake" would be misleading. What is
proven here: the logic is right before anyone spends bench time on it.

**122 test cases** in total (110 before), 406,808 assertions, 0 failures.

## Observations

**6.3 has two halves, and the second is the important one.** "Key unchanged" — an error
that is returned *after* the key has already been written passes a test that only looks at
the response. The check is therefore that the host was never reached at all:
`provisioned.empty()`.

**All seven bonded commands individually.** Not just `PROVISION_KEY`. Each one of them does
not reach the host and still gets a response — §2 says explicitly
"It does not silently no-op".

**An unknown opcode falls into the bonded tier.** `tierOf()` is deliberately conservative: a
command that is added to the protocol list and forgotten in the dispatcher is thereby
**unreachable** instead of unprotected.

**6.6 is met by construction, not by a check.** "Nothing partially applied" can only be
guaranteed if nothing is applied before the whole message is there — so the dispatch runs
after reassembly and never during it. The test additionally checks that the next complete
message still goes through afterwards.

**A bug that only the test found:** the first `tick(0)` did not publish the STATUS
characteristic, because `0 − 0 < 5000`. A phone that reads immediately after connecting —
and that is exactly when it reads — would have seen an empty characteristic for five seconds.
Fixed: the first pass publishes unconditionally.

## What is still missing

`hal/ble_transport_bluefruit.cpp` — the binding to Bluefruit52Lib and the SoftDevice.
Without it nothing runs on the device. It belongs in `hal/`, for the same reason as
`radio_sx1262`: that way everything above it stays checkable on the host.
