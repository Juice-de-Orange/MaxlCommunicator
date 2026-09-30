# Versioning and updates

Three independent version numbers. Confusing them is how a two-node network ends up
half-upgraded and silent.

| Version | Where | Range | Changes when |
|---|---|---|---|
| Wire protocol `ver` | nibble in every radio frame header | 0–15 | the on-air format changes |
| `BRIDGE_PROTO` | `GET_INFO` over BLE | u8 | the phone↔device protocol changes |
| Firmware version | semver + git short hash, in `GET_INFO` and on the `STATUS` screen | — | every release |

---

## 1. Wire protocol version

### The mismatch problem

The frame header is authenticated as AAD, so a node receiving a frame with a different
`ver` gets a MIC failure, which is indistinguishable from corruption or a wrong key. That
would leave a half-upgraded pair showing "no peer" with no explanation, and you would spend
an evening on it.

**Rule:** `ver` is read from the cleartext header *before* the MIC is checked, for
diagnostics only. If it does not match, the frame is discarded, but the peer is recorded in
peer state as `VERSION_MISMATCH` and the `PEERS` screen shows the observed version next to
the node's own. Nothing else about the frame is trusted — no payload is parsed, no state is
updated beyond that one flag.

This costs a few lines and turns a silent failure into a screen that tells you what to do.

### Deployment rule

With two nodes there is no rolling upgrade. A release that bumps `ver` is flashed to **all
nodes in the same session**, and the release notes say so explicitly. Do not carry a node
into the field on a version its peer does not speak.

Corollary: bump `ver` reluctantly. Prefer additive changes that fit in the reserved bits of
`flags` or in a new frame type, since unknown frame types are safely ignorable within the
same `ver`. Reserve the nibble bump for changes to the header layout, the crypto
construction, or the nonce.

### Version history

| `ver` | Date | Change |
|---|---|---|
| 1 | — | Initial format. 16-byte header, 32-bit persistent counter, AES-128-CCM. |

Append here on every bump, with the reason. `CLAUDE.md` §2.1 remains the normative
description of the current format.

---

## 2. Bridge protocol version

Looser rules, because the phone is upgraded independently and often.

- **Additive only within a major.** New opcodes, new TLV types, new event types and new
  trailing fields on existing bodies are all fine.
- Clients must tolerate `ERR_UNSUPPORTED` and degrade. A client that sees an unknown event
  opcode discards it and continues; it does not disconnect.
- Devices must tolerate unknown config TLVs by ignoring them and reporting them as
  unapplied, never by rejecting the whole write.
- A major bump means the client refuses to talk and tells the user which side is behind.
  The client learns this from `GET_INFO`, which is step 2 of the connection sequence
  precisely so this check happens before anything is written.

Since the node holds its journal indefinitely, an out-of-date phone is an inconvenience,
not data loss.

---

## 3. Firmware updates

### Supported path: USB UF2

Double-press reset, the node enumerates as a `TECHOBOOT` mass storage device, copy the
`.uf2`. This is the stock LILYGO bootloader and it is the only supported update path for
now.

### Why not OTA over BLE, yet

The stock T-Echo bootloader accepts firmware packages **without verifying the signature**,
even though the packages carry one. Anyone in Bluetooth range with the DFU service exposed
could therefore flash arbitrary firmware, and that firmware would have the network key.

So: **BLE DFU stays disabled until signature verification exists.** Enabling Nordic's DFU
service on a device that holds a shared network key, behind a bootloader that does not
check signatures, is worse than having no update path at all — a device you have to plug in
is a device that cannot be silently replaced.

If OTA becomes worth having, the order of work is: replace the bootloader with one that
verifies a signature against a key burned at provisioning time, then enable DFU, then think
about the UX. Not before. That is Phase 9 territory at the earliest.

### Release checklist

1. `ver` unchanged, or all nodes scheduled to be flashed together.
2. Version and git hash baked into the image and visible in `GET_INFO`.
3. Release build: serial debug compiled out, development-key flag absent (the build fails
   if it is set — `CLAUDE.md` §6).
4. Test plan gates for every phase touched are green (`test-plan.md`).
5. Previous image archived. The T-Echo has one flash slot and no rollback; the archived
   `.uf2` is the rollback.
6. Both nodes flashed, then a two-node smoke test before either leaves the desk.

---

## 4. Network key rotation

Rotation must not require a wire format change, so it is done with **two key slots**
rather than a key ID in the frame.

- The node holds slot 0 and slot 1. One is `active` for transmit.
- On receive, the MIC is checked against the active slot first, then the other slot. Two
  AES-CCM checks over 61 bytes is microseconds; this is not a performance concern.
- `ROTATE_KEY` makes the new slot active for transmit while both remain valid for receive.
  This is the **rotation window**.
- The window closes after 24 hours or on an explicit second `ROTATE_KEY`, whichever comes
  first, and the old slot is erased. A window that stays open forever is just two valid
  keys.
- The `STATUS` screen shows that a rotation is in progress, because a half-rotated pair
  looks fine until the window closes.

The frame counter does **not** reset on rotation. It never resets, for any reason
(`CLAUDE.md` §2.1).

---

## 5. Factory reset

`FACTORY_RESET` erases both key slots, all bonds, the config, the event journal and the
budget ring. It does **not** reset the frame counter — a device that came back from a reset
with a counter of zero would reuse nonces against any peer that still remembered it.

The counter is the one piece of state that survives everything.
