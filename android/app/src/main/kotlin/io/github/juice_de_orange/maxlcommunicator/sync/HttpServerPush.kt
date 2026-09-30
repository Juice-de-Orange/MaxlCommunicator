package io.github.juice_de_orange.maxlcommunicator.sync

import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.time.Instant
import java.util.Base64

/**
 * The real ServerPush: POST /api/ingest with a bearer token, exactly as
 * bridge/src/sync/server.ts does it -- that file is the reference
 * implementation of the ordering rule in docs/bridge-protocol.md section 5:
 * events reach the server BEFORE ACK_QUEUE goes back to the device, and this
 * class returning true is what BridgeSession acknowledges on.
 *
 * Until 2026-09-01 the app carried only the NoServerYet stub, so the code that
 * sends ACK_QUEUE existed, was tested against a RecordingServer, and never ran:
 * push() always said false and the device's journal only ever grew.
 *
 * The JSON is built by hand rather than through a library: the payload is
 * numbers, base64 and an ISO timestamp -- none of which ever contain a quote or
 * a backslash -- and :app deliberately has no JSON dependency to date.
 *
 * Plain java.net + java.util so the whole class runs on the JVM under
 * testDebugUnitTest against a loopback HttpServer, phone not required.
 */
class HttpServerPush(
    private val baseUrl: String,
    private val token: String,
    private val clock: () -> Instant = Instant::now,
) : ServerPush {

    override fun push(nodeId: Int, events: List<EventStore.Stored>): Boolean {
        if (events.isEmpty()) {
            return true
        }
        // web/src/pages/api/ingest.ts caps a batch at 512; the PWA sends 400.
        // Same margin here, same reason: stay clearly under the refusal.
        var offset = 0
        while (offset < events.size) {
            val slice = events.subList(offset, minOf(offset + MAX_BATCH, events.size))
            if (!pushBatch(nodeId, slice)) {
                // Partial progress is fine: what landed is idempotent on the
                // server (UNIQUE device/counter/direction), and ACK_QUEUE for
                // it simply waits for a sync that gets further.
                return false
            }
            offset += MAX_BATCH
        }
        return true
    }

    private fun pushBatch(nodeId: Int, events: List<EventStore.Stored>): Boolean {
        val receivedAt = clock().toString()
        val body = buildString {
            append("{\"nodeId\":").append(nodeId).append(",\"events\":[")
            events.forEachIndexed { index, event ->
                if (index > 0) append(',')
                append("{\"journalCounter\":").append(event.counter)
                // Everything the device journals is an event from the device.
                append(",\"direction\":\"rx\"")
                append(",\"opcode\":").append(event.opcode)
                append(",\"body\":\"")
                append(Base64.getEncoder().encodeToString(event.body))
                append("\",\"receivedAt\":\"").append(receivedAt).append("\"}")
            }
            append("]}")
        }

        return try {
            val connection = URL("$baseUrl/api/ingest").openConnection() as HttpURLConnection
            try {
                connection.requestMethod = "POST"
                connection.connectTimeout = TIMEOUT_MS
                connection.readTimeout = TIMEOUT_MS
                connection.doOutput = true
                connection.setRequestProperty("content-type", "application/json")
                connection.setRequestProperty("authorization", "Bearer $token")
                connection.outputStream.use { it.write(body.toByteArray(Charsets.UTF_8)) }
                // The response body is ingest statistics; the contract here is
                // only "the server durably took them", which the status carries.
                connection.responseCode in 200..299
            } finally {
                connection.disconnect()
            }
        } catch (_: IOException) {
            // Failing is allowed; lying is not (ServerPush's contract). The
            // events stay held and the next sync offers them again.
            false
        }
    }

    private companion object {
        const val MAX_BATCH = 400
        const val TIMEOUT_MS = 15_000
    }
}
