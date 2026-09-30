#include "peer_state.h"

namespace app {

Peer *PeerTable::slotFor(uint16_t nodeId)
{
    for (Peer &peer : peers_) {
        if (peer.used && peer.nodeId == nodeId) {
            return &peer;
        }
    }
    for (Peer &peer : peers_) {
        if (!peer.used) {
            peer = Peer{};
            peer.used = true;
            peer.nodeId = nodeId;
            return &peer;
        }
    }

    // Full. Evict whoever has been silent longest -- with eight slots and two
    // nodes this cannot happen in the field, but a table that silently ignored a
    // ninth peer would make a new node look like a radio fault.
    Peer *oldest = &peers_[0];
    for (Peer &peer : peers_) {
        if (peer.lastSeenUnix < oldest->lastSeenUnix) {
            oldest = &peer;
        }
    }
    *oldest = Peer{};
    oldest->used = true;
    oldest->nodeId = nodeId;
    return oldest;
}

Peer *PeerTable::heard(uint16_t nodeId, int16_t rssi, int8_t snr, uint8_t sf, uint32_t nowUnix)
{
    Peer *peer = slotFor(nodeId);
    peer->lastRssi = rssi;
    peer->lastSnr = snr;
    peer->lastSf = sf;
    peer->lastSeenUnix = nowUnix;
    ++peer->framesReceived;
    return peer;
}

void PeerTable::position(uint16_t nodeId, int32_t latE7, int32_t lonE7, int16_t altM,
                         uint32_t nowUnix)
{
    Peer *peer = slotFor(nodeId);
    peer->latE7 = latE7;
    peer->lonE7 = lonE7;
    peer->altM = altM;
    peer->positionAtUnix = nowUnix;
    peer->hasPosition = true;
}

void PeerTable::acked(uint16_t nodeId)
{
    Peer *peer = slotFor(nodeId);
    ++peer->framesAcked;
}

const Peer *PeerTable::find(uint16_t nodeId) const
{
    for (const Peer &peer : peers_) {
        if (peer.used && peer.nodeId == nodeId) {
            return &peer;
        }
    }
    return nullptr;
}

size_t PeerTable::size() const
{
    size_t total = 0;
    for (const Peer &peer : peers_) {
        if (peer.used) {
            ++total;
        }
    }
    return total;
}

bool PeerTable::contactLapsed(uint16_t nodeId, uint32_t nowUnix, uint32_t beaconIntervalS) const
{
    const Peer *peer = find(nodeId);
    if (peer == nullptr) {
        return true;
    }
    if (nowUnix < peer->lastSeenUnix) {
        // The wall clock went backwards -- SET_TIME or a GNSS fix corrected it.
        // Treating that as an age of billions of seconds would drop a perfectly
        // good link back to the rendezvous configuration for no reason.
        return false;
    }
    return (nowUnix - peer->lastSeenUnix) > 3u * beaconIntervalS;
}

} // namespace app
