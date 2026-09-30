# Open decisions and tensions in the specification

Recorded on 2026-08-30 during project setup. `CLAUDE.md` remains normative — this
document collects the places where the specification does not work out on the real hardware,
or does not work out literally, so that they are **decided** rather than silently worked around.

---

## D1 — Gate 0.2 "reads back SSD1681 ID" is probably not achievable as written

`docs/test-plan.md` Gate 0.2 requires: *"Reads back SSD1681 ID; a test pattern renders
correctly."*

**Problem:** The SSD1681 has no classic chip ID register. There is `0x2E` (10-byte
user ID from OTP) and `0x2F` (Status Bit Read), but both need MISO — and on
e-paper FPCs MISO is often not connected at all. In addition, Meshtastic uses an explicitly
`FIXME`-marked dummy on this pin (P1.07); according to LilyGO and cfr34k the real pin is
P1.06, but whether it is electrically connected to the panel is unconfirmed.

**Proposal:** Make Gate 0.2 more precise instead of letting it fail. Passed when:

1. A photo of the FPC label is in `docs/hardware/photos/`, **and**
2. `GxEPD2_154_D67` renders a test pattern geometrically correctly (200×200, no mirroring,
   no offset, clean 1-px lines), **and**
3. Partial refresh works and takes about 0.3 s.

If (2) fails, `GxEPD2_154_GDEY0154D67` (successor panel, also SSD1681) and
`GxEPD2_154` (IL3829, very old batches) are the next candidates.

**Status:** open — to be decided before Phase 1.

---

## D2 — Gate 2.8 assumes an RTC backup supply that may not exist

`docs/test-plan.md` Gate 2.8: *"Budget survives reboot — saturate, reboot, immediately
attempt TX → Refused; release time correct against the pre-reboot window"*, explicitly
by **pulling the battery**, not by a clean reboot.

**Problem:** If the PCF8563 has no backup supply of its own, the time is lost after a
battery change. The device then starts — fully compliant with
`CLAUDE.md` §1.2 — in the state "no valid time, transmission completely blocked" and cannot
compute the release time at all. The gate would be unachievable even though the firmware
behaves exactly right.

**Proposal:** Split Gate 2.8 in two:

- **2.8a** Soft reset: the budget is correctly reconstructed against the RTC, the release time is correct.
- **2.8b** Battery pulled: the device is fully TX-blocked and returns `ERR_NO_TIME`
  until `SET_TIME` or a GNSS fix restores the time.

**Status:** open — depends on the measurement "does the RTC have its own supply?" (Phase 0,
sketch `07_rtc`). To be decided before Phase 2.

---

## D3 — Where `IRadioLink` lives

`CLAUDE.md` §2.6 puts the link layer behind an `IRadioLink` interface, "to keep the
framing code free of RadioLib types". At the same time §3 requires strict layering: a lower
layer never calls into a higher one.

**Tension:** If `IRadioLink` lived in `link/`, the SX1262 implementation in `hal/` would have to
include it — and thereby violate `hal → link`.

**Decision:** The interface lives in `hal/i_radio_link.h`, the implementation in
`hal/radio_sx1262.cpp`. `link/` includes only the interface.

Welcome side effect: RadioLib types **and RadioLib allocations** stay in `hal/`,
i.e. outside the no-alloc check for `link/` and `app/` (`CLAUDE.md` §3). If the
allowlist of that check can stay empty, the layering was cut correctly.

The same pattern applies to `hal/i_block_store.h`, used by the frame counter and the budget ring.

**Status:** decided. Permission matrix:

| from ↓ may include → | `hal` | `link` | `ble` | `ui` | `app` |
|---|---|---|---|---|---|
| `hal` | ✅ | ❌ | ❌ | ❌ | ❌ |
| `link` | ✅ | ✅ | ❌ | ❌ | ❌ |
| `ble` | ✅ | ❌ | ✅ | ❌ | ❌ |
| `ui` | ✅ | ❌ | ❌ | ✅ | ❌ |
| `app` | ✅ | ✅ | ✅ | ✅ | ✅ |

`ble/` accesses the application through a `ble::IHost` interface that `app/`
implements; `ui/` receives a view model from `app/` instead of reaching into the link layer itself.

---

## D4 — AES-128-CCM: tinycrypt

`REVIEW.md` C1 requires "a vetted software implementation" but does not name one.

Checked:

| Candidate | Licence | CCM | 13-byte nonce | 4-byte MIC |
|---|---|---|---|---|
| **tinycrypt** | BSD-3 | ✅ `ccm_mode.c` | ✅ mandatory 13 bytes | ✅ |
| Cifra | CC0 | ✅ | API not verified | API not verified |
| mbedTLS | Apache-2.0 | ✅ | ✅ | ✅ — but considerably heavier |
| `rweather/Crypto` | MIT | ❌ **no CCM** | — | — |

**Decision (provisional, final in Phase 2):** tinycrypt, vendored into the project as source
(`aes_encrypt.c` + `ccm_mode.c`, approx. 15 KB). The 13-byte nonce requirement fits
`CLAUDE.md` §2.1 exactly. The upstream repo has been archived since 2024-03; the maintained one is
`zephyrproject-rtos/tinycrypt`.

Per `REVIEW.md` C1, CryptoCell/CC310 explicitly remains not a requirement.

---

## D5 — GxEPD2 is licensed under GPL-3.0

`CLAUDE.md` §0.1 pins GxEPD2 without a licence note. Version 1.6.9 is **GPL-3.0**, which
extends to a distributed work.

Uncritical for a private repository and personal use. **To be resolved before any
publication.** Alternatives would be an in-house SSD1681 driver (cfr34k's `epaper.c` is MIT and
contains the complete command sequences for full and partial refresh) or the
deliberate choice of GPL-3.0.

The other sources are unproblematic: cfr34k MIT, Meshtastic variant LGPL-2.1,
RadioLib MIT, Adafruit libraries MIT/BSD.

**Status:** resolved 2026-09-30, before publication: the project's sources are MIT; firmware
images link GxEPD2 and are distributed under GPL-3.0; the board variant files stay LGPL-2.1+.
Recorded in `THIRD_PARTY.md`. An in-house SSD1681 driver remains an option if MIT-only binaries are
ever wanted.

---

## D6 — Node 24 instead of the pinned Node 22 LTS

`CLAUDE.md` §0.1 pins Node 22 LTS for Astro 7; the development machine runs Node
24.19.0. Only affects Phase 6/7. Pin it there via `.nvmrc` or Volta.

**Status:** noted, not urgent.

---

## D7 — The frame overhead was stated three different ways in `CLAUDE.md`

Noticed while writing `link/frame.cpp` (2026-08-30). Revision 2 gave three different numbers
in three places:

| Location | Statement | Overhead |
|---|---|---|
| §2.1 byte layout | Bytes 0–11 used, payload from byte 12, MIC 4 B | 16 B |
| §2.1 prose, `docs/protocol.md` | "16-byte header + 4-byte MIC" | 20 B |
| §1.4 | "Frame overhead is 9 header + 4 MIC = 13 bytes" | 13 B |

The field list needs exactly 12 bytes. The airtime table in §1.4, on the other hand, was
arithmetically consistent with 13 B for **all 16 entries** — recomputed with the Semtech ToA
formula, which reproduces every number in the table exactly. Both could not be right.

**Decision:** The field list wins. **12-byte header + 4-byte MIC = 16 B overhead.**
It is the only reading under which every field described in §2.1 is preserved; "16-byte
header" was a slipped label for the total overhead, "9 header" in §1.4 was outdated.

Subsequently corrected: the airtime and lockout table in §1.4, the columns
"TX airtime" and "msgs/h" in §2.3, the sentence in §2.1 and the line in `docs/protocol.md`.

**The wire format itself has not changed** — it was only described wrongly. `ver`
therefore stays at 1 (`versioning-and-updates.md` §1: the nibble is bumped for changes to the
header layout, not for corrections of its description).

What it costs compared to the previously printed numbers:

| | before (13 B) | now (16 B) |
|---|---|---|
| POSITION @ SF9 | 206 ms | 226 ms |
| POSITION @ SF12 | 1483 ms | 1647 ms |
| g1 lockout SF12 | 146.8 s | 163.0 s |
| Messages/h SF9, g3, 2 s | 165 | 163 |

`REVIEW.md` A1 carries the same table with the old numbers. It stays there unchanged
— the document is a dated review and not normative text; changing it retroactively
would falsify the record. A reference to this entry sits next to it.

**Status:** decided on 2026-08-30, applied in `CLAUDE.md`.

---

## D8 — Host tests run in Docker against the same sources

`firmware/test/README.md` stated: "No host gcc on the dev machine — these run in CI or
Docker." That is now implemented.

- `firmware/test/Makefile` compiles the **same** `.cpp` files from `src/link/` and `src/ble/`
  as the ARM build — no copy, no stub.
- `firmware/tools/hosttest.sh` starts `gcc:14` with the repo as a bind mount.
- Test framework: **doctest** (one header, MIT, under `firmware/test/vendor/`) instead of Unity.
  The failure output names the actual and expected value, and that is exactly what matters for
  the airtime and CCM vector tests, where the number is the finding.

**Make instead of CMake.** The first draft was CMake; but the `gcc:14` image does not ship
CMake, and building a custom image for it would have added a dependency and a build step
to replace thirty lines of Makefile. `make`, `g++` and `python3` are present in the
standard image, so it runs unchanged there **and** on a bare CI runner.

**Shared test vectors.** `test-vectors/*.json` is read by firmware and bridge —
by the C++ side via `tools/gen_vectors.py` as a header of byte arrays, by the
TypeScript side directly. Written by hand from the specification, **not** generated from either
implementation: a generator would bake in one side's bugs and make the
cross-check worthless. Cross-check performed — a corrupted byte makes both
test suites fail.

The side effect is the actual point: if `link/` passes on a bare host toolchain
without the Arduino core, the cut from D3 is **demonstrated** rather than asserted.

**Status:** implemented.

---

## D9 — Rolling hourly total **and** per-frame lockout

Noticed while writing `link/budget.cpp` (2026-08-30). `CLAUDE.md` described two
different blocking mechanisms without saying which one applies:

| Location | Mechanism |
|---|---|
| §1.2 | "maintains a **rolling 60-minute airtime total** per sub-band … MUST refuse to transmit when the budget is exhausted" |
| §1.4 (lockout table), `REVIEW.md` A3 | "a single SF12 frame **locks the 1 % band for ~147 s**" |

The two are not the same. Under a pure hourly total, transmitting again immediately after a
POSITION frame is allowed, and the lockout table would simply be wrong.
Conversely, `REVIEW.md` A3's finding would never occur — the retry timer (`2 × airtime + 300 ms`)
would always fire before the budget, and the budget-aware scheduler from §2.4 would have no
reason to exist.

**Decision: both apply, the later point in time wins.**

```
earliestLegalTx(band, airtime) =
    max( when the hourly total has room again,
         lastTX_end + airtime × (1/dutyCycle − 1) )
```

Rationale:

- It is the only reading under which §1.2 **and** the §1.4 table are true at the same time.
- It is strictly more conservative than either one alone, so it is never non-compliant when either
  one would be compliant.
- The lockout prevents the hourly total from being spent as one burst: at SF9 in g3,
  163 frames back to back followed by 54 minutes of radio silence would be compliant and
  worthless as a communication device.
- The hourly total is needed because `EVT_BUDGET` (`docs/bridge-protocol.md` §4) and the
  `STATUS` screen (§3.3) must report `usedMs`/`limitMs`. A pure lockout would have no
  answer to that.

And it makes `REVIEW.md` A3 real: in **every** computed configuration the lockout wins
against the retry timer.

| | Retry timer | Lockout | |
|---|---|---|---|
| SF9, g3 | 753 ms | 2037 ms | budget wins |
| SF12, g3 | 3593 ms | 14819 ms | budget wins |
| SF12, g1 | 3593 ms | 163013 ms | budget wins |

### Resolution, and why it is computed twice

The lockout is tracked **twice**, and that is intentional:

- **During operation** in monotonic milliseconds — exact. At SF7 in g3 it is 0.6 s, and whole
  seconds cannot express that.
- **Across a reboot** from the persisted record, i.e. in the one-second resolution
  of the PCF8563, **rounded up**. Rounding up waits marginally too long; that is the safe
  direction. Rounding down would transmit marginally too early, and that is exactly what this module is meant to prevent.

Without the second form, a node could cut short a 163-second lockout in g1 with a reboot
— the same hole that `REVIEW.md` A4 found in the hourly total.

**Status:** decided on 2026-08-30, implemented in `link/budget.cpp`, brought into line in `CLAUDE.md` §1.2
and §1.4.

---

## D10 — The journal counter is not the frame counter

Noticed while building the test data for the ingestion endpoint (2026-08-31). Two places
in the specification are mutually exclusive:

| Location | Statement |
|---|---|
| `docs/bridge-protocol.md` §4 | "State `2` may be followed later by `0` or `1` **for the same counter** — the phone and the server must treat the journal as a log of state transitions" |
| `CLAUDE.md` §4.3 | "`events` carries **`UNIQUE (device_id, frame_counter, direction)`**" |

A frame that first reports `queued` and then `delivered` produces two journal entries with
the same counter and the same direction. Under the unique index, the second one is a
duplicate and is discarded — **the state change that matters would never arrive.** Precisely
the case that §4 explicitly requires would be the only one the idempotency throws away.

**Decision: the journal has its own counter.** The frame counter is in the body.

This is not an invention; it is already in the layout. `EVT_FRAME_TX_RESULT` is
`counter:u32, dst:u16, seq:u8, result:u8, attempts:u8, rssi:i16, snr:i8` — the body carries
the frame's counter **as a field**. If the journal key were the same value, that
field would be redundant. It is there because the entry has a different key from the frame
it reports on.

Both statements thereby remain true:

- A frame has exactly one counter, monotonic use, never reused (`CLAUDE.md` §2.1).
  It identifies **the frame** and is the body value in the TX/RX events.
- A journal entry has exactly one counter from the same monotonic supply. It
  identifies **the entry** and is the idempotency key.

Cost: counters are consumed faster — up to three entries per transmitted frame
(`queued`, then `delivered` or `undelivered`). At 32 bits and the
hourly quota from §1.2 (at most 163 frames per hour and node in g3) the
supply is exhausted after

```
2^32 / (163 * 3) ≈ 8.8 million hours ≈ 1000 years
```

The 256-block reservation from §2.1 burns more anyway. Not a problem.

Implemented:

- `CLAUDE.md` §4.3 and `docs/bridge-protocol.md` §3 now state explicitly which
  counter is meant.
- The column is called `events.journal_counter`, not `frame_counter` — the old name was the
  cause of the confusion.
- The projections key messages by the **frame** counter from the body, so that
  `queued` and `delivered` collapse into the same message.

**What is still open:** outgoing messages have no text on the server side.
`EVT_FRAME_TX_RESULT` carries counter and radio values, no content. The text is in the
**receiver's** `EVT_FRAME_RX`; as soon as both nodes sync, it could be matched via
`(src, counter)`. Until then the dashboard shows outgoing messages with
delivery state but without content. Not solved, deliberately not invented.

**Status:** decided on 2026-08-31, at night, alone — **please review.**


---

## D11 — `hdop` never had a scale

`CLAUDE.md` §2.2 defines the `POSITION` payload:

```
lat int32 (deg × 1e7), lon int32, alt int16 (m), hdop uint8, fixAge uint8
```

Four of the five fields carry their unit in their name. **`hdop uint8` carries none.**
`link/payloads.h` passed the byte through without interpreting it — which was right as long as
nobody filled it. With `hal/nmea` there is now a source, and it has to
decide.

**The problem:** HDOP is a dimensionless floating-point number that in practice lies between
0.5 and about 20. Rounded to an integer, every usable fix would be a `1` and every
poor one a `2`. That is not information, but a field that looks as if it carried
some.

**Decision: tenths, capped at 255.**

| Raw value | Byte | Meaning |
|---|---|---|
| 0.9 | 9 | very good fix |
| 1.26 | 13 | usable |
| 25.5 and worse | 255 | unusable |
| unknown | 255 | unknown, see below |

Three reasons:

1. **Resolution where it matters.** The interesting range is 0.5 to 5. In tenths
   that is 45 distinguishable steps instead of 5.
2. **The headroom is sufficient.** HDOP above 25 is unusable by any standard; above the
   cap, no statement is lost that anyone would make.
3. **Unknown is 255, not 0.** A missing or unparseable field must appear as the
   *worst* value. With 0, a missing value would read on the
   `POSITION` screen as a perfect fix — exactly the wrong way round.

**No `ver` bump.** The byte layout does not change, nor the field width, nor the position.
What is defined is a meaning that was missing before — the same case as the
`EVT_STATUS` body in the session of 2026-08-31. `docs/protocol.md` records it,
`CLAUDE.md` §2.2 remains literally valid.

**Status:** decided on 2026-08-31, alone — **please review.** Reversible as long as
no second node with the other reading is in the field; with two nodes they are flashed in the
same session anyway (`CLAUDE.md` §6).

---

## D12 — The external flash does not enter deep power-down

**Measured, not assumed.** Bring-up sketch 04, node A, 2026-08-31, after 100 of 100
reboot cycles:

```
RESULT dpd.jedec_while_asleep = 0xBA6015
RESULT dpd.jedec_after_wake   = 0xBA6015
RESULT dpd.entered            = 0
RESULT dpd.woke               = 1
```

After `0xB9` (Deep Power Down) the ZD25WQ16B **still** answers with its JEDEC ID.
It should have stayed silent.

**Why this matters:** `CLAUDE.md` §3.0 says *"The external flash **must** be put into deep
power-down whenever it is idle. Its standby current is around 12 µA — on its own that is
most of the `DEEP_IDLE` budget."* The budget for `DEEP_IDLE` is **< 20 µA** (§3.1). If
the chip does not sleep, more than half of that budget is gone before the MCU has even
been counted.

**What is not yet established:** whether the chip does not sleep, or whether the measurement wakes it.
Two explanations are open, and the second is the more likely one:

1. `Adafruit_FlashTransport_QSPI::runCommand(0xB9)` does not reach the chip in QSPI mode
   the way a single SPI command would.
2. `getJEDECID()` itself wakes it. Many SPI NOR chips leave deep power-down on
   **every** command, not only on `0xAB` — in that case the test measures its own wake-up call and
   the chip did sleep after all.

From within the firmware the two cannot be distinguished. **The ammeter
decides, and that is exactly what Gate 0.4 is for.**

**Procedure until then:** The call stays in — it does no harm and is correct if
explanation 2 holds. `docs/test-plan.md` Gate 0.4 becomes a **blocking** prerequisite
for Phase 5: if the measurement comes out above 12 µA, §3.0's assumption is wrong and the
`DEEP_IDLE` budget must be recomputed, not the sketch adjusted.

### Resolved on 2026-08-31, evening — and against this entry's assumption

**The chip does sleep.** Node A, sketch 10 with a wake-up command in front:

```
RESULT flash.jedec_before_wake = 0xFFFFFF
RESULT flash.jedec             = 0xBA6015     (after 0xAB)
RESULT littlefs.mounted        = 1
```

`0xFFFFFF` before waking and the real ID afterwards is the signature of a chip in
deep power-down, and nothing else produces this pair. Of the two explanations above it was
**the second**: `getJEDECID()` woke it itself, and that is why sketch 04 saw it answer.

**What made this expensive.** The ZD25WQ16B has no reset line, so a deep power-down survives
every MCU reset. Without flash there is no persistent frame counter, and `CLAUDE.md`
§2.1 then requires refusing to transmit. A node whose flash has been put to sleep once
is therefore mute for ever — fully compliant with the specification, without any report
saying so. That is exactly what node A was for half the evening.

Fixed in `hal::openExternalFlash()`: wake-up command, then open, in one place instead of
six. The return value `jedecBeforeWake` is kept because it is the only evidence that
distinguishes a sleeping chip from a missing one.

**What remains open:** how much sleeping saves. That is the measurement with the ammeter,
and Phase 5 still depends on it.

**Status:** cause identified and fixed; the current measurement for Gate 0.4 is pending — needs
someone at the bench with a multimeter.

---

## D13 — On the cable, the battery ADC does not measure the cell

**Measured.** Bring-up sketch 05, node A, 2026-08-31, USB plugged in:

```
RESULT adc.raw          = 3281        (12 bit, internal reference 3.0 V)
RESULT adc.at_pin_mv    = 2403.7
RESULT battery.mv       = 4807        (× divider 2:1)
RESULT usb.vbus_present = 1
VERDICT fail -- reading is outside any voltage a running lithium cell can have
```

**4807 mV is not a lithium cell.** A single cell lies between 3.0 and 4.2 V; above
4.4 V there is no operating state in which it would read that.

**The reference is not to blame.** According to the core, `AR_INTERNAL_3_0` is 0.6 V × 5 = 3.0 V
full scale, `AR_INTERNAL` 0.6 V × 6 = 3.6 V. Both configurations compute the same 4807 mV from the same
pin voltage — only the raw number differs. The pin really is at 2.404 V.

**Most likely explanation: the divider is not connected to the cell but to the latch node
that VBUS feeds through D5.** 4807 mV is 5.0 V minus about 0.19 V — a Schottky forward drop.
Exactly the same structure already explains why the PCF8563 keeps answering when `PWROFF`
pulls `PIN_PWR_ON` LOW (see **D2** and `docs/hardware/pinmap.md` §3). Two
independent findings, one cause.

The alternative — the divider is not 2:1 — fits worse: with a cell at 4.15 V
the ratio would be 1.727, and there is no common resistor pair for that.

**Consequences:**

1. **Gate 1.6 cannot be carried out on the cable.** It requires ±50 mV against a multimeter
   over 3.3–4.2 V. As long as USB is plugged in, the measured node is not the cell, and a
   calibration against the cell voltage would have to fail.
2. **The same run without the cable answers D2 and D13 together.** Both need exactly that:
   device on battery, no USB. That is one bench session, not two.
3. **`hal::BatteryAdc` adopts `AR_INTERNAL_3_0`**, not `AR_INTERNAL`. Not because of
   resolution — at ±50 mV tolerance both suffice —, but because that is the configuration
   for which there is a measurement on this board. At 2.6 V on the pin (VBUS at 5.25 V) it stays
   in range.
4. **`pinmap.md` §6 question 4** ("battery divider permanently on VBAT or switched?") is
   thereby **half answered**: it is not permanently on VBAT, at least not on VBAT alone.

**Until then the reading counts as unusable, not as the cell voltage.** The
`TELEMETRY` field `battery uint16 (mV)` and the `LOW_BATT` flag from §2.1 depend on it; a
device on the cable reports too high, a device in the field — without cable, the only state that
counts — probably correctly. "Probably" is explicitly marked as such here.

**Status:** open — needs a run without the USB cable and a multimeter.

---

## D14 — The full refresh takes 4.4 s, not 2 s

`CLAUDE.md` §1.6: *"Full refresh ~2 s, partial ~0.3 s."*

**Measured on node A, 2026-08-31, twice independently:**

| | Specification | Sketch 08 | Sketch 11 |
|---|---|---|---|
| Full refresh | ~2000 ms | **4403 ms** | **4410 ms** |
| Partial refresh | ~300 ms | 323 ms | 324 ms |

The partial value is right. The full value is **more than double** the assumed one.
Two measurements from different sketches, 7 ms apart — that is not noise.

**Why this is not just a number.** `CLAUDE.md` §1.6 requires a full refresh
*"after 16 partials or on screen change"*, and §3.2 assigns "next screen" to the short touch.
Together that means: **every press on the touch pad costs 4.4 seconds**, during which the
panel runs through the well-known e-paper inversions. Once through all five screens is
22 seconds. That is a device people will think is broken in the field.

**Two options, and the second is not free:**

| | Behaviour | Screen change | Ghosting |
|---|---|---|---|
| **A — as specified** | screen change = full | 4.4 s | none, ever |
| B — screen change = partial | full only after 16 partials | 0.47 s | possible for up to 16 changes |

Option B treats the 16-count as the *only* ghosting limit and relies on
16 partials still looking clean even with completely different images. Whether that
holds is exactly what **Gate 1.2** answers by looking — and that has not
happened yet.

**A is implemented, i.e. the specification.** Not because it is better, but because the
measurement that would have to justify B is still pending. Changing a behaviour whose
justification would be an untested "that'll do" is the wrong order.

**What has to be decided, and when:** after Gate 1.2. If the panel looks clean after 16 partials with
changing screens, B is by far the better user experience and `CLAUDE.md` §1.6
would have to be changed accordingly. If it does not look clean, A stays and the 4.4 s is the
price of the hardware — then the number belongs in §1.6, where 2 s stands today.

Either way: **§1.6's "~2 s" is wrong on this panel** and needs correcting, regardless
of which option is chosen.

**Status:** open — depends on Gate 1.2, and that needs eyes.

---

## D15 — The journal counter never leaves the device, and the bridge throws away six of seven event types

Found on 2026-08-31 while writing the third client (Kotlin) against
`docs/bridge-protocol.md`. **That is exactly why you write a third client.**

### The finding, in three parts

**1. The journal counter is computed and then thrown away.**

`app::Node::fetchQueue` fills `ble::QueuedEvent.counter` with the entry's journal counter.
`ble::GattServer` then sends:

```cpp
emitEvent(static_cast<EventCode>(events[i].opcode), events[i].body, events[i].length);
```

`events[i].counter` does not appear **a single time** in `gatt_server.cpp`. The counter never reaches
the phone.

**2. So no client can form `ACK_QUEUE` correctly.**

According to §3, `GET_QUEUE(sinceCounter)` and `ACK_QUEUE(upToCounter)` are journal counters. A
client that does not know them cannot form one.

What is in the event bodies is something else: `EVT_FRAME_RX` and
`EVT_FRAME_TX_RESULT` carry the **frame** counter — and **D10** insists explicitly
that this is not the same thing. `EVT_STATUS`, `EVT_BUDGET`, `EVT_FIX`, `EVT_LOG` and
`EVT_CONFIG_APPLIED` carry none at all.

That it happens to match for `EVT_FRAME_RX` is a property of the implementation:
the body carries `counter_.peek()`, and `journalEvent()` hands out exactly that value with
`counter_.next()` right afterwards. A coincidence, not a design.

**3. The PWA stores only `frame-rx` — and acknowledges everything anyway.**

`bridge/src/sync/session.ts`:

```ts
const event = decodeEvent(message);
if (event.kind === "frame-rx") {
  this.#inbox.push({ counter: event.counter, ... });
}
```

Only `frame-rx` ends up in storage. `EVT_FRAME_TX_RESULT`, `EVT_STATUS`, `EVT_BUDGET`,
`EVT_FIX`, `EVT_CONFIG_APPLIED` and `EVT_LOG` go to listeners and **are never stored,
never pushed to the server and never seen again**.

And then:

```ts
const highest = pending[pending.length - 1]!.counter;
await this.#request(Opcode.AckQueue, encodeAckQueue(highest));
```

`ackQueue` releases **everything** on the device up to this counter — including the entries that
the phone never stored. **After that they are gone for good.**

### Why this is worse than a missing field

`docs/bridge-protocol.md` §4 says about `EVT_FRAME_TX_RESULT`:

> State `2` may be followed later by `0` or `1` for the same counter — the phone and the
> server must treat the journal as a log of state transitions, not as a set of final
> outcomes.

**D10** exists for that reason alone: keying the server's idempotency on the frame counter
would "discard exactly the state transitions it is supposed to carry". We
solved the key question cleanly — and throw the transitions away anyway, one layer
further up.

Concretely: the delivery state in the dashboard (`queued` → `delivered`/`undelivered`) can
**never** arrive. Nor can the budget histories, the fixes and the
configuration acknowledgements that `config_versions.applied_at` depends on.

### What needs to be decided

Three options, and none is obvious enough that I would pick it alone.

| | How | Cost |
|---|---|---|
| **A** | The `GET_QUEUE` response additionally carries `highestCounter:u32` | **additive**, an old client keeps reading only byte 0. But the server needs a counter **per entry** for its idempotency key `(device_id, journal_counter, direction)` — A does not solve that. |
| **B** | New event code `EVT_JOURNAL` = `counter:u32, opcode:u8, len:u8, body[len]`, which `GET_QUEUE` sends instead of the bare events | clean and explicit: spontaneous events stay as they are. But a client that does not know it gets nothing at all from `GET_QUEUE` — a break for the PWA. |
| **C** | Prefix every event body with a journal counter | uniform, but changes **every** event type and thus `BRIDGE_PROTO`. |

**My proposal is B**, because it names the distinction that §3 makes anyway: a
spontaneous event and one re-delivered from the journal are not the same. And because the
PWA has to be touched anyway — it is the second part of this finding.

### Decided on 2026-08-31: option B

Option B was chosen. `EVT_JOURNAL` = `0x88`, body `counter:u32, opcode:u8, len:u8, body[len]`.
`GET_QUEUE` delivers it instead of the bare events; spontaneous events stay unchanged.
`BRIDGE_PROTO` goes to **2**.

During implementation a rule was added that nobody had written down before and without which B
does not hold: **the two copies of an event mean different things.**

| | Spontaneous | From `GET_QUEUE` |
|---|---|---|
| Form | bare, `txnId = 0` | in `EVT_JOURNAL` |
| Counter | none | yes |
| What for | the display, while connected | the durable entry |
| Client stores | **no** | **yes** |

A spontaneous event is a courtesy, not a delivery. Storing it without a counter
was the old stopgap — the synthetic key from `0x4000_0000` — and exactly that goes
away with this, in both clients.

**This implies a binding obligation for the device: every event in the table in §4 is
journalled.** Otherwise it is an event that the phone only catches if it is connected at the same
moment — and `CLAUDE.md` §4.2 builds the product around a phone that
is usually *not* connected. `EVT_CONFIG_APPLIED` is the sharpest case:
`config_versions.applied_at` depends on it alone. The only exception is `EVT_LOG`, which exists only
in debug builds.

**Implementation status, 2026-08-31 evening:**

| Part | Status |
|---|---|
| `docs/bridge-protocol.md` §3, §4, §5, §7 | **done**, `BRIDGE_PROTO = 2` |
| `test-vectors/bridge_protocol.json` | **done** — two new vectors, by hand, one with an unknown body opcode |
| PWA (`bridge/`) | **done** — `EVT_JOURNAL` unpacked, synthetic key removed, `ACK_QUEUE` back on. 92 tests green (previously 88) |
| Kotlin (`android/`) | **done** — the same. 81 tests green (previously 74) |
| Firmware (`src/ble/gatt_server.cpp`) | **open** — `events[i].counter` is still lost there |
| Firmware: journal the other five event types | **open** — today `app/node.cpp` journals only `EVT_FRAME_RX` and `EVT_FRAME_TX_RESULT` |

The two open rows are the reason why the device reports `BRIDGE_PROTO = 1` until then
and both clients consistently reject it. That is the right order — a
client that serves a device with the old `GET_QUEUE` because it does not check the version is
exactly the bug that §5 step 2 prevents.

**To be corrected regardless of the option:** the PWA must store **all** event types, not
only `frame-rx`. That is not a design question, that is a bug.

> **Done on 2026-08-31.** `bridge/src/sync/session.ts` now stores every event, and
> `MockDevice` can produce journals that do not consist only of `EVT_FRAME_RX` — before that
> no test could have shown the bug. A new test pins it down.
>
> And it no longer sends **any `ACK_QUEUE`**, for the same reason as the Kotlin client. The
> test that pinned down the old order has been rewritten and names itself as
> the assertion to be reversed when D15 is resolved.

### Until then

**Both** clients store every event and send **no** `ACK_QUEUE` as long as they cannot
assign a journal counter to every stored entry. They use the same
synthetic base `0x40000000` for events without a counter — two clients that guess
differently would be worse than two that guess the same. The journal then fills
up and `lostEntries()` counts along — but nothing the phone has ever seen is
lost. §3 says it itself: better to send twice.

**Status (superseded, see addendum):** option decided (B), document and both clients implemented, **firmware open** —
see the table above.

---

**Addendum 2026-09-01:** The firmware half is complete. `BRIDGE_PROTO = 2` and
`EVT_JOURNAL` already came with commit `34c22ac` (evening of 2026-08-31; the table above lagged
behind the code); the firmware has produced the remaining event types since the night session:
`EVT_BUDGET` after every `recordTransmission` ("emitted when the budget changes", §4 verbatim),
`EVT_STATUS` on changes of the flags byte (rationale below as a separate point), `EVT_FIX`
once per fix from `main.cpp`. Two conformance bugs in the bodies were fixed along the way:
`EVT_FRAME_RX` carried the node's own `counter_.peek()` instead of the received counter, and
`EVT_FRAME_TX_RESULT` carried the local queue ID instead of the frame counter — both made the
dashboard's (src, counter) correlation meaningless. The frame counter in TX_RESULT is
that of the **first** transmission and stays stable across the queued/delivered/undelivered transitions
(exactly the "state 2 may be followed by 0 or 1 for the same counter" from §4); since retries are
rebuilt under a fresh counter, the correlation misses a message delivered by a retry
— known, documented, unresolved like the server-side text of outgoing messages (D10).

**One emission rule had to be fixed during the night because §4 does not name it:** When is
`EVT_STATUS` produced? The body carries battery, uptime, queue depth — values that move
constantly; journalling per tick or per timer would mean flooding the 128-entry journal with battery noise
(every append is a full region rewrite). Chosen conservatively: **it is journalled
on a change of the flags byte** — timeValid, keyProvisioned, gnssPowered —, i.e. on the
state transitions that are history. A client that wants the current values reads
`GET_STATUS` (§5 step 3), as the protocol provides for anyway. Reversible; if an
interval is wanted, it is a one-liner in `Node::tick()`.

## D16 — Which rejection keeps a pending message, and which discards it

Decided alone on 2026-08-31, **please review.** Implemented in both clients and
normative in `docs/bridge-protocol.md` §5.

### Why the question exists at all

`CLAUDE.md` §4.2 builds the product around the foreground restriction: the device queues
everything, the user opens the app to sync. That covers one direction. In the
other there is **no device that could queue anything** — the phone is the only
place where a written message can wait.

The PWA could not do that: the send button was only visible while a connection was up. You
had to hold the node in your hand to write to it. The `outbox` parameter in `run()`
existed, but nobody ever filled it. It was noticed because the Kotlin client had an outbox
from the start — the same mechanism as with D15: two implementations
see what one does not.

That raises the question the protocol had not answered: **what happens to
a pending message that the node rejects?**

### The decision

Not *which* error it is, but **what it is about**:

| Rejection | What it is about | Consequence |
|---|---|---|
| `ERR_BAD_LENGTH`, `ERR_BAD_PARAM`, `ERR_UNSUPPORTED` | the **message** | discarded, reason reported |
| everything else | the **state of the node** | stays pending, next connection offers it again |

Rationale for each side. The same bytes get the same answer for ever — keeping a message
that the node rejects for its own defects costs a write on every sync until someone
notices. State, on the other hand, changes: at 10 % duty cycle `ERR_BUDGET_EXHAUSTED` is
the **normal** answer and not a fault, `ERR_NO_TIME` is cleared by step 4
of the same flow, `ERR_NOT_AUTHORISED` and `ERR_NO_KEY` are cleared by bonding and
provisioning.

The short list is deliberately the one that discards. An error code the device
gains later is far more likely to be a state than a verdict on exactly these bytes — and
guessing wrong costs a retry in this direction, the message in the other.

**A rejection does not abort the connection.** At step 7 the journal has long been
fetched and pushed to the server; throwing that away because the node gave a valid
answer would mean fetching it again next time. A *transport error* is
something else and is passed through: then the connection is gone, the remaining
messages were not rejected by anyone and stay pending, untouched.

**Attempts are counted but never used as a reason to discard anything.** The PWA says
from the third time on that a message was rejected repeatedly — not from the first,
because one or two rejections at 10 % are the ordinary course of things and a notice
every time teaches the user to ignore it. An upper limit that discards would be
the same data loss, only slower.

### What I got wrong in between

The first version classified `ERR_NO_TIME` as final and discarded the message. Wrong:
an invalid clock is a state that `SET_TIME` fixes — and the loss would have occurred
most often with a freshly unpacked node, i.e. exactly when nobody is
paying attention. Corrected one hour after the commit, with tests on both sides.

**Status:** decided and implemented — **please review.** An objection changes one
constant per client and two sentences in the protocol document, not a wire format bump.

---

## D17 — The touch pad cannot be triggered on this device, and it carries the navigation

Documented on 2026-08-31: `docs/test-results/2026-08-31_touch_not_reachable.md`. The TTP223 is
fitted, powered and drives its output, but it does not trigger through the closed housing
— across several test series, with LED feedback on the pin and following the
operating technique from the datasheet.

**Why this is more than a missing gate.** `CLAUDE.md` §3.2:

| Input | Action |
|---|---|
| Touch, short | **Next screen** |
| Touch, long | Screen's primary action |
| Button, short | Wake / refresh |
| Button, long | Power menu |

The touch pad thus carries the entire navigation. Without it the five screens cannot be
paged through on the device. §1.7 saves the functionality — interaction must never be required, everything
goes over BLE — but a field device whose display cannot be switched is not what
§3.3 describes.

**Three options, and none is obvious:**

| | How | Cost |
|---|---|---|
| **A** | Open the housing and inspect the coupling | Answers whether it is this device or the design. Not wanted for now, and it answers nothing if both devices are affected |
| **B** | Rework the button map: short button press pages, long press opens the power menu, the screen action moves to a double press | Works without touch. But §3.2 explicitly forbids double presses — "the touch button is not reliable enough for timing-sensitive input" —, and the reason does not apply to the push button, so the rule would be made more precise rather than broken |
| **C** | Screens change only via BLE and automatically (context switch on events) | No intervention in the hardware assumptions, but a device you hold in your hand and cannot switch |

**My proposal is B**, because it keeps on-device interaction without inventing anything: the
user button is demonstrably clean (734 edges for 367 presses, no bounce), and §3.2's
ban on double presses is explicitly justified by the unreliability of the **touch** input,
not of the push button.

### Decided on 2026-08-31: option B — the push button takes over

| Input | New | Previously (§3.2) |
|---|---|---|
| Button, short | **next screen** | wake / refresh |
| Button, double | **screen's primary action** | — (explicitly forbidden) |
| Button, long (> 2 s) | power menu | power menu |
| Touch, short/long | — (not operable) | next screen / primary action |

**Why this does not break §3.2 but makes it more precise.** The sentence there reads: *"No chords, no
double-taps — the touch button is not reliable enough for timing-sensitive input."* The
justification explicitly names the **touch** input. It does not apply to the push button, and we
have the counter-measurement: 734 edges for 367 presses, exactly 2:1, no double triggering, no
bounce. The double press thus lands on the input whose reliability is documented, and
the ban stays in place for the one it was written for.

**Waking loses nothing.** A short press wakes the device anyway; it then pages.
A screen change is a refresh, so the old meaning coincides with the new one
instead of giving way to it.

**To implement:** `ui/ui.cpp` (button map and double-press detection), `hal::PressDetector`
(second edge within a window, host-testable), `CLAUDE.md` §3.2, and gates 4.2
and 4.3 in `docs/test-plan.md`. The touch handling stays in the code — it costs nothing and
works as soon as a device turns up on which the pad responds.

**Status:** decided, **not yet implemented.**

---

## D18 — Undeliverable messages wall up the outbox

Measured on 2026-08-31 on node A, after the send path ran for the first time:

```
RESULT queue.depth        = 24
RESULT queue.capacity     = 24
RESULT queue.pending      = 0
RESULT queue.in_flight    = 1
RESULT queue.delivered    = 0
RESULT queue.undelivered  = 23
```

The node tried 23 messages three times, gave up and keeps them. `reclaimDelivered()`
discards only `Delivered`. As a result this node **never again** accepts a message
— every further one gets `ERR_QUEUE_FULL` — until a phone collects the old ones.

**This is compatible with the specification and wrong nonetheless.** `message_queue.h` says of
`Undelivered`: *"Kept until the user sees it"*, and `CLAUDE.md` §2.4 explicitly requires that
a failed delivery becomes visible instead of silently disappearing. Both are right. But
the result is a field device that walls itself up after 24 unsuccessful messages — and
§4.2 builds the product precisely around the assumption that the phone is **mostly not there**.

The frequency is not exotic: it occurs exactly when the other end does not answer for a longer
time, i.e. in a dead zone — the case the device is built for.

| | How | Cost |
|---|---|---|
| **A** | `Undelivered` becomes evictable like `Delivered`, oldest first, as soon as the queue is full | The user loses the message *and* the notice that it failed. Exactly the silent disappearance that §2.4 forbids |
| **B** | A separate small area for undeliverables — the queue stays free, the failures stay visible until the area itself is full | A second storage area and a second ring. Honest, but it only moves the limit |
| **C** | On eviction the **header** is kept (destination, time, reason), the text goes | The user still sees that and why something failed, and only loses the content. Costs ~10 instead of 62 bytes per entry |

**My proposal is C.** §2.4 requires visibility of the failure, not retention of the
text; the header carries everything that the `MESSAGES` screen and the dashboard display. And a
device that keeps accepting is worth more in a dead zone than one that keeps the old texts.

### Decided on 2026-08-31: option C — the header stays, the text goes

If the queue is full and no `Delivered` entry is available for eviction, the **oldest
undeliverable entry is reduced to its header**: destination, time, attempts and reason stay, the
up to 48 bytes of text are discarded and the entry is marked as *truncated*. Only when even
that is no longer enough is the message rejected.

**Why this complies with §2.4.** What is required is that a failure becomes *visible* — "marked
undelivered and surfaced in the UI", not "silently dropped". The header is what is visible: the
`MESSAGES` screen shows destination and state, the dashboard projects from the same fields. The
text is the only thing that is lost, and it is the only thing the user wrote
themselves and can recreate.

**Why anything has to give at all.** §4.2 builds the product around a phone that is mostly
not there. A node that permanently accepts nothing after 24 unsuccessful messages
is least usable in a dead zone — i.e. exactly where it is built for.

**To implement:** `app::MessageQueue` (a `truncated` bit in the entry, eviction rule in
`submit()`), host tests for it, `ui/screens` shows truncated entries as such, and
`docs/bridge-protocol.md`, if the state is to become visible across the bridge.

**Status:** decided; **implemented on 2026-09-01** (night session), with three
implementation decisions that the resolution left open:

1. **The headers live on a separate stub ring** (`UndeliveredStub`, 8 bytes: dst, seq,
   attempts, time; 16 slots), not as marked entries in the fixed 62-byte grid — there
   a truncated entry would have kept its slot and freed nothing. That is the
   reading the resolution's "~10 instead of 62 bytes" leads to.
2. **If the ring also overflows, the oldest header drops out and `stubsDropped()` counts it.**
   That is the only place where the reduction really loses information; the
   alternative — rejecting again at 16 headers — would only have moved the wall that C
   was meant to tear down. If that should be different: the rule is in one place
   (`MessageQueue::truncateToStub`).
3. **The queue blob is format 2** (stub area + persistent seq counter); a format-1 blob
   is **discarded completely** on the first boot of the new firmware instead of being migrated. Both
   nodes are flashed at the same bench (versioning-and-updates.md §3), the discarded
   outbox is visible there and can be typed again; a migration belongs to whoever writes format 3.
   The devices currently hold only sketch test content anyway.

Gate 3.2's wording in `docs/test-plan.md` now describes the full ladder (evict Delivered
→ truncate Undelivered → only then reject), sketch 16 checks it, and the
`MESSAGES` screen shows headers as "(text dropped)" in the UNDELIVERED frame.
