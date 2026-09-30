#include "payloads.h"

namespace link {
namespace {

void put16(uint8_t *out, uint16_t v)
{
    out[0] = static_cast<uint8_t>(v & 0xFFu);
    out[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

void put32(uint8_t *out, uint32_t v)
{
    out[0] = static_cast<uint8_t>(v & 0xFFu);
    out[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    out[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    out[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

uint16_t get16(const uint8_t *in)
{
    return static_cast<uint16_t>(static_cast<uint16_t>(in[0]) |
                                 static_cast<uint16_t>(static_cast<uint16_t>(in[1]) << 8));
}

uint32_t get32(const uint8_t *in)
{
    return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
           (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
}

} // namespace

void encodePosition(const PositionPayload &in, uint8_t *out)
{
    put32(out + 0, static_cast<uint32_t>(in.latitudeE7));
    put32(out + 4, static_cast<uint32_t>(in.longitudeE7));
    put16(out + 8, static_cast<uint16_t>(in.altitudeM));
    out[10] = in.hdop;
    out[11] = in.fixAgeS;
}

bool decodePosition(const uint8_t *in, size_t len, PositionPayload *out)
{
    if (len != kPositionPayloadBytes || out == nullptr) {
        return false;
    }
    out->latitudeE7 = static_cast<int32_t>(get32(in + 0));
    out->longitudeE7 = static_cast<int32_t>(get32(in + 4));
    out->altitudeM = static_cast<int16_t>(get16(in + 8));
    out->hdop = in[10];
    out->fixAgeS = in[11];
    return true;
}

void encodeTelemetry(const TelemetryPayload &in, uint8_t *out)
{
    put16(out + 0, static_cast<uint16_t>(in.temperatureCentiC));
    put16(out + 2, in.humidityCentiPercent);
    put32(out + 4, in.pressurePa);
    put16(out + 8, in.batteryMv);
    put32(out + 10, in.uptimeS);
}

bool decodeTelemetry(const uint8_t *in, size_t len, TelemetryPayload *out)
{
    if (len != kTelemetryPayloadBytes || out == nullptr) {
        return false;
    }
    out->temperatureCentiC = static_cast<int16_t>(get16(in + 0));
    out->humidityCentiPercent = get16(in + 2);
    out->pressurePa = get32(in + 4);
    out->batteryMv = get16(in + 8);
    out->uptimeS = get32(in + 10);
    return true;
}

void encodeAck(const AckPayload &in, uint8_t *out)
{
    out[0] = in.seq;
    put16(out + 1, static_cast<uint16_t>(in.rssiDbm));
    out[3] = static_cast<uint8_t>(in.snrDb);
}

bool decodeAck(const uint8_t *in, size_t len, AckPayload *out)
{
    if (len != kAckPayloadBytes || out == nullptr) {
        return false;
    }
    out->seq = in[0];
    out->rssiDbm = static_cast<int16_t>(get16(in + 1));
    out->snrDb = static_cast<int8_t>(in[3]);
    return true;
}

size_t encodeText(const uint8_t *utf8, size_t len, uint8_t *out)
{
    if (len > kMaxTextBytes || (len > 0 && utf8 == nullptr)) {
        return 0;
    }
    for (size_t i = 0; i < len; ++i) {
        out[i] = utf8[i];
    }
    return len;
}

} // namespace link
