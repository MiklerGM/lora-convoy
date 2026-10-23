package io.github.miklergm.convoy

/**
 * The phone API's lines from a board (firmware/src/api.h, docs/PROTOCOL.md): `key=value` tokens
 * after a word. Pure, so the parsing is tested without Android.
 */
object BoardLines {

    /** The version of the phone API this app speaks. A board with another is refused (D3). */
    const val PROTO = 5

    /** The board's own state, from its STATUS line. */
    data class Status(
        val proto: Int,
        val board: String,
        val slot: Int,
        /** Riders in its group: the frame holds a slot for each. */
        val slots: Int?,
        /** Without a group key a board sends nothing, and hears nobody. */
        val keyed: Boolean,
        /** The group's tag, the second byte of its packets; 0 without a key. */
        val group: Int,
        val fix: Boolean,
        val stopped: Boolean,
        /** Our rider's call for help is on, and so many others' boards say they hear it. */
        val help: Boolean,
        val heard: Int,
        /** Where its clock comes from: none, nmea or pps; and how far off it may be, in ms. */
        val time: String,
        val timeErrMs: Double?,
        val riders: Int,
        /** Packets heard in its own slot from another radio: one invitation used twice, as a rule. */
        val clash: Int,
        val duty: String,
        val dbm: Int?,
        val reset: String,
        /** False when the board could not save its setup: switched off, it would forget it. */
        val kept: Boolean,
    )

    /** What a board took from a GROUP: the group's tag, and its slot, slots and interval. */
    data class Setup(val group: Int, val slot: Int, val slots: Int, val intervalMs: Int) {
        fun of(g: Invitation) = group == g.groupTag() && slot == g.slot && slots == g.riders && intervalMs == g.intervalMs
    }

    fun setup(line: String): Setup? {
        if (!line.startsWith("OK group=")) return null
        val t = tokens(line)
        return Setup(
            group = t["group"]?.toIntOrNull(16) ?: return null,
            slot = t["slot"]?.toIntOrNull() ?: return null,
            slots = t["slots"]?.toIntOrNull() ?: return null,
            intervalMs = t["interval"]?.toIntOrNull() ?: return null,
        )
    }

    /** The words after the first, as a map; a token without '=' is ignored. */
    fun tokens(line: String): Map<String, String> =
        line.split(' ').drop(1).mapNotNull { t ->
            val eq = t.indexOf('=')
            if (eq > 0) t.substring(0, eq) to t.substring(eq + 1) else null
        }.toMap()

    fun status(line: String): Status? {
        if (!line.startsWith("STATUS ")) return null
        val t = tokens(line)
        return Status(
            proto = t["proto"]?.toIntOrNull() ?: return null,
            board = t["board"] ?: "?",
            slot = t["slot"]?.toIntOrNull() ?: return null,
            slots = t["slots"]?.toIntOrNull(),
            keyed = t["keyed"] == "1",
            group = t["group"]?.toIntOrNull(16) ?: return null,
            fix = t["fix"] == "1",
            stopped = t["stopped"] == "1",
            help = t["help"] == "1",
            heard = t["heard"]?.toIntOrNull() ?: 0,
            time = t["time"] ?: "?",
            timeErrMs = t["err"]?.toDoubleOrNull(),
            riders = t["riders"]?.toIntOrNull() ?: 0,
            clash = t["clash"]?.toIntOrNull() ?: 0,
            duty = t["duty"] ?: "?",
            dbm = t["dbm"]?.toIntOrNull(),
            reset = t["reset"] ?: "?",
            kept = t["kept"] != "0",
        )
    }

    /**
     * A RIDER line as a [Rider], its ages turned into clock times on this phone's clock at [nowMs],
     * named from [group]'s invitation. Unknown values ("?") stay unknown; a negative fix age (the
     * sender's clock is wrong) stays negative, so the fix shows from the future rather than as fresh.
     */
    fun rider(line: String, nowMs: Long, group: Invitation? = null): Rider? {
        if (!line.startsWith("RIDER ")) return null
        val t = tokens(line)
        val slot = t["slot"]?.toIntOrNull() ?: return null
        val lat = t["lat"]?.toDoubleOrNull() ?: return null
        val lon = t["lon"]?.toDoubleOrNull() ?: return null
        val heardS = t["heard"]?.toDoubleOrNull() ?: return null
        if (lat !in -90.0..90.0 || lon !in -180.0..180.0) return null
        // riders count from 1, as the group screen does; slots from 0
        fun nameOf(s: Int) = group?.riderName(s) ?: "Rider ${s + 1}"
        val name = nameOf(slot)
        return Rider(
            id = "slot$slot",
            name = name,
            // the map's label: a short name whole, a long one cut, an unnamed rider by number
            short = if (name == "Rider ${slot + 1}") "${slot + 1}" else name.codePoints().limit(8).toArray().let { String(it, 0, it.size) },
            color = PALETTE[slot % PALETTE.size],
            lat = lat,
            lon = lon,
            fixAtMs = t["fix_age"]?.toLongOrNull()?.let { nowMs - it * 1000 },
            heardAtMs = nowMs - (heardS * 1000).toLong(),
            speedKmh = t["speed"]?.toIntOrNull(),
            courseDeg = t["course"]?.toIntOrNull(),
            help = t["help"] == "1",
            stopped = t["stopped"] == "1",
            gpsOk = t["gps"] == "1",
            answeredBy = t["answered"]?.split(',')?.mapNotNull { it.toIntOrNull() }?.map(::nameOf) ?: emptyList(),
        )
    }

    /** One colour per slot; red and grey are taken. */
    private val PALETTE = intArrayOf(
        0xFF1E88E5.toInt(), 0xFF43A047.toInt(), 0xFFFB8C00.toInt(), 0xFF8E24AA.toInt(),
        0xFF00897B.toInt(), 0xFF6D4C41.toInt(), 0xFF3949AB.toInt(), 0xFFC0CA33.toInt(),
    )
}
