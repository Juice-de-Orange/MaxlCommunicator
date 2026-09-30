package io.github.juice_de_orange.maxlcommunicator.protocol

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

/**
 * docs/bridge-protocol.md section 1.
 *
 * Written out in full rather than derived, because these are what a client
 * filters on: a wrong digit produces a service nothing can find, and nothing
 * anywhere reports an error.
 */
class GattTest {

    @Test
    fun `the UUIDs are the ones in the specification`() {
        assertEquals("6d61786c-0001-4c6f-5261-4e6f64650000", Gatt.SERVICE.toString())
        assertEquals("6d61786c-0002-4c6f-5261-4e6f64650000", Gatt.TX.toString())
        assertEquals("6d61786c-0003-4c6f-5261-4e6f64650000", Gatt.RX.toString())
        assertEquals("6d61786c-0004-4c6f-5261-4e6f64650000", Gatt.CONFIG.toString())
        assertEquals("6d61786c-0005-4c6f-5261-4e6f64650000", Gatt.STATUS.toString())
    }

    @Test
    fun `only the second group differs between them`() {
        val parts = listOf(Gatt.SERVICE, Gatt.TX, Gatt.RX, Gatt.CONFIG, Gatt.STATUS)
            .map { it.toString().split("-") }

        val varying = parts.map { it[1] }.toSet()
        assertEquals(5, varying.size)

        for (index in listOf(0, 2, 3, 4)) {
            assertEquals(1, parts.map { it[index] }.toSet().size, "group $index must be constant")
        }
    }

    @Test
    fun `the name prefix is what tells two nodes apart`() {
        assertTrue("Maxl-ABCD".startsWith(Gatt.NAME_PREFIX))
        assertEquals(2, Gatt.BRIDGE_PROTOCOL_VERSION)
    }
}
