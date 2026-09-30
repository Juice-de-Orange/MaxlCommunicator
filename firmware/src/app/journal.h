/*
 * The event journal the phone drains.
 *
 * docs/bridge-protocol.md section 3: "The device keeps an append-only event
 * journal on external flash. Each entry has its own journal counter ... The
 * phone asks for everything after the last counter it durably stored, writes
 * those events to its own IndexedDB, pushes them to the server, and only then
 * sends ACK_QUEUE. The device frees journal space up to that counter."
 *
 * The ordering is the whole design and it is why this class has no "mark read"
 * that the fetch path can call. Space is released by acknowledge() and by
 * nothing else, so a phone that dies between GET_QUEUE and ACK_QUEUE costs a
 * repeated transfer and never an event.
 *
 * Gate 3.3: "Fill the journal region; oldest acked entries reclaimed correctly."
 *
 * When the region is full of entries that have NOT been acknowledged, something
 * has to give -- storage is finite and the radio does not stop. The oldest is
 * overwritten and `lostEntries()` counts it. That count is not decoration: it is
 * the only way the phone and the dashboard can know their event log has a hole,
 * and a hole nobody knows about is worse than one that is reported.
 */

#ifndef MAXL_APP_JOURNAL_H
#define MAXL_APP_JOURNAL_H

#include <stddef.h>
#include <stdint.h>

#include "hal/i_block_store.h"

namespace app {

/// Longest event body worth journalling: EVT_FRAME_RX is 11 bytes of header
/// plus a payload, and the largest payload is a 48-byte TEXT frame.
inline constexpr size_t kMaxJournalBody = 64;

/// counter(4) + opcode(1) + len(1) + body
inline constexpr size_t kJournalRecordBytes = 6 + kMaxJournalBody;

/// Entries held before the oldest is overwritten. At the g3 hourly allowance of
/// about 163 frames, this is several hours of the busiest plausible traffic --
/// long enough that a phone which syncs daily loses nothing, and small enough
/// that it is a few kilobytes of a 2 MiB chip.
inline constexpr size_t kJournalCapacity = 128;

struct JournalEntry {
    uint32_t counter = 0;
    uint8_t opcode = 0;
    uint8_t length = 0;
    uint8_t body[kMaxJournalBody] = {};
};

class Journal {
public:
    explicit Journal(hal::IBlockStore &store) : store_(store) {}

    /**
     * Rebuild from flash. Returns the number of entries held.
     *
     * `counterHighWater` is the first counter value this device has not yet
     * reserved (link::FrameCounter::reservedUpTo). A stored record at or above
     * it cannot have been written by this device, so it is not ours and is
     * dropped.
     *
     * That bound is not defensive programming; it is a bug that cost node A its
     * entire event log. sketch_10_hal.cpp exercises the journal region by
     * leaving one record of the pattern 0x40,0x41,0x42,... behind, whose first
     * four bytes restore as counter 0x43424140 -- 1.13 billion. append() then
     * refuses every real event for ever, because it requires counters to
     * increase and nothing will ever out-rank that. Silently: it returns false
     * and raises nothing. Node A ran 20.8 h on 2026-08-31 and reported
     * journal.entries = 1 with all 21 of its events swallowed, and a reflash
     * does not clear it -- the region survives.
     *
     * Passing 0 means "no bound", for callers with no counter to hand.
     */
    size_t restore(uint32_t counterHighWater = 0);

    /**
     * Append one event.
     *
     * `counter` comes from the caller, not from here: it is drawn from the same
     * monotonic, persistent supply as frame counters (CLAUDE.md 2.1, decision
     * D10), and that supply belongs to link/counter.
     */
    bool append(uint32_t counter, uint8_t opcode, const uint8_t *body, size_t length);

    /// Entries with a counter strictly greater than `sinceCounter`, oldest
    /// first, at most `max`. This is GET_QUEUE.
    size_t fetch(uint32_t sinceCounter, JournalEntry *out, size_t max) const;

    /// Release everything up to and including `upToCounter`. This is ACK_QUEUE,
    /// and it is the only thing that frees space.
    size_t acknowledge(uint32_t upToCounter);

    size_t size() const { return count_; }
    size_t capacity() const { return kJournalCapacity; }

    /*
     * Direct access, oldest first.
     *
     * fetch() is the phone's view -- everything after a counter, copied into the
     * caller's buffer. The MESSAGES screen wants the opposite end: the last few
     * entries, without copying 128 records of 70 bytes onto a stack that is 4 KB
     * in total. Indexing is what that needs, and it costs nothing.
     *
     * Undefined for index >= size(). Callers walk backwards from size().
     */
    const JournalEntry &at(size_t index) const { return entries_[index]; }

    /// How many entries were overwritten before anyone acknowledged them. A
    /// non-zero value means the log has a gap.
    uint32_t lostEntries() const { return lost_; }

    uint32_t highestCounter() const { return count_ == 0 ? 0 : entries_[count_ - 1].counter; }
    uint32_t oldestCounter() const { return count_ == 0 ? 0 : entries_[0].counter; }

    bool persist() const;

private:
    hal::IBlockStore &store_;
    /*
     * A plain array kept in counter order rather than a head/tail ring.
     *
     * A ring saves the memmove on eviction, which happens at most once per
     * appended event and moves 8 kB. That is microseconds against a frame
     * measured in hundreds of milliseconds (CLAUDE.md 1.4). What the array buys
     * is that fetch() and acknowledge() are a scan over a sorted range with no
     * wraparound arithmetic -- and wraparound arithmetic is exactly where the
     * bug that loses somebody's messages would live.
     */
    JournalEntry entries_[kJournalCapacity];
    size_t count_ = 0;
    uint32_t lost_ = 0;

    void dropOldest();
};

} // namespace app

#endif // MAXL_APP_JOURNAL_H
