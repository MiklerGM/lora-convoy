package io.github.miklergm.convoy

import java.nio.ByteBuffer
import java.security.SecureRandom
import java.util.Base64
import javax.crypto.Cipher
import javax.crypto.spec.SecretKeySpec

/**
 * One rider's invitation to a group (01, wishes): a string the organiser sends each rider, from
 * which the rider's app sets up their board. It carries the group's key, so whoever holds it can
 * read and forge the group's positions; a key per ride limits what a forwarded one costs (06).
 *
 * `LC1.` and the base64url of: version (1), key (16), slot (1), riders (1), interval in ms (2,
 * big-endian), the name's length (1) and the name in UTF-8. Versioned like every interface (D3).
 */
data class Invitation(
    val key: ByteArray,
    val slot: Int,
    val riders: Int,
    val intervalMs: Int,
    val name: String,
) {
    init {
        require(key.size == 16) { "a group key is 16 bytes" }
        require(riders in 1..32 && slot in 0 until riders) { "slot $slot of $riders riders" }
        require(intervalMs in 500..60000) { "interval $intervalMs ms" }
        require(name.toByteArray(Charsets.UTF_8).size <= MAX_NAME) { "a name of at most $MAX_NAME bytes" }
    }

    fun encode(): String {
        val nameBytes = name.toByteArray(Charsets.UTF_8)
        val b = ByteBuffer.allocate(22 + nameBytes.size)
            .put(VERSION.toByte()).put(key).put(slot.toByte()).put(riders.toByte())
            .putShort(intervalMs.toShort()).put(nameBytes.size.toByte()).put(nameBytes)
        return PREFIX + Base64.getUrlEncoder().withoutPadding().encodeToString(b.array())
    }

    /** The board's commands for this invitation (firmware/src/api.h). */
    fun commands(): List<String> = listOf("KEY ${key.toHex()}", "SLOT $slot", "INTERVAL $intervalMs")

    /** The tag the board shows as group= once it has this key. */
    fun groupTag(): Int = groupTag(key)

    override fun equals(other: Any?) = other is Invitation && other.encode() == encode()
    override fun hashCode() = encode().hashCode()
    override fun toString() = "Invitation($name, slot $slot of $riders)"

    companion object {
        const val PREFIX = "LC1."
        const val VERSION = 1
        const val MAX_NAME = 32

        /** Null for anything that is not an invitation this app reads; never a partial one. */
        fun decode(text: String): Invitation? {
            val s = text.trim()
            if (!s.startsWith(PREFIX)) return null
            val bytes = try {
                Base64.getUrlDecoder().decode(s.substring(PREFIX.length))
            } catch (e: IllegalArgumentException) {
                return null
            }
            if (bytes.size < 22 || bytes[0].toInt() != VERSION) return null
            val b = ByteBuffer.wrap(bytes)
            b.get()
            val key = ByteArray(16).also { b.get(it) }
            val slot = b.get().toInt() and 0xFF
            val riders = b.get().toInt() and 0xFF
            val interval = b.getShort().toInt() and 0xFFFF
            val nameLen = b.get().toInt() and 0xFF
            if (b.remaining() != nameLen) return null
            val name = ByteArray(nameLen).also { b.get(it) }.toString(Charsets.UTF_8)
            return try {
                Invitation(key, slot, riders, interval, name)
            } catch (e: IllegalArgumentException) {
                null
            }
        }

        /** A new group: a fresh key, and one invitation per slot. */
        fun newGroup(name: String, riders: Int, intervalMs: Int, random: SecureRandom = SecureRandom()): List<Invitation> {
            val key = ByteArray(16).also { random.nextBytes(it) }
            return (0 until riders).map { Invitation(key, it, riders, intervalMs, name) }
        }

        /** The firmware's group tag (codec.c): the first byte of AES-128 under the key of 0xFF "convoy-tag" 0... */
        @Suppress("GetInstance") // ECB on one fixed block: a value derived from the key, no message encrypted
        fun groupTag(key: ByteArray): Int {
            val block = ByteArray(16)
            block[0] = 0xFF.toByte()
            "convoy-tag".toByteArray(Charsets.US_ASCII).copyInto(block, 1)
            val aes = Cipher.getInstance("AES/ECB/NoPadding")
            aes.init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, "AES"))
            return aes.doFinal(block)[0].toInt() and 0xFF
        }

        private fun ByteArray.toHex() = joinToString("") { "%02x".format(it) }
    }
}
