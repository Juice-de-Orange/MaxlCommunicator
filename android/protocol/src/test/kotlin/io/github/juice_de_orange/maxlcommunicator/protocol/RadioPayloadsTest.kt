package io.github.juice_de_orange.maxlcommunicator.protocol

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/**
 * CLAUDE.md 2.2, against the shared radio payload vectors.
 *
 * Each case builds its own struct from the field values written in the JSON and
 * checks that encoding it yields the shared bytes -- rather than decoding the
 * bytes and checking they decode. If this side has misread a field width or an
 * endianness, its encoding does not match, which is the whole point of the
 * exercise.
 */
class RadioPayloadsTest {

    @Test
    fun `an ordinary fix encodes to the shared bytes`() {
        val position = PositionPayload(
            latitudeE7 = 472692000,
            longitudeE7 = 114041000,
            altitudeM = 574,
            hdopTenths = 12,
            fixAgeS = 3,
        )
        assertEquals(hex(vector("position_innsbruck")), hex(position.encode()))
        assertEquals(position, PositionPayload.decode(vector("position_innsbruck")))
    }

    @Test
    fun `three sign conversions at once`() {
        // The note on this vector says it plainly: southern and western
        // hemisphere plus altitude below sea level is where a hand-rolled codec
        // goes wrong.
        val position = PositionPayload(
            latitudeE7 = -338688000,
            longitudeE7 = -1732090000,
            altitudeM = -15,
            hdopTenths = 31,
            fixAgeS = 250,
        )
        assertEquals(hex(vector("position_southern_negative")), hex(position.encode()))

        val decoded = PositionPayload.decode(vector("position_southern_negative"))
        assertEquals(-338688000, decoded.latitudeE7)
        assertEquals(-1732090000, decoded.longitudeE7)
        assertEquals(-15, decoded.altitudeM)
        assertTrue(decoded.latitude < 0 && decoded.longitude < 0)
    }

    @Test
    fun `the all-zero frame round-trips like any other`() {
        val zero = PositionPayload(0, 0, 0, 0, 0)
        assertEquals(hex(vector("position_zero")), hex(zero.encode()))
        assertEquals(zero, PositionPayload.decode(vector("position_zero")))
    }

    @Test
    fun `hdop is tenths, and 255 means it is not known`() {
        // Decision D11. The value at 255 must not read as a perfect fix.
        assertEquals(1.2, PositionPayload.decode(vector("position_innsbruck")).hdop)
        assertTrue(PositionPayload.decode(vector("position_innsbruck")).hdopKnown)

        val unknown = PositionPayload(0, 0, 0, 255, 0)
        assertEquals(25.5, unknown.hdop)
        assertFalse(unknown.hdopKnown)
    }

    @Test
    fun `telemetry encodes to the shared bytes`() {
        val telemetry = TelemetryPayload(
            temperatureCentiC = 2135,
            humidityCentiPercent = 4820,
            pressurePa = 95230,
            batteryMv = 3987,
            uptimeS = 86400,
        )
        assertEquals(hex(vector("telemetry_typical")), hex(telemetry.encode()))

        val decoded = TelemetryPayload.decode(vector("telemetry_typical"))
        assertEquals(21.35, decoded.temperatureC)
        assertEquals(48.20, decoded.humidityPercent)
        assertEquals(952.30, decoded.pressureHpa)
    }

    @Test
    fun `freezing exercises the only signed field, and uptime at its maximum`() {
        val telemetry = TelemetryPayload(
            temperatureCentiC = -1275,
            humidityCentiPercent = 9155,
            pressurePa = 101325,
            batteryMv = 3312,
            uptimeS = 4294967295L,
        )
        assertEquals(hex(vector("telemetry_freezing")), hex(telemetry.encode()))

        val decoded = TelemetryPayload.decode(vector("telemetry_freezing"))
        assertEquals(-12.75, decoded.temperatureC)
        // A u32 at its maximum must not come back negative, which is what it
        // would do if it were read into an Int.
        assertEquals(4294967295L, decoded.uptimeS)
    }

    @Test
    fun `an ACK carries the link quality the sender needs`() {
        val ack = AckPayload(seq = 42, rssiDbm = -97, snrDb = 7)
        assertEquals(hex(vector("ack_typical")), hex(ack.encode()))
        assertEquals(ack, AckPayload.decode(vector("ack_typical")))
    }

    @Test
    fun `a weak link is negative in both fields and seq is at its maximum`() {
        val ack = AckPayload(seq = 255, rssiDbm = -128, snrDb = -13)
        assertEquals(hex(vector("ack_weak_link")), hex(ack.encode()))

        val decoded = AckPayload.decode(vector("ack_weak_link"))
        assertEquals(255, decoded.seq)
        assertEquals(-128, decoded.rssiDbm)
        assertEquals(-13, decoded.snrDb)
    }

    @Test
    fun `text is UTF-8 and counted in bytes`() {
        assertEquals("Hallo Welt", TextPayload.decode(vector("text_ascii")))

        val umlaut = vector("text_utf8_umlaut")
        assertEquals("Grüße vom Berg", TextPayload.decode(umlaut))
        // 14 characters, 16 bytes. CLAUDE.md 2.2's limit is the second number.
        assertEquals(14, TextPayload.decode(umlaut).length)
        assertEquals(16, umlaut.size)
    }

    @Test
    fun `exactly forty-eight bytes is allowed and forty-nine is not`() {
        val atTheLimit = vector("text_max_48")
        assertEquals(48, atTheLimit.size)
        assertEquals(hex(atTheLimit), hex(TextPayload.encode(TextPayload.decode(atTheLimit))))

        assertFailsWith<BridgeFormatException> { TextPayload.encode("A".repeat(49)) }
    }

    @Test
    fun `every payload refuses a body that is too short`() {
        assertFailsWith<BridgeFormatException> { PositionPayload.decode(ByteArray(11)) }
        assertFailsWith<BridgeFormatException> { TelemetryPayload.decode(ByteArray(13)) }
        assertFailsWith<BridgeFormatException> { AckPayload.decode(ByteArray(3)) }
    }
}
