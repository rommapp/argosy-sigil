# GameCube

Status: synced
sigil identifies GameCube discs and collects and restores Dolphin saves, from a GCI folder or a raw memory card.

## Identification

| Slug | Platform | Inputs | `title_id` example | `usage` | Status |
|---|---|---|---|---|---|
| `gamecube` | GameCube | `.iso`, `.rvz`, `.wbfs` | `475A4C45` (hex of ASCII gameId) | file-prefix | |

`ngc` and `gc` resolve to `gamecube`. See [Identification](../identification.md) for the result fields.

| Value | Meaning | Example |
|---|---|---|
| `file-prefix` | Multiple files per game, all containing `save_id` in the basename | GameCube GCI: `<maker>-<gameId>-<name>.gci` (e.g. `01-GZLE-Animal Crossing.gci`) |

GameCube diverges further:
its artifacts are `.gci` files named `<maker>-<gameId>-<internal>.gci`,
carrying the four ASCII characters of the disc header rather than a hex
rendering of them, so `save_id` is that ASCII id
(`title_id=475A4C45`, `save_id=GZLE`).

**GameCube saves share a file prefix.** Saves are `.gci` files with the
convention `<makerCode>-<gameId>-<internalName>.gci`. Sigil emits the
ASCII gameId (`GZLE`); consumers match files whose basename contains
`-<gameId>-`, or read the same four bytes off the GCI header. argosy's
`GciSaveHandler` is a reference implementation. `title_id` is the hex
rendering of those bytes and names nothing on disk here, so a consumer
that fell back to it found no save.

**Wii and GameCube title ids are the hex of ASCII.** The disc header
carries a 4-character ASCII gameId (`RZTE`, `GZLE`). `title_id` is the
hex encoding of those bytes (`52535445`, `475A4C45`) on both consoles,
and `raw_serial` preserves the ASCII form. Where they land on disk
differs: Dolphin's Wii NAND directory is the hex, lowercased, while a
GameCube `.gci` file name carries the ASCII characters, so `save_id`
follows the platform and only Wii's tracks `title_id`. The header
starts at 0 in an `.iso` and at 0x58 in an `.rvz`,
behind the RVZ container header; a console magic backs it (Wii
`5D1C9EA3` at +0x18, GameCube `C2339F3D` at +0x1C). That magic also
names the console when nobody else does, since `.iso`, `.rvz` and
`.wbfs` carry either one and the extension cannot tell them apart. A
platform passed in wins over the magic, so a caller that has already
classified the file keeps its answer; with no platform and no magic
there is nothing left to read, and the extraction fails rather than
guessing from the name.

The [Wii page](wii.md#identification) covers how sigil finds the disc header inside a `.wbfs` file.

## Save layouts

| Layout | Files (role, option) | Shared | Source |
|---|---|---|---|
| `dolphin` | | `User/GC/{gc_region}/Card A/`, one `.gci` file per save, when Dolphin.ini's `SlotA` = `8` (default); `User/GC/MemoryCardA.{gc_region}.raw` when `SlotA` = `1`, or `.1019.raw`, `.507.raw`, `.251.raw`, `.123.raw`, `.59.raw` for smaller cards, picked by `MemoryCardSize` (`-1` for 2043 blocks, `4` to `0` for 1019 to 59). Without that option, the one card file there; with two or more there, collect and restore return `SIGIL_ERR_AMBIGUOUS` naming them. `MemoryCardSize` sizes the GCI folder too, and restore refuses a save Dolphin wouldn't load from it | dolphin-emu `Config/MainSettings.cpp` `GetGCIFolderPath`, `GetMemcardPath`; the libretro core's User folder is the save folder's `User/`; subdir `User/GC` |
| `dolphin_standalone` | | as `dolphin`, rooted at Dolphin's User folder (`GC/{gc_region}/Card A/`, `GC/MemoryCardA.{gc_region}.raw`) | subdir `GC` |

`{gc_region}` is Dolphin's GameCube region folder from the region letter
ending the game code: `E` gives `USA`, `J` and `K` give `JAP`, any other
letter `EUR`. Dolphin reads the disc's region field, which follows the
letter on retail discs.

Both rows live in [`src/save_layout.c`](../../src/save_layout.c). [Save units](../save-units.md) explains rows, templates and options.

## Sync

On
GameCube it is the game's saves as `.gci` files named as Dolphin names
them (`<maker>-<gamecode>-<file>.gci`, escaped): the one file, or a zip
of them named `<stem>.zip`. It is the same whether they came off a raw
card or a GCI folder, and restore puts them into either; F-Zero GX's save
is bound to the target card's serial on a raw card, as Dolphin binds it.
Into a GCI folder, restore refuses with `SIGIL_ERR_NO_SPACE` a save Dolphin
wouldn't load: Dolphin loads the running game's files first, then other
games' (a companion's too) in name order while each leaves a tenth of the
folder's blocks free, up to 112 saves, on a folder the size `MemoryCardSize`
sets. Files are found by a `.gci` extension in any case.

Restore needs the `remove` callback for Dolphin's GCI folder, to drop a save the unit lacks.
It refuses with `SIGIL_ERR_INVALID_ARG`, writing nothing, when it must remove a file and `remove` is NULL.

`sigil_card_list` reads a GameCube raw card as `SIGIL_CARD_FORMAT_GAMECUBE_RAW`.
Each entry's `owner_id` is the game id as disc identification reports it, such as `"47465A45"`.

GameCube-specific refusals, on top of the general ones in [Sync](../sync.md):

| Code | When | `problem` |
|---|---|---|
| `SIGIL_ERR_REGION` | a GameCube companion's save is from another Dolphin region than the game | the save |
| `SIGIL_ERR_AMBIGUOUS` | more than one file could be the emulator's card and the options don't say which: Dolphin raw cards of two sizes with no `MemoryCardSize` | the files |
| `SIGIL_ERR_EXISTS` | Dolphin's GCI folder holds other games' files under the name Dolphin gives a new save and each of its ten `0`-inserted forms, so Dolphin would write over one | the save |

## Emulator research

Scope: in-game saves only (no save states). Sources pinned to the default branch as fetched 2026-09-26. Line numbers are for that snapshot; re-pin to a commit SHA before citing in save sync. Anything without a source is marked UNVERIFIED.

Abbreviations: `DOL` = https://github.com/dolphin-emu/dolphin/blob/master, `CEMU` = https://github.com/cemu-project/Cemu/blob/main, `EDEN` = https://git.eden-emu.dev/eden-emu/eden/src/branch/master, `CIT` = https://github.com/citron-neo/emulator/blob/main, `LIBHAC` = LibHac as vendored in https://github.com/hyjinx-emu/Hyjinx/tree/master/src/Hyjinx.LibHac (the canonical LibHac host, git.ryujinx.app/ryubing/libhac, refused connections from here; the GitHub original Thealexbarney/LibHac is gone).

| Platform | Neutral per-game unit | Identity key |
| --- | --- | --- |
| GameCube | One or more `.gci` files (0x40-byte DEntry + N x 0x2000 blocks) | gamecode(4) + makercode(2) + internal filename(32) from the DEntry |

### Row 1. GameCube / Dolphin standalone (desktop), raw card mode

- Files and path: `<User>/GC/MemoryCardA.<REGION>[.<blocks>].raw` (slot B: `MemoryCardB...`). `<REGION>` is `USA`, `EUR`, `JAP` (legacy spelling is the default for raw cards), or `DEV`. `.<blocks>` is appended only for cards smaller than 2043 blocks, e.g. `.59`, `.251`. Custom path via `[Core] MemcardAPath`/`MemcardBPath` in `Dolphin.ini`; Dolphin rewrites the region token in the custom name to the running game's region.
  - Source: `DOL/Source/Core/Core/Config/MainSettings.cpp` `GetMemcardPath()` L776-791 (default template `"{}{}.{}{}.raw"`), region token parsing L805-835; `GetDirectoryForRegion()` L731-753 (Legacy -> `JAP`, Modern -> `JPN`); `DOL/Source/Core/Core/Config/MainSettings.h` L414-420 (default style `Legacy`); `DOL/Source/Core/Common/CommonPaths.h` L40-44, L141-142.
- `<User>` resolution: Windows `%APPDATA%\Dolphin Emulator\` (or `Documents\Dolphin Emulator\` for pre-PR-10708 installs, or registry `HKCU\Software\Dolphin Emulator\UserConfigPath`, or `<exe>\User\` with `portable.txt`); macOS `~/Library/Application Support/Dolphin/`; Linux `~/.dolphin-emu/` if it exists and not flatpak, else `$XDG_DATA_HOME/dolphin-emu/` (default `~/.local/share/dolphin-emu/`); `$DOLPHIN_EMU_USERPATH` overrides on macOS/Linux. Source: `DOL/Source/Core/UICommon/UICommon.cpp` L317-470; `CommonPaths.h` L15-21 (`NORMAL_USER_DIR`).
- Format: whole 16 MiB (2043-block, 0x80 Mbit) GameCube card image: 5 system blocks (header, 2x directory, 2x BAT) then 0x2000-byte data blocks. Up to 127 files (`DIRLEN = 0x7F`). Source: `DOL/Source/Core/Core/HW/GCMemcard/GCMemcard.h` L97-118; DEntry layout L161-221.
- Scope: SHARED container. All games of that region share one image per slot. Selected by `[Core] SlotA = 1` (`EXIDeviceType::MemoryCard`); default is folder mode (`SlotA = 8`, `MemoryCardFolder`). Source: `MainSettings.cpp` L133-136; enum values `DOL/Source/Core/Core/HW/EXI/EXI_Device.h` L25-45 (MemoryCard=1, MemoryCardFolder=8, None=0xFF).
- Extraction/injection: feasible, not byte-lossless on import.
  - Export (`GCMemcard::ExportFile`, `GCMemcard.cpp` L786-800) copies the DEntry verbatim plus the file's blocks, i.e. an exact `.gci`.
  - Import (`GCMemcard::ImportFile`, `GCMemcard.cpp` L713-785) rejects a file with the same identity (`TITLEPRESENT`), assigns a new `m_first_block`, increments `m_copy_counter`, bumps the directory update counter, and for F-Zero GX and PSO rewrites card-serial-bound bytes in the payload (`FZEROGX_MakeSaveGameValid` L1066-1090, `PSO_MakeSaveGameValid` L1121-1150). The payload is otherwise unchanged.
  - Replacing a save means delete-then-import. A sync client must parse the directory/BAT itself (or shell out to Dolphin's `GCMemcard` code) to do this in place; there is no Dolphin CLI for it (UNVERIFIED that no CLI exists; DolphinTool has no memcard verb in the tree fetched).
  - Tools: Dolphin Memory Card Manager (DolphinQt, uses `GCMemcard` import/export, writes `.gci`/`.gcs`/`.sav`); GCMM on real hardware (backs up `.gci`, restores `.gci/.gcs/.sav`, raw `.raw/.gcp/.mci`; https://github.com/suloku/gcmm README).
- Neutral per-game form: `.gci`. Identity is `gamecode + makercode + filename` (`GCMemcardUtils.cpp` `HasSameIdentity` ~L25-58). One game can own several files (e.g. a system file plus data files), so a "game's save" is the set of DEntries whose 4-byte gamecode matches the disc's game ID.
- Profile/user path components: none. Client must resolve `<User>`, slot, region token, and block-count suffix.

### Row 2. GameCube / Dolphin standalone, GCI folder mode (default)

- Files and path: `<User>/GC/<REGION>/Card A/*.gci` (slot B: `Card B`). Default region dir uses `GetDirectoryForRegion` with the default Legacy style, so Japanese is `JAP`; a custom `[Core] GCIFolderAPath` gets the Modern style, so `<custom>/JPN`. Also honoured: `GCIFolderAPathOverride` (per-game/session). Source: `MainSettings.cpp` `GetGCIFolderPath()` L848-902, config keys L104-119.
- Filenames Dolphin writes: `<makercode>-<gamecode>-<internal filename>.gci`, escaped (`GCMemcardDirectory.cpp` `GenerateDefaultGCIFilename` L42-62; `GCMemcardUtils.cpp` `GenerateFilename` ~L318-325). Dolphin loads any `*.gci` regardless of name (`GCMemcardDirectory.cpp` L192 `DoFileSearch(..., ".gci")`), so the header, not the filename, is the identity. Deleted saves are renamed to `<name>.gci.deleted` (L691-699). An optional `MC_SYSTEM_AREA` file holds the virtual card header (L40, L186-188).
- Format: each `.gci` = 0x40-byte DEntry header (big-endian) + `m_block_count` x 0x2000 blocks. File size mod 0x2000 == 0x40. Source: `GCMemcardUtils.cpp` L19 `GCI_HEADER_SIZE = DENTRY_SIZE`, `ReadSavefile` L206-227; DEntry field table in `GCMemcard.h` L161-221 (gamecode 0x00/4, makercode 0x04/2, banner flags 0x07, filename 0x08/0x20, mtime 0x28, image offset 0x2C, icon fmt 0x30, anim speed 0x32, permissions 0x34, copy counter 0x35, first block 0x36, block count 0x38, comments addr 0x3C).
- Scope: PER-GAME files in a SHARED folder. On boot Dolphin builds a virtual card: it loads every `.gci` whose gamecode equals the running game's first 4 ID chars, then fills with other games' files while keeping ~10% blocks free and at most 112 entries (`GCMemcardDirectory.cpp` L190-251). `SESSION_GCI_FOLDER_CURRENT_GAME_ONLY` (set for NetPlay) loads only the current game's files (L190, L209-212). Two `.gci` files with the same internal identity: the second is refused (L64-75).
- Extraction/injection: trivially per-game and lossless. Copy the `.gci` files in and out while Dolphin is not running that game. Dolphin flushes other-game data to disk and drops it from memory (L703-715). On load it applies the F-Zero GX/PSO serial fix to the in-memory copy (L112-113), which is written back if the game saves.
- Other import formats Dolphin accepts (Memory Card Manager, `ReadSavefile` detects by `filesize % 0x2000`): `.gcs` (header 0x150, magic `GCSAVE`, DEntry at 0x110) and `.sav` (header 0xC0, magic `DATELGC_SAVE`, DEntry at 0x80 with byte-swapped 16-bit fields per `ByteswapDEntrySavHeader`). Source: `GCMemcardUtils.cpp` L19-26, L91-114, L149-204. Both reduce losslessly to `.gci` (same DEntry + blocks).
- Neutral per-game form: `.gci` (same as row 1).
- Profile/user path components: none beyond `<User>`, region dir, slot letter.

### Row 3. GameCube / Dolphin Android

- Same code and defaults as desktop (shared `MainSettings.cpp`; SlotA default `MemoryCardFolder`).
- `<User>` = `context.getExternalFilesDir(null)`, i.e. `/storage/emulated/0/Android/data/org.dolphinemu.dolphinemu/files/`, or legacy `/sdcard/dolphin-emu/` when legacy storage is in use and that folder already exists. Source: `DOL/Source/Android/app/src/main/java/org/dolphinemu/dolphinemu/utils/DirectoryInitialization.kt` L88-119, L320-323.
- Scoped storage: another app (a sync client) cannot read `Android/data/<pkg>` directly on Android 11+ without SAF/root/Shizuku. UNVERIFIED whether Dolphin exposes it through its DocumentsProvider (it ships one for the user dir, UNVERIFIED).
- Package ID for other flavours (e.g. MMJR forks) UNVERIFIED.

### Row 4. GameCube / Dolphin libretro core

- `<User>` = `<RetroArch save dir>/User` when the frontend provides a save directory, else `<system dir>/dolphin-emu/User`. Source: https://github.com/libretro/dolphin/blob/master/Source/Core/DolphinLibretro/Boot.cpp L241-251 (`user_dir = save_dir + "/User"`). With RetroArch "Sort saves into folders by core name" enabled, the save dir it passes is `saves/dolphin-emu/` (RetroArch behaviour, UNVERIFIED in source; libretro docs say `saves/dolphin-emu/User`, https://docs.libretro.com/library/dolphin/).
- The core does not override `SlotA`/memcard settings (only SP1 and GBA paths, `Boot.cpp` L283-445), and the fork's `MainSettings.cpp` L133-134 keeps `MemoryCardFolder` as default. So default layout is `<User>/GC/<REGION>/Card A/*.gci`, raw card if a `Dolphin.ini` under `<User>/Config` says `SlotA = 1`.
- RetroArch's own `.srm` is not used for GC/Wii saves (UNVERIFIED; the core's `retro_get_memory_data` for SAVE_RAM was not checked).
- Everything else as rows 1-2.

### Notes for save sync

- Every format here is lossless at the per-game level when the emulator is not running the game. The two lossy spots are Cemu's built-in export (drops `common/`) and the GC raw-card import path (rewrites DEntry block pointers and copy counter, re-signs F-Zero GX/PSO).
- The only shared containers are the Dolphin raw card (`.raw`) and, logically, the Ryujinx indexer. A client that supports raw cards needs a GC memcard directory/BAT writer, or should steer users to GCI folder mode (Dolphin's default).
- Identity fields a client must compute locally: GC 4-char gamecode (from disc ID) plus the per-file DEntry identity; Wii 64-bit title ID; Wii U title ID + Cemu persistent ID; Switch application ID + profile UUID (byte-reversed for yuzu legacy layout, raw for "future" layout), and for Ryujinx the save data ID from `imkvdb.arc`.
- Region matters for GC paths (`USA`/`EUR`/`JAP` vs `JPN` depending on default vs custom path). A PAL and NTSC copy of one game use different folders and usually different gamecodes.

## Open items

- Dolphin's Android package for other flavours, and whether its DocumentsProvider exposes the User folder to a sync client, are UNVERIFIED (row 3).
- sigil reads only slot A. Slot B (`MemoryCardB...`, `Card B`) has no layout row.
