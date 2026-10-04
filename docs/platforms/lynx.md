# Atari Lynx

Status: located
`sigil_save_resolve()` names the EEPROM file Handy keeps for one game; collect and restore return `SIGIL_ERR_UNSUPPORTED_FORMAT` for it.

## Save layouts

| Layout | Files (role, option) | Shared | Source |
|---|---|---|---|
| `handy` | `{stem}.eeprom` primary | | `libretro.cpp`, written at `retro_deinit` |

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

Most commercial Lynx carts do not save. A few carts and many homebrew titles use a 93Cxx serial EEPROM, and the `.lnx` header declares its type.

| Emulator | Files and naming | Format | Scope and options | Per-game extraction / neutral form | Source |
|---|---|---|---|---|---|
| libretro handy | Core-managed `<savedir>/<base>.eeprom`. **No `.srm`**: `retro_get_memory_data` exposes only SYSTEM_RAM | Raw EEPROM image. Size = (ADDR_MASK+1) bytes if type bit 0x80 (8-bit org), else x2 (16-bit org). 93C46 = 128 B, 93C56/66/76/86 larger, up to 2048 B. The type comes from the LNX header or the internal DB (`lynxDB`). The file is written only when the cart declares an EEPROM | Per content | Per-game, raw | https://github.com/libretro/libretro-handy `libretro/libretro.cpp` L1148-1230 (path), L1295-1307. `lynx/eeprom.cpp` L45-80 (Load/Save), L129-194 (`SetEEPROMType`, `Size`). `lynx/cart.cpp` L99, L129 |

### Notes for save sync

- The core-managed paths that bypass `.srm` in this set are beetle-ngp `.flash`, handy `.eeprom`, opera `opera/{per_game,shared}/...`, fbneo `fbneo/*.fs|*.hi|*.memcard`, mame2003-plus `mame2003-plus/{nvram,hi,memcard}/`, mame `mame/nvram/...` (and hiscores in the **system** dir), same_cdi `same_cdi/nvram/...`, dosbox_pure `*.pure.zip`, and geargrafx `geargrafx_mb128.sav`. A frontend-agnostic `<base>.srm` rule misses all of them.
