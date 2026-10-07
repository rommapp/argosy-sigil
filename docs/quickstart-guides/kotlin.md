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
    platformSlug: String? = null,       // optional. Slug from identification.md. null sniffs the
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
    titleId: String,                      // "" on gb, gbc, snes. A Switch update or DLC gives its game's id.
    rawSerial: String,                    // As found in the binary; a Switch update's or DLC's own id.
    saveId: String,                       // On-disk name the emulator keys the save by. "" on gb, gbc, snes.
    platformSlug: String,
    source: Source,                       // Binary, Filename.
    usage: Usage,                         // FolderExact, FolderPrefix, FileExact, FilePrefix, FolderSplit.
                                          //   identification.md, "usage", says how to apply saveId for each.
    experimental: Boolean,
    switchContentType: SwitchContentType, // Unknown, Application, Patch, Addon.
    titleVersion: Long,                   // Switch only.
    features: Int,                        // Bit set. FEATURE_RTC: cart has a clock. hasRtc reads it.
    n64Header: String,                    // N64 only. The cart's name; "" when not plain ASCII.
    n64Md5: String,                       // N64 only. The ROM's MD5 in .z64 byte order, uppercase.
    n64Md5N64: String,                    // N64 only. The same in .n64 byte order, as Project64 hashes it.
)
```

For an N64 ROM, `extract` reads the whole file to fill the two MD5s; the
standalone N64 emulators name saves from them.

A Switch XCI or NSP needs keys: without them `extractOrThrow` raises
`SigilException.NEEDS_KEY`, and with keys that don't open the content (a
key file older than the dump, or a wrong header key) `KEYS_INCOMPATIBLE`;
`extract` returns null for both.

Store `titleId`, `saveId`, `rawSerial`, `platformSlug`, `features` and,
for N64, the three `n64*` fields. Rebuild the result from them later, or
build one for a platform sigil cannot extract (Sega CD returns null):

```kotlin
SigilResult.persisted(
    platformSlug: String,     // required. Slug from identification.md, or segacd, fds.
    titleId: String,          // required. Stored titleId, or "" when the platform has none.
    saveId: String,           // required. Stored saveId, or "" when the platform has none.
    features: Int,            // required. Stored features, or 0.
    rawSerial: String = "",   // optional. Stored rawSerial; pcsx_rearmed's serial cards are named from it.
    n64Header: String = "",   // optional. The stored N64 fields; the standalone N64 emulators'
    n64Md5: String = "",      //   saves are named from them.
    n64Md5N64: String = "",
): SigilResult
```

## 2. Locate the saves

At sync time. No save is read.

```kotlin
Sigil.locateSaves(
    game: SigilResult,                          // required. Step 1.
    core: String,                               // required. Layout id of the emulator running the game;
                                                //   see below.
    contentPath: String,                        // required. The path you handed the emulator, verbatim:
                                                //   rom, .m3u, .cue, .chd, or archive.zip#member.ext when a
                                                //   member was loaded. Only the file name part is used.
    saveRoot: String? = null,                   // optional. Directory the emulator writes this game's save
                                                //   into. RetroArch: savefile_directory, plus the core-named
                                                //   subfolder when sort_savefiles_enable is on. Sigil lists
                                                //   it and the subfolders the core writes into.
    listing: List<String>? = null,              // optional. Instead of saveRoot: every file directly in the
                                                //   root plus every file under layoutSubdirs(core), twelve
                                                //   levels deep, as root-relative / paths. listSaveRoot
                                                //   builds this.
    options: Map<String, String> = emptyMap(),  // optional. Core option key to value, the strings the core
                                                //   defines and RetroArch writes to <core>.opt, never display
                                                //   labels: "genesis_plus_gx_system_bram" to "per game".
                                                //   Pass all of them; only the keys the row names are read.
    profile: String? = null,                    // optional. Layouts with profiles: the profile whose saves
                                                //   to take, by SigilProfile.id.
    fileAccess: SigilFileAccess = PosixFileAccess,  // optional. How sigil reaches the files; see File
                                                //   access below.
): SigilSaveUnit                                // Hashes empty, except on a layout with profiles given a
                                                //   saveRoot, where sigil reads the profile list and fills
                                                //   them.
```

`core` names the emulator, because each keeps its saves differently. For
a libretro core, pass the core's name without `_libretro`
(`genesis_plus_gx`, `mednafen_psx_hw`). For a standalone emulator, pass
its layout id (`dolphin_standalone`, `pcsx2_standalone`, `eden`).
[platforms/](../platforms/README.md#layouts) lists every id with its
emulator. An id with no row gets the libretro default (`<stem>.srm`, plus
`<stem>.rtc` when the cart has a clock), which fits an unlisted libretro
core but names nothing an unlisted standalone emulator writes.

If `alternates` is not empty, the root holds this game's saves under
other option values, such as Beetle Saturn's `.bkr` from a build older
than its save-method option. Ask the user, or call again with each
alternate's `options`; sigil never picks one itself.

A listing you build yourself has to recurse the way `listSaveRoot` does,
into folders as well as files. Sigil tells a PCSX2 folder card from a file
card by its contents: `memcards/Mcd001.ps2` is a folder card when the
listing holds paths below it (`memcards/Mcd001.ps2/_pcsx2_superblock`), and
a file card when it is listed alone.

On a folder layout (the Switch, Wii U, PS Vita, PS3, PSP and 3DS
emulators; [platforms/](../platforms/README.md#layouts) lists them),
`saveRoot` may be any folder around the emulator's own: its base (the
folder holding `nand/`, `bis/`, `mlc01/`, `ux0/`, `dev_hdd0/`, `PSP/` or
`sdmc/`), a folder above it, or one inside it such as a profile's save
folder. Sigil re-roots at the base and takes the profile the root lies in.
Member paths are then relative to the base, which `saveBase` returns.
[save-units.md](../save-units.md#profiles) has the folders and the profile rules.

With two or more profiles and none picked, `collect` and `restore` raise
`SigilException.AMBIGUOUS`. If you don't know which profile the user plays
as, ask them: `listProfiles(core, saveRoot)` lists the emulator's profiles
without a game, and the exception's `profiles` holds the same list. Pass
the answer as `profile` and keep it per user.

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
    alternates: List<SigilSaveAlternate>, // Listed files other option values would take.
)

SigilSaveAlternate(
    path: String,                 // Root-relative.
    shared: Boolean,              // A file every game shares.
    options: Map<String, String>, // The values that take it; pass them as options to read it.
)

SigilSaveMember(
    path: String,      // Root-relative.
    entry: String,     // Archive entry name.
    role: Role,        // Primary, Sidecar, Rtc.
    present: Boolean,
    area: Area,        // Account or Device on a layout with profiles, None elsewhere.
)
```

## 3. Hash the saves

When you need to compare with the server.

```kotlin
Sigil.hashSaves(
    saves: SigilSaveUnit,   // required. Step 2.
    saveRoot: String,       // required. Directory its paths are relative to.
    fileAccess: SigilFileAccess = PosixFileAccess,  // optional.
): SigilSaveUnit            // Same unit with contentHash and identityHash filled.
                            //   Raises SigilException (I/O code) when a member cannot be opened.
```

```kotlin
contentHash: String     // What RomM stores for the artifact and compares against.
identityHash: String    // The same over the saves alone, leaving out the clock file.
```

Compare `contentHash` with RomM's. `identityHash` is sigil's own: RomM
never sees it, and sync's `changed` is already built on it, so a clock
that ticked doesn't read as a new save. Most clients never read it.

## Upload and restore

`collect` and `restore` (see [Sync](#sync)) build and unpack the
artifact for you. One round trip, with `romm` and `store` standing for
your own server client and storage, on `Dispatchers.IO`:

```kotlin
val core = "pcsx_rearmed"
val content = "Chrono Cross (USA).cue"
val root = "/storage/emulated/0/RetroArch/saves/psx"

// After the game closes: collect, upload what changed, then keep the state.
val collected = Sigil.collect(game, core, content, root, state = store.state(game))
val data = collected.data
if (collected.changed && data != null) {
    romm.uploadSave(game, collected.artifact, data, collected.contentHash)
    collected.holding?.let { romm.uploadSave(game, "holding.zip", it, null) }
    store.setState(game, collected.state)     // only once every upload succeeded
}

// Before the next launch: put the server's save back.
val unit = romm.downloadSave(game)
val restored = try {
    Sigil.restore(unit, game, core, content, root, state = store.state(game))
} catch (e: SigilException) {
    when (e.code) {
        // The saves on disk changed since the last sync. Ask the user, then:
        SigilException.CONFLICT ->
            Sigil.restore(unit, game, core, content, root, state = store.state(game), overwriteLocal = true)
        // More than one profile could take the saves. Ask which is theirs:
        SigilException.AMBIGUOUS ->
            Sigil.restore(unit, game, core, content, root, state = store.state(game),
                profile = askUser(e.profiles).id)
        else -> throw e
    }
}
store.setState(game, restored.state)
```

Upload `data` under the name `artifact`; RomM computes the same
`contentHash`. Pass back the `state` the last call returned every time,
so sigil can tell a local change from its own last restore.

### Without collect and restore

`locateSaves` doesn't build an upload. It gives you `members`, the files
that make up the game's save, and you package them yourself:

1. Upload by `shape`. `Single`: send the one member's file as it is.
   `Multi`: zip the members yourself, each stored at the zip's root
   under its `entry`. `Folder`: zip the `key` folder so entries read
   `<key>/<file>`. Name the upload `artifact`.
2. Compare with RomM by the `contentHash` from step 3; it matches what
   RomM computes for that upload.
3. To restore, unpack the artifact yourself. `Single`: write it to the
   member's `path`. `Multi`: write each zip entry to the `path` of the
   member with that `entry`. `Folder`: unzip into the key folder's
   parent. When the emulator hasn't created a primary yet, `expected`
   gives its `path`.

This path writes whole files, so it can't merge a game's saves into a
shared memory card or a profile folder the way `restore` does. Use
`collect` and `restore` wherever they cover the system. Hash rules:
[save-units.md](../save-units.md#hash).

## Sync

`collect` gathers one game's saves into the unit that travels to RomM;
`restore` puts a unit back and reads it back, removing files where a save
folder holds a save the unit lacks. PS1 and PS2 memory cards, PCSX2 folder
cards, GameCube cards and Dolphin's GCI folder, Saturn and Sega CD backup RAM,
Dreamcast VMUs, the save folders the yuzu forks, Cemu, Vita3K and RPCS3
keep per user profile, and PSP save folders work today. `restore` raises `SigilException`
with code `SigilException.NOT_FOUND` for a unit holding none of the game's
saves, and ignores other games' saves inside a unit. The rules every system
shares are in [sync.md](../sync.md); what a unit holds, how Saturn and Sega CD
saves find their owner, and how genesis_plus_gx's region file is picked are
on each system's page under [platforms/](../platforms/README.md). For Saturn and Sega CD, build the
game with `SigilResult.persisted("saturn", "", "", 0)` or `("segacd", ...)`.

```kotlin
Sigil.collect(
    game: SigilResult, core: String, contentPath: String, saveRoot: String,
    listing: List<String>? = null,          // Root-relative paths; null lists saveRoot.
    options: Map<String, String> = emptyMap(),
    gameIds: List<String> = emptyList(),    // Every id the game's saves may carry: all discs of a set.
    state: ByteArray? = null,               // What the last call returned for this platform and emulator.
    unmanaged: Boolean = false,             // The game runs outside the caller.
    claimed: List<String> = emptyList(),    // Saturn, Sega CD, Dreamcast: names from `unowned` the user gave this game.
    companions: List<SigilCompanion> = emptyList(),   // Games whose saves this game reads, in the order they go on.
    repair: Boolean = false,                // Rebuild what SigilException.DAMAGED named, where sigil can.
    profile: String? = null,                // Layouts with profiles: the profile whose saves to take.
    fileAccess: SigilFileAccess = PosixFileAccess,   // How sigil reads, writes and removes the files.
): SigilSyncResult

Sigil.restore(unit: ByteArray, /* same inputs */, overwriteLocal: Boolean = false): SigilSyncResult

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
    state: ByteArray,           // Store it once every upload succeeded; pass it back next time.
    holding: ByteArray?,        // Saturn, Sega CD, Dreamcast: zip of the unclaimed saves on a shared volume.
                                //   Managed: keep it until they're claimed (sync.md, "Unclaimed saves").
    unowned: List<String>,      // The names of the saves in holding, escaped as SigilCardEntry.name
                                //   is. Pass them to `claimed` as they are.
    unownedChanged: Int,        // How many names at the front of unowned are new or rewritten since
                                //   the last collect.
    restoreAgain: Boolean,      // Unmanaged: the saves the last restore wrote were overwritten.
                                //   Restore again instead of uploading.
    companions: List<SigilCompanionResult>,
    profiles: List<SigilProfile>,   // Layouts with profiles: every profile the emulator lists.
    profile: String,            // The profile whose saves were taken or written; "" for none.
    alternates: List<SigilSaveAlternate>, // Listed files other option values would take, as on SigilSaveUnit.
    hardcoreMarker: Boolean,    // Restore: the unit ended in Argosy's legacy hardcore marker.
)

data class SigilProfile(
    id: String,                 // As its save folder is named.
    name: String,               // The nickname; "" when the emulator keeps none.
)
```

A companion's saves go on the game's card beside the game's own and stay
out of the game's unit; [sync.md](../sync.md#companions) has the rules.

### Refusals

`restore` writes nothing when it raises `SigilException` with one of
these codes. `collect` raises `DAMAGED`, `AMBIGUOUS` and `IO` the same
way. The exception's `problem` names the save, member or files at fault
when there is one, each line escaped as `SigilCardEntry.name` is.
[sync.md](../sync.md#refusals) has when each one happens.

| Code | Meaning |
|---|---|
| `CONFLICT` | the saves under `saveRoot` changed since the last sync; pass `overwriteLocal = true` once the user agrees |
| `UNCOLLECTED` | a shared volume holds saves no collect has passed on yet; collect for the game that ran last first |
| `NO_SPACE` | the saves don't fit; `blocksShort` says by how much |
| `REGION` | a companion's save is from another region |
| `NO_TARGET` | the unit holds a volume or member with no file to go in |
| `AMBIGUOUS` | more than one card file or profile could take the saves; `profiles` lists the profiles, so ask the user and pass the choice |
| `DAMAGED` | a file the saves are in is damaged; pass `repair = true` once the user agrees |
| `EXISTS` | Dolphin's GCI folder has no free name for a new save |
| `IO` | a file the listing holds won't open, or a member's path would leave the root |

## Memory cards

List the saves on a memory card or backup RAM volume: PS1 cards (the raw
card as `.mcr`, `.mcd` or `.srm`, DexDrive `.gme`, PSP or Vita `.vmp`),
PS2 `.ps2` file cards, GameCube raw cards, Dreamcast VMUs, and Saturn and
Sega CD backup RAM. Sync doesn't need it; it's for showing the user
what a card holds.

```kotlin
Sigil.listCard(
    path: String,               // required. The card file. Its format is detected from the content.
    fileAccess: SigilFileAccess = PosixFileAccess,  // optional. Reads the card as (folder, name).
): SigilCardListing             // Raises SigilException with UNSUPPORTED_FORMAT when the file is
                                //   not a card sigil reads.

data class SigilCardListing(
    format: Format,             // Ps1Raw, Ps1Gme, Ps1Vmp, Ps2, GamecubeRaw, DreamcastVmu, SaturnBackup,
                                //   SegacdBram.
    totalBlocks: Int,
    freeBlocks: Int,            // Blocks a new save can use.
    freeSlots: Int,             // Directory slots a new save can use.
    corruptCount: Int,          // Saves left out because their block chain is broken.
    entries: List<SigilCardEntry>,         // Live saves, in directory order.
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

## File access

Every call that touches files reads, writes, lists and removes them
through a `SigilFileAccess`. The default, `PosixFileAccess`, uses
`java.io.File`. On Android 11 and later an app can't open files under
another app's `Android/data` that way, even with storage permission, so
pass your own implementation: a DocumentsContract layer or a root shell.

```kotlin
interface SigilFileAccess {
    fun list(root: String, path: String): List<SigilFileEntry>?   // The folder's entries; null when no
                                                                  //   folder is there.
    fun read(root: String, path: String): ByteArray?              // The whole file; null when unreadable.
    fun write(root: String, path: String, data: ByteArray): Boolean   // Makes the folders above it.
    fun remove(root: String, path: String): Boolean               // A path ending in / is a folder sigil
                                                                  //   emptied first.
}

data class SigilFileEntry(name: String, isDirectory: Boolean)
```

Each call gets the save root as you passed it and a path relative to it:
`/`-separated, never starting with `/`, never holding a `..` segment, and
`""` for the root itself. `list` must name folders as well as files,
since sigil recurses into them. A call that throws, or returns `null` or
`false` for a file that's there, raises `SigilException.IO`; sigil never
takes a failed read as "no saves". `null` from `list` means the folder
doesn't exist, which is not an error.

## Helpers

```kotlin
Sigil.contentStem(contentPath: String): String        // Stem the save is named after.
Sigil.layoutSubdirs(core: String): List<String>       // Subfolders the core writes into.
Sigil.layouts(platform: String? = null): List<SigilLayout>   // Layout rows, their options and
                                                             //   region option.
Sigil.listSaveRoot(root: String, core: String,              // Below root too, where a layout with
    fileAccess: SigilFileAccess = PosixFileAccess): List<String>   //   profiles has its base.
Sigil.saveBase(core: String, path: String): Pair<String, String>   // (base, profile) for a path.
Sigil.listProfiles(core: String, saveRoot: String,            // The emulator's profiles;
    fileAccess: SigilFileAccess = PosixFileAccess): List<SigilProfile>   //   UNSUPPORTED_FORMAT without profiles.
Sigil.platformSlug(slug: String?): String             // Canonical slug, or "auto".
Sigil.loadHeaderKeyFromProdKeys(prodKeysPath: String): ByteArray   // 32 bytes.
Sigil.version(): String
```
