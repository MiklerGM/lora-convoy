package io.github.miklergm.convoy

import org.junit.Assert.assertEquals
import org.junit.Test

class BoardRidersTest {

    private fun rider(slot: Int, help: Boolean = false) = Rider("slot$slot", "Rider ${slot + 1}", "${slot + 1}", 0,
        47.0, 9.0, fixAtMs = 0, heardAtMs = 0, speedKmh = null, courseDeg = null, help = help)

    @Test
    fun `riders the board no longer knows leave with its status, whatever lines were lost`() {
        val riders = BoardRiders()
        // the group before: two riders, one calling for help
        riders.take(rider(1), 0)
        riders.take(rider(2, help = true), 0)
        // another group set up: its table asked for lost its end, and one rider's line came
        riders.take(rider(3), 9_000)
        riders.known(setOf("slot3"), 10_000)
        assertEquals(setOf("slot3"), riders.all.map { it.id }.toSet())
    }

    @Test
    fun `a rider whose line has just come stays, as a status may be older than the line`() {
        val riders = BoardRiders()
        riders.take(rider(4), 10_000)
        riders.known(emptySet(), 11_000)
        assertEquals(1, riders.all.size)
        riders.known(emptySet(), 20_000)
        assertEquals(0, riders.all.size)
    }
}
