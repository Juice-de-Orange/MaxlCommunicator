/*
 * Frame header encoding, and the version check that has to happen before the MIC.
 */

#include "doctest.h"

#include "link/frame.h"

using namespace link;

TEST_CASE("the header is 12 bytes and the overhead is 16")
{
    // Decision D7. CLAUDE.md 2.1 gave this three different ways; this is the
    // reading that survived, and it is what the airtime table is computed from.
    CHECK(kHeaderBytes == 12);
    CHECK(kMicBytes == 4);
    CHECK(kFrameOverheadBytes == 16);
    CHECK(kMaxFrameBytes == 12 + 48 + 4);
}

TEST_CASE("a header round-trips through the wire format")
{
    Header header{};
    header.version = kWireVersion;
    header.type = FrameType::Position;
    header.netId = 0x2A;
    header.src = 0x1234;
    header.dst = 0xBEEF;
    header.counter = 0xDEADBEEFu;
    header.seq = 0x7F;
    header.flags = kFlagAckReq | kFlagLowBatt;

    uint8_t wire[kHeaderBytes];
    encodeHeader(header, wire);

    // Spot-check the layout rather than only the round trip: a symmetric bug in
    // both directions would pass a round-trip test.
    CHECK(wire[0] == 0x14);  // ver 1, type 4 (POSITION)
    CHECK(wire[1] == 0x2A);
    CHECK(wire[2] == 0x34);  // src little endian
    CHECK(wire[3] == 0x12);
    CHECK(wire[4] == 0xEF);  // dst little endian
    CHECK(wire[5] == 0xBE);
    CHECK(wire[6] == 0xEF);  // counter little endian
    CHECK(wire[7] == 0xBE);
    CHECK(wire[8] == 0xAD);
    CHECK(wire[9] == 0xDE);
    CHECK(wire[10] == 0x7F);
    CHECK(wire[11] == 0x05);

    const Header back = decodeHeader(wire);
    CHECK(back.version == header.version);
    CHECK(back.type == header.type);
    CHECK(back.netId == header.netId);
    CHECK(back.src == header.src);
    CHECK(back.dst == header.dst);
    CHECK(back.counter == header.counter);
    CHECK(back.seq == header.seq);
    CHECK(back.flags == header.flags);
}

TEST_CASE("reserved flag bits are transmitted as zero")
{
    // docs/protocol.md: bits 3-7 are held back for additive changes, which only
    // works if today's firmware does not put junk in them.
    Header header{};
    header.version = kWireVersion;
    header.type = FrameType::Data;
    header.flags = 0xFF;

    uint8_t wire[kHeaderBytes];
    encodeHeader(header, wire);
    CHECK((wire[11] & kFlagReservedMask) == 0);
    CHECK(wire[11] == 0x07);
}

TEST_CASE("a version mismatch is diagnosable, not just a MIC failure")
{
    /*
     * versioning-and-updates.md 1: the header is AAD, so a frame with a different
     * ver fails the MIC, and a MIC failure is indistinguishable from corruption
     * or a wrong key. Without reading ver from the cleartext first, a
     * half-upgraded pair shows "no peer" and you lose an evening.
     */
    Header header{};
    header.version = kWireVersion + 1;
    header.type = FrameType::Beacon;
    header.src = 0x0002;

    uint8_t frame[kHeaderBytes + kMicBytes] = {};
    encodeHeader(header, frame);

    Header seen{};
    CHECK(inspect(frame, sizeof(frame), &seen) == FrameError::VersionMismatch);
    // The observed version is available, which is what the PEERS screen shows
    // next to the node's own.
    CHECK(seen.version == kWireVersion + 1);
    CHECK(seen.src == 0x0002);
}

TEST_CASE("an unknown type within the same version is reported, not guessed at")
{
    // docs/protocol.md calls unknown frame types "safely ignorable", which is
    // what makes additive changes cost nothing.
    Header header{};
    header.version = kWireVersion;
    header.type = static_cast<FrameType>(0x0F);

    uint8_t frame[kHeaderBytes + kMicBytes] = {};
    encodeHeader(header, frame);
    CHECK(inspect(frame, sizeof(frame), nullptr) == FrameError::UnknownType);

    for (uint8_t raw = 0; raw <= 6; ++raw) {
        CHECK(isKnownType(raw));
    }
    for (uint8_t raw = 7; raw <= 15; ++raw) {
        CHECK_FALSE(isKnownType(raw));
    }
}

TEST_CASE("frames outside the length bounds are rejected")
{
    uint8_t frame[kMaxFrameBytes + 4] = {};
    Header header{};
    header.version = kWireVersion;
    header.type = FrameType::Text;
    encodeHeader(header, frame);

    // Header plus MIC is the shortest legal frame -- an empty payload.
    CHECK(inspect(frame, kHeaderBytes + kMicBytes, nullptr) == FrameError::None);
    CHECK(inspect(frame, kHeaderBytes + kMicBytes - 1, nullptr) == FrameError::TooShort);
    CHECK(inspect(frame, kMaxFrameBytes, nullptr) == FrameError::None);
    CHECK(inspect(frame, kMaxFrameBytes + 1, nullptr) == FrameError::TooLong);
}

TEST_CASE("payload length is the frame minus header and MIC")
{
    CHECK(payloadLength(kHeaderBytes + kMicBytes) == 0);
    CHECK(payloadLength(28) == 12);   // POSITION
    CHECK(payloadLength(30) == 14);   // TELEMETRY
    CHECK(payloadLength(20) == 4);    // ACK
    CHECK(payloadLength(64) == 48);   // TEXT at its maximum
    CHECK(payloadLength(3) == 0);     // nonsense in, zero out
}

TEST_CASE("the broadcast address is what CLAUDE.md 2.1 says")
{
    CHECK(kBroadcastAddress == 0xFFFF);
}
