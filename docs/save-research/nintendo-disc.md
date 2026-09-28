# Save data layout: GameCube, Wii, Wii U, Switch

Scope: in-game saves only (no save states). Sources pinned to the default branch as fetched 2026-09-26. Line numbers are for that snapshot; re-pin to a commit SHA before citing in save sync. Anything without a source is marked UNVERIFIED.

Abbreviations: `DOL` = https://github.com/dolphin-emu/dolphin/blob/master, `CEMU` = https://github.com/cemu-project/Cemu/blob/main, `EDEN` = https://git.eden-emu.dev/eden-emu/eden/src/branch/master, `CIT` = https://github.com/citron-neo/emulator/blob/main, `LIBHAC` = LibHac as vendored in https://github.com/hyjinx-emu/Hyjinx/tree/master/src/Hyjinx.LibHac (the canonical LibHac host, git.ryujinx.app/ryubing/libhac, refused connections from here; the GitHub original Thealexbarney/LibHac is gone).

---

## Summary of neutral per-game forms

| Platform | Neutral per-game unit | Identity key |
| --- | --- | --- |
| GameCube | One or more `.gci` files (0x40-byte DEntry + N x 0x2000 blocks) | gamecode(4) + makercode(2) + internal filename(32) from the DEntry |
| Wii | Directory tree of `title/<tid-hi>/<tid-lo>/data/` (banner.bin + game files) | 64-bit title ID |
| Wii U | Directory trees `user/<persistentId>/` and `user/common/` (+ optional `meta/`) | 64-bit title ID (+ persistent ID for the user part) |
| Switch | Directory tree = root of the save filesystem (what JKSV zips / what sits in yuzu's title dir / Ryujinx `.../0/`) | application ID + account UID (+ save type) |

---

## Row 1. GameCube / Dolphin standalone (desktop), raw card mode

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

## Row 2. GameCube / Dolphin standalone, GCI folder mode (default)

- Files and path: `<User>/GC/<REGION>/Card A/*.gci` (slot B: `Card B`). Default region dir uses `GetDirectoryForRegion` with the default Legacy style, so Japanese is `JAP`; a custom `[Core] GCIFolderAPath` gets the Modern style, so `<custom>/JPN`. Also honoured: `GCIFolderAPathOverride` (per-game/session). Source: `MainSettings.cpp` `GetGCIFolderPath()` L848-902, config keys L104-119.
- Filenames Dolphin writes: `<makercode>-<gamecode>-<internal filename>.gci`, escaped (`GCMemcardDirectory.cpp` `GenerateDefaultGCIFilename` L42-62; `GCMemcardUtils.cpp` `GenerateFilename` ~L318-325). Dolphin loads any `*.gci` regardless of name (`GCMemcardDirectory.cpp` L192 `DoFileSearch(..., ".gci")`), so the header, not the filename, is the identity. Deleted saves are renamed to `<name>.gci.deleted` (L691-699). An optional `MC_SYSTEM_AREA` file holds the virtual card header (L40, L186-188).
- Format: each `.gci` = 0x40-byte DEntry header (big-endian) + `m_block_count` x 0x2000 blocks. File size mod 0x2000 == 0x40. Source: `GCMemcardUtils.cpp` L19 `GCI_HEADER_SIZE = DENTRY_SIZE`, `ReadSavefile` L206-227; DEntry field table in `GCMemcard.h` L161-221 (gamecode 0x00/4, makercode 0x04/2, banner flags 0x07, filename 0x08/0x20, mtime 0x28, image offset 0x2C, icon fmt 0x30, anim speed 0x32, permissions 0x34, copy counter 0x35, first block 0x36, block count 0x38, comments addr 0x3C).
- Scope: PER-GAME files in a SHARED folder. On boot Dolphin builds a virtual card: it loads every `.gci` whose gamecode equals the running game's first 4 ID chars, then fills with other games' files while keeping ~10% blocks free and at most 112 entries (`GCMemcardDirectory.cpp` L190-251). `SESSION_GCI_FOLDER_CURRENT_GAME_ONLY` (set for NetPlay) loads only the current game's files (L190, L209-212). Two `.gci` files with the same internal identity: the second is refused (L64-75).
- Extraction/injection: trivially per-game and lossless. Copy the `.gci` files in and out while Dolphin is not running that game. Dolphin flushes other-game data to disk and drops it from memory (L703-715). On load it applies the F-Zero GX/PSO serial fix to the in-memory copy (L112-113), which is written back if the game saves.
- Other import formats Dolphin accepts (Memory Card Manager, `ReadSavefile` detects by `filesize % 0x2000`): `.gcs` (header 0x150, magic `GCSAVE`, DEntry at 0x110) and `.sav` (header 0xC0, magic `DATELGC_SAVE`, DEntry at 0x80 with byte-swapped 16-bit fields per `ByteswapDEntrySavHeader`). Source: `GCMemcardUtils.cpp` L19-26, L91-114, L149-204. Both reduce losslessly to `.gci` (same DEntry + blocks).
- Neutral per-game form: `.gci` (same as row 1).
- Profile/user path components: none beyond `<User>`, region dir, slot letter.

## Row 3. GameCube / Dolphin Android

- Same code and defaults as desktop (shared `MainSettings.cpp`; SlotA default `MemoryCardFolder`).
- `<User>` = `context.getExternalFilesDir(null)`, i.e. `/storage/emulated/0/Android/data/org.dolphinemu.dolphinemu/files/`, or legacy `/sdcard/dolphin-emu/` when legacy storage is in use and that folder already exists. Source: `DOL/Source/Android/app/src/main/java/org/dolphinemu/dolphinemu/utils/DirectoryInitialization.kt` L88-119, L320-323.
- Scoped storage: another app (a sync client) cannot read `Android/data/<pkg>` directly on Android 11+ without SAF/root/Shizuku. UNVERIFIED whether Dolphin exposes it through its DocumentsProvider (it ships one for the user dir, UNVERIFIED).
- Package ID for other flavours (e.g. MMJR forks) UNVERIFIED.

## Row 4. GameCube / Dolphin libretro core

- `<User>` = `<RetroArch save dir>/User` when the frontend provides a save directory, else `<system dir>/dolphin-emu/User`. Source: https://github.com/libretro/dolphin/blob/master/Source/Core/DolphinLibretro/Boot.cpp L241-251 (`user_dir = save_dir + "/User"`). With RetroArch "Sort saves into folders by core name" enabled, the save dir it passes is `saves/dolphin-emu/` (RetroArch behaviour, UNVERIFIED in source; libretro docs say `saves/dolphin-emu/User`, https://docs.libretro.com/library/dolphin/).
- The core does not override `SlotA`/memcard settings (only SP1 and GBA paths, `Boot.cpp` L283-445), and the fork's `MainSettings.cpp` L133-134 keeps `MemoryCardFolder` as default. So default layout is `<User>/GC/<REGION>/Card A/*.gci`, raw card if a `Dolphin.ini` under `<User>/Config` says `SlotA = 1`.
- RetroArch's own `.srm` is not used for GC/Wii saves (UNVERIFIED; the core's `retro_get_memory_data` for SAVE_RAM was not checked).
- Everything else as rows 1-2.

## Row 5. Wii / Dolphin (standalone, Android, libretro)

- Files and path: `<WiiRoot>/title/<tid-hi 8 hex>/<tid-lo 8 hex>/data/` where hex is lowercase, e.g. `title/00010000/524d4745/data/` for RMGE. `tid-hi` is `00010000` for disc games, `00010001` for channels/WiiWare/VC, `00010004` for disc-based game channels (hi values are standard Wii title types, UNVERIFIED in Dolphin source beyond the format string). Source: `DOL/Source/Core/Common/NandPaths.cpp` `GetTitlePath` L63-66, `GetTitleDataPath` L69-72.
- `<WiiRoot>` = `<User>/Wii/` (`FileUtil.cpp` L840 `D_WIIROOT_IDX = D_USER_IDX + "Wii/"`), overridable by `[General] NANDRootPath` (`MainSettings.cpp` L335). Same `<User>` resolution as GC rows; libretro core uses its own `<User>`.
- NAND is a HOST FOLDER, not an image. Dolphin's `HostFileSystem` maps each NAND file to a host file. Per-file ownership/permissions (uid, gid, modes, attribute) live in `<WiiRoot>/fst.bin` (0x20-byte `SerializedFstEntry` per node). Files that exist on disk without an `fst.bin` entry get a default RW/RW/RW entry, so injecting plain files works. Source: `DOL/Source/Core/Core/IOS/FS/HostBackend/FS.cpp` L56-82, L157, L234-270.
- NAND filenames containing `" * / : < > ? \ |`, 0x7F, control chars, or `__` are escaped on the host as `__xx__`. A client copying the tree must copy host names verbatim (do not unescape). Source: `NandPaths.cpp` `EscapeFileName` L119-145.
- Format: arbitrary game files plus `banner.bin` (the save banner/icon the System Menu shows). Some games put files in `data/nocopy/` (not copyable on real hardware; Dolphin's `NandStorage` handles the nocopy flag, `WiiSave.cpp` L124). Games that also write to SD (`WiiSD.raw` / SD sync folder) or `shared2/` are out of this model (UNVERIFIED list of such games).
- Scope: PER-GAME directory inside a shared NAND tree. No option switches it.
- Extraction/injection: lossless per game by copying the `data/` tree while the game is not running. Dolphin's own export (`Tools > Export Wii Save` / `ExportAll`) writes the console-format `private/wii/title/<ID4>/data.bin`: AES-128-CBC with the Wii SD key, a BK header with Dolphin's fixed NG ID `0x0403AC68`, per-file headers carrying permissions/attributes, and an ECDSA-signed cert chain from Dolphin's emulated console. Import reads the same format into the NAND. Source: `DOL/Source/Core/Core/HW/WiiSave.cpp` L47-51, `DataBinStorage` L270-475, `Import` L544-565, `Export` L568-575, `ExportAll` L583-593.
  - `data.bin` from a real Wii or another Dolphin imports fine (signatures are not verified on import, UNVERIFIED; the code path decrypts and copies).
- Neutral per-game form: the raw `data/` directory tree (host-escaped names) is simplest and lossless for Dolphin-to-Dolphin. `data.bin` is the interchange form for real hardware (SaveGame Manager GX, Wii System Menu) and is itself lossless apart from nocopy files.
- Profile/user path components: none. Client needs `<WiiRoot>` and the 64-bit title ID (from the disc header/ID4 -> `00010000` + ASCII hex of the 4-char ID).

## Row 6. Wii U / Cemu

- Files and path: `<mlc01>/usr/save/<tid-hi 8 hex>/<tid-lo 8 hex>/`, lowercase hex, `tid-hi` = `00050000` for games. Inside:
  - `user/<persistentId 8 hex>/` per-account save (e.g. `user/80000001/`)
  - `user/common/` save shared by all accounts on the console
  - `meta/meta.xml`, `meta/iconTex.tga` (copied from the title's meta on save-dir creation), `meta/saveinfo.xml` (per-account `<account persistentId="...">` with a hex `<timestamp>`)
  - Source: `CEMU/src/Cafe/IOSU/legacy/iosu_acp.cpp` `_ACPCreateSaveDir` L720-756 (creates `meta/`, `user/`, `user/common`, `user/%08x`, persistentId 0 mapped to `0x80000001` at L751), `CreateSaveMetaFiles` L679-715, `ACPUpdateSaveTimeStamp` L590-640; `CEMU/src/Cafe/OS/libs/nn_save/nn_save.cpp` `GetAbsoluteFullPath` L97-115 (`/vol/save/<pid>/` and `/vol/save/common/`), `GetAbsoluteFullPathOtherApplication` L120-146; `/vol/save/` is mounted at `<mlc01>/usr/save/<hi>/<lo>/user/` in `CEMU/src/Cafe/OS/libs/nn_acp/nn_acp.cpp` L65-67.
- `<mlc01>`: `mlc_path` from settings.xml, or `--mlc` launch arg, else `GetUserDataPath("mlc01")`. Source: `CEMU/src/config/ActiveSettings.cpp` L276-301. Default user data dir per OS: Windows `%APPDATA%\Cemu\` or the exe dir in portable mode, Linux `~/.local/share/Cemu/`, macOS `~/Library/Application Support/Cemu/` (UNVERIFIED: `GetUserDataPath` implementation not read).
- Format: plain host files, whatever the game writes. No container.
- Scope: PER-GAME directory; within it the save is split per account plus `common`. No option switches it.
- Account / persistent ID: accounts live in `<mlc01>/usr/save/system/act/<pid 8 hex>/account.dat` (text, `PersistentId=<hex>` line); valid IDs start at `0x80000001` (`kMinPersistendId`), next ID tracked in `usr/save/system/act/persisid.dat`. Active account = `--account` launch arg or `account.m_persistent_id` in settings.xml. Source: `CEMU/src/Cafe/Account/Account.cpp` L136-137, L177-195, L304-346, L383-410; `Account.h` L45; `ActiveSettings.cpp` L157-160.
- Extraction/injection: lossless per game by copying directories while the game is closed. Cemu's built-in Title Manager export zips ONLY `user/<pid>/` plus a `cemu_meta` file (`titleId = 0x...`); it omits `user/common/` and `meta/`. Source: `CEMU/src/gui/wxgui/TitleManager.cpp` `OnSaveExport` L620-700. Import unzips into `user/<target pid>/` (`CEMU/src/gui/wxgui/dialogs/SaveImport/SaveImportWindow.cpp` L90-140, L168, L205-260). Cemu's "transfer" renames `user/<old>` to `user/<new>` and rewrites the `persistentId` attribute in `saveinfo.xml` (`SaveTransfer.cpp` L102-187). So a sync client must carry `common/` itself; the built-in export is lossy for games that use it.
- Neutral per-game form: `user/common/` tree + `user/<pid>/` tree, with the pid component relabelled to the destination account on injection. `meta/` is regenerable (Cemu recreates `meta.xml`/`iconTex.tga` from the title on save-dir creation, and `saveinfo.xml` on the next save). SaveMii (Wii U homebrew) uses the same `<pid>/` + `common/` split (UNVERIFIED layout details).
- Profile/user path components: `<persistentId>` (8 lowercase hex, typically `80000001`). The client must map "RomM user" to a local persistent ID.

## Row 7. Switch / yuzu forks: Eden, Citron, Sudachi, Suyu

- Files and path (legacy layout, what all four create by default): `<nand>/user/save/0000000000000000/<USER>/<TITLEID 16 hex upper>/` for account saves. Device saves use `<USER>` = 32 zeros. Source: `EDEN/src/core/file_sys/savedata_factory.cpp` `GetFullPath` L104-144, format at L133-134: `"{}save/{:016X}/{:016X}{:016X}/{:016X}", out, 0, user_id[1], user_id[0], title_id`; same string in `CIT/src/core/file_sys/savedata_factory.cpp` L184; Sudachi and Suyu mirrors identical (https://github.com/Synoptikon/Sudachi/blob/master/src/core/file_sys/savedata_factory.cpp L134, https://github.com/bolucat/Archive/blob/master/suyu/src/core/file_sys/savedata_factory.cpp L134; these are mirrors, not the projects' own hosts, which were unreachable).
- `<USER>` derivation: the profile UUID's 16 bytes are memcpy'd into a `u128` (two little-endian u64) and printed high half then low half, each `{:016X}`. Net effect: `<USER>` = the profile's 16 UUID bytes in REVERSE order, uppercase hex. It is not the UUID string shown in the UI (`RawString()` is bytes in order, lowercase). Source: `EDEN/src/common/uuid.cpp` `RawString` L148-154, `AsU128` L175-179.
- Alternate "future" layout, preferred when the directory already exists: `<nand>/user/save/account/<uuid RawString lowercase>/<TITLEID with low byte masked to 00>/0/` and `<nand>/user/save/device/<TITLEID>/0/`. Source: `EDEN/.../savedata_factory.cpp` `GetFutureSaveDataPath` L26-53, used at L115-124; Citron L62, L198. Clients should check this path first, then the legacy path.
- Size sidecar: each save dir holds a 0x10-byte size file that records the save's data/journal size. Name differs per fork: `.yuzu_save_size` (Eden, `EDEN/src/core/file_sys/savedata_factory.h` L23-24), `.citron_save_size` (Citron, `CIT/src/core/file_sys/savedata_factory.h` L22), `.sudachi_save_size`, `.suyu_save_size` (mirrors above, `savedata_factory.h` L21). A cross-fork sync should strip it on extract and let the target create its own (or rename it).
- `<nand>` root: Eden `%APPDATA%\eden\nand` (Windows) / `$XDG_DATA_HOME/eden/nand` (Linux), `<exe>/user/nand` in portable mode; Eden also has a configurable save dir (`EdenPath::SaveDir`, defaults to the NAND dir). Other forks swap the dir name (`citron`, `sudachi`, `suyu`, legacy `yuzu`). Source: `EDEN/src/common/fs/fs_paths.h` L11-39, `EDEN/src/common/fs/path_util.cpp` L116-181. Android package paths UNVERIFIED.
- Profiles: `<nand>/system/save/8000000000000010/su/avators/profiles.dat` (0x650-byte `ProfileDataRaw`, 8 x 0xC8 `UserRaw` each starting with the UUID). Source: `EDEN/src/core/hle/service/acc/profile_manager.cpp` L30-51.
- Format: plain host directory = the save filesystem root. No container, no journal copy.
- Scope: PER-GAME directory per user. No option switches it.
- Extraction/injection: lossless per game by copying the directory (minus the size sidecar) while the game is closed. Eden additionally ships a "link to Ryujinx save" feature that reads Ryujinx's index (row 8) and symlinks/copies between the two (https://git.eden-emu.dev/eden-emu/eden/pulls/2815).
- Neutral per-game form: the directory tree (same content as a JKSV backup, row 9).
- Profile/user path components: `<USER>` (32 hex, byte-reversed UUID, uppercase) in the legacy layout, or `<uuid RawString>` (32 lowercase hex, byte order) in the future layout.

## Row 8. Switch / Ryujinx (Ryubing)

- Files and path: `<Ryujinx>/bis/user/save/<saveDataId 16 hex lowercase>/`, containing:
  - `0/` committed save filesystem root (authoritative when the game is closed)
  - `1/` working copy; `_/` transient during commit; `.lock`
  - `ExtraData0`, `ExtraData1` (and `ExtraData_` during commit): the save's extra data (attribute, owner, sizes, timestamps)
  - Source: `LIBHAC/FsSystem/DirectorySaveDataFileSystem.cs` L33-36, L806-810; on open, if `0/` exists it is copied over `1/`, and if `0/` is missing but `_/`/`1/` exist the commit is completed (Initialize, ~L240-290). For non-journaling saves only `1/` is used (same function, "Only the working directory is needed for non-journaling savedata"). Eden's reader hardcodes `.../<id>/0` (`EDEN/src/common/fs/ryujinx_compat.cpp` `GetRyuSavePath` L37-42).
- The folder name is NOT the title ID. It is an allocated save data ID (user saves counting up from `0000000000000001`, per the Eden PR description, https://git.eden-emu.dev/eden-emu/eden/pulls/2815). The mapping lives in the save data indexer: `<Ryujinx>/bis/system/save/8000000000000000/0/imkvdb.arc` (`ryujinx_compat.cpp` `GetKvdbPath` L20-24).
- `imkvdb.arc` format: 12-byte header `IMKV` + u32 reserved + u32 entry count, then entries of 0x8C bytes: `IMEN` magic, u32 key size (0x40), u32 value size (0x40), key = `SaveDataAttribute`, value = `SaveDataIndexerValue`. Source: `ryujinx_compat.h` L12-14, `ReadKvdb` L44-104 (reads title ID at key+0x00 and save ID at value+0x00).
  - `SaveDataAttribute` (0x40): ProgramId u64 @0x00, UserId 16 bytes @0x08, StaticSaveDataId u64 @0x18, Type u8 @0x20 (Account=1, Device=3 per HOS; UNVERIFIED enum values not read), Rank u8 @0x21, Index u16 @0x22, reserved to 0x40. Source: `LIBHAC/Fs/Common/SaveDataTypes.cs` L130-138.
  - `SaveDataIndexerValue` (0x40): SaveDataId u64 @0x00, Size i64 @0x08, Field10 u64 @0x10, SpaceId u8 @0x18, State u8 @0x19, reserved. Source: `LIBHAC/FsSrv/SaveDataIndexerValue.cs`.
  - Byte order of UserId inside the key relative to the Profiles.json string: UNVERIFIED.
- `<Ryujinx>` root: Windows `%APPDATA%\Ryujinx`, Linux `$XDG_CONFIG_HOME/Ryujinx` (`~/.config/Ryujinx`) per Eden's compat code (`EDEN/src/common/fs/path_util.cpp` L177, L181); macOS `~/Library/Application Support/Ryujinx` and portable `<exe>/portable/` UNVERIFIED (Ryubing source host unreachable).
- Profiles: `<Ryujinx>/system/Profiles.json`; default profile UserId `00000000000000010000000000000000` (UNVERIFIED, not read from source).
- Format: plain host directory tree under `0/`.
- Scope: PER-GAME directory per user, but keyed by an opaque ID; lookup requires parsing `imkvdb.arc`.
- Extraction/injection: extraction is lossless (copy `<id>/0/`). Injection into an existing save is lossless (replace `<id>/0/` contents, delete `1/` or let Ryujinx overwrite it from `0/` on next mount). Injecting a save for a title that has never been booted requires creating an indexer entry plus `ExtraData0/1`; the practical route is "boot the game once so Ryujinx creates the save, then replace `0/`". Ryujinx's GUI "Open User Save Directory" resolves the same path (UNVERIFIED function name).
- Neutral per-game form: contents of `<id>/0/` (same as JKSV/yuzu forks).
- Profile/user path components: none in the path. Client must resolve `(applicationId, userId, type)` -> `saveDataId` via `imkvdb.arc`.

## Row 9. Switch / JKSV (real hardware backup format)

- Files and path: `sdmc:/JKSV/<path-safe title>/<path-safe nickname> - <date>/` (folder) or `.../<name>.zip`. Working directory is configurable. Source: https://github.com/J-D-K/JKSV/blob/rewrite/source/appstates/BackupMenuState.cpp L31 (`get_working_directory() / get_path_safe_title()`), L288-292 (default name `"%s - %s"`, nickname and date); `source/tasks/backup.cpp` L58-85 (zip vs folder).
- Format: the save filesystem root copied as-is (`copy_directory(DEFAULT_SAVE_ROOT, path)` or `copy_directory_to_zip`), plus `.nx_save_meta.bin` at the root of the folder/zip. `backup.cpp` L66-85.
- `.nx_save_meta.bin` (packed, 86 bytes, little-endian): magic u32 `0x56534B4A` ("JKSV") @0x00, revision u8 (=1) @0x04, applicationID u64 @0x05, accountID 16 bytes @0x0D, systemSaveID u64 @0x1D, saveDataType u8 @0x25, saveDataRank u8 @0x26, saveDataIndex u16 @0x27, ownerID u64 @0x29, timestamp u64 @0x31, flags u32 @0x39, saveDataSize i64 @0x3D, journalSize i64 @0x45, commitID u64 @0x4D, saveDataSpaceID u8 @0x55. Filled from `FsSaveDataExtraData`. Source: `include/fs/SaveMetaData.hpp` (struct, magic, filename), `source/fs/SaveMetaData.cpp` `fill_save_meta_data` L15-37. Offsets computed from the packed struct.
- On restore JKSV only uses the meta to extend the target save if `saveDataSize` is larger (`process_save_meta_data` L39-50); it does not rebind account or title. Older JKSV releases (pre-rewrite `master`) write no meta file (UNVERIFIED which release introduced it).
- Scope: PER-GAME, per user.
- Extraction/injection: lossless for file content. Mapping a JKSV backup onto an emulator = copy the tree minus `.nx_save_meta.bin` into the yuzu-fork title dir or Ryujinx `<id>/0/`. The meta file is the only portable carrier of the save's declared size, which matters when the target save must be created or extended.
- Neutral per-game form: this is the de facto neutral form (tree + optional `.nx_save_meta.bin`).
- Profile/user path components: nickname in the backup folder name (display only); the UID is in the meta file.

---

## Notes for save sync

- Every format here is lossless at the per-game level when the emulator is not running the game. The two lossy spots are Cemu's built-in export (drops `common/`) and the GC raw-card import path (rewrites DEntry block pointers and copy counter, re-signs F-Zero GX/PSO).
- The only shared containers are the Dolphin raw card (`.raw`) and, logically, the Ryujinx indexer. A client that supports raw cards needs a GC memcard directory/BAT writer, or should steer users to GCI folder mode (Dolphin's default).
- Identity fields a client must compute locally: GC 4-char gamecode (from disc ID) plus the per-file DEntry identity; Wii 64-bit title ID; Wii U title ID + Cemu persistent ID; Switch application ID + profile UUID (byte-reversed for yuzu legacy layout, raw for "future" layout), and for Ryujinx the save data ID from `imkvdb.arc`.
- Region matters for GC paths (`USA`/`EUR`/`JAP` vs `JPN` depending on default vs custom path). A PAL and NTSC copy of one game use different folders and usually different gamecodes.
