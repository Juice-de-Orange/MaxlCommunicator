# Bridge protocol

Between a T-Echo node and a paired phone. This document is **normative and
client-independent**: the PWA and the later native Android client (Phase 9) are both
implementations of it. Nothing here may assume a browser.

Version: `BRIDGE_PROTO = 2`. Reported in `GET_INFO`. See `versioning-and-updates.md` for
the compatibility rules.

---

## 1. Transport

BLE GATT, custom 128-bit service UUID. Four characteristics:

| Characteristic | UUID | Properties | Direction | Carries |
|---|---|---|---|---|
| service | `6d61786c-0001-4c6f-5261-4e6f64650000` | — | — | — |
| `TX` | `6d61786c-0002-4c6f-5261-4e6f64650000` | notify | device → phone | events and responses |
| `RX` | `6d61786c-0003-4c6f-5261-4e6f64650000` | write | phone → device | commands |
| `CONFIG` | `6d61786c-0004-4c6f-5261-4e6f64650000` | read / write | both | settings blob, versioned |
| `STATUS` | `6d61786c-0005-4c6f-5261-4e6f64650000` | read / notify | device → phone | battery, uptime, queue depth, budget |

The bytes spell the project: `6d61786c` is `maxl`, `4c6f` `Lo`, `5261` `Ra`, `4e6f6465`
`Node`. Only the second group varies between the service and its characteristics.

The service UUID is in the advertising packet, so a client can filter on it. The advertised
device name is `Maxl-XXXX`, where `XXXX` is the **low 16 bits** of the nRF52840's factory
`DEVICEID` -- the last four hex digits of the serial, not the first. Node A's serial is
`0123ABCD4567EF01` and it advertises as `Maxl-ABCD` — the same identifier `firmware/nodes.ini` distinguishes nodes by, so
that two nodes in one rucksack are two different entries in the chooser.

> Note for anyone implementing the device side: a 128-bit UUID is handed to Nordic's stack
> **least significant byte first**. Written the human way round it produces a service no
> client can find, and nothing anywhere reports an error.

`CONFIG` and `STATUS` exist so a generic BLE tool (nRF Connect) can inspect a node without
implementing this protocol. Everything the app does goes through `TX`/`RX`.

MTU is negotiated up to 247. Assume 23 until negotiation completes.

### 1.1 Chunking

Every message is split into chunks of at most `MTU - 3 - 2` payload bytes.

```
byte 0 : F(1) | L(1) | msgId(6)     F = first chunk, L = last chunk
byte 1 : chunkIndex (0..255)
byte 2..: fragment
```

- `msgId` is a 6-bit rolling counter per direction. It exists to detect interleaving and
  loss, not to allow concurrency — **one message in flight per direction at a time.**
- A single-chunk message has both `F` and `L` set and `chunkIndex = 0`.
- The receiver drops a partial message if a chunk arrives out of order, if `msgId` changes
  mid-message, or if 5 s elapse between chunks. It does not attempt recovery; the sender
  re-sends the whole message.
- Maximum reassembled message: 4096 bytes. Larger is a protocol error.

### 1.2 Message layer

After reassembly:

```
byte 0  : opcode
byte 1  : txnId        echoed in the response; the phone increments it, wrapping freely
byte 2..: body
```

Every command produces exactly one response, `RSP_OK` or `RSP_ERR`, carrying the same
`txnId`. Events are unsolicited and carry `txnId = 0`.

All multi-byte integers are little endian. Strings are UTF-8, length-prefixed, never
null-terminated.

---

## 2. Authentication

Two tiers. Tier is enforced on the device, not by the client.

**Open** — permitted on any connection, including unbonded:
`GET_INFO`, `GET_STATUS`, `GET_BUDGET`.

**Bonded** — requires an authenticated, bonded connection with passkey entry, where the
passkey is shown on the node's own display: everything else, and in particular
`PROVISION_KEY`, `ROTATE_KEY`, `SET_CONFIG` and `FACTORY_RESET`.

A bonded-tier command on an unbonded connection returns `ERR_NOT_AUTHORISED`. It does not
silently no-op — revision 1's lack of this rule is exactly how you end up with a network
key settable by anyone in Bluetooth range.

Bonds are stored on the device, limited to 4 peers, and cleared by `FACTORY_RESET`.

---

## 3. Commands (phone → device)

| Op | Name | Tier | Body |
|---|---|---|---|
| `0x01` | `GET_INFO` | open | — |
| `0x02` | `GET_STATUS` | open | — |
| `0x03` | `SEND_TEXT` | bonded | `dst:u16, len:u8, utf8[len]` (len ≤ 48) |
| `0x04` | `GET_QUEUE` | bonded | `sinceCounter:u32, maxEvents:u8` |
| `0x05` | `ACK_QUEUE` | bonded | `upToCounter:u32` |
| `0x06` | `GET_CONFIG` | bonded | — |
| `0x07` | `SET_CONFIG` | bonded | `configVersion:u32, tlv[]` |
| `0x08` | `SET_TIME` | bonded | `unixSeconds:u32` |
| `0x09` | `REQUEST_FIX` | bonded | `timeoutSeconds:u16` |
| `0x0A` | `PROVISION_KEY` | bonded | `slot:u8, netId:u8, key[16]` |
| `0x0B` | `ROTATE_KEY` | bonded | `newSlot:u8` |
| `0x0C` | `FACTORY_RESET` | bonded | `magic:u32 = 0x5245534D` |
| `0x0D` | `GET_BUDGET` | open | — |
| `0x0E` | `LINK_TEST` | bonded | `dst:u16, count:u8, sf:u8 (0 = current)` |

`0x0F`–`0x1F` reserved. `0x20`+ reserved for Phase 8 tile transfer.

### Notes on the non-obvious ones

**`GET_QUEUE` / `ACK_QUEUE` are the whole sync model.** The device keeps an append-only
event journal on external flash. Each entry has its own **journal counter**, drawn from the
same monotonic, persistent, never-reused supply as frame counters (`CLAUDE.md` §2.1) but
counting entries rather than frames. `sinceCounter` and `upToCounter` below are journal
counters.

They have to be distinct. One transmitted frame produces several entries — `queued`, then
`delivered` or `undelivered` — and §4 requires all of them to reach the phone. If the
journal were keyed on the frame counter, the server's idempotency rule would discard every
entry after the first and lose precisely the state transitions it exists to carry. The
frame counter is a *field* in the event body, which is why `EVT_FRAME_TX_RESULT` carries a
`counter` at all. See `docs/decisions/0001-open-decisions.md` D10.
**The counter travels in `EVT_JOURNAL`, and only there.** A journal entry delivered in
answer to `GET_QUEUE` is wrapped in `EVT_JOURNAL` (§4), which carries the journal counter in
front of the event it wraps. A spontaneous event -- one the device emits because something
just happened -- is not wrapped and carries no counter, because there is nothing to
acknowledge yet: it will be offered again, wrapped, on the next `GET_QUEUE`.

That distinction is the resolution of D15. Before it, the counter was computed on the device
and thrown away before transmission, so no client could form `ACK_QUEUE` at all, and both
clients had to store everything and acknowledge nothing.

The phone asks for everything after the last counter it durably stored, writes those events
to its own IndexedDB, pushes them to the server, and only then sends `ACK_QUEUE`. The
device frees journal space up to that counter.

This ordering is deliberate. Acknowledging before the server has the data would lose events
whenever the phone dies between the two steps, and this is a device you carry into places
where the phone dies. The counter is also what makes the server's idempotency key work
(`CLAUDE.md` §4.3), so the same event arriving twice is harmless — which means erring
towards re-sending is always the right call.

**`SET_CONFIG` is versioned and acknowledged.** `configVersion` is the phone's monotonic
counter. The device stores it, applies the settings, and emits `EVT_CONFIG_APPLIED` with
the version it actually applied. A pushed config is not an applied config until that event
arrives; the dashboard's `config_versions.applied_at` is set from it.

**`SET_TIME` is not cosmetic.** The duty cycle budget is reconstructed against the RTC on
boot (`CLAUDE.md` §1.2). A node with no valid time is transmit-blocked. `SET_TIME` and a
GNSS fix are the two ways out of that state.

**`LINK_TEST` spends real airtime.** It transmits `count` frames and reports per-frame
RSSI/SNR/result. It is subject to the budget like everything else and will return
`ERR_BUDGET_EXHAUSTED` rather than queueing, because a delayed link test is a useless link
test.

### Config TLVs

`type:u8, len:u8, value[len]`. Unknown types are ignored and reported back in
`EVT_CONFIG_APPLIED` as unapplied, rather than failing the whole write.

| Type | Name | Value |
|---|---|---|
| `0x01` | `deviceName` | utf8, ≤ 16 B |
| `0x02` | `sniffIntervalMs` | u16, 250–10000 |
| `0x03` | `band` | u8 — 0 = g3 (default), 1 = g1 |
| `0x04` | `sfMode` | u8 — 0 = adaptive, 1 = fixed |
| `0x05` | `fixedSf` | u8, 7–12, only when `sfMode = 1` |
| `0x06` | `txPowerDbm` | i8 — clamped by the per-band ceiling, never trusted |
| `0x07` | `telemetryIntervalS` | u16, 0 = off |
| `0x08` | `beaconIntervalS` | u16 |
| `0x09` | `gnssFixTimeoutS` | u16 |

There is deliberately **no TLV for the duty cycle limit**. It is not configurable, in
firmware or over the air.

---

## 4. Events and responses (device → phone)

| Op | Name | Body |
|---|---|---|
| `0x81` | `EVT_FRAME_RX` | `counter:u32, src:u16, type:u8, rssi:i16, snr:i8, len:u8, payload[len]` |
| `0x82` | `EVT_FRAME_TX_RESULT` | `counter:u32, dst:u16, seq:u8, result:u8, attempts:u8, rssi:i16, snr:i8` |
| `0x83` | `EVT_STATUS` | see the status body below |
| `0x84` | `EVT_LOG` | `level:u8, len:u8, utf8[len]` — debug builds only |
| `0x85` | `EVT_CONFIG_APPLIED` | `configVersion:u32, appliedMask:u32, unappliedTypes[]` |
| `0x86` | `EVT_FIX` | `lat:i32, lon:i32, alt:i16, hdop:u8, fixAge:u8, satellites:u8` — `hdop` in **tenths**, 255 = unusable or unknown (D11) |
| `0x87` | `EVT_BUDGET` | `band:u8, usedMs:u32, limitMs:u32, nextTxUnix:u32` |
| `0x88` | `EVT_JOURNAL` | `counter:u32, opcode:u8, len:u8, body[len]` — a journal entry with its counter; see below |
| `0xC0` | `RSP_OK` | `opcode:u8, body[]` |
| `0xC1` | `RSP_ERR` | `opcode:u8, error:u8` |

`EVT_FRAME_TX_RESULT.result`: `0 = delivered`, `1 = undelivered`, `2 = queued`,
`3 = dropped by user`. State `2` may be followed later by `0` or `1` for the same counter —
the phone and the server must treat the journal as a log of state transitions, not as a
set of final outcomes.

### `EVT_JOURNAL` — a journal entry, with the counter that identifies it

```
counter:u32   the journal counter of this entry (§3)
opcode:u8     the event code it wraps: 0x81..0x87
len:u8        length of the wrapped body
body[len]     that event's body, byte for byte unchanged
```

**Only `GET_QUEUE` produces it.** Everything the device emits spontaneously stays exactly as
it was: an `EVT_FRAME_RX` that arrives because a frame arrived is an `EVT_FRAME_RX`. What
comes back out of the journal is wrapped, because that is the only case where a counter
exists to be acknowledged.

That makes the two copies of an event mean different things, and a client must treat them
differently:

| | Spontaneous | Out of `GET_QUEUE` |
|---|---|---|
| Shape | bare `EVT_*`, `txnId = 0` | wrapped in `EVT_JOURNAL` |
| Carries a counter | no | yes |
| What it is for | the live UI, while connected | the durable record |
| Client must store it | **no** | **yes** |

A spontaneous event is a courtesy: it lets the screen move the moment something happens.
It is **not** the delivery. The delivery is the wrapped copy, and a client that stored the
spontaneous one would have an entry it cannot acknowledge and cannot deduplicate against
the wrapped one that follows.

**Which makes one rule binding on the device: every event in the table above is journaled.**
Not "may be" -- an event type the device emits but never journals is a type the phone can
only catch by being connected at the instant it happens, and §4.2 of `CLAUDE.md` designs
for a phone that is usually not connected at all. `EVT_CONFIG_APPLIED` is the sharpest case:
`config_versions.applied_at` in the dashboard is set from it and from nothing else.

`EVT_LOG` is the one exception, and it is one because it exists only in debug builds and
carries nothing anybody projects.

A client that reads the wrapper gets, for every entry, the pair the server's idempotency key
needs: `(journal counter, event)`. `CLAUDE.md` §4.3 puts that key on
`(device_id, journal_counter, direction)`, and before `EVT_JOURNAL` the middle column had no
source on the wire — the counter existed only as an index inside the device's own storage.

Three rules, because the shape invites two mistakes:

- **The wrapped body is not re-encoded.** `len` is the wrapped event's own body length; a
  client can hand `body[len]` to the same decoder it already has.
- **`len` bounds the body at 255**, which every event in §4 fits: the longest is
  `EVT_FRAME_RX` with a 48-byte payload, 59 bytes in total.
- **Unknown wrapped opcodes are kept, not dropped.** A client that does not understand
  `body` still knows the counter, and must store the entry and count it towards `ACK_QUEUE`.
  Dropping it would free journal space for something nobody ever saw. This is the rule that
  makes a future event type safe to add.

### Response bodies

`RSP_OK` is `opcode:u8, body[]`. What that body holds was missing from revision 1 of this
document for every single command, which made the claim in the header — that a client can be
written against this document without reading the TypeScript — false. Found on 2026-08-31
while doing exactly that. The bodies:

| Command | `RSP_OK` body |
|---|---|
| `GET_INFO` | `bridgeProtocol:u8, nodeId:u16, fwMajor:u8, fwMinor:u8, fwPatch:u8, wireVersion:u8` — 7 bytes |
| `GET_STATUS` | the 17-byte status body below |
| `GET_BUDGET` | `band:u8, usedMs:u32, limitMs:u32, nextTxUnix:u32` — 13 bytes, the same shape as `EVT_BUDGET` |
| `GET_QUEUE` | `count:u8` — **see below, this one is not what it looks like** |
| `GET_CONFIG` | the current config as TLVs, same encoding as `SET_CONFIG`'s |
| `SEND_TEXT`, `ACK_QUEUE`, `SET_CONFIG`, `SET_TIME`, `REQUEST_FIX`, `PROVISION_KEY`, `ROTATE_KEY`, `FACTORY_RESET`, `LINK_TEST` | empty |

**`GET_QUEUE` does not return the events.** It emits them first, each as its own
unsolicited `EVT_JOURNAL` message on `TX` with `txnId = 0`, and *then* answers `RSP_OK` with
the count it emitted.

Before `BRIDGE_PROTO = 2` the bare events went out here instead, without their counters.
That is D15, and it is why this changed.

A client must therefore collect events as they arrive and use the response only to know how
many to expect and whether to ask again — §5 step 5 loops until the count is less than
`maxEvents`. A client that waited for a response containing the events would wait for ever.
The device caps `maxEvents` at 32 whatever is asked for.

`LINK_TEST` answers empty and reports per-frame results afterwards as events, for the same
reason: the frames take airtime measured in seconds, and a response cannot wait for them.

### The status body

Returned by `GET_STATUS` and carried unchanged by `EVT_STATUS`. Revision 1 of this document
defined `EVT_STATUS` as "see `GET_STATUS` body" and then never gave that body, which left
the one field the `STATUS` screen and the dashboard both need — the budget — unspecified.
Fixed length, 17 bytes, little endian like everything else:

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `batteryMv` | u16 | millivolts at the cell |
| 2 | `uptimeS` | u32 | seconds since boot |
| 6 | `queueDepth` | u8 | outbound messages not yet delivered |
| 7 | `band` | u8 | 0 = g3, 1 = g1 — which band the budget below refers to |
| 8 | `budgetUsedMs` | u32 | airtime spent in the rolling hour (`CLAUDE.md` §1.2) |
| 12 | `budgetLimitMs` | u32 | 360000 on g3, 36000 on g1 |
| 16 | `flags` | u8 | bit0 `timeValid`, bit1 `keyProvisioned`, bit2 `gnssPowered`, bits 3–7 reserved |

`budgetUsedMs` and `budgetLimitMs` are repeated from `EVT_BUDGET` on purpose. `EVT_BUDGET`
is emitted when the budget *changes*; this is what a client gets by reading the `STATUS`
characteristic cold, with no journal and no subscription — which is what a generic BLE tool
does (§1), and what the phone does in step 3 of §5 to have something on screen within a
second.

`flags` bit 0 is not cosmetic either. A node with no valid time is transmit-blocked
(`CLAUDE.md` §1.2), and a UI that cannot say so shows a device that simply refuses to send
with no explanation.

### Error codes

| Code | Name | Meaning |
|---|---|---|
| `0x01` | `ERR_UNSUPPORTED` | unknown opcode — the client should degrade, not fail |
| `0x02` | `ERR_BAD_LENGTH` | body length wrong for the opcode |
| `0x03` | `ERR_BAD_PARAM` | value out of range |
| `0x04` | `ERR_NOT_AUTHORISED` | bonded tier required |
| `0x05` | `ERR_NO_KEY` | no network key provisioned |
| `0x06` | `ERR_BUDGET_EXHAUSTED` | duty cycle; `EVT_BUDGET` carries the release time |
| `0x07` | `ERR_QUEUE_FULL` | outbound queue full |
| `0x08` | `ERR_NO_TIME` | RTC invalid, node is transmit-blocked |
| `0x09` | `ERR_BUSY` | another message in flight |
| `0x0A` | `ERR_STORAGE` | flash write failed |

---

## 5. Connection lifecycle

The client must implement exactly this sequence, because the foreground-only constraint
means every connection is short and must be productive:

1. Connect, negotiate MTU.
2. `GET_INFO` — check `BRIDGE_PROTO` compatibility before anything else.
3. `GET_STATUS`, `GET_BUDGET` — populate the UI immediately, so the user sees something
   within a second of opening the app.
4. `SET_TIME` if the node reports no valid time.
5. `GET_QUEUE` in a loop until it returns fewer events than `maxEvents`. Every entry
   arrives as `EVT_JOURNAL` and carries its own counter; store the counter with the event.
6. Push to server. On success, `ACK_QUEUE(highest counter durably stored)` — and not before
   the store has it. A client that cannot name a counter for an entry must not acknowledge
   it: the device frees journal space up to whatever is acknowledged, and what it frees is
   gone.
7. Flush any outbound `SEND_TEXT` the user queued while offline.
8. Stay connected while the app is in the foreground; subscribe to `TX` notifications.

### Step 7 in detail — what a refused text means

The phone is the only place an outbound text can wait. The node queues in the other
direction (§3), but nothing queues *for* the node, so a message written with no connection
lives in the client's own durable store until a connection takes it. A client that only
offers to send while connected forces the user to be holding the node to write to it, which
is the opposite of what `CLAUDE.md` §4.2 designs for.

Step 7 has **three** outcomes per text, and a client must distinguish them. The line is not
which error it is but **what the error is about**:

| Node's answer | What it is about | What the client does |
|---|---|---|
| `RSP_OK` | — | remove from the outbox; the node's own delivery states take over |
| `ERR_BAD_LENGTH`, `ERR_BAD_PARAM`, `ERR_UNSUPPORTED` | **the message** | remove it and report the refusal |
| everything else | **the node's current state** | keep it; the next connection offers it again |

A refusal about the message will be repeated for ever: the same bytes produce the same
answer, so keeping it spends a write on every sync until somebody notices. A refusal about
the node's state will not. `ERR_BUDGET_EXHAUSTED` is the ordinary answer at a 10 % duty
cycle, not a fault (`CLAUDE.md` §1.2). `ERR_NO_TIME` means the node is transmit-blocked
because its clock is invalid, and step 4 of this very sequence is one of the two ways out of
that. `ERR_NOT_AUTHORISED` and `ERR_NO_KEY` are answered by bonding and by provisioning.
Dropping any of them would throw away something the user wrote for a reason that has nothing
to do with what they wrote.

The client counts attempts per entry so a text that keeps being refused can be surfaced. It
does **not** discard one on a count: that would be the same data loss, arrived at more
slowly.

A refusal **must not abort the connection.** By step 7 the journal has already been fetched
and pushed, and throwing away a completed sync because of an answer the node was entitled to
give means fetching it all again next time. A transport failure is different — the
connection is gone, so the remaining texts have not been refused by anybody and must stay
exactly where they are.

On disconnect, the client does nothing clever. It shows when the last sync happened and how
many events the node still holds. There is no retry loop in the background — see
`CLAUDE.md` §4.2 for why service workers and wake locks do not help here.

---

## 6. Things this protocol deliberately does not do

- **No concurrency.** One message in flight per direction. A BLE link to a sleeping
  microcontroller is not the place to discover a reordering bug.
- **No compression.** The bottleneck is BLE connection time, and the payloads are already
  binary and small.
- **No streaming of the radio.** The phone never gets raw LoRa frames in real time; it gets
  journal events. Real-time monitoring is a debug feature (`EVT_LOG`), not a product one.
- **No key material ever leaves the device.** `PROVISION_KEY` writes; there is no read.

---

## 7. What this document has had to be told

`BRIDGE_PROTO` went to 2 on 2026-08-31, for `EVT_JOURNAL` and that alone. Everything else
below changed no byte on the wire: each entry is something the document did not say and an
implementer would have had to guess — and every one of them was found by writing a **second**
client (Kotlin, Phase 9) against this text rather than against the PWA's source.

| Date | What was missing |
|---|---|
| 2026-08-31 | The GATT service and characteristic UUIDs. They existed only in TypeScript and C++ |
| 2026-08-31 | The body of every response. §3 listed commands and said nothing about what came back |
| 2026-08-31 | `GET_QUEUE` emits the events first and *then* answers with the count. A client waiting for a response containing the events waits for ever |
| 2026-08-31 | The device caps `maxEvents` at 32 whatever is asked for |
| 2026-08-31 | The advertised device name (`Maxl-XXXX`, the low 16 bits of the DEVICEID) |
| 2026-08-31 | §5 step 7: which refusals leave a queued text in place and which discard it, and that neither aborts the connection |
| 2026-08-31 | `EVT_STATUS` referred to a `GET_STATUS` body that was defined nowhere. Now §4 |
| 2026-08-31 | The journal counter itself. `GET_QUEUE` and `ACK_QUEUE` were specified in terms of a counter that **no event on the wire carried** — D15. Resolved by `EVT_JOURNAL`, and the only change here that moved `BRIDGE_PROTO` |

D15 is the one that cost a wire version, and it is worth saying why it survived so long: the
PWA stored only `EVT_FRAME_RX`, whose body happens to carry a frame counter that coincides
with its journal counter, and acknowledged everything anyway. Two defects that hid each
other. The Kotlin client, written against this text rather than against that code, could not
form `ACK_QUEUE` at all — and that was the question that found both.

**The lesson, for whoever writes the third client:** a protocol document that only one
implementation was ever written against is not verified, only transcribed. If something
here is ambiguous, that is a defect in this file — fix it here first, then in the code.
