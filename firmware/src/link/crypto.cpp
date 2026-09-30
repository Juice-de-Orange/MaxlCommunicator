#include "crypto.h"

#include "frame.h"

extern "C" {
#include <tinycrypt/aes.h>
#include <tinycrypt/ccm_mode.h>
#include <tinycrypt/constants.h>
}

namespace link {
namespace {

/// Constant-time-ish clear. Keys must not linger in a stack buffer after use.
void wipe(uint8_t *buffer, size_t len)
{
    volatile uint8_t *p = buffer;
    for (size_t i = 0; i < len; ++i) {
        p[i] = 0;
    }
}

bool validSlot(uint8_t slot)
{
    return slot < kKeySlots;
}

} // namespace

void buildNonce(uint16_t src, uint32_t counter, uint8_t *nonce13)
{
    nonce13[0] = static_cast<uint8_t>(src & 0xFFu);
    nonce13[1] = static_cast<uint8_t>((src >> 8) & 0xFFu);
    nonce13[2] = static_cast<uint8_t>(counter & 0xFFu);
    nonce13[3] = static_cast<uint8_t>((counter >> 8) & 0xFFu);
    nonce13[4] = static_cast<uint8_t>((counter >> 16) & 0xFFu);
    nonce13[5] = static_cast<uint8_t>((counter >> 24) & 0xFFu);
    for (size_t i = 6; i < kNonceBytes; ++i) {
        nonce13[i] = 0;
    }
}

Crypto::Crypto()
    : keys_{}, keyPresent_{false, false}, activeSlot_(0), rotating_(false), rotationDeadline_(0)
{
}

CryptoError Crypto::setKey(uint8_t slot, const uint8_t *key16)
{
    if (!validSlot(slot)) {
        return CryptoError::BadSlot;
    }
    if (key16 == nullptr) {
        return CryptoError::BadLength;
    }
    for (size_t i = 0; i < kKeyBytes; ++i) {
        keys_[slot][i] = key16[i];
    }
    keyPresent_[slot] = true;
    return CryptoError::None;
}

CryptoError Crypto::clearKey(uint8_t slot)
{
    if (!validSlot(slot)) {
        return CryptoError::BadSlot;
    }
    wipe(keys_[slot], kKeyBytes);
    keyPresent_[slot] = false;
    return CryptoError::None;
}

bool Crypto::hasKey(uint8_t slot) const
{
    return validSlot(slot) && keyPresent_[slot];
}

bool Crypto::hasAnyKey() const
{
    return keyPresent_[0] || keyPresent_[1];
}

CryptoError Crypto::beginRotation(uint8_t newSlot, uint32_t nowUnix)
{
    if (!validSlot(newSlot)) {
        return CryptoError::BadSlot;
    }
    if (!keyPresent_[newSlot]) {
        return CryptoError::NoKey;
    }
    // Rotating onto the slot that is already active is a no-op, not an error:
    // the phone may re-send ROTATE_KEY after a dropped connection.
    if (newSlot == activeSlot_ && !rotating_) {
        return CryptoError::None;
    }
    activeSlot_ = newSlot;
    rotating_ = true;
    rotationDeadline_ = nowUnix + kRotationWindowSeconds;
    return CryptoError::None;
}

void Crypto::completeRotation()
{
    if (!rotating_) {
        return;
    }
    const uint8_t oldSlot = static_cast<uint8_t>(1u - activeSlot_);
    // "the old slot is erased. A window that stays open forever is just two valid
    // keys." -- versioning-and-updates.md 4
    (void)clearKey(oldSlot);
    rotating_ = false;
    rotationDeadline_ = 0;
}

bool Crypto::expireRotation(uint32_t nowUnix)
{
    if (!rotating_ || nowUnix < rotationDeadline_) {
        return false;
    }
    completeRotation();
    return true;
}

CryptoError Crypto::encrypt(const uint8_t *header, const uint8_t *plain, size_t plainLen,
                            uint8_t *out, size_t outCapacity)
{
    if (header == nullptr || out == nullptr) {
        return CryptoError::BadLength;
    }
    if (plainLen > kMaxPayloadBytes || (plainLen > 0 && plain == nullptr)) {
        return CryptoError::BadLength;
    }
    if (outCapacity < plainLen + kMicBytes) {
        return CryptoError::BadLength;
    }
    if (!keyPresent_[activeSlot_]) {
        // CLAUDE.md 2.1: without a key there is nothing to send. The caller turns
        // this into ERR_NO_KEY over the bridge (docs/bridge-protocol.md 4).
        return CryptoError::NoKey;
    }

    // The nonce comes from the header we are authenticating, so encrypt() cannot
    // be handed a header and a nonce that disagree.
    const Header parsed = decodeHeader(header);
    uint8_t nonce[kNonceBytes];
    buildNonce(parsed.src, parsed.counter, nonce);

    struct tc_aes_key_sched_struct sched;
    struct tc_ccm_mode_struct ccm;
    CryptoError result = CryptoError::None;

    if (tc_aes128_set_encrypt_key(&sched, keys_[activeSlot_]) != TC_CRYPTO_SUCCESS ||
        tc_ccm_config(&ccm, &sched, nonce, kNonceBytes, kMicBytes) != TC_CRYPTO_SUCCESS) {
        result = CryptoError::Internal;
    } else if (tc_ccm_generation_encryption(
                   out, static_cast<unsigned int>(outCapacity), header,
                   static_cast<unsigned int>(kHeaderBytes), plain,
                   static_cast<unsigned int>(plainLen), &ccm) != TC_CRYPTO_SUCCESS) {
        result = CryptoError::Internal;
    }

    wipe(reinterpret_cast<uint8_t *>(&sched), sizeof(sched));
    return result;
}

CryptoError Crypto::runDecrypt(uint8_t slot, const uint8_t *header, const uint8_t *body,
                               size_t bodyLen, uint8_t *out)
{
    const Header parsed = decodeHeader(header);
    uint8_t nonce[kNonceBytes];
    buildNonce(parsed.src, parsed.counter, nonce);

    struct tc_aes_key_sched_struct sched;
    struct tc_ccm_mode_struct ccm;
    CryptoError result = CryptoError::None;

    if (tc_aes128_set_encrypt_key(&sched, keys_[slot]) != TC_CRYPTO_SUCCESS ||
        tc_ccm_config(&ccm, &sched, nonce, kNonceBytes, kMicBytes) != TC_CRYPTO_SUCCESS) {
        result = CryptoError::Internal;
    } else if (tc_ccm_decryption_verification(
                   out, static_cast<unsigned int>(bodyLen - kMicBytes), header,
                   static_cast<unsigned int>(kHeaderBytes), body,
                   static_cast<unsigned int>(bodyLen), &ccm) != TC_CRYPTO_SUCCESS) {
        result = CryptoError::AuthFailed;
    }

    wipe(reinterpret_cast<uint8_t *>(&sched), sizeof(sched));
    return result;
}

CryptoError Crypto::decrypt(const uint8_t *header, const uint8_t *body, size_t bodyLen,
                            uint8_t *out, size_t outCapacity, uint8_t *usedSlot)
{
    if (header == nullptr || body == nullptr || out == nullptr) {
        return CryptoError::BadLength;
    }
    if (bodyLen < kMicBytes || bodyLen > kMaxPayloadBytes + kMicBytes) {
        return CryptoError::BadLength;
    }
    if (outCapacity < bodyLen - kMicBytes) {
        return CryptoError::BadLength;
    }
    if (!hasAnyKey()) {
        return CryptoError::NoKey;
    }

    /*
     * Active slot first, then the other one -- but only while a rotation window
     * is open. Trying both unconditionally would mean an erased-but-stale slot
     * could keep verifying frames, which is the failure mode the window exists to
     * bound.
     */
    if (keyPresent_[activeSlot_]) {
        if (runDecrypt(activeSlot_, header, body, bodyLen, out) == CryptoError::None) {
            if (usedSlot != nullptr) {
                *usedSlot = activeSlot_;
            }
            return CryptoError::None;
        }
    }

    if (rotating_) {
        const uint8_t other = static_cast<uint8_t>(1u - activeSlot_);
        if (keyPresent_[other] &&
            runDecrypt(other, header, body, bodyLen, out) == CryptoError::None) {
            if (usedSlot != nullptr) {
                *usedSlot = other;
            }
            return CryptoError::None;
        }
    }

    if (usedSlot != nullptr) {
        *usedSlot = kInvalidSlot;
    }
    return CryptoError::AuthFailed;
}

} // namespace link
