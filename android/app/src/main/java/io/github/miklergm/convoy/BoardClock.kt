package io.github.miklergm.convoy

/**
 * A board's clock against this phone's. A line says when the board made it (at=, its UTC from
 * GNSS) and comes some while later, after a queue and the air. The quickest lines of the last
 * minute give the two clocks' difference, so any line is placed when it was made and the ages it
 * carries are reckoned from then, not from when it came: a line that waited 5 s shows its fix 5 s
 * older (ARCHITECTURE D16). A wait every line shares for a whole minute would look like the
 * clocks' difference, and stay unseen.
 *
 * Lines come on the link's thread only.
 */
class BoardClock(private val windowMs: Long = 60_000) {
    /** Of each line of the window: when it came, on this phone's clock, and that less its at=. */
    private val seen = ArrayDeque<LongArray>()

    fun observe(atMs: Long, receivedMs: Long) {
        seen.addLast(longArrayOf(receivedMs, receivedMs - atMs))
        while (seen.first()[0] < receivedMs - windowMs) seen.removeFirst()
    }

    /** When the board made a line it says it made at [atMs], on this phone's clock; null before any line. */
    fun madeAt(atMs: Long): Long? = seen.minOfOrNull { it[1] }?.let { atMs + it }
}
