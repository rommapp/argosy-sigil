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
                                                //   root plus every file under layoutSubdirs(core), four
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

## Memory cards

List the saves on a memory card or backup RAM volume: PS1 cards (the raw
card as `.mcr`, `.mcd` or `.srm`, DexDrive `.gme`, PSP or Vita `.vmp`),
PS2 `.ps2` file cards, GameCube raw cards, Dreamcast VMUs, and Saturn and
Sega CD backup RAM.

```kotlin
Sigil.listCard(
    path: String,               // required. The card file. Its format is detected from the content.
): SigilCardListing             // Raises SigilException (unsupported format code) when the file is not
                                //   a card sigil reads.

data class SigilCardListing(
    format: Format,             // Ps1Raw, Ps1Gme, Ps1Vmp, Ps2, GamecubeRaw, DreamcastVmu, SaturnBackup,
                                //   SegacdBram.
    totalBlocks: Int,
    freeBlocks: Int,            // Blocks a new save can use.
    freeSlots: Int,             // Directory slots a new save can use.
    corruptCount: Int,          // Saves left out because their block chain is broken.
    entries: List<SigilCardEntry>,  // Live saves, in directory order.
    corruptEntries: List<SigilCardEntry>,  // The left-out saves the card still names; blocks is 0.
)

data class SigilCardEntry(
    name: String,               // As stored on the card, e.g. "BASLUSP01041USCHRO00". Bytes outside
                                //   printable ASCII, and '%', read as %XX.
    ownerId: String,            // The game id the save carries, as extract reports it: PS1 and PS2
                                //   "SLUS-01041", GameCube "47465A45". "" when the format has none.
    blocks: Int,                // In the card's own block size.
    firstBlock: Int,
)
```

## Sync

`collect` gathers one game's saves into the unit that travels to RomM;
`restore` puts a unit back and reads it back, removing files where a save
folder holds a save the unit lacks. PS1 and PS2 memory cards, PCSX2 folder
cards, GameCube cards and Dolphin's GCI folder, Saturn and Sega CD backup RAM, and
Dreamcast VMUs work today. `restore` raises `SigilException`
with code `SigilException.NOT_FOUND` for a unit holding none of the game's
saves, and ignores other games' saves inside a unit. What a unit holds, how
Saturn and Sega CD saves find their owner, and how genesis_plus_gx's region
file is picked: [c.md](c.md), "Sync". For Saturn and Sega CD, build the
game with `SigilResult.persisted("saturn", "", "", 0)` or `("segacd", ...)`.

```kotlin
Sigil.collect(
    game: SigilResult, core: String, contentPath: String, saveRoot: String,
    listing: List<String>? = null,          // Root-relative paths; null lists saveRoot.
    options: Map<String, String> = emptyMap(),
    gameIds: List<String> = emptyList(),    // Every id the game's saves may carry: all discs of a set.
    state: ByteArray? = null,               // What the last call returned for this platform and emulator.
    unmanaged: Boolean = false,             // The game runs outside the caller.
    claimed: List<String> = emptyList(),    // Saturn, Sega CD: names from `unowned` the user gave this game.
    companions: List<SigilCompanion> = emptyList(),   // Games whose saves this game reads, in the order they go on.
    repair: Boolean = false,                // Rebuild what SigilException.DAMAGED named, where sigil can.
): SigilSyncResult
    // Raises SigilException.DAMAGED when a file holding the saves is damaged and repair is false,
    //   or isn't a card sigil can read at all (repair doesn't change that).

Sigil.restore(unit: ByteArray, /* same inputs */, overwriteLocal: Boolean = false): SigilSyncResult
    // Each of these raises SigilException and writes nothing: CONFLICT (the saves under
    //   saveRoot changed since the last sync), UNCOLLECTED (a shared volume holds saves no
    //   collect has passed on yet), NO_SPACE (the saves don't fit; `blocksShort` says by how
    //   much), REGION (a companion's save from another region), NO_TARGET (the unit holds a
    //   volume the emulator's settings keep no file for), AMBIGUOUS (more than one file could be
    //   the emulator's card) and DAMAGED. The last five name the save, member or files in
    //   `problem`. c.md, "Sync", has the table.

class SigilCompanion(
    gameIds: List<String>,      // The companion's ids, as for gameIds.
    unit: ByteArray? = null,    // restore: its unit from RomM, or null to leave its saves as they are.
)

class SigilCompanionResult(     // collect: one per companion, in request order.
    data: ByteArray?,           // The companion's unit. null when none of its saves are there.
    contentHash: String,
    identityHash: String,
    changed: Boolean,           // identityHash differs from the companion's last sync.
)

class SigilSyncResult(
    artifact: String,           // File name the unit travels under.
    shape: SigilSaveUnit.Shape,
    data: ByteArray?,           // collect: the unit. null when the game has no saves.
    contentHash: String,        // RomM content_hash of the unit.
    identityHash: String,       // Over the saves themselves; placement and timestamps don't move it.
    changed: Boolean,           // identityHash differs from the last sync.
    conflict: Boolean,
    state: ByteArray,           // Store it once every upload succeeded; pass it back next time.
    holding: ByteArray?,        // Saturn, Sega CD: zip of the saves on a shared volume with no known
                                //   owner. Upload it with the unit.
    unowned: List<String>,      // The names of the saves in holding, escaped as SigilCardEntry.name
                                //   is. Pass them to `claimed` as they are.
    restoreAgain: Boolean,      // Unmanaged: the saves the last restore wrote were overwritten.
                                //   Restore again instead of uploading.
    companions: List<SigilCompanionResult>,
)
```

A companion's saves go on the game's card beside the game's own and stay
out of the game's unit; [c.md](c.md), "Sync", has the rules.

## Helpers

```kotlin
Sigil.contentStem(contentPath: String): String        // Stem the save is named after.
Sigil.layoutSubdirs(core: String): List<String>       // Subfolders the core writes into.
Sigil.listSaveRoot(root: File, core: String): List<String>
Sigil.platformSlug(slug: String?): String             // Canonical slug, or "auto".
Sigil.loadHeaderKeyFromProdKeys(prodKeysPath: String): ByteArray   // 32 bytes.
Sigil.version(): String
```
