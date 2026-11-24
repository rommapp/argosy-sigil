// SPDX-License-Identifier: MPL-2.0
package com.nendo.sigil

import java.security.MessageDigest
import org.junit.Assert.assertEquals
import org.junit.Assert.fail
import org.junit.BeforeClass
import org.junit.Test

/** A unit's identity, read through JNI. Skips without the host library, as [FileAccessTest] does. */
class UnitIdentityTest {
    companion object {
        @BeforeClass
        @JvmStatic
        fun hostLibrary() = FileAccessTest.hostLibrary()
    }

    private fun redRam(): ByteArray {
        val ram = ByteArray(0x8000) { (it * 31 + it / 97).toByte() }
        var sum = 0
        for (i in 0x2598 until 0x3523) sum += ram[i].toInt() and 0xFF
        ram[0x3523] = sum.inv().toByte()
        return ram
    }

    @Test
    fun scratchWritesKeepTheIdentity() {
        val ram = redRam()
        val saved = Sigil.unitIdentity("gb", SigilResult.FEATURE_GB_SCRATCH_GEN1, ram)
        ram[0x0100] = (ram[0x0100].toInt() xor 0x5A).toByte()
        assertEquals(saved, Sigil.unitIdentity("gb", SigilResult.FEATURE_GB_SCRATCH_GEN1, ram))
        val whole = MessageDigest.getInstance("MD5").digest(ram).joinToString("") { "%02x".format(it) }
        assertEquals(whole, Sigil.unitIdentity("gb", 0, ram))
    }

    @Test
    fun anotherPlatformIsRefused() {
        try {
            Sigil.unitIdentity("nes", 0, "save".toByteArray())
            fail("nes answered")
        } catch (e: SigilException) {
            assertEquals(SigilException.UNSUPPORTED_FORMAT, e.code)
        }
    }
}
