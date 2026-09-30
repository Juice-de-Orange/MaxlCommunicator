package io.github.juice_de_orange.maxlcommunicator.protocol

/**
 * Splitting messages into BLE writes, and putting them back together.
 *
 * docs/bridge-protocol.md section 1.1:
 *
 * ```
 * byte 0 : F(1) | L(1) | msgId(6)     F = first chunk, L = last chunk
 * byte 1 : chunkIndex (0..255)
 * byte 2..: fragment
 * ```
 *
 * `msgId` is a 6-bit rolling counter per direction. It exists to detect
 * interleaving and loss, **not to allow concurrency** -- one message in flight
 * per direction at a time. Section 6 says why: "A BLE link to a sleeping
 * microcontroller is not the place to discover a reordering bug."
 */
object Chunking {
    const val HEADER_BYTES = 2

    /** Section 1.1. Anything larger is a protocol error, not a big message. */
    const val MAX_MESSAGE_BYTES = 4096

    /** Section 1: "Assume 23 until negotiation completes." */
    const val DEFAULT_MTU = 23
    const val MAX_MTU = 247

    private const val FIRST = 0x80
    private const val LAST = 0x40
    private const val MSG_ID_MASK = 0x3F

    /**
     * Fragment bytes that fit in one write at this MTU.
     *
     * Section 1.1: "chunks of at most MTU - 3 - 2 payload bytes". The 3 is ATT's
     * own overhead on a write, the 2 is the header above.
     */
    fun fragmentSize(mtu: Int): Int {
        val usable = mtu - 3 - HEADER_BYTES
        if (usable < 1) {
            throw BridgeFormatException("MTU $mtu leaves no room for a fragment")
        }
        return usable
    }

    /** Split one reassembled message into the writes that carry it. */
    fun split(message: ByteArray, msgId: Int, mtu: Int): List<ByteArray> {
        if (message.isEmpty()) {
            throw BridgeFormatException("nothing to send")
        }
        if (message.size > MAX_MESSAGE_BYTES) {
            throw BridgeFormatException("message is ${message.size} bytes, limit is $MAX_MESSAGE_BYTES")
        }

        val size = fragmentSize(mtu)
        val id = msgId and MSG_ID_MASK
        val chunks = ArrayList<ByteArray>()

        var offset = 0
        var index = 0
        while (offset < message.size) {
            val end = minOf(offset + size, message.size)
            val first = offset == 0
            val last = end == message.size

            if (index > 0xFF) {
                // 256 chunks of at least 18 bytes is past the 4096 limit above,
                // so this is unreachable -- and it is checked anyway, because
                // "unreachable" is a claim about today's constants.
                throw BridgeFormatException("more than 256 chunks")
            }

            val header = (if (first) FIRST else 0) or (if (last) LAST else 0) or id
            chunks.add(
                ByteWriter().u8(header).u8(index).bytes(message.copyOfRange(offset, end)).finish()
            )

            offset = end
            index += 1
        }
        return chunks
    }
}

/**
 * Puts chunks back into messages, and drops what it cannot trust.
 *
 * Section 1.1: "The receiver drops a partial message if a chunk arrives out of
 * order, if `msgId` changes mid-message, or if 5 s elapse between chunks. It
 * does not attempt recovery; the sender re-sends the whole message."
 *
 * So there is no buffering of a second message, no reordering window and no
 * request for a missing chunk. Every one of those would be a way for two
 * different messages to be spliced into one, which on this link would surface as
 * a corrupt journal event rather than as an error.
 *
 * Time is passed in rather than read, so the timeout is testable without waiting
 * five seconds -- and so this module still depends on nothing.
 */
class Reassembler(private val timeoutMs: Long = 5_000) {
    private var buffer = ByteArray(0)
    private var msgId = -1
    private var nextIndex = 0
    private var lastChunkAtMs = 0L
    private var inProgress = false

    /** Partial messages abandoned: out of order, wrong id, or timed out. */
    var dropped: Int = 0
        private set

    val hasPartial: Boolean get() = inProgress

    /**
     * Feed one chunk. Returns the complete message, or null while more is needed.
     *
     * A dropped partial does not throw: the sender re-sends, and a client that
     * crashed on every lost notification would be useless on a link that loses
     * them.
     */
    fun accept(chunk: ByteArray, nowMs: Long): ByteArray? {
        if (chunk.size < Chunking.HEADER_BYTES) {
            drop()
            return null
        }

        val header = chunk[0].toInt() and 0xFF
        val first = (header and 0x80) != 0
        val last = (header and 0x40) != 0
        val id = header and 0x3F
        val index = chunk[1].toInt() and 0xFF
        val fragment = chunk.copyOfRange(Chunking.HEADER_BYTES, chunk.size)

        if (inProgress && nowMs - lastChunkAtMs > timeoutMs) {
            drop()
        }

        if (first) {
            // A new first chunk always wins. If one was in flight it is gone,
            // and that is the sender having given up on it rather than an error
            // on this side.
            if (inProgress) {
                drop()
            }
            buffer = ByteArray(0)
            msgId = id
            nextIndex = 0
            inProgress = true
        } else if (!inProgress) {
            // A continuation with nothing to continue. Common right after a
            // reconnect, and not worth counting as a drop of ours.
            return null
        }

        if (id != msgId || index != nextIndex) {
            drop()
            return null
        }

        if (buffer.size + fragment.size > Chunking.MAX_MESSAGE_BYTES) {
            drop()
            return null
        }

        buffer += fragment
        nextIndex += 1
        lastChunkAtMs = nowMs

        if (!last) {
            return null
        }

        val complete = buffer
        reset()
        return complete
    }

    /** Section 5: the link went away, so anything half-assembled is abandoned. */
    fun onDisconnect() {
        if (inProgress) {
            dropped += 1
        }
        reset()
    }

    private fun drop() {
        dropped += 1
        reset()
    }

    private fun reset() {
        buffer = ByteArray(0)
        msgId = -1
        nextIndex = 0
        inProgress = false
    }
}
