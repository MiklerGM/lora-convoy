package io.github.miklergm.convoy

import android.util.Log
import java.io.File
import java.io.FileWriter
import java.io.IOException
import java.io.Writer
import java.time.LocalDate
import java.time.LocalDateTime
import java.time.format.DateTimeFormatter

/**
 * Everything to and from the board, a day to a file, as lab/con.py writes the cable's: each line
 * after this phone's time, "> " before what was sent, "# " before what happened to the link; so
 * the bench's scripts read either. With LOG (docs/PROTOCOL.md) the board's own console lines come
 * too, as LOG n=.. at=.. <line>. Kept for [KEEP_DAYS] days; Share log hands the newest on.
 * A group's key never goes in: a log is for sharing.
 */
class BoardLog(private val dir: File) {
    private var day: String? = null
    private var out: Writer? = null

    @Synchronized
    fun write(text: String, at: LocalDateTime = LocalDateTime.now()) {
        try {
            val d = at.toLocalDate().toString()
            if (d != day) {
                out?.close()
                dir.mkdirs()
                prune(d)
                // appended: a service started again the same day goes on in the same file
                out = FileWriter(File(dir, "convoy-$d.log"), true).buffered()
                day = d
            }
            out!!.apply {
                write(STAMP.format(at) + " " + KEY.replace(text, "<key>") + "\n")
                flush()
            }
        } catch (e: IOException) {
            Log.w("BoardLog", "log: ${e.message}")
        }
    }

    @Synchronized
    fun close() {
        runCatching { out?.close() }
        out = null
        day = null
    }

    /** Days before [today] by more than KEEP_DAYS go; their names sort as their dates do. */
    private fun prune(today: String) {
        val oldest = "convoy-${LocalDate.parse(today).minusDays(KEEP_DAYS)}.log"
        dir.listFiles()?.filter { NAME.matches(it.name) && it.name < oldest }?.forEach { it.delete() }
    }

    companion object {
        const val DIR = "logs"
        const val KEEP_DAYS = 7L
        val NAME = Regex("convoy-\\d{4}-\\d{2}-\\d{2}\\.log")
        private val STAMP = DateTimeFormatter.ofPattern("HH:mm:ss.SSS")
        /** A key as GROUP and KEY carry it. */
        private val KEY = Regex("\\b[0-9a-fA-F]{32}\\b")

        fun newest(dir: File): File? = dir.listFiles()?.filter { NAME.matches(it.name) }?.maxByOrNull { it.name }
    }
}
