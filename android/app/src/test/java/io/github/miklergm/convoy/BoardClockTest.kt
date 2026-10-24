package io.github.miklergm.convoy

import org.junit.Assert.assertEquals
import org.junit.Test

class BoardClockTest {

    @Test
    fun `a quick exchange grown old gives way to a fresh one a little slower`() {
        val clock = BoardClock()
        // 20 ms either way at the phone's 100 000, the board's clock 40 s behind ...
        clock.observe(boot = 1, t1Ms = 100_000, t2Us = 60_020_000, t3Us = 60_021_000, t4Ms = 100_041)
        // ... and 30 ms either way 300 s on, the two crystals 50 ms apart by then
        clock.observe(boot = 1, t1Ms = 400_000, t2Us = 360_080_000, t3Us = 360_081_000, t4Ms = 400_061)
        // the old one's 20 ms grew by 300 s of drift to 50: the fresh one's 30 is believed
        assertEquals(-39_950.0, clock.reckoning(boot = 1, nowMs = 400_061)!!.offsetMs, 0.5)
    }
}
