# Sync

`sigil_collect` gathers one game's saves into the unit that travels to
RomM; `sigil_restore` puts a unit back and reads it back. Sigil keeps what
it must remember between calls in an opaque state blob the caller stores
and passes back. The calls and their fields are on each language's page
([C](quickstart-guides/c.md#sync), [Python](quickstart-guides/python.md#sync),
[Go](quickstart-guides/go.md#sync), [Kotlin](quickstart-guides/kotlin.md#sync)); this page has the rules they share.

## What syncs

| System | Unit | Page |
|---|---|---|
| PS1 | one per-game raw card | [psx](platforms/psx.md#sync) |
| PS2 | one per-game 8 MB `.ps2` card with ECC, from a file or PCSX2 folder card | [ps2](platforms/ps2.md#sync) |
| GameCube | the game's `.gci` files, one or a zip, from a raw card or Dolphin's GCI folder | [gamecube](platforms/gamecube.md#sync) |
| Saturn, Sega CD | `backup.ram`, or a zip with `backup.ram` and `cart.ram` | [saturn](platforms/saturn.md#sync), [segacd](platforms/segacd.md#sync) |
| Dreamcast | `vmu_A1.bin`, or a zip of the VMUs present | [dreamcast](platforms/dreamcast.md#sync) |
| Switch, Wii U, PS Vita, PS3 | a zip of the game's save folders, without the profile id | [switch](platforms/switch.md#sync), [wiiu](platforms/wiiu.md#sync), [psvita](platforms/psvita.md#sync), [ps3](platforms/ps3.md#sync) |
| PSP | a zip of the game's save folders | [psp](platforms/psp.md#sync) |
| Wii | a zip of the title's `data/` folder, under its code | [wii](platforms/wii.md#sync) |
| 3DS | a zip of the title's save folder, its metadata and its extdata | [3ds](platforms/3ds.md#sync) |
| GB, GBC | the cart RAM; on a cart with a clock, a zip of `save.sram` and `clock.rtc` | [gb](platforms/gb.md#sync) |
| N64 | a zip of the save types the game uses (`eeprom`, `pak1` to `pak4`, `sram`, `flash`), whichever emulator wrote them | [n64](platforms/n64.md#sync) |

What restore writes for each layout, and which options change it, is in
[restore targets](platforms/README.md#restore-targets).

Every volume in a unit is raw, whatever form the emulator stores it in;
restore writes each file back in the emulator's form (gzip, byte
expansion), and a file the emulator hasn't created yet in the form and
size its layout row names.

### Units from before sigil

Restore also reads the shapes clients uploaded before they used sigil's
units, and writes the save exactly as it would from sigil's own unit. The
next collect then gives the save in sigil's form; upload that.

| System | Shape restore reads |
|---|---|
| PS1 | a raw card, or a flat zip of cards under any names (Beetle's `{stem}.0.mcr` and `{stem}.1.mcr`) |
| PS2 | a zip of PCSX2 save folders, `<folder>/<file>`, or the same under `<card>.ps2/` |
| GameCube | a raw `.gci`, or a flat zip of `.gci` files; of two files with one identity, the first by name, as Dolphin loads a folder |
| Saturn, Sega CD, Dreamcast | a raw volume file as the emulator stored it (gzip and byte expansion included), or a flat zip of them under their on-disk names, each matched to its volume by the layout's file names |
| Wii U | a zip naming the account folder by its id (`<save_id>/user/80000001/`) |
| Wii | the title folder zipped from its code (`<code>/data/...`, `<code>/content/...`); `content/` is left out |
| 3DS | a zip rooted at `data/` and `extdata/` |
| GB, GBC | a raw `.srm`, a `.sav` with its clock appended, or a zip of an emulator's `.srm` and `.rtc` |
| N64 | a libretro `.srm`, a lone `.eep`, or a zip of an emulator's files (`.srm`, `.eep`, `.sra`, `.fla`, `.mpk`, `_Cont_<n>.mpk`) |
| Any | any of the above, or sigil's own unit, ending in the hardcore marker Argosy appended (`{"h":true,...}`, its length as a little-endian u32, then `ARGOSY` 01 00). Restore leaves the marker out of the saves, companion units included, and sets `hardcore_marker`; `content_hash` still covers it |
| Switch | one folder rooted at `<title>/`: the device save for the titles that keep theirs on the device (Animal Crossing: New Horizons, Nintendo Switch Sports, 1-2-Switch, Ring Fit Adventure, the four Labo kits, Go Vacation), the account save for any other |

## Whose saves

A game's saves are the ones carrying one of its ids, on its own card and on
the shared cards beside it. Restore puts each save back on the card it was
on, or on the game's own card when it is new; it never touches another
game's save, and refuses with `SIGIL_ERR_NO_SPACE` before writing when the
saves don't fit. A unit holding none of the game's saves is refused with
`SIGIL_ERR_NOT_FOUND`. Saves of other games inside a unit are ignored.

Saturn, Sega CD and Dreamcast saves carry no game id. Every save on a
per-game volume (Beetle Saturn's `<stem>.srm` and `.bcr`, genesis_plus_gx
with `system_bram` = `per game`, flycast's per-game VMUs) is the game's. On
a shared volume (genesis_plus_gx's default `scd_U.brm`, Beetle's shared
volumes, Yaba Sanshiro's `backup.bin`, flycast's `vmu_save_A1.bin`) a save
belongs to the game the user claimed it for, else to the game the state
learned it belongs to, else, in managed mode, to the game the volume was
last swapped in for, else to the game the save-name table gives it by one
of the ids in `title_id` or `game_ids` (`src/save_names.c`; Saturn and
Sega CD product codes as the disc header spells them, Dreamcast product
numbers), else to the companion the table gives it by one of that
companion's `game_ids`. The rest are unclaimed saves; see below.

### Unclaimed saves

Collect returns the saves on a shared volume that no rule gives a game
in `holding`, a zip of the volumes they're on, and their names in
`unowned`. They're real saves of a game sigil couldn't identify. Call
them unclaimed saves to the user.

- A client claims a save by passing its name, exactly as `unowned` gave
  it, in `claimed` on that game's collect. From then on the save is that
  game's: it travels in that game's unit, and the state remembers the
  owner. A later claim for another game moves it, so a wrong claim is
  undone by claiming the save for the right game.
- `unowned_changed` counts the names at the front of `unowned` that are
  new or rewritten since the last collect saw their volume; it is 0 on a
  volume no collect has seen. To claim what a session wrote, collect once
  before launching the game, then claim the first `unowned_changed`
  names from the collect after it exits. Without the collect before
  launch, those names include whatever any other game wrote since.
- In managed mode, a restore swaps the shared volume for one holding only
  the game's saves, so `holding` becomes the only copy of the unclaimed
  saves. A managed client keeps the latest `holding` per volume, and
  uploads or stores it, until those saves are claimed. In unmanaged mode
  a restore never takes them off the volume, and a client needs only the
  claim.

On layouts with profiles, the profile decides whose saves are taken: see
[save-units.md](save-units.md#profiles).

## Managed and unmanaged

In managed mode the caller launches the game and calls collect after it
closes. Restore swaps each shared volume for one holding only the
game's saves, keeping the file's form (gzip, byte expansion). It refuses
with `SIGIL_ERR_UNCOLLECTED` while the volume holds a save that isn't in
the last holding unit or doesn't match its game's last collect: call
collect for the game that ran last, upload, then restore again. In
unmanaged mode the game runs outside the caller, and restore never swaps.
It replaces only the game's saves,
and only when the volume is as the last collect saw it; a collect that
then finds the old saves back sets `restore_again`.

## Compressed saves

RetroArch's `save_file_compression` writes saves in RZIP: `#RZIPv`, a
version byte (1 for deflate, 2 for Zstandard, which current builds write
where it's compiled in), `#`, the chunk and total sizes, then the
compressed chunks. Collect, restore and `sigil_save_hash` read any save
file, card or volume through it, so a compressed save gives the same unit
and hash as the same save uncompressed. Restore writes uncompressed files,
which RetroArch reads either way. An RZIP file whose chunks don't
decompress is read as it is, and so as damaged where a card or volume
belongs.

## Damaged files

A card or volume file sigil can't read as what its path holds (no card
magic, cut short, an internal volume where a cart goes) is damaged, and so
is a save folder of the game or a companion that sigil can't pack. So is a
card or volume holding a corrupt save that may be the game's or a
companion's (one carrying their id, one the owner rules give them, or one
whose name the card lost), since collect would read that save as deleted. A
managed restore also refuses a shared volume holding any corrupt save,
which the swap would drop; unmanaged keeps it in place. `repair` rebuilds
the structures sigil can rebuild (a `.vmp` signature, a PCSX2 index or
superblock) and changes none of the rest: sigil never writes over saves it
can't read. An empty card file counts as no card, and so does one with
every byte 0xFF, the erased card AetherSX2, NetherSX2 and ARMSX2 create for
a slot never used: collect passes over it and restore may format it.

## Writing and removing

`write` and `remove` only ever get relative paths with no `..` segment;
sigil fails the restore with `SIGIL_ERR_IO` before passing any other.
Where saves are files of their own (Dolphin's GCI folder, PCSX2 folder
cards, layouts with profiles), restore removes the files of the game's
saves the unit lacks through `remove`; it refuses with
`SIGIL_ERR_INVALID_ARG`, writing nothing, when it must remove a file and
`remove` is `NULL`. A path ending in `/` is a directory sigil emptied (a
dropped PCSX2 save folder, which PCSX2 would still show): remove it.

## Companions

A game that reads an earlier title's save, as a sequel reads its prequel's,
lists that title in `companions`. Restore puts each companion's saves on the
game's card, volume or GCI folder beside the game's own. A companion without
a unit keeps the saves it already has there. Collect leaves companion saves
out of the game's unit and hash, and returns each companion's saves as its
own unit, with `changed` against that companion's last sync. Layouts with
profiles take no companions (`SIGIL_ERR_INVALID_ARG`).

## Refusals

Restore refuses, writing nothing, with:

| Code | When | `problem` |
|---|---|---|
| `SIGIL_ERR_CONFLICT` | the saves on disk changed since the last sync and `overwrite_local` is 0 | |
| `SIGIL_ERR_UNCOLLECTED` | a shared volume holds saves no collect has passed on yet | |
| `SIGIL_ERR_NO_SPACE` | the saves don't fit; `blocks_short` says by how much | the save |
| `SIGIL_ERR_REGION` | a GameCube companion's save is from another Dolphin region than the game | the save |
| `SIGIL_ERR_DAMAGED` | a file the saves go in is damaged and `repair` is 0, it isn't a card sigil can read, or it holds a corrupt save of the game or a companion (above) | the file |
| `SIGIL_ERR_AMBIGUOUS` | more than one file could be the emulator's card and the options don't say which: Dolphin raw cards of two sizes with no `MemoryCardSize`. On layouts with profiles, two or more profiles and none picked, for a unit with an account save (ask the user which profile is theirs; `profiles` or `sigil_save_profiles` lists them); or two emulator folders under the root. Collect refuses the same way | the files, the profiles as `id name`, or the folders, one per line |
| `SIGIL_ERR_NO_TARGET` | the unit holds a volume the emulator's settings keep no file for: a `cart.ram` for a core with no cart, a VMU port flycast doesn't keep per game. On layouts with profiles, a member no folder of the game takes, an account save with no profile there, or a folder outside the root | the unit member |
| `SIGIL_ERR_UNSUPPORTED_FORMAT` | layouts with profiles: two unit members go to one file | |
| `SIGIL_ERR_IO` | a unit member's path would leave the save root, or a file the listing holds won't open through `open`. Collect refuses the same way, so a file it can't read never counts as no saves | |
| `SIGIL_ERR_EXISTS` | Dolphin's GCI folder holds other games' files under the name Dolphin gives a new save and each of its ten `0`-inserted forms, so Dolphin would write over one | the save |

Collect refuses with `SIGIL_ERR_DAMAGED`, `SIGIL_ERR_AMBIGUOUS` and
`SIGIL_ERR_IO` in the same way. With each of these the call still sets
`*out`; free it as usual. sigil reports and the
client decides: re-run with `overwrite_local` or `repair` once the user
agreed, or leave the saves as they are.
