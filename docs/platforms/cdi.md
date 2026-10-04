# Philips CD-i

Status: located
`sigil_save_resolve()` names the NVRAM folder SAME CDi keeps for one disc; collect and restore return `SIGIL_ERR_UNSUPPORTED_FORMAT` for it.

## Save layouts

| Layout | Files (role, option) | Shared | Source |
|---|---|---|---|
| `same_cdi` | `same_cdi/nvram/{stem}/` primary (folder) when `same_cdi_nvram_saves` = `enabled` (default) | | `retro_init.cpp`; subdir `same_cdi/nvram` |

See [Save units](../save-units.md) for how rows expand and how folder members archive.

## Emulator research

Scope is game saves (battery RAM, EEPROM, flash, NVRAM, memory cards, emulated HDD content). Save states are excluded.
Sources were fetched 2026-09-26 from the default branch of each repo. Line numbers refer to that snapshot and will drift.
Items with no source are marked UNVERIFIED.

Conventions used below:

- `<savedir>` is the libretro frontend save directory. RetroArch can also insert `<core name>/` or `<content dir name>/` under it (sort_savefiles_by_* settings). That layer belongs to the frontend, not the core.
- "Core-managed" means the core writes its own file, and the frontend's `.srm` path does not apply. Sync code has to know the core-specific path.

The CD-i Mono-I has an 8 KiB MK48T08 timekeeper NVRAM. The high bytes carry the RTC registers. CD-RTOS keeps an `/nvr` filesystem there that all discs share on hardware.

| Emulator | Files and naming | Format | Scope and options | Per-game extraction / neutral form | Source |
|---|---|---|---|---|---|
| libretro same_cdi | Core-managed. Per game (default): `<savedir>/same_cdi/nvram/<gamename>/cdimono1/mk48t08`. Shared: `<savedir>/same_cdi/nvram/cdimono1/mk48t08`. `<gamename>` is the parsed content name (`MgameName`) | Raw 0x2000 B timekeeper image (includes the clock registers at the top) | Option `same_cdi_nvram_saves` = `enabled` (per game, default) / `disabled` (shared). The forced system is `cdimono1` | Per-game mode is already per-game (a whole NVRAM per disc). Shared mode is a container. Splitting the CD-RTOS NVR filesystem per title is UNVERIFIED and has no known tool. Recommendation: sync the whole 8 KiB file per game | https://github.com/libretro/same_cdi `src/osd/libretro/libretro-internal/retro_init.cpp` L63, L83-86, L96-118, L385-410. `libretro.cpp` L163, L184, L486-495. `libretro_shared.h` L114 (`core[]="same_cdi"`). `src/mame/drivers/cdi.cpp` L93, L323. `src/devices/machine/timekpr.cpp` L162, L443-460 |
| MAME standalone (cdimono1) | `nvram/cdimono1/mk48t08` | Same | Shared across all discs (the CD image does not own the timekeeper) | Container, as above | mamedev/mame `src/mame/philips/cdi.cpp`. `machine.cpp` L1189-1220 |

### Notes for save sync

- The core-managed paths that bypass `.srm` in this set are beetle-ngp `.flash`, handy `.eeprom`, opera `opera/{per_game,shared}/...`, fbneo `fbneo/*.fs|*.hi|*.memcard`, mame2003-plus `mame2003-plus/{nvram,hi,memcard}/`, mame `mame/nvram/...` (and hiscores in the **system** dir), same_cdi `same_cdi/nvram/...`, dosbox_pure `*.pure.zip`, and geargrafx `geargrafx_mb128.sav`. A frontend-agnostic `<base>.srm` rule misses all of them.
- Shared containers (one file, many games) are the geargrafx MB128, the fbneo `shared.memcard`, the mame2003-plus `MEMCARD.NNN`, MAME's `.neo` card and `nvram/neocd/saveram`, opera shared NVRAM, same_cdi shared / MAME `cdimono1` NVRAM, the xemu HDD image, and the Jaguar Memory Track on hardware. Of these, lossless per-game split/inject is well-defined for the PCE BRAM (HUBM entries), the Neo Geo cards/NeoCD (NGH-tagged directory) and the xemu HDD (FATX `UDATA\<TitleID>`). It is defined but untooled for 3DO (Opera linked-mem FS), and unknown for CD-i NVR, MB128 and Memory Track (sync those whole).
- Emulators that make a hardware-shared store per-game: every PCE core (BRAM per content), fbneo NeoCD, virtualjaguar Memory Track (inside the per-disc `.srm`), opera (default per game), and same_cdi (default per game). Importing a real-hardware dump into any of them means either duplicating the whole container per game or splitting it by the formats above.

## Open items

- No row for the shared NVRAM (`same_cdi_nvram_saves` = `disabled`).
