package io.github.miklergm.convoy

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import android.provider.Settings
import android.text.InputType
import android.view.Menu
import android.view.MenuItem
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Spinner
import android.widget.TextView
import android.widget.Toast

/**
 * The main screen: the board in two lines, our state and how many riders have it, the riders, and
 * the two buttons wanted on a ride; everything else in the menu, the board's details in its settings.
 */
class MainActivity : Activity() {

    private val handler = Handler(Looper.getMainLooper())
    private lateinit var status: TextView
    private lateinit var picker: Spinner
    private lateinit var carried: TextView
    private lateinit var riders: TextView
    private lateinit var found: LinearLayout
    private lateinit var batteryButton: Button
    private lateinit var runButton: Button
    private var scanner: BleBoardLink.Scanner? = null
    /** The board's state the picker was last set to, and what it shows now, set by us or the rider. */
    private var shownState: Int? = null
    private var picked: Int? = null

    private val actions by lazy {
        linkedMapOf<String, () -> Unit>(
            "Board settings" to { startActivity(Intent(this, BoardActivity::class.java)) },
            "Group" to { startActivity(Intent(this, GroupActivity::class.java)) },
            "Share log" to ::shareLog,
            "Forget this board" to ::forget,
            "Mock riders" to { start(ConvoyService.Source.Mock) },
            "Simulated board…" to ::askSimulated,
        )
    }

    private val refresh = object : Runnable {
        override fun run() {
            status.put(statusText())
            val board = ConvoyService.board
            if (picker.isEnabled != (board != null)) picker.isEnabled = board != null
            // only when the board's state changes, so the rider's pick stays while the board takes it
            if (board != null && board.state != shownState && board.state in STATE_NAMES.indices) {
                shownState = board.state
                picked = board.state
                picker.setSelection(board.state)
            }
            carried.put(board?.let(::carriedText) ?: "")
            riders.put(ridersText())
            batteryButton.visibility = if (batteryUnrestricted()) View.GONE else View.VISIBLE
            // Start comes back to the board last chosen; with none, Find boards is the way in
            val running = ConvoyService.state != null
            runButton.put(if (running) "Stop" else "Start")
            runButton.visibility = if (running || ConvoyService.Source.load(this@MainActivity) !is ConvoyService.Source.Mock)
                View.VISIBLE else View.GONE
            handler.postDelayed(this, 1000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = (16 * resources.displayMetrics.density).toInt()
        title = "LoRa Convoy ${packageManager.getPackageInfo(packageName, 0).versionName}"
        status = TextView(this).apply { textSize = 16f; setPadding(0, 0, 0, pad / 2) }
        picker = Spinner(this).apply {
            adapter = ArrayAdapter(this@MainActivity, android.R.layout.simple_spinner_item, STATE_NAMES).apply {
                setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
            }
            onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                    // its first call, before the board's state is shown, and ours to show it, send nothing
                    if (shownState == null || position == picked) return
                    picked = position
                    choose(position)
                }
                override fun onNothingSelected(parent: AdapterView<*>?) {}
            }
        }
        carried = TextView(this).apply { textSize = 16f; setPadding(pad / 2, 0, 0, 0) }
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad / 2, pad, pad)
            addView(status)
            batteryButton = button("Let it run in the background") { askBatteryExemption() }
            addView(batteryButton)
            addView(row(TextView(this@MainActivity).apply { text = "Your state"; textSize = 16f }, picker, carried))
            riders = TextView(this@MainActivity).apply { textSize = 16f; setPadding(0, pad / 2, 0, pad / 2) }
            addView(riders)
            runButton = button("Stop") { startOrStop() }
            addView(row(button("Open OsmAnd") { openOsmAnd() }, button("Find boards") { findBoards() }, runButton))
            found = LinearLayout(this@MainActivity).apply { orientation = LinearLayout.VERTICAL }
            addView(found)
        }
        // Android 15 draws apps edge to edge: this keeps the column clear of the system bars.
        // scrolls: a long list of riders and the boards found push the buttons past the screen's end
        setContentView(FrameLayout(this).apply {
            fitsSystemWindows = true
            addView(ScrollView(this@MainActivity).apply { addView(column) })
        })
    }

    override fun onCreateOptionsMenu(menu: Menu): Boolean {
        actions.keys.forEach { menu.add(it) }
        return true
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean =
        actions[item.title.toString()]?.let { it(); true } ?: super.onOptionsItemSelected(item)

    /** Only a change: each setText lays the row out again, and a screen never still keeps uiautomator waiting. */
    private fun TextView.put(t: String) {
        if (text.toString() != t) text = t
    }

    private fun say(text: String) = Toast.makeText(this, text, Toast.LENGTH_LONG).show()

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
                    found.addView(button(name) {
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

    /** The newest day's log of the board, to another app: a messenger, mail, a file manager. */
    private fun shareLog() {
        val f = BoardLog.newest(java.io.File(filesDir, BoardLog.DIR))
        if (f == null) {
            say("No log yet: it starts with a board, mock or simulated.")
            return
        }
        val send = Intent(Intent.ACTION_SEND).apply {
            type = "text/plain"
            putExtra(Intent.EXTRA_STREAM, Uri.parse("content://${RouteProvider.AUTHORITY}/${f.name}"))
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        // the riders' positions are in it, in clear
        startActivity(Intent.createChooser(send, "${f.name}: with the riders' positions"))
    }

    /** What our rider says, through the board: the others' maps show it, and their boards carry it on. */
    private fun choose(state: Int) {
        if (!ConvoyService.command(this, "STATE $state")) say("No board connected to tell it through.")
    }

    /** Whether the others have our state yet: 1/2, all of them ✓; a rider's board that carries it has it. */
    private fun carriedText(b: BoardLines.Status): String {
        if (picked != b.state) return "telling the board…"
        if (b.riders == 0) return "no one to tell"
        // a board's status comes every 10 s: an age under a minute would only mislead
        return (if (b.heard >= b.riders) "✓" else "${b.heard}/${b.riders}") +
            (b.echoS?.takeIf { it >= 60 }?.let { ", ${age(it * 1000L)} ago" } ?: "")
    }

    private fun ridersText(): String {
        val all = ConvoyService.riders
        if (all.isEmpty()) return "No riders heard yet."
        val now = ConvoyService.ridersAtMs
        return all.sortedBy { it.name }.joinToString("\n") { it.line(now) }
    }

    /**
     * Battery optimisation ends long background work after a few hours, about the length of a ride.
     * Asked directly rather than sending the rider through settings (dmd2-lora-bridge).
     */
    private fun askBatteryExemption() {
        val direct = Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:$packageName"))
        if (runCatching { startActivity(direct) }.isFailure) {
            runCatching { startActivity(Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS)) }
        }
    }

    private fun batteryUnrestricted() =
        runCatching { getSystemService(PowerManager::class.java).isIgnoringBatteryOptimizations(packageName) }
            .getOrDefault(false)

    /** The board drops this phone, and Android's bond goes once it says FORGOTTEN. */
    private fun forget() {
        if (ConvoyService.Source.load(this) !is ConvoyService.Source.Board || ConvoyService.state == null) {
            say("No board connected to forget.")
            return
        }
        startService(Intent(this, ConvoyService::class.java).setAction(ConvoyService.ACTION_FORGET))
    }

    /** host:port, as the simulator prints it; an emulator reaches the computer at 10.0.2.2. */
    private fun askSimulated() {
        val saved = ConvoyService.Source.load(this) as? ConvoyService.Source.Simulated
        val field = EditText(this).apply {
            setText(saved?.let { "${it.host}:${it.port}" } ?: "10.0.2.2:4403")
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI
        }
        AlertDialog.Builder(this).setTitle("Simulated board, host:port").setView(field)
            .setPositiveButton("Connect") { _, _ ->
                val parts = field.text.toString().trim().split(':')
                val port = parts.getOrNull(1)?.toIntOrNull()
                if (parts.size != 2 || parts[0].isEmpty() || port == null || port !in 1..65535) {
                    say("host:port, such as 10.0.2.2:4403")
                } else {
                    start(ConvoyService.Source.Simulated(parts[0], port))
                }
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun startOrStop() {
        if (ConvoyService.state != null) stopService(Intent(this, ConvoyService::class.java))
        else start(ConvoyService.Source.load(this))
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

    /** Who the board rides as, and whether it can: its fix, its time, the riders it knows; then only trouble. */
    private fun statusText(): String {
        // the board does not need the phone to ride, and riders took Stop to silence it
        if (ConvoyService.state == null) return "Stopped: no riders on OsmAnd's map." +
            if (ConvoyService.Source.load(this) is ConvoyService.Source.Board) " The board goes on sending until it is switched off." else ""
        val lines = mutableListOf<String>()
        val b = ConvoyService.board
        val link = ConvoyService.linkState
        if (b == null) {
            lines.add((ConvoyService.sourceText ?: "") + (link?.let { ": $it" } ?: ""))
        } else {
            val mine = GroupActivity.Group.load(this)
            // in this phone's group once the board has said it took it, not by the tag: two keys can share one
            val inMine = mine != null && b.keyed && ConvoyService.confirmed == mine
            val slot = "rider ${b.slot + 1}" + (b.slots?.let { " of $it" } ?: "")
            lines.add(when {
                !b.keyed -> "The board has no group, so it sends nothing."
                inMine -> "${mine!!.riderName(b.slot)} in ${mine.name} · $slot"
                mine != null -> "The board is in another group, not ${mine.name} yet"
                else -> "In group %08x · $slot".format(b.group)
            })
            val time = if (b.time in setOf("pps", "group", "nmea")) "time ±" + (b.timeErrMs?.let { "%.0f".format(it) } ?: "?") + " ms"
                else "no time yet"
            lines.add((if (b.fix) "GPS" + (b.sats?.let { " $it sat" } ?: "") else "No GPS fix") +
                " · $time · ${b.riders} riders known")
            if (link != null && !link.startsWith("connected")) lines.add("Link: $link")
            if (!b.kept) lines.add("The board could not save its setup: switched off, it will forget it.")
        }
        when (ConvoyService.state) {
            OsmAnd.State.MISSING -> lines.add("OsmAnd is not installed. Install OsmAnd, OsmAnd+ or OsmAnd~.")
            OsmAnd.State.CONNECTING -> lines.add("Connecting to OsmAnd…")
            OsmAnd.State.REFUSED -> lines.add("OsmAnd has not let us in yet. In OsmAnd, open Menu → Plugins and " +
                "switch LoRa Convoy on.")
            else -> {}
        }
        if (ConvoyService.clashAtMs != 0L && SystemClock.elapsedRealtime() - ConvoyService.clashAtMs < 60_000) {
            lines.add("Another board is sending in your slot, so the others see neither: has one invitation " +
                "been used twice? Each rider needs their own.")
        }
        return lines.joinToString("\n")
    }
}

fun Activity.button(label: String, onClick: () -> Unit) = Button(this).apply {
    text = label
    setOnClickListener { onClick() }
}

/** Side by side; the views after a text share the width left. */
fun Activity.row(vararg views: View) = LinearLayout(this).apply {
    gravity = android.view.Gravity.CENTER_VERTICAL
    views.forEach {
        if (it is Button || it is Spinner) addView(it, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        else addView(it)
    }
}
