package io.github.juice_de_orange.maxlcommunicator.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothProfile
import android.content.Context
import android.os.Build
import io.github.juice_de_orange.maxlcommunicator.protocol.Chunking
import io.github.juice_de_orange.maxlcommunicator.protocol.Gatt
import io.github.juice_de_orange.maxlcommunicator.protocol.Message
import io.github.juice_de_orange.maxlcommunicator.protocol.Reassembler
import io.github.juice_de_orange.maxlcommunicator.sync.DeviceLink
import java.util.UUID
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.TimeUnit

/**
 * The GATT client, and nothing above it.
 *
 * It knows about Android's Bluetooth stack and about chunks. It does not know
 * what a command is, what order they go in, or what to do with an event -- that
 * is [io.github.juice_de_orange.maxlcommunicator.sync.BridgeSession], which is written against
 * docs/bridge-protocol.md and could equally drive a mock.
 *
 * The split is the same one the firmware makes at hal::IBleTransport, and for
 * the same reason: everything above this line is testable without a radio.
 */
class MaxlGatt(private val context: Context) : DeviceLink {

    override var listener: DeviceLink.Listener? = null

    @Volatile
    var mtu: Int = Chunking.DEFAULT_MTU
        private set

    @Volatile
    var connected: Boolean = false
        private set

    private var gatt: BluetoothGatt? = null
    private var rx: BluetoothGattCharacteristic? = null
    private val reassembler = Reassembler()

    /**
     * One write at a time.
     *
     * docs/bridge-protocol.md section 1.1: one message in flight per direction,
     * and section 6 explains why -- "A BLE link to a sleeping microcontroller is
     * not the place to discover a reordering bug." Android will also silently
     * drop a second write issued before the first has completed, so this is both
     * the protocol's rule and the platform's.
     */
    private val writeAcks = ArrayBlockingQueue<Boolean>(1)

    /** msgId is a 6-bit rolling counter per direction, section 1.1. */
    private var nextMsgId = 0

    @SuppressLint("MissingPermission")
    fun connect(device: BluetoothDevice) {
        close()
        gatt = device.connectGatt(context, false, callback, BluetoothDevice.TRANSPORT_LE)
    }

    @SuppressLint("MissingPermission")
    fun close() {
        gatt?.let {
            it.disconnect()
            it.close()
        }
        gatt = null
        rx = null
        connected = false
        mtu = Chunking.DEFAULT_MTU
        reassembler.onDisconnect()
    }

    /**
     * Send one reassembled message, split into as many writes as the MTU needs.
     *
     * Blocks until the last chunk has been acknowledged by the stack, or until
     * the write times out. Blocking is correct here: the caller is the sync
     * service's own thread, the protocol allows one message in flight anyway,
     * and an asynchronous version would need a queue that could only ever hold
     * one entry.
     */
    @SuppressLint("MissingPermission")
    override fun send(message: Message): Boolean {
        val target = rx ?: return false
        val handle = gatt ?: return false

        val id = nextMsgId
        nextMsgId = (nextMsgId + 1) and 0x3F

        val chunks = Chunking.split(message.encode(), id, mtu)
        for (chunk in chunks) {
            writeAcks.clear()
            val started = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                handle.writeCharacteristic(
                    target, chunk, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                ) == BluetoothGatt.GATT_SUCCESS
            } else {
                @Suppress("DEPRECATION")
                run {
                    target.value = chunk
                    target.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                    handle.writeCharacteristic(target)
                }
            }
            if (!started) {
                return false
            }
            val acked = writeAcks.poll(WRITE_TIMEOUT_MS, TimeUnit.MILLISECONDS)
            if (acked != true) {
                return false
            }
        }
        return true
    }

    private val callback = object : BluetoothGattCallback() {

        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                // Section 5 step 1: connect, then negotiate MTU. Asking for the
                // maximum and taking what arrives -- section 1 says assume 23
                // until negotiation completes, which is what `mtu` already holds.
                g.requestMtu(Chunking.MAX_MTU)
            } else {
                connected = false
                reassembler.onDisconnect()
                listener?.onConnectionChanged(false)
            }
        }

        @SuppressLint("MissingPermission")
        override fun onMtuChanged(g: BluetoothGatt, negotiated: Int, status: Int) {
            mtu = if (status == BluetoothGatt.GATT_SUCCESS) negotiated else Chunking.DEFAULT_MTU
            g.discoverServices()
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val service = g.getService(Gatt.SERVICE)
            if (service == null) {
                // Not our device, or the firmware wrote its 128-bit UUID
                // least-significant byte first by accident -- which produces a
                // service no client can find and reports no error anywhere.
                listener?.onConnectionChanged(false)
                return
            }

            rx = service.getCharacteristic(Gatt.RX)
            val tx = service.getCharacteristic(Gatt.TX)
            if (rx == null || tx == null) {
                listener?.onConnectionChanged(false)
                return
            }

            g.setCharacteristicNotification(tx, true)
            val descriptor = tx.getDescriptor(CLIENT_CONFIG)
            if (descriptor != null) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    g.writeDescriptor(descriptor, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                } else {
                    @Suppress("DEPRECATION")
                    run {
                        descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                        g.writeDescriptor(descriptor)
                    }
                }
            }

            connected = true
            listener?.onConnectionChanged(true)
        }

        override fun onCharacteristicWrite(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int,
        ) {
            writeAcks.offer(status == BluetoothGatt.GATT_SUCCESS)
        }

        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            deliver(characteristic.uuid, value)
        }

        @Deprecated("Superseded on API 33; still called below it.")
        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
        ) {
            deliver(characteristic.uuid, characteristic.value ?: ByteArray(0))
        }
    }

    private fun deliver(uuid: UUID, value: ByteArray) {
        if (uuid != Gatt.TX) {
            return
        }
        val complete = reassembler.accept(value, System.currentTimeMillis()) ?: return
        listener?.onMessage(Message.decode(complete))
    }

    private companion object {
        /**
         * How long one chunk's write-acknowledgement may take.
         *
         * Per chunk, not per message: a 5 KB tile is ~20 writes, and a single
         * budget for the whole message would either be far too tight for that or
         * far too slack for a status query.
         */
        const val WRITE_TIMEOUT_MS = 5_000L

        /** The standard Client Characteristic Configuration descriptor. */
        val CLIENT_CONFIG: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    }
}
