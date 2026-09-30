# CLAUDE.md — LoRa Field Node Project

> Revision 2 (August 2026). Supersedes revision 1. See `REVIEW.md` for what changed and
> why. Where this document and revision 1 disagree, this document wins.

## 0. What this is

A custom LoRa communicator built on LilyGO T-Echo hardware (nRF52840 + SX1262, EU 868 MHz
band). Fully custom firmware — no Meshtastic, no SoftRF. Three components:

**Confirmed hardware** (from the device labels, August 2026):

| | |
|---|---|
| Model | **T-Echo** (original, not Plus, not Lite) — `MODEL: T-Echo`, FCC ID `2ASYE-T-ECHO` |
| Band | 868 MHz, matching 868M antenna |
| Sensor | BME280 fitted |
| External flash | **ZD25WQ16B — 2 MiB SPI NOR**, 1.65–3.6 V, quad SPI capable |
| Battery | 800 mAh (standard T-Echo cell) → **2.38 mA average budget** for the two-week target |

The `2 MB Flash` on LILYGO's spec sheet refers to this external chip, not to the MCU. The
nRF52840 itself has 1 MB internal flash and 256 KB RAM; the `2 MB RAM` claim is simply
wrong. The external chip is already a known device in `Adafruit_SPIFlash`
(`flash_devices.h`), so LittleFS on it works without writing a driver.

| Document | Covers |
|---|---|---|
| `docs/protocol.md` | Radio wire format change log and version history |
| `docs/bridge-protocol.md` | Normative phone↔device protocol, client-independent |
| `docs/versioning-and-updates.md` | The three version numbers, firmware updates, key rotation |
| `docs/test-plan.md` | Per-phase gates with pass criteria |

| Component | Path | Stack |
|---|---|---|
| Device firmware | `firmware/` | PlatformIO, Adafruit nRF52 Arduino core, C++17 |
| Phone bridge | `bridge/` | PWA, Web Bluetooth, vanilla TS + Vite |
| Management dashboard | `web/` | Astro 7 SSR + React 19 islands + Tailwind v4 + Drizzle + PostgreSQL |

Deployment: `web/` runs in Docker Compose behind a TLS-terminating reverse proxy
(`docs/DEPLOYMENT.md`). `bridge/` is served as static assets from the same origin (Web
Bluetooth requires a secure context).

Initial scale: 2 devices. Design must not assume 2 — device IDs, key management and the
dashboard are built for N nodes from day one. A stationary gateway is out of scope for now,
but the protocol must not prevent adding one later.

### 0.1 Pinned versions

Pin these. Do not float them mid-project.

| Thing | Version | Note |
|---|---|---|
| `platform-nordicnrf52` | 10.11.0 | bundles Adafruit nRF52 core 1.7.0 |
| RadioLib | 7.x | 7.0 broke API vs 6.x; pin the major |
| GxEPD2 | latest | driver class `GxEPD2_154_D67` (SSD1681) |
| Node | 22 LTS | Astro 7 requires ≥22 |
| Astro | 7.x | `astro db`/`login`/`link`/`init` CLI commands were removed |
| Tailwind | v4 | CSS-first config, Vite plugin, no `tailwind.config.js` |
| drizzle-orm | 0.45.x | 1.0 is still beta — do not adopt mid-project |
| PostgreSQL | 17 | |

C++17 needs `build_unflags = -std=gnu++11` — the Adafruit core still defaults to gnu++11.

### 0.2 Required reading before Phase 0

`cfr34k/t-echo-lora-aprs` is a custom, non-Meshtastic firmware for this exact board. It has
already solved the SSD1681 partial-refresh driver, GNSS power sequencing, a BLE config
service, and a measured low-power design reaching ~100 µA standby with BLE up. It is built
on the bare nRF5 SDK rather than the Arduino core, so its code does not transfer directly —
read it for hardware knowledge, and check its licence before borrowing anything. See also
`cfr34k/t-echo-bootloader`.

---

## 1. Hard constraints — do not design around these, design *for* them

**1.1 No WiFi.** The nRF52840 has Bluetooth LE only. All internet connectivity flows
through a paired Android phone running the PWA. A device with no phone in range is fully
offline and must remain useful in that state.

**1.2 The duty cycle is legally binding and is enforced in firmware.** The budget tracker
maintains a rolling 60-minute airtime total per sub-band, per device, and MUST refuse to
transmit when the budget is exhausted. It is not a configurable option and is not exposed
as a user setting. A blocked transmission is queued with a scheduled release time, never
dropped.

**Two constraints, and a transmission must satisfy both.** The rolling hourly
total above, *and* a per-frame lockout: after transmitting a frame of airtime `A`,
the sub-band stays silent for `A × (1/dutyCycle − 1)`, measured from the end of
that transmission. §1.4 tabulates the lockout. `earliestLegalTx()` returns the
later of the two.

Without the lockout the hourly total could legally be spent as one burst — 163
frames back to back at the default configuration, then 54 minutes of silence —
which satisfies the regulation and is useless as a communicator. Without the
hourly total there is no budget to report on the `STATUS` screen or in
`EVT_BUDGET`. See `docs/decisions/0001-open-decisions.md` D9.

The budget **survives reboot**. It is persisted to flash as a ring of (timestamp, airtime,
sub-band) records and reconstructed on boot against the RTC. If the RTC time is not valid
on boot, the device starts fully transmit-blocked until time is re-established over BLE or
GNSS. A budget that a power cycle can clear is not a budget.

**1.3 Band plan.** Reference: ETSI EN 300 220-2 V3.3.1 (2025-03).

| Sub-band | Range | ERP | Duty cycle | Airtime/hour |
|---|---|---|---|---|
| g3 / P — **primary** | 869.4–869.65 MHz | 500 mW | 10 % | 360 s |
| g1 / M — fallback | 868.0–868.6 MHz | 25 mW | 1 % | 36 s |

g3 is the working band — decided, not provisional. It gives ten times the airtime and
legalises the SX1262's full +22 dBm. g1 remains selectable and the tracker handles both,
because the budget is per-sub-band anyway.

**Default centre frequency: 869.575 MHz.** At BW125 that occupies 869.5125–869.6375 MHz,
comfortably inside g3. It deliberately avoids 869.525 MHz, which is the LoRaWAN RX2
downlink frequency and therefore where every gateway in range is transmitting at high
power. g1 fallback default is 868.1 MHz.

g3 is only 250 kHz wide, so there is no room for channel hopping and no second channel to
retreat to. That is the price of the 10 %: one channel, chosen carefully. The 868M antenna
that ships with the device covers 869.575 without issue.

ERP includes antenna gain and cable loss, so the configured TX power is capped per band in
code, not by the user. Do **not** implement LBT+AFA as a duty-cycle escape: in g3 it is
more restrictive than the duty cycle (100 s per 200 kHz per hour), not less.

Before shipping SF12, verify against the current EN 300 220-2 table whether a maximum
continuous on-time applies to g3. An SF12 frame runs 1.5–2.8 s.

**1.4 Airtime is the scarce resource.** BW125, CR4/5, explicit header, CRC on, 8-symbol
preamble. Frame overhead is 12 header + 4 MIC = 16 bytes (see 2.1), so no real frame is
small:

| Frame | SF7 | SF9 | SF10 | SF12 |
|---|---|---|---|---|
| ACK (20 B) | 57 ms | 185 ms | 371 ms | 1319 ms |
| POSITION (28 B) | 67 ms | 226 ms | 412 ms | 1647 ms |
| TELEMETRY (30 B) | 72 ms | 226 ms | 453 ms | 1647 ms |
| TEXT max (64 B) | 118 ms | 390 ms | 698 ms | 2793 ms |

Lockout after a single POSITION frame — the time before you may transmit again, even
with the hourly total otherwise empty. This is the second of the two constraints in
§1.2, measured from the end of the transmission:

| Band | SF7 | SF9 | SF10 | SF12 |
|---|---|---|---|---|
| g3 (10 %) | 0.6 s | 2.0 s | 3.7 s | 14.8 s |
| g1 (1 %) | 6.6 s | 22.4 s | 40.8 s | 163.0 s |

Every ACK spends the *receiver's* budget. Payloads are binary and minimal. No JSON, no
Protobuf, no text protocol on the air interface.

**1.5 GPS is the power budget.** The L76K draws tens of mA when active versus microamps
for the sleeping MCU. GNSS is powered down by default and only enabled for an explicit fix
request or while the position screen is open, with a hard timeout. Never leave it running
"just in case".

**1.6 E-paper is slow.** 200×200 monochrome, SSD1681 controller. Use partial refresh for
anything that changes often. Redraw on state change only. Ghosting mitigation is counted in
*partial updates performed* — full refresh after 16 partials or on screen change, never on
a wall-clock timer.

**Measured on node A, 2026-08-31** (twice, from separate sketches, 7 ms apart) — these
replace the ~2 s / ~0.3 s this paragraph carried before, which were estimates:

| | measured | note |
|---|---|---|
| Full refresh | **4.4 s** | more than double the estimate |
| Partial, changed region only | **0.32 s** | matches |
| Partial, whole 200×200 panel | 0.47 s | misses gate 1.1's 400 ms |

The last row is why the display driver takes a rectangle: a screen that changes one line
must not pay for two hundred rows. `hal::Canvas::diffBounds()` computes it.

The 4.4 s has a consequence this paragraph does not resolve. A full refresh on every screen
change means **every touch costs 4.4 seconds**, and the five-screen cycle costs 22. The
alternative — screen change as a partial, relying on the 16-partial counter alone for
ghosting — is cheaper by a factor of nine and is only safe if 16 partials across *different*
screens still look clean. That is gate 1.2, it needs eyes, and until it has been looked at
this document's rule stands as written. See `docs/decisions/0001-open-decisions.md` D14.

**1.7 Two usable inputs.** A capacitive touch button (TTP223) and one physical user button.
The second push button is hard-wired to nRESET and is not available to the application;
double-pressing it enters the bootloader, which users will do by accident, so document it.
Button interaction must never be required for correct operation — all configuration is
possible over BLE from the PWA.

---

## 2. Radio protocol

### 2.1 Frame format

```
byte 0       : ver:4 | type:4
byte 1       : netId:8
bytes 2-3    : src:16       (device ID, little endian)
bytes 4-5    : dst:16       (0xFFFF = broadcast)
bytes 6-9    : counter:32   (monotonic, persistent, per-device)
byte 10      : seq:8        (ARQ handle, wraps freely)
byte 11      : flags:8      (bit0 ACK_REQ, bit1 RETRY, bit2 LOW_BATT, bits3-7 reserved)
bytes 12..n  : payload
last 4       : MIC:32
```

12-byte header + 4-byte MIC, so 16 bytes of overhead on every frame. The LoRa explicit
header carries the length, so no length field is needed.

**Crypto.** AES-128-CCM. Header is authenticated (AAD), payload is encrypted and
authenticated. The CCM nonce is built from `src` and `counter` — those 13 bytes are what
makes nonce reuse impossible, which is the entire reason `counter` is 32 bits and
persistent.

Rules for `counter`:
- Monotonic per device, never reset, never reused, persisted to flash.
- Persist in blocks (e.g. reserve 256 ahead, write on block exhaustion) so you are not
  writing flash on every frame; on boot, resume from the reserved high-water mark, not
  from the last used value.
- The receiver keeps a replay window per peer and rejects anything at or below the
  high-water mark outside the window.
- If the counter cannot be persisted, the device must refuse to transmit.

Use a vetted software AES-128-CCM implementation. The nRF ECB hardware peripheral may be
used for the block function as an optimisation. **Do not** make CryptoCell/CC310 a
requirement: the only Arduino-level wrapper (`Adafruit_nRFCrypto`, last release 2023) does
not expose CCM, Nordic's `nrf_cc310` needs SDK headers absent from the Adafruit BSP, and
the CC310 requires DMA-accessible input buffers. Software CCM over a 61-byte frame costs
microseconds against a transmission measured in seconds. Revisit only if profiling says
otherwise.

Network key is shared, provisioned over BLE, stored in flash. Never hardcode a key, never
commit one. A development key behind a compile flag is permitted for bench work in phases
2–3; that flag must fail the build in release configuration.

### 2.2 Frame types

`BEACON`, `DATA`, `ACK`, `TELEMETRY`, `POSITION`, `TEXT`, `CONFIG`.

Payload encodings — fixed-width binary, little endian:
- `POSITION`: lat int32 (deg × 1e7), lon int32, alt int16 (m), hdop uint8, fixAge uint8
- `TELEMETRY`: tempC int16 (0.01 °C), humidity uint16 (0.01 %), pressure uint32 (Pa),
  battery uint16 (mV), uptime uint32 (s)
- `TEXT`: UTF-8, max 48 bytes, no null terminator
- `ACK`: seq uint8 of acknowledged frame, rssi int16, snr int8 (dB, rounded)

### 2.3 Duty-cycled receive — the core mechanism

Devices are not continuously listening. The receiver uses the SX1262's own
`SetRxDutyCycle` (RadioLib `startReceiveDutyCycleAuto`), which sniffs for a preamble and
returns to sleep without waking the MCU. The sender transmits a preamble long enough to
span one sniff interval.

**The sniff interval is the single tuning knob.** It trades latency against both battery
and airtime — not just battery, which is what revision 1 got wrong.

| SF | Interval | Preamble | TX airtime | msgs/h @10 % | RX average |
|---|---|---|---|---|---|
| SF9 | 0.5 s | 123 sym | 697 ms | 516 | 0.77 mA |
| SF9 | **2 s (default)** | 489 sym | 2196 ms | 163 | 0.21 mA |
| SF9 | 5 s | 1221 sym | 5195 ms | 69 | 0.09 mA |
| SF12 | 1 s | 31 sym | 2400 ms | 149 | 4.21 mA |
| SF12 | 5 s | 153 sym | 6398 ms | 56 | 0.86 mA |

Read the SF12 rows carefully: at short intervals the receiver is effectively always on,
which alone blows a two-week target on an 800 mAh cell. High SF and low latency are
mutually exclusive on this hardware. The dashboard must show this table, not a vague
"latency vs. battery" note.

Implementation gotchas, both of which will cost you a day if you meet them blind:
- `setPreambleLength()` on the receiver must match the sender's preamble, or
  `startReceiveDutyCycleAuto()` silently drops 10–25 % of packets.
- `minSymbols` below 8 is unreliable; Semtech's guidance is 8 symbols to latch a preamble.
  Use 8 for SF ≤ 10 and 12 above.
- The TCXO adds ~5 ms of startup to every wake. Below roughly 0.5 s intervals you are
  paying for oscillator startup, not reception.

### 2.4 Reliable delivery

Stop-and-wait ARQ with per-peer sequence numbers.

1. Sender transmits with `ACK_REQ` set.
2. Receiver validates the MIC, checks the replay window on `counter`, dedupes on
   `(src, seq)`, delivers to the app layer, and replies with `ACK` carrying its measured
   RSSI and SNR — *if its own budget allows*. A budget-blocked ACK is queued.
3. The retry timer is `2 × expectedAirtime(SF) + 300 ms`, but the retry is scheduled at
   `max(timerExpiry, budget.earliestLegalTx())`. The budget always wins.
4. Up to 3 retries with randomised backoff (`base × 2^attempt ± 25 % jitter`), each
   subject to the same scheduling.
5. After 3 failed attempts the frame is marked undelivered and surfaced in the UI. It is
   **not** silently dropped.

The UI distinguishes three states, because they mean different things to the user:
`queued until HH:MM` (budget), `in flight` (retrying), `undelivered` (gave up).

Broadcast frames never request an ACK.

### 2.5 Adaptive spreading factor

There is a **fixed rendezvous configuration** that never changes: SF9 / BW125 / CR4/5 on
the primary channel, with the default sniff interval. It is the floor the link falls back
to, and both nodes always accept it. Adaptation happens above that floor.

- Start at the rendezvous configuration.
- Escalate one SF step (max SF12) after 2 consecutive failed delivery attempts, or when
  the last 5 ACKs show SNR below the demodulation floor + 3 dB.
- De-escalate one step after 10 consecutive first-attempt successes with SNR margin above
  10 dB.
- Never change SF mid-retry sequence.
- After `3 × beaconInterval` with no contact, both sides return to the rendezvous
  configuration rather than scanning the SF set. Scanning costs receive time at every SF
  and is exactly what you cannot afford when the link is already bad.

`BEACON` advertises the current listening SF and sniff interval. Beacons cost airtime like
everything else — with two nodes, beacon rarely, and treat any received frame as an
implicit beacon.

### 2.6 Radio abstraction

The link layer sits behind an `IRadioLink` interface for testability and to keep the
framing code free of RadioLib types. Do not implement a second protocol stack — the SX1262
holds one modem configuration at a time.

---

## 3. Firmware architecture

Five modules, strictly layered. Lower layers must not call into higher ones.

```
app/        Sensor sampling, message queue, peer state, mode logic
ui/         Screen definitions, e-paper rendering, button handling
link/       Framing, crypto, ARQ, duty cycle budget, adaptive SF
ble/        GATT server, bridge protocol
hal/        Pin map, SPI/I2C, power rails, RTC, sleep management
```

No dynamic allocation in `link/` or `app/` — fixed buffers only, verified by a build check
on `malloc`/`new` in those translation units. RadioLib and the Arduino core allocate; that
is out of scope and not something to fight.

### 3.0 Persistent storage layout

Internal flash (1 MB, minus S140 SoftDevice ~152 KB and bootloader):
- application image
- network key and device identity — internal only, never on the external chip

External flash (ZD25WQ16B, 2 MiB, LittleFS via `Adafruit_SPIFlash`):
- frame counter high-water mark (§2.1)
- duty cycle budget ring (§1.2)
- message queue with delivery state (Phase 3)
- map tiles, if built (§3.3)

Rationale for the split: the key lives in internal flash because the external chip is
trivially readable by anyone with a clip and a logic analyser. Everything else benefits
from LittleFS's wear levelling, which matters for the counter and budget records that are
written continuously.

The external flash **must be put into deep power-down** whenever it is idle. Its standby
current is around 12 µA — on its own that is most of the `DEEP_IDLE` budget below.

**And it must be woken before every open.** Measured on node A, 2026-08-31: the ZD25WQ16B has
no reset line, so a deep power-down survives every MCU reset there is. In that state it
ignores every command except `0xAB`, the bus reads back `0xFFFFFF`, and
`Adafruit_SPIFlash::begin()` concludes there is no chip.

That is not merely a missing filesystem. Without the store there is no persistent frame
counter, and §2.1 requires a node that cannot persist its counter to refuse to transmit — so
a node whose flash was left asleep goes silent for ever, behaving exactly as this document
specifies, with nothing in any report to say why. It cost an evening.

Every open therefore goes through `hal::openExternalFlash()`, which sends the release command
first. There is no second path, for the same reason there is no second path to the radio.

### 3.1 Power state machine

| State | Trigger | Current target |
|---|---|---|
| `DEEP_IDLE` | no activity, screen static, flash in deep power-down | < 20 µA |
| `SNIFF` | SX1262 RxDutyCycle running, MCU asleep | see §2.3 table |
| `ACTIVE` | button press or incoming frame | ~15 mA |
| `GNSS_FIX` | position screen or fix request | ~40 mA |

Use nRF52 System-ON sleep with RTC wakeup, not delay loops. Verify in Phase 0 whether the
Adafruit core's FreeRTOS has tickless idle enabled, and if not, what the fallback is
(`waitForEvent()` / `suspendLoop()`). Verify actual draw with a meter before declaring any
phase complete — calculated estimates are not acceptance criteria.

### 3.2 Button map

> Amended 2026-08-31 by decision D17: the touch pad on this device does not respond through
> the closed housing (`docs/test-results/2026-08-31_touch_not_reachable.md`), so the push
> button carries the navigation. The touch mappings stay wired for a device where the pad
> works.

| Input | Action |
|---|---|
| Button, short | Next screen (a screen change is also the wake/refresh) |
| Button, double | Screen-specific primary action |
| Button, long (> 2 s) | Power menu (sleep / shutdown) |
| Touch, short | Next screen |
| Touch, long (> 1 s) | Screen-specific primary action |

No chords, and no double-taps **on the touch pad** — that input is not reliable enough for
timing-sensitive gestures, and its detector has the double-press machinery disabled. The
push button measured clean (734 edges for 367 presses, exactly two per press), which is
what D17's double press stands on. Long-press must give visual feedback before the action
fires.

### 3.3 Screens

`STATUS` (battery, link quality per peer, unread count, duty cycle budget remaining) →
`MESSAGES` → `TELEMETRY` (local BME280 + last received peer values) → `PEERS` (last seen,
RSSI, SNR, SF) → `POSITION`.

`POSITION` shows own coordinates, fix age, and bearing plus distance to each peer as a
numeric readout and an arrow. This is what you actually read in the field, and it costs
nothing.

Map tiles are **feasible but not in the first release**. The 2 MiB external chip holds
roughly 400 tiles of 200×200 at 1 bit — plenty. The real constraint is the upload path:
at a 247-byte MTU a single 5 KB tile is ~20 BLE writes, so a useful tile set is a
multi-minute transfer. Build `MAP` after Phase 7, with a tile set scoped to named areas
the user selects, not a coverage grid.

---

## 4. Bridge and backend

### 4.1 BLE GATT service

Custom 128-bit service UUID. Characteristics:

- `TX` (notify) — frames and events from device to phone
- `RX` (write) — commands and outbound messages from phone to device
- `CONFIG` (read/write) — settings blob, versioned
- `STATUS` (read/notify) — battery, uptime, queue depth, duty cycle budget remaining

MTU negotiation up to 247 bytes; anything larger is chunked with a 2-byte sequence prefix.
Writes that change configuration or provision keys require a paired, bonded connection with
passkey authentication — an unauthenticated GATT write must not be able to set the network
key.

### 4.2 Bridge — PWA first, native Android later

The PWA is the bridge for now. A native Android app is a planned later phase, not a
hypothetical, so **the bridge protocol is specified independently of the client** in
`docs/bridge-protocol.md` — framing, chunking, command set, event set — such that a Kotlin
client can be written against the document without reading the TypeScript. Nothing in the
firmware or the server may assume the client is a browser. In particular the ingestion
endpoint (§4.3) authenticates a device-bridge pair, not a web session.

Web Bluetooth in Chrome for Android. **The connection drops when the tab is hidden and
there is no background mode.** Opportunistic background sync is not possible on this
platform, so the product is designed around it: the device queues everything, and the user
opens the app to sync. The UI states plainly when the last sync happened and how many
events are pending on the device.

iOS is not supported — Apple has declined to implement Web Bluetooth and that has not
changed. Say so in the UI rather than failing silently.

The PWA works fully offline: events are queued in IndexedDB and pushed to the server when
the network is available *and the app is open*.

Do not attempt to work around the foreground limitation with service workers, wake locks or
periodic background sync — none of them keep a GATT connection alive. Background sync is
what Phase 9 is for.

### 4.3 Server data model

Append-only event log. Every frame received from any device is inserted as an immutable
event row; all current state (last position, latest telemetry, delivery status) is derived
by projection, never mutated in place.

Core tables: `devices`, `events`, `messages`, `link_stats`, `config_versions`.

`events` carries `UNIQUE (device_id, journal_counter, direction)`. The bridge re-sends on
reconnect and would otherwise duplicate rows; the persistent counter from §2.1 is what
makes this key possible.

**The journal counter is not the frame counter.** Both come from the same monotonic,
persistent, never-reused supply in §2.1, but they identify different things: a frame
counter identifies a transmitted frame, a journal counter identifies an entry in the
device's event journal. One frame produces several entries — `queued`, then `delivered` or
`undelivered` — and §4 of `docs/bridge-protocol.md` requires every one of them to arrive.
Keying the log on the frame counter would make the idempotency rule discard exactly the
state transitions it is supposed to carry. The frame counter travels in the event body,
which is why `EVT_FRAME_TX_RESULT` has a `counter` field at all. See
`docs/decisions/0001-open-decisions.md` D10.

`config_versions` carries `applied_at`, set from a device acknowledgement — a pushed config
is not an applied config.

The dashboard shows: node list with last position, telemetry history charts, message log
with delivery status, per-link RSSI/SNR/SF history, duty cycle budget consumption over
time, and remote config push.

---

## 5. Phases

Each phase ends with a working, verifiable state. Do not start a phase before the previous
one is confirmed on real hardware. Ask before assuming a phase is complete.

**Phase 0 — Hardware ground truth.** *Nothing else is written until this is done.*
Board identity, battery and external flash are already settled — see §0. What remains:
- Extract and verify the full pin map (e-paper, SX1262 + DIO1/DIO2/DIO3/RF switch, L76K,
  BME280, buttons, LED, RTC, external flash CS, power rails) from the LILYGO repository
  and from Meshtastic `variants/nrf52840/t-echo/` (the path moved), then confirm each
  peripheral with a minimal blink/read sketch.
- Identify the e-paper panel and confirm the SSD1681 controller. Some T-Echo units have
  shipped with different panels, so confirm on the actual device rather than from docs.
- Confirm the RTC part and that it keeps time across reset — §1.2 depends on it.
- Bring up `Adafruit_SPIFlash` on the ZD25WQ16B, mount LittleFS, confirm JEDEC ID, and
  measure the deep-power-down current so §3.0's assumption is real rather than a datasheet
  number.
- Document the internal flash and RAM budget from the map file.
- Verify whether the core's FreeRTOS has tickless idle.
- Deliver a `hal/board.h` that is known-correct.

**Phase 1 — Peripheral drivers.** E-paper full and partial refresh, BME280 read, button
handling with debounce, RGB LED, battery voltage measurement, RTC read/write. GNSS driver
with explicit power control and fix timeout. No radio yet.

**Phase 2 — Radio link layer.** SX1262 driver (DIO2 as RF switch, DIO3 as TCXO supply),
framing, AES-CCM, persistent frame counter, persistent duty cycle budget tracker,
stop-and-wait ARQ with budget-aware scheduling, RxDutyCycle sniffing, adaptive SF. Includes
a **minimal BLE key provisioning path** so the link can be keyed without waiting for
Phase 6. Bench test with two devices, then at increasing distance. Log every frame with
RSSI/SNR to serial.

Exit criteria: 100 frames exchanged at the default configuration with no MIC failures, no
counter reuse across a forced power cycle, and a budget tracker that correctly refuses and
reschedules when driven past the limit.

**Phase 3 — Application layer.** Sensor scheduling, message queue with persistence across
reboot, peer state tracking, configurable sniff interval.

**Phase 4 — UI.** All five screens, button map, partial refresh strategy, ghosting
mitigation.

**Phase 5 — Power optimisation.** Measured, not estimated. Target: **2.38 mA average**
(800 mAh over two weeks) at the default sniff interval with GNSS off. Produce a measured breakdown per subsystem.

Method note: compile out serial debug and measure with USB disconnected — an active USB CDC
peripheral will dominate the reading. For calibration, `cfr34k/t-echo-lora-aprs` reaches
~100 µA standby with BLE up on this board, so the target is reachable; expect the Arduino
core and SoftDevice to cost some of that margin against a bare-SDK build.

**Phase 6 — Full BLE and PWA.** Complete GATT service, bridge protocol, PWA with offline
queue and foreground-sync UX, bonded provisioning flow for network key and device ID, key
rotation, factory reset.

**Phase 7 — Dashboard.** Astro app, event ingestion endpoint with idempotency, projections,
UI.

**Phase 8 — Map tiles.** Tile format, selection UI, BLE upload, `MAP` screen. Storage is
confirmed available (§3.0); the constraint is transfer time.

**Phase 9 — Native Android bridge.** Kotlin client against `docs/bridge-protocol.md`, with
a foreground service so sync happens with the phone in a pocket. The PWA stays as the
zero-install path and as the reference implementation of the protocol.

**Later (not now):** stationary gateway, more than a handful of nodes, CryptoCell
acceleration, iOS in any form.

---

## 6. Working agreements

- Firmware is C++17 on the Adafruit nRF52 Arduino core via PlatformIO. Fixed buffers only
  in `link/` and `app/`.
- Every wire format change bumps the `ver` nibble, is recorded in `docs/protocol.md`, and
  follows the deployment rule in `docs/versioning-and-updates.md` — with two nodes there is
  no rolling upgrade, so both get flashed in the same session.
- The bridge protocol is specified in `docs/bridge-protocol.md` independently of any
  client. Neither firmware nor server may assume the client is a browser.
- A phase is done when its gate in `docs/test-plan.md` passes on hardware, with the numbers
  written down.
- Never power a node without an antenna attached.
- Serial debug output is compiled out in release builds via a log level macro.
- No secrets in the repository. Keys are provisioned at runtime. The development-key
  compile flag must fail a release build.
- Radio behaviour changes require a bench test with two devices before being called done.
- Anything that transmits goes through the budget tracker. There is no second path.
- When a design decision is ambiguous, ask with multiple-choice options rather than
  picking silently.
