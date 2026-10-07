// SPDX-License-Identifier: MPL-2.0
package com.nendo.sigil

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.BeforeClass
import org.junit.Test

/** The layout catalog, read through JNI. Skips without the host library, as [FileAccessTest] does. */
class LayoutsTest {
    companion object {
        @BeforeClass
        @JvmStatic
        fun hostLibrary() = FileAccessTest.hostLibrary()
    }

    @Test
    fun layoutsReportOptionsAndRegion() {
        val rows = Sigil.layouts("segacd")
        assertEquals("libretro", rows.first().id)
        assertTrue(rows.all { it.platform == "segacd" || it.platform == "" })
        val gpgx = rows.first { it.id == "genesis_plus_gx" }
        assertEquals("genesis_plus_gx_region_detect", gpgx.regionOption)
        val region = gpgx.options.first { it.key == gpgx.regionOption }
        assertEquals("auto", region.default)
        assertTrue("pal" in region.values)
        val ryujinx = Sigil.layouts().first { it.id == "ryujinx" }
        assertEquals("switch", ryujinx.platform)
        assertTrue(ryujinx.profiles && ryujinx.needsExisting)
    }
}
