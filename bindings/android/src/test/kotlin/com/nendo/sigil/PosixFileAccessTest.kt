// SPDX-License-Identifier: MPL-2.0
package com.nendo.sigil

import java.nio.file.Files
import java.nio.file.attribute.PosixFilePermissions
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * [PosixFileAccess.write] replaces a file whole, through a temporary file renamed over it;
 * [PosixFileAccess.writeAt] writes part of a file that is there, and makes none.
 */
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

    @Test
    fun aPartWriteChangesOnlyItsBytesAndGrowsTheFile() {
        val root = root()
        val drive = java.io.File(root, "hdd.img")
        drive.writeBytes(byteArrayOf(0, 1, 2, 3))
        assertTrue(PosixFileAccess.writeAt(root.path, "hdd.img", 2, byteArrayOf(9, 9, 9)))
        assertArrayEquals(byteArrayOf(0, 1, 9, 9, 9), drive.readBytes())
    }

    @Test
    fun aPartWriteNeverMakesAFile() {
        val root = root()
        assertFalse(PosixFileAccess.writeAt(root.path, "hdd.img", 0, byteArrayOf(1)))
        assertEquals(emptyList<String>(), root.list()!!.toList())
    }
}
