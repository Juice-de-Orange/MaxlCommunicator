/*
 * Radio frame header (CLAUDE.md 2.1).
 *
 *   byte 0       ver:4 | type:4
 *   byte 1       netId:8
 *   bytes 2-3    src:16        little endian
 *   bytes 4-5    dst:16        0xFFFF = broadcast
 *   bytes 6-9    counter:32    monotonic, persistent, per device
 *   byte 10      seq:8         ARQ handle, wraps freely
 *   byte 11      flags:8
 *   bytes 12..n  payload       encrypted
 *   last 4       MIC:32
 *
 * 12-byte header + 4-byte MIC = 16 bytes of overhead. Revision 2 of CLAUDE.md gave
 * this number three different ways; see docs/decisions/0001-open-decisions.md D7
 * for which one won and why. The LoRa explicit header carries the length, so the
 * frame has no length field.
 *
 * This module does encoding and decoding only. It does not touch crypto, and it
 * deliberately cannot: an authenticated header is just the first 12 bytes handed
 * to CCM as AAD, and keeping that in link/crypto.h means the framing code has no
 * opinion about keys.
 */

#ifndef MAXL_LINK_FRAME_H
#define MAXL_LINK_FRAME_H

#include <stddef.h>
#include <stdint.h>

namespace link {

/// The wire format version this build speaks (CLAUDE.md 2.1, docs/protocol.md).
constexpr uint8_t kWireVersion = 1;

constexpr size_t kHeaderBytes = 12;
constexpr size_t kMicBytes = 4;
constexpr size_t kFrameOverheadBytes = kHeaderBytes + kMicBytes;

/// TEXT is the largest payload (CLAUDE.md 2.2: UTF-8, max 48 bytes).
constexpr size_t kMaxPayloadBytes = 48;
constexpr size_t kMaxFrameBytes = kHeaderBytes + kMaxPayloadBytes + kMicBytes;

constexpr uint16_t kBroadcastAddress = 0xFFFFu;

enum class FrameType : uint8_t {
    Beacon = 0,
    Data = 1,
    Ack = 2,
    Telemetry = 3,
    Position = 4,
    Text = 5,
    Config = 6,
};

/// flags byte, CLAUDE.md 2.1. Bits 3-7 are reserved and must be transmitted as
/// zero -- docs/protocol.md notes that additive changes go here precisely because
/// they cost no version bump.
constexpr uint8_t kFlagAckReq = 1u << 0;
constexpr uint8_t kFlagRetry = 1u << 1;
constexpr uint8_t kFlagLowBatt = 1u << 2;
constexpr uint8_t kFlagReservedMask = 0xF8u;

struct Header {
    uint8_t version;
    FrameType type;
    uint8_t netId;
    uint16_t src;
    uint16_t dst;
    uint32_t counter;
    uint8_t seq;
    uint8_t flags;
};

enum class FrameError : uint8_t {
    None = 0,
    TooShort,        ///< fewer bytes than header + MIC
    TooLong,         ///< more than kMaxFrameBytes
    VersionMismatch, ///< ver nibble is not kWireVersion
    UnknownType,     ///< type nibble is not a FrameType we know
};

/// Write a header into `out`, which must hold at least kHeaderBytes.
void encodeHeader(const Header &header, uint8_t *out);

/// Read a header from `in`, which must hold at least kHeaderBytes. Does not
/// validate; use inspect() on anything that came off the air.
Header decodeHeader(const uint8_t *in);

/*
 * Look at a frame as received, before any MIC check.
 *
 * versioning-and-updates.md 1 requires exactly this: "ver is read from the
 * cleartext header *before* the MIC is checked, for diagnostics only". A frame
 * with the wrong ver fails the MIC anyway -- the header is AAD -- and a MIC
 * failure is indistinguishable from corruption or a wrong key. Without this step
 * a half-upgraded pair shows "no peer" and you spend an evening on it.
 *
 * Nothing but the header is trusted on the strength of this. The caller records
 * VERSION_MISMATCH in peer state and discards the frame.
 */
FrameError inspect(const uint8_t *frame, size_t len, Header *out);

/// Payload length of a well-formed frame of `len` on-air bytes.
size_t payloadLength(size_t frameLen);

/// True if the type nibble names a frame type this build knows.
bool isKnownType(uint8_t rawType);

} // namespace link

#endif // MAXL_LINK_FRAME_H
