# PSP

Status: synced

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

## Save formats

PPSSPP, standalone and libretro, keeps each save as a folder under
`PSP/SAVEDATA/` named by the game: a disc id plus a suffix the game
picks. The folder's `PARAM.SFO` carries hashes of its files, so copy a
folder whole and never edit a file inside it. A game can read other
titles' folders for sequel carry-over. The same root also holds game-data
installs, which are not save progress. The libretro core uses the
frontend's save folder as the memory stick root.

## Save layouts

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| `ppsspp` | folders: `PSP/SAVEDATA/<every folder starting with {save_id}>/` | | emulator source |
| `ppsspp_standalone` | folders: `PSP/SAVEDATA/<every folder starting with {save_id}>/` | | emulator source |
| `psp_console` | folders: `PSP/SAVEDATA/<every folder starting with {save_id}>/` | | emulator source |

The layouts list `PSP/SAVEDATA`, which `sigil_save_layout_subdirs()`
names, and match `psp` only. The base folder is the memory stick, the one
holding `PSP/`: PPSSPP's memory stick folder, the libretro core's save
folder, a PSP's `ms0:/`, or a Vita's `ux0:pspemu/` under Adrenaline. The
PSP keeps no user profiles, so every folder is a device folder:

| Layout | Account | Device | Profile list |
|---|---|---|---|
| `ppsspp`, `ppsspp_standalone`, `psp_console` | | `PSP/SAVEDATA/{save_id}*/` | none |

## Sync

The unit is a zip of the game's save folders, each under its own name
(`ULUS10064DATA00/PARAM.SFO`), and `companions` is
`SIGIL_ERR_INVALID_ARG`. Folders travel whole, so the hashes in each
`PARAM.SFO` stay valid. The rules for folder layouts are in
[sync](../sync.md) and [profiles](../save-units.md#profiles).

Game-data installs share `PSP/SAVEDATA/` and the game's prefix. Every
save's `PARAM.SFO` carries `SAVEDATA_PARAMS` and `SAVEDATA_FILE_LIST`; an
install's carries neither. A folder whose `PARAM.SFO` reads as an SFO
holding neither key is left out of the unit, and restore never removes it.
A folder with no `PARAM.SFO`, or one that won't open or parse, stays with
the saves. Telling them apart reads the file, so locate without `open`
keeps every folder that starts with the game's id.

## Open items

- `psp_console` is not yet confirmed on a live PSP or Adrenaline: copy a game's folders off, check their names start with the disc id sigil reads, then restore to a stick without them and load the save in the game.
- A game picks its folder names at run time; one that saves under another title's id is not found. A client that knows the id can set `save_id` to it on the result it passes to collect and restore.
- A sequel reading its prequel's folders is not synced as a companion.
- PPSSPP's default memory stick root per OS is unconfirmed.
- `.cso` v2 (LZ4) is not supported, and v1 has not been validated against a real CSO sample.
