/*
 * AES-128-CCM for the radio link (CLAUDE.md 2.1).
 *
 * The header is authenticated as AAD, the payload is encrypted and
 * authenticated, and the tag is truncated to 4 bytes. The CCM nonce is built from
 * src and counter -- that is the entire reason counter is 32 bits and persistent,
 * because nonce reuse under a shared key breaks CCM wide open (REVIEW.md A2).
 *
 * Implementation is tinycrypt, vendored under firmware/vendor/tinycrypt
 * (decision D4). Its CCM insists on a 13-byte nonce, which is exactly what
 * CLAUDE.md 2.1 specifies, and it accepts a 4-byte tag. CryptoCell/CC310 is
 * deliberately not a requirement (REVIEW.md C1): software CCM over a 64-byte
 * frame costs microseconds against a transmission measured in seconds.
 *
 * Two key slots, per versioning-and-updates.md 4. Rotation must not require a
 * wire format change, so there is no key ID in the frame -- the receiver simply
 * tries the active slot and then the other one. Two CCM checks over 64 bytes is
 * not a performance concern.
 *
 * No clock in here on purpose. The 24-hour rotation window is expressed as a
 * deadline the owner supplies and checks; making this module depend on IClock
 * would drag time handling into the one place that must stay a pure function of
 * its inputs, and the tests would have to fake a clock to check a tag.
 */

#ifndef MAXL_LINK_CRYPTO_H
#define MAXL_LINK_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

namespace link {

constexpr size_t kKeyBytes = 16;
constexpr size_t kNonceBytes = 13;
constexpr uint8_t kKeySlots = 2;
constexpr uint8_t kInvalidSlot = 0xFFu;

/// versioning-and-updates.md 4: "The window closes after 24 hours or on an
/// explicit second ROTATE_KEY, whichever comes first."
constexpr uint32_t kRotationWindowSeconds = 24u * 60u * 60u;

enum class CryptoError : uint8_t {
    None = 0,
    NoKey,          ///< no slot holds a key -- the node must not transmit
    BadSlot,
    BadLength,
    AuthFailed,     ///< MIC did not verify under any valid slot
    Internal,       ///< tinycrypt refused the parameters; a bug, not an input
};

/*
 * Build the 13-byte CCM nonce.
 *
 * src (2 B) and counter (4 B) little endian, then seven zero bytes. Uniqueness
 * comes entirely from the first six: counter is monotonic and persistent per
 * device (CLAUDE.md 2.1) and src differs between devices, so no (src, counter)
 * pair is ever used twice under one key. The remaining bytes are fixed rather
 * than filled with other header fields, because anything variable there would
 * suggest the uniqueness depends on it, and it does not.
 */
void buildNonce(uint16_t src, uint32_t counter, uint8_t *nonce13);

class Crypto {
public:
    Crypto();

    /// Install a 16-byte key. Slot 0 or 1.
    CryptoError setKey(uint8_t slot, const uint8_t *key16);

    /// Erase a slot. Erasing the active slot leaves the node unable to transmit.
    CryptoError clearKey(uint8_t slot);

    bool hasKey(uint8_t slot) const;

    /// True when at least one slot holds a key.
    bool hasAnyKey() const;

    uint8_t activeSlot() const { return activeSlot_; }

    /*
     * Start a rotation window: `newSlot` becomes the transmit key, both slots
     * stay valid for receive until `nowUnix + kRotationWindowSeconds`.
     */
    CryptoError beginRotation(uint8_t newSlot, uint32_t nowUnix);

    /// Close the window early and erase the old slot (a second ROTATE_KEY).
    void completeRotation();

    /// Close the window if its deadline has passed. Call from the main loop.
    /// Returns true if it closed on this call.
    bool expireRotation(uint32_t nowUnix);

    bool rotationInProgress() const { return rotating_; }
    uint32_t rotationDeadline() const { return rotationDeadline_; }

    /*
     * Encrypt with the active slot.
     *
     * header must be kHeaderBytes long and is used as AAD, not copied into out.
     * out receives plainLen + kMicBytes bytes -- ciphertext followed by the
     * truncated tag. Caller writes the header into the frame itself.
     */
    CryptoError encrypt(const uint8_t *header, const uint8_t *plain, size_t plainLen,
                        uint8_t *out, size_t outCapacity);

    /*
     * Verify and decrypt. `body` is ciphertext followed by the 4-byte tag.
     *
     * Tries the active slot first, then the other one if a rotation window is
     * open. `usedSlot` reports which slot verified, so the caller can tell that a
     * peer is still on the old key -- which is what makes a half-rotated pair
     * visible instead of silently fine until the window closes.
     */
    CryptoError decrypt(const uint8_t *header, const uint8_t *body, size_t bodyLen,
                        uint8_t *out, size_t outCapacity, uint8_t *usedSlot);

private:
    CryptoError runDecrypt(uint8_t slot, const uint8_t *header, const uint8_t *body,
                           size_t bodyLen, uint8_t *out);

    uint8_t keys_[kKeySlots][kKeyBytes];
    bool keyPresent_[kKeySlots];
    uint8_t activeSlot_;
    bool rotating_;
    uint32_t rotationDeadline_;
};

} // namespace link

#endif // MAXL_LINK_CRYPTO_H
