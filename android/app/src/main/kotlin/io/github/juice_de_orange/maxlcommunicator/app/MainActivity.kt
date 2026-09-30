package io.github.juice_de_orange.maxlcommunicator.app

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * The screen, and the rule it is built around.
 *
 * docs/test-results/2026-08-31_gate_6-9_foreground_ux.md, carried over from the
 * PWA: **the disconnected view is the main view.** Last sync and how many events
 * are waiting must be readable before anything is connected, because that is the
 * state the user sees most of the time.
 *
 * Views in code rather than XML layouts. This is four labels and a button; a
 * layout file and a resource id for each of them would be more moving parts than
 * the screen has.
 */
class MainActivity : Activity() {

    private lateinit var connection: TextView
    private lateinit var lastSync: TextView
    private lateinit var held: TextView
    private lateinit var problem: TextView

    private val refresh = Handler(Looper.getMainLooper())
    private val tick = object : Runnable {
        override fun run() {
            render()
            refresh.postDelayed(this, 1_000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(48, 96, 48, 48)
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            )
        }

        connection = TextView(this).apply { textSize = 22f }
        lastSync = TextView(this).apply { textSize = 16f }
        held = TextView(this).apply { textSize = 16f }
        problem = TextView(this).apply { textSize = 14f; gravity = Gravity.START }

        val start = Button(this).apply {
            text = "Sync"
            setOnClickListener { requestPermissionsThenStart() }
        }

        root.addView(connection)
        root.addView(lastSync)
        root.addView(held)
        root.addView(problem)
        root.addView(start)
        setContentView(root)
    }

    override fun onResume() {
        super.onResume()
        refresh.post(tick)
    }

    override fun onPause() {
        refresh.removeCallbacks(tick)
        super.onPause()
    }

    private fun render() {
        val state = SyncService.state

        connection.text = if (state.connected) "Connected" else "Disconnected"
        lastSync.text = if (state.lastSyncUnix == 0L) {
            "Never synced"
        } else {
            val when_ = SimpleDateFormat("HH:mm:ss", Locale.UK)
                .format(Date(state.lastSyncUnix * 1000))
            "Last synced at $when_, ${state.lastFetched} events fetched"
        }
        held.text = "${state.heldOnPhone} events on this phone"
        problem.text = state.lastError ?: ""
    }

    private fun requestPermissionsThenStart() {
        val needed = ArrayList<String>()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            needed.add(Manifest.permission.BLUETOOTH_SCAN)
            needed.add(Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            @Suppress("DEPRECATION")
            needed.add(Manifest.permission.ACCESS_FINE_LOCATION)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            needed.add(Manifest.permission.POST_NOTIFICATIONS)
        }

        val missing = needed.filter {
            checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isNotEmpty()) {
            requestPermissions(missing.toTypedArray(), REQUEST_CODE)
            return
        }
        startService(Intent(this, SyncService::class.java))
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQUEST_CODE && grantResults.all { it == PackageManager.PERMISSION_GRANTED }) {
            startService(Intent(this, SyncService::class.java))
        }
    }

    private companion object {
        const val REQUEST_CODE = 1
    }
}
