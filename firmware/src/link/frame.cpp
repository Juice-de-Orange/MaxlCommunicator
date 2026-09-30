#include "frame.h"

namespace link {
namespace {

void put16(uint8_t *out, uint16_t value)
{
    out[0] = static_cast<uint8_t>(value & 0xFFu);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

void put32(uint8_t *out, uint32_t value)
{
    out[0] = static_cast<uint8_t>(value & 0xFFu);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    out[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    out[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

uint16_t get16(const uint8_t *in)
{
    return static_cast<uint16_t>(in[0]) | static_cast<uint16_t>(static_cast<uint16_t>(in[1]) << 8);
}

uint32_t get32(const uint8_t *in)
{
    return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
           (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
}

constexpr uint8_t kHighestKnownType = static_cast<uint8_t>(FrameType::Config);

} // namespace

void encodeHeader(const Header &header, uint8_t *out)
{
    out[0] = static_cast<uint8_t>(((header.version & 0x0Fu) << 4) |
                                  (static_cast<uint8_t>(header.type) & 0x0Fu));
    out[1] = header.netId;
    put16(out + 2, header.src);
    put16(out + 4, header.dst);
    put32(out + 6, header.counter);
    out[10] = header.seq;
    // Reserved bits go out as zero. A peer that later starts using them stays
    // compatible; a peer that receives junk in them would not.
    out[11] = static_cast<uint8_t>(header.flags & ~kFlagReservedMask);
}

Header decodeHeader(const uint8_t *in)
{
    Header header{};
    header.version = static_cast<uint8_t>((in[0] >> 4) & 0x0Fu);
    header.type = static_cast<FrameType>(in[0] & 0x0Fu);
    header.netId = in[1];
    header.src = get16(in + 2);
    header.dst = get16(in + 4);
    header.counter = get32(in + 6);
    header.seq = in[10];
    header.flags = in[11];
    return header;
}

bool isKnownType(uint8_t rawType)
{
    return rawType <= kHighestKnownType;
}

FrameError inspect(const uint8_t *frame, size_t len, Header *out)
{
    if (len < kHeaderBytes + kMicBytes) {
        return FrameError::TooShort;
    }
    if (len > kMaxFrameBytes) {
        return FrameError::TooLong;
    }

    const Header header = decodeHeader(frame);
    if (out != nullptr) {
        *out = header;
    }

    /*
     * Version first. A mismatch is reported even though the type check below
     * might also fail: the version is the diagnosis the user needs, and a node
     * speaking a different ver may well use type nibbles we have never heard of.
     */
    if (header.version != kWireVersion) {
        return FrameError::VersionMismatch;
    }

    /*
     * An unknown type within the same ver is not an error in the protocol's
     * sense -- docs/protocol.md calls it "safely ignorable", which is what makes
     * additive changes cheap. It is still reported, so the caller can drop the
     * frame after the MIC check rather than guessing at its payload.
     */
    if (!isKnownType(static_cast<uint8_t>(header.type))) {
        return FrameError::UnknownType;
    }

    return FrameError::None;
}

size_t payloadLength(size_t frameLen)
{
    if (frameLen < kHeaderBytes + kMicBytes) {
        return 0;
    }
    return frameLen - kHeaderBytes - kMicBytes;
}

} // namespace link
