# Arcade (FBNeo, MAME 2003-Plus, current MAME)

Status: located
`sigil_save_resolve()` names the files FBNeo and MAME 2003-Plus keep for one romset; collect and restore return `SIGIL_ERR_UNSUPPORTED_FORMAT` for them.

## Save layouts

| Layout | Files (role, option) | Shared | Source |
|---|---|---|---|
| `fbneo` | `fbneo/{romset}.fs` primary; `fbneo/{romset}.nv` sidecar; `fbneo/{romset}.memcard` sidecar when `fbneo-memcard-mode` = `per-game` | `fbneo/shared.memcard` when `shared` | `retro_common.cpp`, `eeprom.cpp`; subdir `fbneo`; option values are the English table of a localized option |
| `mame2003_plus` | `mame2003-plus/nvram/{romset}.nv` primary and `mame2003-plus/hi/{romset}.hi` sidecar when `mame2003-plus_core_save_subfolder` = `enabled` (default); `nvram/{romset}.nv` and `hi/{romset}.hi` when `disabled` | | `fileio.c`; subdirs `mame2003-plus/nvram`, `mame2003-plus/hi`, `nvram`, `hi` |

`{romset}` (same as the stem). See [Save units](../save-units.md) for how rows expand.

Neo Geo carts, cards and the Neo Geo CD are on [Neo Geo](neogeo.md).

## Emulator research

Scope is game saves (battery RAM, EEPROM, flash, NVRAM, memory cards, emulated HDD content). Save states are excluded.
Sources were fetched 2026-09-26 from the default branch of each repo. Line numbers refer to that snapshot and will drift.
Items with no source are marked UNVERIFIED.

Conventions used below:

- `<savedir>` is the libretro frontend save directory. RetroArch can also insert `<core name>/` or `<content dir name>/` under it (sort_savefiles_by_* settings). That layer belongs to the frontend, not the core.
- `<base>` is the content filename without extension (for multi-disc, the `.m3u`/`.cue`/`.chd` basename that was loaded).
- `.srm` means the core exposes a buffer via `retro_get_memory_data(RETRO_MEMORY_SAVE_RAM)` and the frontend writes it verbatim as `<savedir>/<base>.srm`. The core never sees a filename. A `.srm` is always raw bytes of the exposed buffer, with no header.
- "Core-managed" means the core writes its own file, and the frontend's `.srm` path does not apply. Sync code has to know the core-specific path.

Arcade "saves" are NVRAM/EEPROM contents (settings, bookkeeping, sometimes progress), hiscore.dat memory dumps, and Neo Geo memory cards ([Neo Geo](neogeo.md)). All are keyed by romset short name, so they are per-game by construction. Formats are driver-specific and not portable between emulators except where noted.

| Emulator | Files and naming | Format | Scope and options | Per-game extraction / neutral form | Source |
|---|---|---|---|---|---|
| libretro fbneo (all drivers) | `<savedir>/fbneo/<driver>.fs` (NVRAM/EEPROM). `<savedir>/fbneo/<driver>.hi` (hiscores). Neo Geo cards: see [Neo Geo](neogeo.md) | `.fs`: headerless concatenation of the driver's `ACB_NVRAM` areas in scan order (legacy `FB1 FS1 ` compressed files are auto-converted). `.hi`: raw memory ranges from hiscore.dat | Per driver | Already per-game. Neutral form is the emulator-native blob (layout is driver-defined, not portable to MAME) | `libretro.cpp` L1923-1926, L2194-2206. `src/burner/nvram.cpp`. `src/burn/hiscore.cpp` L425-430 |
| libretro mame2003_plus | `<savedir>/mame2003-plus/nvram/<game>.nv`, `<savedir>/mame2003-plus/hi/<game>.hi`, `<savedir>/mame2003-plus/memcard/MEMCARD.<NNN>`. Setting `mame2003-plus_core_save_subfolder`=disabled drops the `mame2003-plus/` level. The hiscore DB is under the system path | `.nv`: whatever the driver's `NVRAM_HANDLER` writes, one file. `.hi`: raw hiscore ranges. `.mem`: 2 KiB Neo Geo card | Per game, except the memcard (shared, numbered). `mame2003-plus_autosave_hiscore` = default/recursively/disabled | Per-game. MAME 0.78-era layout; not compatible with current MAME's per-device files | https://github.com/libretro/mame2003-plus-libretro `src/fileio.c` L110-140, L184-243, L825-886. `src/mame.c` L488, L521. `src/hiscore.c` L166, L203. `src/mame2003/core_options.c` L656-685. `src/mame2003/mame2003.h` L62 |
| MAME standalone | `<nvram_directory>/<system>[_<bios>]/<device_tag>` (one raw file per NVRAM-capable device: `saveram`, `eeprom`, `nvram`, timekeepers etc.; `:` in tags becomes `_`). Devices under a mounted image add `/<software basename>/`. Hiscores: `<homepath>/hiscore/<romname>.hi` or `<romname>_<softlist>.hi` (hiscore plugin, homepath default `.`) | Raw per device. Zero-length files are deleted on save | Per system, or per software when the device belongs to an image. The hiscore plugin saves on update, or only on exit (plugin setting) | Per-game. The neutral form is the directory `nvram/<system>/` as a unit, keeping every device file | https://github.com/mamedev/mame `src/emu/machine.cpp` L1189-1271. `src/emu/emuopts.cpp` L60. `plugins/hiscore/init.lua` L26-28, L210-216 |
| libretro mame (current) | `<savedir>/mame/nvram/<system>/<tag>`. `cfg`, `diff`, `input`, `states`, `snaps` also go under `<savedir>/mame/`. **homepath = `<systemdir>/mame/`**, so hiscore files land in `<systemdir>/mame/hiscore/<romname>.hi`, not the save dir | Same as standalone | Same. Option `mame_paths_enable` (read MAME paths from ini) skips this mapping | Same. Sync code must also watch the system dir for `.hi` | https://github.com/libretro/mame `src/osd/libretro/libretro-internal/retro_init.cpp` L108-150 (`dir_name`/`opt_name`/`opt_type`), L555-580. Whether the hiscore plugin is enabled by default in the core: UNVERIFIED |

MAME's `diff/` directory holds CHD write-diffs (for example arcade hard-disk games). It is not a normal save. Include it only if full hard-disk-game persistence is wanted.

### Notes for save sync

- The core-managed paths that bypass `.srm` in this set are beetle-ngp `.flash`, handy `.eeprom`, opera `opera/{per_game,shared}/...`, fbneo `fbneo/*.fs|*.hi|*.memcard`, mame2003-plus `mame2003-plus/{nvram,hi,memcard}/`, mame `mame/nvram/...` (and hiscores in the **system** dir), same_cdi `same_cdi/nvram/...`, dosbox_pure `*.pure.zip`, and geargrafx `geargrafx_mb128.sav`. A frontend-agnostic `<base>.srm` rule misses all of them.
- Shared containers (one file, many games) are the geargrafx MB128, the fbneo `shared.memcard`, the mame2003-plus `MEMCARD.NNN`, MAME's `.neo` card and `nvram/neocd/saveram`, opera shared NVRAM, same_cdi shared / MAME `cdimono1` NVRAM, the xemu HDD image, and the Jaguar Memory Track on hardware. Of these, lossless per-game split/inject is well-defined for the PCE BRAM (HUBM entries), the Neo Geo cards/NeoCD (NGH-tagged directory) and the xemu HDD (FATX `UDATA\<TitleID>`). It is defined but untooled for 3DO (Opera linked-mem FS), and unknown for CD-i NVR, MB128 and Memory Track (sync those whole).
- Byte-identical pairs: Mednafen standalone `.sav`/`.flash` equals the beetle `.srm`/`.flash` for PCE, NGP, and WonderSwan single-region carts (gunzip first if gzipped). The fbneo MAME-format memcard equals the MAME `.neo` equals the mame2003-plus `MEMCARD.NNN` (all 2 KiB data bytes). Arcade NVRAM is **not** interchangeable between fbneo, mame2003-plus and MAME (different sizes and layouts, e.g. Neo Geo MVS: 64 KiB vs 8 KiB MSB-first vs 64 KiB per-device).

## Open items

- No row for current MAME (libretro `mame` or standalone).
- The `fbneo` row has no `.hi` member and the `mame2003_plus` row has no `memcard/MEMCARD.<NNN>` member, though the research above lists both.
