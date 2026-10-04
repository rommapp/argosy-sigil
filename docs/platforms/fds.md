# Famicom Disk System and NES

Status: located
Sigil names the files Nestopia keeps for a disk. It reads no NES or FDS header, and collect and restore don't cover these games.

## Identification

Sigil extracts no id from an NES cart or an FDS disk. Layout rows use the `fds` platform slug, and callers may pass `famicom_disk_system` for it.

## Save layouts

Row from the [layout table](../save-units.md). An NES cart core with no row of its own uses the default row (`{stem}.srm` primary; `{stem}.rtc` rtc), listed on [gb.md](gb.md#save-layouts).

| Layout | Files (role, option) | Shared | Source |
|---|---|---|---|
| `nestopia` | `{stem}.sav` primary when `nestopia_fds_savefile_format` = `sav_ups` (default); `{stem}.ups` primary when `ups`; `{stem}.ips` primary when `ips` | | nestopia `b9fdc9c4e6` `libretro/libretro.cpp` L461-539 (`SAVE_FDS`), L2446-2447 |

A disk has no save RAM, so RetroArch writes no `{stem}.srm` under
Nestopia; the core writes the patch itself. A `{stem}.srm` beside it is
fceumm's full disk image from before a core switch, which Nestopia never
reads, so the row leaves it out. Builds before the format option
(`3ac52e67c4`, 2025-10-15) always wrote `{stem}.sav` as UPS, the same as
the default now.

## Emulator research

Scope: battery/flash/EEPROM saves and RTC data, not save states. Researched 2026-09-26 against the default branch of each repo (shallow clones). Line numbers refer to those heads and will drift; the function names are the stable anchor.

`{stem}` = content filename without extension. "RA" = RetroArch frontend. RetroArch's own behaviour is in [RetroArch frontend behaviour](gb.md#retroarch-frontend-behaviour-applies-to-every-libretro-core-that-exposes-memory).

### Emulators

| Platform | Emulator | Files and naming | Format | Scope | Options / versions that change files | Lossless conversion | Source |
|---|---|---|---|---|---|---|---|
| NES cart | fceumm (libretro) | `{stem}.srm` | Raw battery PRG-RAM, `SaveGameLen[0]` bytes (usually 8 KiB). Only the first save chunk (`SaveGame[0]`) is exposed. | per-game | none | Byte-identical to Mesen `.sav` and Nestopia `.srm` for single-chunk mappers. MMC5 and Namco163 in Mesen concatenate extra RAM (see Mesen row), so compare sizes. | [libretro.c L4368-4401](https://github.com/libretro/libretro-fceumm/blob/master/src/drivers/libretro/libretro.c#L4368-L4401) |
| FDS | fceumm (libretro) | `{stem}.srm` | The whole modified disk, headerless: `sides x 65500` bytes (the fwNES `FDS\x1a` 16-byte header is dropped). No diff, a full image. | per-game | none | To Mesen: prepend the original 16-byte header if the base ROM had one, then compute IPS against the original file. To Nestopia: UPS/IPS against the original file. Both need the exact original ROM (hash match). | [libretro.c L4373/L4400](https://github.com/libretro/libretro-fceumm/blob/master/src/drivers/libretro/libretro.c#L4373), [fds.c `BYTES_PER_SIDE` L103, `FDSLoad` L640-660](https://github.com/libretro/libretro-fceumm/blob/master/src/fds.c#L103) |
| NES cart | nestopia (libretro) | `{stem}.srm` | Raw SRAM (`sram_size`) | per-game | none | Same bytes as fceumm/Mesen for plain battery carts | [libretro.cpp L2430-2447](https://github.com/libretro/nestopia/blob/master/libretro/libretro.cpp#L2430-L2447) |
| FDS | nestopia (libretro) | Core-written, in the RA save dir: `{stem}.sav` (UPS patch), `{stem}.ups` (UPS) or `{stem}.ips` (IPS) | A patch against the original loaded file, with the header kept if the original had one ([NstFds.cpp `Sides::Save` L685-691](https://github.com/libretro/nestopia/blob/master/source/core/NstFds.cpp#L685-L691)) | per-game | `nestopia_fds_savefile_format` = `sav_ups` (default) / `ups` / `ips`. The `ips` value is labelled "IPS (Mesen)" in the core options. The option arrived in `3ac52e67c4` (2025-10-15); before it the core always wrote `.sav` as UPS. | `ips` mode is intended to be Mesen-compatible (same base: the original file). UPS to IPS: apply the patch to the original, re-diff. | [libretro.cpp L477-535, L1254-1271](https://github.com/libretro/nestopia/blob/master/libretro/libretro.cpp#L477-L535), [libretro_core_options.h L141-143](https://github.com/libretro/nestopia/blob/master/libretro/libretro_core_options.h) |
| NES / FDS | mesen (libretro, legacy Mesen 0.9 core) | `{stem}.srm` (RA, cart RAM). The core's own BatteryManager also writes `{romname}.sav`, `.sav.chr` and FDS `{romname}.ips` into the RA save dir. | Cart raw. FDS = IPS patch. | per-game | none | As Mesen2 | [Libretro/libretro.cpp L1199-1209, L1068](https://github.com/libretro/Mesen/blob/master/Libretro/libretro.cpp#L1199), [Core/FDS.cpp L36-66](https://github.com/libretro/Mesen/blob/master/Core/FDS.cpp#L36-L66), [BaseMapper.cpp L370-379](https://github.com/libretro/Mesen/blob/master/Core/BaseMapper.cpp#L370-L379). UNVERIFIED: whether the core's direct `.sav` write happens under RA as well as the `.srm`. |
| NES / FDS | Mesen2 (standalone) | `Saves/{romname}.sav`. Also `.chr.sav` (battery CHR-RAM), `.eeprom256`/`.eeprom128` (Bandai 24C0x), `.turbofile.sav`, `.battlebox.sav`. FDS: `Saves/{romname}.ips`. | Raw. MMC5 `.sav` = saveRam followed by mapperRam. Namco163 `.sav` includes the internal audio RAM. FDS: IPS of the rebuilt FDS file against the original file (the header is kept if the original had `FDS\x1a`). | per-game (multiple files) | FDS "overwrite original ROM" setting (`OverwriteOriginalRom`) writes into the ROM and produces no `.ips` | Cart `.sav` equals fceumm/nestopia `.srm` for simple mappers. The Bandai EEPROM files have no libretro equivalent in fceumm (UNVERIFIED how fceumm exposes 24C0x). | [BaseMapper.cpp L462-495](https://github.com/SourMesen/Mesen2/blob/master/Core/NES/BaseMapper.cpp#L462-L495), [Fds.cpp L58-100](https://github.com/SourMesen/Mesen2/blob/master/Core/NES/Mappers/FDS/Fds.cpp#L58-L100), [MMC5.h L377-389](https://github.com/SourMesen/Mesen2/blob/master/Core/NES/Mappers/Nintendo/MMC5.h#L377-L389) |

### Conversion notes

- **FDS**: there is no common format. fceumm = full headerless disk image; Mesen = IPS vs the original file; Nestopia = UPS (default) or IPS vs the original file. Converting needs the original ROM on the server (RomM has it), which makes all three convertible without loss: materialize the disk, then re-diff or strip.

## Open items

- Legacy Mesen libretro: whether the core writes `.sav` alongside RA's `.srm`.
