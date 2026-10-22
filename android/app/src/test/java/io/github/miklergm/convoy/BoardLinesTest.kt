package io.github.miklergm.convoy

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class BoardLinesTest {

    private val now = 1_800_000_000_000L

    @Test
    fun `a rider line as the firmware prints it`() {
        val line = "RIDER slot=2 lat=47.1415722 lon=9.5280433 fix_age=3 heard=1.5 speed=40 course=172 " +
            "gps=1 help=0 stopped=0 rssi=-90 snr=5.00 rx=12/14"
        val r = BoardLines.rider(line, now)!!
        assertEquals("slot2", r.id)
        assertEquals("Rider 3", r.name)
        assertEquals(47.1415722, r.lat, 0.0)
        assertEquals(9.5280433, r.lon, 0.0)
        assertEquals(now - 3_000, r.fixAtMs)
        assertEquals(now - 1_500, r.heardAtMs)
        assertEquals(40, r.speedKmh)
        assertEquals(172, r.courseDeg)
        assertTrue(r.gpsOk)
        assertFalse(r.help || r.stopped)
    }

    @Test
    fun `what the board does not know stays unknown, and a wrong clock stays wrong`() {
        val r = BoardLines.rider("RIDER slot=3 lat=-0.5 lon=-120 fix_age=? heard=0.2 speed=? course=? gps=0 help=1 stopped=1", now)!!
        assertNull(r.fixAtMs)
        assertNull(r.speedKmh)
        assertNull(r.courseDeg)
        assertTrue(r.help && r.stopped && !r.gpsOk)
        val future = BoardLines.rider("RIDER slot=3 lat=1 lon=2 fix_age=-18 heard=0.0 speed=0 course=0 gps=1", now)!!
        assertEquals(now + 18_000, future.fixAtMs)
    }

    @Test
    fun `not a rider, for other words or missing or impossible values`() {
        assertNull(BoardLines.rider("RIDERS 3", now))
        assertNull(BoardLines.rider("RIDER slot=1 none", now))
        assertNull(BoardLines.rider("RIDER slot=1 lat=95 lon=2 heard=0", now))
        assertNull(BoardLines.rider("RIDER slot=x lat=1 lon=2 heard=0", now))
        assertNull(BoardLines.rider("TX counter=1 len=17 ok", now))
    }

    @Test
    fun `the status line, and its version`() {
        val s = BoardLines.status("STATUS proto=2 board=sim reset=poweron up=0 slot=0 keyed=1 group=86 fix=1 " +
            "lat=47.1436949 lon=9.5215000 sats=9 hdop=0.9 time=1 tx=0 skipped=0 duty=0.43% riders=2 bad=0 " +
            "foreign=0 forged=0 replay=0 clash=0 power=3 dbm=16")!!
        assertEquals(2, s.proto)
        assertEquals("sim", s.board)
        assertEquals(0, s.slot)
        assertTrue(s.keyed)
        assertEquals(0x86, s.group)
        assertTrue(s.fix)
        assertEquals(2, s.riders)
        assertEquals("0.43%", s.duty)
        assertEquals(16, s.dbm)
        // without a version it is not a status this app can trust
        assertNull(BoardLines.status("STATUS slot=0 keyed=1 group=86"))
        assertEquals(3, BoardLines.status("STATUS proto=3 slot=0 group=7")!!.proto)
        // a board without a key
        assertFalse(BoardLines.status("STATUS proto=2 slot=4 keyed=0 group=00")!!.keyed)
    }

    @Test
    fun `lines from pieces, and an overlong one dropped to its end`() {
        val a = LineAssembler(max = 16)
        assertEquals(emptyList<String>(), a.feed("RIDER sl".toByteArray()))
        assertEquals(listOf("RIDER slot=1"), a.feed("ot=1\r\nST".toByteArray()))
        assertEquals(listOf("STATUS"), a.feed("ATUS\n".toByteArray()))
        // 20 characters: dropped whole, its tail never taken for a line of its own
        assertEquals(listOf("OK"), a.feed("01234567890123456789\nOK\n".toByteArray()))
        assertEquals(listOf("a", "b"), a.feed("\n\na\n\nb\n".toByteArray()))
    }
}
