#include "self_test.h"

#include "crypto.h"
#include "frame.h"

#include <string.h>

#if defined(MAXL_SELF_TEST)
#include "test_vectors.h"

#include <tinycrypt/aes.h>
#include <tinycrypt/ccm_mode.h>
#include <tinycrypt/constants.h>
#endif

namespace link {
namespace {

#if defined(MAXL_SELF_TEST)

/// RFC 3610 appendix A carries 24 vectors. Payloads reach 31 bytes and tags 10,
/// so 96 is comfortable without being a guess.
constexpr size_t kScratch = 96;

/// Build "rfc3610_07_nonce" without snprintf -- newlib's can reach for the heap
/// on some formats, and link/ does not allocate (CLAUDE.md 3).
void vectorName(char *out, size_t size, uint8_t index, const char *field)
{
    size_t at = 0;
    const char *prefix = "rfc3610_";
    while (*prefix != '\0' && at + 1 < size) {
        out[at++] = *prefix++;
    }
    if (at + 2 < size) {
        out[at++] = static_cast<char>('0' + (index / 10));
        out[at++] = static_cast<char>('0' + (index % 10));
    }
    if (at + 1 < size) {
        out[at++] = '_';
    }
    while (*field != '\0' && at + 1 < size) {
        out[at++] = *field++;
    }
    out[at] = '\0';
}

const vectors::Vector *lookup(uint8_t index, const char *field)
{
    char name[32];
    vectorName(name, sizeof(name), index, field);
    return vectors::find(name);
}

/// One RFC 3610 vector through the vendored primitive.
bool runRfc3610(uint8_t index)
{
    const vectors::Vector *key = lookup(index, "key");
    const vectors::Vector *nonce = lookup(index, "nonce");
    const vectors::Vector *adata = lookup(index, "adata");
    const vectors::Vector *payload = lookup(index, "payload");
    const vectors::Vector *expected = lookup(index, "expected");
    const vectors::Vector *maclen = lookup(index, "maclen");

    if (key == nullptr || nonce == nullptr || adata == nullptr || payload == nullptr ||
        expected == nullptr || maclen == nullptr || maclen->length != 1) {
        return false;
    }
    if (expected->length > kScratch || key->length != 16) {
        return false;
    }

    struct tc_aes_key_sched_struct sched;
    struct tc_ccm_mode_struct ccm;

    // tc_ccm_config takes a non-const nonce; it does not modify it, but the
    // signature is what it is, so a copy rather than a cast.
    uint8_t nonceCopy[16];
    if (nonce->length > sizeof(nonceCopy)) {
        return false;
    }
    memcpy(nonceCopy, nonce->bytes, nonce->length);

    if (tc_aes128_set_encrypt_key(&sched, key->bytes) != TC_CRYPTO_SUCCESS) {
        return false;
    }
    if (tc_ccm_config(&ccm, &sched, nonceCopy, static_cast<unsigned int>(nonce->length),
                      static_cast<unsigned int>(maclen->bytes[0])) != TC_CRYPTO_SUCCESS) {
        return false;
    }

    uint8_t out[kScratch];
    if (tc_ccm_generation_encryption(out, sizeof(out), adata->bytes,
                                     static_cast<unsigned int>(adata->length), payload->bytes,
                                     static_cast<unsigned int>(payload->length),
                                     &ccm) != TC_CRYPTO_SUCCESS) {
        return false;
    }

    return memcmp(out, expected->bytes, expected->length) == 0;
}

/*
 * The frame construction, through the class that ships.
 *
 * Inputs are the ones recorded in test-vectors/frame_crypto.json; only the
 * expected bytes come from the generated header, because that is the part that
 * must not move. A change to the nonce layout, to the choice of AAD or to the
 * tag length is a wire format change -- and its symptom on a bench is a MIC
 * failure indistinguishable from a wrong key.
 */
bool runFrameConstruction()
{
    const vectors::Vector *fixture = vectors::find("frame_ccm_regression");
    if (fixture == nullptr) {
        return false;
    }

    static const uint8_t kKey[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                     0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    static const uint8_t kHeader[kHeaderBytes] = {0x14, 0x07, 0x01, 0x00, 0x02, 0x00,
                                                  0x2a, 0x00, 0x00, 0x00, 0x11, 0x01};
    static const uint8_t kPlain[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};

    Crypto crypto;
    if (crypto.setKey(0, kKey) != CryptoError::None) {
        return false;
    }

    uint8_t body[sizeof(kPlain) + kMicBytes];
    if (crypto.encrypt(kHeader, kPlain, sizeof(kPlain), body, sizeof(body)) !=
        CryptoError::None) {
        return false;
    }
    if (fixture->length != sizeof(body)) {
        return false;
    }
    return memcmp(body, fixture->bytes, sizeof(body)) == 0;
}

#endif // MAXL_SELF_TEST

} // namespace

SelfTestResult runCryptoSelfTest()
{
    SelfTestResult result;

#if defined(MAXL_SELF_TEST)
    result.available = true;

    // RFC 3610 appendix A, vectors 1 through 24.
    for (uint8_t index = 1; index <= 24; ++index) {
        ++result.vectorsRun;
        if (runRfc3610(index)) {
            ++result.vectorsPassed;
        }
    }

    result.frameConstruction = runFrameConstruction();
#endif

    return result;
}

} // namespace link
