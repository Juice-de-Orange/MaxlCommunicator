/*
 * Payload encodings, checked against the shared test vectors.
 *
 * The vectors in test-vectors/radio_payloads.json are also read by the
 * TypeScript bridge. Neither implementation generated them -- they were written
 * from CLAUDE.md 2.2 -- so agreeing with them means agreeing with each other.
 *
 * Each case builds its own struct from the field values in the JSON. That is
 * deliberate: if this side has misread a field width or an endianness, its
 * encoding will not match, which is the whole point of the exercise.
 */

#include <cstring>

#include "doctest.h"

#include "link/payloads.h"
#include "test_vectors.h"

using namespace link;

namespace {

void checkAgainstVector(const char *name, const uint8_t *encoded, size_t len)
{
    const vectors::Vector *vector = vectors::find(name);
    CAPTURE(name);
    // A missing vector must fail loudly. A test that silently skips is a test
    // that passes for the wrong reason.
    REQUIRE(vector != nullptr);
    REQUIRE(vector->length == len);
    CHECK(std::memcmp(encoded, vector->bytes, len) == 0);
}

} // namespace

TEST_CASE("POSITION matches the shared vectors")
{
    uint8_t out[kPositionPayloadBytes];

    PositionPayload innsbruck{472692000, 114041000, 574, 12, 3};
    encodePosition(innsbruck, out);
    checkAgainstVector("position_innsbruck", out, sizeof(out));

    // Three separate sign conversions in one frame, which is where a hand-rolled
    // little-endian codec goes wrong.
    PositionPayload southern{-338688000, -1732090000, -15, 31, 250};
    encodePosition(southern, out);
    checkAgainstVector("position_southern_negative", out, sizeof(out));

    PositionPayload zero{0, 0, 0, 0, 0};
    encodePosition(zero, out);
    checkAgainstVector("position_zero", out, sizeof(out));
}

TEST_CASE("TELEMETRY matches the shared vectors")
{
    uint8_t out[kTelemetryPayloadBytes];

    TelemetryPayload typical{2135, 4820, 95230, 3987, 86400};
    encodeTelemetry(typical, out);
    checkAgainstVector("telemetry_typical", out, sizeof(out));

    // -12.75 C is the only signed field; uptime at u32 max is the widest.
    TelemetryPayload freezing{-1275, 9155, 101325, 3312, 4294967295u};
    encodeTelemetry(freezing, out);
    checkAgainstVector("telemetry_freezing", out, sizeof(out));
}

TEST_CASE("ACK matches the shared vectors")
{
    uint8_t out[kAckPayloadBytes];

    AckPayload typical{42, -97, 7};
    encodeAck(typical, out);
    checkAgainstVector("ack_typical", out, sizeof(out));

    AckPayload weak{255, -128, -13};
    encodeAck(weak, out);
    checkAgainstVector("ack_weak_link", out, sizeof(out));
}

TEST_CASE("TEXT matches the shared vectors and is limited by bytes, not characters")
{
    const vectors::Vector *umlaut = vectors::find("text_utf8_umlaut");
    REQUIRE(umlaut != nullptr);
    // "Gruesse vom Berg" with real umlauts: 14 characters, 16 bytes. CLAUDE.md
    // 2.2's limit is bytes, so a client that truncates by character overruns it.
    CHECK(umlaut->length == 16);

    uint8_t out[kMaxTextBytes];
    CHECK(encodeText(umlaut->bytes, umlaut->length, out) == umlaut->length);
    CHECK(std::memcmp(out, umlaut->bytes, umlaut->length) == 0);

    const vectors::Vector *maxText = vectors::find("text_max_48");
    REQUIRE(maxText != nullptr);
    CHECK(maxText->length == kMaxTextBytes);
    CHECK(encodeText(maxText->bytes, maxText->length, out) == kMaxTextBytes);

    // One byte over is refused rather than truncated -- a silently shortened
    // message is worse than a rejected one.
    uint8_t tooLong[kMaxTextBytes + 1] = {};
    CHECK(encodeText(tooLong, sizeof(tooLong), out) == 0);
}

TEST_CASE("every payload decodes back to what it was")
{
    uint8_t out[kTelemetryPayloadBytes];

    PositionPayload position{-338688000, -1732090000, -15, 31, 250};
    encodePosition(position, out);
    PositionPayload decodedPosition{};
    REQUIRE(decodePosition(out, kPositionPayloadBytes, &decodedPosition));
    CHECK(decodedPosition.latitudeE7 == position.latitudeE7);
    CHECK(decodedPosition.longitudeE7 == position.longitudeE7);
    CHECK(decodedPosition.altitudeM == position.altitudeM);
    CHECK(decodedPosition.hdop == position.hdop);
    CHECK(decodedPosition.fixAgeS == position.fixAgeS);

    TelemetryPayload telemetry{-1275, 9155, 101325, 3312, 4294967295u};
    encodeTelemetry(telemetry, out);
    TelemetryPayload decodedTelemetry{};
    REQUIRE(decodeTelemetry(out, kTelemetryPayloadBytes, &decodedTelemetry));
    CHECK(decodedTelemetry.temperatureCentiC == telemetry.temperatureCentiC);
    CHECK(decodedTelemetry.uptimeS == telemetry.uptimeS);

    AckPayload ack{255, -128, -13};
    encodeAck(ack, out);
    AckPayload decodedAck{};
    REQUIRE(decodeAck(out, kAckPayloadBytes, &decodedAck));
    CHECK(decodedAck.seq == ack.seq);
    CHECK(decodedAck.rssiDbm == ack.rssiDbm);
    CHECK(decodedAck.snrDb == ack.snrDb);
}

TEST_CASE("a payload of the wrong length is refused")
{
    uint8_t buffer[kTelemetryPayloadBytes] = {};
    PositionPayload position{};
    CHECK_FALSE(decodePosition(buffer, kPositionPayloadBytes - 1, &position));
    CHECK_FALSE(decodePosition(buffer, kPositionPayloadBytes + 1, &position));
    CHECK_FALSE(decodePosition(buffer, kPositionPayloadBytes, nullptr));
}
