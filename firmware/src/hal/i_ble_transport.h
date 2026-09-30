/*
 * The seam between the bridge protocol and the BLE stack underneath it.
 *
 * Same reasoning as hal/i_radio_link.h and decision D3: the SoftDevice and
 * Bluefruit52Lib live in hal/, and ble/ works against this interface. That is
 * what lets the whole of docs/bridge-protocol.md -- the command dispatch, the
 * authorisation tiers, the chunking -- be exercised on a host with no Nordic
 * headers in sight, which is where gates 6.3 to 6.6 are actually testable.
 *
 * Bonding state is read from here rather than tracked in ble/ because only the
 * stack knows it. Section 2 hangs PROVISION_KEY on it: "an unauthenticated GATT
 * write must not be able to set the network key."
 */

#ifndef MAXL_HAL_I_BLE_TRANSPORT_H
#define MAXL_HAL_I_BLE_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

namespace hal {

/*
 * What the transport hands upwards: chunks arriving on the RX characteristic,
 * and the moment the link goes away.
 *
 * It lives here rather than in the Bluefruit driver's header because ble/ has to
 * derive from it, and a class in ble/ must not include a driver -- that would
 * drag Bluefruit52Lib and the SoftDevice into the host build, where gates 6.3 to
 * 6.6 are checked.
 *
 * It used to live in ble_transport_bluefruit.h with a comment saying
 * "ble::GattServer implements this shape". It did -- by coincidence, checked by
 * nobody, because until src/main.cpp nothing ever handed one to the other. An
 * interface two classes agree on by accident is one a signature change breaks
 * silently.
 */
class IBleChunkSink {
public:
    virtual ~IBleChunkSink() = default;

    /// One chunk from the phone. Reassembly is the caller's problem, not this
    /// interface's -- docs/bridge-protocol.md section 1.1.
    virtual void onChunk(const uint8_t *chunk, size_t length, uint32_t nowMs) = 0;

    /// The link is gone. Any half-assembled message is abandoned: section 1.1
    /// discards on disconnect rather than resuming, because a resumed
    /// reassembly across a reconnect could splice two different messages.
    virtual void onDisconnect() = 0;
};

class IBleTransport {
public:
    virtual ~IBleTransport() = default;

    /// Send one chunk on the TX characteristic. False when the link is gone.
    virtual bool notify(const uint8_t *chunk, size_t length) = 0;

    /// Negotiated ATT MTU. docs/bridge-protocol.md section 1: "Assume 23 until
    /// negotiation completes."
    virtual uint16_t mtu() const = 0;

    /// Whether the current connection is bonded and authenticated with a passkey.
    /// The passkey is shown on the node's own display (section 2).
    virtual bool bonded() const = 0;

    virtual bool connected() const = 0;

    /// Publish the STATUS characteristic so a generic BLE tool can read it
    /// without implementing the protocol (section 1).
    virtual void publishStatus(const uint8_t *body, size_t length) = 0;
};

} // namespace hal

#endif // MAXL_HAL_I_BLE_TRANSPORT_H
