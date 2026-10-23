package io.github.miklergm.convoy

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView

/** The main screen: where the riders come from, what OsmAnd says, and the board's own state. */
class MainActivity : Activity() {

    private val handler = Handler(Looper.getMainLooper())
    private lateinit var status: TextView
    private lateinit var target: EditText
    private lateinit var found: LinearLayout
    private lateinit var helpButton: Button
    private var scanner: BleBoardLink.Scanner? = null

    private val refresh = object : Runnable {
        override fun run() {
            status.text = statusText()
            helpButton.text = if (ConvoyService.board?.help == true) "Call off help" else "Need help"
            handler.postDelayed(this, 1000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = (16 * resources.displayMetrics.density).toInt()
        val version = packageManager.getPackageInfo(packageName, 0).versionName
        status = TextView(this).apply { textSize = 16f; setPadding(0, pad, 0, pad) }
        val saved = ConvoyService.Source.load(this) as? ConvoyService.Source.Simulated
        target = EditText(this).apply {
            setText(saved?.let { "${it.host}:${it.port}" } ?: "10.0.2.2:4403")
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI
            hint = "host:port of a simulated board"
        }
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
            addView(TextView(this@MainActivity).apply { text = "LoRa Convoy $version"; textSize = 20f })
            addView(status)
            helpButton = button("Need help") { help() }
            addView(helpButton)
            addView(button("Group") { startActivity(Intent(this@MainActivity, GroupActivity::class.java)) })
            addView(button("Mock riders") { start(ConvoyService.Source.Mock) })
            addView(target)
            addView(button("Simulated board") { simulated()?.let(::start) })
            addView(button("Find boards") { findBoards() })
            found = LinearLayout(this@MainActivity).apply { orientation = LinearLayout.VERTICAL }
            addView(found)
            addView(button("Forget this board") { forget() })
            addView(button("Stop") { stopService(Intent(this@MainActivity, ConvoyService::class.java)) })
            addView(button("Open OsmAnd") { openOsmAnd() })
            addView(button("Last route") { startActivity(Intent(this@MainActivity, RouteActivity::class.java)) })
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
        scanner?.stop()
        scanner = null
        super.onPause()
    }

    /** Boards in range, as buttons; a tap makes that board the source. Scans for 15 s. */
    private fun findBoards() {
        val needed = arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        if (needed.any { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }) {
            requestPermissions(needed, 2)
            return
        }
        found.removeAllViews()
        val s = BleBoardLink.Scanner(this)
        if (!s.start { address, name ->
                runOnUiThread {
                    found.addView(button("$name ($address)") {
                        s.stop()
                        found.removeAllViews()
                        start(ConvoyService.Source.Board(address, name))
                    })
                }
            }) {
            found.addView(TextView(this).apply { text = "Bluetooth is off." })
            return
        }
        scanner = s
        found.addView(TextView(this).apply { text = "Looking for boards…" })
        handler.postDelayed({ s.stop() }, 15_000)
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == 2 && grantResults.isNotEmpty() && grantResults.all { it == PackageManager.PERMISSION_GRANTED }) {
            findBoards()
        }
    }

    /** Our rider's call for help, through the board: the others' maps show it, and their boards answer. */
    private fun help() {
        val board = ConvoyService.board
        if (board == null || ConvoyService.state == null) {
            status.text = "No board connected to call for help through."
            return
        }
        startService(Intent(this, ConvoyService::class.java).setAction(ConvoyService.ACTION_HELP)
            .putExtra(ConvoyService.EXTRA_ON, !board.help))
    }

    /** The board drops this phone, and Android's bond goes once it says FORGOTTEN. */
    private fun forget() {
        if (ConvoyService.Source.load(this) !is ConvoyService.Source.Board || ConvoyService.state == null) {
            status.text = "No board connected to forget."
            return
        }
        startService(Intent(this, ConvoyService::class.java).setAction(ConvoyService.ACTION_FORGET))
    }

    /** host:port, as the simulator prints it; an emulator reaches the computer at 10.0.2.2. */
    private fun simulated(): ConvoyService.Source.Simulated? {
        val parts = target.text.toString().trim().split(':')
        val port = parts.getOrNull(1)?.toIntOrNull()
        if (parts.size != 2 || parts[0].isEmpty() || port == null || port !in 1..65535) {
            target.error = "host:port, such as 10.0.2.2:4403"
            return null
        }
        return ConvoyService.Source.Simulated(parts[0], port)
    }

    /** Switching source restarts the service, so nothing of the old source lingers on the map. */
    private fun start(source: ConvoyService.Source) {
        // Android 13 and later. Without it the service still runs; only its notification stays hidden.
        if (Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 1)
        }
        ConvoyService.Source.save(this, source)
        stopService(Intent(this, ConvoyService::class.java))
        startForegroundService(Intent(this, ConvoyService::class.java))
    }

    private fun openOsmAnd() {
        val pkg = OsmAnd(this).installed() ?: return
        packageManager.getLaunchIntentForPackage(pkg)?.let(::startActivity)
    }

    private fun statusText(): String {
        val osmand = when (ConvoyService.state) {
            null -> return "Stopped."
            OsmAnd.State.MISSING -> "OsmAnd is not installed. Install OsmAnd, OsmAnd+ or OsmAnd~."
            OsmAnd.State.CONNECTING -> "Connecting to OsmAnd…"
            OsmAnd.State.REFUSED -> "OsmAnd has not let us in yet. In OsmAnd, open Menu → Plugins and " +
                "switch LoRa Convoy on."
            OsmAnd.State.SHOWING -> "Riders on the map of ${ConvoyService.version ?: "OsmAnd"}."
        }
        val lines = mutableListOf(ConvoyService.sourceText ?: "", osmand)
        ConvoyService.linkState?.let { lines.add("Link: $it") }
        ConvoyService.board?.takeIf { it.help }?.let {
            lines.add(if (it.heard == 0) "You are calling for help; no one has answered yet."
                else "You are calling for help; ${it.heard} riders' boards have answered.")
        }
        ConvoyService.board?.let {
            val mine = GroupActivity.Group.load(this)
            val tag = "%02x".format(it.group)
            val group = when {
                !it.keyed -> "no group key, so it sends nothing"
                mine != null && it.group == mine.groupTag() -> "in ${mine.name}"
                mine != null -> "in group $tag, not ${mine.name} yet"
                else -> "in group $tag"
            }
            val clock = when (it.time) {
                "pps" -> "time from PPS"
                "nmea" -> "time from NMEA"
                else -> "no time yet"
            } + (it.timeErrMs?.let { e -> " ±$e ms" } ?: "")
            lines.add("Board: ${it.board}, rider ${it.slot + 1}${it.slots?.let { n -> " of $n" } ?: ""}, $group, " +
                (if (it.fix) "GNSS fix" else "no GNSS fix") + ", $clock, ${it.riders} riders heard, " +
                "${it.dbm ?: "?"} dBm, duty ${it.duty}, started after ${it.reset}")
        }
        return lines.joinToString("\n")
    }
}

fun Activity.button(label: String, onClick: () -> Unit) = Button(this).apply {
    text = label
    setOnClickListener { onClick() }
}
