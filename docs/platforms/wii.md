# Wii

Status: synced

## Identification

| Slug | Platform | Inputs | `title_id` example | `usage` | Status |
|---|---|---|---|---|---|
| `wii` | Wii | `.iso`, `.rvz`, `.wia`, `.wbfs`, `.wad` | `525A5445` (hex of ASCII gameId); `.wad`: `00010001574B5445` (full 16-hex title id) | folder-split (`save_id` is `<category>/<code>` from the ticket) | |

See [Identification](../identification.md) for the result fields.

**A disc's `save_id` comes from its ticket.** Dolphin names a disc's save
folder after the title id in the game partition's ticket
(`VolumeWii::GetTitleID`), as `Wii/title/<category>/<code>/data` with
`{:08x}`. Most discs carry category `00010000`; a disc that installs a
channel carries `00010004` (Mario Kart Wii, Wii Fit, Wii Fit Plus). So
`save_id` is `<category>/<code>` in lowercase, the same shape as a WAD's,
with `usage` folder-split: `00010000/525a4445` for Twilight Princess,
`00010004/524d4345` for Mario Kart Wii. `title_id` stays the hex of the
disc's gameId.

Sigil finds the game partition as Dolphin does: the first partition of
type 0 in the table at 0x40000, four groups of a count and a table
offset, offsets stored shifted right by 2. The title id sits at ticket
offset 0x1DC, the ticket at the start of the partition. It reads it
through the container: directly in an `.iso`, through the cluster table
in a `.wbfs` (Dolphin's `WbfsBlob.cpp`), and from the raw data groups of
a `.wia` or `.rvz` (Dolphin's `docs/WiaAndRvz.md`), decompressing the
group and undoing RVZ packing. Sigil decodes WIA and RVZ groups stored
uncompressed, PURGE, LZMA and Zstandard; a file compressed with bzip2 or
LZMA2 gets no `save_id`. Neither does a disc whose ticket can't be read,
or whose ticket names no disc category, since a guessed category points
at a folder Dolphin never writes. The filename fallback has no ticket and
keeps the code alone, which the layout rows place under `00010000`.

**Wii and GameCube title ids are the hex of ASCII.** The disc header
carries a 4-character ASCII gameId (`RZTE`, `GZLE`). `title_id` is the
hex encoding of those bytes (`52535445`, `475A4C45`) on both consoles,
and `raw_serial` preserves the ASCII form. Where they land on disk
differs: Dolphin's Wii NAND directory is the hex, lowercased, while a
GameCube `.gci` file name carries the ASCII characters, so `save_id`
follows the platform. The header
starts at 0 in an `.iso` and at 0x58 in an `.rvz` or `.wia`,
behind the container header; a console magic backs it (Wii
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
`Wii/title/<category>/<code>/data` and the category is not fixed. Discs
use the same shape, so a consumer's Wii root is `Wii/title` for both.

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

`{category}` comes from a `save_id` of `<category>/<code>`, a disc's or
a WAD's. A `save_id` of the code alone, from the filename fallback or a
result persisted before discs carried their category, takes `00010000`. The
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
- WIA and RVZ files compressed with bzip2 or LZMA2 get no `save_id`: sigil bundles neither decoder. Dolphin compresses RVZ with Zstandard by default.
