# Game Boy Advance

Status: synced

## Identification

| Slug | Platform | Inputs | `title_id` example | `usage` | Status |
|---|---|---|---|---|---|
| `gba` | Game Boy Advance | `.gba` | none; sets `features` | file-prefix | |

A GBA cart carries a game code (`BPEE`), but emulators name the save
after the content file, so `title_id` and `save_id` stay empty, as on
[Game Boy](gb.md). Sigil checks the header and reports what the cart holds
in `features`. Byte `0xB2` has to be `0x96` and the complement check over
`0xA0..0xBC` has to match byte `0xBD`; the BIOS refuses a cart that fails
it, so a mismatch means the bytes are not a GBA header.

### Features

The save chip comes from the id Nintendo's save library leaves in the
ROM, the ids VBA-M's libretro core and gpSP search for to pick a chip.
Each is followed by a three-digit version (`EEPROM_V124`). Reading them
reads the whole ROM.

| Feature | The ROM carries | Chip |
|---|---|---|
| `SIGIL_FEATURE_GBA_SRAM` | `SRAM_Vnnn` or `SRAM_F_Vnnn` | 32 KiB SRAM |
| `SIGIL_FEATURE_GBA_FLASH` | `FLASH_Vnnn` or `FLASH512_Vnnn` | 64 KiB flash |
| `SIGIL_FEATURE_GBA_FLASH1M` | `FLASH1M_Vnnn` | 128 KiB flash |
| `SIGIL_FEATURE_GBA_EEPROM` | `EEPROM_Vnnn` | 4 Kbit (512 bytes) or 64 Kbit (8 KiB) EEPROM |
| `SIGIL_FEATURE_RTC` | `SIIRTC_Vnnn` | a real-time clock |

At most one chip is set: a ROM that names two kinds sets none, and its
saves are sized from the file alone. An EEPROM's size isn't in the ROM.
The game picks it in code, and emulators tell the two apart once the game
addresses the chip.

## Save layouts

Rows from the [layout table](../save-units.md). The default row applies to any cart core with no row of its own.

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| default | `{stem}.srm` primary | | emulator source, emulator run |
| `vba_next`, `gpsp` | `{stem}.srm` primary | | emulator source, emulator run |
| `mgba_standalone` | `{stem}.sav` primary: the save, then the clock on a clock cart | | emulator source, emulator run |

The default row also names `{stem}.rtc`, but no libretro GBA core exposes
a clock region (mGBA's libretro core answers `RETRO_MEMORY_RTC` for GB
carts alone), so on GBA locate never expects one. RetroArch's own rules
for `.srm` are on [Game Boy](gb.md#retroarch-behaviour). Standalone mGBA
saves beside the ROM unless `savegamePath` is set; pass the folder that
holds the `.sav` as the save root.

## Save formats

The chip's bytes are the same in every emulator, an EEPROM's included:
each keeps its 64-bit blocks in address order, the first bit sent in each
byte's top bit. The file around them is not.

| Emulator | File |
|---|---|
| mGBA libretro (`mgba`, the default row) | The core reports 128 KiB of save RAM, 0xFF past what was read in, until the game addresses the chip, then the chip's size; an EEPROM is 512 bytes until the game addresses past them. RetroArch 1.22.2's SRAM autosave keeps the length the core reported when the game loaded (`save.c`, `autosave_new`), so the `.srm` came out 128 KiB there. A frontend that asks the core at each save writes the chip's size. |
| VBA-M libretro (`vbam`, the default row) | The chip's size, from its override table or the ROM's ids. An EEPROM starts at 8 KiB and becomes 512 bytes once the game addresses it as one. |
| gpSP (`gpsp`) | 128 KiB for every chip: the chip's bytes, then 0xFF. |
| VBA Next (`vba_next`) | 136 KiB for every chip: SRAM or flash at 0, an EEPROM's bytes at 128 KiB. It never reads an EEPROM from 0, so another emulator's EEPROM file loads as no save. |
| mGBA standalone (`mgba_standalone`) | `.sav`: the chip's size, then on a clock cart a 16-byte footer ([clock format](#clock-format)). |

## Sync

The unit is the save chip's bytes at the chip's size: 512 bytes or 8 KiB
for an EEPROM, 32 KiB for SRAM, 64 or 128 KiB for flash. A cart without a
clock travels as one file, `{stem}.srm`. A clock cart (`SIGIL_FEATURE_RTC`,
or a save with mGBA's footer) travels as a zip of `save.sram` and, when
the emulator kept the clock, `clock.rtc`: mGBA's footer as mGBA wrote it.
The identity covers the chip only, as on [Game Boy](gb.md#sync), so a
clock cart's identity is the same with or without `clock.rtc`.

Collect takes the chip out of any emulator's file. mGBA's footer comes
off (a file 16 bytes past a multiple of 512 ends in one), VBA Next's
EEPROM comes from 128 KiB, and padding comes off down to the chip the ROM
names. Collect never drops a byte that isn't 0xFF: a file holding data
past the named chip keeps the next size that holds it. An EEPROM file that
is exactly 512 bytes or 8 KiB keeps its size. From a padded file an EEPROM
is 512 bytes unless a byte past them isn't 0xFF; every emulator here reads
a 512-byte file for either size.

Without the ROM's facts, a file of a chip's own size is that chip, a
512-byte or 8 KiB file is an EEPROM, VBA Next's EEPROM is found by where it
sits, and a padded file (gpSP's, mGBA's in RetroArch, VBA Next's SRAM or
flash) keeps 128 KiB, since nothing in it says which chip it holds.

Restore writes what each emulator writes itself, picked by the layout
row: gpSP's 128 KiB, VBA Next's 136 KiB with an EEPROM at 128 KiB, and for
standalone mGBA the save at its own size, then on a clock cart the unit's
clock, else the clock already in the `.sav`. Any other core gets the chip
as it is, or 0xFF to 128 KiB over a file already that long, as mGBA's
libretro core leaves one in RetroArch. No libretro GBA core keeps the
clock, so restore writes no `.rtc`. Collecting an emulator's file and
restoring it for the same emulator gives the file back byte for byte.

Standalone mGBA takes the EEPROM games its override table names as 8 KiB
and grows a shorter `.sav` to 8 KiB with zeros where the cart would read
0xFF. That is mGBA's own handling of a 512-byte file, so a 512-byte save
restored there gets it as it would from any other source.

Restore also reads any of the files above, and a zip of one with or
without `clock.rtc`, as clients uploaded them before sigil.

## Clock format

Standalone mGBA is the one emulator that keeps a GBA cart's clock with the
save. Its footer (`GBASavedataRTCBuffer`, mGBA's `gba/savedata.c`) is the
clock's registers as the game last read them, BCD year, month, day,
weekday, hour, minute and second, then the control byte, then the time it
read them at as a u64 LE Unix time. mGBA runs the clock on from there by
the computer's clock. The libretro cores run the cart's clock from the
computer's clock each session and keep nothing.

## Open items

- Without the ROM's facts a padded save keeps 128 KiB, so gpSP's or VBA Next's save of a game whose ROM sigil hasn't read gets another identity than mGBA's file of the same save.
- An EEPROM save keeps the size its emulator wrote, so the same save from a 512-byte file and from an exact 8 KiB file (VBA-M starts every EEPROM at 8 KiB) are two units with two identities.
- A clock cart whose ROM carries no `SIIRTC_V` id is only known as one when its save has mGBA's footer.
- mGBA's 64 KiB SRAM (`SRAM512`, homebrew) travels like 64 KiB flash; no Nintendo id names it.
- Pizza Boy A/C: closed source, format and RTC handling unknown.
- Whether the Mesen2 GBA `.rtc` values are BCD.
