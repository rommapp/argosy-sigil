# Wii

Status: synced

## Identification

| Slug | Platform | Inputs | `title_id` example | `usage` | Status |
|---|---|---|---|---|---|
| `wii` | Wii | `.iso`, `.rvz`, `.wbfs`, `.wad` | `525A5445` (hex of ASCII gameId); `.wad`: `00010001574B5445` (full 16-hex title id) | folder-exact; `.wad`: folder-split | |

See [Identification](../identification.md) for the result fields.

Wii and Wii U diverge the same way and for the same reason: Dolphin
writes `Wii/title/00010000/525a4445` and Cemu writes
`mlc01/usr/save/00050000/1010ec00`, both with `{:08x}`, so `save_id` is
the lowercase form of `title_id` on those two platforms
(`title_id=525A4445`, `save_id=525a4445`).

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

**In a Wii `.wbfs` the disc header moves; it does not disappear.** A WBFS
file wraps a real disc header behind its own container header; the
wrapped header starts at the first hd sector, whose size the container
records as a shift at offset 8. Sigil reads the id there only when a
console magic backs it (Wii `5D1C9EA3` at +0x18, GameCube `C2339F3D`
at +0x1C). That check is not decoration: `WBFS` is four uppercase ASCII
bytes, so without it the container magic itself passes as a game id and
every wbfs dump collapses to the same bogus `57424653`.

**Wii `.wad` files hold WiiWare and Virtual Console channels.** A WAD is the
NAND install package: a 0x20-byte header, then the certificate chain,
ticket, TMD, data and footer, each 64-byte aligned in that order. The
header holds a size of 0x20 at offset 0, the type at 4 (`Is` 0x4973
installable, `ib` 0x6962 boot2, `Bk` 0x426B backup), the certificate
chain size at 8, the ticket size at 0x10 and the TMD size at 0x14. The
8-byte title id sits at ticket offset 0x1DC, and at TMD offset 0x18C
for a ticketless package; sigil reads the ticket first and falls back
to the TMD. The upper four bytes are the category (`00010001` WiiWare
and Virtual Console, `00010004` channels that ship with a disc game),
the lower four are the ASCII game code, and a package whose lower half
is not `[A-Z0-9]` (IOS, system titles) has no save and returns
`SIGIL_ERR_NOT_FOUND`. `title_id` is the full 16 hex, `raw_serial` the
ASCII code, and `save_id` is `<category>/<code>` in lowercase with
`usage` folder-split, because Dolphin keeps the save at
`Wii/title/<category>/<code>/data` and the category is not fixed. Disc
saves keep their `Wii/title/00010000/<code>` form, so a consumer's Wii
root is `Wii/title/00010000` for discs and `Wii/title` for WADs.

## Save formats

Dolphin keeps a Wii save as the folder `Wii/title/<category>/<code>/data/`
in its emulated NAND, the game's files as the game wrote them. Saves are
per title, with no user accounts. A real console exports saves as a
signed `data.bin`; Dolphin imports and exports that form.

## Save layouts

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| `dolphin` | folder: `User/Wii/title/{category}/{save_id}/data/` | | emulator source |
| `dolphin_standalone` | folder: `Wii/title/{category}/{save_id}/data/` | | emulator source |

`{category}` is `00010000` for a disc, whose `save_id` is the code alone,
and the WAD's own category from a `save_id` of `<category>/<code>`. The
libretro core keeps its NAND in the save folder's `User/`, so its root is
the save folder; standalone Dolphin's root is its User folder, the one
holding `Wii/` and `GC/`. The same ids name the GameCube rows, picked by
the platform.

## Sync

The unit is a zip of the title's `data/` folder under its code:
`52534245/data/banner.bin`, `52534245/data/...`. The category stays out,
so the request's own category places it. Installed content beside the
save (`content/`) stays out of the unit, and restore leaves it out of an
older unit that carries it: Argosy uploaded the whole title folder,
rooted at the code, and restores the same as sigil's unit. The rules for
folder layouts are in [sync](../sync.md) and
[profiles](../save-units.md#profiles).

## Open items

- Which games write to SD or `shared2/`, and whether Dolphin verifies `data.bin` signatures on import, are unconfirmed.
- A console's exported `data.bin` isn't read or written.
