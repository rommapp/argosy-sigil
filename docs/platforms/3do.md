# 3DO

Status: located
`sigil_save_resolve()` names the NVRAM image Opera keeps for one game; collect and restore return `SIGIL_ERR_UNSUPPORTED_FORMAT` for it.

## Save layouts

| Layout | Files (role, option) | Shared | Source |
|---|---|---|---|
| `opera` | `opera/per_game/{stem}.{nvram_version}.srm` primary when `opera_nvram_storage` = `per game` (default) | `opera/shared/nvram.{nvram_version}.srm` when `shared` | `opera_lr_nvram.c`; subdirs `opera/per_game`, `opera/shared` |

`{nvram_version}` is `opera_nvram_version` (default `0`). See [Save units](../save-units.md) for how rows expand.

## Emulator research

Scope is game saves (battery RAM, EEPROM, flash, NVRAM, memory cards, emulated HDD content). Save states are excluded.
Sources were fetched 2026-09-26 from the default branch of each repo. Line numbers refer to that snapshot and will drift.
Items with no source are marked UNVERIFIED.

Conventions used below:

- `<savedir>` is the libretro frontend save directory. RetroArch can also insert `<core name>/` or `<content dir name>/` under it (sort_savefiles_by_* settings). That layer belongs to the frontend, not the core.
- `<base>` is the content filename without extension (for multi-disc, the `.m3u`/`.cue`/`.chd` basename that was loaded).
- `.srm` means the core exposes a buffer via `retro_get_memory_data(RETRO_MEMORY_SAVE_RAM)` and the frontend writes it verbatim as `<savedir>/<base>.srm`. The core never sees a filename. A `.srm` is always raw bytes of the exposed buffer, with no header.
- "Core-managed" means the core writes its own file, and the frontend's `.srm` path does not apply. Sync code has to know the core-specific path.

The 3DO has 32 KiB of battery NVRAM formatted as an Opera "linked memory" filesystem. Every game writes named files into it, and on hardware all games share it. It fills up often (NFS "NVRAM Full" reports).

Format. A DiscLabel with `dl_VolumeStructureVersion = VOLUME_STRUCTURE_LINKED_MEM`, block size 1, block count 32768, commentary "opera formatted", then a linked list of `LinkedMemBlock`s (flink/blink offsets, big-endian) holding files. Source: https://github.com/libretro/opera-libretro `libopera/opera_nvram.c` L22-81, `libopera/opera_mem.h` L36 (`NVRAM_SIZE = 32 KiB`).

| Emulator | Files and naming | Format | Scope and options | Per-game extraction / neutral form | Source |
|---|---|---|---|---|---|
| libretro opera | Core-managed. **No `.srm`**: SAVE_RAM returns NULL/0. Per game: `<savedir>/opera/per_game/<base>.<ver>.srm`. Shared: `<savedir>/opera/shared/nvram.<ver>.srm`. `<ver>` = `opera_nvram_version` 0-9 (default 0). Written via a `.tmp` file then renamed. Legacy fallbacks on load: per game `<savedir>/<base>.srm` then `<systemdir>/<base>.srm`; shared `<savedir>/3DO.nvram` then `<systemdir>/3DO.nvram`. If there is no save dir, `<systemdir>/opera/...` is used | Raw 32768 B Opera linked-mem filesystem | Option `opera_nvram_storage` = `per game` (default) / `shared`. `opera_nvram_version` selects independent NVRAM "slots" | Per-game mode still holds a full filesystem, but normally only that game's files. Shared mode is a container. Extracting one game means parsing the linked-mem FS and filtering by filename, and no filename-to-title mapping exists. It is lossless in principle. Existing tooling is UNVERIFIED (trapexit's 3dt handles disc images; NVRAM support not confirmed). Recommendation: sync the whole 32 KiB image per game in per-game mode, and treat shared mode as a single opaque container | `opera_lr_nvram.c` (`OLD_NVRAM_FILENAME "3DO.nvram"`, `opera_lr_nvram_save_pergame`/`_shared`, load order). `libretro.c` L430-505, L586-615. `libretro_core_options.c` L271-304 |

### Notes for save sync

- The core-managed paths that bypass `.srm` in this set are beetle-ngp `.flash`, handy `.eeprom`, opera `opera/{per_game,shared}/...`, fbneo `fbneo/*.fs|*.hi|*.memcard`, mame2003-plus `mame2003-plus/{nvram,hi,memcard}/`, mame `mame/nvram/...` (and hiscores in the **system** dir), same_cdi `same_cdi/nvram/...`, dosbox_pure `*.pure.zip`, and geargrafx `geargrafx_mb128.sav`. A frontend-agnostic `<base>.srm` rule misses all of them.
- Shared containers (one file, many games) are the geargrafx MB128, the fbneo `shared.memcard`, the mame2003-plus `MEMCARD.NNN`, MAME's `.neo` card and `nvram/neocd/saveram`, opera shared NVRAM, same_cdi shared / MAME `cdimono1` NVRAM, the xemu HDD image, and the Jaguar Memory Track on hardware. Of these, lossless per-game split/inject is well-defined for the PCE BRAM (HUBM entries), the Neo Geo cards/NeoCD (NGH-tagged directory) and the xemu HDD (FATX `UDATA\<TitleID>`). It is defined but untooled for 3DO (Opera linked-mem FS), and unknown for CD-i NVR, MB128 and Memory Track (sync those whole).
- Emulators that make a hardware-shared store per-game: every PCE core (BRAM per content), fbneo NeoCD, virtualjaguar Memory Track (inside the per-disc `.srm`), opera (default per game), and same_cdi (default per game). Importing a real-hardware dump into any of them means either duplicating the whole container per game or splitting it by the formats above.
