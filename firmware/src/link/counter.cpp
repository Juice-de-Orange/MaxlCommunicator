#include "counter.h"

namespace link {
namespace {

constexpr hal::StoreRegion kRegion = hal::StoreRegion::FrameCounter;

void encodeRecord(uint32_t value, uint8_t *out)
{
    out[0] = static_cast<uint8_t>(value & 0xFFu);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    out[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    out[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
    const uint32_t inverted = ~value;
    out[4] = static_cast<uint8_t>(inverted & 0xFFu);
    out[5] = static_cast<uint8_t>((inverted >> 8) & 0xFFu);
    out[6] = static_cast<uint8_t>((inverted >> 16) & 0xFFu);
    out[7] = static_cast<uint8_t>((inverted >> 24) & 0xFFu);
}

/// Returns false if the record is torn or was never written.
bool decodeRecord(const uint8_t *in, uint32_t *out)
{
    const uint32_t value = static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
                           (static_cast<uint32_t>(in[2]) << 16) |
                           (static_cast<uint32_t>(in[3]) << 24);
    const uint32_t inverted = static_cast<uint32_t>(in[4]) | (static_cast<uint32_t>(in[5]) << 8) |
                              (static_cast<uint32_t>(in[6]) << 16) |
                              (static_cast<uint32_t>(in[7]) << 24);
    if ((value ^ inverted) != 0xFFFFFFFFu) {
        return false;
    }
    *out = value;
    return true;
}

} // namespace

FrameCounter::FrameCounter()
    : store_(nullptr), nextValue_(0), reservedUpTo_(0), healthy_(false)
{
}

bool FrameCounter::begin(hal::IBlockStore &store)
{
    store_ = &store;
    healthy_ = false;
    nextValue_ = 0;
    reservedUpTo_ = 0;

    if (store.recordSize(kRegion) != kCounterRecordBytes) {
        return false;
    }

    /*
     * Highest valid record, not the last one. A torn trailing record must not be
     * able to move the counter backwards, and neither must a store that returns
     * records in an order we did not expect.
     */
    const size_t records = store.count(kRegion);
    uint32_t highWater = 0;
    for (size_t i = 0; i < records; ++i) {
        uint8_t raw[kCounterRecordBytes];
        if (store.read(kRegion, i, raw, sizeof(raw)) != hal::StoreResult::Ok) {
            continue;
        }
        uint32_t value = 0;
        if (decodeRecord(raw, &value) && value > highWater) {
            highWater = value;
        }
    }

    /*
     * Resume from the reserved high-water mark, not from the last used value --
     * and never hand out zero.
     *
     * Zero is reserved for "nothing". docs/bridge-protocol.md has the phone ask
     * GET_QUEUE(sinceCounter) for "everything after the last counter it durably
     * stored", and a phone that has stored nothing sends 0; the bridge's own
     * EventStore documents highWaterMark() as "the highest counter this store
     * durably holds, **or 0 if it holds nothing**". A device whose first counter
     * really is 0 therefore has one journal entry that no client can ever ask
     * for, and it is the first one.
     *
     * Found by the end-to-end test in test/unit/test_node_integration.cpp, which
     * is the first thing to drive both sides of that contract at once.
     */
    nextValue_ = highWater > 0 ? highWater : 1;
    reservedUpTo_ = highWater;
    healthy_ = true;

    // Reserve immediately rather than on the first next(): a node that cannot
    // persist should discover that at boot, not halfway through its first frame.
    if (!reserveBlock()) {
        healthy_ = false;
        return false;
    }
    return true;
}

bool FrameCounter::reserveBlock()
{
    if (store_ == nullptr) {
        return false;
    }

    const uint32_t target = reservedUpTo_ + kCounterBlockSize;
    if (target < reservedUpTo_) {
        // 2^32 frames at the default rate is roughly three million years. If it
        // ever happens, stopping is the only safe answer -- wrapping would reuse
        // every nonce this device has ever produced.
        return false;
    }

    uint8_t raw[kCounterRecordBytes];
    encodeRecord(target, raw);

    hal::StoreResult result = store_->append(kRegion, raw, sizeof(raw));
    if (result == hal::StoreResult::Full) {
        /*
         * Compaction. replaceAll is atomic by contract (hal/i_block_store.h), so
         * there is no moment at which the region holds nothing -- which is the
         * whole reason that method exists rather than erase() + append().
         */
        result = store_->replaceAll(kRegion, raw, sizeof(raw));
    }
    if (result != hal::StoreResult::Ok) {
        return false;
    }

    // The reservation only counts once it is on flash. Without this a power cut
    // between append and sync would hand out values the next boot re-uses.
    if (store_->sync(kRegion) != hal::StoreResult::Ok) {
        return false;
    }

    reservedUpTo_ = target;
    return true;
}

bool FrameCounter::next(uint32_t *out)
{
    if (!healthy_ || out == nullptr) {
        return false;
    }
    if (nextValue_ >= reservedUpTo_) {
        if (!reserveBlock()) {
            healthy_ = false;
            return false;
        }
    }
    *out = nextValue_;
    ++nextValue_;
    return true;
}

} // namespace link
