# tinycrypt (vendored)

AES-128-CCM for the radio link. Decision D4 in `docs/decisions/0001-open-decisions.md`
records why this one: it is BSD-3, its CCM insists on a **13-byte nonce** — exactly what
`CLAUDE.md` §2.1 specifies — and it accepts a 4-byte tag.

Source: [`zephyrproject-rtos/tinycrypt`](https://github.com/zephyrproject-rtos/tinycrypt).
The original `intel/tinycrypt` has been archived since March 2024; the Zephyr fork is the
maintained one.

Only three files are here. CCM is CTR mode plus CBC-MAC, so it never calls AES decryption
and `aes_decrypt.c` is deliberately absent — vendoring code that is never executed only
grows the image and the audit surface.

| File | Purpose |
|---|---|
| `src/aes_encrypt.c` | the AES-128 block function |
| `src/ccm_mode.c` | CCM generation/encryption and decryption/verification |
| `src/utils.c` | constant-time compare and helpers |

**Do not patch these files.** They are built as a PlatformIO library rather than as project
sources precisely so that the project's `-Wall -Wextra -Werror` does not apply to them; a
warning fixed here is a diff against upstream that the next update silently reverts.

Verified against RFC 3610's published vectors in `firmware/test/unit/test_crypto.cpp` —
all 24 of them, extracted from the RFC text rather than from tinycrypt's own copy.
That is `docs/test-plan.md` gate 2.15.
