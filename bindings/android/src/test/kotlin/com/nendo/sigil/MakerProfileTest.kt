// SPDX-License-Identifier: MPL-2.0
package com.nendo.sigil

import java.io.File
import java.nio.ByteBuffer
import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.fail
import org.junit.Assume.assumeTrue
import org.junit.BeforeClass
import org.junit.Test

/**
 * [SigilSyncResult.makerProfile] through JNI: a Xenia Canary save's header names the profile it
 * was made under, and collect reports it.
 *
 * The tests need the host build of libsigil-jni (docs/building.md, "Bindings"). Without it they
 * skip, so an app that includes this module runs its own tests unaffected; sigil's own harness
 * sets `sigil.requireHostJni` so a missing library fails there instead.
 */
class MakerProfileTest {
    companion object {
        @BeforeClass
        @JvmStatic
        fun hostLibrary() {
            val loaded = try {
                System.loadLibrary("sigil-jni")
                true
            } catch (e: UnsatisfiedLinkError) {
                false
            }
            if (!loaded && System.getProperty("sigil.requireHostJni") == "true") {
                fail("no host build of libsigil-jni on java.library.path")
            }
            assumeTrue("no host build of libsigil-jni on java.library.path", loaded)
        }
    }

    private val forza = SigilResult.persisted("xbox360", "4D530AA4", "4D530AA4", 0)
    private val xuid = "E030000043414231"

    /**
     * Forza Horizon 2's header as Canary writes one for a save it makes: the container header
     * padded to 0xA000, the maker's XUID as profile_id at 0x371 (xcontent/xcontent_package.cc
     * InitializePackageHeader at 27d11a2).
     */
    private fun canaryHeader(): ByteArray {
        val h = ByteBuffer.allocate(0xA000)
        h.put(0, "CON ".toByteArray())
        h.putInt(0x340, 0x971A)
        h.putInt(0x344, 1)
        h.putInt(0x348, 2)
        h.putInt(0x360, 0x4D530AA4)
        h.putLong(0x371, xuid.toULong(16).toLong())
        h.put(0x971A, "ForzaProfile".toByteArray())
        return h.array()
    }

    private fun write(root: File, path: String, data: ByteArray) {
        val file = File(root, path)
        file.parentFile.mkdirs()
        file.writeBytes(data)
    }

    @Test
    fun collectReportsTheProfileACanaryHeaderNames() {
        val root = Files.createTempDirectory("sigil-maker").toFile()
        try {
            write(root, "content/$xuid/FFFE07D1/00010000/$xuid/Account", ByteArray(404) { 0xA5.toByte() })
            write(root, "content/$xuid/4D530AA4/00000001/ForzaProfile/PlayerDatabase", "progress".toByteArray())
            write(root, "content/$xuid/4D530AA4/Headers/00000001/ForzaProfile.header", canaryHeader())
            val unit = Sigil.collect(forza, "xenia_canary", "Forza Horizon 2.iso", root.path)
            assertEquals(xuid, unit.makerProfile)
        } finally {
            root.deleteRecursively()
        }
    }
}
