// SPDX-License-Identifier: MPL-2.0
package com.nendo.sigil

/**
 * Result of a successful title-id extraction.
 *
 * @property n64Header N64 only: the header name, "" when not plain ASCII.
 * @property n64Md5 N64 only: the ROM's MD5 in .z64 byte order, uppercase, as mupen64plus computes it.
 * @property n64Md5N64 N64 only: the ROM's MD5 in .n64 byte order, uppercase, as Project64 computes it.
 * Standalone N64 emulators name saves from these three; store them with the result.
 */
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
    val titleVersion: Long = 0,
    val n64Header: String = "",
    val n64Md5: String = "",
    val n64Md5N64: String = ""
) {
    internal val n64Fields: Array<String> get() = arrayOf(n64Header, n64Md5, n64Md5N64)

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
         * [rawSerial] names pcsx_rearmed's per-disc cards, which follow the boot file as written.
         * The `n64` fields name the standalone N64 emulators' saves.
         */
        fun persisted(
            platformSlug: String,
            titleId: String,
            saveId: String,
            features: Int,
            rawSerial: String = "",
            n64Header: String = "",
            n64Md5: String = "",
            n64Md5N64: String = ""
        ) = SigilResult(
            titleId, rawSerial, saveId, platformSlug, Source.Binary.code, Usage.FolderExact.code, false, features,
            n64Header = n64Header, n64Md5 = n64Md5, n64Md5N64 = n64Md5N64
        )
    }
}

/**
 * A failed sigil call; [code] is the C error code and the message is `sigil_strerror` for it.
 * After collect or restore, [problem] names what is at fault when the error has one: the save
 * that didn't fit ([NO_SPACE], with [blocksShort] the blocks it lacked, 0 when the free blocks
 * were there but a directory slot or a Dreamcast game file's starting blocks weren't), the
 * companion's save from another region ([REGION]), the damaged file ([DAMAGED]), the unit member
 * with no file to go in ([NO_TARGET]), the files that could each be the emulator's card or the
 * profiles that could each take the saves, one per line ([AMBIGUOUS]), or the save Dolphin's GCI
 * folder has no free name for ([EXISTS]). Each line is escaped as [SigilCardEntry.name] is.
 * [profiles] lists every profile the emulator lists, on a layout with profiles: when you don't
 * know which one the user plays as, ask them from it (or [Sigil.listProfiles]) and pass `profile`.
 */
class SigilException(
    val code: Int,
    message: String,
    val problem: String = "",
    val blocksShort: Int = 0,
    val profiles: List<SigilProfile> = emptyList()
) : Exception(message) {
    companion object {
        const val INVALID_ARG = -1
        const val IO = -2
        const val UNKNOWN_PLATFORM = -3
        /** The file isn't in a format sigil reads, or a unit holds a corrupt save. */
        const val UNSUPPORTED_FORMAT = -4
        /** Nothing identified the file, or a unit holds none of the game's saves. */
        const val NOT_FOUND = -5
        const val NEEDS_KEY = -6
        const val CRYPTO = -7
        const val OOM = -8
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
        /**
         * The unit holds a volume or member with no file to go in: the emulator's settings keep
         * none, no profile is there for an account save, or the folder lies outside the save root.
         */
        const val NO_TARGET = -15
        /**
         * More than one file could be the emulator's card, more than one profile and none picked,
         * or more than one emulator folder under the save root.
         */
        const val AMBIGUOUS = -16
        /** The key file doesn't open this content: it lacks the key for its generation, or its header key is wrong. */
        const val KEYS_INCOMPATIBLE = -17
    }
}

/**
 * One file of a save unit. [path] is relative to the save root; [entry] is its archive name.
 * [area] says whose it is on an emulator that keeps saves per user profile.
 */
data class SigilSaveMember(
    val path: String,
    val entry: String,
    private val roleCode: Int,
    val present: Boolean,
    private val areaCode: Int = 0
) {
    val role: Role get() = Role.fromCode(roleCode)
    val area: Area get() = Area.fromCode(areaCode)

    enum class Role(val code: Int) {
        Primary(0),
        Sidecar(1),
        Rtc(2);
        companion object {
            fun fromCode(c: Int): Role = values().firstOrNull { it.code == c } ?: Sidecar
        }
    }

    enum class Area(val code: Int) {
        None(0),
        Account(1),
        Device(2);
        companion object {
            fun fromCode(c: Int): Area = values().firstOrNull { it.code == c } ?: None
        }
    }
}

/** A user profile the emulator lists. [id] is how its save folder is named. */
data class SigilProfile(val id: String, val name: String)

/**
 * A file under the save root the layout would take with other option values: a save kept under
 * another mode or by an older build of the core. Passing [options] takes it. [shared] marks a
 * file every game shares, as in [SigilSaveUnit.unkeyed].
 */
data class SigilSaveAlternate(
    val path: String,
    val shared: Boolean,
    private val optionKeys: List<String>,
    private val optionValues: List<String>
) {
    val options: Map<String, String> get() = optionKeys.zip(optionValues).toMap()
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
    val identityHash: String,
    val alternates: List<SigilSaveAlternate> = emptyList()
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
 * [holding] is, for Saturn, Sega CD and Dreamcast, a zip of the saves on a shared volume with no known
 * owner, and [unowned] their names, with bytes outside printable ASCII, and '%', written as
 * %XX. Pass a name back in `claimed` as it is.
 * [restoreAgain] is true in unmanaged mode when the saves the last restore wrote were
 * overwritten: restore again instead of uploading.
 * [companions] has one entry per request companion, in request order.
 * [profiles] lists every profile the emulator lists on a layout with profiles, and [profile] is
 * the one whose saves were taken or written.
 */
class SigilSyncResult(
    val artifact: String,
    private val shapeCode: Int,
    val data: ByteArray?,
    val contentHash: String,
    val identityHash: String,
    val changed: Boolean,
    val state: ByteArray,
    val holding: ByteArray?,
    val unowned: List<String>,
    val restoreAgain: Boolean,
    val companions: List<SigilCompanionResult>,
    val profiles: List<SigilProfile>,
    val profile: String,
    val alternates: List<SigilSaveAlternate>
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
        rawSerial: String?,
        saveId: String?,
        features: Int,
        n64: Array<String>,
        optionKeys: Array<String>,
        optionValues: Array<String>,
        listing: Array<String>,
        rootPath: String?,
        profile: String?
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
        rawSerial: String?,
        saveId: String?,
        features: Int,
        n64: Array<String>,
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
        repair: Boolean,
        profile: String?
    ): SigilSyncResult
    @JvmStatic private external fun nativeLayoutSubdirs(layout: String): Array<String>
    @JvmStatic private external fun nativeSaveBase(layout: String, path: String): Array<String>
    @JvmStatic private external fun nativeLayoutTop(layout: String): String?
    @JvmStatic private external fun nativeListProfiles(
        layout: String,
        rootPath: String,
        listing: Array<String>
    ): List<SigilProfile>
    @JvmStatic private external fun nativeContentStem(contentPath: String): String
    @JvmStatic private external fun nativePlatformSlug(slug: String?): String
    @JvmStatic private external fun nativeLoadHeaderKey(prodKeysPath: String): ByteArray

    fun version(): String = nativeVersion()

    /** The canonical slug for [slug], or `auto` when sigil does not know it. */
    fun platformSlug(slug: String?): String = nativePlatformSlug(slug)

    /** The base name RetroArch names save files after; see docs/save-units.md, "Stem". */
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
     * The files under a save root that belong to [game] when [core] runs [contentPath]. No save
     * is read; on a layout with profiles the emulator's profile list is, when [saveRoot] is
     * given, and the hashes are filled. docs/quickstart-guides/kotlin.md defines every input.
     */
    fun locateSaves(
        game: SigilResult,
        core: String,
        contentPath: String,
        saveRoot: String? = null,
        listing: List<String>? = null,
        options: Map<String, String> = emptyMap(),
        profile: String? = null
    ): SigilSaveUnit {
        val root = saveRoot?.let { rooted(core, it, profile) }
        val paths = listing ?: root?.let { listSaveRoot(java.io.File(it.first), core) } ?: emptyList()
        return nativeLocateSaves(
            core,
            game.platformSlug,
            contentPath,
            game.titleId.ifEmpty { null },
            game.rawSerial.ifEmpty { null },
            game.saveId.ifEmpty { null },
            game.features,
            game.n64Fields,
            options.keys.toTypedArray(),
            options.values.toTypedArray(),
            paths.toTypedArray(),
            root?.first,
            root?.second ?: profile
        )
    }

    /**
     * On a layout with profiles, the emulator's base folder for [path] and the profile folder
     * [path] lies in ("" for none); collect and restore re-root there themselves. On other
     * layouts, [path] itself and "".
     */
    fun saveBase(layout: String, path: String): Pair<String, String> {
        val out = nativeSaveBase(layout, path)
        return out[0] to out[1]
    }

    /**
     * The profiles the emulator lists around [saveRoot], for asking the user which one they play
     * as when collect or restore raised [SigilException.AMBIGUOUS]. Raises [SigilException] with
     * [SigilException.UNSUPPORTED_FORMAT] for a core whose saves aren't kept per profile.
     */
    fun listProfiles(core: String, saveRoot: String): List<SigilProfile> {
        val (root, _) = rooted(core, saveRoot, null)
        return nativeListProfiles(core, root, listSaveRoot(java.io.File(root), core).toTypedArray())
    }

    /** The folder to list and write under for [saveRoot], and the profile: one given wins. */
    private fun rooted(core: String, saveRoot: String, profile: String?): Pair<String, String?> {
        val (base, implied) = saveBase(core, saveRoot)
        return base to (profile?.ifEmpty { null } ?: implied.ifEmpty { null })
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
     * RomM. Raises [SigilException] with [SigilException.DAMAGED] and
     * [SigilException.AMBIGUOUS] as [restore] does. docs/quickstart-guides/kotlin.md defines every input.
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
        repair: Boolean = false,
        profile: String? = null
    ): SigilSyncResult =
        sync(null, game, core, contentPath, saveRoot, listing, options, gameIds, state, unmanaged, false, claimed,
            companions, repair, profile)

    /**
     * Puts [unit], and each companion's unit given, back under [saveRoot] and reads them back.
     * Each of these raises [SigilException] and writes nothing: [SigilException.CONFLICT] when the
     * saves there changed since the last sync and [overwriteLocal] is false;
     * [SigilException.UNCOLLECTED] when a shared Saturn or Sega CD volume holds saves no collect
     * has passed on yet; [SigilException.NO_SPACE] when the saves don't fit;
     * [SigilException.REGION] for a companion's save from another region;
     * [SigilException.NO_TARGET] when the unit holds a volume or member with no file to go in;
     * [SigilException.AMBIGUOUS] when more than one file could be the emulator's card, or more
     * than one profile could take the saves;
     * and [SigilException.DAMAGED] when a file the saves go in is damaged and [repair]
     * is false, isn't a card sigil can read at all, or holds a corrupt save of the game or a
     * companion (repair changes neither of the last two); [SigilException.EXISTS] when Dolphin's
     * GCI folder has no free name for a new save.
     * The last six name the save, member or files in [SigilException.problem].
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
        repair: Boolean = false,
        profile: String? = null
    ): SigilSyncResult =
        sync(unit, game, core, contentPath, saveRoot, listing, options, gameIds, state, unmanaged, overwriteLocal,
            claimed, companions, repair, profile)

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
        repair: Boolean,
        profile: String?
    ): SigilSyncResult {
        val (root, picked) = rooted(core, saveRoot, profile)
        val paths = listing ?: listSaveRoot(java.io.File(root), core)
        return nativeSync(
            unit,
            root,
            core,
            game.platformSlug,
            contentPath,
            game.titleId.ifEmpty { null },
            game.rawSerial.ifEmpty { null },
            game.saveId.ifEmpty { null },
            game.features,
            game.n64Fields,
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
            repair,
            picked
        )
    }

    /** The saves on the memory card at [path]. The card format is detected from its content. */
    fun listCard(path: String): SigilCardListing = nativeListCard(path)

    /** Subfolders under the save root a layout writes into, so the caller knows what to list. */
    fun layoutSubdirs(layout: String): List<String> = nativeLayoutSubdirs(layout).toList()

    /**
     * Root-relative paths of the files directly in [root] plus those under the layout's
     * subfolders, the listing [locateSaves] expects. On a layout with profiles whose base sits
     * below [root], the subfolders under each such base.
     */
    fun listSaveRoot(root: java.io.File, layout: String): List<String> {
        val out = ArrayList<String>()
        root.listFiles()?.forEach { if (it.isFile) out.add(it.name) }
        val top = nativeLayoutTop(layout)
        val bases = if (top != null && !java.io.File(root, top).isDirectory) {
            basesBelow(root, top).ifEmpty { listOf("") }
        } else {
            listOf("")
        }
        for (base in bases) {
            layoutSubdirs(layout).forEach { subdir ->
                val relative = if (base.isEmpty()) subdir else "$base/$subdir"
                listRecursive(java.io.File(root, relative), relative, SUBDIR_LIST_DEPTH, out)
            }
        }
        return out
    }

    /** Root-relative folders under [root] that hold the layout's [top] folder. */
    private fun basesBelow(root: java.io.File, top: String): List<String> {
        val found = ArrayList<String>()
        fun walk(dir: java.io.File, relative: String, depth: Int) {
            if (depth == 0) return
            dir.listFiles()?.filter { it.isDirectory }?.forEach { child ->
                if (child.name == top) {
                    if (relative.isNotEmpty()) found.add(relative)
                } else {
                    walk(child, if (relative.isEmpty()) child.name else "$relative/${child.name}", depth - 1)
                }
            }
        }
        walk(root, "", BASE_SEARCH_DEPTH)
        return found
    }

    private fun listRecursive(dir: java.io.File, relative: String, depth: Int, out: MutableList<String>) {
        if (depth == 0 || !dir.isDirectory) return
        dir.listFiles()?.forEach { file ->
            val rel = "$relative/${file.name}"
            if (file.isFile) out.add(rel)
            else if (file.isDirectory) listRecursive(file, rel, depth - 1, out)
        }
    }

    private const val SUBDIR_LIST_DEPTH = 12
    private const val BASE_SEARCH_DEPTH = 5
}
