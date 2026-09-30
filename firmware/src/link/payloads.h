/*
 * Frame payload encodings (CLAUDE.md 2.2).
 *
 * "Fixed-width binary, little endian." No JSON, no Protobuf, no text protocol on
 * the air interface (1.4) -- every byte here is a byte of airtime, and airtime is
 * the resource the whole design is built around.
 *
 *   POSITION   lat i32 (deg x 1e7), lon i32, alt i16 (m), hdop u8, fixAge u8   12 B
 *   TELEMETRY  tempC i16 (0.01 C), humidity u16 (0.01 %), pressure u32 (Pa),
 *              battery u16 (mV), uptime u32 (s)                                14 B
 *   TEXT       UTF-8, max 48 bytes, no null terminator                       <= 48 B
 *   ACK        seq u8 of the acknowledged frame, rssi i16, snr i8 (dB)          4 B
 *
 * The same encodings appear on the phone side: docs/bridge-protocol.md 4 hands
 * EVT_FRAME_RX the radio payload verbatim, so the PWA parses exactly these bytes.
 * That is why test-vectors/radio_payloads.json is shared between the two
 * implementations rather than each side having its own idea of the layout.
 */

#ifndef MAXL_LINK_PAYLOADS_H
#define MAXL_LINK_PAYLOADS_H

#include <stddef.h>
#include <stdint.h>

namespace link {

constexpr size_t kPositionPayloadBytes = 12;
constexpr size_t kTelemetryPayloadBytes = 14;
constexpr size_t kAckPayloadBytes = 4;
constexpr size_t kMaxTextBytes = 48;

struct PositionPayload {
    int32_t latitudeE7;
    int32_t longitudeE7;
    int16_t altitudeM;
    uint8_t hdop;
    uint8_t fixAgeS;
};

struct TelemetryPayload {
    int16_t temperatureCentiC;
    uint16_t humidityCentiPercent;
    uint32_t pressurePa;
    uint16_t batteryMv;
    uint32_t uptimeS;
};

struct AckPayload {
    uint8_t seq;
    int16_t rssiDbm;
    int8_t snrDb;
};

void encodePosition(const PositionPayload &in, uint8_t *out);
bool decodePosition(const uint8_t *in, size_t len, PositionPayload *out);

void encodeTelemetry(const TelemetryPayload &in, uint8_t *out);
bool decodeTelemetry(const uint8_t *in, size_t len, TelemetryPayload *out);

void encodeAck(const AckPayload &in, uint8_t *out);
bool decodeAck(const uint8_t *in, size_t len, AckPayload *out);

/*
 * TEXT is copied, not interpreted. The device does not validate UTF-8: a node
 * that drops a message because a byte sequence offended it is worse than one that
 * forwards it and lets the phone render a replacement character. Length is the
 * only thing enforced.
 *
 * Returns the number of bytes written, or 0 if the input does not fit.
 */
size_t encodeText(const uint8_t *utf8, size_t len, uint8_t *out);

} // namespace link

#endif // MAXL_LINK_PAYLOADS_H
