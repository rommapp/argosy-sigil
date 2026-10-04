# Nintendo Switch

Status: synced
sigil identifies Switch XCI and NSP dumps with the user's keys, and collects and restores the save folders the yuzu forks (Eden, Citron, Sudachi, yuzu) keep per profile and per device.

## Identification

| Slug | Platform | Inputs | `title_id` example | `usage` | Status |
|---|---|---|---|---|---|
| `switch` | Nintendo Switch | `.nsp`, `.xci` | `0100ABCD12345000` | folder-exact | |

| Value | Meaning | Example |
|---|---|---|
| `folder-exact` | One folder per game named exactly `save_id` | `Switch/saves/0100ABCD12345000/` |

See [Identification](../identification.md) for the result fields. The result also carries two Switch-only fields: `switch_content_type` (`SIGIL_SWITCH_CONTENT_UNKNOWN`, `_APPLICATION`, `_PATCH`, `_ADDON`) and `title_version`.

An update or a DLC reads as the game it belongs to. Its saves live in the
game's folder, so `title_id` and `save_id` are the game's id, the one the
CNMT's extended header names (`0100152000022000` for a Mario Kart 8 Deluxe
DLC). `raw_serial` keeps the content's own id (`0100152000023001`), and
`switch_content_type` and `title_version` describe the content itself. A
dump that holds the game beside its update or DLC reads as the game, its
version included. Read from a file name, an id ending in `800` is an update
of the game ending in `000`, and any other id not ending in `000` is a DLC of
the game `0x1000` below its block.

### Switch keys

Switch NCAs are encrypted; extracting the title ID from a retail XCI
or NSP requires the `header_key` from a `prod.keys` file. Pass it via
the support struct in any of three forms. Sigil resolves them in
this priority order:

```c
sigil_support sup = {
    .struct_version = SIGIL_SUPPORT_V1,

    /* (1) Highest priority: a raw 32-byte key, for callers that
     *     already loaded prod.keys themselves. */
    .switch_header_key = my_header_key_bytes,

    /* (2) An in-memory text blob, for sandboxed environments like
     *     Android SAF where direct file I/O is mediated. */
    .switch_prod_keys_text = prod_keys_blob,
    .switch_prod_keys_text_len = prod_keys_blob_len,

    /* (3) A path on disk; sigil opens and parses the file. */
    .switch_prod_keys_path = "/path/to/prod.keys",
};
sigil_options opts = { .struct_version = SIGIL_OPTIONS_V1, .support = &sup };
sigil_extract_from_path("game.xci", SIGIL_PLATFORM_SWITCH, &opts, &r);
```

Without a key, sigil reads nothing from an XCI or NSP and returns
`SIGIL_ERR_NEEDS_KEY`. It never guesses a title id from NCA file names,
which in a retail dump are content ids. When keys are given but don't
open the content, because the key file lacks the key for the dump's key
generation or its header key is wrong, it returns
`SIGIL_ERR_KEYS_INCOMPATIBLE`; a newer `prod.keys` fixes that. Either
error falls through to the file-name source when the caller sets
`SIGIL_FLAG_FILENAME_FALLBACK`, and the result then says
`source = filename`.

A Switch XCI or NSP needs keys: without them extract returns
`SIGIL_ERR_NEEDS_KEY`, and with keys that don't open the content (a key file
older than the dump's key generation, or a wrong header key)
`SIGIL_ERR_KEYS_INCOMPATIBLE`.

## Save layouts

| Layout | Files (role, option) | Shared | Source |
|---|---|---|---|
| `eden`, `citron`, `sudachi`, `yuzu` | folders per profile, see Profiles: `nand/user/save/0000000000000000/{profile}/{save_id}/` account, `.../00000000000000000000000000000000/{save_id}/` device | | EDEN `src/core/file_sys/savedata_factory.cpp`; subdirs `nand/user/save/0000000000000000`, `nand/system/save/8000000000000010/su/avators` |

The yuzu forks keep a game's saves per user profile, and also keep saves every profile on the device shares. [Save units](../save-units.md#profiles) has the rules for picking a profile.

| Layout | Account | Device | Profile list |
|---|---|---|---|
| yuzu forks | `nand/user/save/0000000000000000/{profile}/{save_id}/` | the same under the all-zero user | `nand/system/save/8000000000000010/su/avators/profiles.dat`; the folder is the profile's UUID with its bytes reversed |

A profile id is named as its save folder is, such as `125D2DBAEBDEB11000296E1E1ECBF401`. The root is the emulator's base folder, the one holding `nand/`.

A
yuzu-fork unit holds `<save_id>/...` for the account save and
`device/<save_id>/...` for the device save. The size file each yuzu fork keeps in a save folder
(`.yuzu_save_size`, `.citron_save_size`, `.sudachi_save_size`,
`.suyu_save_size`) is never collected, written or removed.

The rows live in [`src/save_layout.c`](../../src/save_layout.c).

Verified on hardware:

- **verified (AYN Odin 3, Eden and Citron on Android, 2026-10-04)**: each emulator's `nand/system/save/8000000000000010/su/avators/profiles.dat` holds one profile, and that profile's save folder is its UUID with the bytes reversed (Eden: UUID `01F4CB1E1E6E290010B1DEEBBA2D5D12`, folder `125D2DBAEBDEB11000296E1E1ECBF401`; fixtures `eden-profile`, `citron-profile`). Eden also keeps a stray folder `1000296E1E1ECBF40100000000000000` matching no profile, holding only `.yuzu_save_size`: folders that match no profile are not a user's.
- **verified**: Animal Crossing: New Horizons is a device save. Its JKSV `.nx_save_meta.bin` (from a real console) has `saveDataType` 3, Device in the Switch's enum (Account is 1), account id all zeros, owner `01006F8002326000`; Eden keeps it in the all-zero user folder. The whole island travels as one device save with no profile.

## Sync

On a layout
with profiles the unit is a zip of the game's save folders under the names
above, and `companions` is `SIGIL_ERR_INVALID_ARG`.

Without
`open`, a yuzu fork's list can't be read, so pass `profile` or a root inside
the profile's folder.

[Sync](../sync.md) has the rest of collect and restore.

## Emulator research

Scope: in-game saves only (no save states). Sources pinned to the default branch as fetched 2026-09-26. Line numbers are for that snapshot; re-pin to a commit SHA before citing in save sync. Anything without a source is marked UNVERIFIED.

Abbreviations: `DOL` = https://github.com/dolphin-emu/dolphin/blob/master, `CEMU` = https://github.com/cemu-project/Cemu/blob/main, `EDEN` = https://git.eden-emu.dev/eden-emu/eden/src/branch/master, `CIT` = https://github.com/citron-neo/emulator/blob/main, `LIBHAC` = LibHac as vendored in https://github.com/hyjinx-emu/Hyjinx/tree/master/src/Hyjinx.LibHac (the canonical LibHac host, git.ryujinx.app/ryubing/libhac, refused connections from here; the GitHub original Thealexbarney/LibHac is gone).

| Platform | Neutral per-game unit | Identity key |
| --- | --- | --- |
| Switch | Directory tree = root of the save filesystem (what JKSV zips / what sits in yuzu's title dir / Ryujinx `.../0/`) | application ID + account UID (+ save type) |

### Row 7. Switch / yuzu forks: Eden, Citron, Sudachi, Suyu

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

### Row 8. Switch / Ryujinx (Ryubing)

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

### Row 9. Switch / JKSV (real hardware backup format)

- Files and path: `sdmc:/JKSV/<path-safe title>/<path-safe nickname> - <date>/` (folder) or `.../<name>.zip`. Working directory is configurable. Source: https://github.com/J-D-K/JKSV/blob/rewrite/source/appstates/BackupMenuState.cpp L31 (`get_working_directory() / get_path_safe_title()`), L288-292 (default name `"%s - %s"`, nickname and date); `source/tasks/backup.cpp` L58-85 (zip vs folder).
- Format: the save filesystem root copied as-is (`copy_directory(DEFAULT_SAVE_ROOT, path)` or `copy_directory_to_zip`), plus `.nx_save_meta.bin` at the root of the folder/zip. `backup.cpp` L66-85.
- `.nx_save_meta.bin` (packed, 86 bytes, little-endian): magic u32 `0x56534B4A` ("JKSV") @0x00, revision u8 (=1) @0x04, applicationID u64 @0x05, accountID 16 bytes @0x0D, systemSaveID u64 @0x1D, saveDataType u8 @0x25, saveDataRank u8 @0x26, saveDataIndex u16 @0x27, ownerID u64 @0x29, timestamp u64 @0x31, flags u32 @0x39, saveDataSize i64 @0x3D, journalSize i64 @0x45, commitID u64 @0x4D, saveDataSpaceID u8 @0x55. Filled from `FsSaveDataExtraData`. Source: `include/fs/SaveMetaData.hpp` (struct, magic, filename), `source/fs/SaveMetaData.cpp` `fill_save_meta_data` L15-37. Offsets computed from the packed struct.
- On restore JKSV only uses the meta to extend the target save if `saveDataSize` is larger (`process_save_meta_data` L39-50); it does not rebind account or title. Older JKSV releases (pre-rewrite `master`) write no meta file (UNVERIFIED which release introduced it).
- Scope: PER-GAME, per user.
- Extraction/injection: lossless for file content. Mapping a JKSV backup onto an emulator = copy the tree minus `.nx_save_meta.bin` into the yuzu-fork title dir or Ryujinx `<id>/0/`. The meta file is the only portable carrier of the save's declared size, which matters when the target save must be created or extended.
- Neutral per-game form: this is the de facto neutral form (tree + optional `.nx_save_meta.bin`).
- Profile/user path components: nickname in the backup folder name (display only); the UID is in the meta file.

### Per-user and device saves

- **verified**: saves are per application per user, plus "device" saves with no user. yuzu forks keep device saves under user `0` (32 zeros) in the legacy layout, and under `user/save/device/<TITLEID>/0/` in the newer layout. Ryujinx keys every save by `(ProgramId, UserId, Type)` in `imkvdb.arc`, where the type tells account from device saves (enum values UNVERIFIED). See rows 7 and 8 above.
- **verified**: JKSV backups carry `.nx_save_meta.bin` with `saveDataType`, the account id and the declared save size, so a JKSV unit says whether it is an account or device save. See row 9.

### Notes for save sync

- The only shared containers are the Dolphin raw card (`.raw`) and, logically, the Ryujinx indexer. A client that supports raw cards needs a GC memcard directory/BAT writer, or should steer users to GCI folder mode (Dolphin's default).
- Identity fields a client must compute locally: GC 4-char gamecode (from disc ID) plus the per-file DEntry identity; Wii 64-bit title ID; Wii U title ID + Cemu persistent ID; Switch application ID + profile UUID (byte-reversed for yuzu legacy layout, raw for "future" layout), and for Ryujinx the save data ID from `imkvdb.arc`.

## Open items

sigil has no layout for Ryubing or for the yuzu forks' newer layout, and reads no NACP.

- Ryubing (the live Ryujinx fork, git.ryujinx.app) names save folders by an allocated id from `imkvdb.arc`, so its restore needs that lookup, and a title it has never booted has no folder yet.
- Animal Crossing: New Horizons is a device save (see Save layouts); Mario Kart 8 Deluxe's split is not shown yet.
- **not shown**: Mario Kart 8 Deluxe splitting its data. On the Odin, its device folder's `sg33.dat` is byte-identical to the account copy beside `ghostlist.dat` and `userdata.dat` (fixture `mk8d-eden-split`), so it reads as a copy, not a device save the game wrote. Its NACP device save size would settle it.
- **lead**: each game's control data (NACP, in the control NCA's RomFS as `control.nacp`) declares the save areas the game asks the system to create: user account save size and journal size, device save size and journal size, BCAT delivery cache size, temporary storage size, cache storage size, plus `StartupUserAccount` (whether the game requires a user to be picked at boot). A nonzero device save size would be the flag that a game keeps device-wide data. Field offsets are on switchbrew's NACP page; check them before use.
- **lead**: Animal Crossing: New Horizons keeps the island for every resident in one save, and its island transfer tool exists because the island is tied to the console. Whether that save is an account save, a device save or both is what the NACP check above would answer. Mario Kart 8 Deluxe: same question, not checked.
- What sigil reads today: Switch NSP, XCI and NCA headers, decrypted with the header key, for the title id (`src/switch_nca.c`). Reading the NACP needs the control NCA's section decrypted, which needs the key-area keys (and the title key for NCAs with a rights id) from the user's `prod.keys` and `title.keys`. That is a larger step than anything sigil does with keys now.

Next checks:

1. Read the NACP of a few Switch titles with a known split (Animal Crossing: New Horizons, Mario Kart 8 Deluxe) and of a few without one, using the local ROM corpus and its `prod.keys`, and confirm the field offsets.
2. Check how Ryujinx and the yuzu forks lay out a device save on disk for one of those titles.
