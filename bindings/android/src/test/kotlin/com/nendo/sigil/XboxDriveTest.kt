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
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/**
 * An Xbox save restored into xemu's drive image through JNI: an access that can write part of a
 * file gets only the parts that changed, one that can't gets the whole file, and both leave the
 * same image. Skips without the host library, as [FileAccessTest] does, and without the sample.
 */
class XboxDriveTest {
    companion object {
        @BeforeClass
        @JvmStatic
        fun hostLibrary() = FileAccessTest.hostLibrary()

        private val SAMPLE = File("../../tests/fixtures/saves/xbox/files/xemu-blank/xbox_hdd.qcow2")
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

    /** java.io.File access that counts its writes and can be told it can't write part of a file. */
    private class Counting(private val parts: Boolean) : SigilFileAccess by PosixFileAccess {
        var wholeWrites = 0
        var partWrites = 0
        var partBytes = 0L

        override val writesParts: Boolean get() = parts

        override fun write(root: String, path: String, data: ByteArray): Boolean {
            wholeWrites++
            return PosixFileAccess.write(root, path, data)
        }

        override fun writeAt(root: String, path: String, offset: Long, data: ByteArray): Boolean {
            partWrites++
            partBytes += data.size
            return PosixFileAccess.writeAt(root, path, offset, data)
        }
    }

    private fun restoreWith(access: Counting): File {
        val root = Files.createTempDirectory("sigil-xbox").toFile()
        val drive = File(root, "xbox_hdd.qcow2")
        SAMPLE.copyTo(drive)
        val restored = Sigil.restore(unit(), halo, "xemu", "Halo (USA).iso", root.path, fileAccess = access)
        val back = Sigil.collect(halo, "xemu", "Halo (USA).iso", root.path, fileAccess = access)
        assertEquals(restored.identityHash, back.identityHash)
        return drive
    }

    @Test
    fun aDriveImageIsWrittenInPartsOrWholeAndComesOutTheSame() {
        assumeTrue("Xbox drive sample missing", SAMPLE.exists())
        val inParts = Counting(true)
        val whole = Counting(false)
        val first = restoreWith(inParts)
        val second = restoreWith(whole)

        assertEquals(0, inParts.wholeWrites)
        assertTrue(inParts.partWrites > 0)
        assertEquals(1, whole.wholeWrites)
        assertEquals(0, whole.partWrites)
        assertArrayEquals(second.readBytes(), first.readBytes())
    }

    @Test
    fun theAccessEveryCallUsesWritesParts() {
        assumeTrue("Xbox drive sample missing", SAMPLE.exists())
        assertTrue(PosixFileAccess.writesParts)
        val root = Files.createTempDirectory("sigil-xbox").toFile()
        val drive = File(root, "xbox_hdd.qcow2")
        SAMPLE.copyTo(drive)
        Sigil.restore(unit(), halo, "xemu", "Halo (USA).iso", root.path)
        assertArrayEquals(restoreWith(Counting(false)).readBytes(), drive.readBytes())
    }

    @Test
    fun aRestoreWithoutADriveImageNamesTheOneItNeeds() {
        val root = Files.createTempDirectory("sigil-xbox-empty").toFile()
        try {
            Sigil.restore(unit(), halo, "xemu", "Halo (USA).iso", root.path)
            fail("a restore with no drive image was taken")
        } catch (e: SigilException) {
            assertEquals(SigilException.NO_TARGET, e.code)
            assertEquals("xbox_hdd.qcow2", e.problem)
        }
    }
}
