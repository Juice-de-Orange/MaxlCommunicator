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
 */

#ifndef MAXL_LINK_REPLAY_H
#define MAXL_LINK_REPLAY_H

#include <stddef.h>
#include <stdint.h>

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

enum class ReplayVerdict : uint8_t {
    Accept = 0,
    Replayed,    ///< at or below the high-water mark, outside the window
    Duplicate,   ///< seen this (src, seq) recently -- an ARQ retry
    NoRoom,      ///< peer table full
};

class ReplayGuard {
public:
    ReplayGuard();

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

    /// Forget everything. Used by FACTORY_RESET, which clears peer state --
    /// though never the local frame counter (versioning-and-updates.md 5).
    void reset();

private:
    struct Peer {
        uint16_t src;
        bool used;
        uint32_t highWater;
        /// Bit i set means "counter (highWater - 1 - i) has been seen".
        uint64_t window;
        uint8_t recentSeq[kSeqHistory];
        uint8_t recentSeqCount;
        uint8_t recentSeqNext;
    };

    const Peer *findPeer(uint16_t src) const;
    Peer *findOrCreatePeer(uint16_t src);

    Peer peers_[kMaxPeers];
};

} // namespace link

#endif // MAXL_LINK_REPLAY_H
