package io.github.miklergm.convoy

/**
 * Whether this phone's pairing with its board is still any good (after dmd2-lora-bridge's).
 *
 * A board that forgot the phone, by its button while the phone was away, refuses its encrypted
 * writes. Some phones then pair again by themselves, once the rider consents (Android 15 on the
 * bench does); others keep the stale key for good (what the bridge met on rides). So after a run
 * of sessions ended that way, the phone's bond goes too, and pairing starts over. One such session
 * is not enough to judge on: links drop for dull reasons.
 */
class BondHealth(private val attempts: Int = 2) {

    private var deadSessions = 0

    /** An encrypted write that went through: the keys on both sides still agree. */
    fun authenticatedWriteSucceeded() {
        deadSessions = 0
    }

    /** True when the bond should go: a run of sessions in which the bonded board refused a write. */
    fun sessionEnded(authenticationRejected: Boolean): Boolean {
        if (!authenticationRejected) return false
        if (++deadSessions < attempts) return false
        deadSessions = 0
        return true
    }
}
