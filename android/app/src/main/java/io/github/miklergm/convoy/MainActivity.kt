package io.github.miklergm.convoy

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView

/** One screen until M4: what OsmAnd says, and the mock riders on or off. */
class MainActivity : Activity() {

    private val handler = Handler(Looper.getMainLooper())
    private lateinit var status: TextView

    private val refresh = object : Runnable {
        override fun run() {
            status.text = statusText()
            handler.postDelayed(this, 1000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = (16 * resources.displayMetrics.density).toInt()
        val version = packageManager.getPackageInfo(packageName, 0).versionName
        status = TextView(this).apply { textSize = 16f; setPadding(0, pad, 0, pad) }
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
            addView(TextView(this@MainActivity).apply { text = "LoRa Convoy $version"; textSize = 20f })
            addView(status)
            addView(button("Start mock riders") { start() })
            addView(button("Stop") { stopService(Intent(this@MainActivity, ConvoyService::class.java)) })
            addView(button("Open OsmAnd") { openOsmAnd() })
        }
        // Android 15 draws apps edge to edge: this keeps the column clear of the system bars.
        setContentView(FrameLayout(this).apply { fitsSystemWindows = true; addView(column) })
    }

    override fun onResume() {
        super.onResume()
        handler.post(refresh)
    }

    override fun onPause() {
        handler.removeCallbacks(refresh)
        super.onPause()
    }

    private fun start() {
        // Android 13 and later. Without it the service still runs; only its notification stays hidden.
        if (Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 1)
        }
        startForegroundService(Intent(this, ConvoyService::class.java))
    }

    private fun openOsmAnd() {
        val pkg = OsmAnd(this).installed() ?: return
        packageManager.getLaunchIntentForPackage(pkg)?.let(::startActivity)
    }

    private fun statusText(): String = when (ConvoyService.state) {
        null -> "Stopped."
        OsmAnd.State.MISSING -> "OsmAnd is not installed. Install OsmAnd, OsmAnd+ or OsmAnd~."
        OsmAnd.State.CONNECTING -> "Connecting to OsmAnd…"
        OsmAnd.State.REFUSED -> "OsmAnd has not let us in yet. In OsmAnd, open Menu → Plugins and " +
            "switch LoRa Convoy on."
        OsmAnd.State.SHOWING -> "Riders on the map of ${ConvoyService.version ?: "OsmAnd"}. " +
            "Mock riders circle where OsmAnd's map was centred when they started."
    }

    private fun button(label: String, onClick: () -> Unit) = Button(this).apply {
        text = label
        setOnClickListener { onClick() }
    }
}
