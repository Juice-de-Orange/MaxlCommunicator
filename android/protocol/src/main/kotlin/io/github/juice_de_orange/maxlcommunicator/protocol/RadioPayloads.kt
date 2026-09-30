package io.github.juice_de_orange.maxlcommunicator.protocol

/**
 * The radio payloads that travel inside EVT_FRAME_RX.
 *
 * CLAUDE.md 2.2, fixed-width binary, little endian. The phone never sees a LoRa
 * frame -- docs/bridge-protocol.md section 6: "The phone never gets raw LoRa
 * frames in real time; it gets journal events." What it does get is the payload
 * of one, and these are its shapes.
 *
 * The scaling is not decoration. Temperature is hundredths of a degree because a
 * float on the air would cost four bytes to say less; latitude is degrees times
 * ten million because a float holds about seven significant digits and a
 * coordinate to a metre needs nine.
 */

/** CLAUDE.md 2.2 frame types. */
object FrameType {
    const val BEACON = 0
    const val DATA = 1
    const val ACK = 2
    const val TELEMETRY = 3
    const val POSITION = 4
    const val TEXT = 5
    const val CONFIG = 6
}

/** `lat i32 (deg x 1e7), lon i32, alt i16 (m), hdop u8, fixAge u8` -- 12 bytes. */
data class PositionPayload(
    val latitudeE7: Int,
    val longitudeE7: Int,
    val altitudeM: Int,
    /** Tenths, 255 = unusable or unknown. Decision D11. */
    val hdopTenths: Int,
    val fixAgeS: Int,
) {
    val latitude: Double get() = latitudeE7 / 1e7
    val longitude: Double get() = longitudeE7 / 1e7

    /**
     * Decision D11 again, and exposed here as well as on [FixEvent] because it
     * is the same field with the same meaning arriving by a different route --
     * one over the air, one from the node's own GNSS.
     *
     * 255 means both "25.5 or worse" and "not known". The important half is that
     * an unknown value reads as the WORST one: at 0 a field nobody filled in
     * would render as a perfect fix.
     */
    val hdop: Double get() = hdopTenths / 10.0
    val hdopKnown: Boolean get() = hdopTenths != 255

    fun encode(): ByteArray = ByteWriter()
        .i32(latitudeE7).i32(longitudeE7).u16(altitudeM and 0xFFFF)
        .u8(hdopTenths).u8(fixAgeS).finish()

    companion object {
        const val BYTES = 12

        fun decode(body: ByteArray): PositionPayload {
            val r = ByteReader(body)
            r.require(BYTES)
            return PositionPayload(r.i32(), r.i32(), r.i16(), r.u8(), r.u8())
        }
    }
}

/**
 * `tempC i16 (0.01 C), humidity u16 (0.01 %), pressure u32 (Pa), battery u16 (mV),
 * uptime u32 (s)` -- 14 bytes.
 */
data class TelemetryPayload(
    val temperatureCentiC: Int,
    val humidityCentiPercent: Int,
    val pressurePa: Long,
    val batteryMv: Int,
    val uptimeS: Long,
) {
    val temperatureC: Double get() = temperatureCentiC / 100.0
    val humidityPercent: Double get() = humidityCentiPercent / 100.0
    val pressureHpa: Double get() = pressurePa / 100.0

    fun encode(): ByteArray = ByteWriter()
        .u16(temperatureCentiC and 0xFFFF).u16(humidityCentiPercent)
        .u32(pressurePa).u16(batteryMv).u32(uptimeS).finish()

    companion object {
        const val BYTES = 14

        fun decode(body: ByteArray): TelemetryPayload {
            val r = ByteReader(body)
            r.require(BYTES)
            return TelemetryPayload(r.i16(), r.u16(), r.u32(), r.u16(), r.u32())
        }
    }
}

/** `seq u8 of the acknowledged frame, rssi i16, snr i8` -- 4 bytes. */
data class AckPayload(val seq: Int, val rssiDbm: Int, val snrDb: Int) {
    fun encode(): ByteArray = ByteWriter().u8(seq).u16(rssiDbm and 0xFFFF).u8(snrDb and 0xFF).finish()

    companion object {
        const val BYTES = 4

        fun decode(body: ByteArray): AckPayload {
            val r = ByteReader(body)
            r.require(BYTES)
            return AckPayload(r.u8(), r.i16(), r.i8())
        }
    }
}

/**
 * `UTF-8, max 48 bytes, no null terminator`.
 *
 * Decoded leniently: a text frame whose bytes are not valid UTF-8 arrived from
 * the air, and dropping the whole journal event over it would lose the RSSI and
 * the sender too. Kotlin's String constructor substitutes replacement characters,
 * which is the right amount of complaining.
 */
object TextPayload {
    const val MAX_BYTES = 48

    fun decode(body: ByteArray): String = String(body, Charsets.UTF_8)

    fun encode(text: String): ByteArray {
        val utf8 = text.toByteArray(Charsets.UTF_8)
        if (utf8.size > MAX_BYTES) {
            throw BridgeFormatException("text is ${utf8.size} bytes, limit is $MAX_BYTES")
        }
        return utf8
    }
}
