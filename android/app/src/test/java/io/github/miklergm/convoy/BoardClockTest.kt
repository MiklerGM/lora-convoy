package io.github.miklergm.convoy

import org.junit.Assert.assertEquals
import org.junit.Test

class BoardClockTest {

    @Test
    fun `a line that waited on its way carries ages from when it was made, not from when it came`() {
        val clock = BoardClock()
        // the board's clock 2 s behind the phone's, and a status that came in 50 ms
        clock.observe(atMs = 1_000_000, receivedMs = 1_002_050)
        // a rider's line made at 1 010 000 that waited 5 s in a queue, its fix 29 s old
        val line = "RIDER slot=2 group=86c91c92 at=1010000 lat=47.1 lon=9.5 fix_age=29 heard=0.4 speed=40 course=0 gps=1"
        val arrived = 1_017_050L
        clock.observe(BoardLines.madeAt(line)!!, arrived)
        val made = clock.madeAt(BoardLines.madeAt(line)!!) ?: arrived
        val r = BoardLines.rider(line, made)!!
        assertEquals(34_000L, arrived - r.fixAtMs!!)
        assertEquals(5_400L, arrived - r.heardAtMs)
    }

    @Test
    fun `the clocks are reckoned from the last minute's quickest lines`() {
        val clock = BoardClock(windowMs = 60_000)
        clock.observe(atMs = 0, receivedMs = 3_000)
        // a minute and more on, the phone's clock set 1 s back: the old quickest line no longer counts
        clock.observe(atMs = 70_000, receivedMs = 72_100)
        assertEquals(82_100L, clock.madeAt(80_000))
    }
}
