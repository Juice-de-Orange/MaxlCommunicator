/*
 * Where the network key lives.
 *
 * CLAUDE.md 3.0 is specific about this and about why: the key and the device
 * identity go in **internal** flash, "never on the external chip", because "the
 * external chip is trivially readable by anyone with a clip and a logic
 * analyser". Everything else -- counter, budget, queue, journal -- is on the
 * external chip for its wear levelling. The key is the exception.
 *
 * An interface, so that link/ and app/ never touch a filesystem and the tests
 * can drive a store that fails. It is also the seam that keeps this file from
 * having to know about LittleFS, and hence the one that lets the whole key
 * lifecycle -- provisioning, rotation, factory reset -- be tested on a host.
 *
 * Reading a key back out over the air is impossible by construction:
 * docs/bridge-protocol.md section 6, "No key material ever leaves the device.
 * PROVISION_KEY writes; there is no read." There is no opcode for it and this
 * interface's read is for the crypto layer, not for a client.
 */

#ifndef MAXL_HAL_I_KEY_STORE_H
#define MAXL_HAL_I_KEY_STORE_H

#include <stddef.h>
#include <stdint.h>

namespace hal {

/// Matches link::kKeyBytes and link::kKeySlots. Repeated rather than included:
/// hal/ must not depend on link/ (decision D3), and a static assert in the
/// wiring code is what keeps the two honest.
inline constexpr size_t kKeyBytes = 16;
inline constexpr uint8_t kKeySlots = 2;

enum class KeyResult : uint8_t {
    Ok = 0,
    BadSlot,
    NotFound,
    IoError,
};

class IKeyStore {
public:
    virtual ~IKeyStore() = default;

    /// Write a key. Overwrites whatever was in the slot.
    virtual KeyResult store(uint8_t slot, uint8_t netId, const uint8_t key[kKeyBytes]) = 0;

    /// Read one back for the crypto layer. NotFound when the slot is empty.
    virtual KeyResult load(uint8_t slot, uint8_t *netIdOut, uint8_t keyOut[kKeyBytes]) const = 0;

    virtual bool hasKey(uint8_t slot) const = 0;
    virtual bool hasAnyKey() const = 0;

    /// FACTORY_RESET. CLAUDE.md 3.0 keeps the key here and the counter on the
    /// external chip precisely so that wiping one does not touch the other.
    virtual KeyResult eraseAll() = 0;
};

} // namespace hal

#endif // MAXL_HAL_I_KEY_STORE_H
