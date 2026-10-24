package io.github.miklergm.convoy

import kotlin.math.abs
import kotlin.math.roundToLong

/**
 * A board's clock against this phone's, both monotonic, from CLOCK exchanges. The phone notes when
 * it asked (t1) and when the answer came (t4); the board, when the request came (t2) and when it
 * made the answer (t3). An exchange gives the board's clock less the phone's,
 * ((t2 - t1) + (t3 - t4)) / 2, true to within half its round trip less the board's own time,
 * ((t4 - t1) - (t3 - t2)) / 2: a slow exchange is a loose one, and the quickest is believed. The two
 * clocks drift apart as well, by up to [DRIFT] between them, so an exchange's error grows with its
 * age, and a fresh one wins over a quicker one long past.
 *
 * Each run of the board (its boot) is reckoned apart: a board restarted counts from 0 again, and
 * its new run's reckoning never moves the moments of the run before. Exchanges come on the link's
 * thread, the map asks on another.
 *
 * A board with GNSS time says it in its answer, and that is the calendar the riders' ages are told
 * by, carried on this phone's monotonic clock: a phone's own clock may be seconds off, and a fix
 * would then look stale, or come from the future.
 */
class BoardClock {
    /** An exchange's reckoning: the board's clock less the phone's, ms, within errMs, as of phone time atMs. */
    data class Reckoning(val offsetMs: Double, val errMs: Double, val atMs: Long)

    /** The board's GNSS time less this phone's monotonic clock, ms, from its latest answer; null if that had none. */
    @Volatile private var gnssLessMono: Double? = null

    private val runs = object : LinkedHashMap<Long, ArrayDeque<Reckoning>>() {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<Long, ArrayDeque<Reckoning>>) = size > RUNS
    }

    /** One exchange: t1 and t4 on this phone's monotonic clock in ms, t2 and t3 on the board's in µs, and its GNSS time at t3. */
    @Synchronized
    fun observe(boot: Long, t1Ms: Long, t2Us: Long, t3Us: Long, t4Ms: Long, utcMs: Long? = null): Reckoning {
        val t2 = t2Us / 1000.0
        val t3 = t3Us / 1000.0
        val r = Reckoning(((t2 - t1Ms) + (t3 - t4Ms)) / 2, ((t4Ms - t1Ms) - (t3 - t2)).coerceAtLeast(0.0) / 2, t4Ms)
        val kept = runs.getOrPut(boot) { ArrayDeque() }
        kept.addLast(r)
        if (kept.size > EXCHANGES) kept.removeFirst()
        gnssLessMono = utcMs?.let { it - (t3 - reckoning(boot, t4Ms)!!.offsetMs) }
        return r
    }

    /** A run's best reckoning as of [nowMs]: the least error, each grown by the drift since. Null before any. */
    @Synchronized
    fun reckoning(boot: Long, nowMs: Long): Reckoning? =
        runs[boot]?.map { it.copy(errMs = it.errMs + abs(nowMs - it.atMs) * DRIFT) }?.minByOrNull { it.errMs }

    /** A moment of a run on the board's clock, in ms, on this phone's monotonic clock; null before that run's first exchange. */
    fun phoneMs(boot: Long, boardMs: Long, nowMs: Long): Long? =
        reckoning(boot, nowMs)?.let { (boardMs - it.offsetMs).roundToLong() }

    /** Our calendar at [nowMono]: the board's GNSS time where it has one, else this phone's own, [nowWall]. */
    fun calendarMs(nowMono: Long, nowWall: Long): Long = gnssLessMono?.let { nowMono + it.roundToLong() } ?: nowWall

    companion object {
        /** Two crystals of 50 ppm each. */
        const val DRIFT = 100e-6
        /** Kept a run: at one every 10 s, the last five minutes. */
        private const val EXCHANGES = 32
        private const val RUNS = 4
    }
}
