package io.github.miklergm.convoy

import org.junit.Assert.assertEquals
import org.junit.Test

class BoardRidersTest {

    private fun rider(slot: Int, help: Boolean = false) = Rider("slot$slot", "Rider ${slot + 1}", "${slot + 1}", 0,
        47.0, 9.0, fixAtMs = 0, heardAtMs = 0, speedKmh = null, courseDeg = null, help = help)

    @Test
    fun `the board's table takes the place of ours, a group's riders going with their group`() {
        val riders = BoardRiders()
        // the group before: two riders, one calling for help
        riders.take(rider(1))
        riders.take(rider(2, help = true))
        // another group set up, and its table asked: one rider so far, and one heard meanwhile
        riders.asked()
        riders.take(rider(3))
        riders.take(rider(4))
        assertEquals(4, riders.all.size)
        riders.ended()
        assertEquals(setOf("slot3", "slot4"), riders.all.map { it.id }.toSet())
        // and lines after it are taken as they come
        riders.take(rider(5))
        riders.ended()
        assertEquals(3, riders.all.size)
    }
}
