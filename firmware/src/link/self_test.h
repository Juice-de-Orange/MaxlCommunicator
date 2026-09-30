/*
 * The crypto known-answer test, run on the device at boot.
 *
 * docs/test-plan.md gate 2.15: *"Known-answer test -- fixed key, nonce,
 * plaintext -> expected ciphertext and tag. Runs on device at boot in debug
 * builds; matches published AES-CCM test vectors."*
 *
 * The host tests have checked these vectors since the link layer was written,
 * and that is not the same claim. The host is x86-64 with a different compiler,
 * different alignment rules and a different notion of how much unaligned access
 * costs; tinycrypt is C that indexes into byte buffers and does its own
 * endianness. A primitive that is right on the host and wrong on a Cortex-M4 is
 * exactly the failure this gate is written against, and it would otherwise show
 * up as MIC failures on a bench that look like a wrong key.
 *
 * Two things are checked, and they answer different questions:
 *
 *   1. The vendored tinycrypt CCM against RFC 3610 appendix A. Published
 *      vectors, so this is about the cipher being correct.
 *   2. This project's frame construction through link::Crypto against the
 *      regression fixture -- the 13-byte nonce layout, the 12-byte header as
 *      AAD, the tag truncated to four bytes. RFC 3610 has no 4-byte-tag vectors,
 *      so nothing published can cover it. That one is about the wire format not
 *      moving underneath us.
 *
 * Debug builds only. MAXL_SELF_TEST is defined by scripts/test_vectors.py, which
 * also generates the header of vectors this reads. In a release build the whole
 * thing compiles to a result that says so.
 */

#ifndef MAXL_LINK_SELF_TEST_H
#define MAXL_LINK_SELF_TEST_H

#include <stdint.h>

namespace link {

struct SelfTestResult {
    uint16_t vectorsRun = 0;
    uint16_t vectorsPassed = 0;

    /// The frame construction against the regression fixture.
    bool frameConstruction = false;

    /// False in a release build, where no vectors are compiled in. Distinguishes
    /// "did not run" from "ran and failed", which a bare pass/fail cannot.
    bool available = false;

    bool passed() const
    {
        return available && vectorsRun > 0 && vectorsRun == vectorsPassed && frameConstruction;
    }
};

/*
 * Run it. Costs a few milliseconds and no allocation.
 *
 * Fixed buffers throughout: this is link/, and scripts/check_no_alloc.py holds
 * that line. A vector longer than the buffers is counted as run and failed
 * rather than skipped -- a self test that quietly skips is worse than none.
 */
SelfTestResult runCryptoSelfTest();

} // namespace link

#endif // MAXL_LINK_SELF_TEST_H
