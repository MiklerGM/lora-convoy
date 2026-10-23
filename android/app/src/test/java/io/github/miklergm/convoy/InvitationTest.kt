package io.github.miklergm.convoy

import java.util.Base64
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class InvitationTest {

    private val key = ByteArray(16) { it.toByte() }
    private val five = listOf("Ann", "Bob", "Cid", "Dee", "Eve")

    @Test
    fun `the format, and what the board is told`() {
        val invitation = Invitation(key, 2, 5, 2000, "Alps ride", five)
        // from Python, by the format in Invitation's comment: a change to it shows here
        val text = "LC2.AgABAgMEBQYHCAkKCwwNDg8CBQfQCUFscHMgcmlkZQNBbm4DQm9iA0NpZANEZWUDRXZl"
        assertEquals(text, invitation.encode())
        assertEquals(invitation, Invitation.decode(text))
        assertEquals(invitation, Invitation.decode("  $text\n")) // as copied from a messenger
        assertEquals(
            listOf("KEY 000102030405060708090a0b0c0d0e0f", "SLOTS 5", "SLOT 2", "INTERVAL 2000"),
            invitation.commands(),
        )
        assertEquals("Cid", invitation.riderName(2))
        assertEquals("Rider 2", invitation.copy(names = listOf("Ann", "", "Cid", "Dee", "Eve")).riderName(1))
    }

    @Test
    fun `the group tag is the firmware's`() {
        // firmware/test/test_core.c, and openssl: the key 00 01 .. 0f has the tag 86
        assertEquals(0x86, Invitation.groupTag(key))
    }

    @Test
    fun `a new group has one fresh key and a slot for each rider named`() {
        val group = Invitation.newGroup("Bench", listOf("Ann", "Bob", "Cid"), 2000)
        assertEquals(listOf(0, 1, 2), group.map { it.slot })
        assertTrue(group.all { it.key.contentEquals(group[0].key) && it.riders == 3 && it.names[1] == "Bob" })
        assertFalse(Invitation.newGroup("Bench", listOf("Ann", "Bob"), 2000)[0].key.contentEquals(group[0].key))
    }

    @Test
    fun `anything else is refused whole`() {
        val good = Invitation(key, 0, 2, 2000, "x", listOf("Ann", "Bob")).encode()
        val bytes = Base64.getUrlDecoder().decode(good.removePrefix(Invitation.PREFIX))
        fun encoded(b: ByteArray) = Invitation.PREFIX + Base64.getUrlEncoder().withoutPadding().encodeToString(b)
        fun with(i: Int, v: Int) = encoded(bytes.copyOf().also { it[i] = v.toByte() })
        assertEquals(1, Invitation.decode(with(17, 1))!!.slot) // the bytes below are where the format says
        assertNull(Invitation.decode(with(0, 1))) // v1, without names
        assertNull(Invitation.decode(with(17, 2))) // slot 2 of 2 riders
        assertNull(Invitation.decode(with(19, 0))) // an interval of 208 ms
        assertNull(Invitation.decode(with(18, 3))) // three riders, two names
        assertNull(Invitation.decode(with(21, 2))) // the group's name longer than it is
        assertNull(Invitation.decode(encoded(bytes.copyOf(bytes.size - 1)))) // cut short in the last name
        assertNull(Invitation.decode(encoded(bytes + 0.toByte()))) // more than the names
        assertNull(Invitation.decode(good.dropLast(2))) // not base64
        assertNull(Invitation.decode("LC1." + good.removePrefix(Invitation.PREFIX)))
        assertNull(Invitation.decode("hello"))
        // a rider's name over 16 bytes is not made either
        assertTrue(runCatching { Invitation(key, 0, 1, 2000, "x", listOf("A name far too long")) }.isFailure)
    }
}
