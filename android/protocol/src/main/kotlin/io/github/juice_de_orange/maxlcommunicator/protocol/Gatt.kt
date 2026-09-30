package io.github.juice_de_orange.maxlcommunicator.protocol

import java.util.UUID

/**
 * The GATT service and its four characteristics.
 *
 * docs/bridge-protocol.md section 1. The bytes spell the project: `6d61786c` is
 * `maxl`, `4c6f` `Lo`, `5261` `Ra`, `4e6f6465` `Node`, and only the second group
 * varies between the service and its characteristics.
 *
 * These are in the pure-Kotlin module rather than in the Android one on purpose.
 * A UUID is a fact about the protocol, and putting it beside the Bluetooth code
 * would mean the protocol module could not describe the protocol completely.
 */
object Gatt {
    private fun uuid(second: String): UUID =
        UUID.fromString("6d61786c-$second-4c6f-5261-4e6f64650000")

    val SERVICE: UUID = uuid("0001")

    /** notify, device to phone: events and responses. */
    val TX: UUID = uuid("0002")

    /** write, phone to device: commands. */
    val RX: UUID = uuid("0003")

    /** read/write, the settings blob. */
    val CONFIG: UUID = uuid("0004")

    /** read/notify: battery, uptime, queue depth, budget. */
    val STATUS: UUID = uuid("0005")

    /**
     * What a node calls itself: `Maxl-` and the top four hex digits of the
     * nRF52840's factory DEVICEID.
     *
     * Worth matching on rather than assuming, because two nodes in one rucksack
     * differ only in those four digits -- and the same identifier is what
     * firmware/nodes.ini distinguishes them by.
     */
    const val NAME_PREFIX: String = "Maxl-"

    /** docs/bridge-protocol.md: reported by GET_INFO, checked before anything else. */
    /**
     * Went to 2 on 2026-08-31 for `EVT_JOURNAL` and nothing else -- decision D15.
     * With two nodes there is no rolling upgrade; both get flashed in the same
     * session (`CLAUDE.md` section 6), so the break costs nothing.
     */
    const val BRIDGE_PROTOCOL_VERSION: Int = 2
}
