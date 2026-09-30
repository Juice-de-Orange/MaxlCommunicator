package io.github.juice_de_orange.maxlcommunicator.sync

import io.github.juice_de_orange.maxlcommunicator.protocol.EventCode
import io.github.juice_de_orange.maxlcommunicator.protocol.Gatt
import io.github.juice_de_orange.maxlcommunicator.protocol.JournalEntry
import io.github.juice_de_orange.maxlcommunicator.protocol.Message
import io.github.juice_de_orange.maxlcommunicator.protocol.Opcode

/**
 * A node that answers over a [DeviceLink], scripted from the test.
 *
 * It answers from inside [send], on the caller's thread. That is not a shortcut:
 * BridgeSession blocks on a queue with an eight-second timeout, so answering
 * synchronously means a failing test fails immediately instead of after eight
 * seconds of nothing -- and the code path exercised is the same one, because
 * BridgeSession clears the response queue before it sends.
 */
class FakeNode : DeviceLink {

    override var listener: DeviceLink.Listener? = null

    /** Everything the client sent, in order. The order is what section 5 is. */
    val sent = ArrayList<Message>()

    val opcodes: List<Int> get() = sent.map { it.opcode }

    var infoBody: ByteArray = info(bridgeProtocol = Gatt.BRIDGE_PROTOCOL_VERSION, nodeId = 0x0042)
    var statusBody: ByteArray = status(timeValid = true)
    var budgetBody: ByteArray = budget()

    /**
     * One entry per GET_QUEUE, consumed in order: the events to emit before
     * answering. When the script runs out the node answers with nothing, which
     * is what an idle node does.
     */
    val queueScript = ArrayDeque<List<Message>>()

    /** Opcodes to refuse once, with the error code to refuse them with. */
    val refuse = HashMap<Int, Int>()

    /** Set when the client writes without the node ever having accepted one. */
    var acceptWrites = true

    override fun send(message: Message): Boolean {
        if (!acceptWrites) return false
        sent.add(message)

        val error = refuse.remove(message.opcode)
        if (error != null) {
            emit(Message(Opcode.RSP_ERR, message.txnId, byteArrayOf(message.opcode.toByte(), error.toByte())))
            return true
        }

        when (message.opcode) {
            Opcode.GET_INFO -> ok(message.txnId, message.opcode, infoBody)
            Opcode.GET_STATUS -> ok(message.txnId, message.opcode, statusBody)
            Opcode.GET_BUDGET -> ok(message.txnId, message.opcode, budgetBody)
            Opcode.GET_QUEUE -> {
                // Section 3: the events come first, each unsolicited, and the
                // response carries only how many there were.
                val batch = if (queueScript.isEmpty()) emptyList() else queueScript.removeFirst()
                batch.forEach { emit(it) }
                ok(message.txnId, message.opcode, byteArrayOf(batch.size.toByte()))
            }
            else -> ok(message.txnId, message.opcode, ByteArray(0))
        }
        return true
    }

    private fun ok(txnId: Int, echoed: Int, payload: ByteArray) =
        emit(Message(Opcode.RSP_OK, txnId, byteArrayOf(echoed.toByte()) + payload))

    private fun emit(message: Message) {
        listener?.onMessage(message)
    }

    companion object {
        /**
         * Wrap an event the way GET_QUEUE delivers it (section 4, D15).
         *
         * `counter` is the JOURNAL counter -- deliberately a separate argument
         * from anything inside `inner`, because D10 turns on the two not being
         * the same number.
         */
        fun journal(counter: Long, inner: Message): Message =
            Message(
                EventCode.JOURNAL,
                0,
                JournalEntry(counter, inner.opcode, inner.body).encode(),
            )

        /*
         * The bodies are built here by hand rather than through the protocol
         * module's writer, which is internal to it anyway. Building them from
         * the field list in docs/bridge-protocol.md is the point: a test that
         * encoded with the same helper the decoder was written against would
         * agree with itself and prove nothing.
         */

        private fun u16(v: Int) = byteArrayOf((v and 0xFF).toByte(), ((v shr 8) and 0xFF).toByte())

        private fun u32(v: Long) = byteArrayOf(
            (v and 0xFF).toByte(), ((v shr 8) and 0xFF).toByte(),
            ((v shr 16) and 0xFF).toByte(), ((v shr 24) and 0xFF).toByte(),
        )

        /** `bridgeProtocol:u8, nodeId:u16, fw major/minor/patch:u8, wire:u8` */
        fun info(bridgeProtocol: Int, nodeId: Int): ByteArray =
            byteArrayOf(bridgeProtocol.toByte()) + u16(nodeId) + byteArrayOf(0, 1, 0, 1)

        /** `batt:u16, uptime:u32, queue:u8, band:u8, used:u32, limit:u32, flags:u8` */
        fun status(timeValid: Boolean, queueDepth: Int = 0): ByteArray =
            u16(4100) + u32(3600) + byteArrayOf(queueDepth.toByte(), 0) +
                u32(0) + u32(360_000) + byteArrayOf(if (timeValid) 0x03 else 0x02)

        /** `band:u8, used:u32, limit:u32, nextTx:u32` */
        fun budget(usedMs: Long = 0): ByteArray =
            byteArrayOf(0) + u32(usedMs) + u32(360_000) + u32(0)

        /** An EVT_FRAME_RX whose body starts with the frame counter. */
        fun frameRx(counter: Long): Message =
            Message(EventCode.FRAME_RX, 0, u32(counter) + u16(2) + byteArrayOf(5, 0xC0.toByte(), 0xFF.toByte(), 8, 0))

        /** An event that carries no counter of any kind. */
        fun event(opcode: Int, marker: Int): Message =
            Message(opcode, 0, byteArrayOf(marker.toByte()))
    }
}

/** A server that remembers, and can be told to fail. */
class RecordingServer : ServerPush {
    val batches = ArrayList<List<EventStore.Stored>>()
    var accept = true

    override fun push(nodeId: Int, events: List<EventStore.Stored>): Boolean {
        batches.add(events)
        return accept
    }
}
