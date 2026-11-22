package io.github.miklergm.convoy

import org.junit.Assert.assertEquals
import org.junit.Test

/** What the rider reads beside their state: whether the others' boards carry it (MainActivity). */
class CarriedTest {

    private fun status(state: Int, heard: Int, riders: Int, slots: Int) = BoardLines.status(
        "STATUS proto=8 board=sim reset=poweron up=0 slot=0 slots=$slots keyed=1 group=86c91c92 " +
            "fix=1 stopped=0 heard=$heard state=$state echo=3 send=3/10/1 sats=9 hdop=0.9 time=pps err=0.1 " +
            "budget=233.0 lag=112/98/131 tx=0 late=0 unsure=0 skipped=0 duty=0.43% riders=$riders bad=0 foreign=0 " +
            "forged=0 replay=0 clash=0 power=3 dbm=16")!!

    @Test
    fun `of all the others in the group, not of those the board knows`() {
        // a group of ten, the board knowing one rider, who carries the call: one of nine, not all
        assertEquals("1 of 9", carried(STATE_HELP, status(STATE_HELP, heard = 1, riders = 1, slots = 10), true))
        assertEquals("✓ all 9", carried(STATE_HELP, status(STATE_HELP, heard = 9, riders = 9, slots = 10), true))
        assertEquals("no one to tell", carried(STATE_NORMAL, status(STATE_NORMAL, heard = 0, riders = 0, slots = 1), true))
    }

    @Test
    fun `a choice the board has yet to take says whether it can`() {
        val riding = status(STATE_NORMAL, heard = 2, riders = 2, slots = 3)
        assertEquals("telling the board…", carried(STATE_HELP, riding, linked = true))
        assertEquals("not on the board yet: it is out of reach, and gets it when back", carried(STATE_HELP, riding, linked = false))
    }
}
