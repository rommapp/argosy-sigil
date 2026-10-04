# PC Engine / TurboGrafx-16 / SuperGrafx / PC Engine CD

Status: located
sigil has no row of its own for these cores. Their `<stem>.srm` resolves through the default libretro row, and nothing reads the BRAM or syncs it.

## Emulator research

Scope is game saves (battery RAM, EEPROM, flash, NVRAM, memory cards, emulated HDD content). Save states are excluded.
Sources were fetched 2026-09-26 from the default branch of each repo. Line numbers refer to that snapshot and will drift.
Items with no source are marked UNVERIFIED.

Conventions used below:

- `<savedir>` is the libretro frontend save directory. RetroArch can also insert `<core name>/` or `<content dir name>/` under it (sort_savefiles_by_* settings). That layer belongs to the frontend, not the core.
- `<base>` is the content filename without extension (for multi-disc, the `.m3u`/`.cue`/`.chd` basename that was loaded).
- `.srm` means the core exposes a buffer via `retro_get_memory_data(RETRO_MEMORY_SAVE_RAM)` and the frontend writes it verbatim as `<savedir>/<base>.srm`. The core never sees a filename. A `.srm` is always raw bytes of the exposed buffer, with no header.
- "Core-managed" means the core writes its own file, and the frontend's `.srm` path does not apply. Sync code has to know the core-specific path.

Hardware background. HuCards have no save memory, apart from the Populous card (32 KiB on-card RAM) and the Tennokoe/Tsushin cases below. CD-ROM2/Super CD/Duo units, and the Tennokoe 2 / IFU-30 / Duo internal BRAM, give 2 KiB of backup RAM that all games on a real console share. It uses the "HuBM" file-cabinet layout. The Memory Base 128 is an external 128 KiB unit that is also shared.

BRAM layout ("HUBM"). A 16-byte header starts with the magic `HUBM`. Bytes 6-7 hold the LE pointer to the next free byte, with a 0x8000 bias. Entries follow back to back. Each entry has a 16-byte entry header: LE u16 length (header included), checksum, and a name field at +6..+16. After it comes the payload. A zero length terminates the list. Source: PCE_BRAM_Manager `BRAM_Manager/DataFuncLib.cs` ReadFile/SaveFile, and `ByteFuncLib.cs` constants (maxBRAMSize 2048, memoryOffset 0x8000, headerSize 16, entryHeaderSize 16). https://github.com/Widdiful/PCE_BRAM_Manager. The init string, from geargrafx `src/memory.cpp:153`, is `'H','U','B','M',0x00,0xA0,0x10,0x80`. Checksum algorithm: UNVERIFIED.

Per-game extraction. Every PCE emulator below keeps one BRAM per content, so an emulator's BRAM normally holds one game's entries. A real-hardware BRAM dump holds many. Entries are self-delimiting, so splitting and merging is lossless. The neutral form is a list of entries `{name[10], raw entry bytes}`. Re-injecting means appending entries and rewriting the next-free pointer at bytes 6-7.

| Emulator | Files and naming | Format | Scope and options | Per-game extraction / neutral form | Source |
|---|---|---|---|---|---|
| libretro mednafen_pce (beetle-pce) | `<savedir>/<base>.srm` | Raw 2048 B BRAM (HUBM). Populous gives a 32768 B `PopRAM` instead | Per content. HuCard and CD both get BRAM. No MB128 support | Neutral form is the BRAM entry list. Populous is a raw 32 KiB blob and stays per-game | https://github.com/libretro/beetle-pce-libretro/blob/master/libretro.cpp `retro_get_memory_data`/`_size` L2062-2093 |
| libretro mednafen_pce_fast | `<savedir>/<base>.srm` | Raw 2048 B `SaveRAM` (HUBM), pre-initialised with the HUBM string. Populous gives 32768 B | Per content | Same as beetle-pce | https://github.com/libretro/beetle-pce-fast-libretro/blob/master/libretro.c L60 `SaveRAM[2048]`, L978-983, `retro_get_memory_data` L2818-2843 |
| libretro mednafen_supergrafx | `<savedir>/<base>.srm` | Raw 2048 B `SaveRAM`. Populous gives 32768 B (ROMSpace + 0x40*8192) | Per content | Same as beetle-pce | https://github.com/libretro/beetle-supergrafx-libretro/blob/master/libretro.cpp L2003-2040 |
| libretro geargrafx | (a) `<savedir>/<base>.srm`. (b) Core-managed `<savedir>/geargrafx_mb128.sav` | (a) 2048 B BRAM (only the first 0x800 of an internal 0x2000 buffer is exposed and saved). (b) 131072 B raw MB128 image | (a) Per content. Option `geargrafx_backup_ram` Enabled/Disabled; when disabled the size is 0 and no `.srm` is written. (b) One file shared by every game. Option `geargrafx_mb128` Auto/Enabled/Disabled. Written only when dirty | (a) BRAM entry list. (b) The MB128 has its own directory format (UNVERIFIED layout). Treat it as a shared container and sync it as an opaque blob unless an MB128 parser is written | https://github.com/drhelius/Geargrafx `platforms/libretro/libretro.cpp` L540-560 (SAVE_RAM), L660-720 (`save_mb128`/`load_mb128`, filename), `libretro_core_options.h` L94-106 and L639-652. `src/memory_inline.h` `GetBackupRAMSize` returns 0x800. `src/mb128.h` L62 `kMB128Size = 0x20000` |
| Mednafen standalone (pce, pce_fast; SuperGrafx runs in the pce module) | `<path_sav>/<fbase>.<md5>.sav`. Default `filesys.path_sav="sav"`, `filesys.fname_sav="%f.%M%x"`. `%M` is the game MD5 plus a trailing period, left empty if a legacy file without the hash exists. Mednafen-GENJIN mapper NV: `<fbase>.<md5>.mg<N>` | Raw 2048 B BRAM, written only if the BRAM differs from a freshly initialised one (`IsBRAMUsed`). Reads accept gzip (`GZFileStream`). Populous and Tsushin RAM are 32768 B. Backups rotate into `b/` as gzip | Per game, because the MD5 of each disc/card goes into the name. Settings `pce.disable_bram_cd` / `pce.disable_bram_hucard` turn it off (`DisableBRAM` param). No MB128 | Byte-identical with the libretro `.srm`. Converting is a rename, plus gunzip if the file is gzipped | https://github.com/libretro-mirrors/mednafen-git `src/pce/huc.cpp` L189-208 (`LoadSaveMemory`, gz), L470-482, L539-574 (`HuC_SaveNV`). `src/pce_fast/huc.cpp` L243, L388-392. `src/mednafen.cpp` L246, L257 (path/fname defaults). `src/general.cpp` L141-216 (`%m`/`%M`). `src/file.cpp` L316-342 (gz backups) |

### Notes for save sync

- The core-managed paths that bypass `.srm` in this set are beetle-ngp `.flash`, handy `.eeprom`, opera `opera/{per_game,shared}/...`, fbneo `fbneo/*.fs|*.hi|*.memcard`, mame2003-plus `mame2003-plus/{nvram,hi,memcard}/`, mame `mame/nvram/...` (and hiscores in the **system** dir), same_cdi `same_cdi/nvram/...`, dosbox_pure `*.pure.zip`, and geargrafx `geargrafx_mb128.sav`. A frontend-agnostic `<base>.srm` rule misses all of them.
- Shared containers (one file, many games) are the geargrafx MB128, the fbneo `shared.memcard`, the mame2003-plus `MEMCARD.NNN`, MAME's `.neo` card and `nvram/neocd/saveram`, opera shared NVRAM, same_cdi shared / MAME `cdimono1` NVRAM, the xemu HDD image, and the Jaguar Memory Track on hardware. Of these, lossless per-game split/inject is well-defined for the PCE BRAM (HUBM entries), the Neo Geo cards/NeoCD (NGH-tagged directory) and the xemu HDD (FATX `UDATA\<TitleID>`). It is defined but untooled for 3DO (Opera linked-mem FS), and unknown for CD-i NVR, MB128 and Memory Track (sync those whole).
- Byte-identical pairs: Mednafen standalone `.sav`/`.flash` equals the beetle `.srm`/`.flash` for PCE, NGP, and WonderSwan single-region carts (gunzip first if gzipped). The fbneo MAME-format memcard equals the MAME `.neo` equals the mame2003-plus `MEMCARD.NNN` (all 2 KiB data bytes). Arcade NVRAM is **not** interchangeable between fbneo, mame2003-plus and MAME (different sizes and layouts, e.g. Neo Geo MVS: 64 KiB vs 8 KiB MSB-first vs 64 KiB per-device).
- Emulators that make a hardware-shared store per-game: every PCE core (BRAM per content), fbneo NeoCD, virtualjaguar Memory Track (inside the per-disc `.srm`), opera (default per game), and same_cdi (default per game). Importing a real-hardware dump into any of them means either duplicating the whole container per game or splitting it by the formats above.

## Open items

- No row names geargrafx's shared `geargrafx_mb128.sav`.
- FBNeo keeps a PC Engine CD game's save as `fbneo/pcecd_<name>.fs`. `<name>` is FBNeo's short name from its disc database, or for a disc it doesn't know, the file name lowercased, cut at the first `-`, `(` or `[`, with everything but letters and digits dropped, at most 32 characters (FBNeo `63c4190785` `src/burner/cdlist.cpp` `PceCDInfo_Text`, `CDInfo_GamePrefix`). The `fbneo` row names `fbneo/{romset}.fs`, which never matches. Closing this needs the same disc-id-to-name table as the Neo Geo CD.
