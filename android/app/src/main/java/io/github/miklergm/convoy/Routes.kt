package io.github.miklergm.convoy

import android.content.ContentProvider
import android.content.ContentValues
import android.content.Context
import android.database.Cursor
import android.database.MatrixCursor
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.OpenableColumns
import java.io.File
import java.io.FileNotFoundException

/**
 * Routes the app has received, each kept under the SHA-256 of its bytes: the address a route will
 * have over LoRa and on the hub (06, Geometry and intent). What happened to each is three separate
 * times, since OsmAnd accepting a request is not the same as the rider following the route (05).
 */
class Routes(private val context: Context) {

    enum class Step { RECEIVED, IMPORTED, NAVIGATING }

    private val dir = File(context.filesDir, DIR).apply { mkdirs() }
    private val prefs = context.getSharedPreferences("routes", Context.MODE_PRIVATE)

    /** Keeps the bytes, unless a route with the same hash is kept already, and makes it the last. */
    fun put(bytes: ByteArray, summary: GpxSummary, nowMs: Long): String {
        val sha = Gpx.sha256(bytes)
        val file = File(dir, "$sha.gpx")
        if (!file.exists()) {
            val tmp = File(dir, "$sha.part")
            tmp.writeBytes(bytes)
            check(tmp.renameTo(file)) { "cannot keep the route" }
        }
        prefs.edit()
            .putString("last", sha)
            .putString("$sha.name", summary.name)
            .putString("$sha.summary", describe(summary))
            .putLong("$sha.${Step.RECEIVED}", nowMs)
            .apply()
        return sha
    }

    fun last(): String? = prefs.getString("last", null)?.takeIf { File(dir, "$it.gpx").exists() }
    fun name(sha: String): String? = prefs.getString("$sha.name", null)
    fun summary(sha: String): String = prefs.getString("$sha.summary", "") ?: ""
    fun at(sha: String, step: Step): Long? = prefs.getLong("$sha.$step", 0L).takeIf { it > 0 }
    fun mark(sha: String, step: Step, nowMs: Long) = prefs.edit().putLong("$sha.$step", nowMs).apply()

    /** What OsmAnd reads the route through: our own provider, granted to OsmAnd's package only. */
    fun uri(sha: String): Uri = Uri.parse("content://${RouteProvider.AUTHORITY}/$sha.gpx")

    companion object {
        const val DIR = "routes"

        fun describe(s: GpxSummary): String = buildList {
            if (s.trackPoints > 0) add("${s.trackPoints} track points")
            if (s.routePoints > 0) add("${s.routePoints} route points")
            if (s.waypoints > 0) add("${s.waypoints} waypoints")
            add("%.1f km".format(s.lengthM / 1000))
        }.joinToString(", ")
    }
}

/**
 * Serves kept routes read-only to apps this one grants them to, OsmAnd in practice. A provider of
 * our own rather than AndroidX's FileProvider: a few lines, and no dependency.
 */
class RouteProvider : ContentProvider() {

    override fun onCreate() = true

    private fun file(uri: Uri): File {
        val name = uri.lastPathSegment
        if (name == null || !NAME.matches(name)) throw FileNotFoundException(uri.toString())
        val file = File(File(context!!.filesDir, Routes.DIR), name)
        if (!file.exists()) throw FileNotFoundException(uri.toString())
        return file
    }

    override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor {
        if (mode != "r") throw FileNotFoundException("read only")
        return ParcelFileDescriptor.open(file(uri), ParcelFileDescriptor.MODE_READ_ONLY)
    }

    override fun query(uri: Uri, projection: Array<out String>?, selection: String?,
                       selectionArgs: Array<out String>?, sortOrder: String?): Cursor {
        val f = file(uri)
        return MatrixCursor(arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE)).apply {
            addRow(arrayOf<Any>(f.name, f.length()))
        }
    }

    override fun getType(uri: Uri) = "application/gpx+xml"
    override fun insert(uri: Uri, values: ContentValues?): Uri? = throw UnsupportedOperationException()
    override fun update(uri: Uri, values: ContentValues?, selection: String?, selectionArgs: Array<out String>?) =
        throw UnsupportedOperationException()
    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?) =
        throw UnsupportedOperationException()

    companion object {
        const val AUTHORITY = "io.github.miklergm.convoy.routes"
        private val NAME = Regex("[0-9a-f]{64}\\.gpx")
    }
}
