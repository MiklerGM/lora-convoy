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
        val line = "RIDER slot=2 group=86c91c92 boot=0000000a at=70100 heard_at=68600 fix=53757 lat=47.1415722 lon=9.5280433 fix_age=3 heard=1.5 speed=40 course=172 " +
            "gps=1 help=0 stopped=0 answered=- rssi=-90 snr=5.00 rx=12/14"
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
        assertNull(r.via)
        assertEquals(0x86c91c92L, BoardLines.groupOf(line))
        // where it was heard, read past the packets' rx= at the end of the line
        assertEquals(BoardLines.Heard(boot = 0xa, rxMs = 68_600, fix = 53_757), BoardLines.heard(line))
        // named from the invitation, the map's label too
        val group = Invitation(ByteArray(16), 0, 3, 2000, "Bench", listOf("Ann", "Bob", "Christopher"))
        val named = BoardLines.rider(line, now, group)!!
        assertEquals("Christopher", named.name)
        assertEquals("Christop", named.short)
        assertEquals("3", r.short)
        // a board before v5: help= alone
        assertTrue(BoardLines.rider(line.replace("help=0", "help=1"), now)!!.help)
    }

    @Test
    fun `a v5 rider line, its state and who told of it`() {
        val line = "RIDER slot=2 group=86c91c92 boot=0000000a at=70100 heard_at=68600 fix=53757 pos=1 lat=47.1415722 " +
            "lon=9.5280433 fix_age=13 heard=1.5 speed=40 course=172 gps=1 help=0 stopped=0 answered=- state=3 via=0 " +
            "direct=- rssi=-90 snr=5.00 rx=12/14 offset=? late=0 sats=9"
        val group = Invitation(ByteArray(16), 0, 3, 2000, "Bench", listOf("Ann", "Bob", "Christopher"))
        val r = BoardLines.rider(line, now, group)!!
        assertEquals(3, r.state)
        assertFalse(r.help)
        assertEquals("Ann", r.via)
        // heard itself: told by no one
        assertNull(BoardLines.rider(line.replace("via=0", "via=-"), now, group)!!.via)
        assertTrue(BoardLines.rider(line.replace("state=3", "state=1"), now)!!.help)
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
    fun `a rider told of before its first fix, its call for help counted and its 0, 0 no place`() {
        val r = BoardLines.rider("RIDER slot=2 fix=? pos=0 lat=0.0000000 lon=0.0000000 fix_age=? heard=1.0 gps=0 state=1 help=1", now)!!
        assertTrue(r.help && !r.hasPosition)
        assertNull(r.fixAtMs)
        // listed with its call, and kept off the map rather than drawn in the Gulf of Guinea
        assertEquals("Rider 3 · Needs help · no position yet, not on the map · heard 1 s ago", r.line(now))
        val placed = r.copy(id = "slot3", hasPosition = true, lat = 47.1, lon = 9.5)
        assertEquals(listOf("slot3"), mapPoints(listOf(r, placed), now).map { it.id })
    }

    @Test
    fun `the status line, and its version`() {
        val s = BoardLines.status("STATUS proto=8 board=sim reset=poweron up=0 slot=0 slots=3 keyed=1 group=86c91c92 " +
            "fix=1 stopped=0 heard=2 state=1 echo=3 send=3/10/1 sats=9 hdop=0.9 time=pps err=0.1 " +
            "budget=233.0 lag=112/98/131 tx=0 late=0 unsure=0 skipped=0 duty=0.43% riders=2 bad=0 foreign=0 " +
            "forged=0 replay=0 clash=0 power=3 dbm=16")!!
        assertEquals(8, s.proto)
        assertTrue(s.help && !s.stopped)
        assertEquals(2, s.heard)
        assertEquals(0, s.clash)
        assertEquals(3, s.slots)
        assertEquals("pps", s.time)
        assertEquals(0.1, s.timeErrMs!!, 0.0)
        assertEquals("sim", s.board)
        assertEquals(0, s.slot)
        assertTrue(s.keyed)
        assertEquals(0x86c91c92L, s.group)
        assertTrue(s.fix)
        assertEquals(2, s.riders)
        assertEquals("0.43%", s.duty)
        assertEquals(16, s.dbm)
        assertTrue(s.kept)
        assertEquals(1, s.state)
        assertEquals(3, s.echoS)
        assertEquals(listOf(3, 10, 1), listOf(s.beat, s.moveM, s.passengers))
        assertEquals(3, s.power)
        assertEquals(9, s.sats)
        // no one has carried our state yet; a board before v5's send rule
        val first = BoardLines.status("STATUS proto=8 slot=0 group=86c91c92 echo=- mode=7/250")!!
        assertNull(first.echoS)
        assertNull(first.beat)
        assertEquals("7/250", first.mode)
        assertFalse(BoardLines.status("STATUS proto=8 slot=0 group=86c91c92 kept=0")!!.kept)
        // another state than a call for help is not one
        assertFalse(BoardLines.status("STATUS proto=8 slot=0 group=86c91c92 state=3")!!.help)
        // without a version it is not a status this app can trust
        assertNull(BoardLines.status("STATUS slot=0 keyed=1 group=86c91c92"))
        assertEquals(7, BoardLines.status("STATUS proto=7 slot=0 group=00000007")!!.proto)
        // a board without a key, nor a time yet
        val bare = BoardLines.status("STATUS proto=8 slot=4 keyed=0 group=00000000 time=none err=?")!!
        assertFalse(bare.keyed)
        assertNull(bare.timeErrMs)
    }

    @Test
    fun `what a board took from a GROUP, against what was sent`() {
        val sent = Invitation(ByteArray(16) { it.toByte() }, 2, 5, 2000, "Alps ride", listOf("Ann", "Bob", "Cid", "Dee", "Eve"))
        val taken = BoardLines.setup("OK group=86c91c92 slot=2 slots=5 interval=2000")!!
        assertTrue(taken.of(sent))
        assertFalse(taken.of(sent.copy(slot = 3)))
        assertFalse(taken.of(sent.copy(key = ByteArray(16))))
        assertNull(BoardLines.setup("OK keyed group=86c91c92"))
        assertNull(BoardLines.setup("OK group=86c91c92 slot=2"))
    }

    @Test
    fun `a board's answer to CLOCK, for reckoning its clock against ours`() {
        assertEquals(BoardLines.ClockReply(t1Ms = 123_456, boot = 0xa, rxUs = 10_000_000, txUs = 10_000_100, utcMs = 1_792_670_415_000),
            BoardLines.clock("CLOCK t1=123456 boot=0000000a rx=10000000 tx=10000100 utc=1792670415000"))
        // a board with no GNSS time yet
        assertNull(BoardLines.clock("CLOCK t1=123456 boot=0000000a rx=10000000 tx=10000100 utc=?")!!.utcMs)
        assertNull(BoardLines.clock("CLOCK t1=123456 boot=0000000a rx=10000000"))
        assertNull(BoardLines.clock("ERR commands: ..."))
    }

    @Test
    fun `the board's own lines for the log, numbered, and its answer to LOG`() {
        assertEquals(41L, BoardLines.logNumber("LOG n=41 at=70100 TX counter=12 len=34 ok"))
        assertEquals(null, BoardLines.logNumber("TX counter=12 len=34 ok"))
        assertEquals(BoardLines.LogStart(boot = 0x1a2b3c4d, from = 14, next = 142),
            BoardLines.logStart("OK log boot=1a2b3c4d from=14 next=142"))
        assertNull(BoardLines.logStart("OK beat=3"))
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
