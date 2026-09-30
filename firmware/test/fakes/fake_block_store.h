/*
 * A block store that can be made to fail, and to lose power.
 *
 * The requirements this exists to test cannot be reached with a working store:
 *
 *   "If the counter cannot be persisted, the device must refuse to transmit."
 *                                                        -- CLAUDE.md 2.1
 *   "A budget that a power cycle can clear is not a budget."
 *                                                        -- CLAUDE.md 1.2
 *
 * So the fake offers three things a real one does not: a write budget after which
 * every write fails, a power-cut that discards everything not synced, and a
 * counter of how many writes actually happened -- because "persist in blocks so
 * you are not writing flash on every frame" is a claim that needs checking, not
 * asserting.
 */

#ifndef MAXL_TEST_FAKE_BLOCK_STORE_H
#define MAXL_TEST_FAKE_BLOCK_STORE_H

#include <cstring>
#include <vector>

#include "hal/i_block_store.h"
#include "app/journal.h"
#include "app/message_queue.h"

namespace fakes {

class FakeBlockStore : public hal::IBlockStore {
public:
    static constexpr size_t kRegionCount = static_cast<size_t>(hal::StoreRegion::RegionCount);

    FakeBlockStore()
    {
        // Sizes chosen to match what the modules that own each region declare.
        configure(hal::StoreRegion::FrameCounter, 8, 64);
        configure(hal::StoreRegion::BudgetRing, 10, 512);
        // The whole message queue is one record, so replaceAll can be atomic --
        // see app/message_queue.h.
        configure(hal::StoreRegion::MessageQueue, app::kBlobBytes, 1);
        configure(hal::StoreRegion::EventJournal, app::kJournalRecordBytes, app::kJournalCapacity);
    }

    void configure(hal::StoreRegion region, size_t recordBytes, size_t maxRecords)
    {
        Region &r = regions_[static_cast<size_t>(region)];
        r.recordBytes = recordBytes;
        r.maxRecords = maxRecords;
        r.committed.clear();
        r.pending.clear();
    }

    size_t recordSize(hal::StoreRegion region) const override
    {
        return at(region).recordBytes;
    }

    size_t capacity(hal::StoreRegion region) const override { return at(region).maxRecords; }

    size_t count(hal::StoreRegion region) const override
    {
        return at(region).pending.size() / at(region).recordBytes;
    }

    hal::StoreResult append(hal::StoreRegion region, const uint8_t *data, size_t len) override
    {
        Region &r = at(region);
        if (len != r.recordBytes) {
            return hal::StoreResult::BadLength;
        }
        if (count(region) >= r.maxRecords) {
            return hal::StoreResult::Full;
        }
        if (!consumeWriteBudget()) {
            return hal::StoreResult::IoError;
        }
        r.pending.insert(r.pending.end(), data, data + len);
        ++writes;
        return hal::StoreResult::Ok;
    }

    hal::StoreResult read(hal::StoreRegion region, size_t index, uint8_t *out,
                          size_t len) const override
    {
        const Region &r = at(region);
        if (len != r.recordBytes) {
            return hal::StoreResult::BadLength;
        }
        if (index >= count(region)) {
            return hal::StoreResult::NotFound;
        }
        std::memcpy(out, r.pending.data() + index * r.recordBytes, len);
        return hal::StoreResult::Ok;
    }

    hal::StoreResult write(hal::StoreRegion region, size_t index, const uint8_t *data,
                           size_t len) override
    {
        Region &r = at(region);
        if (len != r.recordBytes) {
            return hal::StoreResult::BadLength;
        }
        if (index >= count(region)) {
            return hal::StoreResult::NotFound;
        }
        if (!consumeWriteBudget()) {
            return hal::StoreResult::IoError;
        }
        std::memcpy(r.pending.data() + index * r.recordBytes, data, len);
        ++writes;
        return hal::StoreResult::Ok;
    }

    hal::StoreResult erase(hal::StoreRegion region) override
    {
        if (!consumeWriteBudget()) {
            return hal::StoreResult::IoError;
        }
        at(region).pending.clear();
        ++writes;
        return hal::StoreResult::Ok;
    }

    hal::StoreResult replaceAll(hal::StoreRegion region, const uint8_t *data, size_t len) override
    {
        Region &r = at(region);
        if (len != r.recordBytes) {
            return hal::StoreResult::BadLength;
        }
        if (!consumeWriteBudget()) {
            return hal::StoreResult::IoError;
        }
        // Atomic by contract, so the fake commits it in one step: there is no
        // intermediate state a power cut could observe.
        r.pending.assign(data, data + len);
        r.committed = r.pending;
        ++writes;
        return hal::StoreResult::Ok;
    }

    hal::StoreResult sync(hal::StoreRegion region) override
    {
        if (!consumeWriteBudget()) {
            return hal::StoreResult::IoError;
        }
        Region &r = at(region);
        r.committed = r.pending;
        ++syncs;
        return hal::StoreResult::Ok;
    }

    // --- test controls -----------------------------------------------------

    /// Everything written but not synced is lost, exactly as pulling the battery
    /// would lose it. docs/test-plan.md 2.8 insists on this rather than a clean
    /// reboot, because a clean shutdown is the easy case.
    void powerCut()
    {
        for (Region &r : regions_) {
            r.pending = r.committed;
        }
    }

    /// Fail every write after `n` more of them. -1 means never fail.
    void failWritesAfter(int n) { writeBudget_ = n; }

    void resetCounters()
    {
        writes = 0;
        syncs = 0;
    }

    size_t writes = 0;
    size_t syncs = 0;

private:
    struct Region {
        size_t recordBytes = 1;
        size_t maxRecords = 0;
        std::vector<uint8_t> pending;    ///< what a reader sees now
        std::vector<uint8_t> committed;  ///< what survives a power cut
    };

    bool consumeWriteBudget()
    {
        if (writeBudget_ < 0) {
            return true;
        }
        if (writeBudget_ == 0) {
            return false;
        }
        --writeBudget_;
        return true;
    }

    Region &at(hal::StoreRegion region) { return regions_[static_cast<size_t>(region)]; }
    const Region &at(hal::StoreRegion region) const
    {
        return regions_[static_cast<size_t>(region)];
    }

    Region regions_[kRegionCount];
    int writeBudget_ = -1;
};

} // namespace fakes

#endif // MAXL_TEST_FAKE_BLOCK_STORE_H
