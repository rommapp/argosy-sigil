// SPDX-License-Identifier: MPL-2.0
package com.nendo.sigil

/** Result of a successful title-id extraction. */
data class SigilResult(
    val titleId: String,
    val rawSerial: String,
    val saveId: String,
    val platformSlug: String,
    private val sourceCode: Int,
    private val usageCode: Int,
    val experimental: Boolean = false,
    val features: Int = 0,
    private val switchContentTypeCode: Int = 0,
    val titleVersion: Long = 0
) {
    val source: Source get() = Source.fromCode(sourceCode)
    val usage: Usage get() = Usage.fromCode(usageCode)
    val switchContentType: SwitchContentType get() = SwitchContentType.fromCode(switchContentTypeCode)

    /** The cart carries a real-time clock; a libretro frontend persists it as `<stem>.rtc`. */
    val hasRtc: Boolean get() = (features and FEATURE_RTC) != 0

    enum class Source(val code: Int) {
        Binary(0),
        Filename(1);
        companion object {
            fun fromCode(c: Int): Source = values().firstOrNull { it.code == c } ?: Filename
        }
    }

    /** EXACT vs PREFIX is load-bearing; SPLIT nests saveId as path segments. See README. */
    enum class Usage(val code: Int) {
        FolderExact(0),
        FolderPrefix(1),
        FileExact(2),
        FilePrefix(3),
        FolderSplit(4);
        companion object {
            fun fromCode(c: Int): Usage = values().firstOrNull { it.code == c } ?: FolderExact
        }
    }

    enum class SwitchContentType(val code: Int) {
        Unknown(0),
        Application(1),
        Patch(2),
        Addon(3);
        companion object {
            fun fromCode(c: Int): SwitchContentType = values().firstOrNull { it.code == c } ?: Unknown
        }
    }

    companion object {
        const val FEATURE_RTC = 1

        /**
         * A result rebuilt from stored columns, or built for a platform that has no title id.
         * [platformSlug] selects the save layout; the rest is what [Sigil.extract] returned.
         */
        fun persisted(platformSlug: String, titleId: String, saveId: String, features: Int) =
            SigilResult(titleId, "", saveId, platformSlug, Source.Binary.code, Usage.FolderExact.code, false, features)
    }
}

/** A failed sigil call; [code] is the C error code and the message is `sigil_strerror` for it. */
class SigilException(val code: Int, message: String) : Exception(message)

/** One file of a save unit. [path] is relative to the save root; [entry] is its archive name. */
data class SigilSaveMember(
    val path: String,
    val entry: String,
    private val roleCode: Int,
    val present: Boolean
) {
    val role: Role get() = Role.fromCode(roleCode)

    enum class Role(val code: Int) {
        Primary(0),
        Sidecar(1),
        Rtc(2);
        companion object {
            fun fromCode(c: Int): Role = values().firstOrNull { it.code == c } ?: Sidecar
        }
    }
}

/**
 * Every file under a save root that belongs to one game, the archive shape it
 * travels in, and the content hash the RomM server computes for that artifact.
 */
data class SigilSaveUnit(
    val key: String,
    private val shapeCode: Int,
    val members: List<SigilSaveMember>,
    val expected: List<SigilSaveMember>,
    val unkeyed: List<String>,
    val artifact: String,
    val contentHash: String,
    val identityHash: String
) {
    val shape: Shape get() = Shape.fromCode(shapeCode)

    enum class Shape(val code: Int) {
        None(0),
        Single(1),
        Multi(2),
        Folder(3);
        companion object {
            fun fromCode(c: Int): Shape = values().firstOrNull { it.code == c } ?: None
        }
    }
}

/**
 * Sigil — extract platform-native title IDs from console ROM files, and
 * resolve the save unit an emulator keeps for one under a save root.
 * Calls block on I/O; invoke from a background thread. Failures raise
 * [SigilException]; [extract] alone returns null instead, since an
 * unidentified rom is an ordinary outcome at import.
 */
object Sigil {
    init {
        System.loadLibrary("sigil-jni")
    }

    const val FLAG_FILENAME_FALLBACK = 1
    const val FLAG_3DS_ALLOW_HOMEBREW = 2

    @JvmStatic private external fun nativeVersion(): String

    @JvmStatic private external fun nativeExtract(
        path: String,
        platformSlug: String?,
        prodKeysPath: String?,
        prodKeysText: ByteArray?,
        headerKey: ByteArray?,
        flags: Int
    ): SigilResult

    @JvmStatic private external fun nativeLocateSaves(
        layout: String,
        platformSlug: String?,
        contentPath: String,
        titleId: String?,
        saveId: String?,
        features: Int,
        optionKeys: Array<String>,
        optionValues: Array<String>,
        listing: Array<String>
    ): SigilSaveUnit

    @JvmStatic private external fun nativeHashSaves(
        rootPath: String,
        key: String,
        shape: Int,
        memberPaths: Array<String>,
        memberEntries: Array<String>,
        memberRoles: IntArray
    ): Array<String>

    @JvmStatic private external fun nativeLayoutSubdirs(layout: String): Array<String>
    @JvmStatic private external fun nativeContentStem(contentPath: String): String
    @JvmStatic private external fun nativePlatformSlug(slug: String?): String
    @JvmStatic private external fun nativeLoadHeaderKey(prodKeysPath: String): ByteArray

    fun version(): String = nativeVersion()

    /** The canonical slug for [slug], or `auto` when sigil does not know it. */
    fun platformSlug(slug: String?): String = nativePlatformSlug(slug)

    /** The base name RetroArch names save files after; see README, "Save units". */
    fun contentStem(contentPath: String): String = nativeContentStem(contentPath)

    /** The 32-byte Switch header key read from a prod.keys file. */
    fun loadHeaderKeyFromProdKeys(prodKeysPath: String): ByteArray = nativeLoadHeaderKey(prodKeysPath)

    /**
     * Extracts the title id from [path]. [platformSlug] null sniffs from the extension.
     * Switch decryption takes [prodKeysPath], [prodKeysText] or a 32-byte [headerKey].
     */
    fun extractOrThrow(
        path: String,
        platformSlug: String? = null,
        prodKeysPath: String? = null,
        prodKeysText: ByteArray? = null,
        headerKey: ByteArray? = null,
        filenameFallback: Boolean = true,
        allow3dsHomebrew: Boolean = false
    ): SigilResult {
        require(headerKey == null || headerKey.size == 32) { "headerKey must be 32 bytes" }
        var flags = 0
        if (filenameFallback) flags = flags or FLAG_FILENAME_FALLBACK
        if (allow3dsHomebrew) flags = flags or FLAG_3DS_ALLOW_HOMEBREW
        return nativeExtract(path, platformSlug, prodKeysPath, prodKeysText, headerKey, flags)
    }

    fun extract(
        path: String,
        platformSlug: String? = null,
        prodKeysPath: String? = null,
        prodKeysText: ByteArray? = null,
        headerKey: ByteArray? = null,
        filenameFallback: Boolean = true,
        allow3dsHomebrew: Boolean = false
    ): SigilResult? = try {
        extractOrThrow(path, platformSlug, prodKeysPath, prodKeysText, headerKey, filenameFallback, allow3dsHomebrew)
    } catch (e: SigilException) {
        null
    }

    /**
     * The files under a save root that belong to [game] when [core] runs [contentPath]. Names
     * only, no file is read; docs/kotlin.md defines every input.
     */
    fun locateSaves(
        game: SigilResult,
        core: String,
        contentPath: String,
        saveRoot: String? = null,
        listing: List<String>? = null,
        options: Map<String, String> = emptyMap()
    ): SigilSaveUnit {
        val paths = listing ?: saveRoot?.let { listSaveRoot(java.io.File(it), core) } ?: emptyList()
        return nativeLocateSaves(
            core,
            game.platformSlug,
            contentPath,
            game.titleId.ifEmpty { null },
            game.saveId.ifEmpty { null },
            game.features,
            options.keys.toTypedArray(),
            options.values.toTypedArray(),
            paths.toTypedArray()
        )
    }

    /** [saves] with [SigilSaveUnit.contentHash] and [SigilSaveUnit.identityHash] computed from the files under [saveRoot]. */
    fun hashSaves(saves: SigilSaveUnit, saveRoot: String): SigilSaveUnit {
        if (saves.members.isEmpty()) return saves
        val hashes = nativeHashSaves(
            saveRoot,
            saves.key,
            saves.shape.code,
            saves.members.map { it.path }.toTypedArray(),
            saves.members.map { it.entry }.toTypedArray(),
            saves.members.map { it.role.code }.toIntArray()
        )
        return saves.copy(contentHash = hashes[0], identityHash = hashes[1])
    }

    /** Subfolders under the save root a layout writes into, so the caller knows what to list. */
    fun layoutSubdirs(layout: String): List<String> = nativeLayoutSubdirs(layout).toList()

    /**
     * Root-relative paths of the files directly in [root] plus those under the layout's
     * subfolders, the listing [locateSaves] expects.
     */
    fun listSaveRoot(root: java.io.File, layout: String): List<String> {
        val out = ArrayList<String>()
        root.listFiles()?.forEach { if (it.isFile) out.add(it.name) }
        layoutSubdirs(layout).forEach { subdir ->
            listRecursive(java.io.File(root, subdir), subdir, SUBDIR_LIST_DEPTH, out)
        }
        return out
    }

    private fun listRecursive(dir: java.io.File, relative: String, depth: Int, out: MutableList<String>) {
        if (depth == 0 || !dir.isDirectory) return
        dir.listFiles()?.forEach { file ->
            val rel = "$relative/${file.name}"
            if (file.isFile) out.add(rel)
            else if (file.isDirectory) listRecursive(file, rel, depth - 1, out)
        }
    }

    private const val SUBDIR_LIST_DEPTH = 3
}
