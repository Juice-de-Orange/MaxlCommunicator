#include "bridge_codec.h"

namespace ble {

AuthTier tierOf(uint8_t opcode)
{
    /*
     * docs/bridge-protocol.md 2: exactly three opcodes are open.
     *
     * "Open -- permitted on any connection, including unbonded: GET_INFO,
     *  GET_STATUS, GET_BUDGET."
     *
     * Everything else, and PROVISION_KEY in particular, needs a bonded
     * connection with passkey entry.
     */
    switch (static_cast<Opcode>(opcode)) {
    case Opcode::GetInfo:
    case Opcode::GetStatus:
    case Opcode::GetBudget:
        return AuthTier::Open;
    default:
        return AuthTier::Bonded;
    }
}

bool nextTlv(const uint8_t *body, size_t bodyLen, size_t *cursor, TlvView *out)
{
    if (body == nullptr || cursor == nullptr || out == nullptr) {
        return false;
    }
    // Type and length must both be present, and the value must fit.
    if (*cursor + 2u > bodyLen) {
        return false;
    }
    const uint8_t type = body[*cursor];
    const uint8_t length = body[*cursor + 1u];
    if (*cursor + 2u + length > bodyLen) {
        return false;  // truncated run; the caller rejects the whole write
    }
    out->type = type;
    out->length = length;
    out->value = body + *cursor + 2u;
    *cursor += 2u + length;
    return true;
}

size_t encodeTlv(uint8_t type, const uint8_t *value, uint8_t length, uint8_t *out,
                 size_t outCapacity)
{
    if (out == nullptr || outCapacity < 2u + length) {
        return 0;
    }
    if (length > 0 && value == nullptr) {
        return 0;
    }
    out[0] = type;
    out[1] = length;
    for (uint8_t i = 0; i < length; ++i) {
        out[2u + i] = value[i];
    }
    return 2u + length;
}

size_t encodeMessage(uint8_t opcode, uint8_t txnId, const uint8_t *body, size_t bodyLen,
                     uint8_t *out, size_t outCapacity)
{
    if (out == nullptr || bodyLen + 2u > kMaxMessageBytes || outCapacity < bodyLen + 2u) {
        return 0;
    }
    if (bodyLen > 0 && body == nullptr) {
        return 0;
    }
    out[0] = opcode;
    out[1] = txnId;
    for (size_t i = 0; i < bodyLen; ++i) {
        out[2u + i] = body[i];
    }
    return bodyLen + 2u;
}

bool decodeMessage(const uint8_t *message, size_t len, MessageView *out)
{
    if (message == nullptr || out == nullptr || len < 2u) {
        return false;
    }
    out->opcode = message[0];
    out->txnId = message[1];
    out->body = message + 2u;
    out->bodyLen = len - 2u;
    return true;
}

// --- Chunker --------------------------------------------------------------

Chunker::Chunker()
    : message_(nullptr), length_(0), offset_(0), fragmentBytes_(0), msgId_(0), index_(0),
      active_(false)
{
}

bool Chunker::begin(const uint8_t *message, size_t len, uint16_t mtu, uint8_t msgId)
{
    if (message == nullptr || len == 0 || len > kMaxMessageBytes) {
        return false;
    }
    message_ = message;
    length_ = len;
    offset_ = 0;
    fragmentBytes_ = maxFragmentBytes(mtu);
    msgId_ = static_cast<uint8_t>(msgId & kMsgIdMask);
    index_ = 0;
    active_ = true;

    /*
     * chunkIndex is a single byte, so a message longer than 256 fragments cannot
     * be addressed. At the 23-byte minimum MTU a fragment is 18 bytes, which caps
     * an unnegotiated connection at 4608 -- above the 4096 message limit, so this
     * is unreachable through the documented path and is a guard, not a case.
     */
    const size_t chunks = (len + fragmentBytes_ - 1u) / fragmentBytes_;
    if (chunks > 256u) {
        active_ = false;
        return false;
    }
    return true;
}

bool Chunker::done() const
{
    return !active_ || offset_ >= length_;
}

size_t Chunker::chunkCount() const
{
    if (length_ == 0 || fragmentBytes_ == 0) {
        return 0;
    }
    return (length_ + fragmentBytes_ - 1u) / fragmentBytes_;
}

bool Chunker::next(uint8_t *out, size_t outCapacity, size_t *written)
{
    if (done() || out == nullptr) {
        return false;
    }

    size_t remaining = length_ - offset_;
    size_t take = (remaining < fragmentBytes_) ? remaining : fragmentBytes_;
    if (outCapacity < kChunkHeaderBytes + take) {
        return false;
    }

    uint8_t flags = msgId_;
    if (offset_ == 0) {
        flags = static_cast<uint8_t>(flags | kChunkFirst);
    }
    if (take == remaining) {
        flags = static_cast<uint8_t>(flags | kChunkLast);
    }

    out[0] = flags;
    out[1] = index_;
    for (size_t i = 0; i < take; ++i) {
        out[kChunkHeaderBytes + i] = message_[offset_ + i];
    }

    offset_ += take;
    ++index_;
    if (written != nullptr) {
        *written = kChunkHeaderBytes + take;
    }
    return true;
}

// --- Reassembler ----------------------------------------------------------

Reassembler::Reassembler()
    : buffer_{}, length_(0), msgId_(0), expectedIndex_(0), lastChunkMs_(0), inProgress_(false)
{
}

void Reassembler::reset()
{
    length_ = 0;
    expectedIndex_ = 0;
    inProgress_ = false;
}

bool Reassembler::tick(uint32_t nowMs)
{
    if (!inProgress_) {
        return false;
    }
    if (static_cast<uint32_t>(nowMs - lastChunkMs_) < kChunkTimeoutMs) {
        return false;
    }
    reset();
    return true;
}

ReassembleResult Reassembler::feed(const uint8_t *chunk, size_t len, uint32_t nowMs)
{
    if (chunk == nullptr || len < kChunkHeaderBytes) {
        return ReassembleResult::BadChunk;
    }

    const uint8_t flags = chunk[0];
    const uint8_t index = chunk[1];
    const bool first = (flags & kChunkFirst) != 0;
    const bool last = (flags & kChunkLast) != 0;
    const uint8_t msgId = static_cast<uint8_t>(flags & kMsgIdMask);
    const size_t fragmentLen = len - kChunkHeaderBytes;

    // A quiet partial message is dropped before this chunk is considered, so a
    // late fragment cannot be stitched onto a stale one.
    (void)tick(nowMs);

    if (first) {
        // A new first chunk replaces whatever was in progress. The sender only
        // re-sends a whole message, so a new start means the old one is gone.
        reset();
        msgId_ = msgId;
        inProgress_ = true;
        expectedIndex_ = 0;
    } else if (!inProgress_) {
        // A continuation with nothing to continue.
        return ReassembleResult::Discarded;
    }

    if (msgId != msgId_) {
        // "if msgId changes mid-message" -- interleaving, which this protocol
        // does not allow (bridge-protocol.md 6: no concurrency).
        reset();
        return ReassembleResult::Discarded;
    }
    if (index != expectedIndex_) {
        // "if a chunk arrives out of order". No recovery is attempted; the sender
        // re-sends the whole message.
        reset();
        return ReassembleResult::Discarded;
    }
    if (length_ + fragmentLen > kMaxMessageBytes) {
        reset();
        return ReassembleResult::Discarded;
    }

    for (size_t i = 0; i < fragmentLen; ++i) {
        buffer_[length_ + i] = chunk[kChunkHeaderBytes + i];
    }
    length_ += fragmentLen;
    ++expectedIndex_;
    lastChunkMs_ = nowMs;

    if (last) {
        inProgress_ = false;
        return ReassembleResult::Complete;
    }
    return ReassembleResult::NeedMore;
}

} // namespace ble
