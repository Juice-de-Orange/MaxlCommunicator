package io.github.juice_de_orange.maxlcommunicator.sync

import io.github.juice_de_orange.maxlcommunicator.protocol.BridgeError
import io.github.juice_de_orange.maxlcommunicator.protocol.EventCode
import io.github.juice_de_orange.maxlcommunicator.protocol.Message
import io.github.juice_de_orange.maxlcommunicator.protocol.Opcode
import java.io.File
import java.nio.file.Files
import kotlin.test.AfterTest
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/**
 * docs/bridge-protocol.md section 5, checked step by step.
 *
 * These run on the JVM because [BridgeSession] depends on [DeviceLink] rather
 * than on the GATT client -- the whole reason that interface exists. Nothing
 * here touches Bluetooth, and nothing here proves Bluetooth works.
 */
class BridgeSessionTest {

    private val dir: File = Files.createTempDirectory("maxl-session").toFile()
    private val node = FakeNode()
    private val server = RecordingServer()
    private var storeIndex = 0

    private fun store(): EventStore = EventStore(File(dir, "events-${storeIndex++}.bin"))

    private fun session(store: EventStore = store()) = BridgeSession(node, store, server)

    @AfterTest
    fun cleanUp() {
        dir.deleteRecursively()
    }

    @Test
    fun `runs section 5 in the documented order`() {
        session().run(nowUnix = 1_800_000_000)

        // GET_INFO first is not a style choice: the compatibility check has to
        // happen before anything is interpreted, because a node speaking a
        // different BRIDGE_PROTO would have different bodies.
        assertEquals(
            listOf(Opcode.GET_INFO, Opcode.GET_STATUS, Opcode.GET_BUDGET, Opcode.GET_QUEUE),
            node.opcodes,
        )
    }

    @Test
    fun `stops at GET_INFO when the node speaks another protocol version`() {
        node.infoBody = FakeNode.info(bridgeProtocol = 3, nodeId = 1)

        assertFailsWith<IllegalStateException> { session().run(1_800_000_000) }

        // The point of the test: not that it threw, but that it asked nothing
        // else. Continuing here would decode bodies of an unknown shape.
        assertEquals(listOf(Opcode.GET_INFO), node.opcodes)
    }

    @Test
    fun `sets the time only when the node says it has none`() {
        node.statusBody = FakeNode.status(timeValid = false)
        session().run(1_800_000_000)
        assertTrue(Opcode.SET_TIME in node.opcodes, "a transmit-blocked node must be given the time")

        node.sent.clear()
        node.statusBody = FakeNode.status(timeValid = true)
        session().run(1_800_000_000)
        assertFalse(Opcode.SET_TIME in node.opcodes, "a node with valid time must not be re-set")
    }

    @Test
    fun `keeps asking while a batch comes back full`() {
        // A full batch means "there may be more". Anything short ends it. Getting
        // this wrong either loses the tail of the journal or never terminates.
        node.queueScript.add((1..32).map { FakeNode.journal(it.toLong(), FakeNode.frameRx(it.toLong())) })
        node.queueScript.add((33..40).map { FakeNode.journal(it.toLong(), FakeNode.frameRx(it.toLong())) })

        val fetched = session().run(1_800_000_000)

        assertEquals(40, fetched)
        assertEquals(2, node.opcodes.count { it == Opcode.GET_QUEUE })
    }

    @Test
    fun `takes the events from the emissions, not from the response`() {
        // GET_QUEUE's response body is the count, never the events. A client that
        // parsed events out of the response would fetch nothing and report
        // success -- the worst shape of failure this protocol has.
        // The journal counters are 7 and 8; the FRAME counters inside are 700 and
        // 800. What gets stored must be the former -- D10, and the whole reason
        // EVT_JOURNAL exists. A client reading the body's counter would store
        // 700 and 800, acknowledge 800, and free entries nobody has.
        node.queueScript.add(
            listOf(
                FakeNode.journal(7, FakeNode.frameRx(700)),
                FakeNode.journal(8, FakeNode.frameRx(800)),
            )
        )
        val store = store()

        assertEquals(2, session(store).run(1_800_000_000))
        assertEquals(listOf(7L, 8L), store.all().map { it.counter })
    }

    @Test
    fun `acknowledges with the journal counter, after the server has the events`() {
        // Decision D15, resolved: EVT_JOURNAL carries the counter, so ACK_QUEUE
        // can be formed at all -- and it goes out only after the push, which is
        // the order section 5 steps 5 and 6 insist on.
        node.queueScript.add(
            listOf(FakeNode.journal(1, FakeNode.frameRx(1)), FakeNode.journal(2, FakeNode.frameRx(2)))
        )
        session().run(1_800_000_000)

        assertTrue(Opcode.ACK_QUEUE in node.opcodes)
        assertTrue(
            node.opcodes.indexOf(Opcode.ACK_QUEUE) > node.opcodes.indexOf(Opcode.GET_QUEUE),
            "the acknowledgement must follow the fetch, never precede it",
        )
        val acked = node.sent.last { it.opcode == Opcode.ACK_QUEUE }
        assertEquals(2L, littleEndianU32(acked.body))
    }

    @Test
    fun `acknowledges nothing when the server refuses the push`() {
        node.queueScript.add(listOf(FakeNode.journal(1, FakeNode.frameRx(1))))
        server.accept = false

        session().run(1_800_000_000)

        assertFalse(
            Opcode.ACK_QUEUE in node.opcodes,
            "acknowledging before the server has the data loses events whenever the phone dies between the two",
        )
    }

    @Test
    fun `keeps a journal entry whose wrapped opcode it does not know`() {
        // Section 4: the counter is legible even when the body is not, so the
        // entry is stored and counted towards ACK_QUEUE. Dropping it would free
        // journal space for something nobody ever saw -- and this is what makes
        // a future event type safe to add.
        node.queueScript.add(listOf(FakeNode.journal(9, Message(0x8F, 0, byteArrayOf(1, 2, 3)))))
        val store = store()

        assertEquals(1, session(store).run(1_800_000_000))
        assertEquals(listOf(9L), store.all().map { it.counter })
        assertEquals(0x8F, store.all().first().opcode)
    }

    @Test
    fun `does not store a spontaneous event`() {
        // A bare event is the device saying something just happened. It carries
        // no counter, so it can neither be acknowledged nor deduplicated against
        // the wrapped copy that follows on the next GET_QUEUE. Section 4 puts it
        // in the live UI and nowhere else.
        node.queueScript.add(listOf(FakeNode.frameRx(1)))
        val store = store()

        assertEquals(0, session(store).run(1_800_000_000))
        assertEquals(0, store.size())
    }

    @Test
    fun `stores every event type, not only frame-rx`() {
        // The other half of D15: the PWA kept EVT_FRAME_RX and dropped the other
        // six, which is exactly the delivery-state history D10 exists to carry.
        node.queueScript.add(
            listOf(
                FakeNode.journal(1, FakeNode.frameRx(1)),
                FakeNode.journal(2, FakeNode.event(EventCode.FRAME_TX_RESULT, 0x11)),
                FakeNode.journal(3, FakeNode.event(EventCode.STATUS, 0x22)),
                FakeNode.journal(4, FakeNode.event(EventCode.LOG, 0x33)),
                FakeNode.journal(5, FakeNode.event(EventCode.CONFIG_APPLIED, 0x44)),
                FakeNode.journal(6, FakeNode.event(EventCode.FIX, 0x55)),
                FakeNode.journal(7, FakeNode.event(EventCode.BUDGET, 0x66)),
            )
        )
        val store = store()

        assertEquals(7, session(store).run(1_800_000_000))
        assertEquals(
            listOf(
                EventCode.FRAME_RX, EventCode.FRAME_TX_RESULT, EventCode.STATUS, EventCode.LOG,
                EventCode.CONFIG_APPLIED, EventCode.FIX, EventCode.BUDGET,
            ),
            store.all().map { it.opcode },
        )
    }

    @Test
    fun `keeps the events when the server refuses them`() {
        node.queueScript.add(
            listOf(FakeNode.journal(1, FakeNode.frameRx(1)), FakeNode.journal(2, FakeNode.frameRx(2)))
        )
        server.accept = false
        val store = store()

        session(store).run(1_800_000_000)

        assertEquals(2, store.size(), "a failed push must not lose the events")
        assertEquals(2, store.pendingPush, "and must leave them pending for the next connection")
    }

    @Test
    fun `sends queued text at the end, after the journal is drained`() {
        val s = session()
        s.queueText(dst = 2, text = "hello")
        s.run(1_800_000_000)

        assertEquals(Opcode.SEND_TEXT, node.opcodes.last())
        // dst:u16 le, len:u8, then the bytes.
        assertContentEquals(
            byteArrayOf(2, 0, 5) + "hello".toByteArray(Charsets.UTF_8),
            node.sent.last().body,
        )
    }

    @Test
    fun `puts text back when the node is out of budget`() {
        node.refuse[Opcode.SEND_TEXT] = BridgeError.BUDGET_EXHAUSTED.code
        val s = session()
        s.queueText(dst = 2, text = "hello")
        s.run(1_800_000_000)

        assertEquals(1, node.opcodes.count { it == Opcode.SEND_TEXT })
        assertTrue(s.snapshot.error != null, "and the user is told why")

        // The second connection tries again. ERR_BUDGET_EXHAUSTED is the node
        // saying "not yet", not "no".
        node.sent.clear()
        s.run(1_800_000_100)
        assertEquals(1, node.opcodes.count { it == Opcode.SEND_TEXT })
    }

    @Test
    fun `drops text the node refused on its own merits`() {
        // ERR_BAD_PARAM is not "not yet": the same bytes get the same answer for
        // ever, so re-sending it would spend a write on every sync.
        node.refuse[Opcode.SEND_TEXT] = BridgeError.BAD_PARAM.code
        val s = session()
        s.queueText(dst = 2, text = "hello")
        s.run(1_800_000_000)

        node.sent.clear()
        s.run(1_800_000_100)
        assertFalse(Opcode.SEND_TEXT in node.opcodes)
    }

    @Test
    fun `keeps text refused for a state the node can leave`() {
        /*
         * ERR_NO_TIME reads like a hard refusal and is not one: the node is
         * transmit-blocked because its RTC is invalid, and step 4 of this very
         * sequence is one of the two ways out of that. Same for the two that are
         * answered by bonding and by provisioning. Dropping any of them loses
         * what the user wrote for a reason that is not about what they wrote.
         */
        for (code in listOf(BridgeError.NO_TIME, BridgeError.NOT_AUTHORISED, BridgeError.NO_KEY)) {
            val s = session()
            s.queueText(dst = 2, text = "wartet")

            node.refuse[Opcode.SEND_TEXT] = code.code
            s.run(1_800_000_000)

            node.sent.clear()
            s.run(1_800_000_100)
            assertEquals(1, node.opcodes.count { it == Opcode.SEND_TEXT }, "$code must be retried")
        }
    }

    @Test
    fun `numbers transactions from one, never zero`() {
        // txnId 0 marks an event. A response carrying 0 would be indistinguishable
        // from one, and the client would wait for a response that had arrived.
        val s = session()
        repeat(40) { s.run(1_800_000_000) }
        assertTrue(node.sent.none { it.txnId == 0 }, "no command may use the event txnId")
    }

    @Test
    fun `fails loudly when the link will not take a write`() {
        node.acceptWrites = false
        assertFailsWith<IllegalStateException> { session().run(1_800_000_000) }
    }

    @Test
    fun `reports what it fetched and what it still holds`() {
        node.queueScript.add(
            (1..3).map { FakeNode.journal(it.toLong(), FakeNode.frameRx(it.toLong())) }
        )
        server.accept = false
        val store = store()
        val s = session(store)

        s.run(1_800_000_000)

        assertEquals(3, s.snapshot.fetched)
        assertEquals(3, s.snapshot.heldOnPhone)
        assertEquals(1_800_000_000L, s.snapshot.lastSyncUnix)
        assertEquals(0x0042, s.snapshot.info?.nodeId)
        assertEquals(360_000L, s.snapshot.budget?.limitMs)
    }
}

/**
 * `upToCounter:u32`, read without the protocol module's internal ByteReader.
 *
 * Written out rather than imported on purpose: this is the test asserting what
 * went on the wire, and it should not share the reader with the code that put it
 * there.
 */
private fun littleEndianU32(bytes: ByteArray): Long =
    (bytes[0].toLong() and 0xFF) or
        ((bytes[1].toLong() and 0xFF) shl 8) or
        ((bytes[2].toLong() and 0xFF) shl 16) or
        ((bytes[3].toLong() and 0xFF) shl 24)
