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
    val features: Int = 0
) {
    val source: Source get() = Source.fromCode(sourceCode)
    val usage: Usage get() = Usage.fromCode(usageCode)

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

    companion object {
        const val FEATURE_RTC = 1
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
    val contentHash: String
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
 *
 * For Switch, pass `prodKeysPath` to enable encrypted-NCA decryption.
 * Calls block on I/O — invoke from a background thread.
 */
object Sigil {
    init {
        System.loadLibrary("sigil-jni")
    }

    @JvmStatic external fun nativeVersion(): String

    @JvmStatic external fun nativeExtract(
        path: String,
        platformSlug: String?,
        prodKeysPath: String?
    ): SigilResult?

    @JvmStatic external fun nativeResolveSaveUnit(
        layout: String,
        platformSlug: String?,
        contentName: String,
        titleId: String?,
        saveId: String?,
        features: Int,
        optionKeys: Array<String>,
        optionValues: Array<String>,
        listing: Array<String>,
        rootPath: String?
    ): SigilSaveUnit?

    @JvmStatic external fun nativeLayoutSubdirs(layout: String): Array<String>

    fun extract(path: String, platformSlug: String? = null, prodKeysPath: String? = null): SigilResult? =
        nativeExtract(path, platformSlug, prodKeysPath)

    /**
     * Resolves the save unit for [contentName] under a save root already listed by the
     * caller as [listing] (paths relative to the root, '/' separated). Pass [rootPath] to
     * have the members opened and hashed; leave it null to resolve names only.
     */
    fun resolveSaveUnit(
        layout: String,
        platformSlug: String?,
        contentName: String,
        titleId: String? = null,
        saveId: String? = null,
        features: Int = 0,
        options: Map<String, String> = emptyMap(),
        listing: List<String>,
        rootPath: String? = null
    ): SigilSaveUnit? = nativeResolveSaveUnit(
        layout,
        platformSlug,
        contentName,
        titleId,
        saveId,
        features,
        options.keys.toTypedArray(),
        options.values.toTypedArray(),
        listing.toTypedArray(),
        rootPath
    )

    /** Subfolders under the save root a layout writes into, so the caller knows what to list. */
    fun layoutSubdirs(layout: String): List<String> = nativeLayoutSubdirs(layout).toList()
}
