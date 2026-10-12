#include "replay.h"

namespace link {
namespace {

constexpr hal::StoreRegion kRegion = hal::StoreRegion::ReplayMarks;
constexpr size_t kCheckOffset = kReplayRecordBytes - 4;

/// FNV-1a, 32 bits. A torn-write detector, as the counter's complement is --
/// not a defence against someone who can write the flash.
uint32_t fnv1a(const uint8_t *bytes, size_t len)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

void put16(uint8_t *out, uint16_t value)
{
    out[0] = static_cast<uint8_t>(value & 0xFFu);
    out[1] = static_cast<uint8_t>(value >> 8);
}

void put32(uint8_t *out, uint32_t value)
{
    out[0] = static_cast<uint8_t>(value & 0xFFu);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    out[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    out[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

uint16_t get16(const uint8_t *in)
{
    return static_cast<uint16_t>(in[0] | (in[1] << 8));
}

uint32_t get32(const uint8_t *in)
{
    return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
           (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
}

} // namespace

ReplayGuard::ReplayGuard() : peers_{}, store_(nullptr), storeFault_(false)
{
    reset();
}

bool ReplayGuard::begin(hal::IBlockStore &store)
{
    store_ = &store;
    storeFault_ = store.recordSize(kRegion) != kReplayRecordBytes;
    reset();
    return !storeFault_;
}

void ReplayGuard::reset()
{
    for (size_t i = 0; i < kMaxPeers; ++i) {
        peers_[i] = Peer{};
    }
    if (store_ != nullptr && !storeFault_) {
        restore();
    }
}

void ReplayGuard::restore()
{
    const size_t records = store_->count(kRegion);
    for (size_t r = 0; r < records; ++r) {
        uint8_t raw[kReplayRecordBytes];
        if (store_->read(kRegion, r, raw, sizeof(raw)) != hal::StoreResult::Ok) {
            continue;
        }
        if (fnv1a(raw, kCheckOffset) != get32(raw + kCheckOffset)) {
            continue;  // torn or never written
        }
        const uint8_t peers = raw[0] <= kMaxPeers ? raw[0] : static_cast<uint8_t>(kMaxPeers);
        for (uint8_t p = 0; p < peers; ++p) {
            const uint16_t src = get16(raw + 4 + p * 6);
            const uint32_t mark = get32(raw + 4 + p * 6 + 2);
            Peer *peer = findOrCreatePeer(src);
            if (peer == nullptr || mark <= peer->persistedMark) {
                continue;
            }
            /*
             * Highest mark wins, never the last one read. Everything at or
             * below it counts as seen: the window bits are all set, so the 63
             * counters under the mark are rejected like the mark itself, and
             * anything older falls outside the window and is rejected anyway.
             */
            peer->highWater = mark;
            peer->window = ~0ull;
            peer->persistedMark = mark;
        }
    }
}

bool ReplayGuard::persistMark(uint16_t src, uint32_t mark)
{
    if (storeFault_) {
        return false;
    }

    uint8_t raw[kReplayRecordBytes] = {};
    uint8_t count = 0;
    bool placed = false;
    for (size_t i = 0; i < kMaxPeers; ++i) {
        const Peer &peer = peers_[i];
        if (!peer.used || (peer.persistedMark == 0 && peer.src != src)) {
            continue;  // nothing on flash for this one yet, and not the one being written
        }
        const uint32_t value = (peer.src == src) ? mark : peer.persistedMark;
        placed = placed || peer.src == src;
        put16(raw + 4 + count * 6, peer.src);
        put32(raw + 4 + count * 6 + 2, value);
        ++count;
    }
    if (!placed) {
        if (count >= kMaxPeers) {
            return false;  // unreachable: admit() only creates a peer into a free slot
        }
        put16(raw + 4 + count * 6, src);
        put32(raw + 4 + count * 6 + 2, mark);
        ++count;
    }
    raw[0] = count;
    put32(raw + kCheckOffset, fnv1a(raw, kCheckOffset));

    hal::StoreResult result = store_->append(kRegion, raw, sizeof(raw));
    if (result == hal::StoreResult::Full) {
        // Every record holds the whole table, so one record replaces them all,
        // atomically (hal/i_block_store.h).
        result = store_->replaceAll(kRegion, raw, sizeof(raw));
    }
    // Not reserved until it is on flash: a mark only in RAM is exactly what a
    // power cut forgets.
    if (result != hal::StoreResult::Ok || store_->sync(kRegion) != hal::StoreResult::Ok) {
        storeFault_ = true;
        return false;
    }
    return true;
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

    /*
     * Reserve before accepting. A counter above the persisted mark would be
     * accepted again by a receiver that reboots before the mark moves, so the
     * mark moves first and the frame is accepted only once it is on flash.
     * Counters at or below the mark are already covered and cost no write.
     */
    uint32_t newMark = 0;
    if (store_ != nullptr) {
        const Peer *known = findPeer(src);
        if (known == nullptr || counter > known->persistedMark) {
            newMark = (counter > UINT32_MAX - kReplayMarkBlock) ? UINT32_MAX
                                                                : counter + kReplayMarkBlock;
            if (!persistMark(src, newMark)) {
                return ReplayVerdict::NotPersisted;
            }
        }
    }

    Peer *peer = findOrCreatePeer(src);
    if (peer == nullptr) {
        return ReplayVerdict::NoRoom;
    }
    if (newMark != 0) {
        peer->persistedMark = newMark;
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
