package io.github.juice_de_orange.maxlcommunicator.sync

import java.io.BufferedReader
import java.io.InputStreamReader
import java.net.ServerSocket
import java.time.Instant
import java.util.Base64
import kotlin.concurrent.thread
import kotlin.test.AfterTest
import kotlin.test.BeforeTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/**
 * HttpServerPush against a loopback server: the wire shape must match what
 * web/src/pages/api/ingest.ts validates, because that endpoint is the one
 * authority on the format (and bridge/src/sync/server.ts the reference client).
 *
 * The server is a hand-rolled ServerSocket loop rather than
 * com.sun.net.httpserver: unit tests compile against android.jar, which does
 * not carry the com.sun tree.
 */
class HttpServerPushTest {

    private lateinit var socket: ServerSocket
    private var acceptor: Thread? = null
    private val requests = ArrayList<Pair<Map<String, String>, String>>()
    @Volatile private var status = 200

    @BeforeTest
    fun start() {
        socket = ServerSocket(0)
        acceptor = thread(isDaemon = true) {
            while (!socket.isClosed) {
                val client = try {
                    socket.accept()
                } catch (_: Exception) {
                    break
                }
                client.use { connection ->
                    val reader = BufferedReader(InputStreamReader(connection.getInputStream()))
                    val headers = HashMap<String, String>()
                    reader.readLine() ?: return@use // request line
                    while (true) {
                        val line = reader.readLine() ?: break
                        if (line.isEmpty()) break
                        val colon = line.indexOf(':')
                        if (colon > 0) {
                            headers[line.substring(0, colon).lowercase()] =
                                line.substring(colon + 1).trim()
                        }
                    }
                    val length = headers["content-length"]?.toIntOrNull() ?: 0
                    val body = CharArray(length)
                    var read = 0
                    while (read < length) {
                        val n = reader.read(body, read, length - read)
                        if (n < 0) break
                        read += n
                    }
                    synchronized(requests) { requests.add(headers to String(body, 0, read)) }

                    val answer =
                        "{\"received\":1,\"inserted\":1,\"duplicates\":0,\"highWaterMark\":9}"
                    val response = "HTTP/1.1 $status X\r\n" +
                        "content-type: application/json\r\n" +
                        "content-length: ${answer.length}\r\n" +
                        "connection: close\r\n\r\n" + answer
                    connection.getOutputStream().write(response.toByteArray())
                    connection.getOutputStream().flush()
                }
            }
        }
    }

    @AfterTest
    fun stop() {
        socket.close()
        acceptor?.join(2000)
    }

    private fun push(events: List<EventStore.Stored>, nodeId: Int = 1): Boolean {
        val push = HttpServerPush(
            baseUrl = "http://127.0.0.1:${socket.localPort}",
            token = "geheim",
            clock = { Instant.parse("2026-09-01T02:00:00Z") },
        )
        return push.push(nodeId, events)
    }

    @Test
    fun `sends the shape ingest validates, with the bearer token`() {
        val body = byteArrayOf(0x10, 0x00, 0x00, 0x00, 0x7F)
        assertTrue(push(listOf(EventStore.Stored(4098, 0x82, body))))

        val (headers, request) = synchronized(requests) { requests.single() }
        assertEquals("Bearer geheim", headers["authorization"])
        assertEquals("application/json", headers["content-type"])
        // The journal counter keys the row (D10) -- not the frame counter in
        // the body, which here would be 0x10 = 16, not 4098.
        assertTrue("\"journalCounter\":4098" in request)
        assertTrue("\"direction\":\"rx\"" in request)
        assertTrue("\"opcode\":130" in request)
        assertTrue("\"body\":\"${Base64.getEncoder().encodeToString(body)}\"" in request)
        assertTrue("\"receivedAt\":\"2026-09-01T02:00:00Z\"" in request)
        assertTrue("\"nodeId\":1" in request)
    }

    @Test
    fun `batches at 400, like the PWA`() {
        val events = (1L..401L).map { EventStore.Stored(it, 0x81, byteArrayOf(1)) }
        assertTrue(push(events))
        val bodies = synchronized(requests) { requests.map { it.second } }
        assertEquals(2, bodies.size)
        assertTrue("\"journalCounter\":400," in bodies[0])
        assertFalse("\"journalCounter\":401," in bodies[0])
        assertTrue("\"journalCounter\":401," in bodies[1])
    }

    @Test
    fun `a refused batch is false -- nothing gets acknowledged on a lie`() {
        status = 401
        assertFalse(push(listOf(EventStore.Stored(1, 0x81, byteArrayOf(1)))))
    }

    @Test
    fun `an unreachable server is false, not an exception`() {
        socket.close()
        assertFalse(push(listOf(EventStore.Stored(1, 0x81, byteArrayOf(1)))))
    }

    @Test
    fun `an empty batch is trivially true and sends nothing`() {
        assertTrue(push(emptyList()))
        assertTrue(synchronized(requests) { requests.isEmpty() })
    }
}
