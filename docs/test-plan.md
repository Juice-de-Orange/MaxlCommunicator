# Test plan

A phase is complete when its gate passes, on real hardware, with the numbers written down.
"It worked when I tried it" is not a gate. Record results in `docs/test-results/` with the
date, firmware hash and conditions.

Two rules that apply everywhere:

- **Never power the node without an antenna.** It damages the SX1262's PA. This has killed
  more hobby LoRa boards than any software bug.
- Measurements taken over USB are invalid for anything power-related. USB CDC keeps the
  peripheral alive and dominates the reading.

---

## Equipment

| Needed for | Item |
|---|---|
| Everything | 2× T-Echo, 2× 868 antenna |
| Phase 0–1 | Multimeter, SWD probe (J-Link or CMSIS-DAP) |
| Phase 2 | Serial logging on both nodes; ideally a step attenuator or a bag of coax attenuators |
| Phase 5 | Power analyser, or a shunt plus a scope with sufficient dynamic range — µA and mA in the same trace |
| Field tests | A hill, a phone with GPS, and patience |

A step attenuator is the single most useful purchase here. It turns "walk 800 m and hope"
into a repeatable bench test of the adaptive SF logic.

---

## Phase 0 — Hardware ground truth

| # | Test | Pass |
|---|---|---|
| 0.1 | Every pin in `hal/board.h` exercised by a minimal sketch | Each peripheral responds |
| 0.2 | E-paper panel identified, controller confirmed | Reads back SSD1681 ID; a test pattern renders correctly |
| 0.3 | External flash JEDEC ID | Matches ZD25WQ16B; LittleFS mounts, survives 100 write/reboot cycles |
| 0.4 | Flash deep-power-down current | Measured, recorded. If ≫ 12 µA, §3.0's idle budget needs revisiting. **Blocks phase 5** — see below |
| 0.5 | RTC keeps time across reset and across battery-only operation | Drift recorded over 24 h; < 5 s/day |
| 0.6 | Internal flash and RAM budget from the map file | Recorded; ≥ 200 KB flash headroom remains |
| 0.7 | Tickless idle present in the core's FreeRTOS | Answered yes or no, with the fallback identified if no |

**0.4 is a precondition for phase 5, and half of it is now settled.** Sketch 04 saw the
ZD25WQ16B still answer its JEDEC id after the deep-power-down command (`dpd.entered = 0`),
which left two readings open: it never sleeps, or the act of asking wakes it.

**It was the second.** On the evening of 2026-08-31 node A read `0xFFFFFF` before the release
command and `0xBA6015` after it -- the chip had been asleep across several resets, and
`getJEDECID()` is what had been waking it. The part sleeps. What still needs a meter is *how
much that saves*, which is what `CLAUDE.md` §3.0's sub-20 µA `DEEP_IDLE` budget hangs on.
See `docs/decisions/0001-open-decisions.md` D12 and
`docs/test-results/2026-08-31_send_path_never_transmitted.md`.

The consequence of a sleeping chip is larger than a power number: without the store there is
no persistent frame counter, and §2.1 requires a node that cannot persist its counter to
refuse to transmit. A node whose flash was left asleep goes silent for ever, behaving exactly
as specified. `hal::openExternalFlash()` now sends the release command before every open.

0.5 is not a formality. The duty cycle budget is reconstructed against this clock; an RTC
that drifts badly turns a compliance mechanism into a guess.

---

## Phase 1 — Peripheral drivers

| # | Test | Pass |
|---|---|---|
| 1.1 | Partial refresh timing, **of the region a screen actually changes** | < 400 ms measured, 20 consecutive updates |
| 1.2 | Ghosting | After 16 partials, a full refresh clears it completely; judged visually, photographed. **Also decides D14**: whether a screen change may be a partial |
| 1.3 | BME280 sanity | Within 2 °C / 5 % RH / 3 hPa of a reference instrument |
| 1.4 | Button debounce | 500 deliberate presses → 500 events, zero doubles, zero misses |
| 1.5 | Touch button | 200 presses; false-trigger rate recorded (it will not be zero) |
| 1.6 | Battery ADC | Within ±50 mV of a multimeter across 3.3–4.2 V |
| 1.7 | GNSS cold fix | Time-to-fix and current draw recorded outdoors; power-down confirmed to actually cut current |

**1.1's wording changed on 2026-08-31, after it failed once for the wrong reason.** It was
measured by pushing the whole 200x200 panel as a partial window: 471 ms, a fail. That is a
correct measurement of something no screen ever does. Through the region a screen really
changes it is 324 ms, and `hal::Canvas::diffBounds()` is what computes that region. Both
numbers are in `docs/test-results/2026-08-31_phase1_drivers.md`; the gate is about the path
the application takes.

1.7's second half matters more than the first. A GNSS module that stays powered because a
rail was not switched will quietly eat the entire power budget in Phase 5, and you will
blame the radio.

---

## Phase 2 — Radio link layer

The most important gate in the project. Nothing here is optional.

### Functional

| # | Test | Pass |
|---|---|---|
| 2.1 | 100 frames at each of SF7 / SF9 / SF12, bench distance | PER recorded; **zero MIC failures** |
| 2.2 | Bit-flip tamper — flip one bit in payload, then in header | Both rejected, every time, 20 trials each |
| 2.3 | Replay — retransmit a captured frame | Rejected by the replay window, 20 trials |
| 2.4 | ACK path | Every `ACK_REQ` frame acknowledged; RSSI/SNR in the ACK match the receiver's log |
| 2.5 | Retry then give up — power off the peer mid-exchange | Exactly 3 retries, then `undelivered` surfaced. Never silent |

### Counter and budget — the compliance gates

| # | Test | Pass |
|---|---|---|
| 2.6 | 50 forced power cycles, some mid-transmit | Counter never repeats or goes backwards across the whole log |
| 2.7 | Budget saturation — queue 30 messages back to back | Measured airtime never exceeds 360 s in any rolling hour |
| 2.8 | Budget survives reboot — saturate, reboot, immediately attempt TX | Refused; release time correct against the pre-reboot window |
| 2.9 | No-time state — clear the RTC, boot | Transmit fully blocked; `ERR_NO_TIME` returned; recovers after `SET_TIME` |
| 2.10 | Airtime accounting accuracy | Firmware's computed airtime within 5 % of measured on-air time |

2.7 and 2.8 are the tests that make §1.2 true rather than aspirational. Run 2.8 by pulling
the battery, not by a clean reboot — a clean shutdown is the easy case.

### Adaptive SF

| # | Test | Pass |
|---|---|---|
| 2.11 | Escalation — attenuate until 2 consecutive failures | Both nodes reach the same higher SF within 5 frames |
| 2.12 | De-escalation — remove attenuation | Returns to rendezvous SF; no oscillation over 30 min |
| 2.13 | Divergence recovery — force nodes onto different SFs | Both return to the rendezvous configuration within `3 × beaconInterval` |
| 2.14 | No mid-retry SF change | Verified from logs across 50 retry sequences |

2.12's oscillation check is the one people skip. Hysteresis bugs only show up over time.

**2.11-2.14 were never blocked by the second node.** `link::AdaptiveSf` is written and has
fourteen unit tests, and outside its own file it appears only in tests and in the simulation
-- not in `app/`, not in `main.cpp`. `Node::currentSf_` is set once in `attachRadio()` and
never touched again. These four gates need the class wired into the ARQ's outcomes first, and
that is firmware work rather than bench time (2026-08-31).

**2.2 and 2.3 need a bench seam that does not exist yet.** Both require a chosen buffer on
the air -- a valid frame with one bit flipped, and a byte-exact repeat of one already
accepted. `Node::buildFrame` is private and should stay private: it is the only thing between
the application and a frame without a counter. Bring-up 19 therefore covers 2.1 and 2.4 and
says so; the other two are covered in `test/sim/two_node.cpp` and still owe a hardware run.

### Crypto regression

| # | Test | Pass |
|---|---|---|
| 2.15 | Known-answer test — fixed key, nonce, plaintext → expected ciphertext and tag | Runs on device at boot in debug builds; matches published AES-CCM test vectors |

### Range

| # | Test | Record |
|---|---|---|
| 2.16 | PER at 3 distances (bench, ~200 m line of sight, ~1 km obstructed) at SF9 and SF12 | PER, RSSI, SNR, TX power, terrain, weather |

Range results are data, not a pass/fail. Their purpose is to calibrate the adaptive SF
thresholds in `CLAUDE.md` §2.5, which are currently plausible defaults rather than measured
ones. Revisit those numbers after this test and record what you changed.

---

## Phase 3 — Application layer

| # | Test | Pass |
|---|---|---|
| 3.1 | Queue survives reboot with **20 pending and at least one in flight** | All present, order preserved, no duplicates, and **nothing still marked in flight** |
| 3.2 | Queue full behaviour | Oldest-delivered dropped first; with nothing delivered, the oldest undelivered is reduced to its head and marked truncated (D18); refused only when nothing is delivered or undelivered; user-visible; never a crash |
| 3.3 | Journal wraparound | Fill the journal region; oldest acked entries reclaimed correctly |
| 3.4 | Sensor scheduling | Telemetry interval honoured ±10 % over 6 h |

**3.1's wording changed on 2026-08-31, after it passed while the defect it describes was
present.** Its twenty messages were all still `Pending` -- they had never been sent, so none
of them could strand. On real hardware nine entries came back as `InFlight`, which means "the
ARQ holds it", and the ARQ is RAM: after a reset there is no slot, no timer and no attempt
count, and `promoteQueued` only looks at `Pending`. Those nine could reach no outcome at all,
occupied the queue for ever, and every new message was refused with `ERR_QUEUE_FULL`.

A reboot test that only reboots idle state tests the easy case. The gate now requires at
least one entry to have been in flight when the power went.

---

## Phase 4 — UI

| # | Test | Pass |
|---|---|---|
| 4.1 | Idle redraw count over 1 h with no state change | Zero partial refreshes; only scheduled full refreshes |
| 4.2 | All five screens reachable and correct with a peer present and absent, using the D17 button map (short pages, double fires the screen action, long opens the power menu) | Manual walkthrough, screenshots; the double-press gap must feel usable with a thumb |
| 4.3 | Long-press feedback appears before the action fires | Verified on all long-press actions, on the button (the input D17 makes primary) |
| 4.4 | Delivery states distinguishable | `queued until`, `in flight`, `undelivered` all reachable and visibly distinct |

4.1 catches the most common e-paper mistake: a redraw triggered by a value that technically
changed (uptime, RSSI jitter) but that the user does not care about.

---

## Phase 5 — Power

Method: battery only, USB disconnected, release build, GNSS off, default sniff interval,
peer present and beaconing normally.

| # | Measurement | Target |
|---|---|---|
| 5.1 | `DEEP_IDLE` | < 20 µA |
| 5.2 | `SNIFF` average at 2 s / SF9 | ≈ 0.21 mA (§2.3) |
| 5.3 | `ACTIVE` | ≈ 15 mA |
| 5.4 | `GNSS_FIX` | ≈ 40 mA |
| 5.5 | Per-subsystem breakdown | MCU, radio, flash, e-paper, BLE — each attributed |
| 5.6 | **24 h run, extrapolated** | **≤ 2.38 mA average** → two weeks on 800 mAh |

5.6 is the gate. If it fails, the sniff interval is the knob — and `CLAUDE.md` §2.3 gives
what each setting costs in both battery and airtime. Do not fix a power problem by
weakening the duty cycle enforcement.

---

## Phase 6 — BLE and PWA

| # | Test | Pass |
|---|---|---|
| 6.1 | 50 connect/disconnect cycles | Zero lost journal events; no duplicated events after `ACK_QUEUE` |
| 6.2 | Kill the app mid-sync, before `ACK_QUEUE` | Events re-delivered on next connect; nothing lost |
| 6.3 | Unbonded `PROVISION_KEY` | `ERR_NOT_AUTHORISED`, key unchanged |
| 6.4 | Unbonded `GET_INFO` / `GET_STATUS` | Succeed |
| 6.5 | Max-size message chunking (4096 B) | Reassembled correctly; 20 trials |
| 6.6 | Chunk loss — drop a middle chunk | Message discarded cleanly after timeout, no partial application |
| 6.7 | Key rotation | Both nodes reachable throughout the window; old key rejected after close |
| 6.8 | Version mismatch — flash one node with a bumped `ver` | Peer shows `VERSION_MISMATCH` rather than "no peer" |
| 6.9 | Foreground-only behaviour documented in UI | Last-sync time and pending count visible without connecting |

---

## Phase 7 — Dashboard

| # | Test | Pass |
|---|---|---|
| 7.1 | Idempotency — post the same event batch 3× | Row count unchanged |
| 7.2 | Out-of-order arrival | Projections correct regardless of arrival order |
| 7.3 | Config push | `applied_at` set only after `EVT_CONFIG_APPLIED` |
| 7.4 | Budget chart against device-reported budget | Agree within 5 % |

---

## Field tests

Run these before trusting the thing, and after any change to §2.3 or §2.5.

| # | Scenario | Looking for |
|---|---|---|
| F.1 | Both nodes body-worn, 2 km, mixed terrain | Real PER with a human body between antenna and peer — worse than you expect |
| F.2 | Rain or wet foliage | Degradation vs. F.1 |
| F.3 | One node stationary indoors, one moving | Whether adaptive SF tracks a changing link or thrashes |
| F.4 | 48 h carry with normal use | Actual battery consumed vs. Phase 5 extrapolation |
| F.5 | Message sent while budget-blocked | User can tell what is happening and when it will send |

F.5 is a UX test, not a radio test, and it is the one that decides whether the device is
pleasant to use. A radio that silently does nothing for two minutes feels broken even when
it is behaving exactly as designed.
