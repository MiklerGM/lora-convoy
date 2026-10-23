package io.github.miklergm.convoy

import java.util.concurrent.ConcurrentHashMap

/**
 * A board's riders as this phone knows them: each RIDER line as it comes, and the board's whole
 * table in place of ours when it sends that (RIDERS, ended by "RIDERS n"), so a rider the board no
 * longer knows, of another group or before its restart, leaves the map too.
 *
 * Lines come on the link's thread; [all] is read on another.
 */
class BoardRiders {
    private val riders = ConcurrentHashMap<String, Rider>()
    /** The lines since RIDERS was asked, until its end. */
    private var table: MutableMap<String, Rider>? = null

    val all: List<Rider> get() = riders.values.toList()

    /** RIDERS has been asked: what comes until its end is the board's whole table. */
    fun asked() {
        table = mutableMapOf()
    }

    fun take(r: Rider) {
        riders[r.id] = r
        table?.put(r.id, r)
    }

    /** "RIDERS n": the table, in place of ours. Never empty on the way, for a map drawn meanwhile. */
    fun ended() {
        val t = table ?: return
        riders.keys.retainAll(t.keys)
        riders.putAll(t)
        table = null
    }

    fun clear() {
        riders.clear()
        table = null
    }
}
