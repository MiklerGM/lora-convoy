package io.github.miklergm.convoy

import java.util.Base64
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class InvitationTest {

    private val key = ByteArray(16) { it.toByte() }

    @Test
    fun `the format, and what the board is told`() {
        val invitation = Invitation(key, 2, 5, 2000, "Alps ride")
        // from Python, by the format in Invitation's comment: a change to it shows here
        val text = "LC1.AQABAgMEBQYHCAkKCwwNDg8CBQfQCUFscHMgcmlkZQ"
        assertEquals(text, invitation.encode())
        assertEquals(invitation, Invitation.decode(text))
        assertEquals(invitation, Invitation.decode("  $text\n")) // as copied from a messenger
        assertEquals(
            listOf("KEY 000102030405060708090a0b0c0d0e0f", "SLOTS 5", "SLOT 2", "INTERVAL 2000"),
            invitation.commands(),
        )
    }

    @Test
    fun `the group tag is the firmware's`() {
        // firmware/test/test_core.c, and openssl: the key 00 01 .. 0f has the tag 86
        assertEquals(0x86, Invitation.groupTag(key))
    }

    @Test
    fun `a new group has one fresh key and a slot for each rider`() {
        val group = Invitation.newGroup("Bench", 3, 2000)
        assertEquals(listOf(0, 1, 2), group.map { it.slot })
        assertTrue(group.all { it.key.contentEquals(group[0].key) && it.riders == 3 })
        assertFalse(Invitation.newGroup("Bench", 3, 2000)[0].key.contentEquals(group[0].key))
    }

    @Test
    fun `anything else is refused whole`() {
        val good = Invitation(key, 0, 2, 2000, "x").encode()
        val bytes = Base64.getUrlDecoder().decode(good.removePrefix(Invitation.PREFIX))
        fun encoded(b: ByteArray) = Invitation.PREFIX + Base64.getUrlEncoder().withoutPadding().encodeToString(b)
        fun with(i: Int, v: Int) = encoded(bytes.copyOf().also { it[i] = v.toByte() })
        assertEquals(1, Invitation.decode(with(17, 1))!!.slot) // the bytes below are where the format says
        assertNull(Invitation.decode(with(0, 2))) // a later version
        assertNull(Invitation.decode(with(17, 2))) // slot 2 of 2 riders
        assertNull(Invitation.decode(with(19, 0))) // an interval of 208 ms
        assertNull(Invitation.decode(with(21, 2))) // a name longer than what follows
        assertNull(Invitation.decode(encoded(bytes.copyOf(10)))) // cut short
        assertNull(Invitation.decode(encoded(bytes + 0.toByte()))) // more than the name
        assertNull(Invitation.decode(good.dropLast(2))) // not base64
        assertNull(Invitation.decode("LC2." + good.removePrefix(Invitation.PREFIX)))
        assertNull(Invitation.decode("hello"))
    }
}
