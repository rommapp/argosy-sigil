// SPDX-License-Identifier: MPL-2.0
package com.nendo.sigil

import java.nio.file.Files
import java.nio.file.attribute.PosixFilePermissions
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** [PosixFileAccess.write] replaces a file whole, through a temporary file renamed over it. */
class PosixFileAccessTest {
    private fun root() = Files.createTempDirectory("sigil-posix").toFile().apply { deleteOnExit() }

    @Test
    fun aWriteReplacesTheFileAndKeepsItsPermissions() {
        val root = root()
        val card = java.io.File(root, "Chrono Cross.srm")
        card.writeBytes(byteArrayOf(1, 2, 3))
        val mode = PosixFilePermissions.fromString("rw-r-----")
        Files.setPosixFilePermissions(card.toPath(), mode)

        assertTrue(PosixFileAccess.write(root.path, "Chrono Cross.srm", byteArrayOf(4, 5)))
        assertArrayEquals(byteArrayOf(4, 5), card.readBytes())
        assertEquals(mode, Files.getPosixFilePermissions(card.toPath()))
        assertEquals(listOf("Chrono Cross.srm"), root.list()!!.toList())
    }

    @Test
    fun aWriteMakesTheFoldersAboveTheFile() {
        val root = root()
        assertTrue(PosixFileAccess.write(root.path, "User/Card A/save.gci", byteArrayOf(7)))
        assertArrayEquals(byteArrayOf(7), java.io.File(root, "User/Card A/save.gci").readBytes())
    }

    @Test
    fun aWriteThatFailsLeavesTheFileAsItWas() {
        val root = root()
        val card = java.io.File(root, "Chrono Cross.srm")
        card.writeBytes(byteArrayOf(1, 2, 3))
        root.setWritable(false)
        try {
            assertFalse(PosixFileAccess.write(root.path, "Chrono Cross.srm", byteArrayOf(4, 5)))
        } finally {
            root.setWritable(true)
        }
        assertArrayEquals(byteArrayOf(1, 2, 3), card.readBytes())
        assertEquals(listOf("Chrono Cross.srm"), root.list()!!.toList())
    }
}
