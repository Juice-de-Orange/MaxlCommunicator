package io.github.juice_de_orange.maxlcommunicator.sync

import java.io.File
import java.nio.file.Files
import kotlin.test.AfterTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/**
 * The store is the one thing here that must not lose data.
 *
 * docs/bridge-protocol.md section 3: the phone writes events durably, pushes
 * them, and only then acknowledges -- "Acknowledging before the server has the
 * data would lose events whenever the phone dies between the two steps, and this
 * is a device you carry into places where the phone dies."
 *
 * These run on the JVM, not on a device: EventStore touches java.io.File and
 * nothing Android. That is deliberate, and it is why they exist at all.
 */
class EventStoreTest {

    private val dir: File = Files.createTempDirectory("maxl-store").toFile()
    private val file = File(dir, "journal.bin")

    @AfterTest
    fun cleanUp() {
        dir.deleteRecursively()
    }

    @Test
    fun `an empty store has a high water mark of zero`() {
        // Which is what GET_QUEUE's sinceCounter wants for a first sync, and the
        // reason the device's own counter starts at 1: a device whose first
        // entry were 0 would have exactly one entry no client could ask for.
        assertEquals(0L, EventStore(file).highWaterMark())
    }

    @Test
    fun `what is appended survives a restart`() {
        val first = EventStore(file)
        assertTrue(first.append(7, 0x81, byteArrayOf(1, 2, 3)))
        assertTrue(first.append(9, 0x82, byteArrayOf(4)))

        val reopened = EventStore(file)
        assertEquals(2, reopened.size())
        assertEquals(9L, reopened.highWaterMark())
        assertEquals(0x81, reopened.all()[0].opcode)
        assertEquals(listOf<Byte>(1, 2, 3), reopened.all()[0].body.toList())
    }

    @Test
    fun `the same event twice is stored once`() {
        // Section 3 makes re-delivery normal -- "erring towards re-sending is
        // always the right call" -- which only holds if the client is idempotent.
        val store = EventStore(file)
        assertTrue(store.append(7, 0x81, byteArrayOf(1)))
        assertTrue(store.append(7, 0x81, byteArrayOf(1)))
        assertEquals(1, store.size())
    }

    @Test
    fun `a record cut in half by a phone that died is dropped, and the rest kept`() {
        val store = EventStore(file)
        store.append(1, 0x81, byteArrayOf(1, 2, 3))
        store.append(2, 0x82, byteArrayOf(4, 5))

        // Truncate mid-record, which is what a process killed during a write
        // leaves behind.
        val bytes = file.readBytes()
        file.writeBytes(bytes.copyOfRange(0, bytes.size - 1))

        val reopened = EventStore(file)
        assertEquals(1, reopened.size())
        assertEquals(1L, reopened.highWaterMark())

        // And the device offers the missing one again, because it was never
        // acknowledged. That is the whole point of the ordering in section 3.
        assertTrue(reopened.append(2, 0x82, byteArrayOf(4, 5)))
        assertEquals(2, reopened.size())
    }

    @Test
    fun `a header cut in half is dropped too`() {
        val store = EventStore(file)
        store.append(1, 0x81, byteArrayOf(1))
        file.appendBytes(byteArrayOf(0x02, 0x00))

        val reopened = EventStore(file)
        assertEquals(1, reopened.size())
    }

    @Test
    fun `everything is pending until something says it was pushed`() {
        val store = EventStore(file)
        store.append(1, 0x81, byteArrayOf(1))
        store.append(2, 0x81, byteArrayOf(2))
        assertEquals(2, store.pendingPush)

        store.markPushed(2)
        assertEquals(0, store.pendingPush)

        // A reopened store treats everything as unpushed again. That is the
        // conservative direction: a duplicate is harmless because of the server's
        // idempotency key, a gap is not.
        assertEquals(2, EventStore(file).pendingPush)
    }

    @Test
    fun `a counter at the top of its range round-trips`() {
        // Journal counters are u32, and the synthetic keys D15 forces on events
        // that carry none start at 0x40000000. Reading either into a signed Int
        // would come back negative.
        val store = EventStore(file)
        assertTrue(store.append(4294967294L, 0x81, byteArrayOf(1)))
        assertEquals(4294967294L, EventStore(file).highWaterMark())
    }

    @Test
    fun `a body at the maximum length round-trips`() {
        // EVT_FRAME_RX is 11 bytes of header plus up to a 48-byte TEXT payload,
        // and the length field is one byte.
        val body = ByteArray(255) { it.toByte() }
        val store = EventStore(file)
        assertTrue(store.append(1, 0x81, body))

        val reopened = EventStore(file)
        assertEquals(1, reopened.size())
        assertEquals(255, reopened.all()[0].body.size)
    }

    @Test
    fun `a store that cannot write says so instead of pretending`() {
        // A store that swallowed a write error would let the caller acknowledge
        // events it had not kept. The path here is a file inside a file.
        val blocker = File(dir, "blocker")
        blocker.writeText("not a directory")
        val store = EventStore(File(blocker, "journal.bin"))
        assertFalse(store.append(1, 0x81, byteArrayOf(1)))
    }
}
