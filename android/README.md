# android/ — native client (Phase 9)

`CLAUDE.md` Phase 9: *"Kotlin client against `docs/bridge-protocol.md`, with a foreground
service so sync happens with the phone in a pocket. The PWA stays as the zero-install path
and as the reference implementation of the protocol."*

## The point of the exercise

`CLAUDE.md` §4.2 makes a verifiable claim:

> the bridge protocol is specified independently of the client in `docs/bridge-protocol.md`
> — framing, chunking, command set, event set — **such that a Kotlin client can be written
> against the document without reading the TypeScript.**

`protocol/` is exactly that attempt. It was written on 2026-08-31 **exclusively** from
`docs/bridge-protocol.md` and `CLAUDE.md` §2.2.

**The claim did not hold.** The attempt found five gaps without which no client is
possible:

| What was missing | Consequence |
|---|---|
| **The GATT UUIDs** | §1 named four characteristics and not a single UUID. A client cannot connect. |
| **All response bodies** | `RSP_OK` was described as `opcode:u8, body[]`, with the content given for **no** command. But `GET_INFO`'s response is §5 step 2. |
| **What `GET_QUEUE` returns** | It does **not** deliver the events — it first sends them individually as unsolicited events and then answers only with the count. A client that waits for a response containing the data waits for ever. |
| **The upper limit of `maxEvents`** | The device caps at 32, no matter what is asked for. |
| **The device name** | `Maxl-XXXX`, which a scan filters on. |

All five are now in the document. Cross-checked that the **two existing**
implementations do the same thing — otherwise I would have described the protocol after one side.

## Modules

| Module | What | Status |
|---|---|---|
| `protocol/` | Pure Kotlin/JVM: chunking, message layer, opcodes, TLVs, status blob, radio payloads, GATT UUIDs | **done, 54 tests green** |
| `app/` | BLE scan and GATT client, the flow from §5, an event store, the foreground service, a UI | **32 tests, APK built — never run on a phone** |

### What "compiles" means here and what it does not

`:app` builds with `allWarningsAsErrors` and produces a 3.2 MB debug APK. The BLE behaviour,
the permission dialogs, the behaviour of the foreground service when switching to the
background — **none of it is tested**, and without a phone it cannot be.

**Two exceptions, and they are the important ones.**

`sync/BridgeSession.kt` is §5 of the protocol and nothing else — which step runs when,
and what makes one fail. None of that is Bluetooth. So that this also holds in the code,
the class depends on `sync/DeviceLink.kt` instead of `ble/MaxlGatt.kt` — **the same cut that
D3 makes in the firmware**, where `link/` depends on `hal::IRadioLink` and therefore runs on the
host. Against it, `FakeNode` plays a device, and **18 tests** check the order, the
abort on a foreign protocol version, `SET_TIME` only with an invalid clock, the
send-first-then-count semantics of `GET_QUEUE`, the unpacking of `EVT_JOURNAL`, that
`ACK_QUEUE` only goes out after a successful server push (D15, option B), and that a message
that failed on the budget stays pending, whereas one with `ERR_BAD_PARAM` does not.

The cut paid off immediately: `run()` set `error = null` at the end and thereby erased
exactly the rejection that the previous step had produced. A message that failed on the duty cycle
would have vanished without a trace for the user.

`sync/EventStore.kt` touches nothing from Android, only `java.io.File` — so it has
**9 JVM tests**:

```bash
android/tools/test.sh :app:testDebugUnitTest
```

They check exactly what must not go wrong: that appended data survives a restart,
that a record half-written by a dying phone is discarded and everything before it
kept, that the same entry written twice is stored only once, that a u32 counter at the upper
edge does not come back negative — and that a store that cannot write **says so**
instead of hiding it. A store that swallows a write error would let the caller
acknowledge events it has not kept.

Whatever can be tested without Android belongs in `:protocol` and is there. That is the reason for the
cut.

| File | Role |
|---|---|
| `ble/MaxlGatt.kt` | Scan filter on the service UUID, GATT client, MTU negotiation, chunking in both directions. Knows nothing about commands. |
| `sync/EventStore.kt` | Append-only file. A half-written record at the end is discarded, everything before it kept — the device offers the rest again anyway. |
| `sync/BridgeSession.kt` | §5, step by step. Blocking requests, because the protocol only allows one message per direction anyway. |
| `app/SyncService.kt` | The foreground service. The reason for Phase 9. |
| `app/MainActivity.kt` | Four lines of text and a button. **The disconnected view is the main view** — the same rule as Gate 6.9 in the PWA. |

### `ACK_QUEUE`, since D15 option B

`EVT_JOURNAL` (BRIDGE_PROTO 2) now carries the journal counter on the wire, so the
client forms a correct `upToCounter` and acknowledges — **but only after the server has the
events durably** (`sync/HttpServerPush.kt`, real instead of a stub since 2026-09-01; without a
configured server base URL and token the honest placeholder keeps everything on the
phone and acknowledges nothing). It stores **every** event type, not only `frame-rx` —
including unknown opcodes, whose counter remains readable (§4).

`protocol/` depends on **nothing** — no Android, no Bluetooth, no coroutines. That is
the mirror image of §4.2: just as nothing in firmware and server may assume that the client is
a browser, nothing here may assume that it is an Android app. In practice this means
that the tests run on any JVM.

## Running

```bash
android/tools/test.sh                       # :protocol:test, 54 cases
android/tools/test.sh :app:testDebugUnitTest   # §5, store and server push, 32 cases
android/tools/test.sh :app:assembleDebug      # the APK
android/tools/test.sh clean
```

The task is passed through to Gradle unchanged, so anything Gradle knows works.

In a container for the same reason as `firmware/tools/hosttest.sh`: the development machine has
a JRE and no JDK, and installing one requires root. Docker does not.

## The shared test vectors

`test-vectors/` is now read by **three** implementations: the PWA parses the JSON,
the firmware compiles a generated C++ header, and this module generates a
Kotlin object from it (`tools/gen_vectors.py --lang kotlin`). Written by hand from the specification,
generated from **no** implementation — a generator would bake one side's bugs
into the other side's test and make the cross-check worthless.

It paid off immediately: the vector `evt_frame_tx_result` found an arithmetic error in
the length of this structure (12 bytes, not 15) before any device was anywhere
near.

## Traps that cost time

- **Kotlin block comments nest**, unlike in C and C++. A `/*` in a
  KDoc comment — for instance in a file path with an asterisk — opens a second comment
  that never closes, and the compiler complains at the **end of the file**.
- **`jvmToolchain(17)` requires a JDK 17** and fails on a machine with 21. What
  the later Android app needs is Java 17 *bytecode* — that is a target, not a compiler.
  So `jvmTarget`, not `jvmToolchain`.
- **Kotlin's `Byte` is signed.** `0xC0` read as a `Byte` is −64. In a
  binary protocol that is the most reliable source of bugs there is; that is why
  every read in `Bytes.kt` goes through `toInt() and 0xFF`, and nothing outside this file touches
  a raw `Byte`.

## What comes next

A phone. Then gates 6.1 and 6.2 against `Maxl-ABCD`, and the server push, which
is currently a `NoServerYet` that honestly returns `false` — a stub that said `true`
would make the phone forget events it has sent nowhere.

The **foreground service** is the whole reason Phase 9 exists. `CLAUDE.md` §4.2: the Web Bluetooth connection drops as soon as the
tab goes into the background, and neither service workers nor wake locks keep a
GATT connection alive. A foreground service does.

The connection flow is written as a step-by-step list in `docs/bridge-protocol.md` §5
and can therefore be implemented directly. The order in step 6 is the important one: first push to the
server, **then** `ACK_QUEUE` — the other way round, events would be lost as soon as the
phone dies between the two steps, "and this is a device you carry into places
where the phone dies".
