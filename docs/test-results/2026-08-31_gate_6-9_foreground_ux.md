# Phase 6 — Gate 6.9 Foreground behaviour in the UI

| | |
|---|---|
| Date | 2026-08-31 |
| Node | none — this gate is UX, not radio |
| Software | `bridge/` at `5794635` |
| Measuring equipment | 12 test cases against the rendered UI (jsdom), plus a look in the browser |

## Result: PASS

Gate 6.9 requires: *"Last-sync time and pending count visible without connecting."*

| Check | Result |
|---|---|
| Last sync visible without a connection | **PASS** |
| Number of events waiting on the device visible without a connection | **PASS** |
| "on the device" and "not yet sent to the server from here" shown separately | **PASS** |
| Reason for the connection drop in the background named explicitly | **PASS** |
| "never synchronised" state named as such, no empty field | **PASS** |
| iOS is named, not silently left out | **PASS** |
| No connect button on a platform where it cannot work | **PASS** |
| Status display stays visible on unsupported platforms too | **PASS** |

### Addendum, 2026-08-31 afternoon: the checked view has changed

Since the outbox was added, the disconnected view also shows the send field — without a
connection labelled "Queue" instead of "Send", and with the number of messages waiting on the
phone. This belongs here because this gate checks exactly this view, and a report that
describes a view that no longer exists is worthless.

The first five criteria above are not affected by this and are still held unchanged by
`bridge/test/ui.test.ts` — each of them is a test case of its own there, and the suite ran
green after the change. The addition goes in the same direction as the gate: the disconnected
view is the main view, so you also have to be able to do something in it.

## Observations

**Two numbers, not one.** "Waiting on the device" and "not yet pushed to the server from
here" are different problems: you solve the first by moving into radio range, the second
needs a network. A combined number does not say which of the two you have.

**The platform message hangs on a code, not on an English sentence.**
`isSupported()` still returns an English `reason` for the log and additionally a stable
`code` field. The UI translates via the code — otherwise a message string would have become
load-bearing, and the first rewording would have silently broken the translation.

Looking at it in the browser revealed two bugs that no test had found: the platform message
was in English, and without Web Bluetooth an empty card was left standing. Both fixed. That
is the argument for really looking at the UI and not just testing it.

## What is still open

All other Phase 6 gates (6.1–6.8) need a device with a running GATT server. It has not been
written yet — Phase 6 is completely missing from the firmware. The client is finished and
has been played through against the mock transport.
