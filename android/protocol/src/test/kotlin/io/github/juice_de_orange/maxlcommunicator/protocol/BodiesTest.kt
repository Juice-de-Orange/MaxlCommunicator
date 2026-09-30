package io.github.juice_de_orange.maxlcommunicator.protocol

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertTrue
import kotlin.test.assertFalse

/** docs/bridge-protocol.md sections 3 and 4: bodies, TLVs and the status blob. */
class BodiesTest {

    @Test
    fun `a SET_CONFIG body is a version followed by a TLV run`() {
        val body = vector("set_config_body")
        val version = ByteReaderProbe(body).u32()
        assertEquals(7L, version)

        val tlvs = ConfigTlv.decodeAll(body.copyOfRange(4, body.size))
        assertEquals(hex(vector("set_config_tlv_run")), hex(body.copyOfRange(4, body.size)))
        assertEquals(5, tlvs.size)

        assertEquals(ConfigTlv.SNIFF_INTERVAL_MS, tlvs[0].type)
        assertEquals(ConfigTlv.BAND, tlvs[1].type)
        assertEquals(ConfigTlv.TX_POWER_DBM, tlvs[2].type)
        assertEquals(ConfigTlv.TELEMETRY_INTERVAL_S, tlvs[3].type)

        // "Unknown types are ignored and reported back in EVT_CONFIG_APPLIED as
        // unapplied, rather than failing the whole write." So decoding must
        // return it, not throw.
        assertEquals(0xF0, tlvs[4].type)
        assertEquals("beef", hex(tlvs[4].value))
    }

    @Test
    fun `encoding a SET_CONFIG body reproduces the shared vector`() {
        val encoded = Commands.setConfig(
            configVersion = 7,
            tlvs = listOf(
                ConfigTlv.sniffIntervalMs(2000),
                ConfigTlv.band(0),
                ConfigTlv(ConfigTlv.TX_POWER_DBM, byteArrayOf(22)),
                ConfigTlv.telemetryIntervalS(600),
                ConfigTlv(0xF0, byteArrayOf(0xbe.toByte(), 0xef.toByte())),
            ),
        )
        assertEquals(hex(vector("set_config_body")), hex(encoded))
    }

    @Test
    fun `a sniff interval outside the documented range is refused here`() {
        // Section 3 gives 250..10000. Refusing locally saves a round trip; the
        // device refuses too, and that is the one that counts.
        assertFailsWith<BridgeFormatException> { ConfigTlv.sniffIntervalMs(100) }
        assertFailsWith<BridgeFormatException> { ConfigTlv.sniffIntervalMs(20_000) }
        ConfigTlv.sniffIntervalMs(250)
        ConfigTlv.sniffIntervalMs(10_000)
    }

    @Test
    fun `a spreading factor outside 7 to 12 is refused`() {
        assertFailsWith<BridgeFormatException> { ConfigTlv.fixedSf(6) }
        assertFailsWith<BridgeFormatException> { ConfigTlv.fixedSf(13) }
    }

    @Test
    fun `the status body is seventeen bytes and its flags mean what section 4 says`() {
        val body = ByteWriterProbe()
            .u16(3987)          // batteryMv
            .u32(86_400)        // uptimeS
            .u8(3)              // queueDepth
            .u8(0)              // band g3
            .u32(42_500)        // budgetUsedMs
            .u32(360_000)       // budgetLimitMs
            .u8(0b011)          // timeValid | keyProvisioned
            .finish()

        assertEquals(StatusBody.BYTES, body.size)
        val status = StatusBody.decode(body)

        assertEquals(3987, status.batteryMv)
        assertEquals(86_400L, status.uptimeS)
        assertEquals(3, status.queueDepth)
        assertEquals("g3", status.bandName)
        assertEquals(317_500L, status.budgetRemainingMs)

        assertTrue(status.timeValid)
        assertTrue(status.keyProvisioned)
        assertFalse(status.gnssPowered)
    }

    @Test
    fun `a node with no valid time says so, which is the whole point of bit zero`() {
        val body = ByteWriterProbe()
            .u16(3987).u32(0).u8(0).u8(0).u32(0).u32(360_000).u8(0)
            .finish()
        assertFalse(StatusBody.decode(body).timeValid)
    }

    @Test
    fun `GET_INFO decodes to seven fields and checks its own compatibility`() {
        // bridgeProtocol 2 since 2026-08-31 -- EVT_JOURNAL, decision D15.
        val body = byteArrayOf(2, 2, 0, 0, 1, 0, 1)
        val info = DeviceInfo.decode(body)

        assertEquals(2, info.bridgeProtocol)
        assertEquals(2, info.nodeId)
        assertEquals("0.1.0", info.firmwareVersion)
        assertEquals(1, info.wireVersion)
        assertTrue(info.isCompatible)

        // A node one version ahead is not compatible, and the client must stop
        // at GET_INFO rather than decode bodies of an unknown shape.
        val future = DeviceInfo.decode(byteArrayOf(3, 2, 0, 0, 1, 0, 1))
        assertFalse(future.isCompatible)

        // And one version behind is just as incompatible: BRIDGE_PROTO went to 2
        // precisely because GET_QUEUE changed shape.
        val past = DeviceInfo.decode(byteArrayOf(1, 2, 0, 0, 1, 0, 1))
        assertFalse(past.isCompatible)
    }

    @Test
    fun `SEND_TEXT counts bytes, not characters`() {
        // CLAUDE.md 2.2 caps TEXT at 48 bytes of UTF-8. One umlaut is two.
        val fortyEightAscii = "A".repeat(48)
        Commands.sendText(2, fortyEightAscii)

        assertFailsWith<BridgeFormatException> { Commands.sendText(2, "A".repeat(49)) }
        assertFailsWith<BridgeFormatException> { Commands.sendText(2, "ü".repeat(25)) }
    }

    @Test
    fun `GET_QUEUE clamps the batch to what the device will give`() {
        // Section 4: "The device caps maxEvents at 32 whatever is asked for."
        val body = Commands.getQueue(1000, 200)
        assertEquals(32, body[4].toInt() and 0xFF)
    }

    @Test
    fun `FACTORY_RESET carries the magic so a stray write cannot mean it`() {
        val body = Commands.factoryReset()
        assertEquals("4d534552", hex(body))
    }

    @Test
    fun `a key must be sixteen bytes`() {
        assertFailsWith<BridgeFormatException> { Commands.provisionKey(0, 42, ByteArray(15)) }
        Commands.provisionKey(0, 42, ByteArray(16))
    }

    @Test
    fun `a truncated TLV run is refused rather than guessed at`() {
        assertFailsWith<BridgeFormatException> {
            ConfigTlv.decodeAll(byteArrayOf(0x02, 0x04, 0x00))
        }
    }
}

/** The internal readers are not public; these thin probes keep the tests honest. */
internal class ByteReaderProbe(bytes: ByteArray) {
    private val reader = ByteReader(bytes)
    fun u32(): Long = reader.u32()
}

internal class ByteWriterProbe {
    private val writer = ByteWriter()
    fun u8(v: Int) = apply { writer.u8(v) }
    fun u16(v: Int) = apply { writer.u16(v) }
    fun u32(v: Long) = apply { writer.u32(v) }
    fun finish(): ByteArray = writer.finish()
}

/**
 * EVT_JOURNAL, section 4 -- decision D15.
 *
 * Against the same two vectors the PWA reads, so "both clients implement the
 * same document" stays checkable rather than hoped for.
 */
class JournalEntryTest {

    @Test
    fun `unwraps an entry and keeps the two counters apart`() {
        val message = Message.decode(vector("evt_journal_wrapping_tx_result"))
        assertEquals(EventCode.JOURNAL, message.opcode)
        // Unsolicited, even though GET_QUEUE asked for it: section 1.2 gives
        // every event txnId 0, and a response carrying 0 would be unroutable.
        assertEquals(0, message.txnId)

        val entry = JournalEntry.decode(message.body)
        assertEquals(4098L, entry.counter)
        assertEquals(EventCode.FRAME_TX_RESULT, entry.opcode)

        // Byte for byte the other vector: the wrapper re-encodes nothing.
        val bare = Message.decode(vector("evt_frame_tx_result"))
        assertEquals(hex(bare.body), hex(entry.body))

        // The point of D10, made checkable. The entry is 4098; the frame it
        // reports on is 1024. A client that acknowledged with the body's counter
        // would free entries it never stored, and would pass every other vector
        // in the file.
        val txResult = FrameTxResult.decode(entry.body)
        assertEquals(1024L, txResult.counter)
        assertTrue(txResult.counter != entry.counter)
    }

    @Test
    fun `keeps an entry whose wrapped opcode it does not know`() {
        val entry = JournalEntry.decode(Message.decode(vector("evt_journal_unknown_opcode")).body)
        assertEquals(4099L, entry.counter)
        assertEquals(0x8F, entry.opcode)
        assertEquals("aabbcc", hex(entry.body))
    }

    @Test
    fun `round-trips`() {
        val original = vector("evt_journal_wrapping_tx_result")
        val entry = JournalEntry.decode(Message.decode(original).body)
        assertEquals(hex(original), hex(Message(EventCode.JOURNAL, 0, entry.encode()).encode()))
    }

    @Test
    fun `refuses a body that will not fit the len field`() {
        assertFailsWith<IllegalArgumentException> {
            JournalEntry(1, EventCode.FRAME_RX, ByteArray(256)).encode()
        }
    }
}
