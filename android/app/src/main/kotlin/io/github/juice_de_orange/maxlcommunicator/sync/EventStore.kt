package io.github.juice_de_orange.maxlcommunicator.sync

import io.github.juice_de_orange.maxlcommunicator.protocol.Message
import java.io.File

/**
 * What the phone durably holds, and in what order it lets go of it.
 *
 * docs/bridge-protocol.md section 3 fixes the ordering and says why:
 *
 * > The phone asks for everything after the last counter it durably stored,
 * > writes those events to its own store, pushes them to the server, and only
 * > then sends ACK_QUEUE. [...] Acknowledging before the server has the data
 * > would lose events whenever the phone dies between the two steps, and this is
 * > a device you carry into places where the phone dies.
 *
 * So [append] must have reached the disk before [highWaterMark] moves, and
 * ACK_QUEUE is only ever sent after a push has returned. Erring towards sending
 * the same event twice is always right: the server's idempotency key makes a
 * duplicate harmless (CLAUDE.md 4.3).
 *
 * A plain append-only file rather than a database. The record is
 * counter + opcode + length + body; the file is read once at startup and
 * appended to thereafter. A database would buy queries nobody makes.
 */
class EventStore(private val file: File) {

    data class Stored(val counter: Long, val opcode: Int, val body: ByteArray) {
        override fun equals(other: Any?): Boolean =
            other is Stored && counter == other.counter && opcode == other.opcode &&
                body.contentEquals(other.body)

        override fun hashCode(): Int =
            (counter.hashCode() * 31 + opcode) * 31 + body.contentHashCode()
    }

    private val events = ArrayList<Stored>()

    /** How many are held but not yet pushed to the server. */
    var pendingPush: Int = 0
        private set

    init {
        load()
    }

    /**
     * The newest journal counter this store has on disk.
     *
     * Zero when it holds nothing -- which is what GET_QUEUE's `sinceCounter`
     * wants for a first sync, and the reason the device's own counter starts at
     * 1 rather than 0. A device whose first entry were 0 would have exactly one
     * journal entry no client could ever ask for.
     */
    fun highWaterMark(): Long = events.maxOfOrNull { it.counter } ?: 0L

    fun size(): Int = events.size

    fun all(): List<Stored> = events.toList()

    /**
     * Append and flush.
     *
     * Returns false if it did not reach the disk, and the caller must then not
     * acknowledge -- the device keeps the event and offers it again.
     */
    fun append(counter: Long, opcode: Int, body: ByteArray): Boolean {
        if (events.any { it.counter == counter }) {
            // Already held. Section 3 makes re-delivery normal, so this is not
            // an error; it is the reason re-sending is always safe.
            return true
        }
        return try {
            file.parentFile?.mkdirs()
            file.appendBytes(encode(counter, opcode, body))
            events.add(Stored(counter, opcode, body))
            pendingPush += 1
            true
        } catch (e: Exception) {
            false
        }
    }

    /** Called once a push has actually returned. Only then may ACK_QUEUE go. */
    fun markPushed(count: Int) {
        pendingPush = (pendingPush - count).coerceAtLeast(0)
    }

    private fun load() {
        events.clear()
        if (!file.exists()) {
            return
        }
        val bytes = try {
            file.readBytes()
        } catch (e: Exception) {
            return
        }

        var at = 0
        while (at + HEADER <= bytes.size) {
            val counter = readU32(bytes, at)
            val opcode = bytes[at + 4].toInt() and 0xFF
            val length = bytes[at + 5].toInt() and 0xFF
            if (at + HEADER + length > bytes.size) {
                // A record cut in half by a phone that died mid-write. Everything
                // before it is intact and is kept; the tail is dropped, and the
                // device will offer those events again because they were never
                // acknowledged.
                break
            }
            events.add(Stored(counter, opcode, bytes.copyOfRange(at + HEADER, at + HEADER + length)))
            at += HEADER + length
        }
        pendingPush = events.size
    }

    private fun encode(counter: Long, opcode: Int, body: ByteArray): ByteArray {
        val out = ByteArray(HEADER + body.size)
        out[0] = (counter and 0xFF).toByte()
        out[1] = ((counter shr 8) and 0xFF).toByte()
        out[2] = ((counter shr 16) and 0xFF).toByte()
        out[3] = ((counter shr 24) and 0xFF).toByte()
        out[4] = (opcode and 0xFF).toByte()
        out[5] = (body.size and 0xFF).toByte()
        body.copyInto(out, HEADER)
        return out
    }

    private fun readU32(bytes: ByteArray, at: Int): Long =
        (bytes[at].toLong() and 0xFF) or
            ((bytes[at + 1].toLong() and 0xFF) shl 8) or
            ((bytes[at + 2].toLong() and 0xFF) shl 16) or
            ((bytes[at + 3].toLong() and 0xFF) shl 24)

    private companion object {
        /** counter u32, opcode u8, length u8. */
        const val HEADER = 6
    }
}

/** Where the events go once the phone has them. Failing is allowed; lying is not. */
interface ServerPush {
    /**
     * Push a batch. Returns true only when the server has durably taken them.
     *
     * A false here keeps the events on the device: ACK_QUEUE is not sent, and the
     * next connection offers them again.
     */
    fun push(nodeId: Int, events: List<EventStore.Stored>): Boolean
}

/** Turns a device message into what the store keeps, or null if it is not an event. */
fun Message.asStoredEvent(counter: Long): EventStore.Stored? =
    if (isEvent) EventStore.Stored(counter, opcode, body) else null
