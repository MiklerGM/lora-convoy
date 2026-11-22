package io.github.miklergm.convoy

/**
 * The phone API's lines from a board (firmware/src/api.h, docs/PROTOCOL.md): `key=value` tokens
 * after a word. Pure, so the parsing is tested without Android.
 */
object BoardLines {

    /** The version of the phone API this app speaks. A board with another is refused (D3). */
    const val PROTO = 8

    /** The board's own state, from its STATUS line. */
    data class Status(
        val proto: Int,
        val board: String,
        val slot: Int,
        /** Riders in its group: the frame holds a slot for each. */
        val slots: Int?,
        /** Without a group key a board sends nothing, and hears nobody. */
        val keyed: Boolean,
        /** The group's id, from its key, its tag first (Invitation.groupId); 0 without a key. */
        val group: Long,
        val fix: Boolean,
        val stopped: Boolean,
        /** What our rider says (STATE_NAMES), and how many others' boards carry it on, and since when (s). */
        val state: Int,
        val heard: Int,
        val echoS: Int?,
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
        /** The radio mode, SF/kHz; the transmit power's level, 0 the strongest. */
        val mode: String?,
        val power: Int?,
        /** The send rule (send=): a beat every 2^beat frames, a new position after so many metres, riders carried per packet. */
        val beat: Int?,
        val moveM: Int?,
        val passengers: Int?,
        val sats: Int?,
    ) {
        val help get() = state == STATE_HELP
    }

    /** What a board took from a GROUP: the group's tag, and its slot, slots and interval. */
    data class Setup(val group: Long, val slot: Int, val slots: Int, val intervalMs: Int) {
        fun of(g: Invitation) = group == g.groupId() && slot == g.slot && slots == g.riders && intervalMs == g.intervalMs
    }

    fun setup(line: String): Setup? {
        if (!line.startsWith("OK group=")) return null
        val t = tokens(line)
        return Setup(
            group = t["group"]?.toLongOrNull(16) ?: return null,
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

    /** Where a RIDER line's rider was heard: the board's run, the moment on its clock, and the fix's time as the packet gave it. */
    data class Heard(val boot: Long, val rxMs: Long, val fix: Int?)

    fun heard(line: String): Heard? {
        if (!line.startsWith("RIDER ")) return null
        val t = tokens(line)
        return Heard(t["boot"]?.toLongOrNull(16) ?: return null, t["heard_at"]?.toLongOrNull() ?: return null, t["fix"]?.toIntOrNull())
    }

    /**
     * A board's answer to CLOCK: this phone's moment echoed, the board's run, its clock in µs when the
     * request came and when it answered, and its GNSS time then, in ms; null without one.
     */
    data class ClockReply(val t1Ms: Long, val boot: Long, val rxUs: Long, val txUs: Long, val utcMs: Long? = null)

    fun clock(line: String): ClockReply? {
        if (!line.startsWith("CLOCK ")) return null
        val t = tokens(line)
        return ClockReply(t["t1"]?.toLongOrNull() ?: return null, t["boot"]?.toLongOrNull(16) ?: return null,
            t["rx"]?.toLongOrNull() ?: return null, t["tx"]?.toLongOrNull() ?: return null, t["utc"]?.toLongOrNull())
    }

    /** A fix's time, UTC seconds modulo 65 536 as the packet carries it, unwrapped to the nearest second of [aroundMs], as the firmware does. */
    fun fixUtcMs(fix: Int, aroundMs: Long): Long {
        val nowS = Math.floorDiv(aroundMs, 1000L)
        val behind = (nowS - fix) and 0xFFFF
        return (if (behind < 0x8000) nowS - behind else nowS + (0x10000 - behind)) * 1000
    }

    /** A LOG line's number: one of the board's own console lines, numbered for this phone's log. */
    fun logNumber(line: String): Long? = if (line.startsWith("LOG ")) tokens(line)["n"]?.toLongOrNull() else null

    /** The board's answer to LOG: its run, and the numbers of the first line it gives and the next it will have. */
    data class LogStart(val boot: Long, val from: Long, val next: Long)

    fun logStart(line: String): LogStart? {
        if (!line.startsWith("OK log ")) return null
        val t = tokens(line)
        return LogStart(t["boot"]?.toLongOrNull(16) ?: return null, t["from"]?.toLongOrNull() ?: return null,
            t["next"]?.toLongOrNull() ?: return null)
    }

    /** The group a line names (group=): which group's a RIDER line is, as its board heard it. */
    fun groupOf(line: String): Long? = tokens(line)["group"]?.toLongOrNull(16)

    fun status(line: String): Status? {
        if (!line.startsWith("STATUS ")) return null
        val t = tokens(line)
        val send = t["send"]?.split('/')
        return Status(
            proto = t["proto"]?.toIntOrNull() ?: return null,
            board = t["board"] ?: "?",
            slot = t["slot"]?.toIntOrNull() ?: return null,
            slots = t["slots"]?.toIntOrNull(),
            keyed = t["keyed"] == "1",
            group = t["group"]?.toLongOrNull(16) ?: return null,
            fix = t["fix"] == "1",
            stopped = t["stopped"] == "1",
            state = t["state"]?.toIntOrNull() ?: STATE_NORMAL,
            heard = t["heard"]?.toIntOrNull() ?: 0,
            echoS = t["echo"]?.toIntOrNull(),
            time = t["time"] ?: "?",
            timeErrMs = t["err"]?.toDoubleOrNull(),
            riders = t["riders"]?.toIntOrNull() ?: 0,
            clash = t["clash"]?.toIntOrNull() ?: 0,
            duty = t["duty"] ?: "?",
            dbm = t["dbm"]?.toIntOrNull(),
            reset = t["reset"] ?: "?",
            kept = t["kept"] != "0",
            mode = t["mode"],
            power = t["power"]?.toIntOrNull(),
            beat = send?.getOrNull(0)?.toIntOrNull(),
            moveM = send?.getOrNull(1)?.toIntOrNull(),
            passengers = send?.getOrNull(2)?.toIntOrNull(),
            sats = t["sats"]?.toIntOrNull(),
        )
    }

    /**
     * A RIDER line as a [Rider], its ages turned into clock times on this phone's clock from
     * [nowMs], when the board made the line (BoardClock), named from [group]'s invitation. Unknown values ("?") stay unknown; a negative fix age (the
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
            // the fix by its own time, as the packet gave it, on this phone's calendar, which needs no
            // GNSS time on the board and no wait on the way counted; else by the board's age of it
            fixAtMs = t["fix"]?.toIntOrNull()?.let { fixUtcMs(it, nowMs) } ?: t["fix_age"]?.toLongOrNull()?.let { nowMs - it * 1000 },
            heardAtMs = nowMs - (heardS * 1000).toLong(),
            speedKmh = t["speed"]?.toIntOrNull(),
            courseDeg = t["course"]?.toIntOrNull(),
            // a board before v5 says help= only
            state = t["state"]?.toIntOrNull() ?: if (t["help"] == "1") STATE_HELP else STATE_NORMAL,
            stopped = t["stopped"] == "1",
            gpsOk = t["gps"] == "1",
            via = t["via"]?.toIntOrNull()?.let(::nameOf),
            // heard before its first fix (v5 pos=0): in the list with its state, kept off the map
            hasPosition = t["pos"] != "0",
        )
    }

    /** One colour per slot; red and grey are taken. */
    private val PALETTE = intArrayOf(
        0xFF1E88E5.toInt(), 0xFF43A047.toInt(), 0xFFFB8C00.toInt(), 0xFF8E24AA.toInt(),
        0xFF00897B.toInt(), 0xFF6D4C41.toInt(), 0xFF3949AB.toInt(), 0xFFC0CA33.toInt(),
    )
}
