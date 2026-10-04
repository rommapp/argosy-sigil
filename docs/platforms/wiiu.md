# Wii U

Status: synced
sigil identifies Wii U WUA archives and collects and restores Cemu save folders, per account and shared by every account.

## Identification

| Slug | Platform | Inputs | `title_id` example | `usage` | Status |
|---|---|---|---|---|---|
| `wiiu` | Wii U | `.wua` | `10143500` (last 8 of folder name) | folder-exact | |

See [Identification](../identification.md) for the result fields. A `.wua` is a ZArchive; [Containers](../containers.md) covers how sigil reads it.

Wii and Wii U diverge the same way and for the same reason: Dolphin
writes `Wii/title/00010000/525a4445` and Cemu writes
`mlc01/usr/save/00050000/1010ec00`, both with `{:08x}`, so `save_id` is
the lowercase form of `title_id` on those two platforms
(`title_id=525A4445`, `save_id=525a4445`).

**Wii U uses the last 8 of 16 hex digits.** WUA archives carry a top-level folder
named `00050000<8 hex>_v0`. The full 16 hex is the formal title ID;
the last 8 chars are what the save system keys on. Sigil emits the
last 8 as `title_id`, the full 16 as `raw_serial`.

## Save layouts

| Layout | Files (role, option) | Shared | Source |
|---|---|---|---|
| `cemu` | folders per profile: `mlc01/usr/save/00050000/{save_id}/user/{profile}/` account, `.../user/common/` and `.../meta/` device | | Cemu `SaveInfo.cpp`, `Account.cpp`; subdirs `mlc01/usr/save/00050000`, `mlc01/usr/save/system/act` |

Cemu keeps a game's saves per user profile, and also keeps saves every profile on the device shares. [Save units](../save-units.md#profiles) has the rules for picking a profile.

| Layout | Account | Device | Profile list |
|---|---|---|---|
| `cemu` | `mlc01/usr/save/00050000/{save_id}/user/{profile}/` | `user/common/` and `meta/` beside it | `mlc01/usr/save/system/act/{profile}/account.dat` (`PersistentId`, `MiiName`) |

A profile id is named as its save folder is, such as `80000001`. The root is the emulator's base folder, the one holding `mlc01/`.

A Cemu unit holds
`<save_id>/meta/`, `<save_id>/user/account/` and `<save_id>/user/common/`; an
older unit naming the account folder by its id restores as the account
save.

The row lives in [`src/save_layout.c`](../../src/save_layout.c).

Verified on hardware:

- **verified (Cemu on an AYN Odin 3, 2026-10-04)**: the `meta/meta.xml` in each save folder declares `common_save_size` and `account_save_size`, and they predict where the save sits. Nintendo Land declares common 4 MiB and account 256 KiB and keeps its whole save in `user/common/`, with nothing under the account; Wind Waker HD, MH3U, Splatoon and Breath of the Wild declare common 0 and keep everything under `user/80000001/`. So sigil reads the split from the save folder itself, with no keys. One account, `act/80000001/account.dat` (fixture `cemu-account`). The same `meta.xml` sits in the title's own `meta/` (WUA archives are decrypted; encrypted WUD/WUX need keys).

## Sync

On a layout
with profiles the unit is a zip of the game's save folders under the names
above, and `companions` is `SIGIL_ERR_INVALID_ARG`.

On restore, Cemu's `meta/`, which
Cemu writes again by itself, is skipped instead of failing with `SIGIL_ERR_NO_TARGET` when no folder of the game takes it.
[Sync](../sync.md) has the rest of collect and restore.

## Emulator research

Scope: in-game saves only (no save states). Sources pinned to the default branch as fetched 2026-09-26. Line numbers are for that snapshot; re-pin to a commit SHA before citing in save sync. Anything without a source is marked UNVERIFIED.

Abbreviations: `DOL` = https://github.com/dolphin-emu/dolphin/blob/master, `CEMU` = https://github.com/cemu-project/Cemu/blob/main, `EDEN` = https://git.eden-emu.dev/eden-emu/eden/src/branch/master, `CIT` = https://github.com/citron-neo/emulator/blob/main, `LIBHAC` = LibHac as vendored in https://github.com/hyjinx-emu/Hyjinx/tree/master/src/Hyjinx.LibHac (the canonical LibHac host, git.ryujinx.app/ryubing/libhac, refused connections from here; the GitHub original Thealexbarney/LibHac is gone).

| Platform | Neutral per-game unit | Identity key |
| --- | --- | --- |
| Wii U | Directory trees `user/<persistentId>/` and `user/common/` (+ optional `meta/`) | 64-bit title ID (+ persistent ID for the user part) |

### Row 6. Wii U / Cemu

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

### Per-user saves

- **verified**: each title's save directory holds `user/<persistentId>/` per account and `user/common/` shared by every account on the console, plus `meta/`. Cemu's own export drops `common/`, so a client must carry it itself. See row 6 above.

### Notes for save sync

- Every format here is lossless at the per-game level when the emulator is not running the game. The two lossy spots are Cemu's built-in export (drops `common/`) and the GC raw-card import path (rewrites DEntry block pointers and copy counter, re-signs F-Zero GX/PSO).
- Identity fields a client must compute locally: GC 4-char gamecode (from disc ID) plus the per-file DEntry identity; Wii 64-bit title ID; Wii U title ID + Cemu persistent ID; Switch application ID + profile UUID (byte-reversed for yuzu legacy layout, raw for "future" layout), and for Ryujinx the save data ID from `imkvdb.arc`.

## Open items

1. Read `meta.xml` from a Wii U WUA in the corpus and confirm the `common_save_size` and `account_save_size` fields.
