/*
 * The phone-facing protocol, framing only (docs/bridge-protocol.md 1 and 2).
 *
 * That document is normative and client-independent: the PWA and the later Kotlin
 * client are both implementations of it, and nothing here may assume a browser.
 * The same reasoning applies in this direction -- this file knows about chunks,
 * opcodes and transaction ids, and nothing about Bluefruit.
 *
 * Payloads pass through OPAQUE. That is not laziness: docs/decisions D3 forbids
 * ble/ from including link/, and it does not need to. app/ hands EVT_FRAME_RX the
 * radio payload verbatim (bridge-protocol.md 4) and the phone parses it. The
 * encodings both sides use are pinned by test-vectors/radio_payloads.json.
 *
 * Chunk layout (1.1):
 *
 *   byte 0    F(1) | L(1) | msgId(6)      F = first chunk, L = last chunk
 *   byte 1    chunkIndex (0..255)
 *   byte 2..  fragment
 *
 * Message layer after reassembly (1.2):
 *
 *   byte 0    opcode
 *   byte 1    txnId, echoed in the response
 *   byte 2..  body
 *
 * "One message in flight per direction at a time." msgId exists to detect
 * interleaving and loss, not to allow concurrency (1.1).
 */

#ifndef MAXL_BLE_BRIDGE_CODEC_H
#define MAXL_BLE_BRIDGE_CODEC_H

#include <stddef.h>
#include <stdint.h>

namespace ble {

/// docs/bridge-protocol.md, reported in GET_INFO.
/*
 * Went to 2 on 2026-08-31 for EVT_JOURNAL and nothing else -- decision D15.
 * With two nodes there is no rolling upgrade; both get flashed in the same
 * session (CLAUDE.md 6), so the break costs nothing.
 */
constexpr uint8_t kBridgeProtocolVersion = 2;

/// "Maximum reassembled message: 4096 bytes. Larger is a protocol error."
constexpr size_t kMaxMessageBytes = 4096;

/// Chunk header, and what it leaves for the fragment.
constexpr size_t kChunkHeaderBytes = 2;

/// "Assume 23 until negotiation completes", "MTU is negotiated up to 247."
constexpr uint16_t kMinMtu = 23;
constexpr uint16_t kMaxMtu = 247;

/// "chunks of at most MTU - 3 - 2 payload bytes" -- 3 for the ATT header, 2 for
/// the chunk header above.
constexpr size_t maxFragmentBytes(uint16_t mtu)
{
    return (mtu > kMinMtu ? mtu : kMinMtu) - 3u - kChunkHeaderBytes;
}

/// "if 5 s elapse between chunks" the partial message is dropped.
constexpr uint32_t kChunkTimeoutMs = 5000;

constexpr uint8_t kChunkFirst = 0x80;
constexpr uint8_t kChunkLast = 0x40;
constexpr uint8_t kMsgIdMask = 0x3F;

// --- opcodes, docs/bridge-protocol.md 3 -----------------------------------
enum class Opcode : uint8_t {
    GetInfo = 0x01,
    GetStatus = 0x02,
    SendText = 0x03,
    GetQueue = 0x04,
    AckQueue = 0x05,
    GetConfig = 0x06,
    SetConfig = 0x07,
    SetTime = 0x08,
    RequestFix = 0x09,
    ProvisionKey = 0x0A,
    RotateKey = 0x0B,
    FactoryReset = 0x0C,
    GetBudget = 0x0D,
    LinkTest = 0x0E,
};

// --- events and responses, docs/bridge-protocol.md 4 ----------------------
enum class EventCode : uint8_t {
    FrameRx = 0x81,
    FrameTxResult = 0x82,
    Status = 0x83,
    Log = 0x84,
    ConfigApplied = 0x85,
    Fix = 0x86,
    Budget = 0x87,
    /*
     * A journal entry with its counter, and the only place a journal counter
     * appears on the wire (D15).
     *
     * Only GET_QUEUE produces it. It wraps one of the codes above; the counter
     * it carries is the JOURNAL counter, which is not the frame counter that
     * FrameRx and FrameTxResult also carry in their bodies (D10).
     */
    Journal = 0x88,
    ResponseOk = 0xC0,
    ResponseError = 0xC1,
};

/// EVT_JOURNAL's fixed part: counter:u32, opcode:u8, len:u8.
inline constexpr size_t kJournalHeaderBytes = 6;

enum class BridgeError : uint8_t {
    Unsupported = 0x01,
    BadLength = 0x02,
    BadParam = 0x03,
    NotAuthorised = 0x04,
    NoKey = 0x05,
    BudgetExhausted = 0x06,
    QueueFull = 0x07,
    NoTime = 0x08,
    Busy = 0x09,
    Storage = 0x0A,
};

/// docs/bridge-protocol.md 2. Enforced on the device, never by the client.
enum class AuthTier : uint8_t {
    Open = 0,
    Bonded = 1,
};

/*
 * Which tier an opcode needs.
 *
 * Unknown opcodes are Bonded. Defaulting an unrecognised opcode to Open would
 * mean that adding one to the enum is what makes it safe, and forgetting to is
 * what makes it reachable by anyone in Bluetooth range -- the exact failure
 * bridge-protocol.md 2 calls out about revision 1.
 */
AuthTier tierOf(uint8_t opcode);

// --- config TLVs, docs/bridge-protocol.md 3 -------------------------------
enum class ConfigTlv : uint8_t {
    DeviceName = 0x01,
    SniffIntervalMs = 0x02,
    Band = 0x03,
    SfMode = 0x04,
    FixedSf = 0x05,
    TxPowerDbm = 0x06,
    TelemetryIntervalS = 0x07,
    BeaconIntervalS = 0x08,
    GnssFixTimeoutS = 0x09,
};

struct TlvView {
    uint8_t type;
    uint8_t length;
    const uint8_t *value;
};

/*
 * Step through a TLV sequence.
 *
 * Returns false at the end, or on a malformed run. Unknown types are returned
 * like any other: bridge-protocol.md 3 requires them to be ignored and reported
 * back as unapplied, "rather than failing the whole write", so the caller has to
 * see them.
 */
bool nextTlv(const uint8_t *body, size_t bodyLen, size_t *cursor, TlvView *out);

size_t encodeTlv(uint8_t type, const uint8_t *value, uint8_t length, uint8_t *out,
                 size_t outCapacity);

// --- message layer --------------------------------------------------------

/// Write opcode, txnId and body into `out`. Returns bytes written, 0 on failure.
size_t encodeMessage(uint8_t opcode, uint8_t txnId, const uint8_t *body, size_t bodyLen,
                     uint8_t *out, size_t outCapacity);

struct MessageView {
    uint8_t opcode;
    uint8_t txnId;
    const uint8_t *body;
    size_t bodyLen;
};

bool decodeMessage(const uint8_t *message, size_t len, MessageView *out);

// --- chunking -------------------------------------------------------------

/// Splits one message into chunks. Holds a pointer to the caller's buffer, which
/// must outlive it -- there is no copy, because on a device there is no spare
/// 4 KB to copy into.
class Chunker {
public:
    Chunker();

    /// Begin a message. `msgId` wraps freely within its 6 bits.
    bool begin(const uint8_t *message, size_t len, uint16_t mtu, uint8_t msgId);

    /// Emit the next chunk. False when there are none left.
    bool next(uint8_t *out, size_t outCapacity, size_t *written);

    bool done() const;
    size_t chunkCount() const;

private:
    const uint8_t *message_;
    size_t length_;
    size_t offset_;
    size_t fragmentBytes_;
    uint8_t msgId_;
    uint8_t index_;
    bool active_;
};

enum class ReassembleResult : uint8_t {
    NeedMore = 0,
    Complete,
    Discarded,   ///< out of order, msgId changed, timed out, or too long
    BadChunk,    ///< malformed chunk header
};

/// Collects chunks back into a message.
class Reassembler {
public:
    Reassembler();

    ReassembleResult feed(const uint8_t *chunk, size_t len, uint32_t nowMs);

    /// Valid only immediately after Complete.
    const uint8_t *message() const { return buffer_; }
    size_t messageLength() const { return length_; }
    uint8_t msgId() const { return msgId_; }

    /// Drop any partial message. Call on disconnect.
    void reset();

    /// Times out a partial message that has gone quiet. Returns true if one was
    /// dropped. bridge-protocol.md 1.1: "It does not attempt recovery; the sender
    /// re-sends the whole message."
    bool tick(uint32_t nowMs);

private:
    uint8_t buffer_[kMaxMessageBytes];
    size_t length_;
    uint8_t msgId_;
    uint8_t expectedIndex_;
    uint32_t lastChunkMs_;
    bool inProgress_;
};

} // namespace ble

#endif // MAXL_BLE_BRIDGE_CODEC_H
