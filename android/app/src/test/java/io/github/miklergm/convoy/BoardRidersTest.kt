package io.github.miklergm.convoy

import org.junit.Assert.assertEquals
import org.junit.Test

class BoardRidersTest {

    private fun rider(slot: Int, help: Boolean = false) = Rider("slot$slot", "Rider ${slot + 1}", "${slot + 1}", 0,
        47.0, 9.0, fixAtMs = 0, heardAtMs = 0, speedKmh = null, courseDeg = null, help = help)

    // the groups of the keys 00 01 .. 0f and 0f 0e 00 ..: their ids, from OpenSSL
    private val before = 0x86c91c92L
    private val after = 0x79e8d53cL

    @Test
    fun `riders of the group before go once the board is in another, a slot taken again among them`() {
        val riders = BoardRiders()
        riders.group(before)
        riders.take(rider(1, help = true), before)
        riders.take(rider(2), before)
        // another group: its rider in slot 1 heard by the board, and that line lost on the way
        riders.group(after)
        assertEquals(emptyList<Rider>(), riders.all)
        // a line of the group before, made before the new key, comes late
        riders.take(rider(2), before)
        assertEquals(emptyList<Rider>(), riders.all)
    }

    @Test
    fun `riders of the board's group stay through its statuses, heard again or not`() {
        val riders = BoardRiders()
        riders.group(before)
        riders.take(rider(1), before)
        riders.group(before)
        riders.take(rider(3), before)
        assertEquals(setOf("slot1", "slot3"), riders.all.map { it.id }.toSet())
    }
}
