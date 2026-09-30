/*
 * A key store in memory, with fault injection.
 *
 * The whole point of hal::IKeyStore being an interface: provisioning, rotation
 * and factory reset are security-relevant paths, and "the write failed" is one
 * of the cases that has to behave correctly. On the device that needs a broken
 * flash; here it needs one call.
 */

#ifndef MAXL_TEST_FAKE_KEY_STORE_H
#define MAXL_TEST_FAKE_KEY_STORE_H

#include <cstring>

#include "hal/i_key_store.h"

namespace fakes {

class FakeKeyStore : public hal::IKeyStore {
public:
    hal::KeyResult store(uint8_t slot, uint8_t netId,
                         const uint8_t key[hal::kKeyBytes]) override
    {
        if (slot >= hal::kKeySlots) {
            return hal::KeyResult::BadSlot;
        }
        if (failWrites) {
            return hal::KeyResult::IoError;
        }
        std::memcpy(slots_[slot].key, key, hal::kKeyBytes);
        slots_[slot].netId = netId;
        slots_[slot].present = true;
        ++writes;
        return hal::KeyResult::Ok;
    }

    hal::KeyResult load(uint8_t slot, uint8_t *netIdOut,
                        uint8_t keyOut[hal::kKeyBytes]) const override
    {
        if (slot >= hal::kKeySlots) {
            return hal::KeyResult::BadSlot;
        }
        if (!slots_[slot].present) {
            return hal::KeyResult::NotFound;
        }
        if (netIdOut != nullptr) {
            *netIdOut = slots_[slot].netId;
        }
        std::memcpy(keyOut, slots_[slot].key, hal::kKeyBytes);
        return hal::KeyResult::Ok;
    }

    bool hasKey(uint8_t slot) const override
    {
        return slot < hal::kKeySlots && slots_[slot].present;
    }

    bool hasAnyKey() const override { return hasKey(0) || hasKey(1); }

    hal::KeyResult eraseAll() override
    {
        for (Slot &slot : slots_) {
            slot = Slot{};
        }
        ++erases;
        return hal::KeyResult::Ok;
    }

    /// For the tests: what is actually in a slot.
    const uint8_t *raw(uint8_t slot) const { return slots_[slot].key; }
    uint8_t netIdOf(uint8_t slot) const { return slots_[slot].netId; }

    bool failWrites = false;
    size_t writes = 0;
    size_t erases = 0;

private:
    struct Slot {
        uint8_t key[hal::kKeyBytes] = {};
        uint8_t netId = 0;
        bool present = false;
    };
    Slot slots_[hal::kKeySlots];
};

} // namespace fakes

#endif // MAXL_TEST_FAKE_KEY_STORE_H
