// SPDX-License-Identifier: MPL-2.0
package com.nendo.sigil

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Assume.assumeTrue
import org.junit.BeforeClass
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.file.Files
import java.nio.file.attribute.BasicFileAttributes
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/**
 * An Xbox save restored into xemu's drive image through JNI. The drive is written whole unless the
 * restore asks for parts, which an access that writes them then gets alone; both leave the same
 * image. An access that implements [SigilFileAccess] alone, as one written in Java does, is asked
 * for nothing more. Skips without the host library, as [FileAccessTest] does, and without the
 * sample.
 */
class XboxDriveTest {
    companion object {
        @BeforeClass
        @JvmStatic
        fun hostLibrary() = FileAccessTest.hostLibrary()

        private val SAMPLE = File("../../tests/fixtures/saves/xbox/files/xemu-blank/xbox_hdd.qcow2")

        /** xemu has no drive until its hdd_path setting names one. */
        private val DRIVE = mapOf("hdd_path" to "/home/me/.local/share/xemu/xemu/xbox_hdd.qcow2")
    }

    private val halo = SigilResult.persisted("xbox", "MS-004", "4D530004", 0)

    /** A game's save folders zipped as they sit on a drive's E:. */
    private fun unit(): ByteArray {
        val out = ByteArrayOutputStream()
        ZipOutputStream(out).use { zip ->
            for ((name, data) in listOf(
                "UDATA/4d530004/TitleMeta.xbx" to "TitleName=Halo\r\n".toByteArray(),
                "UDATA/4d530004/0A1B2C3D4E5F/blam.sav" to ByteArray(40000) { (it % 251).toByte() }
            )) {
                zip.putNextEntry(ZipEntry(name))
                zip.write(data)
                zip.closeEntry()
            }
        }
        return out.toByteArray()
    }

    /** java.io.File access that counts its writes. */
    private class Counting : SigilPartFileAccess by PosixFileAccess {
        var wholeWrites = 0
        var partWrites = 0

        override fun write(root: String, path: String, data: ByteArray): Boolean {
            wholeWrites++
            return PosixFileAccess.write(root, path, data)
        }

        override fun writeAt(root: String, path: String, offset: Long, data: ByteArray): Boolean {
            partWrites++
            return PosixFileAccess.writeAt(root, path, offset, data)
        }
    }

    /** A save root holding a copy of xemu's blank image. */
    private fun blankRoot(): File {
        val root = Files.createTempDirectory("sigil-xbox").toFile()
        SAMPLE.copyTo(File(root, "xbox_hdd.qcow2"))
        return root
    }

    private fun identity(drive: File): Any? =
        Files.readAttributes(drive.toPath(), BasicFileAttributes::class.java).fileKey()

    private fun restoreWith(access: SigilFileAccess, writesParts: Boolean): File {
        val root = blankRoot()
        val restored = Sigil.restore(unit(), halo, "xemu", "Halo (USA).iso", root.path, options = DRIVE,
            fileAccess = access, writesParts = writesParts)
        val back = Sigil.collect(halo, "xemu", "Halo (USA).iso", root.path, options = DRIVE, fileAccess = access)
        assertEquals(restored.identityHash, back.identityHash)
        return File(root, "xbox_hdd.qcow2")
    }

    @Test
    fun aDriveIsWrittenWholeUnlessTheRestoreAsksForParts() {
        assumeTrue("Xbox drive sample missing", SAMPLE.exists())
        val whole = Counting()
        val inParts = Counting()
        val first = restoreWith(whole, false)
        val second = restoreWith(inParts, true)

        assertEquals(1, whole.wholeWrites)
        assertEquals(0, whole.partWrites)
        assertEquals(0, inParts.wholeWrites)
        assertTrue(inParts.partWrites > 0)
        assertArrayEquals(first.readBytes(), second.readBytes())
    }

    @Test
    fun theAccessEveryCallUsesReplacesTheDriveOrWritesItInPlace() {
        assumeTrue("Xbox drive sample missing", SAMPLE.exists())
        val drives = listOf(false, true).map { writesParts ->
            val root = blankRoot()
            val drive = File(root, "xbox_hdd.qcow2")
            val before = identity(drive)
            Sigil.restore(unit(), halo, "xemu", "Halo (USA).iso", root.path, options = DRIVE, writesParts = writesParts)
            assertEquals("written in place", writesParts, identity(drive) == before)
            drive.readBytes()
        }
        assertArrayEquals(drives[0], drives[1])
    }

    @Test
    fun anAccessWrittenInJavaRestoresTheDriveWhole() {
        assumeTrue("Xbox drive sample missing", SAMPLE.exists())
        val access = JavaFileAccess()
        val drive = restoreWith(access, false)
        assertEquals(1, access.writes)
        assertArrayEquals(restoreWith(Counting(), false).readBytes(), drive.readBytes())

        val root = blankRoot()
        try {
            Sigil.restore(unit(), halo, "xemu", "Halo (USA).iso", root.path, options = DRIVE, fileAccess = access,
                writesParts = true)
            fail("parts were asked of an access that can't write them")
        } catch (e: IllegalArgumentException) {
            assertArrayEquals(SAMPLE.readBytes(), File(root, "xbox_hdd.qcow2").readBytes())
        }
    }

    @Test
    fun aRestoreWithoutADriveImageNamesTheOneItNeeds() {
        val root = Files.createTempDirectory("sigil-xbox-empty").toFile()
        try {
            Sigil.restore(unit(), halo, "xemu", "Halo (USA).iso", root.path, options = DRIVE)
            fail("a restore with no drive image was taken")
        } catch (e: SigilException) {
            assertEquals(SigilException.NO_TARGET, e.code)
            assertEquals("xbox_hdd.qcow2", e.problem)
        }
        try {
            Sigil.restore(unit(), halo, "xemu", "Halo (USA).iso", root.path)
            fail("a restore that names no drive was taken")
        } catch (e: SigilException) {
            assertEquals(SigilException.INVALID_ARG, e.code)
        }
    }
}
