# Phase 2 — Gate 2.15 Crypto known-answer test on the device

| | |
|---|---|
| Date | 2026-08-31, 11:25 |
| Node | A (USB storage serial `0123ABCD4567EF01`) |
| Firmware | 0.1.0, bring-up sketch 12 (`env:bringup` extends `env:debug`) |
| Environment | `bringup` — debug, i.e. with the vectors compiled in |
| Measuring equipment | Serial output; vectors from `test-vectors/aes_ccm_rfc3610.json` and `frame_crypto.json` |
| Conditions | USB power. Nothing was transmitted. |

## Result: PASS

Gate 2.15 requires: *"Known-answer test — fixed key, nonce, plaintext → expected ciphertext
and tag. **Runs on device at boot in debug builds**; matches published AES-CCM test
vectors."*

| Check | Expected | Measured | Result |
|---|---|---|---|
| RFC 3610 vectors run | 24 | **24** | PASS |
| of which passed | 24 | **24** | PASS |
| Frame construction against the regression fixture | matches | **matches** | PASS |
| Runs at boot | yes | **yes**, before any peripheral | PASS |
| In the release build | not present, and says so | `available = 0` | PASS |

```
RESULT crypto.self_test_available = 1
RESULT crypto.rfc3610_run         = 24
RESULT crypto.rfc3610_passed      = 24
RESULT crypto.frame_construction  = 1
RESULT gate_2_15.pass             = 1
```

## Why this had not already passed before

**Until 2026-08-31 it was wrongly listed as passed.** Earlier versions of the project notes
gave Phase 2 as "1 of 16" and meant this gate — because
`firmware/test/unit/test_crypto.cpp` has been thoroughly checking the same RFC 3610 vectors
since the link layer was built.

Only **on the host**, though. The gate says "on device", and that is not a formality:

- The host is x86-64, the target a Cortex-M4. Different compiler, different alignment rules,
  different costs for unaligned accesses.
- tinycrypt is C that indexes into byte buffers and handles its endianness itself.
  Exactly the kind of code that can be right on one architecture and wrong on the other.
- The symptom of such a bug would be **a MIC failure on the bench, indistinguishable from a
  wrong key.** An evening that this gate saves.

Found while writing the gate table for the project notes: there was no boot self-test in the
firmware and no report in this directory that claimed 2.15. Phase 2 was therefore correctly
at 0 of 16 — and is now at 1.

## What is checked, and what each check answers

**1. The shipped tinycrypt CCM against RFC 3610 appendix A, all 24 vectors.**
Published vectors, against an independent source — this is about the cipher being right. The
vectors were extracted programmatically from the RFC text, not typed in by hand: a typo in a
crypto vector produces a test that passes for the wrong reason.

**2. This project's frame construction through `link::Crypto`** against the
regression fixture. RFC 3610 has no vectors with a 4-byte tag, so nothing published can cover
this. It pins down the 13-byte nonce layout, the choice of the 12-byte header as AAD and the
truncation of the tag to four bytes. Each of these changes is a wire format change and,
per `docs/versioning-and-updates.md` §1, requires a `ver` bump and both nodes flashed in the
same session.

## Implementation

| File | Role |
|---|---|
| `firmware/src/link/self_test.{h,cpp}` | The test. Fixed buffers, no allocation — `check_no_alloc.py` holds this line for `link/`. |
| `firmware/scripts/test_vectors.py` | Generates the vector header into the build, **in debug builds only** |
| `tools/gen_vectors.py --group` | New: allows emitting only the two groups that are needed |
| `firmware/src/main.cpp` | Calls it in `setup()`, prints the result with the banner |
| `firmware/src/bringup/sketch_12_app.cpp` | The same, and reports it in a collectable form |

**One source for the vectors.** The TypeScript bridge, the host test build and now the debug
image all read the same JSON files. A wrong vector is thus wrong everywhere instead of
unnoticed-right in one place.

**The self-test is built on the host too** (`-DMAXL_SELF_TEST=1` in
`firmware/test/Makefile`) and checked against itself there. Compiling it only in the firmware
image would mean that a bug *in the self-test* — a wrongly built name, a buffer one byte too
short, a vector silently not found — shows up exactly where you are least likely to look.

## Cost

| Image | Flash | Surcharge |
|---|---|---|
| `release` | 127 980 B | +272 B (only the path that says nothing is compiled in) |
| `debug` | 147 108 B | +12 096 B compared to before |

Twelve kilobytes in a debug image, with 671 KiB of headroom. None of it is in release,
and `available = 0` distinguishes "did not run" from "ran and failed" — which a bare
pass/fail cannot.

## What the gate does **not** cover

Nothing over the air. 2.1 to 2.14 and 2.16 need a second device. This is the only gate of
Phase 2 that is reachable without node B — and therefore also the only line of Phase 2 that
can be moved at the moment.
