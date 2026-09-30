package io.github.juice_de_orange.maxlcommunicator.protocol

/**
 * The message layer, docs/bridge-protocol.md section 1.2.
 *
 * ```
 * byte 0  : opcode
 * byte 1  : txnId        echoed in the response; the phone increments it, wrapping freely
 * byte 2..: body
 * ```
 *
 * "Every command produces exactly one response, RSP_OK or RSP_ERR, carrying the
 * same txnId. Events are unsolicited and carry txnId = 0."
 */
data class Message(val opcode: Int, val txnId: Int, val body: ByteArray) {
    fun encode(): ByteArray = ByteWriter().u8(opcode).u8(txnId).bytes(body).finish()

    val isEvent: Boolean get() = txnId == 0 && opcode >= 0x80 && opcode < 0xC0
    val isResponse: Boolean get() = opcode == Opcode.RSP_OK || opcode == Opcode.RSP_ERR

    override fun equals(other: Any?): Boolean =
        other is Message && opcode == other.opcode && txnId == other.txnId &&
            body.contentEquals(other.body)

    override fun hashCode(): Int = 31 * (31 * opcode + txnId) + body.contentHashCode()

    companion object {
        fun decode(bytes: ByteArray): Message {
            if (bytes.size < 2) {
                throw BridgeFormatException("message is ${bytes.size} bytes, minimum is 2")
            }
            return Message(
                opcode = bytes[0].toInt() and 0xFF,
                txnId = bytes[1].toInt() and 0xFF,
                body = bytes.copyOfRange(2, bytes.size),
            )
        }
    }
}

/** Section 3, and the two response opcodes from section 4. */
object Opcode {
    const val GET_INFO = 0x01
    const val GET_STATUS = 0x02
    const val SEND_TEXT = 0x03
    const val GET_QUEUE = 0x04
    const val ACK_QUEUE = 0x05
    const val GET_CONFIG = 0x06
    const val SET_CONFIG = 0x07
    const val SET_TIME = 0x08
    const val REQUEST_FIX = 0x09
    const val PROVISION_KEY = 0x0A
    const val ROTATE_KEY = 0x0B
    const val FACTORY_RESET = 0x0C
    const val GET_BUDGET = 0x0D
    const val LINK_TEST = 0x0E

    const val RSP_OK = 0xC0
    const val RSP_ERR = 0xC1

    /**
     * Section 2. The tier is enforced on the device, not by the client -- this is
     * here so the UI can grey out what will fail, never so it can decide.
     *
     * "A bonded-tier command on an unbonded connection returns
     * ERR_NOT_AUTHORISED. It does not silently no-op."
     */
    fun isOpenTier(opcode: Int): Boolean =
        opcode == GET_INFO || opcode == GET_STATUS || opcode == GET_BUDGET
}

/** Section 4. Events are unsolicited and carry txnId 0. */
object EventCode {
    const val FRAME_RX = 0x81
    const val FRAME_TX_RESULT = 0x82
    const val STATUS = 0x83
    const val LOG = 0x84
    const val CONFIG_APPLIED = 0x85
    const val FIX = 0x86
    const val BUDGET = 0x87

    /**
     * A journal entry with its counter, and the only place a journal counter
     * appears on the wire (decision D15).
     *
     * Only GET_QUEUE produces it. It wraps one of the codes above; the counter
     * it carries is the JOURNAL counter, which is not the frame counter that
     * FRAME_RX and FRAME_TX_RESULT also have in their bodies (D10).
     */
    const val JOURNAL = 0x88
}

/** Section 4, error codes. */
enum class BridgeError(val code: Int) {
    /** "unknown opcode -- the client should degrade, not fail" */
    UNSUPPORTED(0x01),
    BAD_LENGTH(0x02),
    BAD_PARAM(0x03),
    NOT_AUTHORISED(0x04),
    NO_KEY(0x05),

    /** "duty cycle; EVT_BUDGET carries the release time" */
    BUDGET_EXHAUSTED(0x06),
    QUEUE_FULL(0x07),

    /** "RTC invalid, node is transmit-blocked" */
    NO_TIME(0x08),
    BUSY(0x09),
    STORAGE(0x0A);

    companion object {
        fun from(code: Int): BridgeError? = entries.firstOrNull { it.code == code }
    }
}

/** Thrown when the device answered RSP_ERR. Carries what it said. */
class BridgeErrorException(
    val opcode: Int,
    val error: BridgeError?,
    val rawCode: Int,
) : RuntimeException("opcode 0x%02X refused: %s".format(opcode, error?.name ?: "0x%02X".format(rawCode)))
