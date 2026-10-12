/*
 * Replay window and duplicate suppression (CLAUDE.md 2.1, 2.4).
 *
 * Two different jobs that are easy to confuse:
 *
 *   the replay window  is about CRYPTO. "The receiver keeps a replay window per
 *                      peer and rejects anything at or below the high-water mark
 *                      outside the window." It defends against a captured frame
 *                      being retransmitted by someone else (docs/test-plan.md 2.3).
 *
 *   the (src, seq) dedupe  is about ARQ. A retry carries the same seq as the
 *                      original, so a frame whose ACK was lost arrives twice and
 *                      must be delivered to the app once (2.4 step 2).
 *
 * The window exists because frames can legitimately arrive out of order -- a
 * retry scheduled behind a budget block can be overtaken by a later frame -- so
 * "counter must strictly increase" would reject honest traffic. A sliding window
 * of 64 accepts reordering up to 64 frames deep and rejects everything older,
 * which at the default rate is about 24 minutes of history.
 *
 * THE HIGH-WATER MARK SURVIVES A REBOOT (issue #2). Kept in RAM only, a
 * receiver that rebooted forgot every peer, and a frame captured off the air
 * before the reboot was accepted again after it. It is persisted the way the
 * frame counter is (link/counter.h): not on every frame, but as a reservation
 * ahead of what has been seen. When a peer's accepted counter passes its
 * reserved mark, the mark moves to counter + kReplayMarkBlock and is synced to
 * flash BEFORE the frame is accepted. On boot every peer resumes with its
 * reserved mark as the high-water mark and the whole window below it marked
 * as seen, so everything at or below the mark is rejected.
 *
 * The price is the mirror image of the counter's. A counter reservation skips
 * values; a mark reservation refuses the peer's honest frames between what was
 * really seen and the reserved mark, once, after a reboot. Their retries
 * travel under fresh counters and get through (2.4), so the cost is at most a
 * few refused transmissions -- a sender spends a counter on each frame and on
 * each journal entry, so 16 counters are a handful of frames. A replay that is
 * accepted is not a cost that can be paid back.
 *
 * Storage layout, one record per reservation, 56 bytes:
 *
 *   byte  0       count     : u8    peers in this record, at most kMaxPeers
 *   bytes 1-3     reserved  : 0
 *   bytes 4-51    kMaxPeers x { src : u16, mark : u32 }
 *   bytes 52-55   check     : u32   FNV-1a over bytes 0-51
 *
 * Every record holds the whole table, so a compaction is one atomic
 * replaceAll() and there is no moment at which the region has forgotten a
 * peer. On boot each peer's highest mark across all valid records wins, so a
 * torn trailing record cannot move a mark backwards.
 */

#ifndef TRAILTALK_LINK_REPLAY_H
#define TRAILTALK_LINK_REPLAY_H

#include <stddef.h>
#include <stdint.h>

#include "hal/i_block_store.h"

namespace link {

/// How many peers are tracked. CLAUDE.md 0 says the design must not assume two
/// nodes; eight is what fits comfortably in RAM and is far past the "handful of
/// nodes" the phase plan contemplates.
constexpr size_t kMaxPeers = 8;

/// Frames of reordering tolerated below the high-water mark.
constexpr uint32_t kReplayWindowSize = 64;

/// How many recent (src, seq) pairs are remembered per peer for ARQ dedupe. The
/// ARQ is stop-and-wait with at most 3 retries, so anything beyond a handful is
/// generosity.
constexpr size_t kSeqHistory = 8;

/// How far ahead of an accepted counter the persisted mark is reserved. One
/// flash write per this many counter values of a peer; at most this many of
/// its counter values refused after a reboot.
constexpr uint32_t kReplayMarkBlock = 16;

constexpr size_t kReplayRecordBytes = 4 + kMaxPeers * 6 + 4;

/// Records the region holds before a compaction. Its owner configures the
/// store with this (hal/ does not read link/ constants -- decision D3).
constexpr size_t kReplayRecordCapacity = 64;

enum class ReplayVerdict : uint8_t {
    Accept = 0,
    Replayed,      ///< at or below the high-water mark, outside the window
    Duplicate,     ///< seen this (src, seq) recently -- an ARQ retry
    NoRoom,        ///< peer table full
    NotPersisted,  ///< would need a new mark on flash, and the store failed
};

class ReplayGuard {
public:
    ReplayGuard();

    /*
     * Restore the persisted marks and persist from now on.
     *
     * Without begin() the guard is RAM only, which is what the unit tests of
     * the window itself and the link simulation use. With it, a frame that
     * needs a new mark is accepted only once that mark is on flash; if the
     * store is unusable, or fails later, such frames are refused with
     * NotPersisted. That is the safe failure for the same reason as the
     * counter's: a receiver that cannot remember what it accepted must not
     * accept what it could not refuse after a reboot. Frames under a mark
     * that is already on flash are still judged normally.
     *
     * Returns false if the store is unusable.
     */
    bool begin(hal::IBlockStore &store);

    /*
     * Judge a frame that has already passed its MIC check.
     *
     * Order matters: this must run AFTER authentication. Updating a replay window
     * from an unauthenticated frame would let anyone advance a peer's high-water
     * mark and lock out the real one.
     *
     * Accepting a frame records it. Rejecting one changes nothing.
     *
     * `seqParticipates` says whether this frame takes part in the (src, seq)
     * dedupe -- true exactly when the frame carries ACK_REQ. Only those frames
     * are ever retried, so only they can arrive twice with the same seq. An ACK
     * or a beacon does not retry, and recording its seq would poison the
     * history: the peer's ACK echoes OUR seq, and both sides count their seqs
     * from zero, so its next honest message would collide with the echo and be
     * dropped as a duplicate while still being acknowledged.
     */
    ReplayVerdict admit(uint16_t src, uint32_t counter, uint8_t seq,
                        bool seqParticipates = true);

    /// Test a frame without recording it.
    ReplayVerdict inspect(uint16_t src, uint32_t counter, uint8_t seq,
                          bool seqParticipates = true) const;

    /// Highest counter accepted from a peer, or 0 if it is unknown.
    uint32_t highWaterMark(uint16_t src) const;

    bool knowsPeer(uint16_t src) const;

    /// Forget the in-RAM history. The persisted marks are not touched and are
    /// restored again if begin() was called: like the frame counter they
    /// survive FACTORY_RESET (versioning-and-updates.md 5), because the network
    /// key is usually provisioned again afterwards, and every frame recorded
    /// under it would then authenticate.
    void reset();

private:
    struct Peer {
        uint16_t src;
        bool used;
        uint32_t highWater;
        /// The mark on flash: every counter at or below it is rejected after
        /// a reboot. Zero means none persisted.
        uint32_t persistedMark;
        /// Bit i set means "counter (highWater - 1 - i) has been seen".
        uint64_t window;
        uint8_t recentSeq[kSeqHistory];
        uint8_t recentSeqCount;
        uint8_t recentSeqNext;
    };

    const Peer *findPeer(uint16_t src) const;
    Peer *findOrCreatePeer(uint16_t src);

    /// Load the marks from the store into an empty table.
    void restore();

    /// Put the table, with `src` at `mark`, on flash. False latches the fault.
    bool persistMark(uint16_t src, uint32_t mark);

    Peer peers_[kMaxPeers];
    hal::IBlockStore *store_;
    bool storeFault_;
};

} // namespace link

#endif // TRAILTALK_LINK_REPLAY_H
