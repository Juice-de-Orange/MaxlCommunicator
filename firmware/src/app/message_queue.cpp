#include "message_queue.h"

#include <string.h>

namespace app {
namespace {

constexpr uint32_t kMagic = 0x514D584DUL; // "MXMQ"

/*
 * Format 2 (2026-08-31): the D18 stub area follows the entries, and the seq
 * counter lives in header bytes 6-7. restore() accepts format 2 and nothing
 * else -- a format-1 blob is discarded WHOLE at the first boot of this
 * firmware. Deliberate: parsing two layouts for ever is how one of them rots,
 * and versioning-and-updates.md flashes both nodes at the same desk, where a
 * dropped outbox is visible and re-typeable. Not acceptable once there are
 * nodes in the field that upgrade unattended; a migration belongs to whoever
 * writes format 3.
 */
constexpr uint8_t kFormat = 2;

constexpr size_t kStubAreaOffset = kHeaderBytes + kQueueCapacity * kMessageBytes;

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

bool terminal(MessageState state)
{
    return state == MessageState::Delivered || state == MessageState::Undelivered;
}

} // namespace

void MessageQueue::encode(const QueuedMessage &message, uint8_t *out)
{
    put32(out, message.id);
    out[4] = static_cast<uint8_t>(message.dst);
    out[5] = static_cast<uint8_t>(message.dst >> 8);
    out[6] = message.seq;
    out[7] = static_cast<uint8_t>(message.state);
    out[8] = message.attempts;
    out[9] = message.textLen;
    put32(out + 10, message.createdAtUnix);
    put32(out + 14, message.releaseAtUnix);
    memcpy(out + 18, message.text, link::kMaxTextBytes);
}

void MessageQueue::decode(const uint8_t *in, QueuedMessage &out)
{
    out.id = get32(in);
    out.dst = static_cast<uint16_t>(in[4] | (in[5] << 8));
    out.seq = in[6];
    out.state = static_cast<MessageState>(in[7]);
    out.attempts = in[8];
    // Clamp on the way in. A corrupted length would otherwise be handed to the
    // UI as a string length and read past the buffer.
    out.textLen = in[9] > link::kMaxTextBytes ? static_cast<uint8_t>(link::kMaxTextBytes) : in[9];
    out.createdAtUnix = get32(in + 10);
    out.releaseAtUnix = get32(in + 14);
    memcpy(out.text, in + 18, link::kMaxTextBytes);
}

size_t MessageQueue::restore()
{
    count_ = 0;
    nextId_ = 1;
    nextSeq_ = 0;
    stubCount_ = 0;

    uint8_t *blob = blob_;
    if (store_.count(hal::StoreRegion::MessageQueue) == 0) {
        return 0;
    }
    if (store_.read(hal::StoreRegion::MessageQueue, 0, blob, kBlobBytes) != hal::StoreResult::Ok) {
        // Unreadable is treated as empty, not as a crash. The alternative is a
        // device that will not boot because of a queue.
        return 0;
    }
    if (get32(blob) != kMagic || blob[4] != kFormat) {
        return 0;
    }

    const uint8_t stored = blob[5];
    const size_t howMany = stored > kQueueCapacity ? kQueueCapacity : stored;
    nextId_ = get32(blob + 8);

    /*
     * blob[7] says whether blob[6] carries the seq counter; persist() always
     * writes 1 there, so 0 means a damaged header. The fallback skips
     * kSeqHistory (8) past the highest seq still in the queue -- past anything
     * the peer's dedupe history could remember, so a fresh message is never
     * mistaken for a retry of one the peer already acknowledged.
     */
    if (blob[7] == 1) {
        nextSeq_ = blob[6];
    } else {
        uint8_t highest = 0;
        for (size_t i = 0; i < howMany; ++i) {
            const uint8_t seq = blob[kHeaderBytes + i * kMessageBytes + 6];
            if (seq > highest) {
                highest = seq;
            }
        }
        nextSeq_ = static_cast<uint8_t>(highest + 1u + 8u);
    }

    // The D18 stub area.
    const uint8_t storedStubs = blob[kStubAreaOffset];
    stubCount_ = storedStubs > kMaxStubs ? kMaxStubs : storedStubs;
    for (size_t i = 0; i < stubCount_; ++i) {
        const uint8_t *in = blob + kStubAreaOffset + 1 + i * kStubBytes;
        stubs_[i].dst = static_cast<uint16_t>(in[0] | (in[1] << 8));
        stubs_[i].seq = in[2];
        stubs_[i].attempts = in[3];
        stubs_[i].createdAtUnix = get32(in + 4);
    }
    bool revived = false;
    for (size_t i = 0; i < howMany; ++i) {
        decode(blob + kHeaderBytes + i * kMessageBytes, entries_[i]);
        if (entries_[i].id >= nextId_) {
            // Never hand out an id that is already on flash: the UI and the
            // journal both key on it.
            nextId_ = entries_[i].id + 1;
        }

        /*
         * An entry that was in flight when the power went is Pending again.
         *
         * InFlight means "handed to the ARQ, waiting for an ACK or a retry", and
         * the ARQ lives in RAM. After a reset there is no slot, no timer and no
         * attempt count -- nothing that could ever move this entry on. Left as
         * it is, it occupies the queue for ever: app::Node::promoteQueued only
         * looks at Pending, so it is never offered again, never acknowledged and
         * never given up on.
         *
         * Measured on node A, 2026-08-31: a queue of 24 with 9 InFlight and 15
         * Undelivered, nothing promoted, nothing transmitted, and every new
         * message refused with ERR_QUEUE_FULL. Gate 3.1 did not catch it because
         * its twenty messages were still Pending -- they had never been sent.
         *
         * CLAUDE.md 2.4 gives a frame three attempts and then requires it to be
         * surfaced as undelivered, "not silently dropped". An entry that can
         * reach neither outcome is exactly the silent drop that rule forbids, so
         * the retry starts again from the beginning. Sending twice is the
         * conservative direction: the receiver dedupes on (src, seq).
         */
        if (entries_[i].state == MessageState::InFlight) {
            entries_[i].state = MessageState::Pending;
            entries_[i].attempts = 0;
            revived = true;
        }
    }
    count_ = howMany;

    // Write the revival back, so a second reset does not have to make the same
    // decision from the same evidence.
    if (revived) {
        persist();
    }
    return count_;
}

bool MessageQueue::persist() const
{
    uint8_t *blob = blob_;
    memset(blob, 0, kBlobBytes);
    put32(blob, kMagic);
    blob[4] = kFormat;
    blob[5] = static_cast<uint8_t>(count_);
    blob[6] = nextSeq_;
    blob[7] = 1; // blob[6] is meaningful; zero here means a corrupted header
    put32(blob + 8, nextId_);
    for (size_t i = 0; i < count_; ++i) {
        encode(entries_[i], blob + kHeaderBytes + i * kMessageBytes);
    }

    // The D18 stub area: count, then the heads.
    blob[kStubAreaOffset] = static_cast<uint8_t>(stubCount_);
    for (size_t i = 0; i < stubCount_; ++i) {
        uint8_t *out = blob + kStubAreaOffset + 1 + i * kStubBytes;
        out[0] = static_cast<uint8_t>(stubs_[i].dst);
        out[1] = static_cast<uint8_t>(stubs_[i].dst >> 8);
        out[2] = stubs_[i].seq;
        out[3] = stubs_[i].attempts;
        put32(out + 4, stubs_[i].createdAtUnix);
    }
    return store_.replaceAll(hal::StoreRegion::MessageQueue, blob, kBlobBytes)
           == hal::StoreResult::Ok;
}

int MessageQueue::indexOf(uint32_t id) const
{
    for (size_t i = 0; i < count_; ++i) {
        if (entries_[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void MessageQueue::removeAt(size_t index)
{
    for (size_t i = index + 1; i < count_; ++i) {
        entries_[i - 1] = entries_[i];
    }
    --count_;
}

void MessageQueue::truncateToStub(size_t index)
{
    const QueuedMessage &entry = entries_[index];

    if (stubCount_ == kMaxStubs) {
        // Sixteen newer failures push the oldest head off. Counted, because
        // this is the single place D18's reduction actually loses something.
        for (size_t i = 1; i < kMaxStubs; ++i) {
            stubs_[i - 1] = stubs_[i];
        }
        --stubCount_;
        ++stubsDropped_;
    }

    UndeliveredStub &stub = stubs_[stubCount_++];
    stub.dst = entry.dst;
    stub.seq = entry.seq;
    stub.attempts = entry.attempts;
    stub.createdAtUnix = entry.createdAtUnix;

    removeAt(index);
}

size_t MessageQueue::countInState(MessageState state) const
{
    size_t total = 0;
    for (size_t i = 0; i < count_; ++i) {
        if (entries_[i].state == state) {
            ++total;
        }
    }
    return total;
}

Accept MessageQueue::submit(uint16_t dst, const char *text, size_t textLen, uint32_t nowUnix,
                            uint32_t *outId)
{
    if (textLen > link::kMaxTextBytes) {
        // Bytes, not characters. "Grüße" is five characters and seven bytes, and
        // truncating by character would overrun the frame on the first umlaut.
        return Accept::TextTooLong;
    }

    if (count_ == kQueueCapacity) {
        /*
         * Gate 3.2: "Oldest-delivered dropped first; user-visible; never a
         * crash." Only a delivered entry may go. An entry still waiting for the
         * duty cycle, still being retried, or sitting undelivered where the user
         * has not seen it yet is not eviction material -- CLAUDE.md 2.4 says an
         * undelivered frame "is not silently dropped", and evicting it to make
         * room for a newer one is precisely that.
         */
        int victim = -1;
        for (size_t i = 0; i < count_; ++i) {
            if (entries_[i].state == MessageState::Delivered) {
                victim = static_cast<int>(i);
                break; // entries are in submission order, so the first is oldest
            }
        }
        if (victim >= 0) {
            removeAt(static_cast<size_t>(victim));
        } else {
            /*
             * Nothing delivered. Decision D18: the oldest UNDELIVERED entry is
             * reduced to its head -- dst, time, attempts stay visible on the
             * stub ring, the text goes. Only when there is nothing undelivered
             * either -- every entry still waiting or in flight -- is the
             * message refused. That refusal is honest: a queue of 24 messages
             * the radio has not managed to move is not a storage problem.
             */
            for (size_t i = 0; i < count_; ++i) {
                if (entries_[i].state == MessageState::Undelivered) {
                    victim = static_cast<int>(i);
                    break;
                }
            }
            if (victim < 0) {
                return Accept::Full;
            }
            truncateToStub(static_cast<size_t>(victim));
        }
    }

    QueuedMessage &entry = entries_[count_];
    entry = QueuedMessage{};
    entry.id = nextId_++;
    entry.dst = dst;
    entry.seq = nextSeq_++;
    entry.state = MessageState::Pending;
    entry.textLen = static_cast<uint8_t>(textLen);
    entry.createdAtUnix = nowUnix;
    memcpy(entry.text, text, textLen);
    ++count_;

    if (!persist()) {
        // Roll back rather than report success for something that is only in
        // RAM. A message the user believes is queued and that a reboot loses is
        // worse than one that was refused.
        --count_;
        --nextId_;
        --nextSeq_;
        return Accept::StorageFailed;
    }
    if (outId != nullptr) {
        *outId = entry.id;
    }
    return Accept::Ok;
}

bool MessageQueue::setState(uint32_t id, MessageState state, uint8_t attempts,
                            uint32_t releaseAtUnix)
{
    const int index = indexOf(id);
    if (index < 0) {
        return false;
    }
    QueuedMessage &entry = entries_[static_cast<size_t>(index)];

    // A terminal state is terminal. A late ACK arriving after three failed
    // attempts must not move a message from Undelivered back to InFlight and
    // start the machinery again.
    if (terminal(entry.state) && !terminal(state)) {
        return false;
    }

    entry.state = state;
    entry.attempts = attempts;
    entry.releaseAtUnix = releaseAtUnix;
    return persist();
}

const QueuedMessage *MessageQueue::nextSendable() const
{
    for (size_t i = 0; i < count_; ++i) {
        if (!terminal(entries_[i].state)) {
            return &entries_[i];
        }
    }
    return nullptr;
}

size_t MessageQueue::reclaimDelivered()
{
    size_t removed = 0;
    size_t i = 0;
    while (i < count_) {
        if (entries_[i].state == MessageState::Delivered) {
            removeAt(i);
            ++removed;
        } else {
            ++i;
        }
    }
    if (removed > 0) {
        persist();
    }
    return removed;
}

} // namespace app
