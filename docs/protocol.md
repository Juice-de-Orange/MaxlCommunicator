# Radio protocol — change log

**The normative wire format is `CLAUDE.md` §2.** It is not duplicated here, deliberately:
two copies of a byte layout drift apart, and the one in the spec is the one Claude Code
reads while writing the link layer.

This file records what changed, when, and why — the part a format description cannot carry.
The rules for *when* a version may be bumped and how a mismatched pair behaves are in
`versioning-and-updates.md` §1.

---

## `ver = 1`

Initial format.

- 12-byte header: `ver|type`, `netId`, `src`, `dst`, `counter:u32`, `seq`, `flags`.
  With the 4-byte MIC that is 16 bytes of overhead per frame. Revision 2 of `CLAUDE.md`
  originally said "16-byte header" here and "9 header + 4 MIC" in 1.4; neither matched
  the field list. Corrected 2026-08-30, see `docs/decisions/0001-open-decisions.md` D7.
  The wire format itself never changed, so `ver` stays at 1.
- 4-byte MIC. Header authenticated as AAD, payload encrypted and authenticated.
- AES-128-CCM, nonce derived from `src` and the persistent 32-bit `counter`.
- Frame types: `BEACON`, `DATA`, `ACK`, `TELEMETRY`, `POSITION`, `TEXT`, `CONFIG`.
- Default modulation: SF9 / BW125 / CR4/5 at 869.575 MHz (g3).

Design notes worth keeping:

The 32-bit counter exists because an 8-bit sequence number cannot safely feed a CCM nonce —
it wraps after 256 frames and resets on reboot, and nonce reuse under a shared key breaks
CCM open. The counter is therefore persistent, monotonic, and survives factory reset.

`seq` was kept as a separate 8-bit field rather than reusing the counter's low byte, so that
the ARQ layer can wrap freely without any implication for the crypto.

`flags` bits 3–7 are reserved on purpose. Additive changes should go there or into a new
frame type — an unknown frame type is safely ignorable within the same `ver`, so it costs
nothing, whereas bumping `ver` means flashing every node in the network in one session.

### Defined after the fact, without a format change

Two fields were in the layout without their meaning being written down anywhere. Both
were filled in **without bumping `ver`**: no byte moved, no field width
changed, no node has to be reflashed because of it. In each case what was defined was an
interpretation that had been missing.

| Date | Field | Definition | Rationale |
|---|---|---|---|
| 2026-08-31 | `EVT_STATUS` body | 17 bytes, layout in `docs/bridge-protocol.md` §4 | `EVT_STATUS` referred to a `GET_STATUS` body that did not exist anywhere |
| 2026-08-31 | `POSITION.hdop` | **tenths**, capped at 255; 255 also means "unknown" | `uint8` without a scale; as an integer every usable fix would be a `1`. See D11 |
| 2026-09-01 | ARQ retries | A retry is rebuilt under a **fresh counter**, with the RETRY flag set; the seq stays | No format change — header, fields and flags were always specified this way. What is defined is the counter consumption: a byte-identical retransmit (same counter) is an attack as far as the receiver's replay window is concerned and would never be re-ACKed; only "same seq, new counter" is recognisable as a repetition (link/replay.h). Every transmission, including a retry, consumes a counter |

`hdop` in detail: 0.9 becomes `9`, 1.26 becomes `13`, everything from 25.5 up becomes `255`. A missing
or unparseable field also becomes `255` — i.e. the *worst* value, not `0`. With `0`
a missing value would read on the `POSITION` screen as a perfect fix.

---

## Template for future entries

```
## `ver = N`   (YYYY-MM-DD, firmware vX.Y.Z)

What changed:
Why it could not be done additively:
Nodes flashed together on:
```
