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

/**
 * A failed sigil call; [code] is the C error code and the message is `sigil_strerror` for it.
 * After collect or restore, [problem] names what is at fault when the error has one: the save
 * that didn't fit ([NO_SPACE], with [blocksShort] the blocks it lacked, 0 when a directory slot
 * was missing instead), the companion's save from another region ([REGION]), the damaged file
 * ([DAMAGED]), the unit member the emulator's settings keep no file for ([NO_TARGET]), or the
 * files that could each be the emulator's card, one per line ([AMBIGUOUS]). It is escaped as
 * [SigilCardEntry.name] is.
 */
class SigilException(val code: Int, message: String, val problem: String = "", val blocksShort: Int = 0) :
    Exception(message) {
    companion object {
        /** Nothing identified the file, or a unit holds none of the game's saves. */
        const val NOT_FOUND = -5
        /** The saves on disk changed since the last sync; restore wrote nothing. */
        const val CONFLICT = -9
        const val EXISTS = -10
        const val NO_SPACE = -11
        /** A shared volume holds saves no collect has passed on yet; restore wrote nothing. */
        const val UNCOLLECTED = -12
        /** A file the saves are in is damaged; `repair` rebuilds it where sigil can. */
        const val DAMAGED = -13
        /** A companion's save belongs to another region than the game; restore wrote nothing. */
        const val REGION = -14
        /** The unit holds a volume the emulator's settings keep no file for; restore wrote nothing. */
        const val NO_TARGET = -15
        /** More than one file could be the emulator's card and the options don't say which. */
        const val AMBIGUOUS = -16
    }
}

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
 * One save on a memory card. [ownerId] is the product code it carries, or empty when it has none.
 * [name] holds the stored bytes with those outside printable ASCII, and '%', written as %XX.
 */
data class SigilCardEntry(
    val name: String,
    val ownerId: String,
    val blocks: Int,
    val firstBlock: Int
)

/**
 * The saves on a memory card and the space left on it. [corruptEntries] are the saves left out
 * as corrupt that the card still names, with blocks 0.
 */
data class SigilCardListing(
    private val formatCode: Int,
    val totalBlocks: Int,
    val freeBlocks: Int,
    val freeSlots: Int,
    val corruptCount: Int,
    val entries: List<SigilCardEntry>,
    val corruptEntries: List<SigilCardEntry>
) {
    val format: Format get() = Format.fromCode(formatCode)

    enum class Format(val code: Int) {
        Unknown(0),
        Ps1Raw(1),
        Ps1Gme(2),
        Ps1Vmp(3),
        Ps2(4),
        GamecubeRaw(5),
        DreamcastVmu(6),
        SaturnBackup(7),
        SegacdBram(8);
        companion object {
            fun fromCode(c: Int): Format = values().firstOrNull { it.code == c } ?: Unknown
        }
    }
}

/**
 * What [Sigil.collect] or [Sigil.restore] produced. Store [state] and pass it to the next call
 * for this game.
 *
 * [holding] is, for Saturn and Sega CD, a zip of the saves on a shared volume with no known
 * owner, and [unowned] their names, with bytes outside printable ASCII, and '%', written as
 * %XX. Pass a name back in `claimed` as it is.
 * [restoreAgain] is true in unmanaged mode when the saves the last restore wrote were
 * overwritten: restore again instead of uploading.
 * [companions] has one entry per request companion, in request order.
 */
class SigilSyncResult(
    val artifact: String,
    private val shapeCode: Int,
    val data: ByteArray?,
    val contentHash: String,
    val identityHash: String,
    val changed: Boolean,
    val conflict: Boolean,
    val state: ByteArray,
    val holding: ByteArray?,
    val unowned: List<String>,
    val restoreAgain: Boolean,
    val companions: List<SigilCompanionResult>
) {
    val shape: SigilSaveUnit.Shape get() = SigilSaveUnit.Shape.fromCode(shapeCode)
}

/**
 * A game whose saves this game reads, as a sequel reads its prequel's. [unit] is its unit from
 * RomM for restore, or null to leave its saves as they are.
 */
class SigilCompanion(val gameIds: List<String>, val unit: ByteArray? = null)

/** What collect found of a companion's saves with the game's; [data] is null when none are there. */
class SigilCompanionResult(
    val data: ByteArray?,
    val contentHash: String,
    val identityHash: String,
    val changed: Boolean
)

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

    @JvmStatic private external fun nativeListCard(path: String): SigilCardListing

    @JvmStatic private external fun nativeSync(
        unit: ByteArray?,
        rootPath: String,
        layout: String,
        platformSlug: String?,
        contentPath: String,
        titleId: String?,
        saveId: String?,
        features: Int,
        optionKeys: Array<String>,
        optionValues: Array<String>,
        listing: Array<String>,
        gameIds: Array<String>,
        state: ByteArray?,
        unmanaged: Boolean,
        overwriteLocal: Boolean,
        claimed: Array<String>,
        companionIds: Array<Array<String>>,
        companionUnits: Array<ByteArray?>,
        repair: Boolean
    ): SigilSyncResult
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

    /**
     * [game]'s saves under [saveRoot] gathered into the unit that travels to RomM. Store the
     * result's state once the unit, its holding unit and each changed companion unit reached
     * RomM. Raises [SigilException] with [SigilException.DAMAGED] when a file holding the saves
     * is damaged and [repair] is false, or isn't a card sigil can read at all. docs/kotlin.md
     * defines every input.
     */
    fun collect(
        game: SigilResult,
        core: String,
        contentPath: String,
        saveRoot: String,
        listing: List<String>? = null,
        options: Map<String, String> = emptyMap(),
        gameIds: List<String> = emptyList(),
        state: ByteArray? = null,
        unmanaged: Boolean = false,
        claimed: List<String> = emptyList(),
        companions: List<SigilCompanion> = emptyList(),
        repair: Boolean = false
    ): SigilSyncResult =
        sync(null, game, core, contentPath, saveRoot, listing, options, gameIds, state, unmanaged, false, claimed,
            companions, repair)

    /**
     * Puts [unit], and each companion's unit given, back under [saveRoot] and reads them back.
     * Each of these raises [SigilException] and writes nothing: [SigilException.CONFLICT] when the
     * saves there changed since the last sync and [overwriteLocal] is false;
     * [SigilException.UNCOLLECTED] when a shared Saturn or Sega CD volume holds saves no collect
     * has passed on yet; [SigilException.NO_SPACE] when the saves don't fit;
     * [SigilException.REGION] for a companion's save from another region;
     * [SigilException.NO_TARGET] when the unit holds a volume the emulator's settings keep no
     * file for; [SigilException.AMBIGUOUS] when more than one file could be the emulator's card;
     * and [SigilException.DAMAGED] when a file the saves go in is damaged and [repair]
     * is false, or isn't a card sigil can read at all.
     * The last five name the save, member or files in [SigilException.problem].
     */
    fun restore(
        unit: ByteArray,
        game: SigilResult,
        core: String,
        contentPath: String,
        saveRoot: String,
        listing: List<String>? = null,
        options: Map<String, String> = emptyMap(),
        gameIds: List<String> = emptyList(),
        state: ByteArray? = null,
        unmanaged: Boolean = false,
        overwriteLocal: Boolean = false,
        claimed: List<String> = emptyList(),
        companions: List<SigilCompanion> = emptyList(),
        repair: Boolean = false
    ): SigilSyncResult =
        sync(unit, game, core, contentPath, saveRoot, listing, options, gameIds, state, unmanaged, overwriteLocal,
            claimed, companions, repair)

    private fun sync(
        unit: ByteArray?,
        game: SigilResult,
        core: String,
        contentPath: String,
        saveRoot: String,
        listing: List<String>?,
        options: Map<String, String>,
        gameIds: List<String>,
        state: ByteArray?,
        unmanaged: Boolean,
        overwriteLocal: Boolean,
        claimed: List<String>,
        companions: List<SigilCompanion>,
        repair: Boolean
    ): SigilSyncResult {
        val paths = listing ?: listSaveRoot(java.io.File(saveRoot), core)
        return nativeSync(
            unit,
            saveRoot,
            core,
            game.platformSlug,
            contentPath,
            game.titleId.ifEmpty { null },
            game.saveId.ifEmpty { null },
            game.features,
            options.keys.toTypedArray(),
            options.values.toTypedArray(),
            paths.toTypedArray(),
            gameIds.toTypedArray(),
            state,
            unmanaged,
            overwriteLocal,
            claimed.toTypedArray(),
            companions.map { it.gameIds.toTypedArray() }.toTypedArray(),
            companions.map { it.unit }.toTypedArray(),
            repair
        )
    }

    /** The saves on the memory card at [path]. The card format is detected from its content. */
    fun listCard(path: String): SigilCardListing = nativeListCard(path)

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

    private const val SUBDIR_LIST_DEPTH = 4
}
