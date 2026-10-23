package io.github.miklergm.convoy

import java.util.concurrent.ConcurrentHashMap

/**
 * A board's riders as this phone knows them: each RIDER line as it comes, and those the board no
 * longer knows dropped when its status names whom it knows (known=). The status comes every 10 s
 * whatever else was lost on the way, the end of a RIDERS reply among it, so a rider of the group
 * before, or one the board has forgotten, leaves the map within that.
 *
 * Lines come on the link's thread; [all] is read on another.
 */
class BoardRiders {
    private val riders = ConcurrentHashMap<String, Rider>()
    /** When each rider's last line came, on this phone's clock. */
    private val takenAtMs = ConcurrentHashMap<String, Long>()

    val all: List<Rider> get() = riders.values.toList()

    fun take(r: Rider, nowMs: Long) {
        riders[r.id] = r
        takenAtMs[r.id] = nowMs
    }

    /**
     * The board's status: the riders it knows, by id. The others go, but for one whose line came
     * within [GRACE_MS], which a status made just before it does not name yet.
     */
    fun known(ids: Set<String>, nowMs: Long) {
        riders.keys.removeIf { it !in ids && nowMs - (takenAtMs[it] ?: 0L) > GRACE_MS }
        takenAtMs.keys.retainAll(riders.keys)
    }

    fun clear() {
        riders.clear()
        takenAtMs.clear()
    }

    companion object {
        const val GRACE_MS = 5_000L
    }
}
