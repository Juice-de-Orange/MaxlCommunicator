package io.github.juice_de_orange.maxlcommunicator.protocol

/**
 * Command and response bodies, docs/bridge-protocol.md sections 3 and 4.
 *
 * The response bodies were missing from revision 1 of that document for every
 * single command, which made its own claim -- that a client can be written
 * against it without reading the TypeScript -- false. They were added on
 * 2026-08-31 while writing this file, which is the sort of thing writing a second
 * implementation is for.
 */

/** GET_INFO's response. Seven bytes. Section 5 step 2 checks it before anything else. */
data class DeviceInfo(
    val bridgeProtocol: Int,
    val nodeId: Int,
    val firmwareMajor: Int,
    val firmwareMinor: Int,
    val firmwarePatch: Int,
    val wireVersion: Int,
) {
    val firmwareVersion: String get() = "$firmwareMajor.$firmwareMinor.$firmwarePatch"

    /**
     * Whether this client can talk to that node at all.
     *
     * See docs/versioning-and-updates.md. A mismatch is not something to work
     * around: the framing itself may differ.
     */
    val isCompatible: Boolean get() = bridgeProtocol == Gatt.BRIDGE_PROTOCOL_VERSION

    companion object {
        const val BYTES = 7

        fun decode(body: ByteArray): DeviceInfo {
            val r = ByteReader(body)
            r.require(BYTES)
            return DeviceInfo(
                bridgeProtocol = r.u8(),
                nodeId = r.u16(),
                firmwareMajor = r.u8(),
                firmwareMinor = r.u8(),
                firmwarePatch = r.u8(),
                wireVersion = r.u8(),
            )
        }
    }
}

/**
 * The 17-byte status body, section 4.
 *
 * Returned by GET_STATUS and carried unchanged by EVT_STATUS. Also what a
 * generic BLE tool reads cold off the STATUS characteristic, which is why the
 * budget is repeated here rather than only in EVT_BUDGET.
 */
data class StatusBody(
    val batteryMv: Int,
    val uptimeS: Long,
    val queueDepth: Int,
    val band: Int,
    val budgetUsedMs: Long,
    val budgetLimitMs: Long,
    val flags: Int,
) {
    /**
     * "bit 0 is not cosmetic either. A node with no valid time is
     * transmit-blocked, and a UI that cannot say so shows a device that simply
     * refuses to send with no explanation."
     */
    val timeValid: Boolean get() = (flags and 0x01) != 0
    val keyProvisioned: Boolean get() = (flags and 0x02) != 0
    val gnssPowered: Boolean get() = (flags and 0x04) != 0

    /** 0 = g3 (the 10 % working band), 1 = g1 (the 1 % fallback). CLAUDE.md 1.3. */
    val bandName: String get() = if (band == 0) "g3" else "g1"

    val budgetRemainingMs: Long get() = (budgetLimitMs - budgetUsedMs).coerceAtLeast(0)

    companion object {
        const val BYTES = 17

        fun decode(body: ByteArray): StatusBody {
            val r = ByteReader(body)
            r.require(BYTES)
            return StatusBody(
                batteryMv = r.u16(),
                uptimeS = r.u32(),
                queueDepth = r.u8(),
                band = r.u8(),
                budgetUsedMs = r.u32(),
                budgetLimitMs = r.u32(),
                flags = r.u8(),
            )
        }
    }
}

/** GET_BUDGET's response, and EVT_BUDGET's body. Thirteen bytes. */
data class BudgetBody(
    val band: Int,
    val usedMs: Long,
    val limitMs: Long,
    /** Zero means "now" -- a real answer rather than a missing one. */
    val nextTxUnix: Long,
) {
    val remainingMs: Long get() = (limitMs - usedMs).coerceAtLeast(0)

    companion object {
        const val BYTES = 13

        fun decode(body: ByteArray): BudgetBody {
            val r = ByteReader(body)
            r.require(BYTES)
            return BudgetBody(band = r.u8(), usedMs = r.u32(), limitMs = r.u32(), nextTxUnix = r.u32())
        }
    }
}

/**
 * EVT_JOURNAL, section 4 -- one journal entry and the counter that identifies it.
 *
 * `opcode` is the WRAPPED event's code, not JOURNAL, and `body` is that event's
 * own body byte for byte, so it goes straight to the decoder it already had.
 *
 * The wrapper exists because the counter existed nowhere else: it was computed on
 * the device as an index into its own storage and thrown away before
 * transmission, so no client could form ACK_QUEUE at all. That is decision D15,
 * and it is why BRIDGE_PROTO is 2.
 */
data class JournalEntry(
    val counter: Long,
    val opcode: Int,
    val body: ByteArray,
) {
    fun encode(): ByteArray {
        require(body.size <= 0xFF) { "journal body of ${body.size} bytes exceeds the len:u8 field" }
        return ByteWriter().u32(counter).u8(opcode).u8(body.size).bytes(body).finish()
    }

    override fun equals(other: Any?): Boolean =
        other is JournalEntry && counter == other.counter && opcode == other.opcode &&
            body.contentEquals(other.body)

    override fun hashCode(): Int =
        (counter.hashCode() * 31 + opcode) * 31 + body.contentHashCode()

    companion object {
        /**
         * Throws on a truncated entry rather than returning something plausible:
         * a length field that disagrees with the body is a framing error, and
         * storing a counter against bytes that are not what the device sent is
         * worse than failing.
         */
        fun decode(body: ByteArray): JournalEntry {
            val r = ByteReader(body)
            val counter = r.u32()
            val opcode = r.u8()
            val length = r.u8()
            return JournalEntry(counter, opcode, r.bytes(length))
        }
    }
}

/** EVT_FRAME_RX, section 4. */
data class FrameRx(
    val counter: Long,
    val src: Int,
    val frameType: Int,
    val rssiDbm: Int,
    val snrDb: Int,
    val payload: ByteArray,
) {
    override fun equals(other: Any?): Boolean =
        other is FrameRx && counter == other.counter && src == other.src &&
            frameType == other.frameType && rssiDbm == other.rssiDbm && snrDb == other.snrDb &&
            payload.contentEquals(other.payload)

    override fun hashCode(): Int =
        (((counter.hashCode() * 31 + src) * 31 + frameType) * 31 + rssiDbm) * 31 +
            payload.contentHashCode()

    companion object {
        const val HEADER_BYTES = 11

        fun decode(body: ByteArray): FrameRx {
            val r = ByteReader(body)
            r.require(HEADER_BYTES)
            val counter = r.u32()
            val src = r.u16()
            val type = r.u8()
            val rssi = r.i16()
            val snr = r.i8()
            val len = r.u8()
            return FrameRx(counter, src, type, rssi, snr, r.bytes(len))
        }
    }
}

/**
 * EVT_FRAME_TX_RESULT, section 4.
 *
 * "State 2 may be followed later by 0 or 1 for the same counter -- the phone and
 * the server must treat the journal as a log of state transitions, not as a set
 * of final outcomes."
 */
data class FrameTxResult(
    val counter: Long,
    val dst: Int,
    val seq: Int,
    val result: Int,
    val attempts: Int,
    val rssiDbm: Int,
    val snrDb: Int,
) {
    val isDelivered: Boolean get() = result == DELIVERED
    val isTerminal: Boolean get() = result != QUEUED

    companion object {
        /** counter 4 + dst 2 + seq 1 + result 1 + attempts 1 + rssi 2 + snr 1. */
        const val BYTES = 12

        const val DELIVERED = 0
        const val UNDELIVERED = 1
        const val QUEUED = 2
        const val DROPPED_BY_USER = 3

        fun decode(body: ByteArray): FrameTxResult {
            val r = ByteReader(body)
            r.require(BYTES)
            return FrameTxResult(
                counter = r.u32(),
                dst = r.u16(),
                seq = r.u8(),
                result = r.u8(),
                attempts = r.u8(),
                rssiDbm = r.i16(),
                snrDb = r.i8(),
            )
        }
    }
}

/** EVT_FIX, section 4. `hdop` is in tenths; 255 means unusable or unknown (D11). */
data class FixEvent(
    val latitudeE7: Int,
    val longitudeE7: Int,
    val altitudeM: Int,
    val hdopTenths: Int,
    val fixAgeS: Int,
    val satellites: Int,
) {
    val hdop: Double get() = hdopTenths / 10.0
    val hdopKnown: Boolean get() = hdopTenths != 255

    companion object {
        const val BYTES = 13

        fun decode(body: ByteArray): FixEvent {
            val r = ByteReader(body)
            r.require(BYTES)
            return FixEvent(
                latitudeE7 = r.i32(),
                longitudeE7 = r.i32(),
                altitudeM = r.i16(),
                hdopTenths = r.u8(),
                fixAgeS = r.u8(),
                satellites = r.u8(),
            )
        }
    }
}

/** Encoders for the command bodies of section 3. */
object Commands {
    /** `dst:u16, len:u8, utf8[len]` with len <= 48. */
    fun sendText(dst: Int, text: String): ByteArray {
        val utf8 = text.toByteArray(Charsets.UTF_8)
        if (utf8.size > MAX_TEXT_BYTES) {
            // Bytes, not characters: CLAUDE.md 2.2 caps the TEXT payload at 48
            // bytes of UTF-8, and one umlaut is two of them.
            throw BridgeFormatException("text is ${utf8.size} bytes, limit is $MAX_TEXT_BYTES")
        }
        return ByteWriter().u16(dst).u8(utf8.size).bytes(utf8).finish()
    }

    /** `sinceCounter:u32, maxEvents:u8`. The device caps maxEvents at 32. */
    fun getQueue(sinceCounter: Long, maxEvents: Int): ByteArray =
        ByteWriter().u32(sinceCounter).u8(maxEvents.coerceIn(1, MAX_QUEUE_BATCH)).finish()

    /** `upToCounter:u32`. Sent only after the server has the events. */
    fun ackQueue(upToCounter: Long): ByteArray = ByteWriter().u32(upToCounter).finish()

    /** `unixSeconds:u32`. One of the two ways out of a transmit-blocked node. */
    fun setTime(unixSeconds: Long): ByteArray = ByteWriter().u32(unixSeconds).finish()

    /** `timeoutSeconds:u16`. */
    fun requestFix(timeoutSeconds: Int): ByteArray = ByteWriter().u16(timeoutSeconds).finish()

    /** `configVersion:u32, tlv[]`. The version is the phone's monotonic counter. */
    fun setConfig(configVersion: Long, tlvs: List<ConfigTlv>): ByteArray {
        val w = ByteWriter().u32(configVersion)
        tlvs.forEach { w.bytes(it.encode()) }
        return w.finish()
    }

    /** `slot:u8, netId:u8, key[16]`. Bonded tier, and there is no read. */
    fun provisionKey(slot: Int, netId: Int, key: ByteArray): ByteArray {
        if (key.size != KEY_BYTES) {
            throw BridgeFormatException("key is ${key.size} bytes, must be $KEY_BYTES")
        }
        return ByteWriter().u8(slot).u8(netId).bytes(key).finish()
    }

    fun rotateKey(newSlot: Int): ByteArray = ByteWriter().u8(newSlot).finish()

    /** `magic:u32 = 0x5245534D`. Spelled so a stray write cannot mean it. */
    fun factoryReset(): ByteArray = ByteWriter().u32(FACTORY_RESET_MAGIC).finish()

    /** `dst:u16, count:u8, sf:u8 (0 = current)`. Spends real airtime. */
    fun linkTest(dst: Int, count: Int, spreadingFactor: Int = 0): ByteArray =
        ByteWriter().u16(dst).u8(count).u8(spreadingFactor).finish()

    const val MAX_TEXT_BYTES = 48
    const val MAX_QUEUE_BATCH = 32
    const val KEY_BYTES = 16
    const val FACTORY_RESET_MAGIC = 0x5245534DL
}

/**
 * A config TLV, section 3. `type:u8, len:u8, value[len]`.
 *
 * "Unknown types are ignored and reported back in EVT_CONFIG_APPLIED as
 * unapplied, rather than failing the whole write."
 */
data class ConfigTlv(val type: Int, val value: ByteArray) {
    fun encode(): ByteArray = ByteWriter().u8(type).u8(value.size).bytes(value).finish()

    override fun equals(other: Any?): Boolean =
        other is ConfigTlv && type == other.type && value.contentEquals(other.value)

    override fun hashCode(): Int = 31 * type + value.contentHashCode()

    companion object {
        const val DEVICE_NAME = 0x01
        const val SNIFF_INTERVAL_MS = 0x02
        const val BAND = 0x03
        const val SF_MODE = 0x04
        const val FIXED_SF = 0x05
        const val TX_POWER_DBM = 0x06
        const val TELEMETRY_INTERVAL_S = 0x07
        const val BEACON_INTERVAL_S = 0x08
        const val GNSS_FIX_TIMEOUT_S = 0x09

        /*
         * There is deliberately no TLV for the duty cycle limit. Section 3: "It
         * is not configurable, in firmware or over the air." A setter here would
         * be a setter for something the device refuses.
         */

        fun deviceName(name: String): ConfigTlv {
            val utf8 = name.toByteArray(Charsets.UTF_8)
            if (utf8.size > 16) {
                throw BridgeFormatException("device name is ${utf8.size} bytes, limit is 16")
            }
            return ConfigTlv(DEVICE_NAME, utf8)
        }

        /** 250..10000 per section 3. CLAUDE.md 2.3 is why: it trades latency against both battery and airtime. */
        fun sniffIntervalMs(ms: Int): ConfigTlv {
            if (ms !in 250..10_000) {
                throw BridgeFormatException("sniff interval $ms ms is outside 250..10000")
            }
            return ConfigTlv(SNIFF_INTERVAL_MS, ByteWriter().u16(ms).finish())
        }

        fun band(band: Int): ConfigTlv = ConfigTlv(BAND, byteArrayOf(band.toByte()))

        fun fixedSf(sf: Int): ConfigTlv {
            if (sf !in 7..12) {
                throw BridgeFormatException("spreading factor $sf is outside 7..12")
            }
            return ConfigTlv(FIXED_SF, byteArrayOf(sf.toByte()))
        }

        fun telemetryIntervalS(seconds: Int): ConfigTlv =
            ConfigTlv(TELEMETRY_INTERVAL_S, ByteWriter().u16(seconds).finish())

        /** Parse a TLV run. Stops cleanly on a truncated tail rather than guessing. */
        fun decodeAll(body: ByteArray): List<ConfigTlv> {
            val out = ArrayList<ConfigTlv>()
            val r = ByteReader(body)
            while (r.remaining >= 2) {
                val type = r.u8()
                val len = r.u8()
                if (r.remaining < len) {
                    throw BridgeFormatException(
                        "TLV type ${"0x%02X".format(type)} claims $len bytes, ${r.remaining} left"
                    )
                }
                out.add(ConfigTlv(type, r.bytes(len)))
            }
            return out
        }
    }
}
