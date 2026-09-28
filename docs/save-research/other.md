# Save formats: PCE/PCE-CD, Neo Geo, NGP, Lynx, Jaguar, 3DO, WonderSwan, arcade, DOS, CD-i, Xbox, Xbox 360

Scope is game saves (battery RAM, EEPROM, flash, NVRAM, memory cards, emulated HDD content). Save states are excluded.
Sources were fetched 2026-09-26 from the default branch of each repo. Line numbers refer to that snapshot and will drift.
Items with no source are marked UNVERIFIED.

Conventions used below:

- `<savedir>` is the libretro frontend save directory. RetroArch can also insert `<core name>/` or `<content dir name>/` under it (sort_savefiles_by_* settings). That layer belongs to the frontend, not the core.
- `<base>` is the content filename without extension (for multi-disc, the `.m3u`/`.cue`/`.chd` basename that was loaded).
- `.srm` means the core exposes a buffer via `retro_get_memory_data(RETRO_MEMORY_SAVE_RAM)` and the frontend writes it verbatim as `<savedir>/<base>.srm`. The core never sees a filename. A `.srm` is always raw bytes of the exposed buffer, with no header.
- "Core-managed" means the core writes its own file, and the frontend's `.srm` path does not apply. Sync code has to know the core-specific path.

---

## 1. PC Engine / TurboGrafx-16 / SuperGrafx / PC Engine CD

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

---

## 2. Neo Geo (MVS / AES / CD)

Hardware background. MVS boards have 64 KiB of battery backup RAM (bookkeeping, settings, high scores, and on-cart saves for several games). AES and MVS both take a memory card: 2-16 KiB, 64-byte blocks, a directory keyed by the game's NGH number, and FAT1/FAT2. The Neo Geo CD uses 8 KiB of internal memory that works like a memory card and is shared by all discs.

Memory card format. Block 0 is the header, which holds the card size at `$A`, FAT checksums, a 16-byte username, and the region. The directory starts at block 1 with 32-256 entries of 4 bytes each, including the game NGH number. FAT1 and FAT2 (FAT2 mirrors FAT1) each hold 64-256 byte entries. A save's first 20 bytes are its title. Source: https://wiki.neogeodev.org/index.php?title=Memory_card. Because each save is tagged with its NGH number, extracting and injecting one game's saves is feasible without loss (neutral form: `{ngh, title[20], blocks}`). Writing it means allocating in both FATs and fixing the checksums. No existing tool was confirmed (UNVERIFIED).

| Emulator | Files and naming | Format | Scope and options | Per-game extraction / neutral form | Source |
|---|---|---|---|---|---|
| libretro fbneo, MVS carts | Core-managed `<savedir>/fbneo/<driver>.fs` | Raw concatenation of the driver's `ACB_NVRAM` areas with no header. MVS gives `NeoNVRAM` 0x10000 B. Gambling boards (vliner, jockeygp) add 0x2000 B "Extra NVRAM". Legacy files with the `FB1 FS1 ` header (compressed) still load and convert to headerless on exit | Per driver (romset). The same `.fs` is used whatever the BIOS mode (`bIsNeogeoCartGame` save/load around mode switches) | Already per-game. Neutral form: raw 64 KiB MVS backup RAM. Byte order is FBNeo's in-memory 68K layout; byte-swapping against MAME's `saveram` is UNVERIFIED | https://github.com/libretro/FBNeo `src/burner/libretro/libretro.cpp` L2194-2206 (path), L1302-1339. `src/burner/nvram.cpp` `BurnNvramLoad` (headerless, rejects `FB1 FS1 `). `src/burn/drv/neogeo/neo_run.cpp` L1393-1411 |
| libretro fbneo, AES/MVS memory card | Core-managed `<savedir>/fbneo/shared.memcard` or `<savedir>/fbneo/<driver>.memcard` | Two formats. FBA "FC1" is the header `FB1 FC1 `, u32 chunk size, u32 burn version, u32 min version, 12 zero bytes, then 0x20000 B of card-area data. MAME-compatible is headerless with only the odd (data) bytes, so 2 KiB for a standard card. The format read is the format written back | Option `fbneo-memcard-mode` = `disabled` (default) / `shared` / `per-game`. Cart games only (`bIsNeogeoCartGame`) | Shared mode is a container: split by NGH per the wiki format. Per-game mode is one card per driver, and it can still hold several games' saves if the user moved it. The MAME-format file is the neutral form. To convert FC1 to MAME, strip the 32 B header and keep the odd bytes | `libretro.cpp` L1702-1822 (`MemCardRead`/`MemCardWrite`), L2177-2183. `retro_common.cpp` L423-437 (option), L1756-1765. `neo_run.cpp` L1346-1377 (card area 0x20000; size taken from header bytes 21/23) |
| libretro fbneo, Neo Geo CD | Core-managed `<savedir>/fbneo/<CDInfo_GamePrefix()>.fs` | Raw 0x4000 B area (16-bit bus; the data is on alternate bytes, so 8 KiB effective) | Per disc (the prefix comes from disc info), unlike hardware, where it is shared | Already per-game. It is a memory-card structure, so entries can be split by NGH | `neo_run.cpp` L1380-1391. `libretro.cpp` L2195 |
| libretro fbneo, hiscores | Core-managed `<savedir>/fbneo/<driver>.hi`. `hiscore.dat` is read from `<systemdir>/fbneo/` | Raw memory ranges from hiscore.dat, concatenated | Per driver. Needs hiscore.dat and the hiscores option | Per-game and opaque | https://github.com/libretro/FBNeo `src/burn/hiscore.cpp` L407, L425-430, L670-677 (`__LIBRETRO__` uses `szAppEEPROMPath`). `libretro.cpp` L1923-1926 |
| libretro mame2003_plus, MVS/AES | Core-managed `<savedir>/mame2003-plus/nvram/<game>.nv` (the `mame2003-plus/` folder comes from `mame2003-plus_core_save_subfolder`, default enabled) | `NVRAM_HANDLER(neogeo)` writes 0x2000 B of `neogeo_sram16`, MSB-first. That is 8 KiB, not the full 64 KiB | Per game | Per-game. Not size-compatible with FBNeo or MAME (8 KiB vs 64 KiB). Treat it as emulator-specific | https://github.com/libretro/mame2003-plus-libretro `src/fileio.c` L184-191, L226-236, L858-879. `src/machine/neogeo_machine.c` L750-771 |
| libretro mame2003_plus, memory card | Core-managed `<savedir>/mame2003-plus/memcard/MEMCARD.<NNN>` (NNN 000-999, default 000; no extension added because the name already contains a dot) | Raw 0x800 B, data bytes only (the same as MAME's `.neo`) | Shared across all games: one numbered card is loaded via `memcard_number` (default 0, `memcard_status` 0). Insert/create goes through the MAME UI (usrintrf). It is saved on exit through the nvram handler | Shared container. Split by NGH | `neogeo_machine.c` L23-24, L816-877. `usrintrf.c` L2988-3064. `fileio.c` L791-816 (`compose_path`) |
| MAME (standalone), MVS | `<nvram_directory>/<system>/saveram` (default `nvram/`). The name is `<basename>[_<bios#>]/<device tag with ':'→'_'>` | Raw 0x10000 B (`NVRAM(config,"saveram",DEFAULT_ALL_0)`, 16-bit share, host byte order UNVERIFIED) | Per system/romset. A non-default BIOS adds a `_<n>` suffix to the directory, so switching BIOS "loses" the save | Per-game | https://github.com/mamedev/mame `src/emu/machine.cpp` L1189-1220 (`nvram_filename`), L1247-1271. `src/mame/snk/neogeo.cpp` L1735, L1999 |
| MAME (standalone), memory card | A user-mounted image, `-memc <file>.neo` or the File Manager. There is no default directory in current MAME (no `memcard_directory` option exists) | Raw 0x800 B (2 KiB card) | Whatever the user mounts. Usually one card shared across games | Split by NGH | `src/mame/snk/ng_memcard.cpp` L64-92. `ng_memcard.h` L24 (ext `neo`), L46. `src/emu/emuopts.cpp` (no memcard dir) |
| MAME (standalone), Neo Geo CD | `nvram/neocd/saveram` (or `neocdz`) | Raw 0x2000 B internal "memory card" | Shared across all CDs. The CD is a cdrom image, and `saveram` does not sit under the image device, so no per-software subfolder is created | Container. Split by NGH | `src/mame/snk/neogeocd.cpp` L157-168, L865-869, L1107. `machine.cpp` L1198-1217 |

---

## 3. Neo Geo Pocket / Color

The cart saves by writing its own flash ROM. NeoPop-derived emulators store only the modified flash blocks.

Flash file format (NeoPop). Header `{u16 valid_flash_id = 0x0053; u16 block_count; u32 total_file_length}` (8 B), then `block_count` x `{u32 start_address (24-bit); u16 data_length; 2 B padding}` (8 B, struct memcpy'd in host byte order, little-endian in practice), each followed by `data_length` bytes. Max 256 blocks.

| Emulator | Files and naming | Format | Scope and options | Per-game extraction / neutral form | Source |
|---|---|---|---|---|---|
| libretro mednafen_ngp (beetle-ngp) | Core-managed `<savedir>/<base>.flash`. There is **no `.srm`**: `retro_get_memory_data` returns only SYSTEM_RAM | NeoPop flash block file, above | Per content | Per-game. The neutral form is the list of `(address, bytes)` flash patches. It is lossless, and can be applied to the ROM to produce a patched ROM when needed | https://github.com/libretro/beetle-ngp-libretro `libretro.c` L778-790 (no SAVE_RAM), L796-812 (`MDFN_MakeFName` → `<savedir>/<base>.<ext>`). `mednafen/ngp/flash.c` |
| Mednafen standalone (ngp) | `<path_sav>/<fbase>.<md5>.flash` | Same format. Written raw (no gzip on this path) | Per game | Byte-identical with beetle-ngp. Converting is a rename | https://github.com/libretro-mirrors/mednafen-git `src/ngp/flash.cpp` L36-64 (structs), L178-199, L255-312 (`make_flash_commit`). `src/ngp/neopop.cpp` L315-340 (`"flash"` ext) |

---

## 4. Atari Lynx

Most commercial Lynx carts do not save. A few carts and many homebrew titles use a 93Cxx serial EEPROM, and the `.lnx` header declares its type.

| Emulator | Files and naming | Format | Scope and options | Per-game extraction / neutral form | Source |
|---|---|---|---|---|---|
| libretro handy | Core-managed `<savedir>/<base>.eeprom`. **No `.srm`**: `retro_get_memory_data` exposes only SYSTEM_RAM | Raw EEPROM image. Size = (ADDR_MASK+1) bytes if type bit 0x80 (8-bit org), else x2 (16-bit org). 93C46 = 128 B, 93C56/66/76/86 larger, up to 2048 B. The type comes from the LNX header or the internal DB (`lynxDB`). The file is written only when the cart declares an EEPROM | Per content | Per-game, raw | https://github.com/libretro/libretro-handy `libretro/libretro.cpp` L1148-1230 (path), L1295-1307. `lynx/eeprom.cpp` L45-80 (Load/Save), L129-194 (`SetEEPROMType`, `Size`). `lynx/cart.cpp` L99, L129 |

---

## 5. Atari Jaguar (+ Jaguar CD)

Carts use a 128-byte 93C46 EEPROM (64 x 16-bit words). Jaguar CD games save to the Memory Track cartridge (128 KiB flash), which all CD games share on hardware.

| Emulator | Files and naming | Format | Scope and options | Per-game extraction / neutral form | Source |
|---|---|---|---|---|---|
| libretro virtualjaguar (current master) | `<savedir>/<base>.srm` | Cart: 128 B (64 BE u16 words). Memory Track cart content (CRC32 0xFDF37F47): 131072 B raw `mtMem`. CD content: 128 B cart EEPROM + 128 B CD EEPROM + 131072 B Memory Track = 131328 B. The EEPROMs are a prefix, so the layout stays compatible with cart-only saves. No-content boot reports 0 | Per content (the Memory Track becomes per-disc, unlike hardware) | Cart EEPROM is per-game and raw. The Memory Track is an AT29C010 flash with a directory of per-title saves, so it is a container. Its internal layout is UNVERIFIED, so treat it as opaque. Neutral form: split the `.srm` at offsets 0/128/256 | https://github.com/libretro/virtualjaguar-libretro `libretro.c` L236-262 (layout comment and sizes), L6630-6674 (`retro_get_memory_*`). The repo is under very active development (commits 2026-09); earlier releases exposed only the 128 B cart EEPROM (UNVERIFIED which release changed it) |

---

## 6. 3DO

The 3DO has 32 KiB of battery NVRAM formatted as an Opera "linked memory" filesystem. Every game writes named files into it, and on hardware all games share it. It fills up often (NFS "NVRAM Full" reports).

Format. A DiscLabel with `dl_VolumeStructureVersion = VOLUME_STRUCTURE_LINKED_MEM`, block size 1, block count 32768, commentary "opera formatted", then a linked list of `LinkedMemBlock`s (flink/blink offsets, big-endian) holding files. Source: https://github.com/libretro/opera-libretro `libopera/opera_nvram.c` L22-81, `libopera/opera_mem.h` L36 (`NVRAM_SIZE = 32 KiB`).

| Emulator | Files and naming | Format | Scope and options | Per-game extraction / neutral form | Source |
|---|---|---|---|---|---|
| libretro opera | Core-managed. **No `.srm`**: SAVE_RAM returns NULL/0. Per game: `<savedir>/opera/per_game/<base>.<ver>.srm`. Shared: `<savedir>/opera/shared/nvram.<ver>.srm`. `<ver>` = `opera_nvram_version` 0-9 (default 0). Written via a `.tmp` file then renamed. Legacy fallbacks on load: per game `<savedir>/<base>.srm` then `<systemdir>/<base>.srm`; shared `<savedir>/3DO.nvram` then `<systemdir>/3DO.nvram`. If there is no save dir, `<systemdir>/opera/...` is used | Raw 32768 B Opera linked-mem filesystem | Option `opera_nvram_storage` = `per game` (default) / `shared`. `opera_nvram_version` selects independent NVRAM "slots" | Per-game mode still holds a full filesystem, but normally only that game's files. Shared mode is a container. Extracting one game means parsing the linked-mem FS and filtering by filename, and no filename-to-title mapping exists. It is lossless in principle. Existing tooling is UNVERIFIED (trapexit's 3dt handles disc images; NVRAM support not confirmed). Recommendation: sync the whole 32 KiB image per game in per-game mode, and treat shared mode as a single opaque container | `opera_lr_nvram.c` (`OLD_NVRAM_FILENAME "3DO.nvram"`, `opera_lr_nvram_save_pergame`/`_shared`, load order). `libretro.c` L430-505, L586-615. `libretro_core_options.c` L271-304 |

---

## 7. WonderSwan / Color

Carts have either SRAM (8-512 KiB) or a serial EEPROM (128 B / 1 KiB / 2 KiB), selected by ROM footer byte 5. The console's internal EEPROM (owner name/birthday, 1 KiB) belongs to the system, not the game. WonderWitch carts save to flash.

| Emulator | Files and naming | Format | Scope and options | Per-game extraction / neutral form | Source |
|---|---|---|---|---|---|
| libretro mednafen_wswan (beetle-wswan) | `<savedir>/<base>.srm` | Raw. It exposes **either** the cart EEPROM (128/1024/2048 B) **or** SRAM (8K/32K/128K/256K/512K), never both. WonderWitch exposes a WW save block (`WSwan_GetWWSaveBlock`). The internal EEPROM is not persisted | Per content | Per-game, raw | https://github.com/libretro/beetle-wswan-libretro `libretro.c` L597-612 (header[5] size table), L1721-1760 |
| Mednafen standalone (wswan) | `<path_sav>/<fbase>.<md5>.sav`. WonderWitch: `<fbase>.<md5>.flash` (512 KiB, the whole cart ROM image) | Raw **concatenation of EEPROM then SRAM** (`eeprom_size + sram_size`). The reader accepts gzip (`MDFN_AmbigGZOpenHelper`). The internal `iEEPROM` is initialised from settings and not saved | Per game | Converting to or from libretro is lossless when only one of the two regions exists, which is the normal case: the file is identical. If a cart had both, split at `eeprom_size`. WonderWitch formats differ (full 512 KiB flash vs a WW save block), and mapping between them is UNVERIFIED | https://github.com/libretro-mirrors/mednafen-git `src/wswan/memory.cpp` L672-734. `src/wswan/eeprom.cpp` L30-32 |

---

## 8. Arcade (FBNeo, MAME 2003-Plus, current MAME)

Arcade "saves" are NVRAM/EEPROM contents (settings, bookkeeping, sometimes progress), hiscore.dat memory dumps, and Neo Geo memory cards (section 2). All are keyed by romset short name, so they are per-game by construction. Formats are driver-specific and not portable between emulators except where noted.

| Emulator | Files and naming | Format | Scope and options | Per-game extraction / neutral form | Source |
|---|---|---|---|---|---|
| libretro fbneo (all drivers) | `<savedir>/fbneo/<driver>.fs` (NVRAM/EEPROM). `<savedir>/fbneo/<driver>.hi` (hiscores). Neo Geo cards: see section 2 | `.fs`: headerless concatenation of the driver's `ACB_NVRAM` areas in scan order (legacy `FB1 FS1 ` compressed files are auto-converted). `.hi`: raw memory ranges from hiscore.dat | Per driver | Already per-game. Neutral form is the emulator-native blob (layout is driver-defined, not portable to MAME) | `libretro.cpp` L1923-1926, L2194-2206. `src/burner/nvram.cpp`. `src/burn/hiscore.cpp` L425-430 |
| libretro mame2003_plus | `<savedir>/mame2003-plus/nvram/<game>.nv`, `<savedir>/mame2003-plus/hi/<game>.hi`, `<savedir>/mame2003-plus/memcard/MEMCARD.<NNN>`. Setting `mame2003-plus_core_save_subfolder`=disabled drops the `mame2003-plus/` level. The hiscore DB is under the system path | `.nv`: whatever the driver's `NVRAM_HANDLER` writes, one file. `.hi`: raw hiscore ranges. `.mem`: 2 KiB Neo Geo card | Per game, except the memcard (shared, numbered). `mame2003-plus_autosave_hiscore` = default/recursively/disabled | Per-game. MAME 0.78-era layout; not compatible with current MAME's per-device files | https://github.com/libretro/mame2003-plus-libretro `src/fileio.c` L110-140, L184-243, L825-886. `src/mame.c` L488, L521. `src/hiscore.c` L166, L203. `src/mame2003/core_options.c` L656-685. `src/mame2003/mame2003.h` L62 |
| MAME standalone | `<nvram_directory>/<system>[_<bios>]/<device_tag>` (one raw file per NVRAM-capable device: `saveram`, `eeprom`, `nvram`, timekeepers etc.; `:` in tags becomes `_`). Devices under a mounted image add `/<software basename>/`. Hiscores: `<homepath>/hiscore/<romname>.hi` or `<romname>_<softlist>.hi` (hiscore plugin, homepath default `.`) | Raw per device. Zero-length files are deleted on save | Per system, or per software when the device belongs to an image. The hiscore plugin saves on update, or only on exit (plugin setting) | Per-game. The neutral form is the directory `nvram/<system>/` as a unit, keeping every device file | https://github.com/mamedev/mame `src/emu/machine.cpp` L1189-1271. `src/emu/emuopts.cpp` L60. `plugins/hiscore/init.lua` L26-28, L210-216 |
| libretro mame (current) | `<savedir>/mame/nvram/<system>/<tag>`. `cfg`, `diff`, `input`, `states`, `snaps` also go under `<savedir>/mame/`. **homepath = `<systemdir>/mame/`**, so hiscore files land in `<systemdir>/mame/hiscore/<romname>.hi`, not the save dir | Same as standalone | Same. Option `mame_paths_enable` (read MAME paths from ini) skips this mapping | Same. Sync code must also watch the system dir for `.hi` | https://github.com/libretro/mame `src/osd/libretro/libretro-internal/retro_init.cpp` L108-150 (`dir_name`/`opt_name`/`opt_type`), L555-580. Whether the hiscore plugin is enabled by default in the core: UNVERIFIED |

MAME's `diff/` directory holds CHD write-diffs (for example arcade hard-disk games). It is not a normal save. Include it only if full hard-disk-game persistence is wanted.

---

## 9. DOS

| Emulator | Files and naming | Format | Scope and options | Per-game extraction / neutral form | Source |
|---|---|---|---|---|---|
| libretro dosbox_pure | Core-managed `<savedir>/<content name>.pure.zip`. Legacy `<savedir>/<content name>.sav` is still read if no `.pure.zip` exists (not in strict mode). A redirect is possible: if the C: drive holds a file `*.SAVENAME`, the save becomes `<savedir>/<that name>.pure.zip`, so several content files can share one save. Related but separate: `<name>[-<CRC32>].sav` (virtual disk for an installed OS) and `<name>-CDRIVE.sav` (diff disk) | ZIP holding every file created or modified on the union C: drive (the overlay over the read-only game zip), plus `FILEMODS.DBP` (text, CRLF lines `DELETE|<path>`, `REDIRECTFILE|<target>|<source>`, `REDIRECTDIR|...`) recording deletions and renames | Per content, or shared via `.SAVENAME` | Per-game by design. The neutral form is the zip itself: it is an overlay diff, so it only makes sense with the same base content. Lossless. Do not unpack/repack in a way that drops `FILEMODS.DBP` or zip timestamps | https://github.com/schellingb/dosbox-pure `dosbox_pure_libretro.cpp` L811-895 (`DBP_GetSaveFile`), L2730-2760. `src/dos/drive_union.cpp` L57-140 (modification serialisation), L271-300 (`FILEMODS.DBP`), L334-370 (`WriteSaveFile`) |

---

## 10. Philips CD-i

The CD-i Mono-I has an 8 KiB MK48T08 timekeeper NVRAM. The high bytes carry the RTC registers. CD-RTOS keeps an `/nvr` filesystem there that all discs share on hardware.

| Emulator | Files and naming | Format | Scope and options | Per-game extraction / neutral form | Source |
|---|---|---|---|---|---|
| libretro same_cdi | Core-managed. Per game (default): `<savedir>/same_cdi/nvram/<gamename>/cdimono1/mk48t08`. Shared: `<savedir>/same_cdi/nvram/cdimono1/mk48t08`. `<gamename>` is the parsed content name (`MgameName`) | Raw 0x2000 B timekeeper image (includes the clock registers at the top) | Option `same_cdi_nvram_saves` = `enabled` (per game, default) / `disabled` (shared). The forced system is `cdimono1` | Per-game mode is already per-game (a whole NVRAM per disc). Shared mode is a container. Splitting the CD-RTOS NVR filesystem per title is UNVERIFIED and has no known tool. Recommendation: sync the whole 8 KiB file per game | https://github.com/libretro/same_cdi `src/osd/libretro/libretro-internal/retro_init.cpp` L63, L83-86, L96-118, L385-410. `libretro.cpp` L163, L184, L486-495. `libretro_shared.h` L114 (`core[]="same_cdi"`). `src/mame/drivers/cdi.cpp` L93, L323. `src/devices/machine/timekpr.cpp` L162, L443-460 |
| MAME standalone (cdimono1) | `nvram/cdimono1/mk48t08` | Same | Shared across all discs (the CD image does not own the timekeeper) | Container, as above | mamedev/mame `src/mame/philips/cdi.cpp`. `machine.cpp` L1189-1220 |

---

## 11. Xbox (original)

| Emulator | Files and naming | Format | Scope and options | Per-game extraction / neutral form | Source |
|---|---|---|---|---|---|
| xemu | One HDD image for all games, `xbox_hdd.qcow2` (path in xemu settings, `sys.files.hdd_path`). The prebuilt image is 8 GB qcow2. Saves live on partition E: in `E:\UDATA\<TitleID 8-hex>\` (`TitleMeta.xbx` = `TitleName=...`, `TitleImage.xbx` 128x128 texture) and `E:\UDATA\<TitleID>\<save folder>\` (`SaveMeta.xbx` = `Name=...`, optional `SaveImage.xbx` 64x64, then game files). Some titles also keep data in `E:\TDATA\<TitleID>\`. The per-console `eeprom.bin` sits alongside and holds the HDKey | qcow2 wrapping a raw Xbox disk. The E: FATX partition starts at byte 0xABE80000, size 0x131F00000 (retail layout). X/Y/Z cache partitions at 0x80000/0x2EE80000/0x5DC80000, C: at 0x8CA80000 | One shared container for every game. Optional 8 MiB XMU (memory unit) images, FATX, attached through the monitor (`drive_add`/`device_add usb-storage`) | Feasible and lossless at the file level: extract `E:\UDATA\<TitleID>\` (and `TDATA\<TitleID>\`) as a directory tree. That tree is the neutral form, and it is what real-console tools and FTP use. Reading/writing needs a qcow2 layer (`qemu-img convert` or `qemu-nbd`) plus FATX (mborgerson/fatx: libfatx, fatxfs FUSE, pyfatx `python -m pyfatx -x`). The image must not be open in xemu while it is written. Signatures: many games sign saves with a per-title key. "Non-roamable" titles also mix in the console's HDKey from EEPROM, so their saves only validate with the same `eeprom.bin`. Moving between xemu installs with different EEPROMs needs re-signing or the same EEPROM | https://xboxdevwiki.net/Hard_Drive (partition table). https://xboxdevwiki.net/Xbox_Savegame_System (UDATA/TDATA, meta files). https://xemu.app/docs/required-files/ (8G qcow2 `xbox_hdd.qcow2`). https://xemu.app/docs/xmus/. https://github.com/mborgerson/fatx (`README.md`, `pyfatx/README.md`, `fatxfs/README.md` qcow notes). https://consolemods.org/wiki/Xbox:Games_with_Non-Roamable_(EEPROM-Locked)_Saves. https://github.com/feudalnate/Original-Xbox-Gamesave-Resigners/blob/master/XSavSig.md. The `sys.files.hdd_path` key name is UNVERIFIED |

---

## 12. Xbox 360

Real consoles store saves as STFS "CON" packages, content type `0x00000001` (SavedGame). Xenia instead stores each package **extracted as a host directory**, with the STFS header/metadata kept separately.

| Emulator | Files and naming | Format | Scope and options | Per-game extraction / neutral form | Source |
|---|---|---|---|---|---|
| Xenia Canary | `<content_root>/<XUID 16-hex>/<TitleID 8-hex>/00000001/<save file_name>/...` (extracted files). Metadata: `<content_root>/<XUID>/<TitleID>/Headers/00000001/<save file_name>.header` = raw `XContentContainerHeader` (display name, thumbnail etc.), padded to 4 KiB. `<content_root>` = `<storage_root>/content` by default. `storage_root` = the exe folder if `portable` (default true on Windows) or `portable.txt` exists, else `<user docs>/Xenia`. Overridable with `--content_root`. XUID is the signed-in profile's (randomly generated per profile; a default profile uses `B13EBABEBABEBABE`). Content with no user uses XUID `0000000000000000`, and marketplace content is forced to 0. Profile/GPD data: `<content_root>/<XUID>/FFFE07D1/00010000/<XUID>/` | Plain host directory tree plus a header blob | Per title, per profile. Old upstream layouts (`content/<TitleID>/<type>`) are migrated on startup: type `00000001` goes to the user XUID, others to the common XUID, and `Headers` is copied to both | Per-game extraction is trivial and lossless: one directory per save plus its `.header`. Neutral form = `{title_id, content_type=1, file_name, display_name, files[]}`. To inject into another profile, place it under the target XUID. Games that embed the XUID or profile ID inside their own files may reject it (UNVERIFIED per title). Converting to a console CON package requires STFS rebuild and console/profile re-signing with third-party tools (UNVERIFIED) | https://github.com/xenia-canary/xenia-canary (branch `canary_experimental`) `src/xenia/kernel/xam/content_manager.cc` L192-230 (`ResolvePackageRoot`/`ResolvePackagePath`), L45-68 (header write). `src/xenia/kernel/xam/xcontent/xcontent_package_directory.cc` L37-44 (`ComputeHeaderPath`), `xcontent_package.h` L247 (`kGameContentHeaderDirName = "Headers"`). `src/xenia/kernel/xam/profile_manager.cc` L453-512. `src/xenia/emulator.cc` L715-790 (migration). `src/xenia/app/xenia_main.cc` L89-99, L118-130, L482-516 |
| Xenia (upstream master) | `<content_root>/<TitleID>/00000001/<save file_name>/` (**no XUID level**). Thumbnail: `<package>/__thumbnail.png`. Per-profile game user content: `<content_root>/<TitleID>/profile/<user_name>/` | Host directory tree | Per title. Single implicit user | Per-game, lossless. Converting to Canary means moving it under `<XUID>/`, which Canary does automatically on startup | https://github.com/xenia-project/xenia `src/xenia/kernel/xam/content_manager.cc` L26, L60-79, L196-225, L240-254 (last upstream commit 2026-02-18) |

---

## Notes for save sync

1. The core-managed paths that bypass `.srm` in this set are beetle-ngp `.flash`, handy `.eeprom`, opera `opera/{per_game,shared}/...`, fbneo `fbneo/*.fs|*.hi|*.memcard`, mame2003-plus `mame2003-plus/{nvram,hi,memcard}/`, mame `mame/nvram/...` (and hiscores in the **system** dir), same_cdi `same_cdi/nvram/...`, dosbox_pure `*.pure.zip`, and geargrafx `geargrafx_mb128.sav`. A frontend-agnostic `<base>.srm` rule misses all of them.
2. Shared containers (one file, many games) are the geargrafx MB128, the fbneo `shared.memcard`, the mame2003-plus `MEMCARD.NNN`, MAME's `.neo` card and `nvram/neocd/saveram`, opera shared NVRAM, same_cdi shared / MAME `cdimono1` NVRAM, the xemu HDD image, and the Jaguar Memory Track on hardware. Of these, lossless per-game split/inject is well-defined for the PCE BRAM (HUBM entries), the Neo Geo cards/NeoCD (NGH-tagged directory) and the xemu HDD (FATX `UDATA\<TitleID>`). It is defined but untooled for 3DO (Opera linked-mem FS), and unknown for CD-i NVR, MB128 and Memory Track (sync those whole).
3. Byte-identical pairs: Mednafen standalone `.sav`/`.flash` equals the beetle `.srm`/`.flash` for PCE, NGP, and WonderSwan single-region carts (gunzip first if gzipped). The fbneo MAME-format memcard equals the MAME `.neo` equals the mame2003-plus `MEMCARD.NNN` (all 2 KiB data bytes). Arcade NVRAM is **not** interchangeable between fbneo, mame2003-plus and MAME (different sizes and layouts, e.g. Neo Geo MVS: 64 KiB vs 8 KiB MSB-first vs 64 KiB per-device).
4. Emulators that make a hardware-shared store per-game: every PCE core (BRAM per content), fbneo NeoCD, virtualjaguar Memory Track (inside the per-disc `.srm`), opera (default per game), and same_cdi (default per game). Importing a real-hardware dump into any of them means either duplicating the whole container per game or splitting it by the formats above.
