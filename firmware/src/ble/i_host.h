/*
 * What the GATT server needs from the application.
 *
 * docs/decisions/0001-open-decisions.md D3: "`ble/` accesses the application
 * through a `ble::IHost` interface that `app/` implements." The
 * layer policy forbids ble -> app, and this interface is how the dependency is
 * inverted: `app/` implements it and hands a reference down.
 *
 * Every method here answers one command from docs/bridge-protocol.md section 3.
 * None of them decide anything about authorisation -- the tier is enforced in
 * gatt_server before any of this is reached, because section 2 is explicit that
 * the tier is "enforced on the device, not by the client", and scattering that
 * check across a dozen call sites is how one of them ends up missing it.
 */

#ifndef MAXL_BLE_I_HOST_H
#define MAXL_BLE_I_HOST_H

#include <stddef.h>
#include <stdint.h>

namespace ble {

/// Filled in for GET_INFO.
struct DeviceInfo {
    uint8_t bridgeProtocol = 1;
    uint16_t nodeId = 0;
    uint8_t firmwareMajor = 0;
    uint8_t firmwareMinor = 0;
    uint8_t firmwarePatch = 0;
    uint8_t wireVersion = 1;
};

/// One entry on its way out through GET_QUEUE.
struct QueuedEvent {
    uint32_t counter = 0;
    uint8_t opcode = 0;
    uint8_t length = 0;
    const uint8_t *body = nullptr;
};

class IHost {
public:
    virtual ~IHost() = default;

    virtual DeviceInfo info() const = 0;

    /// The 17-byte status body from docs/bridge-protocol.md section 4. Returns
    /// how many bytes were written, 0 on failure.
    virtual size_t status(uint8_t *out, size_t max) const = 0;

    /// EVT_BUDGET's body: band u8, usedMs u32, limitMs u32, nextTxUnix u32.
    virtual size_t budget(uint8_t *out, size_t max) const = 0;

    /// Queue a text for transmission. Returns 0 on success or a BridgeError.
    virtual uint8_t sendText(uint16_t dst, const uint8_t *text, size_t length) = 0;

    /// Up to `max` journal entries after `sinceCounter`, oldest first.
    virtual size_t fetchQueue(uint32_t sinceCounter, QueuedEvent *out, size_t max) = 0;

    /// Release journal space up to and including `upToCounter`.
    virtual void ackQueue(uint32_t upToCounter) = 0;

    /// Apply a TLV blob. `appliedMask` and `unapplied` are reported back in
    /// EVT_CONFIG_APPLIED, so unknown types are ignored rather than fatal --
    /// section 3: "Unknown types are ignored and reported back ... rather than
    /// failing the whole write."
    virtual uint8_t setConfig(uint32_t configVersion, const uint8_t *tlvs, size_t length,
                              uint32_t *appliedMask, uint8_t *unapplied,
                              size_t *unappliedCount) = 0;

    /// The current settings, as a TLV blob, for GET_CONFIG.
    virtual size_t getConfig(uint8_t *out, size_t max) const = 0;

    virtual uint8_t setTime(uint32_t unixSeconds) = 0;
    virtual uint8_t requestFix(uint16_t timeoutSeconds) = 0;

    /// PROVISION_KEY. The key never leaves the device: there is no read.
    virtual uint8_t provisionKey(uint8_t slot, uint8_t netId, const uint8_t key[16]) = 0;
    virtual uint8_t rotateKey(uint8_t newSlot) = 0;

    virtual uint8_t factoryReset() = 0;

    /// LINK_TEST. Spends real airtime and is refused rather than queued when the
    /// budget is exhausted -- "a delayed link test is a useless link test".
    virtual uint8_t linkTest(uint16_t dst, uint8_t count, uint8_t sf) = 0;
};

} // namespace ble

#endif // MAXL_BLE_I_HOST_H
