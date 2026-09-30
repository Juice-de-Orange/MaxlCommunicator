#include "journal.h"

#include <string.h>

namespace app {
namespace {

void put32(uint8_t *out, uint32_t value)
{
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
    out[3] = static_cast<uint8_t>(value >> 24);
}

uint32_t get32(const uint8_t *in)
{
    return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8)
           | (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
}

} // namespace

size_t Journal::restore(uint32_t counterHighWater)
{
    count_ = 0;
    const size_t stored = store_.count(hal::StoreRegion::EventJournal);
    uint8_t record[kJournalRecordBytes];

    for (size_t i = 0; i < stored && count_ < kJournalCapacity; ++i) {
        if (store_.read(hal::StoreRegion::EventJournal, i, record, sizeof(record))
            != hal::StoreResult::Ok) {
            continue;
        }
        JournalEntry &entry = entries_[count_];
        entry.counter = get32(record);
        entry.opcode = record[4];
        entry.length = record[5] > kMaxJournalBody ? static_cast<uint8_t>(kMaxJournalBody)
                                                   : record[5];
        memcpy(entry.body, record + 6, kMaxJournalBody);

        // Records are written in counter order and read back in the same order.
        // One that is not greater than its predecessor is corrupt, and keeping
        // it would break the ordering that fetch() and acknowledge() rely on.
        if (count_ > 0 && entry.counter <= entries_[count_ - 1].counter) {
            continue;
        }
        /*
         * And one this device could not have written is not ours. Without this
         * a single foreign record poisons the journal permanently: append()
         * insists on increasing counters, so nothing can ever out-rank it and
         * every real event is refused in silence. See the header.
         */
        if (counterHighWater != 0 && entry.counter >= counterHighWater) {
            continue;
        }
        ++count_;
    }
    return count_;
}

bool Journal::persist() const
{
    const hal::StoreRegion region = hal::StoreRegion::EventJournal;
    if (store_.erase(region) != hal::StoreResult::Ok) {
        return false;
    }
    uint8_t record[kJournalRecordBytes];
    for (size_t i = 0; i < count_; ++i) {
        memset(record, 0, sizeof(record));
        put32(record, entries_[i].counter);
        record[4] = entries_[i].opcode;
        record[5] = entries_[i].length;
        memcpy(record + 6, entries_[i].body, kMaxJournalBody);
        if (store_.append(region, record, sizeof(record)) != hal::StoreResult::Ok) {
            return false;
        }
    }
    return store_.sync(region) == hal::StoreResult::Ok;
}

void Journal::dropOldest()
{
    if (count_ == 0) {
        return;
    }
    for (size_t i = 1; i < count_; ++i) {
        entries_[i - 1] = entries_[i];
    }
    --count_;
}

bool Journal::append(uint32_t counter, uint8_t opcode, const uint8_t *body, size_t length)
{
    if (length > kMaxJournalBody) {
        return false;
    }
    // Counters are monotonic by construction (CLAUDE.md 2.1). One that is not
    // is a caller bug, and accepting it would leave the journal unsorted --
    // which is a bug that surfaces much later, as missing events.
    if (count_ > 0 && counter <= entries_[count_ - 1].counter) {
        return false;
    }

    if (count_ == kJournalCapacity) {
        // Nothing here has been acknowledged, or acknowledge() would have made
        // room. The oldest goes, and the fact that it went is recorded.
        dropOldest();
        ++lost_;
    }

    JournalEntry &entry = entries_[count_];
    entry = JournalEntry{};
    entry.counter = counter;
    entry.opcode = opcode;
    entry.length = static_cast<uint8_t>(length);
    if (length > 0) {
        memcpy(entry.body, body, length);
    }
    ++count_;
    return persist();
}

size_t Journal::fetch(uint32_t sinceCounter, JournalEntry *out, size_t max) const
{
    size_t written = 0;
    for (size_t i = 0; i < count_ && written < max; ++i) {
        if (entries_[i].counter > sinceCounter) {
            out[written++] = entries_[i];
        }
    }
    return written;
}

size_t Journal::acknowledge(uint32_t upToCounter)
{
    size_t released = 0;
    while (count_ > 0 && entries_[0].counter <= upToCounter) {
        dropOldest();
        ++released;
    }
    if (released > 0) {
        persist();
    }
    return released;
}

} // namespace app
