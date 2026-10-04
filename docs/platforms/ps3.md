# PlayStation 3

Status: synced
sigil reads `TITLE_ID` from `PARAM.SFO`, finds every save folder starting with it under the RPCS3 user profile, and syncs them with collect and restore.

## Identification

| Slug | Inputs | `title_id` example | `usage` | Status |
|---|---|---|---|---|
| `ps3` | `.iso` (needs hint), game folder (recursive) or `.sfo` | `BLUS31426` (TITLE_ID from PARAM.SFO) | folder-prefix | experimental |

`experimental` is `1` on PS3 results: the extractor hasn't been validated
against real-world samples.

**PS3 reads PARAM.SFO (experimental).** Reads the `TITLE_ID` string
(`BLUS31426`) from PARAM.SFO, which is reached three ways: the SFO file
directly, an extracted-game folder (sigil takes the shallowest
`PARAM.SFO` up to 4 levels down, the smallest path when two sit equally
deep, so an update's or a trophy set's SFO further down never wins), or a
disc image. Which shape it is gets decided from the `\0PSF`
magic rather than assumed, so a disc image is never parsed as though its
first sector were an SFO.

On a disc the file sits at `PS3_GAME/PARAM.SFO`, with a copy at the root
on some releases; sigil checks both, the same two locations aPS3e looks
in. PS3 discs carry a plain ISO9660 descriptor for their directory
structure, so no UDF reader is involved. `.iso` is ambiguous across half
a dozen platforms and needs an explicit `ps3` hint.

Saves land in `dev_hdd0/home/<user>/savedata` under directories that
start with the title id and carry a per-artifact suffix
(`BCUS99086GAMEDATA`), so `usage` is `folder-prefix` and consumers
enumerate by prefix. PKG and encrypted-EBOOT inputs are not supported.

## Save layouts

| Layout | Files (role, option) | Shared | Source |
|---|---|---|---|
| `rpcs3` | folders per profile: `dev_hdd0/home/{profile}/savedata/<every folder starting with {save_id}>/` | | RPCS3 `cellSaveData.cpp`; subdir `dev_hdd0/home` |

The row matches `ps3` only. RPCS3 keeps a game's saves per user profile,
with no device area:

| Layout | Account | Device | Profile list |
|---|---|---|---|
| `rpcs3` | `dev_hdd0/home/{profile}/savedata/{save_id}*/` | | `dev_hdd0/home/{profile}/localusername` |

The base folder is the one holding `dev_hdd0/`, and a profile id is the
user folder's name (`00000001`). How sigil picks the profile is in
[profiles](../save-units.md#profiles).

**PS3 on aPS3e** (`aenu.aps3e`).
`aps3e/config/dev_hdd0/home/00000001/savedata/`. Directories begin with
`save_id` and carry a per-artifact suffix, so enumerate by prefix as
`folder-prefix` says. The user is hardcoded to `00000001`, unlike desktop
RPCS3 where several can exist.
The root sits under `/storage/emulated/0/Android/data/<package>/files/`;
see [saves on Android](../saves-on-android.md).

## Sync

On a layout with profiles the unit is a zip of the game's save folders,
and `companions` is `SIGIL_ERR_INVALID_ARG`. The general rules for profile
layouts are in [sync](../sync.md).

## Emulator research

Research date 2026-09-26. Source links are permalinks to the commit that was HEAD on that date unless marked otherwise.

Pinned commits used below:

- rpcs3 `4514ad6216f61ac309f970f29c75fd7d586830dc` (R3)

Anything without a source link is marked UNVERIFIED.

| Emulator | Files written, template, default location | Format | Scope | Per-game extraction | Source |
|---|---|---|---|---|---|
| RPCS3 | `$(EmulatorDir)dev_hdd0/home/<userId %08u>/savedata/<DIRNAME>/` (default user `00000001`). `DIRNAME` is chosen by the game, normally `<TITLE_ID><suffix>` (for example `BLUS30443-AUTOSAVE`). Each folder: `PARAM.SFO` (`SAVEDATA_DIRECTORY`, etc.), `ICON0.PNG`, optional `ICON1.PAM`, `PIC1.PNG`, `SND0.AT3`, data files | Per-title folder, **unencrypted**. RPCS3 does not create `PARAM.PFD`. It records "protected" files as `*<name>=1` integer entries in `PARAM.SFO` and keeps an `RPCS3_BLIST` section | Per title. Games can list and read other `DIRNAME`s (the game supplies a prefix list), so sequels and region variants cross-read. `dev_hdd0/game/<TITLE_ID>/` is installed game data or updates, not saves | Folder is the unit; lossless between RPCS3 instances. Real PS3 saves are encrypted with `PARAM.PFD`, and RPCS3 has no crypto layer (issue #9580 open). Import or export needs Apollo Save Tool on a CFW/HEN PS3 (community method) | R3 `rpcs3/Emu/vfs_config.h` L13; R3 `rpcs3/Emu/Cell/Modules/cellSaveData.cpp` L155, L251-267, L312, L792, L1635, L1716-1724, L2263-2271; https://github.com/RPCS3/rpcs3/issues/9580 ; https://forums.rpcs3.net/archive/index.php/thread-206289.html |
| aPS3e (Android RPCS3 port) | `Android/data/<pkg>/files/aps3e/config/dev_hdd0/home/00000001/savedata/<DIRNAME>/` (app data dir = `getExternalFilesDir("aps3e")`; package id `aenu.aps3e` inferred from the Java namespace, UNVERIFIED) | Same as RPCS3 (bundled RPCS3 `cellSaveData.cpp`) | Same | Same | https://github.com/aenu1/aps3e/blob/main2/app/src/main/java/aenu/aps3e/MainActivity.java L95-103; .../Application.java L22; .../app/src/main/cpp/rpcs3/rpcs3/Emu/Cell/Modules/cellSaveData.cpp L293, L752 |

### Container split summary

| Container | Lossless per-game split? | How | Identity key |
|---|---|---|---|
| PS3 `dev_hdd0/home/*/savedata/` | Yes | Copy folder | `DIRNAME` prefix = title ID (game-chosen) |

### User profiles

- **verified**: RPCS3 keeps saves under `dev_hdd0/home/<userId %08u>/savedata/<DIRNAME>/`, default user `00000001`; each save folder has its own `PARAM.SFO`. See [sony.md](#emulator-research).
- **lead**: a PS3 save's `PARAM.SFO` carries the owning account id and a copy-protection flag that stops a save being used by another account on real hardware. Game data installs (`dev_hdd0/game/<id>/`) are per console, not per user, and some games keep progress-like data there. Not checked.

## Open items

- Real-hardware interop for Vita and PS3 needs encryption tooling outside RomM's reach. Emulator-to-emulator folder copies are lossless.
- PKG and encrypted-EBOOT inputs are not supported.
- The `rpcs3` row collects every save folder whose name starts with `save_id`. A game picks its folder names at run time, and nothing in the game's files records them, so a game that saves under another title's id is not found. A client that knows the id the game uses can set `save_id` to it on the result it passes to collect and restore.
- The extractor is experimental until validated against real-world samples.
