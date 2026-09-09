# Kotlin

`com.nendo.sigil.Sigil` from `bindings/android`. Every call blocks on
I/O; run it on `Dispatchers.IO`. Failures raise
`SigilException(code: Int, message: String)`, the C error code and
`sigil_strerror` for it. `extract` alone returns null instead.

## 1. Identify the game

Once, at import. Skip when the result is already stored.

```kotlin
Sigil.extract(
    path: String,                       // required. Rom file.
    platformSlug: String? = null,       // optional. Slug from the README platform table. null sniffs the
                                        //   extension; a bare .iso or .zip needs it.
    prodKeysPath: String? = null,       // optional. Switch prod.keys file.
    prodKeysText: ByteArray? = null,    // optional. Switch prod.keys contents. Wins over prodKeysPath.
    headerKey: ByteArray? = null,       // optional. Switch header key, 32 bytes. Wins over both.
    filenameFallback: Boolean = true,   // optional. Scan the file name when the binary gives nothing;
                                        //   source reports which happened.
    allow3dsHomebrew: Boolean = false,  // optional.
): SigilResult?                         // null when nothing identified the file.
                                        //   extractOrThrow: same inputs, raises SigilException instead.
```

```kotlin
SigilResult(
    titleId: String,                      // "" on gb, gbc, snes.
    rawSerial: String,                    // As found in the binary.
    saveId: String,                       // On-disk name the emulator keys the save by. "" on gb, gbc, snes.
    platformSlug: String,
    source: Source,                       // Binary, Filename.
    usage: Usage,                         // FolderExact, FolderPrefix, FileExact, FilePrefix, FolderSplit.
                                          //   README "usage" says how to apply saveId for each.
    experimental: Boolean,
    switchContentType: SwitchContentType, // Unknown, Application, Patch, Addon.
    titleVersion: Long,                   // Switch only.
    features: Int,                        // Bit set. FEATURE_RTC: cart has a clock. hasRtc reads it.
)
```

Store `titleId`, `saveId`, `platformSlug`, `features`. Rebuild later, or
build for a platform sigil cannot extract (Sega CD returns null). Every
field is passed every time; a new value comes back, nothing is kept
between calls:

```kotlin
SigilResult.persisted(
    platformSlug: String,     // required. Slug from the README platform table, or segacd, fds.
    titleId: String,          // required. Stored titleId, or "" when the platform has none.
    saveId: String,           // required. Stored saveId, or "" when the platform has none.
    features: Int,            // required. Stored features, or 0.
): SigilResult
```

## 2. Locate the saves

At sync time. No file is read.

```kotlin
Sigil.locateSaves(
    game: SigilResult,                          // required. Step 1.
    core: String,                               // required. Libretro core name without _libretro:
                                                //   genesis_plus_gx, mednafen_psx_hw, mame2003_plus. A core
                                                //   without a README layout row gets the default row
                                                //   (<stem>.srm, plus <stem>.rtc when the cart has a clock).
    contentPath: String,                        // required. The path you handed the emulator, verbatim:
                                                //   rom, .m3u, .cue, .chd, or archive.zip#member.ext when a
                                                //   member was loaded. Only the file name part is used.
    saveRoot: String? = null,                   // optional. Directory the emulator writes this game's save
                                                //   into. RetroArch: savefile_directory, plus the core-named
                                                //   subfolder when sort_savefiles_enable is on. Sigil lists
                                                //   it and the subfolders the core writes into.
    listing: List<String>? = null,              // optional. Instead of saveRoot: every file directly in the
                                                //   root plus every file under layoutSubdirs(core), three
                                                //   levels deep, as root-relative / paths. listSaveRoot
                                                //   builds this.
    options: Map<String, String> = emptyMap(),  // optional. Core option key to value, the strings the core
                                                //   defines and RetroArch writes to <core>.opt, never display
                                                //   labels: "genesis_plus_gx_system_bram" to "per game".
                                                //   Pass all of them; only the keys the row names are read.
): SigilSaveUnit                                // Hashes empty.
```

```kotlin
SigilSaveUnit(
    key: String,                        // Stem, or saveId for folder layouts.
    shape: Shape,                       // Single, Multi, Folder. None when nothing is there.
    members: List<SigilSaveMember>,     // Files present.
    expected: List<SigilSaveMember>,    // Files the core should have written but has not: every applicable
                                        //   primary, plus the rtc file when the cart has a clock.
    unkeyed: List<String>,              // Root files shared by every game. Never bundle them.
    artifact: String,                   // File name the upload travels under.
    contentHash: String,                // "" until step 3.
    identityHash: String,               // "" until step 3.
)

SigilSaveMember(
    path: String,      // Root-relative.
    entry: String,     // Archive entry name.
    role: Role,        // Primary, Sidecar, Rtc.
    present: Boolean,
)
```

## 3. Hash the saves

When you need to compare with the server.

```kotlin
Sigil.hashSaves(
    saves: SigilSaveUnit,   // required. Step 2.
    saveRoot: String,       // required. Directory its paths are relative to.
): SigilSaveUnit            // Same unit with contentHash and identityHash filled.
                            //   Raises SigilException (I/O code) when a member cannot be opened.
```

```kotlin
contentHash: String     // What the RomM server computes for the artifact.
identityHash: String    // The same over the non-rtc members. Different contentHash, same
                        //   identityHash: a clock tick, not a new save.
```

## Upload and restore

Upload by `shape`. `Single` sends the member as is. `Multi` zips the
members flat, each under its `entry`. `Folder` zips the `key` folder so
entries read `<key>/<file>`. Name the upload `artifact`. Hash rules and
the layout table: README, "Save units".

Restore by `path`. Unzip a `Multi` artifact so every entry lands at its
member's `path` under the root. Unzip a `Folder` artifact from the
root's parent of the key folder. `expected` says where a primary goes
when the emulator has not created one yet.

## Helpers

```kotlin
Sigil.contentStem(contentPath: String): String        // Stem the save is named after.
Sigil.layoutSubdirs(core: String): List<String>       // Subfolders the core writes into.
Sigil.listSaveRoot(root: File, core: String): List<String>
Sigil.platformSlug(slug: String?): String             // Canonical slug, or "auto".
Sigil.loadHeaderKeyFromProdKeys(prodKeysPath: String): ByteArray   // 32 bytes.
Sigil.version(): String
```
