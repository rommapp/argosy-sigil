# WonderSwan / Color

Status: located
sigil has no row of its own for Beetle WonderSwan. Its `<stem>.srm` resolves through the default libretro row, and nothing syncs it.

## Emulator research

Scope is game saves (battery RAM, EEPROM, flash, NVRAM, memory cards, emulated HDD content). Save states are excluded.
Sources were fetched 2026-09-26 from the default branch of each repo. Line numbers refer to that snapshot and will drift.
Items with no source are marked UNVERIFIED.

Conventions used below:

- `<savedir>` is the libretro frontend save directory. RetroArch can also insert `<core name>/` or `<content dir name>/` under it (sort_savefiles_by_* settings). That layer belongs to the frontend, not the core.
- `<base>` is the content filename without extension (for multi-disc, the `.m3u`/`.cue`/`.chd` basename that was loaded).
- `.srm` means the core exposes a buffer via `retro_get_memory_data(RETRO_MEMORY_SAVE_RAM)` and the frontend writes it verbatim as `<savedir>/<base>.srm`. The core never sees a filename. A `.srm` is always raw bytes of the exposed buffer, with no header.

Carts have either SRAM (8-512 KiB) or a serial EEPROM (128 B / 1 KiB / 2 KiB), selected by ROM footer byte 5. The console's internal EEPROM (owner name/birthday, 1 KiB) belongs to the system, not the game. WonderWitch carts save to flash.

| Emulator | Files and naming | Format | Scope and options | Per-game extraction / neutral form | Source |
|---|---|---|---|---|---|
| libretro mednafen_wswan (beetle-wswan) | `<savedir>/<base>.srm` | Raw. It exposes **either** the cart EEPROM (128/1024/2048 B) **or** SRAM (8K/32K/128K/256K/512K), never both. WonderWitch exposes a WW save block (`WSwan_GetWWSaveBlock`). The internal EEPROM is not persisted | Per content | Per-game, raw | https://github.com/libretro/beetle-wswan-libretro `libretro.c` L597-612 (header[5] size table), L1721-1760 |
| Mednafen standalone (wswan) | `<path_sav>/<fbase>.<md5>.sav`. WonderWitch: `<fbase>.<md5>.flash` (512 KiB, the whole cart ROM image) | Raw **concatenation of EEPROM then SRAM** (`eeprom_size + sram_size`). The reader accepts gzip (`MDFN_AmbigGZOpenHelper`). The internal `iEEPROM` is initialised from settings and not saved | Per game | Converting to or from libretro is lossless when only one of the two regions exists, which is the normal case: the file is identical. If a cart had both, split at `eeprom_size`. WonderWitch formats differ (full 512 KiB flash vs a WW save block), and mapping between them is UNVERIFIED | https://github.com/libretro-mirrors/mednafen-git `src/wswan/memory.cpp` L672-734. `src/wswan/eeprom.cpp` L30-32 |

### Notes for save sync

- Byte-identical pairs: Mednafen standalone `.sav`/`.flash` equals the beetle `.srm`/`.flash` for PCE, NGP, and WonderSwan single-region carts (gunzip first if gzipped). The fbneo MAME-format memcard equals the MAME `.neo` equals the mame2003-plus `MEMCARD.NNN` (all 2 KiB data bytes). Arcade NVRAM is **not** interchangeable between fbneo, mame2003-plus and MAME (different sizes and layouts, e.g. Neo Geo MVS: 64 KiB vs 8 KiB MSB-first vs 64 KiB per-device).
