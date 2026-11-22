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
    /** What the rider says: [STATE_NAMES], by the firmware's numbers (codec.h). */
    val state: Int = STATE_NORMAL,
    val stopped: Boolean = false,
    val gpsOk: Boolean = true,
    /** Who told us of this rider last, when it was not the rider itself: its name. */
    val via: String? = null,
    /** False for a rider heard before its first fix (v5 pos=0): its state counts, its 0, 0 is no place. */
    val hasPosition: Boolean = true,
) {
    val help get() = state == STATE_HELP
}

const val STATE_NORMAL = 0
const val STATE_HELP = 1
const val STATE_STOP_NOW = 2

/** The states a rider can choose, by number, as the firmware has them (codec.h). */
val STATE_NAMES = listOf("Riding", "Needs help", "Stop now", "Stop when convenient", "Helping")

/** What the map's label adds for each state; "" for none. */
private val STATE_TAGS = listOf("", "SOS", "STOP", "break", "helping")

fun stateName(state: Int) = STATE_NAMES.getOrElse(state) { "State $state" }

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
 * Not heard for this long, or a position this old, and the point turns grey, labelled with how
 * old. ponytail: one fixed threshold; derive it from the group's interval.
 */
const val STALE_AFTER_MS = 30_000L

/** A fix stamped further ahead than this is a clock gone wrong, not a fresh fix. */
private const val FUTURE_SLACK_MS = 5_000L

const val HELP_COLOR = 0xFFD32F2F.toInt()
const val STALE_COLOR = 0xFF9E9E9E.toInt()

private val CLOCK = DateTimeFormatter.ofPattern("HH:mm:ss")

/**
 * OsmAnd 5.4 draws a layer point as a circle in the point's colour, labelled with its short name;
 * the details are lines in the point's menu. So a state and staleness go into the colour and the
 * label, which are redrawn every tick and seen without a tap. The details hold clock times, not
 * ages: an open menu is not redrawn, and "heard 2 s ago" would stay on screen while it grew false.
 */
fun Rider.toPoint(nowMs: Long, zone: ZoneId = ZoneId.systemDefault()): PointSpec {
    val heardAgo = nowMs - heardAtMs
    // The position is what the map shows: old when its fix is, when the rider's GPS is lost, and
    // when its time cannot be told, however lately the radio was heard (01, no GPS fix).
    val fixAgo = fixAtMs?.let { nowMs - it }
    val positionOld = !gpsOk || fixAgo == null || fixAgo > STALE_AFTER_MS || fixAgo < -FUTURE_SLACK_MS
    val stale = heardAgo > STALE_AFTER_MS || positionOld
    // the state's word and, once old, how old: "3 SOS 2 min"
    val label = listOf(short, STATE_TAGS.getOrElse(state) { "" }, if (stale) age(maxOf(heardAgo, fixAgo ?: heardAgo)) else "")
        .filter { it.isNotEmpty() }.joinToString(" ")
    fun at(ms: Long) = CLOCK.format(Instant.ofEpochMilli(ms).atZone(zone))
    val details = buildList {
        if (state != STATE_NORMAL) add(stateName(state))
        add("Heard at ${at(heardAtMs)}" + (via?.let { " from $it" } ?: ""))
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
        fullName = if (state != STATE_NORMAL) "$name: ${stateName(state).lowercase()}" else name,
        color = when {
            // both ask everyone at once; the label tells them apart
            help || state == STATE_STOP_NOW -> HELP_COLOR
            stale -> STALE_COLOR
            else -> color
        },
        lat = lat,
        lon = lon,
        details = details,
        stale = stale,
    )
}

/** What OsmAnd draws: every rider with a position; one with none yet is in the list only. */
fun mapPoints(riders: List<Rider>, nowMs: Long): List<PointSpec> = riders.filter { it.hasPosition }.map { it.toPoint(nowMs) }

/** "4 s", "2 min", "3 h": coarser as it grows, since a stale age is read at a glance. */
fun age(ms: Long): String {
    val s = max(0L, ms) / 1000
    return when {
        s < 60 -> "$s s"
        s < 3600 -> "${s / 60} min"
        else -> "${s / 3600} h"
    }
}

/**
 * One line of the app's own list: who, what they say, how old their position is, and who told it.
 * One age while the rider has a fix, taken just before it was sent, so the older; with none, both.
 */
fun Rider.line(nowMs: Long): String = buildList {
    add(name)
    if (state != STATE_NORMAL) add(stateName(state))
    val via = via?.let { " via $it" } ?: ""
    if (!hasPosition) {
        add("no position yet, not on the map")
        add("heard ${age(nowMs - heardAtMs)} ago$via")
    } else if (gpsOk && fixAtMs != null) {
        add("${age(nowMs - fixAtMs)} ago$via")
    } else {
        add("position " + (fixAtMs?.let { "${age(nowMs - it)} old" } ?: "of unknown age") + if (gpsOk) "" else ", no GPS")
        add("heard ${age(nowMs - heardAtMs)} ago$via")
    }
    speedKmh?.let { add(if (stopped || it == 0) "stopped" else "$it km/h") }
}.joinToString(" · ")
