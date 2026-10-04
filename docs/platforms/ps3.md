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

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| `rpcs3` | folders per profile: `dev_hdd0/home/{profile}/savedata/<every folder starting with {save_id}>/` | | emulator source |

The layout lists `dev_hdd0/home`, which `sigil_save_layout_subdirs()`
names. The row matches `ps3` only. RPCS3 keeps a game's saves per user
profile, with no device area:

| Layout | Account | Device | Profile list |
|---|---|---|---|
| `rpcs3` | `dev_hdd0/home/{profile}/savedata/{save_id}*/` | | `dev_hdd0/home/{profile}/localusername` |

The base folder is the one holding `dev_hdd0/`, and a profile id is the
user folder's name (`00000001`). How sigil picks the profile is in
[profiles](../save-units.md#profiles).

RPCS3 writes save folders unencrypted, so copying a folder moves a save
between RPCS3 installs without loss. A real console's saves are
encrypted, and moving one to or from hardware needs a modded console's
tools.

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

## Open items

- Real-hardware interop for Vita and PS3 needs encryption tooling outside RomM's reach. Emulator-to-emulator folder copies are lossless.
- PKG and encrypted-EBOOT inputs are not supported.
- The `rpcs3` row collects every save folder whose name starts with `save_id`. A game picks its folder names at run time, and nothing in the game's files records them, so a game that saves under another title's id is not found. A client that knows the id the game uses can set `save_id` to it on the result it passes to collect and restore.
- The extractor is experimental until validated against real-world samples.
