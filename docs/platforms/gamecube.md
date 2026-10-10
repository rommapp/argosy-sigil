# GameCube

Status: synced
sigil identifies GameCube discs and collects and restores Dolphin saves, from a GCI folder or a raw memory card.

## Identification

| Slug | Platform | Inputs | `title_id` example | `usage` | Status |
|---|---|---|---|---|---|
| `gamecube` | GameCube | `.iso`, `.rvz`, `.wia`, `.wbfs` | `475A4C45` (hex of ASCII gameId) | file-prefix | |

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
starts at 0 in an `.iso` and at 0x58 in an `.rvz` or `.wia`,
behind the container header; a console magic backs it (Wii
`5D1C9EA3` at +0x18, GameCube `C2339F3D` at +0x1C). That magic also
names the console when nobody else does, since `.iso`, `.rvz` and
`.wbfs` carry either one and the extension cannot tell them apart. A
platform passed in wins over the magic, so a caller that has already
classified the file keeps its answer; with no platform and no magic
there is nothing left to read, and the extraction fails rather than
guessing from the name.

The [Wii page](wii.md#identification) covers how sigil finds the disc header inside a `.wbfs` file.

## Save layouts

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| `dolphin` | | `User/GC/{gc_region}/Card A/`, one `.gci` file per save, when Dolphin.ini's `SlotA` = `8` (default); `User/GC/MemoryCardA.{gc_region}.raw` when `SlotA` = `1`, or `.1019.raw`, `.507.raw`, `.251.raw`, `.123.raw`, `.59.raw` for smaller cards, picked by `MemoryCardSize` (`-1` for 2043 blocks, `4` to `0` for 1019 to 59). Without that option, the one card file there; with two or more there, collect and restore return `SIGIL_ERR_AMBIGUOUS` naming them. A raw card restore creates is the size `MemoryCardSize` sets. `MemoryCardSize` sizes the GCI folder too, and restore refuses a save Dolphin wouldn't load from it | emulator source; live save files |
| `dolphin_standalone` | | as `dolphin`, rooted at Dolphin's User folder (`GC/{gc_region}/Card A/`, `GC/MemoryCardA.{gc_region}.raw`) | emulator source, as `dolphin` |

The libretro core's User folder is the save folder's `User/`, so
`dolphin` lists `User/GC` and `dolphin_standalone` lists `GC`; both are
what `sigil_save_layout_subdirs()` names.

`{gc_region}` is Dolphin's GameCube region folder from the region letter
ending the game code: `E` gives `USA`, `J` and `K` give `JAP`, any other
letter `EUR`. Dolphin reads the disc's region field, which follows the
letter on retail discs.

In a GCI folder Dolphin loads any `*.gci` file, whatever its name, and
renames a save it deletes to `.gci.deleted` rather than removing it.

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

## Open items

- Dolphin's Android package for other flavours, and whether its DocumentsProvider exposes the User folder to a sync client, are unconfirmed.
- sigil reads only slot A. Slot B (`MemoryCardB...`, `Card B`) has no layout row.
- A GCI folder the user moved (`GCIFolderAPath`) takes Dolphin's newer region names, `JPN` where the default folder has `JAP` (Config/MainSettings.cpp `GetGCIFolderPath`); sigil knows only the default folder.
- Korean discs use the `JAP` folder: Dolphin has no Korean GameCube region and maps NTSC-K to `JAP`.
