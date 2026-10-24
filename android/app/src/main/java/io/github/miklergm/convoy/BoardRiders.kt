package io.github.miklergm.convoy

import java.util.concurrent.ConcurrentHashMap

/**
 * A board's riders as this phone knows them: each RIDER line as it comes, with the group its board
 * heard it in (group=, an id from the key), and only those of the group the board is in now, as
 * its status and its answer to GROUP say. A rider of the group before goes as soon as the board is
 * in another, whatever lines were lost on the way, and a line of theirs that comes late is not
 * taken. A rider the board no longer hears stays, aging, as the last that is known of them.
 *
 * Lines come on the link's thread; [all] is read on another.
 */
class BoardRiders {
    private class Kept(val rider: Rider, val group: Long?, val heard: BoardLines.Heard?)
    private val riders = ConcurrentHashMap<String, Kept>()
    /** The board's group, once it has said; until then lines are taken as they come. */
    @Volatile private var group: Long? = null

    /**
     * The riders, each placed anew: the moment its board heard it, on the board's clock, set on
     * this phone's monotonic clock by [clock]'s best reckoning of that run, and so on our calendar
     * through [nowMono] and [nowWall]; and its fix by its own time, on our calendar. So a better
     * reckoning moves a rider kept long since, with no new line of its own. Without a reckoning yet,
     * as its line told it when it came.
     */
    fun all(clock: BoardClock? = null, nowMono: Long = 0, nowWall: Long = 0): List<Rider> = riders.values.map { k ->
        val heard = k.heard ?: return@map k.rider
        val heardMono = clock?.phoneMs(heard.boot, heard.rxMs, nowMono) ?: return@map k.rider
        val heardWall = nowWall - (nowMono - heardMono)
        k.rider.copy(heardAtMs = heardWall, fixAtMs = heard.fix?.let { BoardLines.fixUtcMs(it, heardWall) } ?: k.rider.fixAtMs)
    }

    fun take(r: Rider, inGroup: Long?, heard: BoardLines.Heard? = null) {
        val now = group
        if (now == null || inGroup == now) riders[r.id] = Kept(r, inGroup, heard)
    }

    /** The board's group, from its status or its answer to GROUP: the riders of any other go. */
    fun group(g: Long) {
        group = g
        riders.values.removeIf { it.group != g }
    }

    fun clear() {
        riders.clear()
        group = null
    }
}
