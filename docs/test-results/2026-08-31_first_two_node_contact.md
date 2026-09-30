# The first radio contact — and two findings in the same numbers

| | |
|---|---|
| Date | 2026-08-31, late evening |
| Node | **A** = node 1 (sender, on battery), **B** = node 2 (receiver, on the cable) |
| Sketch | 19, `firmware/src/bringup/sketch_19_link.cpp`, both boards the same build |
| Key | from `MAXL_DEV_KEY`, identical on both (`key.shared = 1`) |
| Modulation | SF9, 869.575 MHz, +22 dBm, sniff interval 500 ms |
| Runtime | 804 s on the receiver side |

## What is proven

```
RESULT rx.frames           = 1
RESULT rx.mic_failures     = 0
RESULT rx.replays_rejected = 79
RESULT peer.last_snr       = 11
RESULT peer.last_sf        = 9
```

**Two devices have talked to each other for the first time.** A frame from node 1 arrived at
node 2 and verified. That proves in one go: the shared key via `MAXL_DEV_KEY`, the framing
from `link/frame`, AES-CCM over the real air instead of over the host, the addressing, and
that both sides derive the same preamble from the same function.

**And the replay window works.** 79 repetitions rejected, zero MIC failures. This is not a
side finding: gate 2.3 requires exactly that, albeit with a deliberately replayed frame
rather than with repetitions that came from the ARQ itself.

## Finding 1 — 1 accepted, 79 rejected

Twenty messages with one first attempt and at most three retries each are at most eighty
transmissions — and exactly eighty arrived. Except they carried **a single** counter: one
message was accepted, all others were recognised as repetitions.

Twenty different counters would have been expected, i.e. twenty acceptances and sixty
rejections. One counter means: node 1 sent the same message again and again and never moved
on to the second one.

Two readings, both unproven and to be told apart with the device:

1. **The ACK does not reach node 1.** Then the message stays `InFlight`, `feed()` waits for
   an empty queue and never feeds the next one — and `kMaxRetries = 3` would have to set it to
   `Undelivered` after three attempts, which apparently does not happen.
2. **The ARQ does not count attempts.** Then it retries without end, and the limit from
   `CLAUDE.md` §2.4 is as ineffective as the budget was until this evening.

The difference is the **sender side's** report, which could not be collected in this run:
node 1 was on battery because only one USB socket works.
`tx.delivered`, `tx.undelivered` and `tx.requested` from node 1 settle it in one reading.

## Finding 2 — `peer.last_rssi = 0` with `peer.last_snr = 11`

An SNR of 11 dB is clean reception at desk distance. An RSSI of exactly zero is not a
measurement but the initial value of an untouched field: at this distance about −30 to
−60 dBm would be expected.

This directly affects **gate 2.4**, which requires that the RSSI and SNR values in the ACK
match what the receiver logged. A value that is never filled can match nothing. The chain to
check is `hal::RxInfo` → `Node::onFrameReceived` → `recordReceivedFrame` → `PeerTable`, and
whether `radio_sx1262` reads out the RSSI at all.

## What this run is not

**Gate 2.1 is not passed.** It requires a hundred frames per SF with zero MIC failures; here
a single distinguishable frame arrived. The MIC record is flawless, the count is not.

**Gate 2.4 is not passed.** See finding 2.

**The setup was not clean.** The cables were re-plugged several times during the run because
only one socket works; node 1 saw at least one restart in the process. The next run belongs
on a hub, with both boards connected to the host at the same time, and then both reports are
readable at once — which resolves finding 1 in minutes instead of in guesses.

---

## Addendum, 2026-09-01 (night session, without device): both findings are settled from the code

The report above stays as it was — the interpretations are partly refuted, and from the
sources, not from a new run.

**On finding 1: "a single counter, repeated 79 times" could not be proven and is wrong.**
`Duplicate` (same seq, new counter) and `Replayed` (counter inside the window) both only
increment `replaysRejected_`; the number 79 cannot tell the two apart. In fact the counters
were different — the rejection happened on the **seq**: `MessageQueue::submit()` never set
`QueuedMessage::seq` (zero-init), so every message flew as `seq = 0`, and the receiver's
(src, seq) dedupe discarded everything after the first frame as an ARQ retry.
20 messages × 4 transmissions = 80, one accepted, 79 discarded — the report's arithmetic
was right, its attribution was not.

**Neither of the two readings of finding 1 was the cause.** The ARQ counts correctly (reading
2 refuted, `link/arq.cpp`), and the ACK could not "get lost" (reading 1), because
`app::Node` **never built an ACK**: `FrameType::Ack` appeared in `node.cpp` exactly once
— consuming. The ACK path existed only in the simulation. Every message ran its four attempts
as planned and became `Undelivered`; that is exactly why node 1 moved on cleanly to the next
one.

**On finding 2: the zero comes from RadioLib 7.7.1.** `getRSSI(true)` reads the wrong byte
from `GetPacketStatus` (bits 7:0 = SignalRssiPkt instead of bits 23:16 = RssiPkt); `getSNR()`
reads bits 15:8 correctly — hence SNR 11 next to RSSI 0 from the same status word. Source
evidence in `hal/radio_sx1262.cpp`; the on-air evidence (raw word in the sketch 19 report,
`radio.packet_status_raw`) is pending until a frame is received again.

Fixed on 2026-09-01 (branch `session-6-night`): seq assignment with persistence, ACK send path
checked against the budget, retries under a fresh counter with the RETRY flag, RSSI from the
right byte. Evidence on the host: the two-node app simulation delivers for the first time
(1 transmission, 1 ACK, `delivered = 1`; with a discarded ACK: 1 retry, 1 re-ACK, exactly one
copy in B's journal). The on-air evidence needs both nodes on the new build — gate 2.4 stays
open until then.
