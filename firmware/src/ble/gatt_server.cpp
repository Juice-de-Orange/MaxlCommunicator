#include "gatt_server.h"

#include <string.h>

namespace ble {
namespace {

/// docs/bridge-protocol.md section 4: RSP_OK and RSP_ERR both echo the opcode.
constexpr size_t kResponseHeaderBytes = 1;

uint32_t readU32(const uint8_t *in)
{
    return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8)
           | (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
}

uint16_t readU16(const uint8_t *in)
{
    return static_cast<uint16_t>(in[0] | (in[1] << 8));
}

void writeU32(uint8_t *out, uint32_t value)
{
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
    out[3] = static_cast<uint8_t>(value >> 24);
}

/// The magic FACTORY_RESET carries, so the command cannot be a stray byte.
constexpr uint32_t kFactoryResetMagic = 0x5245534DUL;

/// How often the STATUS characteristic is refreshed. It exists so a generic BLE
/// tool can read the node cold; it does not need to be live.
constexpr uint32_t kStatusRefreshMs = 5000;

} // namespace

void GattServer::onDisconnect()
{
    reassembler_.reset();
}

void GattServer::tick(uint32_t nowMs)
{
    if (reassembler_.tick(nowMs)) {
        // A message that went quiet mid-transfer. Section 1.1: "It does not
        // attempt recovery; the sender re-sends the whole message."
        ++discarded_;
    }

    // The first pass publishes unconditionally. Waiting a full refresh interval
    // would leave the STATUS characteristic empty for the first five seconds --
    // which is exactly when a phone that just connected reads it, and when
    // somebody with nRF Connect points at the node to see whether it is alive.
    if (!statusEverPublished_ || (nowMs - lastStatusMs_) >= kStatusRefreshMs) {
        statusEverPublished_ = true;
        lastStatusMs_ = nowMs;
        uint8_t body[32];
        const size_t written = host_.status(body, sizeof(body));
        if (written > 0) {
            transport_.publishStatus(body, written);
        }
    }
}

void GattServer::onChunk(const uint8_t *chunk, size_t length, uint32_t nowMs)
{
    switch (reassembler_.feed(chunk, length, nowMs)) {
    case ReassembleResult::Complete: {
        MessageView message{};
        if (!decodeMessage(reassembler_.message(), reassembler_.messageLength(), &message)) {
            return;
        }
        dispatch(message);
        return;
    }
    case ReassembleResult::Discarded:
        // Gate 6.6. Nothing was applied, because dispatch only ever runs on a
        // complete message.
        ++discarded_;
        return;
    case ReassembleResult::BadChunk:
    case ReassembleResult::NeedMore:
        return;
    }
}

bool GattServer::sendMessage(uint8_t opcode, uint8_t txnId, const uint8_t *body, size_t length)
{
    const size_t total = encodeMessage(opcode, txnId, body, length, outMessage_,
                                       sizeof(outMessage_));
    if (total == 0) {
        return false;
    }

    const uint16_t mtu = transport_.mtu();
    if (!chunker_.begin(outMessage_, total, mtu, txMsgId_)) {
        return false;
    }
    txMsgId_ = static_cast<uint8_t>((txMsgId_ + 1) & kMsgIdMask);

    uint8_t chunk[kMaxMtu];
    size_t written = 0;
    while (chunker_.next(chunk, sizeof(chunk), &written)) {
        if (!transport_.notify(chunk, written)) {
            return false;
        }
    }
    return true;
}

bool GattServer::respondOk(uint8_t opcode, uint8_t txnId, const uint8_t *body, size_t length)
{
    uint8_t payload[kMaxResponseBytes];
    if (kResponseHeaderBytes + length > sizeof(payload)) {
        return false;
    }
    payload[0] = opcode;
    if (length > 0) {
        memcpy(payload + kResponseHeaderBytes, body, length);
    }
    return sendMessage(static_cast<uint8_t>(EventCode::ResponseOk), txnId, payload,
                       kResponseHeaderBytes + length);
}

bool GattServer::respondError(uint8_t opcode, uint8_t txnId, BridgeError error)
{
    const uint8_t payload[2] = {opcode, static_cast<uint8_t>(error)};
    return sendMessage(static_cast<uint8_t>(EventCode::ResponseError), txnId, payload,
                       sizeof(payload));
}

bool GattServer::emitEvent(EventCode code, const uint8_t *body, size_t length)
{
    // Events are unsolicited and carry txnId 0 (section 1.2).
    return sendMessage(static_cast<uint8_t>(code), 0, body, length);
}

void GattServer::dispatch(const MessageView &message)
{
    const uint8_t opcode = message.opcode;
    const uint8_t txnId = message.txnId;

    /*
     * The tier check, in one place and before anything else.
     *
     * Section 2: "Tier is enforced on the device, not by the client. A
     * bonded-tier command on an unbonded connection returns
     * ERR_NOT_AUTHORISED. It does not silently no-op -- revision 1's lack of
     * this rule is exactly how you end up with a network key settable by anyone
     * in Bluetooth range."
     *
     * tierOf() defaults unknown opcodes to Bonded, so a command added to the
     * protocol and forgotten here is unreachable rather than unguarded.
     */
    if (tierOf(opcode) == AuthTier::Bonded && !transport_.bonded()) {
        ++rejected_;
        respondError(opcode, txnId, BridgeError::NotAuthorised);
        return;
    }

    const uint8_t *body = message.body;
    const size_t len = message.bodyLen;
    uint8_t out[kMaxResponseBytes];

    switch (static_cast<Opcode>(opcode)) {
    case Opcode::GetInfo: {
        const DeviceInfo info = host_.info();
        out[0] = info.bridgeProtocol;
        out[1] = static_cast<uint8_t>(info.nodeId);
        out[2] = static_cast<uint8_t>(info.nodeId >> 8);
        out[3] = info.firmwareMajor;
        out[4] = info.firmwareMinor;
        out[5] = info.firmwarePatch;
        out[6] = info.wireVersion;
        respondOk(opcode, txnId, out, 7);
        return;
    }

    case Opcode::GetStatus: {
        const size_t written = host_.status(out, sizeof(out));
        respondOk(opcode, txnId, out, written);
        return;
    }

    case Opcode::GetBudget: {
        const size_t written = host_.budget(out, sizeof(out));
        respondOk(opcode, txnId, out, written);
        return;
    }

    case Opcode::SendText: {
        // dst:u16, len:u8, utf8[len]
        if (len < 3 || len != 3u + body[2]) {
            respondError(opcode, txnId, BridgeError::BadLength);
            return;
        }
        const uint8_t error = host_.sendText(readU16(body), body + 3, body[2]);
        if (error != 0) {
            respondError(opcode, txnId, static_cast<BridgeError>(error));
            return;
        }
        respondOk(opcode, txnId, nullptr, 0);
        return;
    }

    case Opcode::GetQueue: {
        if (len != 5) {
            respondError(opcode, txnId, BridgeError::BadLength);
            return;
        }
        const uint32_t since = readU32(body);
        const uint8_t max = body[4];

        QueuedEvent events[32];
        const size_t howMany =
            host_.fetchQueue(since, events, max > 32 ? 32 : max);

        /*
         * The events go out before the response, so a client that counts the
         * response's number knows how many to expect and when to stop asking.
         *
         * Wrapped in EVT_JOURNAL, because they come out of the journal
         * (docs/bridge-protocol.md section 4, decision D15). The wrapper carries
         * the journal counter, which is the value ACK_QUEUE speaks in and the
         * middle column of the server's idempotency key -- and which, until
         * 2026-08-31, was computed here and thrown away, leaving no client able
         * to acknowledge anything at all.
         *
         * A spontaneous event stays bare. That is the whole distinction: this
         * copy is the durable one, the bare one drives the live UI.
         */
        for (size_t i = 0; i < howMany; ++i) {
            writeU32(journalScratch_, events[i].counter);
            journalScratch_[4] = events[i].opcode;
            journalScratch_[5] = events[i].length;
            for (size_t b = 0; b < events[i].length; ++b) {
                journalScratch_[kJournalHeaderBytes + b] = events[i].body[b];
            }
            emitEvent(EventCode::Journal, journalScratch_,
                      kJournalHeaderBytes + events[i].length);
        }
        out[0] = static_cast<uint8_t>(howMany);
        respondOk(opcode, txnId, out, 1);
        return;
    }

    case Opcode::AckQueue: {
        if (len != 4) {
            respondError(opcode, txnId, BridgeError::BadLength);
            return;
        }
        host_.ackQueue(readU32(body));
        respondOk(opcode, txnId, nullptr, 0);
        return;
    }

    case Opcode::GetConfig: {
        const size_t written = host_.getConfig(out, sizeof(out));
        respondOk(opcode, txnId, out, written);
        return;
    }

    case Opcode::SetConfig: {
        if (len < 4) {
            respondError(opcode, txnId, BridgeError::BadLength);
            return;
        }
        uint32_t appliedMask = 0;
        uint8_t unapplied[16];
        size_t unappliedCount = 0;
        const uint8_t error = host_.setConfig(readU32(body), body + 4, len - 4, &appliedMask,
                                              unapplied, &unappliedCount);
        if (error != 0) {
            respondError(opcode, txnId, static_cast<BridgeError>(error));
            return;
        }
        respondOk(opcode, txnId, nullptr, 0);

        /*
         * EVT_CONFIG_APPLIED, and it is not decoration. CLAUDE.md 4.3: "a
         * pushed config is not an applied config", and the dashboard sets
         * config_versions.applied_at from this event and from nothing else.
         */
        uint8_t applied[8 + 16];
        writeU32(applied, readU32(body));
        writeU32(applied + 4, appliedMask);
        for (size_t i = 0; i < unappliedCount && i < 16; ++i) {
            applied[8 + i] = unapplied[i];
        }
        emitEvent(EventCode::ConfigApplied, applied, 8 + unappliedCount);
        return;
    }

    case Opcode::SetTime: {
        if (len != 4) {
            respondError(opcode, txnId, BridgeError::BadLength);
            return;
        }
        const uint8_t error = host_.setTime(readU32(body));
        error == 0 ? respondOk(opcode, txnId, nullptr, 0)
                   : respondError(opcode, txnId, static_cast<BridgeError>(error));
        return;
    }

    case Opcode::RequestFix: {
        if (len != 2) {
            respondError(opcode, txnId, BridgeError::BadLength);
            return;
        }
        const uint8_t error = host_.requestFix(readU16(body));
        error == 0 ? respondOk(opcode, txnId, nullptr, 0)
                   : respondError(opcode, txnId, static_cast<BridgeError>(error));
        return;
    }

    case Opcode::ProvisionKey: {
        // slot:u8, netId:u8, key[16]
        if (len != 18) {
            respondError(opcode, txnId, BridgeError::BadLength);
            return;
        }
        const uint8_t error = host_.provisionKey(body[0], body[1], body + 2);
        error == 0 ? respondOk(opcode, txnId, nullptr, 0)
                   : respondError(opcode, txnId, static_cast<BridgeError>(error));
        return;
    }

    case Opcode::RotateKey: {
        if (len != 1) {
            respondError(opcode, txnId, BridgeError::BadLength);
            return;
        }
        const uint8_t error = host_.rotateKey(body[0]);
        error == 0 ? respondOk(opcode, txnId, nullptr, 0)
                   : respondError(opcode, txnId, static_cast<BridgeError>(error));
        return;
    }

    case Opcode::FactoryReset: {
        if (len != 4) {
            respondError(opcode, txnId, BridgeError::BadLength);
            return;
        }
        // The magic is what stops a corrupted byte from wiping the node.
        if (readU32(body) != kFactoryResetMagic) {
            respondError(opcode, txnId, BridgeError::BadParam);
            return;
        }
        const uint8_t error = host_.factoryReset();
        error == 0 ? respondOk(opcode, txnId, nullptr, 0)
                   : respondError(opcode, txnId, static_cast<BridgeError>(error));
        return;
    }

    case Opcode::LinkTest: {
        if (len != 4) {
            respondError(opcode, txnId, BridgeError::BadLength);
            return;
        }
        const uint8_t error = host_.linkTest(readU16(body), body[2], body[3]);
        error == 0 ? respondOk(opcode, txnId, nullptr, 0)
                   : respondError(opcode, txnId, static_cast<BridgeError>(error));
        return;
    }
    }

    /*
     * Unknown opcode. docs/versioning-and-updates.md: the client "discards it and
     * continues; it does not disconnect" -- and the device owes it an answer
     * rather than silence, or the phone waits out its own timeout for a command
     * this firmware will never understand.
     */
    respondError(opcode, txnId, BridgeError::Unsupported);
}

} // namespace ble
