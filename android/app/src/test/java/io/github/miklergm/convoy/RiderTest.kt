package io.github.miklergm.convoy

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.ZoneOffset
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.sqrt

class RiderTest {

    private val start = 1_800_000_000_000L
    private val lat0 = 46.56
    private val lon0 = 8.34

    private fun riders(atS: Double) = MockRiders.at(lat0, lon0, start, start + (atS * 1000).toLong())

    /** Metres north and east from a to b; exact enough over a few hundred metres. */
    private fun offset(a: Rider, b: Rider): Pair<Double, Double> {
        val north = (b.lat - a.lat) * 111_320.0
        val east = (b.lon - a.lon) * 111_320.0 * cos(Math.toRadians(a.lat))
        return north to east
    }

    @Test
    fun `a rider rides at its speed, along its course`() {
        val a10 = riders(10.0)[0]
        val a11 = riders(11.0)[0]
        val (north, east) = offset(a10, a11)
        assertEquals(40 / 3.6, sqrt(north * north + east * east), 0.2)
        val bearing = Math.floorMod(Math.toDegrees(atan2(east, north)).toInt(), 360)
        val diff = abs(bearing - a10.courseDeg!!)
        assertTrue("bearing $bearing, course ${a10.courseDeg}", minOf(diff, 360 - diff) <= 3)
        assertEquals(start + 11_000, a11.heardAtMs)
    }

    @Test
    fun `B asks for help and stops, C goes quiet and keeps its last report`() {
        assertFalse(riders(19.0)[1].help)
        val b = riders(25.0)[1]
        assertTrue(b.help && b.stopped && b.speedKmh == 0)
        assertEquals(riders(20.0)[1].lat, b.lat, 0.0)
        assertEquals(start + 25_000, b.heardAtMs) // still heard: help needs a link, not silence

        val c = riders(90.0)[2]
        assertEquals(start + 60_000, c.heardAtMs)
        assertEquals(riders(60.0)[2].lat, c.lat, 0.0)
    }

    @Test
    fun `the point shows help and staleness on the map, the two times in the menu`() {
        val now = start + 100_000 // 08:01:40 UTC
        val utc = ZoneOffset.UTC
        val fresh = Rider("a", "Rider A", "A", 0xFF1E88E5.toInt(), lat0, lon0,
            fixAtMs = now - 3_000, heardAtMs = now - 1_000, speedKmh = 40, courseDeg = 270)
        fresh.toPoint(now, utc).let {
            assertEquals("A", it.shortName)
            assertEquals(fresh.color, it.color)
            assertFalse(it.stale)
            assertEquals(listOf("Heard at 08:01:39", "Fix from 08:01:37", "40 km/h, heading 270°"), it.details)
        }
        fresh.copy(help = true, stopped = true).toPoint(now, utc).let {
            assertEquals("A SOS", it.shortName)
            assertEquals("Rider A needs help", it.fullName)
            assertEquals(HELP_COLOR, it.color)
            assertEquals(
                listOf("Needs help", "No one has answered yet", "Heard at 08:01:39", "Fix from 08:01:37", "Stopped"),
                it.details,
            )
        }
        fresh.copy(heardAtMs = now - 45_000, gpsOk = false).toPoint(now, utc).let {
            assertEquals("A 45 s", it.shortName)
            assertEquals(STALE_COLOR, it.color)
            assertTrue(it.stale)
            assertEquals(listOf("Heard at 08:00:55", "No GPS fix; the last from 08:01:37"), it.details.take(2))
        }
        // the edge: stale only once past the threshold
        assertFalse(fresh.copy(heardAtMs = now - STALE_AFTER_MS).toPoint(now, utc).stale)
    }

    @Test
    fun `a call for help shows who answers it`() {
        val now = start
        fun rider(id: String, help: Boolean = false, acks: String? = null) = Rider(id, "Rider $id", id, 0,
            lat0, lon0, fixAtMs = now, heardAtMs = now, speedKmh = 0, courseDeg = null, help = help, acks = acks)
        val riders = withAnswers(listOf(rider("2", help = true), rider("3", acks = "2"), rider("4", acks = "2"),
            rider("5", acks = "7")))
        assertEquals(listOf("Rider 3", "Rider 4"), riders[0].answeredBy)
        assertTrue(riders.drop(1).all { it.answeredBy.isEmpty() })
        assertEquals("Answered by Rider 3, Rider 4", riders[0].toPoint(now).details[1])
    }

    @Test
    fun `ages grow coarser`() {
        assertEquals("0 s", age(-5))
        assertEquals("59 s", age(59_999))
        assertEquals("1 min", age(60_000))
        assertEquals("59 min", age(3_599_999))
        assertEquals("1 h", age(3_600_000))
    }
}
