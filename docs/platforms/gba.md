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
carts alone and keeps a GBA cart's clock in its save RAM), so on GBA
locate never expects one. RetroArch's own rules
for `.srm` are on [Game Boy](gb.md#retroarch-behaviour). Standalone mGBA
saves beside the ROM unless `savegamePath` is set; pass the folder that
holds the `.sav` as the save root.

## Save formats

The chip's bytes are the same in every emulator, an EEPROM's included:
each keeps its 64-bit blocks in address order, the first bit sent in each
byte's top bit. The file around them is not.

| Emulator | File |
|---|---|
| mGBA libretro (`mgba`, the default row) | The core reports 128 KiB of save RAM, 0xFF past what was read in, until the game addresses the chip, then the chip's size; an EEPROM is 512 bytes until the game addresses past them (`src/platform/libretro/libretro.c` `retro_get_memory_size`; `src/gba/savedata.c` `GBASavedataInitEEPROM`, `_ensureEeprom`). The save RAM is a 128 KiB buffer, and on a clock cart the core writes the [clock](#clock-format) inside it at the chip's size, 8 KiB for an EEPROM, past the size it reports (`savedata.c` `GBASavedataClean`, `GBASavedataRTCWrite`; `libretro.c` `retro_load_game`, `_doDeferredSetup`). For 128 KiB flash the place is past the buffer, and the core keeps no clock. RetroArch 1.22.2's SRAM autosave, every 10 seconds by default on desktop and Android (`config.def.h` `DEFAULT_AUTOSAVE_INTERVAL`), writes the length the core reported when the game loaded (`save.c` `autosave_init`), so during play the `.srm` is 128 KiB with the clock in it. Its save on quit asks the core again (`save.c` `content_save_ram_file`) and writes the chip's size. Measured with Boktai: 131,072 bytes with the clock at 8 KiB after the game saved, 8,192 bytes after quitting. A frontend that asks the core at each save writes the chip's size. |
| VBA-M libretro (`vbam`, the default row) | The chip's size, from its override table or the ROM's ids. An EEPROM is 512 bytes, or the override table's size, until the game addresses 8 KiB (`src/libretro/libretro.cpp` `load_image_preferences`; `src/core/gba/gbaEeprom.cpp` `eepromWrite`). Read, not run. |
| gpSP (`gpsp`) | 128 KiB for every chip: the chip's bytes, then 0xFF (`libretro/libretro.c` `retro_get_memory_size`, `retro_load_game`). |
| VBA Next (`vba_next`) | 136 KiB for every chip: SRAM or flash at 0, an EEPROM's bytes at 128 KiB (`libretro/libretro.c` `libretro_save_buf`; `src/memory.c` `eepromData`). It never reads an EEPROM from 0, so another emulator's EEPROM file loads as no save: `adjust_save_ram`, which could move it, runs only in `retro_init`, before the save is loaded. |
| mGBA standalone (`mgba_standalone`) | `.sav`: the chip's size, then on a clock cart a 16-byte footer ([clock format](#clock-format)). |

Sources read: libretro/mgba at `e31759b2` (the libretro core), mGBA's 0.10.5 tag (standalone), visualboyadvance-m v2.2.3, libretro/gpsp at `5819380`, libretro/vba-next at `788192f`, RetroArch v1.22.2.

## Sync

The unit is the save chip's bytes at the chip's size: 512 bytes or 8 KiB
for an EEPROM, 32 KiB for SRAM, 64 or 128 KiB for flash. A cart without a
clock travels as one file, `{stem}.srm`. A clock cart (`SIGIL_FEATURE_RTC`,
or a save with mGBA's clock) travels as a zip of `save.sram` and, when
the emulator kept the clock, `clock.rtc`: mGBA's 16 bytes as mGBA wrote them.
The identity covers the chip only, as on [Game Boy](gb.md#sync), so a
clock cart's identity is the same with or without `clock.rtc`.

Collect takes the chip out of any emulator's file. mGBA's clock comes
off: standalone mGBA's footer ends a file 16 bytes past a multiple of 512,
and in a 128 KiB file, when the ROM names the chip and the clock, the 16
bytes where mGBA's libretro core keeps it (the chip's size, 8 KiB for an
EEPROM) are the clock when they are set and nothing after them is. The
time in them is BCD, so a written clock is never all 0xFF
(`src/gba/cart/gpio.c` `_rtcUpdateClock`). VBA Next's EEPROM comes from
128 KiB, and padding comes off down to the chip the ROM names. Collect
never drops a byte that isn't 0xFF: a file holding data
past the named chip keeps the next size that holds it. An EEPROM file that
is exactly 512 bytes or 8 KiB keeps its size. From a padded file an EEPROM
is 512 bytes unless a byte past them isn't 0xFF; every emulator here reads
a 512-byte file for either size.

Without the ROM's facts, a file of a chip's own size is that chip, a
512-byte or 8 KiB file is an EEPROM, VBA Next's EEPROM is found by where it
sits, and a padded file (gpSP's, mGBA's in RetroArch, VBA Next's SRAM or
flash) keeps 128 KiB, since nothing in it says which chip it holds. A
clock inside a 128 KiB file stays in the save unless the ROM names both
the chip and the clock: nothing else says those 16 bytes are a clock.

Restore writes what each emulator writes itself, picked by the layout
row: gpSP's 128 KiB, VBA Next's 136 KiB with an EEPROM at 128 KiB, and for
standalone mGBA the save at its own size, then on a clock cart the unit's
clock, else the clock already in the `.sav`, where mGBA reads it: at the
chip's size, 8 KiB for an EEPROM. Any other core gets the chip as it is;
over a file already 128 KiB long, as mGBA's libretro core leaves one in
RetroArch, it gets the chip, 0xFF to 128 KiB and, when the ROM names the
chip and the clock, the unit's clock (else the one already there) where
the core keeps it. No libretro GBA core keeps the clock in a file of its
own, so restore writes no `.rtc`. Collecting an emulator's file and
restoring it for the same emulator gives the file back byte for byte.

Standalone mGBA takes the EEPROM games its override table names as 8 KiB
and grows a shorter `.sav` to 8 KiB with zeros where the cart would read
0xFF. That is mGBA's own handling of a 512-byte file, so a 512-byte save
restored there gets it as it would from any other source.

Restore also reads any of the files above, and a zip of one with or
without `clock.rtc`, as clients uploaded them before sigil. A `clock.rtc`
that isn't 16 bytes isn't mGBA's clock: it is left out, and the save
restores without it.

## Clock format

mGBA keeps a GBA cart's clock with the save, standalone and as a libretro
core alike: 16 bytes (`GBASavedataRTCBuffer`, `src/gba/savedata.c`), the
clock's registers as the game last read them, BCD year, month, day,
weekday, hour, minute and second, then the control byte, then the time it
read them at as a u64 LE Unix time. It writes them at the chip's size
(`GBASavedataSize() & ~0xFF`, `GBASavedataRTCWrite`): standalone mGBA
after the chip in the `.sav`, the libretro core inside its 128 KiB save
RAM. mGBA turns the clock on only for the carts its override table lists
with one (`src/gba/overrides.c` `GBAOverrideApply`), and each of those
rows with an EEPROM forces the 64 Kbit chip, so an EEPROM's clock sits at
8 KiB.

Standalone mGBA reads the clock back when the game starts and runs it on
from there by the computer's clock. The libretro core reads it only when
the game is reset (`libretro.c` `retro_reset`): at the first start it
resets the core before it hands the core the save (`_doDeferredSetup`),
so `GBASavedataRTCRead` finds no save, and the clock runs from the
computer's clock. Measured: Boktai, started with a clock set 4,620
seconds behind the time it was read at, wrote its next clock 0 seconds
behind, so the core hadn't read it. The other libretro cores run the
cart's clock from the computer's clock each session and keep nothing.

## Open items

- Without the ROM's facts a padded save keeps 128 KiB, so gpSP's or VBA Next's save of a game whose ROM sigil hasn't read gets another identity than mGBA's file of the same save.
- An EEPROM save keeps the size its emulator wrote, so the same save from a 512-byte file and from an exact 8 KiB file (mGBA and VBA-M write 512 bytes until the game addresses past them, then 8 KiB) are two units with two identities.
- A clock cart whose ROM carries no `SIIRTC_V` id is only known as one when its save ends in standalone mGBA's footer.
- Without the ROM naming the chip and the clock, the clock inside a 128 KiB file from mGBA's libretro core stays in the save: the unit keeps 128 KiB, or the next chip size past the named chip, so it gets another identity than the same save from another emulator, and restored for VBA Next an EEPROM in it lands at 0, where VBA Next doesn't read it.
- Restored for mGBA's libretro core into a file of the chip's size, or where there is no file, the clock is left out: the core reports the chip's size, so a frontend that asks it, and RetroArch's save on quit, keep no clock either.
- A 128 KiB unit with no ROM facts, restored for standalone mGBA with a clock, gets the clock at 128 KiB, where mGBA reads it only for 128 KiB flash.
- mGBA's 64 KiB SRAM (`SRAM512`, homebrew) travels like 64 KiB flash; no Nintendo id names it.
- Pizza Boy A/C: closed source, format and RTC handling unknown.
- Whether the Mesen2 GBA `.rtc` values are BCD.
