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
    private val riders = ConcurrentHashMap<String, Pair<Rider, Long?>>()
    /** The board's group, once it has said; until then lines are taken as they come. */
    @Volatile private var group: Long? = null

    val all: List<Rider> get() = riders.values.map { it.first }

    fun take(r: Rider, inGroup: Long?) {
        val now = group
        if (now == null || inGroup == now) riders[r.id] = r to inGroup
    }

    /** The board's group, from its status or its answer to GROUP: the riders of any other go. */
    fun group(g: Long) {
        group = g
        riders.values.removeIf { it.second != g }
    }

    fun clear() {
        riders.clear()
        group = null
    }
}
