package io.github.miklergm.convoy

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class BondHealthTest {

    @Test
    fun `two refused sessions in a row drop the bond, one does not`() {
        val bond = BondHealth()
        assertFalse(bond.sessionEnded(authenticationRejected = true))
        assertTrue(bond.sessionEnded(authenticationRejected = true))
        // and it starts counting again
        assertFalse(bond.sessionEnded(authenticationRejected = true))
    }

    @Test
    fun `a write that went through clears the suspicion, a dull drop neither counts nor clears`() {
        val bond = BondHealth()
        assertFalse(bond.sessionEnded(authenticationRejected = true))
        bond.authenticatedWriteSucceeded()
        assertFalse(bond.sessionEnded(authenticationRejected = true))
        assertFalse(bond.sessionEnded(authenticationRejected = false))
        assertTrue(bond.sessionEnded(authenticationRejected = true))
    }
}
