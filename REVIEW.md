# Review of the original CLAUDE.md — findings and fixes

Reviewed against current toolchain, hardware and regulatory state as of August 2026.
Every finding below is either fixed in the rewritten `CLAUDE.md` or listed as an open
decision at the end.

Severity: **[BLOCKER]** stops a phase from completing · **[MAJOR]** wrong result or
wasted work · **[MINOR]** inaccuracy worth correcting.

---

## A. Internal contradictions

### A1 [BLOCKER] The 30 s RX window and the 1 % duty cycle cannot both hold
§3.2 said devices open an RX window every 30 s and "senders repeat the preamble to bridge
the gap". A preamble that bridges a 30 s gap *is* 30 s of transmit time. At 1 % you have
36 s of airtime per hour. That is one message per hour, and the second one is illegal.

Real numbers, BW125, CR4/5, 25-byte frame, preamble sized to cover the sniff interval:

| SF | interval | preamble | TX airtime | msgs/h @1 % | msgs/h @10 % | RX avg |
|---|---|---|---|---|---|---|
| SF9 | 0.5 s | 123 sym | 677 ms | 53 | 532 | 0.77 mA |
| SF9 | 2 s | 489 sym | 2176 ms | 16 | 165 | 0.21 mA |
| SF9 | 5 s | 1221 sym | 5174 ms | 7 | 70 | 0.09 mA |
| SF12 | 1 s | 31 sym | 2236 ms | 16 | 161 | 4.21 mA |
| SF12 | 5 s | 153 sym | 6234 ms | 6 | 58 | 0.86 mA |

Note SF12 at short intervals: the receiver is awake essentially all the time (5.3 mA),
which alone exceeds the two-week power target on an 800 mAh cell (2.4 mA budget).

> **Correction, 2026-08-30.** The frame size used for this table (25 B) followed the
> "9 header + 4 MIC" figure, which turned out to contradict the header layout in 2.1. The
> real frame is 28 B and the airtime column is a few per cent higher. The numbers above are
> left as they were written -- this is a dated review, not a normative document. The
> corrected tables are in `CLAUDE.md` 1.4 and 2.3; the reasoning is in
> `docs/decisions/0001-open-decisions.md` D7. **The conclusion is unchanged and in fact
> slightly stronger:** larger frames make A1's point about the 30 s RX window worse, not better.

**Fix:** the sniff interval is now the single tuning knob, with a published table of what
each setting costs in airtime *and* current. Default 2 s at SF9. The mechanism is the
SX1262's own `SetRxDutyCycle` (RadioLib `startReceiveDutyCycleAuto`), so the MCU sleeps
through the sniff cycle instead of waking every window.

### A2 [BLOCKER] AES-CCM with an 8-bit sequence number reuses the nonce
§2.1 mandated AES-128-CCM; §2.3 gave a `seq:8` per peer. CCM is CTR mode plus CBC-MAC —
reusing a nonce under the same key leaks the XOR of two plaintexts and enables tag
forgery. `seq:8` wraps after 256 frames, and a reboot restarts it at zero.

**Fix:** 32-bit monotonic frame counter, persisted to flash, never reset, included in the
nonce and in the replay window. The 8-bit `seq` stays as the ARQ handle only.

### A3 [MAJOR] The ARQ timeout is shorter than the duty-cycle lockout it triggers
§2.3 set the retry timeout to `2 × airtime + 300 ms`. At SF12 that is ~3.5 s, but a single
SF12 frame locks the 1 % band for ~147 s. Every retry would be blocked by the budget
before the timer was ever relevant, and the state machine had no defined behaviour for
that.

**Fix:** the retry scheduler asks the budget for the earliest legal transmit time and
schedules there; UI shows "queued until HH:MM" rather than "failed".

### A4 [MAJOR] The duty-cycle budget was not persistent, which makes it bypassable
§1.2 called the budget legally binding, but a reboot would have cleared it. Power-cycling
between transmissions would have been a compliance hole.

**Fix:** the budget is a persistent ring of (timestamp, airtime) records in flash, keyed
against the on-board RTC. On boot the budget is reconstructed; if the RTC is invalid the
device starts in a fully blocked state until time is re-established.

### A5 [MINOR] "Never redraw on a timer" vs. "schedule a full refresh periodically"
§1.5 forbade timer-driven redraws and required periodic full refresh in the same
paragraph.

**Fix:** ghosting mitigation is counted in *partial updates performed*, not wall-clock
time. Full refresh after N partials (default 16) and on screen change.

### A6 [MINOR] §2.5 defines an abstraction for a feature it forbids
`IRadioLink` existed so a second stack "could be added later", and the same paragraph
explained at length why a second stack is impossible on this hardware.

**Fix:** the seam stays because it is good structure, the justification is dropped.

---

## B. Hardware reality

### B1 [BLOCKER — RESOLVED] "T-Echo" is now three different boards
**Resolved from photos:** `MODEL: T-Echo`, FCC ID `2ASYE-T-ECHO`, 868 MHz, BME280 fitted,
800 mAh cell → Phase 5 target is 2.38 mA average. The carabiner loop and screw thread are
a 3D-printed bracket, not the Plus housing.

Original finding:
There is the original T-Echo, the **T-Echo Plus** (BHI260AP IMU, buzzer, vibration motor,
2400 mAh battery, ¼-inch mount), and the **T-Echo Lite** (176×192 GDEM0122T61 panel,
optional keyboard shield with TCA8418 + ES8311 audio). They have different pin maps,
different batteries, and different peripherals. The T-Echo Plus was not listed as
officially supported by Meshtastic at launch.

The battery difference alone changes the Phase 5 target by a factor of three: 800 mAh
gives a 2.38 mA average budget for two weeks, 2400 mAh gives 7.14 mA.

**Fix:** Phase 0 now starts by identifying which board you actually hold, and the power
target is expressed as an average current derived from the measured capacity.

### B2 [MAJOR — RESOLVED] "2 MB Flash / 2 MB RAM" in LILYGO's spec sheet is half wrong
**Resolved from the device label:** there *is* an external flash chip, a **ZD25WQ16B,
2 MiB SPI NOR**, already supported by `Adafruit_SPIFlash`. So the "2 MB Flash" figure is
real, it just describes the external chip rather than the MCU. The "2 MB RAM" figure
remains wrong. Map tiles are therefore feasible after all (moved to Phase 8), and the
counter, budget ring and message queue get a proper home on LittleFS. Note the chip's
~12 µA standby: it must be put in deep power-down or it eats most of the 20 µA idle target.

Original finding:
The nRF52840 has 1 MB internal flash and 256 KB RAM. After the S140 SoftDevice (~152 KB)
and the UF2 bootloader, an application has roughly 800 KB. Whether the board carries an
external QSPI flash chip must be verified physically — it is not safe to assume from the
marketing table.

**Fix:** flash and RAM budget is a Phase 0 deliverable with a stated method (map file plus
visual inspection of the PCB), and the MAP screen is deferred until that number exists.

### B3 [MAJOR] The MAP screen was specified before its storage was known
§3.4 described pre-rendered tiles uploaded over BLE, while §5 Phase 0 said to establish
the flash budget first. Uploading tiles over BLE at ~247 byte MTU is also slow: a 200×200
1-bit tile is 5 KB, so a modest 4×4 tile set is 80 KB.

**Fix:** MAP moved out of Phase 4 into "later". Position display in Phase 4 is numeric
plus a bearing/distance arrow to the peer, which is what you actually need in the field
and costs nothing.

### B4 [MINOR] The panel and its controller were unspecified
The T-Echo's 1.54" 200×200 panel is a GDEH0154D67-class part driven by an **SSD1681**.
Partial refresh is real (~0.26–0.3 s) and GxEPD2 supports it, but Meshtastic's own driver
historically did full refreshes only, so don't take Meshtastic's behaviour as the limit.
Some units have shipped with different panels.

**Fix:** panel identification is a Phase 0 item; `GxEPD2_154_D67` is the starting point.

### B5 [MINOR] Button count
The T-Echo has a capacitive touch button plus two push buttons, one of which is hard-wired
to nRESET. Double-pressing reset enters the bootloader. So "one usable physical button" is
correct, but the reset button's bootloader behaviour needs to be documented for users, and
long-press-to-shutdown must not be confused with it.

---

## C. Toolchain and library versions

### C1 [BLOCKER] The CryptoCell mandate cannot be met on this stack
§2.1 required AES-128-CCM via the CC310 and explicitly forbade software AES. In practice:

- `Adafruit_nRFCrypto` is the only Arduino-level wrapper. Last release 0.1.2 (Oct 2023),
  43 commits, 11 stars, and it does not expose a CCM interface.
- Nordic's `nrf_cc310` requires SDK headers that are not shipped with the Adafruit BSP.
- The CC310 requires input data in DMA-accessible RAM, which constrains buffer placement.
- The nRF52's own CCM peripheral is hard-wired to the BLE link-layer packet format and is
  awkward to repurpose.

The mandate would have blocked Phase 2 indefinitely for zero benefit: AES-128-CCM over a
61-byte frame on a 64 MHz Cortex-M4F costs microseconds against a 2000 ms transmission.

**Fix:** software AES-128-CCM built on a vetted implementation, with the nRF ECB hardware
peripheral as an optional accelerator for the block function. CryptoCell is listed as an
optional later optimisation, not a requirement.

### C2 [MAJOR] Astro 4 is two majors behind
Astro 6 shipped 10 March 2026 (requires Node 22+, drops Node 18/20). Astro 7 shipped June
2026 with Vite 8 and a Rust compiler; current is 7.2.1 (11 Aug 2026). Astro 7 also removed
the `astro db`, `astro login`, `astro link` and `astro init` CLI commands. Note that the
Astro company joined Cloudflare in January 2026.

**Fix:** Astro 7 + Node 22 LTS, Tailwind v4 (CSS-first config, Vite plugin — there is no
`tailwind.config.js` any more), React 19.

### C3 [MAJOR] Drizzle 1.0 is still beta — do not start on it
Stable is 0.45.2 (27 March 2026). The 1.0 line is at beta.22 and the team's own notes say
things will break. Note also that beta.19 fixed an SQL-injection issue in
`sql.identifier()` / `sql.as()`.

**Fix:** pin `drizzle-orm@0.45.x`, revisit after 1.0 is stable.

### C4 [MINOR] PlatformIO's nRF52 platform is fine, contrary to reputation
`platform-nordicnrf52` v10.11.0 (Feb 2026) bundles Adafruit nRF52 core v1.7.0. Pin both.
C++17 needs `build_unflags = -std=gnu++11` because the core still defaults to gnu++11.

### C5 [MINOR] RadioLib is at v7.x with breaking changes from 6.x
Pin the major. Two documented gotchas that will cost you a day each if you hit them blind:
`startReceiveDutyCycleAuto()` silently misses 10–25 % of packets unless
`setPreambleLength()` matches the sender, and `minSymbols` below 8 is unreliable (Semtech
says the receiver needs 8 symbols to latch a preamble). Also: DIO2 drives the RF switch and
DIO3 powers the TCXO on this module, and the TCXO adds ~5 ms of startup delay to every
wake — which is why very short sniff intervals stop saving power.

---

## D. Regulatory

### D1 [MAJOR] You are working in the wrong sub-band
The current standard is **ETSI EN 300 220-2 V3.3.1 (2025-03)**. The band you chose,
868.0–868.6 MHz (g1 / sub-band M), gives 25 mW ERP and 1 % — 36 s of airtime per hour.

**869.4–869.65 MHz (g3 / sub-band P) gives 500 mW ERP and 10 %** — 360 s per hour, ten
times the budget, and it legalises the SX1262's full +22 dBm output. For a two-node
private point-to-point link this is simply the correct band, and it is what makes the
whole design comfortable rather than marginal. The trade-off: the band is 250 kHz wide, so
BW125 fits but leaves little room to move, and LoRaWAN networks use 869.525 for downlinks,
so expect some neighbours.

**Fix:** g3 is the primary band (confirmed), g1 remains a configurable fallback, and the
budget tracker is per-sub-band so both are handled by the same code. Default centre
frequency 869.575 MHz, chosen to sit clear of the LoRaWAN RX2 downlink at 869.525.

### D2 [MINOR] Duty cycle can be traded for LBT+AFA — but not in g3
EN 300 220 allows Listen Before Talk with Adaptive Frequency Agility instead of a duty
cycle in most sub-bands. In g3 specifically, LBT+AFA is *more* restrictive (100 s per
200 kHz per hour vs. 360 s). So: stay with duty cycle, do not build LBT.

### D3 [MINOR] ERP is not the same as chip output power
25 mW / 500 mW limits are ERP, so antenna gain and cable loss count. Cap the configured TX
power with a per-band ceiling in code, not in a user setting.

### D4 [check] Maximum continuous transmit time
Older versions of EN 300 220 carried a 1 s maximum on-time with a 100 ms minimum off-time
for some band entries. An SF12 frame is 1.5–2.8 s. LoRaWAN transmits SF12 in EU868
routinely, so this is very likely not applicable, but verify against the current V3.3.1
table before shipping SF12 — it is a five-minute check that could invalidate your highest
SF.

---

## E. Bridge and backend

### E1 [BLOCKER] Web Bluetooth has no background access
§4.2 specified a PWA that queues events and "syncs to the server opportunistically". Web
Bluetooth connections drop when the tab is hidden, and there is no background mode. There
is no version of this that syncs while the phone is in your pocket. Apple has declined to
implement Web Bluetooth and its position is unchanged in 2026, so iOS is out entirely
(which your spec already said).

**Fix:** the PWA is specified as an explicitly foreground bridge — "open the app to sync" —
and the device queues everything so that this is workable. A native Android bridge is now
Phase 9 rather than a maybe, which means the bridge protocol is documented independently of
the client from the start.

### E2 [MAJOR] Event ingestion needs an idempotency key
An append-only log fed by a bridge that re-sends on reconnect will duplicate rows. The
original spec had no unique key on events.

**Fix:** `UNIQUE (device_id, frame_counter, direction)` — which the 32-bit counter from A2
now makes possible.

### E3 [MINOR] Config push and versioning
"Queued and applied on next bridge connection" needs a version and an ack, otherwise you
cannot tell whether a config took effect.

**Fix:** `config_versions` gets an `applied_at` set from a device ack.

---

## F. Phase plan

### F1 [BLOCKER] Chicken-and-egg on the network key
The network key is provisioned over BLE (Phase 6), but the link layer that needs it is
Phase 2. As written, Phase 2 could not be tested.

**Fix:** a minimal BLE provisioning path is pulled forward into Phase 2, and the phases are
renumbered. A build-time development key is allowed for bench work behind a compile flag
that also refuses to build in release mode.

### F2 [MAJOR] Phase 5's power target was unanchored
"Two weeks on the built-in battery" means nothing without knowing the battery. It also
implicitly assumed the 30 s RX window from A1.

**Fix:** the target is stated as a measured average current derived from the actual cell,
with a per-subsystem breakdown and the sniff interval named as the variable being traded.

For calibration: cfr34k's `t-echo-lora-aprs`, a custom non-Meshtastic firmware for exactly
this board, reports ~100 µA standby with BLE connectivity and 1–4 months of standby on the
800 mAh cell. That is your proof the target is reachable — but note it is built directly
on the nRF5 SDK, not the Arduino core, and the Arduino core plus SoftDevice will cost you
some of that margin.

### F3 [MINOR] Missing prior art
The spec said "no Meshtastic, no SoftRF" but never pointed at the one project that has
already solved the hard parts of this board: `cfr34k/t-echo-lora-aprs` (custom epaper
driver for the SSD1681 with partial refresh, GNSS power management, BLE config service,
measured low-power design) and `cfr34k/t-echo-bootloader`.

**Fix:** listed as required Phase 0 reading. Read it for the hardware knowledge, don't
copy the code — check the licence before borrowing anything.

---

## G. Smaller corrections folded into the rewrite

- The airtime table used "20-byte payload", but the actual frame is 9 header + payload +
  4 MIC, so nothing on air is ever 20 bytes. Table recomputed for real frame sizes.
- "No dynamic allocation in the link layer" is unenforceable as written, since RadioLib
  and the Arduino core both allocate. Scoped to own code, with a check.
- Adaptive SF requires both peers to converge, but the convergence mechanism (BEACON)
  itself costs airtime and is unreliable at the point where it is most needed. A fixed
  rendezvous configuration that never changes has been added as a floor.
- The Meshtastic variant path has moved to `variants/nrf52840/t-echo/`.
- The board has a PCF8563-class RTC — verify — and A4 now depends on it.
- No key rotation, no factory reset, no link-test mode were specified. Added.
- Serial debug over USB CDC keeps the USB peripheral alive and will distort power
  measurements. Called out in the Phase 5 method.

---

## Open decisions — all resolved

1. ~~**Sub-band.**~~ g3 (869.4–869.65, 10 %, 500 mW) is primary, default centre 869.575 MHz.
2. ~~**Bridge.**~~ PWA now, native Android as Phase 9. Bridge protocol documented
   client-independently from the start.
3. ~~**Board.**~~ Original T-Echo, 868 MHz, BME280, 800 mAh, 2 MiB external flash.

All three are folded into `CLAUDE.md`. Nothing in this review is left hanging.
