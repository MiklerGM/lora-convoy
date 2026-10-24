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
        assertEquals(emptyList<Rider>(), riders.all())
        // a line of the group before, made before the new key, comes late
        riders.take(rider(2), before)
        assertEquals(emptyList<Rider>(), riders.all())
    }

    // This phone: 12:00:00 UTC at its monotonic 100 000 ms. Its board: a clock 40 s behind the
    // phone's, run 0x0a, and no GNSS. A rider it heard at its 70 000 ms, the phone's 110 000, the
    // rider's fix of 12:00:08 (64 200), the line arriving 150 ms after.
    private val wall0 = 1_792_670_400_000L - 100_000
    private fun wall(mono: Long) = mono + wall0
    private val heardLine = "RIDER slot=2 group=86c91c92 boot=0000000a at=70100 heard_at=70000 fix=64200 lat=47.1 lon=9.5 " +
        "fix_age=? heard=0.1 speed=40 course=0 gps=1"

    private fun take(riders: BoardRiders, line: String, arrivedMono: Long) =
        riders.take(BoardLines.rider(line, wall(arrivedMono))!!, BoardLines.groupOf(line), BoardLines.heard(line))

    @Test
    fun `a board without GNSS, its rider's fix age at once, and its radio contact's once the clocks are reckoned`() {
        val riders = BoardRiders()
        val clock = BoardClock()
        take(riders, heardLine, arrivedMono = 110_150)
        val now = 115_000L
        assertEquals(7_000L, wall(now) - riders.all(clock, now, wall(now)).single().fixAtMs!!)
        // an exchange, 20 ms each way, with no new line from the rider
        clock.observe(boot = 0xa, t1Ms = 112_000, t2Us = 72_020_000, t3Us = 72_021_000, t4Ms = 112_041)
        val r = riders.all(clock, now, wall(now)).single()
        assertEquals(5_000L, wall(now) - r.heardAtMs)
        assertEquals(7_000L, wall(now) - r.fixAtMs!!)
    }

    @Test
    fun `a slow first exchange, then a quick one, the stored rider's contact age comes right, no new line`() {
        val riders = BoardRiders()
        val clock = BoardClock()
        take(riders, heardLine, arrivedMono = 110_150)
        // the first answer waited 5 s on its way: the clocks reckoned 2.5 s off, and loose
        clock.observe(boot = 0xa, t1Ms = 112_000, t2Us = 72_020_000, t3Us = 72_021_000, t4Ms = 117_041)
        val now = 118_000L
        assertEquals(5_500L, wall(now) - riders.all(clock, now, wall(now)).single().heardAtMs)
        clock.observe(boot = 0xa, t1Ms = 118_000, t2Us = 78_020_000, t3Us = 78_021_000, t4Ms = 118_041)
        assertEquals(8_000L, wall(now) - riders.all(clock, now, wall(now)).single().heardAtMs)
    }

    @Test
    fun `a board restarted, its new run reckoned afresh, the riders of the run before left where they were`() {
        val riders = BoardRiders()
        val clock = BoardClock()
        take(riders, heardLine, arrivedMono = 110_150)
        clock.observe(boot = 0xa, t1Ms = 112_000, t2Us = 72_020_000, t3Us = 72_021_000, t4Ms = 112_041)
        // restarted near the phone's 200 000: its clock from 0 again, under a run of its own
        clock.observe(boot = 0xb, t1Ms = 205_000, t2Us = 5_020_000, t3Us = 5_021_000, t4Ms = 205_041)
        val after = heardLine.replace("slot=2", "slot=3").replace("boot=0000000a", "boot=0000000b")
            .replace("at=70100", "at=6100").replace("heard_at=70000", "heard_at=6000")
        take(riders, after, arrivedMono = 206_150)
        val now = 210_000L
        val byId = riders.all(clock, now, wall(now)).associateBy { it.id }
        assertEquals(100_000L, wall(now) - byId.getValue("slot2").heardAtMs)
        assertEquals(4_000L, wall(now) - byId.getValue("slot3").heardAtMs)
    }

    @Test
    fun `riders of the board's group stay through its statuses, heard again or not`() {
        val riders = BoardRiders()
        riders.group(before)
        riders.take(rider(1), before)
        riders.group(before)
        riders.take(rider(3), before)
        assertEquals(setOf("slot1", "slot3"), riders.all().map { it.id }.toSet())
    }
}
