// SPDX-License-Identifier: MPL-2.0
package com.nendo.sigil

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.fail
import org.junit.Test

/**
 * Collect and restore through a caller's [SigilFileAccess]. The roots are names no file system
 * holds, so a call that reached for java.io.File or fopen would find nothing.
 */
class FileAccessTest {
    /** Files in memory under one root, keyed by root-relative path; records every path it is handed. */
    private class MemoryAccess(val files: MutableMap<String, ByteArray> = HashMap()) : SigilFileAccess {
        val paths = ArrayList<String>()
        var unreadable: String? = null
        var listFails = false

        override fun list(root: String, path: String): List<SigilFileEntry>? {
            paths.add(path)
            if (listFails) throw java.io.IOException("listing refused")
            val prefix = if (path.isEmpty()) "" else "$path/"
            val entries = files.keys.filter { it.startsWith(prefix) }.map { it.removePrefix(prefix) }
            if (entries.isEmpty() && path.isNotEmpty()) return null
            return entries.map { rest ->
                val slash = rest.indexOf('/')
                if (slash < 0) SigilFileEntry(rest, false) else SigilFileEntry(rest.substring(0, slash), true)
            }.distinct()
        }

        override fun read(root: String, path: String): ByteArray? {
            paths.add(path)
            return if (path == unreadable) null else files[path]
        }

        override fun write(root: String, path: String, data: ByteArray): Boolean {
            paths.add(path)
            files[path] = data
            return true
        }

        override fun remove(root: String, path: String): Boolean {
            paths.add(path)
            return files.remove(path) != null || path.endsWith("/")
        }
    }

    private val cross = SigilResult.persisted("psx", "SLUS-01041", "SLUS-01041", 0)

    /** A raw PS1 card holding each (directory name, block count) in order. */
    private fun ps1Card(vararg saves: Pair<String, Int>): ByteArray {
        val card = ByteArray(128 * 1024)
        card[0] = 'M'.code.toByte()
        card[1] = 'C'.code.toByte()
        for (frame in 1 until 16) {
            card[frame * 128] = 0xA0.toByte()
            card[frame * 128 + 8] = 0xFF.toByte()
            card[frame * 128 + 9] = 0xFF.toByte()
        }
        var block = 1
        for ((name, blocks) in saves) {
            for (i in 0 until blocks) {
                val frame = (block + i) * 128
                val state = if (i == 0) 0x51 else if (i == blocks - 1) 0x53 else 0x52
                val link = if (i == blocks - 1) 0xFFFF else block + i
                val size = if (i == 0) blocks * 8192 else 0
                for (k in 0 until 4) card[frame + k] = (state shr (8 * k)).toByte()
                for (k in 0 until 4) card[frame + 4 + k] = (size shr (8 * k)).toByte()
                card[frame + 8] = link.toByte()
                card[frame + 9] = (link shr 8).toByte()
                if (i == 0) name.toByteArray(Charsets.US_ASCII).copyInto(card, frame + 10)
            }
            block += blocks
        }
        return card
    }

    private fun source() = MemoryAccess(
        hashMapOf("Chrono Cross.srm" to ps1Card("BASLUSP01041CROSS" to 2, "BASCUS-94426SLOTS" to 1))
    )

    @Test
    fun collectAndRestoreGoThroughTheCallersAccess() {
        val from = source()
        val unit = Sigil.collect(cross, "pcsx_rearmed", "Chrono Cross.cue", "mem:/source", fileAccess = from)
        val data = unit.data
        assertNotNull("collect found no saves through the caller's access", data)

        val to = MemoryAccess()
        Sigil.restore(data!!, cross, "pcsx_rearmed", "Chrono Cross.cue", "mem:/target", fileAccess = to)
        val written = to.files["Chrono Cross.srm"]
        assertNotNull("restore wrote nothing through the caller's access", written)

        val back = Sigil.collect(cross, "pcsx_rearmed", "Chrono Cross.cue", "mem:/target", fileAccess = to)
        assertEquals(unit.identityHash, back.identityHash)
        assertArrayEquals(data, back.data)

        val card = Sigil.listCard("mem:/target/Chrono Cross.srm", to)
        assertEquals(listOf("BASLUSP01041CROSS"), card.entries.map { it.name })

        for (path in from.paths + to.paths) {
            assertFalse("absolute path $path", path.startsWith("/"))
            assertFalse("path $path leaves the root", path.split('/').contains(".."))
        }
    }

    @Test
    fun aListedFileThatWontReadIsAnIoError() {
        val from = source()
        from.unreadable = "Chrono Cross.srm"
        expectIo { Sigil.collect(cross, "pcsx_rearmed", "Chrono Cross.cue", "mem:/source", fileAccess = from) }
    }

    @Test
    fun aListingThatFailsIsAnIoError() {
        val from = source()
        from.listFails = true
        expectIo { Sigil.collect(cross, "pcsx_rearmed", "Chrono Cross.cue", "mem:/source", fileAccess = from) }
    }

    @Test
    fun aWriteThatFailsIsAnIoError() {
        val unit = Sigil.collect(cross, "pcsx_rearmed", "Chrono Cross.cue", "mem:/source", fileAccess = source()).data!!
        val refusing = object : SigilFileAccess by MemoryAccess() {
            override fun write(root: String, path: String, data: ByteArray): Boolean = false
        }
        expectIo { Sigil.restore(unit, cross, "pcsx_rearmed", "Chrono Cross.cue", "mem:/target", fileAccess = refusing) }
    }

    private fun expectIo(call: () -> Unit) {
        try {
            call()
            fail("expected SigilException.IO")
        } catch (e: SigilException) {
            assertEquals(SigilException.IO, e.code)
        }
    }
}
