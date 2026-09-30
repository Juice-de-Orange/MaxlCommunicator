/*
 * Persistent record storage.
 *
 * Backs the two pieces of state that CLAUDE.md refuses to let a power cycle
 * clear: the frame counter high-water mark (2.1) and the duty cycle budget ring
 * (1.2). On the device this is LittleFS on the external ZD25WQ16B (3.0); in the
 * tests it is an array with fault injection, which is the only way to exercise
 * "the device must refuse to transmit" if the counter cannot be persisted.
 *
 * The interface is deliberately record-shaped rather than file-shaped. Both users
 * write fixed-size records and read them back in order; neither needs seeking,
 * partial writes or a directory. Keeping it this narrow is what allows link/ to
 * stay free of LittleFS types (D3) and what keeps the fake honest.
 *
 * Errors are reported, never thrown -- CLAUDE.md 6 builds with -fno-exceptions.
 */

#ifndef MAXL_HAL_I_BLOCK_STORE_H
#define MAXL_HAL_I_BLOCK_STORE_H

#include <stddef.h>
#include <stdint.h>

namespace hal {

enum class StoreResult : uint8_t {
    Ok = 0,
    NotFound,   ///< no record at that index
    Full,       ///< the region cannot take another record
    IoError,    ///< the underlying flash operation failed
    BadLength,  ///< length does not match the region's record size
};

/// A named region of the store. Each user owns exactly one, so a bug in the
/// budget ring cannot corrupt the counter.
enum class StoreRegion : uint8_t {
    FrameCounter = 0,
    BudgetRing,

    /// Outbound messages and their delivery state (CLAUDE.md 3, phase 3).
    /// Survives reboot: gate 3.1 wants 20 pending messages back in order after
    /// a power cycle, and a queue a reboot can clear is not a queue.
    MessageQueue,

    /// The append-only event journal the phone drains with GET_QUEUE
    /// (docs/bridge-protocol.md section 3). Entries are reclaimed only after
    /// ACK_QUEUE, which is what makes it safe to lose the phone mid-sync.
    EventJournal,

    /*
     * Bring-up scratch. The application never touches it.
     *
     * Bring-up 18 used to borrow EventJournal for its cross-reboot log, which
     * held exactly as long as the node journaled nothing: the first real event
     * persists the node's journal over the region and the sketch's fifty
     * cycles become an endless one. A measurement log and the thing being
     * measured cannot share a file.
     */
    BenchScratch,

    RegionCount,
};

class IBlockStore {
public:
    virtual ~IBlockStore() = default;

    /// Fixed record size of a region, in bytes.
    virtual size_t recordSize(StoreRegion region) const = 0;

    /// How many records the region can hold.
    virtual size_t capacity(StoreRegion region) const = 0;

    /// How many records the region currently holds.
    virtual size_t count(StoreRegion region) const = 0;

    /// Append one record. Returns Full rather than overwriting: the ring
    /// semantics belong to the caller, which knows which record is oldest.
    virtual StoreResult append(StoreRegion region, const uint8_t *data, size_t len) = 0;

    /// Read record `index`, 0 being the oldest still present.
    virtual StoreResult read(StoreRegion region, size_t index, uint8_t *out, size_t len) const = 0;

    /// Overwrite record `index` in place. Used by the budget ring to reclaim the
    /// oldest slot once its hour has passed.
    virtual StoreResult write(StoreRegion region, size_t index, const uint8_t *data, size_t len) = 0;

    /// Drop every record in the region.
    virtual StoreResult erase(StoreRegion region) = 0;

    /*
     * Atomically replace the region's entire contents with one record.
     *
     * This exists because erase() followed by append() has a window in which the
     * region holds nothing. For the budget ring that window costs an hour of
     * accounting; for the frame counter it costs the counter, and a counter that
     * came back as zero would reuse CCM nonces against a peer that still
     * remembered the old ones (CLAUDE.md 2.1, REVIEW.md A2). That is not a risk
     * worth taking for a compaction step.
     *
     * IMPLEMENTATIONS MUST MAKE THIS CRASH-SAFE. On LittleFS: write a temporary
     * file, sync, rename over the original -- rename is atomic. An implementation
     * that cannot offer that must not pretend to; returning IoError is better
     * than a silent window.
     */
    virtual StoreResult replaceAll(StoreRegion region, const uint8_t *data, size_t len) = 0;

    /// Flush pending writes to flash. The counter calls this before handing out
    /// a reserved block, because a reservation that is only in RAM is not a
    /// reservation (CLAUDE.md 2.1).
    virtual StoreResult sync(StoreRegion region) = 0;
};

} // namespace hal

#endif // MAXL_HAL_I_BLOCK_STORE_H
