package io.github.juice_de_orange.maxlcommunicator.app

import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.os.ParcelUuid
import io.github.juice_de_orange.maxlcommunicator.ble.MaxlGatt
import io.github.juice_de_orange.maxlcommunicator.protocol.Gatt
import io.github.juice_de_orange.maxlcommunicator.sync.BridgeSession
import io.github.juice_de_orange.maxlcommunicator.sync.EventStore
import io.github.juice_de_orange.maxlcommunicator.sync.ServerPush
import java.io.File
import kotlin.concurrent.thread

/**
 * The reason phase 9 exists.
 *
 * CLAUDE.md 4.2, about the PWA it replaces:
 *
 * > The connection drops when the tab is hidden and there is no background mode.
 * > [...] Do not attempt to work around the foreground limitation with service
 * > workers, wake locks or periodic background sync -- none of them keep a GATT
 * > connection alive. Background sync is what Phase 9 is for.
 *
 * A foreground service does keep it alive, and it is the only thing that does.
 * The notification is not decoration either: it is what the platform demands in
 * exchange, and what tells the user their phone is holding a radio link open.
 */
class SyncService : Service() {

    private lateinit var gatt: MaxlGatt
    private lateinit var store: EventStore
    private lateinit var session: BridgeSession

    private var scanning = false
    private var worker: Thread? = null

    override fun onCreate() {
        super.onCreate()
        createChannel()

        gatt = MaxlGatt(this)
        store = EventStore(File(filesDir, "journal.bin"))
        session = BridgeSession(gatt, store, serverPush())
        state = state.copy(heldOnPhone = store.size())
    }

    @SuppressLint("MissingPermission")
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForeground(NOTIFICATION_ID, notification("Looking for a node"))
        startScan()
        // START_STICKY: if the platform kills this for memory, the user's intent
        // was to keep syncing and the service should come back.
        return START_STICKY
    }

    override fun onDestroy() {
        stopScan()
        gatt.close()
        worker?.interrupt()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    // --- scanning ------------------------------------------------------------

    @SuppressLint("MissingPermission")
    private fun startScan() {
        if (scanning) {
            return
        }
        val manager = getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager ?: return
        val scanner = manager.adapter?.bluetoothLeScanner ?: return

        /*
         * Filtered on the service UUID rather than on the name.
         *
         * The UUID is in the advertising packet for exactly this; a name filter
         * would need the scan record parsed and would match a device that merely
         * calls itself the same thing.
         */
        val filter = ScanFilter.Builder()
            .setServiceUuid(ParcelUuid(Gatt.SERVICE))
            .build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_BALANCED)
            .build()

        scanner.startScan(listOf(filter), settings, scanCallback)
        scanning = true
    }

    @SuppressLint("MissingPermission")
    private fun stopScan() {
        if (!scanning) {
            return
        }
        val manager = getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
        manager?.adapter?.bluetoothLeScanner?.stopScan(scanCallback)
        scanning = false
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val device: BluetoothDevice = result.device ?: return
            stopScan()
            connect(device)
        }

        override fun onScanFailed(errorCode: Int) {
            scanning = false
            state = state.copy(lastError = "Scan failed ($errorCode)")
            update()
        }
    }

    // --- the session ---------------------------------------------------------

    private fun connect(device: BluetoothDevice) {
        gatt.connect(device)

        worker?.interrupt()
        worker = thread(name = "maxl-sync") {
            // The GATT callbacks arrive on a binder thread; this one blocks on
            // them, which is why BridgeSession's requests are synchronous.
            val deadline = System.currentTimeMillis() + CONNECT_TIMEOUT_MS
            while (!gatt.connected && System.currentTimeMillis() < deadline) {
                Thread.sleep(100)
            }
            if (!gatt.connected) {
                state = state.copy(lastError = "Could not establish a connection")
                update()
                startScan()
                return@thread
            }

            try {
                val fetched = session.run(System.currentTimeMillis() / 1000)
                state = state.copy(
                    connected = true,
                    lastSyncUnix = System.currentTimeMillis() / 1000,
                    lastFetched = fetched,
                    heldOnPhone = store.size(),
                    lastError = null,
                )
            } catch (e: Exception) {
                state = state.copy(connected = false, lastError = e.message ?: e.toString())
            }
            update()
        }
    }

    // --- the notification ----------------------------------------------------

    private fun createChannel() {
        val manager = getSystemService(NotificationManager::class.java) ?: return
        val channel = NotificationChannel(
            CHANNEL_ID,
            "Node sync",
            NotificationManager.IMPORTANCE_LOW,
        )
        channel.description = "Keeps the connection to the node open while syncing."
        manager.createNotificationChannel(channel)
    }

    private fun notification(text: String): Notification {
        val builder = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            Notification.Builder(this, CHANNEL_ID)
        } else {
            @Suppress("DEPRECATION")
            Notification.Builder(this)
        }
        return builder
            .setContentTitle("MaxlCommunicator")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setOngoing(true)
            .build()
    }

    private fun update() {
        val manager = getSystemService(NotificationManager::class.java) ?: return
        val text = when {
            state.lastError != null -> state.lastError!!
            state.connected -> "Connected, ${state.heldOnPhone} events here"
            state.lastSyncUnix > 0 -> "Disconnected, ${state.heldOnPhone} events here"
            else -> "Looking for a node"
        }
        manager.notify(NOTIFICATION_ID, notification(text))
    }

    /**
     * What the UI shows.
     *
     * Deliberately a plain object rather than a bound service or a flow: the
     * disconnected view is the main view (gate 6.9's rule, carried over from the
     * PWA), and it must be drawable with nothing running at all.
     */
    data class State(
        val connected: Boolean = false,
        val lastSyncUnix: Long = 0,
        val lastFetched: Int = 0,
        val heldOnPhone: Int = 0,
        val lastError: String? = null,
    )

    companion object {
        @Volatile
        var state: State = State()
            private set

        private const val CHANNEL_ID = "maxl-sync"
        private const val NOTIFICATION_ID = 1
        private const val CONNECT_TIMEOUT_MS = 15_000L
    }
}

/**
 * No server configured yet.
 *
 * Returning false is the honest placeholder: EventStore keeps everything as
 * unpushed, and nothing is acknowledged to the device. A stub that returned
 * true would make the phone forget events it never sent anywhere. Since
 * 2026-09-01 the real client exists (sync/HttpServerPush.kt, wired below once
 * base URL and token are stored); this object remains only the unconfigured
 * state.
 */
private object NoServerYet : ServerPush {
    override fun push(nodeId: Int, events: List<EventStore.Stored>): Boolean = false
}

/**
 * The server pairing, read from preferences.
 *
 * There is no settings screen yet -- entering the base URL and the per-node
 * bearer token needs a phone in hand and belongs to the phase 9 bench session.
 * Until both are stored, the honest stub above keeps everything held on the
 * phone, exactly as before.
 */
private fun Service.serverPush(): ServerPush {
    val prefs = getSharedPreferences("maxl", Context.MODE_PRIVATE)
    val baseUrl = prefs.getString("serverBaseUrl", null)
    val token = prefs.getString("serverToken", null)
    return if (baseUrl.isNullOrBlank() || token.isNullOrBlank()) {
        NoServerYet
    } else {
        io.github.juice_de_orange.maxlcommunicator.sync.HttpServerPush(baseUrl.trimEnd('/'), token)
    }
}
