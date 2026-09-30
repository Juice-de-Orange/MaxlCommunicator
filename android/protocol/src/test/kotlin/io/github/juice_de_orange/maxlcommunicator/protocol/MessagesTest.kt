package io.github.juice_de_orange.maxlcommunicator.protocol

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertTrue
import kotlin.test.assertFalse

/** docs/bridge-protocol.md sections 1.2, 2 and 4, against the shared vectors. */
class MessagesTest {

    @Test
    fun `a message is opcode, txnId and body`() {
        val message = Message.decode(vector("chunk_single_message"))
        assertEquals(Opcode.GET_INFO, message.opcode)
        assertEquals(0x2A, message.txnId)
        assertEquals(0, message.body.size)
    }

    @Test
    fun `encoding round-trips through the shared vector`() {
        val message = Message(Opcode.GET_INFO, 0x2A, ByteArray(0))
        assertEquals(hex(vector("chunk_single_message")), hex(message.encode()))
    }

    @Test
    fun `a refused command names the opcode that failed`() {
        // Section 2: "A bonded-tier command on an unbonded connection returns
        // ERR_NOT_AUTHORISED. It does not silently no-op."
        val message = Message.decode(vector("rsp_err_not_authorised"))

        assertEquals(Opcode.RSP_ERR, message.opcode)
        assertEquals(17, message.txnId)
        assertEquals(Opcode.PROVISION_KEY, message.body[0].toInt() and 0xFF)
        assertEquals(BridgeError.NOT_AUTHORISED, BridgeError.from(message.body[1].toInt() and 0xFF))
        assertTrue(message.isResponse)
    }

    @Test
    fun `events carry txnId zero`() {
        val budget = Message.decode(vector("evt_budget"))
        assertEquals(EventCode.BUDGET, budget.opcode)
        assertEquals(0, budget.txnId)
        assertTrue(budget.isEvent)
        assertFalse(budget.isResponse)
    }

    @Test
    fun `the budget event decodes to the values the vector documents`() {
        val body = Message.decode(vector("evt_budget")).body
        val budget = BudgetBody.decode(body)

        assertEquals(0, budget.band)              // g3
        assertEquals(42_500L, budget.usedMs)
        assertEquals(360_000L, budget.limitMs)    // CLAUDE.md 1.3's g3 allowance
        assertEquals(1_788_003_600L, budget.nextTxUnix)
        assertEquals(317_500L, budget.remainingMs)
    }

    @Test
    fun `a transmit result decodes, including its negative RSSI`() {
        val body = Message.decode(vector("evt_frame_tx_result")).body
        val result = FrameTxResult.decode(body)

        assertEquals(1024L, result.counter)
        assertEquals(2, result.dst)
        assertEquals(3, result.seq)
        assertEquals(FrameTxResult.UNDELIVERED, result.result)
        assertEquals(4, result.attempts)
        assertEquals(-97, result.rssiDbm)
        assertEquals(7, result.snrDb)

        assertFalse(result.isDelivered)
        assertTrue(result.isTerminal)
    }

    @Test
    fun `queued is not a terminal state`() {
        // Section 4: "State 2 may be followed later by 0 or 1 for the same
        // counter -- the phone and the server must treat the journal as a log of
        // state transitions, not as a set of final outcomes."
        val queued = FrameTxResult(1, 2, 3, FrameTxResult.QUEUED, 0, 0, 0)
        assertFalse(queued.isTerminal)
        assertFalse(queued.isDelivered)
    }

    @Test
    fun `the open tier is exactly the three commands section 2 lists`() {
        assertTrue(Opcode.isOpenTier(Opcode.GET_INFO))
        assertTrue(Opcode.isOpenTier(Opcode.GET_STATUS))
        assertTrue(Opcode.isOpenTier(Opcode.GET_BUDGET))

        assertFalse(Opcode.isOpenTier(Opcode.PROVISION_KEY))
        assertFalse(Opcode.isOpenTier(Opcode.SET_CONFIG))
        assertFalse(Opcode.isOpenTier(Opcode.FACTORY_RESET))
        assertFalse(Opcode.isOpenTier(Opcode.SEND_TEXT))
    }

    @Test
    fun `every error code round-trips`() {
        for (error in BridgeError.entries) {
            assertEquals(error, BridgeError.from(error.code))
        }
        // An unknown code is null rather than a wrong guess. Section 4 tells the
        // client to degrade on ERR_UNSUPPORTED, not to invent meanings.
        assertEquals(null, BridgeError.from(0x7F))
    }

    @Test
    fun `a truncated message is refused`() {
        assertFailsWith<BridgeFormatException> { Message.decode(byteArrayOf(0x01)) }
    }
}
