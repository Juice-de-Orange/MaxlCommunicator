#include "replay.h"

namespace link {

ReplayGuard::ReplayGuard() : peers_{}
{
    reset();
}

void ReplayGuard::reset()
{
    for (size_t i = 0; i < kMaxPeers; ++i) {
        peers_[i] = Peer{};
    }
}

const ReplayGuard::Peer *ReplayGuard::findPeer(uint16_t src) const
{
    for (size_t i = 0; i < kMaxPeers; ++i) {
        if (peers_[i].used && peers_[i].src == src) {
            return &peers_[i];
        }
    }
    return nullptr;
}

ReplayGuard::Peer *ReplayGuard::findOrCreatePeer(uint16_t src)
{
    for (size_t i = 0; i < kMaxPeers; ++i) {
        if (peers_[i].used && peers_[i].src == src) {
            return &peers_[i];
        }
    }
    for (size_t i = 0; i < kMaxPeers; ++i) {
        if (!peers_[i].used) {
            peers_[i] = Peer{};
            peers_[i].used = true;
            peers_[i].src = src;
            return &peers_[i];
        }
    }
    /*
     * Table full. Deliberately no eviction: evicting a peer resets its window,
     * and an attacker who can make the table overflow could then replay against
     * the evicted peer. Refusing the ninth peer is the safe failure.
     */
    return nullptr;
}

bool ReplayGuard::knowsPeer(uint16_t src) const
{
    return findPeer(src) != nullptr;
}

uint32_t ReplayGuard::highWaterMark(uint16_t src) const
{
    const Peer *peer = findPeer(src);
    return (peer != nullptr) ? peer->highWater : 0u;
}

ReplayVerdict ReplayGuard::inspect(uint16_t src, uint32_t counter, uint8_t seq,
                                   bool seqParticipates) const
{
    const Peer *peer = findPeer(src);
    if (peer == nullptr) {
        // An unknown peer with a free slot is always acceptable: we have no
        // history to judge it against, and its first frame establishes one.
        for (size_t i = 0; i < kMaxPeers; ++i) {
            if (!peers_[i].used) {
                return ReplayVerdict::Accept;
            }
        }
        return ReplayVerdict::NoRoom;
    }

    if (counter > peer->highWater) {
        // Ahead of everything seen. Could still be an ARQ retry of a frame we
        // already delivered, which the seq history below catches.
    } else {
        const uint32_t age = peer->highWater - counter;
        if (age >= kReplayWindowSize) {
            return ReplayVerdict::Replayed;  // too old to judge; assume replay
        }
        if (age == 0 || (peer->window & (1ull << (age - 1u))) != 0u) {
            return ReplayVerdict::Replayed;  // seen this exact counter before
        }
    }

    for (uint8_t i = 0; seqParticipates && i < peer->recentSeqCount; ++i) {
        if (peer->recentSeq[i] == seq) {
            /*
             * Same seq, new counter: an ARQ retry. CLAUDE.md 2.4 has the receiver
             * "dedupe on (src, seq)" so the application sees the message once --
             * but the ACK still has to go out, or the sender keeps retrying.
             */
            return ReplayVerdict::Duplicate;
        }
    }

    return ReplayVerdict::Accept;
}

ReplayVerdict ReplayGuard::admit(uint16_t src, uint32_t counter, uint8_t seq,
                                 bool seqParticipates)
{
    const ReplayVerdict verdict = inspect(src, counter, seq, seqParticipates);
    if (verdict == ReplayVerdict::Replayed || verdict == ReplayVerdict::NoRoom) {
        return verdict;
    }

    Peer *peer = findOrCreatePeer(src);
    if (peer == nullptr) {
        return ReplayVerdict::NoRoom;
    }

    if (peer->highWater == 0 && peer->window == 0 && peer->recentSeqCount == 0) {
        // First frame from this peer establishes the mark.
        peer->highWater = counter;
    } else if (counter > peer->highWater) {
        const uint32_t advance = counter - peer->highWater;
        /*
         * Slide the window. The bit for the old high-water mark moves into the
         * window as "seen"; anything shifted past 64 falls off and will be
         * rejected as too old, which is the point of a bounded window.
         */
        if (advance >= 64u) {
            peer->window = 0;
        } else {
            peer->window = (peer->window << advance) | (1ull << (advance - 1u));
        }
        peer->highWater = counter;
    } else {
        const uint32_t age = peer->highWater - counter;
        if (age > 0 && age <= 64u) {
            peer->window |= (1ull << (age - 1u));
        }
    }

    // A duplicate seq is still recorded above -- its counter is genuinely new and
    // must not be accepted a second time -- but the caller is told not to deliver
    // it to the application. Frames outside the dedupe (ACKs, beacons) leave the
    // seq history untouched; see replay.h for what recording them would break.
    if (verdict == ReplayVerdict::Accept && seqParticipates) {
        if (peer->recentSeqCount < kSeqHistory) {
            peer->recentSeq[peer->recentSeqCount++] = seq;
        } else {
            peer->recentSeq[peer->recentSeqNext] = seq;
            peer->recentSeqNext =
                static_cast<uint8_t>((peer->recentSeqNext + 1u) % kSeqHistory);
        }
    }

    return verdict;
}

} // namespace link
