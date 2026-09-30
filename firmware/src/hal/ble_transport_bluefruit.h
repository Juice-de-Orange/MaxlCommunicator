/*
 * IBleTransport on Bluefruit52Lib and the S140 SoftDevice.
 *
 * The GATT service from docs/bridge-protocol.md section 1, and nothing above it:
 * the command dispatch, the authorisation tiers and the chunking all live in
 * ble/gatt_server.cpp, where they are testable on a host. This file is the part
 * that cannot be.
 *
 * The UUIDs are the ones bridge/src/transport/webble.ts already advertises. They
 * are written here in the same order as there, and reversed at construction,
 * because Bluefruit takes a 128-bit UUID least-significant byte first and
 * getting that backwards produces a service no client can find -- with no error
 * anywhere.
 *
 * NOT covered by the host tests. Nothing in it can be.
 */

#ifndef MAXL_HAL_BLE_TRANSPORT_BLUEFRUIT_H
#define MAXL_HAL_BLE_TRANSPORT_BLUEFRUIT_H

#include "hal/i_ble_transport.h"

#include <stddef.h>
#include <stdint.h>

namespace hal {

class BluefruitBleTransport : public IBleTransport {
public:
    /**
     * Bring up the SoftDevice, the service and its four characteristics.
     *
     * `deviceName` is what appears in an advertising packet and in the phone's
     * chooser, so it is the device's own name and not the product's -- with two
     * nodes in a rucksack, "MaxlCommunicator" twice is not a chooser anyone can
     * use.
     */
    bool begin(const char *deviceName, IBleChunkSink &sink);

    void startAdvertising();

    bool notify(const uint8_t *chunk, size_t length) override;
    uint16_t mtu() const override;
    bool bonded() const override;
    bool connected() const override;
    void publishStatus(const uint8_t *body, size_t length) override;

    /// Publish the settings blob on the CONFIG characteristic so a generic BLE
    /// tool can read it without implementing the protocol (section 1).
    void publishConfig(const uint8_t *body, size_t length);

    /**
     * The passkey the peer must type, shown on the node's own display.
     *
     * Section 2 requires the passkey to come from the node itself -- a pairing
     * where the phone chooses the number authenticates nothing. Until the UI
     * exists (phase 4) this stores it and reports it; nothing is displayed yet,
     * and that is a gap rather than a decision.
     */
    const char *pendingPasskey() const { return passkey_; }
    bool passkeyPending() const { return passkeyPending_; }

private:
    char passkey_[7] = {};
    bool passkeyPending_ = false;
};

} // namespace hal

#endif // MAXL_HAL_BLE_TRANSPORT_BLUEFRUIT_H
