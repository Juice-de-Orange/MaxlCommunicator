package io.github.juice_de_orange.maxlcommunicator.sync

import io.github.juice_de_orange.maxlcommunicator.protocol.BridgeError
import io.github.juice_de_orange.maxlcommunicator.protocol.BridgeErrorException
import io.github.juice_de_orange.maxlcommunicator.protocol.BudgetBody
import io.github.juice_de_orange.maxlcommunicator.protocol.Commands
import io.github.juice_de_orange.maxlcommunicator.protocol.DeviceInfo
import io.github.juice_de_orange.maxlcommunicator.protocol.Gatt
import io.github.juice_de_orange.maxlcommunicator.protocol.EventCode
import io.github.juice_de_orange.maxlcommunicator.protocol.JournalEntry
import io.github.juice_de_orange.maxlcommunicator.protocol.Message
import io.github.juice_de_orange.maxlcommunicator.protocol.Opcode
import io.github.juice_de_orange.maxlcommunicator.protocol.StatusBody
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.TimeUnit

/**
 * docs/bridge-protocol.md section 5, in order, and nothing else.
 *
 * > The client must implement exactly this sequence, because the foreground-only
 * > constraint means every connection is short and must be productive.
 *
 * Written against the document rather than against the PWA. Everything it knows
 * about Bluetooth it knows through [DeviceLink]; everything it knows about bytes
 * it knows through :protocol.
 */
class BridgeSession(
    private val gatt: DeviceLink,
    private val store: EventStore,
    private val server: ServerPush,
) : DeviceLink.Listener {

    data class Snapshot(
        val info: DeviceInfo? = null,
        val status: StatusBody? = null,
        val budget: BudgetBody? = null,
        val lastSyncUnix: Long = 0,
        val fetched: Int = 0,
        val heldOnPhone: Int = 0,
        val error: String? = null,
    )

    @Volatile
    var snapshot: Snapshot = Snapshot()
        private set

    /** Text the user wrote while nothing was connected. Flushed at step 7. */
    private val outbox = ArrayList<Pair<Int, String>>()

    private val responses = ArrayBlockingQueue<Message>(4)
    private val inbox = ArrayList<Message>()
    private var nextTxn = 1

    init {
        gatt.listener = this
        snapshot = snapshot.copy(heldOnPhone = store.size())
    }

    fun queueText(dst: Int, text: String) {
        synchronized(outbox) { outbox.add(dst to text) }
    }

    /**
     * The whole of section 5. Returns the number of journal events fetched.
     *
     * Throws rather than returning a code: every step here is a precondition for
     * the next, and a caller that could continue past a failure would be a
     * caller that had not read section 5.
     */
    fun run(nowUnix: Long): Int {
        // 2. GET_INFO -- check BRIDGE_PROTO compatibility BEFORE anything else.
        val info = DeviceInfo.decode(request(Opcode.GET_INFO).body.drop(1).toByteArray())
        snapshot = snapshot.copy(info = info)
        if (!info.isCompatible) {
            throw IllegalStateException(
                "node speaks BRIDGE_PROTO ${info.bridgeProtocol}, " +
                    "this client speaks ${Gatt.BRIDGE_PROTOCOL_VERSION}"
            )
        }

        // 3. Populate the UI immediately, "so the user sees something within a
        //    second of opening the app".
        val status = StatusBody.decode(request(Opcode.GET_STATUS).body.drop(1).toByteArray())
        val budget = BudgetBody.decode(request(Opcode.GET_BUDGET).body.drop(1).toByteArray())
        snapshot = snapshot.copy(status = status, budget = budget)

        // 4. SET_TIME if the node reports no valid time. Not cosmetic: a node
        //    with no valid time is transmit-blocked (CLAUDE.md 1.2), and this is
        //    one of only two ways out of that state.
        if (!status.timeValid) {
            request(Opcode.SET_TIME, Commands.setTime(nowUnix))
        }

        val fetched = drainJournal()

        // 7. Flush what the user wrote while offline.
        val refusal = flushOutbox()

        snapshot = snapshot.copy(
            lastSyncUnix = nowUnix,
            fetched = fetched,
            heldOnPhone = store.size(),
            // Not unconditionally null: a text the node refused for budget is
            // the one thing in this whole sequence the user actually needs to
            // read, and clearing it here would throw it away in the same call
            // that produced it. Null only when this connection had no refusal.
            error = refusal,
        )
        return fetched
    }

    /**
     * Step 5 and 6.
     *
     * GET_QUEUE does not return the events. It emits them first, each as its own
     * unsolicited message, and then answers with the count -- so a client
     * collects what arrives and uses the response only to know when to stop
     * asking. Waiting for a response containing the events waits for ever.
     *
     * Each event arrives wrapped in EVT_JOURNAL and carries its journal counter
     * (D15). That counter is what ACK_QUEUE speaks in, and it goes out only
     * after the server has the events -- the order in section 5 steps 5 and 6,
     * which exists because acknowledging first loses everything whenever the
     * phone dies between the two.
     */
    private fun drainJournal(): Int {
        var since = store.highWaterMark()
        var fetched = 0

        while (true) {
            synchronized(inbox) { inbox.clear() }
            val response = request(Opcode.GET_QUEUE, Commands.getQueue(since, BATCH))
            val count = response.body.getOrNull(1)?.toInt()?.and(0xFF) ?: 0

            val arrived = synchronized(inbox) { inbox.toList() }
            for (message in arrived) {
                /*
                 * Only what came wrapped is durable.
                 *
                 * A bare event is the device saying something just happened; it
                 * carries no counter, cannot be acknowledged, and will be offered
                 * again -- wrapped -- on the next GET_QUEUE. Storing it would
                 * create an entry that can neither be acknowledged nor
                 * deduplicated against the wrapped copy.
                 *
                 * An unknown wrapped opcode is still stored: section 4 requires
                 * it, because the counter is legible even when the body is not,
                 * and dropping the entry would free journal space for something
                 * nobody ever saw.
                 */
                if (message.opcode != EventCode.JOURNAL) {
                    continue
                }
                val entry = JournalEntry.decode(message.body)
                if (store.append(entry.counter, entry.opcode, entry.body)) {
                    fetched += 1
                }
            }
            if (arrived.isNotEmpty()) {
                since = store.highWaterMark()
            }
            if (count < BATCH) {
                break
            }
        }

        // 6. The server first, then the acknowledgement. If the push fails,
        //    nothing is acknowledged and the device still holds everything.
        val pending = store.all()
        if (pending.isNotEmpty() && store.pendingPush > 0) {
            val nodeId = snapshot.info?.nodeId ?: 0
            if (server.push(nodeId, pending)) {
                store.markPushed(pending.size)
                request(Opcode.ACK_QUEUE, Commands.ackQueue(pending.last().counter))
            }
        }
        return fetched
    }

    /** Returns what to show the user, or null if everything went out. */
    private fun flushOutbox(): String? {
        val waiting = synchronized(outbox) {
            val copy = outbox.toList()
            outbox.clear()
            copy
        }
        var refusal: String? = null
        for ((dst, text) in waiting) {
            try {
                request(Opcode.SEND_TEXT, Commands.sendText(dst, text))
            } catch (e: BridgeErrorException) {
                /*
                 * The line is not which error it is but what the error is ABOUT
                 * -- docs/bridge-protocol.md section 5, step 7.
                 *
                 * About the message: the same bytes get the same answer for
                 * ever, so it goes. About the node's state: it stays, because
                 * the state changes. ERR_BUDGET_EXHAUSTED is the ordinary answer
                 * at 10 %; ERR_NO_TIME is undone by step 4 of this very
                 * sequence; ERR_NOT_AUTHORISED and ERR_NO_KEY are undone by
                 * bonding and provisioning. Dropping one of those would lose
                 * what the user wrote for a reason that is not about what they
                 * wrote.
                 */
                if (e.error !in ABOUT_THE_MESSAGE) {
                    synchronized(outbox) { outbox.add(dst to text) }
                }
                refusal = e.message
            }
        }
        return refusal
    }

    private fun request(opcode: Int, body: ByteArray = ByteArray(0), timeoutMs: Long = 8_000): Message {
        val txnId = nextTxn
        nextTxn = if (nextTxn >= 0xFF) 1 else nextTxn + 1  // never 0: that means an event

        responses.clear()
        if (!gatt.send(Message(opcode, txnId, body))) {
            throw IllegalStateException("could not write opcode 0x%02X".format(opcode))
        }

        val response = responses.poll(timeoutMs, TimeUnit.MILLISECONDS)
            ?: throw IllegalStateException("no response to opcode 0x%02X".format(opcode))

        if (response.opcode == Opcode.RSP_ERR) {
            val failed = response.body.getOrNull(0)?.toInt()?.and(0xFF) ?: 0
            val code = response.body.getOrNull(1)?.toInt()?.and(0xFF) ?: 0
            throw BridgeErrorException(failed, BridgeError.from(code), code)
        }
        return response
    }

    // --- DeviceLink.Listener ---------------------------------------------------

    override fun onMessage(message: Message) {
        if (message.isResponse) {
            responses.offer(message)
            return
        }
        if (message.isEvent) {
            synchronized(inbox) { inbox.add(message) }
        }
    }

    override fun onConnectionChanged(connected: Boolean) {
        if (!connected) {
            responses.clear()
        }
    }

    private companion object {
        /**
         * The refusals that are about the text rather than about the node.
         *
         * A short list with everything else on the other side on purpose: an
         * error code the device gains later is far more likely to be a state
         * than a verdict on these particular bytes, and guessing wrong that way
         * only costs a retry. Guessing wrong the other way loses the message.
         */
        val ABOUT_THE_MESSAGE = setOf(
            BridgeError.BAD_LENGTH, BridgeError.BAD_PARAM, BridgeError.UNSUPPORTED,
        )

        /** The device caps maxEvents at 32 whatever is asked for. */
        const val BATCH = 32
    }
}
