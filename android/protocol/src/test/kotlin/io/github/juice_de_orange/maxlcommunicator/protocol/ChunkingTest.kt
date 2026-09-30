package io.github.juice_de_orange.maxlcommunicator.protocol

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue
import kotlin.test.assertFailsWith

/**
 * docs/bridge-protocol.md section 1.1.
 *
 * Checked against the shared chunk vectors, so agreeing here is agreeing with
 * the firmware and the PWA rather than with itself. The MTU is 23 -- section 1's
 * "assume 23 until negotiation completes" -- which leaves 18 fragment bytes, and
 * that is what produced those vectors.
 */
class ChunkingTest {

    @Test
    fun `a one-chunk message matches the shared vector`() {
        val message = vector("chunk_single_message")
        val chunks = Chunking.split(message, msgId = 5, mtu = Chunking.DEFAULT_MTU)

        assertEquals(1, chunks.size)
        assertEquals(hex(vector("chunk_single_c0")), hex(chunks[0]))

        // "A single-chunk message has both F and L set and chunkIndex = 0."
        assertEquals(0xC5, chunks[0][0].toInt() and 0xFF)
        assertEquals(0, chunks[0][1].toInt())
    }

    @Test
    fun `a three-chunk message matches all three shared vectors`() {
        val message = vector("chunk_three_message")
        val chunks = Chunking.split(message, msgId = 9, mtu = Chunking.DEFAULT_MTU)

        assertEquals(3, chunks.size)
        assertEquals(hex(vector("chunk_three_c0")), hex(chunks[0]))
        assertEquals(hex(vector("chunk_three_c1")), hex(chunks[1]))
        assertEquals(hex(vector("chunk_three_c2")), hex(chunks[2]))
    }

    @Test
    fun `the flag bits are where the specification puts them`() {
        val chunks = Chunking.split(vector("chunk_three_message"), msgId = 9, mtu = 23)

        // F only, then neither, then L only -- and msgId 9 throughout.
        assertEquals(0x89, chunks[0][0].toInt() and 0xFF)
        assertEquals(0x09, chunks[1][0].toInt() and 0xFF)
        assertEquals(0x49, chunks[2][0].toInt() and 0xFF)
    }

    @Test
    fun `chunks reassemble into the message they came from`() {
        val message = vector("chunk_three_message")
        val reassembler = Reassembler()

        assertNull(reassembler.accept(vector("chunk_three_c0"), 0))
        assertNull(reassembler.accept(vector("chunk_three_c1"), 100))
        val out = reassembler.accept(vector("chunk_three_c2"), 200)

        assertEquals(hex(message), hex(out!!))
        assertEquals(0, reassembler.dropped)
    }

    @Test
    fun `a chunk out of order drops the whole message`() {
        val reassembler = Reassembler()
        reassembler.accept(vector("chunk_three_c0"), 0)

        // Section 1.1: no recovery, no reordering window. The sender re-sends.
        assertNull(reassembler.accept(vector("chunk_three_c2"), 100))
        assertEquals(1, reassembler.dropped)
        assertTrue(!reassembler.hasPartial)
    }

    @Test
    fun `a msgId that changes mid-message drops it`() {
        val reassembler = Reassembler()
        reassembler.accept(vector("chunk_three_c0"), 0)

        // Same index, different id: the other end started something else.
        val stray = vector("chunk_three_c1").copyOf()
        stray[0] = ((stray[0].toInt() and 0xC0) or 0x11).toByte()

        assertNull(reassembler.accept(stray, 50))
        assertEquals(1, reassembler.dropped)
    }

    @Test
    fun `five seconds between chunks drops it`() {
        val reassembler = Reassembler(timeoutMs = 5_000)
        reassembler.accept(vector("chunk_three_c0"), 0)

        assertNull(reassembler.accept(vector("chunk_three_c1"), 6_000))
        assertEquals(1, reassembler.dropped)
    }

    @Test
    fun `a new first chunk abandons whatever was in flight`() {
        val reassembler = Reassembler()
        reassembler.accept(vector("chunk_three_c0"), 0)

        // The sender gave up and started again. Not an error on this side, but
        // the partial is gone.
        val out = reassembler.accept(vector("chunk_single_c0"), 100)
        assertEquals(hex(vector("chunk_single_message")), hex(out!!))
        assertEquals(1, reassembler.dropped)
    }

    @Test
    fun `a continuation with nothing to continue is ignored, not counted`() {
        // What arrives right after a reconnect. Counting it as our drop would
        // make the number useless as a link-quality signal.
        val reassembler = Reassembler()
        assertNull(reassembler.accept(vector("chunk_three_c1"), 0))
        assertEquals(0, reassembler.dropped)
    }

    @Test
    fun `a disconnect abandons a partial message`() {
        val reassembler = Reassembler()
        reassembler.accept(vector("chunk_three_c0"), 0)
        reassembler.onDisconnect()

        assertEquals(1, reassembler.dropped)
        assertTrue(!reassembler.hasPartial)
    }

    @Test
    fun `the fragment size follows the negotiated MTU`() {
        // Section 1.1: at most MTU - 3 - 2 payload bytes.
        assertEquals(18, Chunking.fragmentSize(23))
        assertEquals(242, Chunking.fragmentSize(247))

        // A larger MTU means fewer chunks for the same message.
        val message = vector("chunk_three_message")
        assertEquals(3, Chunking.split(message, 1, 23).size)
        assertEquals(1, Chunking.split(message, 1, 247).size)
    }

    @Test
    fun `a message past the limit is refused rather than truncated`() {
        val tooBig = ByteArray(Chunking.MAX_MESSAGE_BYTES + 1)
        assertFailsWith<BridgeFormatException> { Chunking.split(tooBig, 1, 247) }
    }

    @Test
    fun `reassembly refuses to grow past the limit`() {
        val reassembler = Reassembler()
        // A first chunk that claims to continue for ever.
        var accepted = 0
        var index = 0
        while (index < 300) {
            val header = if (index == 0) 0x80 else 0x00
            val chunk = ByteArray(2 + 242)
            chunk[0] = header.toByte()
            chunk[1] = (index and 0xFF).toByte()
            if (reassembler.accept(chunk, index.toLong()) != null) accepted += 1
            if (reassembler.dropped > 0) break
            index += 1
        }
        assertEquals(0, accepted)
        assertTrue(reassembler.dropped > 0, "the 4096-byte limit must stop it")
    }

    @Test
    fun `msgId wraps within six bits`() {
        // "a 6-bit rolling counter per direction"
        val chunks = Chunking.split(byteArrayOf(1, 2), msgId = 0x7F, mtu = 23)
        assertEquals(0x3F, chunks[0][0].toInt() and 0x3F)
    }
}
