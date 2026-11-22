package io.github.miklergm.convoy

import org.junit.Assert.assertEquals
import org.junit.Test

/** What the rider reads beside their state: whether the others' boards carry it (MainActivity). */
class CarriedTest {

    private fun status(state: Int, heard: Int, riders: Int, slots: Int, time: String = "time=pps err=0.1 budget=233.0") = BoardLines.status(
        "STATUS proto=8 board=sim reset=poweron up=0 slot=0 slots=$slots keyed=1 group=86c91c92 " +
            "fix=1 stopped=0 heard=$heard state=$state echo=3 send=3/10/1 sats=9 hdop=0.9 $time " +
            "lag=112/98/131 tx=0 late=0 unsure=0 skipped=0 duty=0.43% riders=$riders bad=0 foreign=0 " +
            "forged=0 replay=0 clash=0 power=3 dbm=16")!!

    @Test
    fun `a board whose clock is too rough for its slots says that it is not sending`() {
        // an L1 on NMEA in ten riders every 2 s: 100 ms against 70.6
        val l1 = status(STATE_NORMAL, 0, 9, 10, "time=nmea err=100.0 budget=70.6")
        assertEquals("Not sending your position: the board's clock, ±100 ms, is too rough for this group's slots (±71 ms). " +
            "It sends once its GPS or a board with PPS sets it closer.", pausedText(l1))
        // the same board, its time from a board with PPS: it sends
        assertEquals(null, pausedText(status(STATE_NORMAL, 0, 9, 10, "time=group err=10.4 budget=70.6")))
        assertEquals(null, pausedText(status(STATE_NORMAL, 0, 9, 10)))
    }

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
