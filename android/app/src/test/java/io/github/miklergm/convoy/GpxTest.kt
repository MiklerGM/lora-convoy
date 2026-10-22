package io.github.miklergm.convoy

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Test

class GpxTest {

    private fun gpx(body: String) = """<?xml version="1.0" encoding="UTF-8"?>
        <gpx version="1.1" creator="test" xmlns="http://www.topografix.com/GPX/1/1">$body</gpx>""".toByteArray()

    @Test
    fun `counts the points, names the route, measures the line`() {
        // 0.001 degrees of latitude is 111.19 m on the sphere
        val s = Gpx.summarize(gpx("""
            <metadata><name> Day 3: the pass </name></metadata>
            <wpt lat="46.0" lon="8.0"><name>fuel</name></wpt>
            <wpt lat="46.1" lon="8.0"/>
            <trk><name>track name, not the route's</name>
              <trkseg><trkpt lat="46.000" lon="8.0"/><trkpt lat="46.001" lon="8.0"/><trkpt lat="46.002" lon="8.0"/></trkseg>
              <trkseg><trkpt lat="47.000" lon="8.0"/><trkpt lat="47.001" lon="8.0"/></trkseg>
            </trk>
            <rte><rtept lat="45.0" lon="9.0"/><rtept lat="45.001" lon="9.0"/></rte>
        """))
        assertEquals("Day 3: the pass", s.name)
        assertEquals(5, s.trackPoints)
        assertEquals(2, s.routePoints)
        assertEquals(2, s.waypoints)
        // two segments are not joined: 2 + 1 + 1 steps of 0.001 degrees
        assertEquals(4 * 111.19, s.lengthM, 0.5)
    }

    @Test
    fun `a track's name stands in when the file has none, and broken points are skipped`() {
        val s = Gpx.summarize(gpx("""
            <trk><name>Morning loop</name><trkseg>
              <trkpt lat="46.0" lon="8.0"/><trkpt lat="north" lon="8.0"/><trkpt lat="95" lon="8.0"/>
            </trkseg></trk>"""))
        assertEquals("Morning loop", s.name)
        assertEquals(1, s.trackPoints)
    }

    @Test
    fun `whatever is not GPX is refused`() {
        assertThrows(IllegalArgumentException::class.java) {
            Gpx.summarize("""<kml xmlns="http://www.opengis.net/kml/2.2"><Document/></kml>""".toByteArray())
        }
        assertThrows(IllegalArgumentException::class.java) { Gpx.summarize("not xml at all".toByteArray()) }
        assertThrows(IllegalArgumentException::class.java) { Gpx.summarize(ByteArray(Gpx.MAX_BYTES + 1)) }
    }

    @Test
    fun `an external entity never reads a file`() {
        val evil = """<?xml version="1.0"?><!DOCTYPE gpx [<!ENTITY x SYSTEM "file:///etc/passwd">]>
            <gpx><metadata><name>&x;</name></metadata></gpx>""".toByteArray()
        // refused outright here, where the parser can forbid a DTD; elsewhere the entity reads empty
        val name = runCatching { Gpx.summarize(evil) }.getOrNull()?.name
        assertFalse(name.orEmpty().contains("root"))
    }

    @Test
    fun `the hash and the file name`() {
        assertEquals("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", Gpx.sha256("abc".toByteArray()))
        val sha = "0123456789abcdef".repeat(4)
        assertEquals("Day 3 the pass-01234567.gpx", Gpx.fileName(" Day 3: the pass/../ ", sha))
        assertEquals("route-01234567.gpx", Gpx.fileName(null, sha))
        assertNull(Gpx.summarize(gpx("<trk/>")).name)
    }
}
