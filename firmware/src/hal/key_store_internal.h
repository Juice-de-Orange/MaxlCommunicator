/*
 * IKeyStore on the nRF52840's internal flash.
 *
 * Uses the Adafruit core's InternalFileSystem, which is LittleFS on the last few
 * pages of internal flash -- below the bootloader, above the application. That
 * is what CLAUDE.md 3.0 asks for: the key never reaches the external chip, which
 * anyone with a clip can read.
 *
 * NOT covered by the host tests. What is covered is everything above it, against
 * fakes/fake_key_store.h.
 */

#ifndef MAXL_HAL_KEY_STORE_INTERNAL_H
#define MAXL_HAL_KEY_STORE_INTERNAL_H

#include "hal/i_key_store.h"

namespace hal {

class InternalKeyStore : public IKeyStore {
public:
    bool begin();

    KeyResult store(uint8_t slot, uint8_t netId, const uint8_t key[kKeyBytes]) override;
    KeyResult load(uint8_t slot, uint8_t *netIdOut, uint8_t keyOut[kKeyBytes]) const override;
    bool hasKey(uint8_t slot) const override;
    bool hasAnyKey() const override;
    KeyResult eraseAll() override;

private:
    bool mounted_ = false;
    static const char *pathOf(uint8_t slot);
};

} // namespace hal

#endif // MAXL_HAL_KEY_STORE_INTERNAL_H
