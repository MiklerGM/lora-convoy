package io.github.miklergm.convoy

import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import kotlin.math.max

/**
 * One rider as the app knows them, whatever the source: the mock, or a board's RIDER lines.
 *
 * Two ages, never one (01-goals, separate statuses): when the position was measured, and when the
 * rider was last heard. A rider heard a second ago can still carry a minute-old fix. What the
 * source does not know stays null rather than turning into a plausible number.
 */
data class Rider(
    val id: String,
    val name: String,
    val short: String,
    val color: Int,
    val lat: Double,
    val lon: Double,
    val fixAtMs: Long?,
    val heardAtMs: Long,
    val speedKmh: Int?,
    val courseDeg: Int?,
    val help: Boolean = false,
    val stopped: Boolean = false,
    val gpsOk: Boolean = true,
)

/** What OsmAnd draws for a rider, as plain data so the rules are tested without Android. */
data class PointSpec(
    val id: String,
    val shortName: String,
    val fullName: String,
    val color: Int,
    val lat: Double,
    val lon: Double,
    val details: List<String>,
    val stale: Boolean,
)

/**
 * Not heard for this long and the point turns grey, labelled with how long.
 * ponytail: one fixed threshold; derive it from the group's cadence once M5 sets one.
 */
const val STALE_AFTER_MS = 30_000L

const val HELP_COLOR = 0xFFD32F2F.toInt()
const val STALE_COLOR = 0xFF9E9E9E.toInt()

private val CLOCK = DateTimeFormatter.ofPattern("HH:mm:ss")

/**
 * OsmAnd 5.4 draws a layer point as a circle in the point's colour, labelled with its short name;
 * the details are lines in the point's menu. So help and staleness go into the colour and the
 * label, which are redrawn every tick and seen without a tap. The details hold clock times, not
 * ages: an open menu is not redrawn, and "heard 2 s ago" would stay on screen while it grew false.
 */
fun Rider.toPoint(nowMs: Long, zone: ZoneId = ZoneId.systemDefault()): PointSpec {
    val heardAgo = nowMs - heardAtMs
    val stale = heardAgo > STALE_AFTER_MS
    val label = when {
        help -> "$short SOS"
        stale -> "$short ${age(heardAgo)}"
        else -> short
    }
    fun at(ms: Long) = CLOCK.format(Instant.ofEpochMilli(ms).atZone(zone))
    val details = buildList {
        if (help) add("Needs help")
        add("Heard at ${at(heardAtMs)}")
        add(when {
            fixAtMs == null -> "Fix time unknown"
            gpsOk -> "Fix from ${at(fixAtMs)}"
            else -> "No GPS fix; the last from ${at(fixAtMs)}"
        })
        add(when {
            stopped || speedKmh == 0 -> "Stopped"
            speedKmh == null -> "Speed unknown"
            else -> "$speedKmh km/h" + (courseDeg?.let { ", heading $it°" } ?: "")
        })
    }
    return PointSpec(
        id = id,
        shortName = label,
        fullName = if (help) "$name needs help" else name,
        color = when {
            help -> HELP_COLOR
            stale -> STALE_COLOR
            else -> color
        },
        lat = lat,
        lon = lon,
        details = details,
        stale = stale,
    )
}

/** "4 s", "2 min", "3 h": coarser as it grows, since a stale age is read at a glance. */
fun age(ms: Long): String {
    val s = max(0L, ms) / 1000
    return when {
        s < 60 -> "$s s"
        s < 3600 -> "${s / 60} min"
        else -> "${s / 3600} h"
    }
}
