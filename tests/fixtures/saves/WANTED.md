# Wanted samples

Cases no sample covers yet. The saves already gathered came from a RomM store and public collections, which don't have these. Most need someone to save a game in the named emulator and add the file as the README describes.

## PS1 (`psx`)

- A card with real saves written by pcsx_rearmed, Beetle PSX or SwanStation. (DuckStation: `thps2-duckstation-mcd`.)
- A card holding live saves from several different games.
- A card with a deleted middle or last block (directory states 0xA2, 0xA3).
- `.vgs`/`.mem` and `.psx` card wrappers, a headerless single save, a PocketStation save.

## PS2 (`ps2`)

- A `.ps2` file card written by PCSX2 with real saves, and a 16 MB or larger card.
- A whole PCSX2 folder card, with a save that has `_pcsx2_meta` files.
- The `_pcsx2_superblock` PCSX2 writes when it formats a folder card, and the one AetherSX2 or ARMSX2 writes. sigil builds its own from mymc's blank card; a real one settles the card_flags byte (`0x2B` in the mymc samples, `0x52` in PCSX2's reference) and what follows the superblock page. The RomM store holds none, since uploads are single save folders.
- A NetherSX2 save.
- The `mymc-mc01` card after mymc imports a save onto it, and after mymc deletes one, each saved as a `.ps2`. sigil's inject and delete would then be checked field by field against mymc's: the `.` entry's parent index, the root's modified time, which time is later, and the value freed clusters get.

## PSP, Vita (`psp`, `psvita`)

- A PSP game-data install folder, to tell apart from a save.
- A Vita save with `sce_sys/`, and a second Vita title.

## Game Boy, GBA (`gb`, `gbc`, `gba`)

- A standalone `.sav` with a 48-byte clock footer (mGBA, VBA-M, SameBoy or Gearboy).
- Clocks from gambatte (8-byte `.rtc`), SameBoy libretro (32-byte `.rtc`), Gearboy libretro, Mesen2 (13-byte `.rtc`) and TGB Dual.
- A vba_next GBA save with its 64 KiB padding.

## Other Nintendo cartridge platforms (`nes`, `snes`, `n64`, `nds`)

- FDS saves from fceumm, Mesen2 and Nestopia.
- Mesen2 MMC5 or Namco163 `.sav` files.
- A SNES `.rtc` from snes9x, bsnes or Mesen2.
- mupen64plus standalone and Project64 per-type files.
- A DeSmuME `.dsv`.

## GameCube, Wii, Wii U, Switch, 3DS (`ngc`, `wii`, `wiiu`, `switch`, `3ds`)

- A raw `.raw` card written by Dolphin.
- A Dolphin GCI folder with `MC_SYSTEM_AREA` or a `.gci.deleted` file.
- A Wii save with `nocopy/` or escaped `__xx__` names.
- A Wii U save with `saveinfo.xml` and a second persistent id.
- A Ryujinx save tree (`0/`, `1/`, `ExtraData0`/`ExtraData1`, `imkvdb.arc`).
- A Switch save in the newer `account/.../0/` layout, and a Citron `.citron_save_size`.
- 3DS extdata, and a 3DS save laid out as `title/<hi>/<lo>/`.

## Sega (`genesis`, `segacd`, `saturn`, `dc`)

- One Genesis game saved by both genesis_plus_gx and picodrive.
- A picodrive Sega CD `.srm`, and a genesis_plus_gx `scd_U.brm` from a known version.- A real 32 KiB Saturn internal volume with several games on it.
- Kronos `.ram` files, and a Saturn cart of 1 MiB or more. A Kronos 4 MiB cart (`-ext4M.ram`) with a save on it would confirm its 1024-byte blocks, which sigil takes from Kronos `bios.c` `GetDeviceStats` and no sample has shown yet.
- A redream VMU.

## Other

- Any real 3DO NVRAM.
- Anything from xemu.

## Every platform

- The emulator version that wrote each sample. Almost every manifest row says `unknown`.
