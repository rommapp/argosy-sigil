# Neo Geo Pocket / Color

Status: located
`sigil_save_resolve()` names the flash file Beetle NGP keeps for one game; collect and restore return `SIGIL_ERR_UNSUPPORTED_FORMAT` for it.

## Save layouts

| Layout | Files (role, option) | Shared | Source |
|---|---|---|---|
| `mednafen_ngp` | `{stem}.flash` primary | | `mednafen/ngp/system.c` `system_io_flash_write` |

See [Save units](../save-units.md) for how rows expand.

## Emulator research

Scope is game saves (battery RAM, EEPROM, flash, NVRAM, memory cards, emulated HDD content). Save states are excluded.
Sources were fetched 2026-09-26 from the default branch of each repo. Line numbers refer to that snapshot and will drift.
Items with no source are marked UNVERIFIED.

Conventions used below:

- `<savedir>` is the libretro frontend save directory. RetroArch can also insert `<core name>/` or `<content dir name>/` under it (sort_savefiles_by_* settings). That layer belongs to the frontend, not the core.
- `<base>` is the content filename without extension (for multi-disc, the `.m3u`/`.cue`/`.chd` basename that was loaded).
- `.srm` means the core exposes a buffer via `retro_get_memory_data(RETRO_MEMORY_SAVE_RAM)` and the frontend writes it verbatim as `<savedir>/<base>.srm`. The core never sees a filename. A `.srm` is always raw bytes of the exposed buffer, with no header.
- "Core-managed" means the core writes its own file, and the frontend's `.srm` path does not apply. Sync code has to know the core-specific path.

The cart saves by writing its own flash ROM. NeoPop-derived emulators store only the modified flash blocks.

Flash file format (NeoPop). Header `{u16 valid_flash_id = 0x0053; u16 block_count; u32 total_file_length}` (8 B), then `block_count` x `{u32 start_address (24-bit); u16 data_length; 2 B padding}` (8 B, struct memcpy'd in host byte order, little-endian in practice), each followed by `data_length` bytes. Max 256 blocks.

| Emulator | Files and naming | Format | Scope and options | Per-game extraction / neutral form | Source |
|---|---|---|---|---|---|
| libretro mednafen_ngp (beetle-ngp) | Core-managed `<savedir>/<base>.flash`. There is **no `.srm`**: `retro_get_memory_data` returns only SYSTEM_RAM | NeoPop flash block file, above | Per content | Per-game. The neutral form is the list of `(address, bytes)` flash patches. It is lossless, and can be applied to the ROM to produce a patched ROM when needed | https://github.com/libretro/beetle-ngp-libretro `libretro.c` L778-790 (no SAVE_RAM), L796-812 (`MDFN_MakeFName` → `<savedir>/<base>.<ext>`). `mednafen/ngp/flash.c` |
| Mednafen standalone (ngp) | `<path_sav>/<fbase>.<md5>.flash` | Same format. Written raw (no gzip on this path) | Per game | Byte-identical with beetle-ngp. Converting is a rename | https://github.com/libretro-mirrors/mednafen-git `src/ngp/flash.cpp` L36-64 (structs), L178-199, L255-312 (`make_flash_commit`). `src/ngp/neopop.cpp` L315-340 (`"flash"` ext) |

### Notes for save sync

- The core-managed paths that bypass `.srm` in this set are beetle-ngp `.flash`, handy `.eeprom`, opera `opera/{per_game,shared}/...`, fbneo `fbneo/*.fs|*.hi|*.memcard`, mame2003-plus `mame2003-plus/{nvram,hi,memcard}/`, mame `mame/nvram/...` (and hiscores in the **system** dir), same_cdi `same_cdi/nvram/...`, dosbox_pure `*.pure.zip`, and geargrafx `geargrafx_mb128.sav`. A frontend-agnostic `<base>.srm` rule misses all of them.
- Byte-identical pairs: Mednafen standalone `.sav`/`.flash` equals the beetle `.srm`/`.flash` for PCE, NGP, and WonderSwan single-region carts (gunzip first if gzipped). The fbneo MAME-format memcard equals the MAME `.neo` equals the mame2003-plus `MEMCARD.NNN` (all 2 KiB data bytes). Arcade NVRAM is **not** interchangeable between fbneo, mame2003-plus and MAME (different sizes and layouts, e.g. Neo Geo MVS: 64 KiB vs 8 KiB MSB-first vs 64 KiB per-device).
