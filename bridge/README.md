# bridge/ — phone bridge

Implementation of `docs/bridge-protocol.md`. That document is normative and
**client-independent**: the native Android client from Phase 9 is written against it without
reading this TypeScript code. Nothing here may assume that the client is a browser —
and the protocol part does not.

## Status

**Protocol core and PWA are built**, 95 tests green. What is missing is missing on the hardware side, not
in the code.

| What | State |
|---|---|
| `EVT_JOURNAL` / `BRIDGE_PROTO 2` (D15, option B) | done — wrapped entries are stored and only acknowledged with `ACK_QUEUE` after the server push; spontaneous ones only drive the display |
| Chunking and reassembly (§1.1) | done, checked against shared vectors |
| Message layer, opcodes, TLVs, error codes (§1.2, §3, §4) | done, tested |
| Radio payload codecs (`CLAUDE.md` §2.2) | done, tested |
| `Transport` seam + mock device | done; the whole flow from §5 can be played through without hardware |
| Connection flow §5 as a state machine | done, tested |
| UI, IndexedDB store, server push | **done** — Gate 6.9 passed |
| Outbox for text without a connection (§5 step 7) | done, tested |
| Configuration fetch (`GET /api/config`) | done, played through against the running stack |
| `WebBluetoothTransport` | written, **never run against a real device** — needs a browser, HTTPS and a node with BLE firmware |

**The disconnected view is the main view.** Last sync and pending events are shown
before anything is connected — that is Gate 6.9, and it is the state the
user sees most of the time (`CLAUDE.md` §4.2: the connection drops as soon as the tab goes into
the background, and nothing helps against that).

`IEventStore` keeps the seam open; next to the IndexedDB implementation there is an
in-memory variant for the tests. `Outbox` is the same design for the opposite direction.

**And you can now write while disconnected, too.** Until now the send button was only visible
while a connection was up — honest, but it meant you had to hold the node in your hand
to write to it. The exact opposite of what §4.2 is built for. A message written without a
connection now lies durably in IndexedDB, is shown as *on the phone*
and goes out on the next sync.

The interesting part is the rejection, and it has been normative in
`docs/bridge-protocol.md` §5 since 2026-08-31. The dividing line is **not which error it is, but
what it is about**: `ERR_BAD_LENGTH`, `ERR_BAD_PARAM` and `ERR_UNSUPPORTED` concern
the message — the same bytes get the same answer for ever, the message is dropped
and the reason is reported. Everything else concerns the state of the node, and that changes:
at 10 % duty cycle `ERR_BUDGET_EXHAUSTED` is the normal answer, `ERR_NO_TIME` is cleared by
step 4 of the same flow, `ERR_NOT_AUTHORISED` and `ERR_NO_KEY` are cleared by bonding and
provisioning. Those stay pending.

And no rejection aborts the sync: by then the journal has already been fetched and pushed, and
throwing it away because the node gave a valid answer would mean fetching it again
next time.

### A field with a new meaning

`Position.hdop` is **tenths**, capped at 255, and 255 also means "unknown". `CLAUDE.md`
§2.2 only said `hdop uint8` and left the scale open. The field is currently not displayed
anywhere — whoever starts doing so divides by 10. See **D11**.

## Running

```bash
cd bridge
npm ci
npm run typecheck
npm test
```

Node 22 LTS, pinned in `.nvmrc` (decision D6 — some machines run Node 24).

## The rule this is built around

`docs/bridge-protocol.md` §3: the phone fetches the events, writes them locally, **pushes them
to the server, and only then** comes `ACK_QUEUE`. The other way round, events would be lost as soon as the
phone dies between the two steps — "and this is a device you carry into places
where the phone dies".

`BridgeSession.syncJournal()` takes the server push as a callback and sends
`ACK_QUEUE` only if it succeeds. If it throws, the device keeps everything. The idempotency key
on the server (`CLAUDE.md` §4.3) makes an event delivered twice harmless — that is why
sending twice is always the right choice.

## What is deliberately missing

- **No reconnect loop, no service worker, no wake lock.** The connection drops as soon as
  the tab goes into the background, and none of these means keeps a GATT connection
  alive (`CLAUDE.md` §4.2). Building them would be a convincing imitation of
  background sync that fails in the field. The native Android client with a foreground service is
  Phase 9.
- **iOS is not supported.** Apple does not implement Web Bluetooth. `isSupported()`
  returns a dedicated text for it, so that the UI can say so instead of failing silently.
- **No TLV for the duty cycle.** `docs/bridge-protocol.md` §3 says explicitly that there is
  none. A UI for it would be a UI for something the device refuses.

## Test vectors

`test/` reads `test-vectors/*.json` — the same files that `firmware/test` reads. Written by hand
from the specification, not generated. Both sides agree with the vectors,
so they agree with each other; that is the only way
"client-independent" becomes verifiable rather than merely claimed.
