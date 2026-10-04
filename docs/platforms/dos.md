# DOS

Status: located
`sigil_save_resolve()` names the `.pure.zip` DOSBox Pure keeps for one content file; collect and restore return `SIGIL_ERR_UNSUPPORTED_FORMAT` for it.

## Save layouts

| Layout | Files (role, option) | Shared | Source |
|---|---|---|---|
| `dosbox_pure` | `{stem}.pure.zip` primary | | `DBP_GetSaveFile`; one zip per content, travels as a file |

A `.pure.zip` is one raw member and hashes as a zip because the server does the same. See [Save units](../save-units.md) for how rows expand and hash.

## Emulator research

Scope is game saves (battery RAM, EEPROM, flash, NVRAM, memory cards, emulated HDD content). Save states are excluded.
Sources were fetched 2026-09-26 from the default branch of each repo. Line numbers refer to that snapshot and will drift.
Items with no source are marked UNVERIFIED.

Conventions used below:

- `<savedir>` is the libretro frontend save directory. RetroArch can also insert `<core name>/` or `<content dir name>/` under it (sort_savefiles_by_* settings). That layer belongs to the frontend, not the core.
- "Core-managed" means the core writes its own file, and the frontend's `.srm` path does not apply. Sync code has to know the core-specific path.

| Emulator | Files and naming | Format | Scope and options | Per-game extraction / neutral form | Source |
|---|---|---|---|---|---|
| libretro dosbox_pure | Core-managed `<savedir>/<content name>.pure.zip`. Legacy `<savedir>/<content name>.sav` is still read if no `.pure.zip` exists (not in strict mode). A redirect is possible: if the C: drive holds a file `*.SAVENAME`, the save becomes `<savedir>/<that name>.pure.zip`, so several content files can share one save. Related but separate: `<name>[-<CRC32>].sav` (virtual disk for an installed OS) and `<name>-CDRIVE.sav` (diff disk) | ZIP holding every file created or modified on the union C: drive (the overlay over the read-only game zip), plus `FILEMODS.DBP` (text, CRLF lines `DELETE|<path>`, `REDIRECTFILE|<target>|<source>`, `REDIRECTDIR|...`) recording deletions and renames | Per content, or shared via `.SAVENAME` | Per-game by design. The neutral form is the zip itself: it is an overlay diff, so it only makes sense with the same base content. Lossless. Do not unpack/repack in a way that drops `FILEMODS.DBP` or zip timestamps | https://github.com/schellingb/dosbox-pure `dosbox_pure_libretro.cpp` L811-895 (`DBP_GetSaveFile`), L2730-2760. `src/dos/drive_union.cpp` L57-140 (modification serialisation), L271-300 (`FILEMODS.DBP`), L334-370 (`WriteSaveFile`) |

### Notes for save sync

- The core-managed paths that bypass `.srm` in this set are beetle-ngp `.flash`, handy `.eeprom`, opera `opera/{per_game,shared}/...`, fbneo `fbneo/*.fs|*.hi|*.memcard`, mame2003-plus `mame2003-plus/{nvram,hi,memcard}/`, mame `mame/nvram/...` (and hiscores in the **system** dir), same_cdi `same_cdi/nvram/...`, dosbox_pure `*.pure.zip`, and geargrafx `geargrafx_mb128.sav`. A frontend-agnostic `<base>.srm` rule misses all of them.

## Open items

- The row has no member for a `.SAVENAME` redirect or the legacy `<content name>.sav`.
