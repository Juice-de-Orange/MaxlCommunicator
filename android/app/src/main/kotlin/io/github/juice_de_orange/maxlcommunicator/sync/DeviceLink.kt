package io.github.juice_de_orange.maxlcommunicator.sync

import io.github.juice_de_orange.maxlcommunicator.protocol.Message

/**
 * A way to exchange bridge-protocol messages with a node, with no opinion about
 * how they get there.
 *
 * This is the same cut decision D3 makes in the firmware, for the same reason.
 * There, `link/` depends on `hal::IRadioLink` rather than on RadioLib, and the
 * proof that the layering is real is that `link/` compiles and runs on a host
 * with no Arduino core in sight. Here, [BridgeSession] implements section 5 of
 * `docs/bridge-protocol.md` -- which steps run, in which order, and what makes
 * one fail -- and none of that is about Bluetooth.
 *
 * Without this interface the entire connection lifecycle could only be exercised
 * on a phone, because [io.github.juice_de_orange.maxlcommunicator.ble.MaxlGatt] pulls in android.bluetooth
 * and the JVM stubs throw on contact. The class comment there already claimed
 * "everything above this line is testable without a radio"; the claim was true of
 * the design and false of the code until the dependency pointed at an interface.
 */
interface DeviceLink {

    interface Listener {
        /** A complete reassembled message arrived from the node. */
        fun onMessage(message: Message)

        fun onConnectionChanged(connected: Boolean)
    }

    var listener: Listener?

    /**
     * Send one message, blocking until the node has taken it.
     *
     * Blocking on purpose: the protocol allows one message in flight, so an
     * asynchronous version would need a queue that could only ever hold one
     * entry. False means it did not go out -- not that it went out and failed.
     */
    fun send(message: Message): Boolean
}
