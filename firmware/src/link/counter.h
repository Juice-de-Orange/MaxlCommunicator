/*
 * The persistent frame counter (CLAUDE.md 2.1).
 *
 * This is the single most consequential piece of state in the firmware. The CCM
 * nonce is built from src and counter; a repeated (src, counter) pair under the
 * same key leaks the XOR of two plaintexts and enables tag forgery (REVIEW.md
 * A2). So the rules are absolute:
 *
 *   - monotonic, never reset, never reused, persisted to flash
 *   - persisted in blocks, so a frame does not cost a flash write
 *   - on boot, resume from the RESERVED high-water mark, not the last used value
 *   - if it cannot be persisted, the device must refuse to transmit
 *   - it survives factory reset (versioning-and-updates.md 5)
 *
 * The block reservation is what makes the third rule matter. Reserving 256 values
 * ahead and persisting the reservation means a power cut loses at most the unused
 * tail of a block -- values are skipped, never repeated. Skipping is free;
 * repeating is a broken cipher.
 *
 * Storage layout, 8 bytes per record:
 *
 *   bytes 0-3   reservedUpTo : u32   first value NOT yet reserved
 *   bytes 4-7   ~reservedUpTo : u32  bitwise complement
 *
 * The complement is a torn-write detector, not a checksum against corruption.
 * A half-written record leaves the two halves inconsistent and the record is
 * ignored; the previous record is still there because records are appended, never
 * overwritten. On boot the highest valid record wins, not the last one, so a
 * trailing garbage record cannot move the counter backwards.
 */

#ifndef MAXL_LINK_COUNTER_H
#define MAXL_LINK_COUNTER_H

#include <stdint.h>

#include "hal/i_block_store.h"

namespace link {

/// Values reserved per flash write. 256 is the figure CLAUDE.md 2.1 suggests.
/// At the default configuration (163 frames/hour) that is one write every 1.6
/// hours, and a power cut discards at most 255 unused values out of 2^32.
constexpr uint32_t kCounterBlockSize = 256;

constexpr size_t kCounterRecordBytes = 8;

class FrameCounter {
public:
    FrameCounter();

    /*
     * Load the high-water mark and reserve the first block.
     *
     * Returns false if the reservation could not be persisted, which latches the
     * counter into a state where next() always fails. That is deliberate: a node
     * that cannot prove where its counter got to must not transmit.
     */
    bool begin(hal::IBlockStore &store);

    /// Hand out the next counter value. False means: do not transmit.
    bool next(uint32_t *out);

    /// The first value not yet reserved. What a reboot would resume from.
    uint32_t reservedUpTo() const { return reservedUpTo_; }

    /// The next value next() would hand out.
    /// The next value that would be handed out. Never zero: zero means "no
    /// counter" everywhere else in the system (docs/bridge-protocol.md
    /// section 3, and the bridge's EventStore::highWaterMark).
    uint32_t peek() const { return nextValue_; }

    /// False once persistence has failed. Never returns to true without a
    /// successful begin() -- a store that failed once is not trusted again
    /// mid-session.
    bool transmitAllowed() const { return healthy_; }

private:
    bool reserveBlock();

    hal::IBlockStore *store_;
    uint32_t nextValue_;
    uint32_t reservedUpTo_;
    bool healthy_;
};

} // namespace link

#endif // MAXL_LINK_COUNTER_H
