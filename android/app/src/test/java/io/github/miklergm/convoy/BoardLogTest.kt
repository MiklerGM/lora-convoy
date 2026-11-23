package io.github.miklergm.convoy

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.nio.file.Files
import java.time.LocalDateTime

class BoardLogTest {

    @Test
    fun `a day to a file, as the cable's logger writes it, appended, a week kept, no key`() {
        val dir = Files.createTempDirectory("boardlog").toFile()
        File(dir, "convoy-2026-11-08.log").writeText("old\n")
        File(dir, "convoy-2026-11-09.log").writeText("a week\n")
        File(dir, "notes.txt").writeText("not ours\n")
        val log = BoardLog(dir)
        val at = LocalDateTime.of(2026, 11, 16, 10, 12, 33, 123_000_000)
        log.write("TX counter=1 len=34 ok", at)
        log.write("> GROUP 000102030405060708090a0b0c0d0e0f 0 3 2000", at)
        log.close()
        // a service started again the same day goes on in the same file
        BoardLog(dir).apply { write("# connected", at.plusSeconds(1)); close() }
        assertEquals(
            listOf("10:12:33.123 TX counter=1 len=34 ok", "10:12:33.123 > GROUP <key> 0 3 2000", "10:12:34.123 # connected"),
            File(dir, "convoy-2026-11-16.log").readLines(),
        )
        assertFalse(File(dir, "convoy-2026-11-08.log").exists())
        assertTrue(File(dir, "convoy-2026-11-09.log").exists() && File(dir, "notes.txt").exists())
        // past midnight, the next file, and the 9th a week old; Share log offers the days kept, the
        // newest first and ticked, no other file
        BoardLog(dir).apply { write("STATUS proto=8", at.plusDays(1)); close() }
        assertEquals(listOf("convoy-2026-11-17.log", "convoy-2026-11-16.log"), BoardLog.days(dir).map { it.name })
        dir.deleteRecursively()
    }
}
