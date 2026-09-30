/*
 * AES-128-CCM. This file is docs/test-plan.md gate 2.15.
 *
 *   "Known-answer test -- fixed key, nonce, plaintext -> expected ciphertext and
 *    tag. Runs on device at boot in debug builds; matches published AES-CCM test
 *    vectors."
 *
 * It is the one Phase 2 gate that is fully satisfiable without hardware, because
 * nothing about it involves a radio.
 *
 * Two layers, deliberately separate:
 *
 *   1. The vendored primitive against RFC 3610's published vectors. Extracted
 *      from the RFC itself, not from tinycrypt's copy of them -- testing a
 *      library against vectors it ships is circular.
 *   2. Our frame construction on top of it: the 13-byte nonce from src and
 *      counter, the header as AAD, the 4-byte tag. RFC 3610 has no 4-byte-tag
 *      vectors, so this layer is pinned by a regression fixture instead, and is
 *      labelled as such.
 */

#include <cstdio>
#include <cstring>

#include "doctest.h"

#include "link/crypto.h"
#include "link/self_test.h"
#include "link/frame.h"
#include "test_vectors.h"

extern "C" {
#include <tinycrypt/aes.h>
#include <tinycrypt/ccm_mode.h>
#include <tinycrypt/constants.h>
}

using namespace link;

namespace {

const vectors::Vector *require(const char *tag, const char *field)
{
    char name[64];
    std::snprintf(name, sizeof(name), "%s_%s", tag, field);
    const vectors::Vector *vector = vectors::find(name);
    CAPTURE(name);
    REQUIRE(vector != nullptr);
    return vector;
}

constexpr int kRfc3610VectorCount = 24;

} // namespace

TEST_CASE("the vendored CCM matches every RFC 3610 vector")
{
    for (int index = 1; index <= kRfc3610VectorCount; ++index) {
        char tag[16];
        std::snprintf(tag, sizeof(tag), "rfc3610_%02d", index);
        CAPTURE(tag);

        const vectors::Vector *key = require(tag, "key");
        const vectors::Vector *nonce = require(tag, "nonce");
        const vectors::Vector *adata = require(tag, "adata");
        const vectors::Vector *payload = require(tag, "payload");
        const vectors::Vector *expected = require(tag, "expected");
        const vectors::Vector *maclen = require(tag, "maclen");

        REQUIRE(nonce->length == kNonceBytes);  // 13, as CLAUDE.md 2.1 requires
        const unsigned int mlen = maclen->bytes[0];
        REQUIRE(expected->length == payload->length + mlen);

        uint8_t nonceCopy[kNonceBytes];
        std::memcpy(nonceCopy, nonce->bytes, kNonceBytes);

        struct tc_aes_key_sched_struct sched;
        struct tc_ccm_mode_struct ccm;
        REQUIRE(tc_aes128_set_encrypt_key(&sched, key->bytes) == TC_CRYPTO_SUCCESS);
        REQUIRE(tc_ccm_config(&ccm, &sched, nonceCopy, kNonceBytes, mlen) == TC_CRYPTO_SUCCESS);

        uint8_t out[64];
        REQUIRE(expected->length <= sizeof(out));
        REQUIRE(tc_ccm_generation_encryption(
                    out, static_cast<unsigned int>(sizeof(out)), adata->bytes,
                    static_cast<unsigned int>(adata->length), payload->bytes,
                    static_cast<unsigned int>(payload->length), &ccm) == TC_CRYPTO_SUCCESS);
        CHECK(std::memcmp(out, expected->bytes, expected->length) == 0);

        // And back again.
        uint8_t plain[64];
        REQUIRE(tc_ccm_decryption_verification(
                    plain, static_cast<unsigned int>(sizeof(plain)), adata->bytes,
                    static_cast<unsigned int>(adata->length), expected->bytes,
                    static_cast<unsigned int>(expected->length), &ccm) == TC_CRYPTO_SUCCESS);
        CHECK(std::memcmp(plain, payload->bytes, payload->length) == 0);
    }
}

TEST_CASE("the nonce is built from src and counter and nothing else")
{
    // CLAUDE.md 2.1: "The CCM nonce is built from src and counter -- those 13
    // bytes are what makes nonce reuse impossible."
    uint8_t nonce[kNonceBytes];
    buildNonce(0x1234, 0xDEADBEEFu, nonce);

    CHECK(nonce[0] == 0x34);
    CHECK(nonce[1] == 0x12);
    CHECK(nonce[2] == 0xEF);
    CHECK(nonce[3] == 0xBE);
    CHECK(nonce[4] == 0xAD);
    CHECK(nonce[5] == 0xDE);
    for (size_t i = 6; i < kNonceBytes; ++i) {
        CHECK(nonce[i] == 0);
    }

    // The property that matters: no two (src, counter) pairs collide.
    uint8_t a[kNonceBytes];
    uint8_t b[kNonceBytes];
    buildNonce(1, 0x00000100u, a);
    buildNonce(1, 0x00000001u, b);
    CHECK(std::memcmp(a, b, kNonceBytes) != 0);
    buildNonce(0x0100, 1, a);
    buildNonce(0x0001, 1, b);
    CHECK(std::memcmp(a, b, kNonceBytes) != 0);
}

namespace {

const uint8_t kTestKey[kKeyBytes] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                     0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

Header sampleHeader()
{
    Header header{};
    header.version = kWireVersion;
    header.type = FrameType::Position;
    header.netId = 0x07;
    header.src = 0x0001;
    header.dst = 0x0002;
    header.counter = 0x0000002Au;
    header.seq = 0x11;
    header.flags = kFlagAckReq;
    return header;
}

} // namespace

TEST_CASE("a frame round-trips through encrypt and decrypt")
{
    Crypto crypto;
    REQUIRE(crypto.setKey(0, kTestKey) == CryptoError::None);

    uint8_t header[kHeaderBytes];
    encodeHeader(sampleHeader(), header);

    const uint8_t plain[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    uint8_t body[sizeof(plain) + kMicBytes];
    REQUIRE(crypto.encrypt(header, plain, sizeof(plain), body, sizeof(body)) ==
            CryptoError::None);

    // The payload really is encrypted, not merely authenticated.
    CHECK(std::memcmp(body, plain, sizeof(plain)) != 0);

    uint8_t recovered[sizeof(plain)];
    uint8_t slot = kInvalidSlot;
    REQUIRE(crypto.decrypt(header, body, sizeof(body), recovered, sizeof(recovered), &slot) ==
            CryptoError::None);
    CHECK(std::memcmp(recovered, plain, sizeof(plain)) == 0);
    CHECK(slot == 0);
}

TEST_CASE("flipping one bit anywhere is rejected")
{
    /*
     * docs/test-plan.md 2.2: "Bit-flip tamper -- flip one bit in payload, then in
     * header. Both rejected, every time, 20 trials each." On hardware that is 20
     * trials; here it is every bit of both, which is 128 header flips and 128
     * payload flips.
     */
    Crypto crypto;
    REQUIRE(crypto.setKey(0, kTestKey) == CryptoError::None);

    uint8_t header[kHeaderBytes];
    encodeHeader(sampleHeader(), header);
    const uint8_t plain[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    uint8_t body[sizeof(plain) + kMicBytes];
    REQUIRE(crypto.encrypt(header, plain, sizeof(plain), body, sizeof(body)) ==
            CryptoError::None);

    uint8_t recovered[sizeof(plain)];

    // Every bit of the header. The header is AAD, so a change there must break
    // the tag even though it is not encrypted.
    for (size_t byte = 0; byte < kHeaderBytes; ++byte) {
        for (int bit = 0; bit < 8; ++bit) {
            uint8_t tampered[kHeaderBytes];
            std::memcpy(tampered, header, sizeof(tampered));
            tampered[byte] = static_cast<uint8_t>(tampered[byte] ^ (1u << bit));
            CAPTURE(byte);
            CAPTURE(bit);
            CHECK(crypto.decrypt(tampered, body, sizeof(body), recovered, sizeof(recovered),
                                 nullptr) == CryptoError::AuthFailed);
        }
    }

    // Every bit of the ciphertext and of the tag.
    for (size_t byte = 0; byte < sizeof(body); ++byte) {
        for (int bit = 0; bit < 8; ++bit) {
            uint8_t tampered[sizeof(body)];
            std::memcpy(tampered, body, sizeof(tampered));
            tampered[byte] = static_cast<uint8_t>(tampered[byte] ^ (1u << bit));
            CAPTURE(byte);
            CAPTURE(bit);
            CHECK(crypto.decrypt(header, tampered, sizeof(tampered), recovered,
                                 sizeof(recovered), nullptr) == CryptoError::AuthFailed);
        }
    }
}

TEST_CASE("a frame from a different counter does not verify")
{
    // The nonce is part of the construction, so replaying a frame under a
    // different counter cannot work even before the replay window sees it.
    Crypto crypto;
    REQUIRE(crypto.setKey(0, kTestKey) == CryptoError::None);

    Header header = sampleHeader();
    uint8_t wire[kHeaderBytes];
    encodeHeader(header, wire);

    const uint8_t plain[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    uint8_t body[sizeof(plain) + kMicBytes];
    REQUIRE(crypto.encrypt(wire, plain, sizeof(plain), body, sizeof(body)) == CryptoError::None);

    header.counter += 1;
    encodeHeader(header, wire);
    uint8_t recovered[sizeof(plain)];
    CHECK(crypto.decrypt(wire, body, sizeof(body), recovered, sizeof(recovered), nullptr) ==
          CryptoError::AuthFailed);
}

TEST_CASE("without a key nothing is transmitted")
{
    // Becomes ERR_NO_KEY over the bridge (docs/bridge-protocol.md 4).
    Crypto crypto;
    uint8_t header[kHeaderBytes];
    encodeHeader(sampleHeader(), header);
    const uint8_t plain[4] = {1, 2, 3, 4};
    uint8_t body[sizeof(plain) + kMicBytes];

    CHECK_FALSE(crypto.hasAnyKey());
    CHECK(crypto.encrypt(header, plain, sizeof(plain), body, sizeof(body)) == CryptoError::NoKey);
    CHECK(crypto.decrypt(header, body, sizeof(body), body, sizeof(body), nullptr) ==
          CryptoError::NoKey);
}

TEST_CASE("key rotation keeps both slots readable, then closes the window")
{
    /*
     * versioning-and-updates.md 4. The failure this guards against is a
     * half-rotated pair that looks fine until the window closes and then goes
     * silent in the field.
     */
    const uint8_t oldKey[kKeyBytes] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    const uint8_t newKey[kKeyBytes] = {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};

    Crypto peer;  // still on the old key
    REQUIRE(peer.setKey(0, oldKey) == CryptoError::None);

    Crypto local;
    REQUIRE(local.setKey(0, oldKey) == CryptoError::None);
    REQUIRE(local.setKey(1, newKey) == CryptoError::None);

    uint8_t header[kHeaderBytes];
    encodeHeader(sampleHeader(), header);
    const uint8_t plain[4] = {9, 8, 7, 6};
    uint8_t fromPeer[sizeof(plain) + kMicBytes];
    REQUIRE(peer.encrypt(header, plain, sizeof(plain), fromPeer, sizeof(fromPeer)) ==
            CryptoError::None);

    constexpr uint32_t kNow = 1788000000u;
    REQUIRE(local.beginRotation(1, kNow) == CryptoError::None);
    CHECK(local.activeSlot() == 1);
    CHECK(local.rotationInProgress());

    // Inside the window the peer's old-key frame still verifies, and we are told
    // which slot did it -- that is how a half-rotated pair becomes visible.
    uint8_t recovered[sizeof(plain)];
    uint8_t usedSlot = kInvalidSlot;
    CHECK(local.decrypt(header, fromPeer, sizeof(fromPeer), recovered, sizeof(recovered),
                        &usedSlot) == CryptoError::None);
    CHECK(usedSlot == 0);

    // The window is 24 hours; a second short of that it is still open.
    CHECK_FALSE(local.expireRotation(kNow + kRotationWindowSeconds - 1));
    CHECK(local.rotationInProgress());

    CHECK(local.expireRotation(kNow + kRotationWindowSeconds));
    CHECK_FALSE(local.rotationInProgress());
    CHECK_FALSE(local.hasKey(0));  // the old slot is erased, not merely inactive

    // And now the peer is locked out, which is the intended end state.
    CHECK(local.decrypt(header, fromPeer, sizeof(fromPeer), recovered, sizeof(recovered),
                        nullptr) == CryptoError::AuthFailed);
}

TEST_CASE("an explicit second rotation closes the window early")
{
    const uint8_t oldKey[kKeyBytes] = {3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};
    const uint8_t newKey[kKeyBytes] = {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};

    Crypto crypto;
    REQUIRE(crypto.setKey(0, oldKey) == CryptoError::None);
    REQUIRE(crypto.setKey(1, newKey) == CryptoError::None);
    REQUIRE(crypto.beginRotation(1, 1788000000u) == CryptoError::None);

    crypto.completeRotation();
    CHECK_FALSE(crypto.rotationInProgress());
    CHECK_FALSE(crypto.hasKey(0));
    CHECK(crypto.hasKey(1));
    CHECK(crypto.activeSlot() == 1);
}

TEST_CASE("outside a rotation window the inactive slot is not tried")
{
    // "A window that stays open forever is just two valid keys."
    const uint8_t keyA[kKeyBytes] = {5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5};
    const uint8_t keyB[kKeyBytes] = {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6};

    Crypto sender;
    REQUIRE(sender.setKey(0, keyA) == CryptoError::None);

    Crypto receiver;
    REQUIRE(receiver.setKey(0, keyB) == CryptoError::None);  // active, wrong key
    REQUIRE(receiver.setKey(1, keyA) == CryptoError::None);  // right key, inactive

    uint8_t header[kHeaderBytes];
    encodeHeader(sampleHeader(), header);
    const uint8_t plain[4] = {1, 2, 3, 4};
    uint8_t body[sizeof(plain) + kMicBytes];
    REQUIRE(sender.encrypt(header, plain, sizeof(plain), body, sizeof(body)) ==
            CryptoError::None);

    uint8_t recovered[sizeof(plain)];
    CHECK(receiver.decrypt(header, body, sizeof(body), recovered, sizeof(recovered), nullptr) ==
          CryptoError::AuthFailed);
}

TEST_CASE("the frame construction is pinned against accidental change")
{
    /*
     * A REGRESSION FIXTURE, not a known-answer test -- the expected bytes came
     * from this implementation, so it proves nothing about correctness. What it
     * does prove is that the nonce layout, the choice of the header as AAD and
     * the 4-byte tag length have not moved. Any of those changing silently would
     * be a wire format change without a `ver` bump, which
     * versioning-and-updates.md 1 exists to prevent.
     *
     * RFC 3610 has no 4-byte-tag vectors, which is why this cannot be a real KAT.
     */
    Crypto crypto;
    REQUIRE(crypto.setKey(0, kTestKey) == CryptoError::None);

    uint8_t header[kHeaderBytes];
    encodeHeader(sampleHeader(), header);
    const uint8_t plain[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    uint8_t body[sizeof(plain) + kMicBytes];
    REQUIRE(crypto.encrypt(header, plain, sizeof(plain), body, sizeof(body)) ==
            CryptoError::None);

    const vectors::Vector *fixture = vectors::find("frame_ccm_regression");
    REQUIRE(fixture != nullptr);
    REQUIRE(fixture->length == sizeof(body));
    CHECK(std::memcmp(body, fixture->bytes, sizeof(body)) == 0);
}

/*
 * The boot self test itself.
 *
 * link/self_test.cpp is what runs on the device at boot (gate 2.15). Compiling
 * it only into the firmware image would mean any bug in it -- a name built
 * wrong, a buffer a byte short, a vector silently not found -- surfaces on the
 * device, which is the hardest place to look. So the host builds it too and
 * checks that it agrees with itself.
 */
TEST_CASE("the boot self test runs every RFC 3610 vector and passes")
{
    const link::SelfTestResult result = link::runCryptoSelfTest();

    CHECK(result.available);
    // RFC 3610 appendix A carries 24.
    CHECK(result.vectorsRun == 24);
    CHECK(result.vectorsPassed == 24);
    CHECK(result.frameConstruction);
    CHECK(result.passed());
}

TEST_CASE("a self test that ran nothing does not report success")
{
    // The distinction the `available` flag exists for: a release build compiles
    // no vectors in, and "did not run" must not read as "passed".
    link::SelfTestResult empty;
    CHECK_FALSE(empty.available);
    CHECK_FALSE(empty.passed());

    link::SelfTestResult ranNothing;
    ranNothing.available = true;
    ranNothing.frameConstruction = true;
    CHECK_FALSE(ranNothing.passed());

    link::SelfTestResult someFailed;
    someFailed.available = true;
    someFailed.vectorsRun = 24;
    someFailed.vectorsPassed = 23;
    someFailed.frameConstruction = true;
    CHECK_FALSE(someFailed.passed());

    link::SelfTestResult frameMoved;
    frameMoved.available = true;
    frameMoved.vectorsRun = 24;
    frameMoved.vectorsPassed = 24;
    frameMoved.frameConstruction = false;
    CHECK_FALSE(frameMoved.passed());
}
