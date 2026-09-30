/*
 * What is known about each peer.
 *
 * Feeds the PEERS screen (CLAUDE.md 3.3: "last seen, RSSI, SNR, SF") and the
 * bearing-and-distance readout on POSITION.
 *
 * Deliberately not persisted. Every field here is a fact about the radio link in
 * the last few minutes, and restoring an hour-old RSSI across a reboot would
 * present a stale number as a current one -- worse than an empty field, because
 * the user cannot tell the difference. What must survive a reboot is the counter
 * and the budget, and those live elsewhere.
 *
 * CLAUDE.md 2.5: "treat any received frame as an implicit beacon". So every
 * received frame updates the peer, not only BEACON.
 */

#ifndef MAXL_APP_PEER_STATE_H
#define MAXL_APP_PEER_STATE_H

#include <stddef.h>
#include <stdint.h>

namespace app {

/// "Initial scale: 2 devices. Design must not assume 2." Eight is not a limit
/// anyone meets on foot; it is a bound so the array has a fixed size.
inline constexpr size_t kMaxPeers = 8;

struct Peer {
    uint16_t nodeId = 0;
    bool used = false;

    int16_t lastRssi = 0;
    int8_t lastSnr = 0;
    uint8_t lastSf = 0;

    uint32_t lastSeenUnix = 0;
    uint32_t framesReceived = 0;
    uint32_t framesAcked = 0;

    /// From the peer's last POSITION frame. Zero when it has never sent one.
    int32_t latE7 = 0;
    int32_t lonE7 = 0;
    int16_t altM = 0;
    uint32_t positionAtUnix = 0;
    bool hasPosition = false;
};

class PeerTable {
public:
    /// Record a received frame. Creates the peer on first contact.
    Peer *heard(uint16_t nodeId, int16_t rssi, int8_t snr, uint8_t sf, uint32_t nowUnix);

    /// Record a position reported by that peer.
    void position(uint16_t nodeId, int32_t latE7, int32_t lonE7, int16_t altM, uint32_t nowUnix);

    void acked(uint16_t nodeId);

    const Peer *find(uint16_t nodeId) const;
    size_t size() const;
    const Peer &at(size_t index) const { return peers_[index]; }
    static constexpr size_t slots() { return kMaxPeers; }

    /**
     * Whether contact has lapsed far enough to fall back.
     *
     * CLAUDE.md 2.5: "After 3 x beaconInterval with no contact, both sides
     * return to the rendezvous configuration rather than scanning the SF set."
     */
    bool contactLapsed(uint16_t nodeId, uint32_t nowUnix, uint32_t beaconIntervalS) const;

private:
    Peer peers_[kMaxPeers];

    Peer *slotFor(uint16_t nodeId);
};

} // namespace app

#endif // MAXL_APP_PEER_STATE_H
