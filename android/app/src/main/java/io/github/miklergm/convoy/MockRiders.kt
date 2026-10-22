package io.github.miklergm.convoy

import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.roundToInt
import kotlin.math.sin

/**
 * Three riders circling a point, standing in for a board until M4:
 * - A rides on;
 * - B asks for help 20 s in, and stops where it is;
 * - C goes out of range 60 s in: its last report stays, and only grows older.
 *
 * A function of time and nothing else, so tests and the service see the same riders.
 */
object MockRiders {

    const val HELP_AFTER_S = 20.0
    const val LOST_AFTER_S = 60.0

    private const val M_PER_DEG_LAT = 111_320.0

    fun at(centreLat: Double, centreLon: Double, startMs: Long, nowMs: Long): List<Rider> {
        val t = (nowMs - startMs) / 1000.0
        val fixAt = nowMs - Math.floorMod(nowMs, 1000L) // whole seconds, as the air format has them
        fun loop(id: String, color: Int, radiusM: Double, kmh: Double, phase: Double, atS: Double) =
            circle(centreLat, centreLon, radiusM, kmh, phase, atS).let { (lat, lon, course) ->
                Rider(id, "Rider ${id.uppercase()}", id.uppercase(), color, lat, lon,
                    fixAt, nowMs, kmh.roundToInt(), course)
            }
        val a = loop("a", 0xFF1E88E5.toInt(), 600.0, 40.0, 0.0, t)
        val b = if (t < HELP_AFTER_S) loop("b", 0xFF43A047.toInt(), 300.0, 20.0, 2.0, t)
        else loop("b", 0xFF43A047.toInt(), 300.0, 20.0, 2.0, HELP_AFTER_S)
            .copy(speedKmh = 0, stopped = true, help = true)
        val c = if (t < LOST_AFTER_S) loop("c", 0xFFFB8C00.toInt(), 1000.0, 60.0, 4.0, t)
        else loop("c", 0xFFFB8C00.toInt(), 1000.0, 60.0, 4.0, LOST_AFTER_S).let {
            val lastMs = startMs + (LOST_AFTER_S * 1000).toLong()
            it.copy(fixAtMs = lastMs - Math.floorMod(lastMs, 1000L), heardAtMs = lastMs)
        }
        return listOf(a, b, c)
    }

    /** Where a rider on a circle is after [atS] seconds, and its course in degrees from north. */
    private fun circle(lat0: Double, lon0: Double, r: Double, kmh: Double, phase: Double, atS: Double):
        Triple<Double, Double, Int> {
        val theta = phase + kmh / 3.6 / r * atS
        val north = r * cos(theta)
        val east = r * sin(theta)
        val lat = lat0 + north / M_PER_DEG_LAT
        val lon = lon0 + east / (M_PER_DEG_LAT * cos(Math.toRadians(lat0)))
        // moving with growing theta: velocity (north, east) is (-sin, cos)
        val course = Math.floorMod(Math.toDegrees(atan2(cos(theta), -sin(theta))).roundToInt(), 360)
        return Triple(lat, lon, course)
    }
}
