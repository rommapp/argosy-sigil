# PSP

Status: in development
sigil reads the title id from the disc image; no layout row locates PSP saves yet, so collect and restore don't cover them.

PS1 classics running on a PSP use the `vita_pops` layout, which is on the [PlayStation](psx.md) page.

## Identification

| Slug | Inputs | `title_id` example | `usage` | Status |
|---|---|---|---|---|
| `psp` | `.iso`, `.chd`, `.cso` / `.ciso` | `ULUS10064` | folder-prefix | `.cso` experimental |

**PSP saves share a folder prefix.** A single game produces multiple sibling
folders under `PSP/SAVEDATA/`, e.g. `ULUS10064DATA00`,
`ULUS10064SETTINGS`, `ULUS10064SAVE01`. Sigil emits the 9-char prefix
(`ULUS10064`); consumers MUST enumerate every folder under the parent
that starts with that prefix.

`folder-prefix` example: `ULUS10064DATA00`, `ULUS10064SETTINGS`.

`raw_serial` keeps the ID exactly as it appears in the binary, before any
normalization (`ULUS-10064`).

**PSP `.cso` / `.ciso` support is experimental.** v1 CSO with raw-deflate
blocks is decompressed transparently and fed to the standard PSP
extractor. v2 (LZ4) is not supported. Flagged `experimental=1` on the
result until validated against a real CSO sample.

## Emulator research

Research date 2026-09-26. Source links are permalinks to the commit that was HEAD on that date unless marked otherwise.

Pinned commits used below:

- ppsspp `cae623f4e6c197f45662358ffc4605e3fb97298e` (PP)

Anything without a source link is marked UNVERIFIED.

| Emulator | Files written, template, default location | Format | Scope | Per-game extraction | Source |
|---|---|---|---|---|---|
| PPSSPP (standalone) | `<memstick>/PSP/SAVEDATA/<gameName><saveName>/` with `PARAM.SFO`, `ICON0.PNG`, optional `ICON1.PMF`, `PIC1.PNG`, `SND0.AT3`, plus the game's data files. `gameName` and `saveName` come from the game's `SceUtilitySavedataParam`. `gameName` is usually the disc ID (for example `ULUS10041`), and the suffix is game-chosen | Per-title folder. `PARAM.SFO` holds `SAVEDATA_DIRECTORY`, `SAVEDATA_FILE_LIST` (per-file hashes) and `SAVEDATA_PARAMS` (hash). With `EncryptSave=true` (default, per-game setting) secure-mode saves are encrypted the same way a real PSP does it, so folders stay portable to hardware | Per title. A game can read and list other titles' folders (sequel carry-over; `gameName` is whatever the game passes), so "files for serial X" is a naming convention the game chooses. The same `SAVEDATA/` root also holds **game-data installs** (`sceUtilityGameDataInstall`) at `SAVEDATA/<gameName><dataName>/`, which can be large and are not save progress | The folder is the neutral unit. Copy it whole; never edit files without updating the SFO hashes | PP `Core/Dialog/SavedataParam.cpp` L41-50 (`ms0:/PSP/SAVEDATA/`), L239-280, L455 (`bEncryptSave`), L495-548 (SFO fields, hash); PP `Core/Dialog/PSPGamedataInstallDialog.cpp` L33, L227, L264-267; PP `Core/Config.cpp` L1088 |
| PPSSPP libretro core | `<RetroArch savedir>/PSP/SAVEDATA/...` (memstick root = frontend save directory). System assets in `<system>/PPSSPP/` | Same | Same | Same | PP `libretro/libretro.cpp` L1221-1269 (`g_Config.memStickDirectory = retro_save_dir`) |

Default standalone memstick root: Windows `Documents/PPSSPP` or `memstick/` next to the exe when portable. Android is the user-chosen storage folder. Both UNVERIFIED.

### Container split summary

| Container | Lossless per-game split? | How | Identity key |
|---|---|---|---|
| PSP `SAVEDATA/` | Yes, folders are independent | Copy folder | Folder prefix = game-supplied `gameName`; separate game-data installs from saves |

## Open items

- No layout row covers PPSSPP, standalone or libretro.
- Vita3K and PPSSPP default roots per OS are UNVERIFIED.
- `.cso` v2 (LZ4) is not supported, and v1 has not been validated against a real CSO sample.
