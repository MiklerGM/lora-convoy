package io.github.miklergm.convoy

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.concurrent.Executors

/**
 * A route from outside: shared to the app, or opened with it, from a messenger, a file manager, or
 * adb. It is checked, kept under its hash, and handed to OsmAnd only when the rider asks: shown,
 * then navigated along after a confirmation. Opened from the main screen, it shows the last route.
 */
class RouteActivity : Activity() {

    private val worker = Executors.newSingleThreadExecutor()
    private lateinit var routes: Routes
    private lateinit var osmand: OsmAnd
    private lateinit var title: TextView
    private lateinit var info: TextView
    private var sha: String? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        routes = Routes(this)
        osmand = OsmAnd(this).also { it.connect() }
        val pad = (16 * resources.displayMetrics.density).toInt()
        title = TextView(this).apply { textSize = 20f }
        info = TextView(this).apply { textSize = 16f; setPadding(0, pad, 0, pad) }
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
            addView(title)
            addView(info)
            addView(button("Show in OsmAnd") { showInOsmAnd() })
            addView(button("Navigate along it") { confirmNavigate() })
        }
        setContentView(FrameLayout(this).apply { fitsSystemWindows = true; addView(column) })

        val incoming = incoming(intent)
        if (incoming != null) receive(incoming) else show(routes.last())
    }

    override fun onDestroy() {
        worker.shutdown()
        osmand.disconnect()
        super.onDestroy()
    }

    /** "Open with" brings the file as data, "share" as a stream. */
    @Suppress("DEPRECATION") // the typed getParcelableExtra is Android 13 and later
    private fun incoming(intent: Intent): Uri? = when (intent.action) {
        Intent.ACTION_VIEW -> intent.data
        Intent.ACTION_SEND -> intent.getParcelableExtra(Intent.EXTRA_STREAM) as? Uri
        else -> null
    }

    private fun receive(uri: Uri) {
        title.text = "Reading the route…"
        worker.execute {
            val result = try {
                val bytes = contentResolver.openInputStream(uri)?.use { readAtMost(it, Gpx.MAX_BYTES + 1) }
                    ?: throw IllegalArgumentException("nothing to read")
                val summary = Gpx.summarize(bytes)
                Result.success(routes.put(bytes, summary, System.currentTimeMillis()))
            } catch (e: Exception) {
                // a SecurityException too: the sender did not let us read it
                Result.failure(e)
            }
            runOnUiThread {
                result.onSuccess { show(it) }.onFailure {
                    title.text = "Not a route we can use"
                    info.text = it.message ?: it.javaClass.simpleName
                }
            }
        }
    }

    private fun show(sha: String?) {
        this.sha = sha
        if (sha == null) {
            title.text = "No route yet"
            info.text = "Share a GPX to LoRa Convoy, or open one with it."
            return
        }
        title.text = routes.name(sha) ?: "Unnamed route"
        val steps = listOf(
            Routes.Step.RECEIVED to "Received",
            Routes.Step.IMPORTED to "Handed to OsmAnd to show",
            Routes.Step.NAVIGATING to "Handed to OsmAnd to navigate",
        ).mapNotNull { (step, label) -> routes.at(sha, step)?.let { "$label at ${clock(it)}" } }
        info.text = (listOf(routes.summary(sha), "SHA-256 ${sha.take(16)}…") + steps).joinToString("\n")
    }

    private fun showInOsmAnd() {
        val sha = sha ?: return
        handOver(sha, Routes.Step.IMPORTED) { uri ->
            osmand.importGpx(uri, Gpx.fileName(routes.name(sha), sha))
        }
    }

    private fun confirmNavigate() {
        val sha = sha ?: return
        AlertDialog.Builder(this)
            .setTitle("Navigate along ${routes.name(sha) ?: "this route"}?")
            .setMessage("OsmAnd follows the track as it is, without snapping it to roads. If it is " +
                "already navigating, it asks before replacing that route.")
            .setPositiveButton("Navigate") { _, _ ->
                handOver(sha, Routes.Step.NAVIGATING) { uri -> osmand.navigateGpx(uri) }
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    /** Lets OsmAnd read the route, makes the request, and records it if OsmAnd took it. */
    private fun handOver(sha: String, step: Routes.Step, request: (Uri) -> Boolean) {
        worker.execute {
            val outcome = when {
                !osmand.awaitConnected(5_000) -> if (osmand.state == OsmAnd.State.MISSING)
                    "OsmAnd is not installed." else "OsmAnd does not answer."
                else -> {
                    val uri = routes.uri(sha)
                    grantUriPermission(osmand.pkg!!, uri, Intent.FLAG_GRANT_READ_URI_PERMISSION) // set once bound
                    if (request(uri)) {
                        routes.mark(sha, step, System.currentTimeMillis())
                        null
                    } else {
                        "OsmAnd refused. If LoRa Convoy is off in OsmAnd's Menu → Plugins, switch it on."
                    }
                }
            }
            runOnUiThread {
                show(sha)
                if (outcome != null) info.append("\n\n$outcome")
            }
        }
    }

    /** Stops one byte past [limit], so an oversized file is refused without reading it all. */
    private fun readAtMost(input: InputStream, limit: Int): ByteArray {
        val out = ByteArrayOutputStream()
        val buf = ByteArray(64 * 1024)
        while (out.size() < limit) {
            val n = input.read(buf, 0, minOf(buf.size, limit - out.size()))
            if (n < 0) break
            out.write(buf, 0, n)
        }
        return out.toByteArray()
    }

    private fun clock(ms: Long) = CLOCK.format(Instant.ofEpochMilli(ms).atZone(ZoneId.systemDefault()))

    companion object {
        private val CLOCK = DateTimeFormatter.ofPattern("HH:mm:ss")
    }
}
