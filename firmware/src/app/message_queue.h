/*
 * The outbound message queue.
 *
 * docs/test-plan.md gates 3.1 and 3.2:
 *
 *   3.1  "Queue survives reboot with 20 pending messages -- all present, order
 *         preserved, no duplicates"
 *   3.2  "Queue full behaviour -- oldest-delivered dropped first; user-visible;
 *         never a crash"
 *
 * Both are about the same property: a message the user typed does not disappear
 * because something happened to the device. CLAUDE.md 2.4 is the other half of
 * it -- after three failed attempts a frame "is marked undelivered and surfaced
 * in the UI. It is **not** silently dropped."
 *
 * So this queue never discards an entry that has not reached a terminal state.
 * When it is full it drops the oldest *delivered* entry, and when there is no
 * delivered entry to drop it refuses the new message and says so. Refusing is a
 * visible failure the user can act on; evicting a message that is still waiting
 * for the duty cycle is an invisible one they cannot.
 *
 * Fixed storage, no allocation (CLAUDE.md 3). Persistence goes through
 * hal::IBlockStore, so the tests drive it with fault injection and the device
 * puts it on LittleFS.
 */

#ifndef MAXL_APP_MESSAGE_QUEUE_H
#define MAXL_APP_MESSAGE_QUEUE_H

#include <stddef.h>
#include <stdint.h>

#include "hal/i_block_store.h"
#include "link/payloads.h"

namespace app {

/// The states CLAUDE.md 2.4 requires the UI to keep apart, plus the two terminal
/// ones the queue itself needs.
enum class MessageState : uint8_t {
    Pending = 0,   ///< accepted, not yet handed to the link layer
    Queued,        ///< the budget is holding it; releaseAtUnix says until when
    InFlight,      ///< transmitted, waiting for an ACK or a retry
    Delivered,     ///< acknowledged. The only state that may be evicted.
    Undelivered,   ///< three attempts, gave up. Kept until the user sees it.
};

/// One entry. Fixed width so a record is a record -- see kRecordBytes.
struct QueuedMessage {
    uint32_t id = 0;            ///< monotonic within a power cycle and across reboots
    uint16_t dst = 0;
    uint8_t seq = 0;
    MessageState state = MessageState::Pending;
    uint8_t attempts = 0;
    uint8_t textLen = 0;
    uint32_t createdAtUnix = 0;
    uint32_t releaseAtUnix = 0; ///< only meaningful in Queued
    char text[link::kMaxTextBytes] = {};
};

/// How many messages fit. Small on purpose: at SF9 in g3 the hourly allowance is
/// about 163 frames, and a queue deeper than that is a queue whose tail cannot
/// be sent within the hour it was written in.
inline constexpr size_t kQueueCapacity = 24;

/// Serialised size of one entry. Kept explicit rather than sizeof(QueuedMessage)
/// so that adding a field cannot silently change the on-flash format.
inline constexpr size_t kMessageBytes = 4 + 2 + 1 + 1 + 1 + 1 + 4 + 4 + link::kMaxTextBytes;

/// magic(4) + format(1) + count(1) + nextSeq(1) + seqStored(1) + nextId(4)
inline constexpr size_t kHeaderBytes = 12;

/// Serialised size of one truncated head (D18): dst(2) + seq(1) + attempts(1) +
/// createdAtUnix(4). Explicit for the same reason as kMessageBytes.
inline constexpr size_t kStubBytes = 8;
inline constexpr size_t kMaxStubs = 16;

/*
 * The whole queue is one record, and that is the point.
 *
 * hal::IBlockStore::replaceAll is documented as atomic -- "write a temporary
 * file, sync, rename over the original". erase() followed by a run of append()
 * has a window in which the region holds nothing, and a power cut inside that
 * window is a user's messages gone. Gate 3.1 asks for 20 pending messages to
 * survive a reboot; it does not ask for them to survive a reboot that misses the
 * window.
 *
 * 12 + 24 * 62 + 1 + 16 * 8 = 1629 bytes. Nothing about that is expensive on a
 * 2 MiB chip.
 */
inline constexpr size_t kBlobBytes =
    kHeaderBytes + kQueueCapacity * kMessageBytes + 1 + kMaxStubs * kStubBytes;

enum class Accept : uint8_t {
    Ok = 0,
    TextTooLong,
    /// Full, and every entry is still waiting for something. Surfaced, never
    /// silent: docs/bridge-protocol.md has ERR_QUEUE_FULL for exactly this.
    Full,
    StorageFailed,
};

/*
 * The head of an undelivered message whose text has been given up (D18).
 *
 * Decision D18, 2026-08-31: a full queue with nothing delivered walls the
 * outbox shut for ever -- measured that evening as 23 undelivered entries and
 * every new message refused. Evicting an undelivered entry outright is the
 * silent drop CLAUDE.md 2.4 forbids; keeping all 62 bytes of it for ever is the
 * wall. So the middle: the text is discarded, and WHO it was for, WHEN, after
 * HOW MANY attempts stays visible, marked as truncated. Eight bytes instead of
 * sixty-two.
 */
struct UndeliveredStub {
    uint16_t dst = 0;
    uint8_t seq = 0;
    uint8_t attempts = 0;
    uint32_t createdAtUnix = 0;
};

class MessageQueue {
public:
    explicit MessageQueue(hal::IBlockStore &store) : store_(store) {}

    /// Rebuild from flash. Returns how many entries came back.
    size_t restore();

    /// Accept a message. `id` is assigned and returned through `outId`.
    Accept submit(uint16_t dst, const char *text, size_t textLen, uint32_t nowUnix,
                  uint32_t *outId = nullptr);

    /// Move an entry to a new state. Unknown ids are ignored rather than
    /// treated as errors: a late ACK for something already given up on is
    /// normal, not a fault.
    bool setState(uint32_t id, MessageState state, uint8_t attempts = 0,
                  uint32_t releaseAtUnix = 0);

    /// The oldest entry not yet in a terminal state, or nullptr.
    const QueuedMessage *nextSendable() const;

    size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    const QueuedMessage &at(size_t index) const { return entries_[index]; }

    /// How many entries are in each terminal state -- what the UI counts.
    size_t countInState(MessageState state) const;

    /// Drop delivered entries the user has seen. Returns how many went.
    size_t reclaimDelivered();

    // --- truncated undelivered heads (D18) --------------------------------
    size_t stubCount() const { return stubCount_; }
    const UndeliveredStub &stubAt(size_t index) const { return stubs_[index]; }
    /// Heads that fell off the stub ring because sixteen newer failures arrived.
    /// Nonzero here is the one place D18's reduction loses information.
    uint32_t stubsDropped() const { return stubsDropped_; }

    /// Persist the whole queue. Called after every mutation; the record count is
    /// small and the alternative is a partial write nobody can reason about.
    bool persist() const;

private:
    hal::IBlockStore &store_;
    QueuedMessage entries_[kQueueCapacity];

    /*
     * The serialisation buffer, as a member rather than a local.
     *
     * kBlobBytes is 1500, and the whole queue is one record on purpose (see
     * above). On a local that is 37 % of the Arduino core's 4 kB loop-task stack
     * in a single frame -- and persist() is reached from submit(), which is
     * reached from Node::sendText(), which is reached from loop(). The frames
     * nest.
     *
     * mutable because persist() is const and has no business pretending it needs
     * to modify the queue in order to write it out. Node::fetched_ is the same
     * pattern for the same reason.
     *
     * Caught by -Wframe-larger-than=1024, added on 2026-08-31 after a bring-up
     * sketch put an 8960-byte app::Journal on that stack and took the device off
     * the USB bus until somebody could press reset.
     */
    mutable uint8_t blob_[kBlobBytes];
    size_t count_ = 0;
    uint32_t nextId_ = 1;

    /*
     * The next ARQ sequence number (CLAUDE.md 2.1: "seq:8, wraps freely").
     *
     * Assigned here, at submit, because until 2026-08-31 it was assigned nowhere:
     * QueuedMessage{} zero-initialises, so every frame went on air as seq 0 and
     * the receiver's (src, seq) dedupe rejected everything after the first frame
     * of the session as a duplicate -- rx.frames = 1, replays_rejected = 79 on
     * the first two-node contact.
     *
     * One counter for all peers, not one per peer: uniqueness across peers is
     * what lets an ACK -- which carries only the seq -- be matched to a message
     * without ambiguity. Persisted in the blob header so a reboot does not
     * restart at 0 straight into the peer's 8-deep seq history.
     */
    uint8_t nextSeq_ = 0;

    UndeliveredStub stubs_[kMaxStubs] = {};
    size_t stubCount_ = 0;
    uint32_t stubsDropped_ = 0;

    int indexOf(uint32_t id) const;
    void removeAt(size_t index);
    /// Reduce the entry at `index` to its head on the stub ring (D18).
    void truncateToStub(size_t index);
    static void encode(const QueuedMessage &message, uint8_t *out);
    static void decode(const uint8_t *in, QueuedMessage &out);
};

} // namespace app

#endif // MAXL_APP_MESSAGE_QUEUE_H
