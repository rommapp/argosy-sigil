# Sega platforms: emulator game-save storage (not save states)

Research date 2026-09-26. Sources are shallow clones read locally unless marked otherwise.

Commits read:

- libretro/Genesis-Plus-GX `c2838c7`
- libretro/picodrive `ab02114`
- libretro/beetle-saturn-libretro `1382b85`
- libretro/yabause master `8926b0c` (yabause core), branch `kronos` `3791ffb2`, branch `yabasanshiro` `09ed8e5b`
- FCare/Kronos `d451a55` (same libretro save code as the libretro/yabause `kronos` branch, which is what the buildbot builds)
- flyinghead/flycast `869038f` (the buildbot builds the libretro core from this repo; libretro/flycast is a deprecated fork per its repo description)
- mednafen 1.32.1 source tarball (mednafen.github.io/releases)
- ares-emulator/ares `4cb8d92` (sparse)
- euan-forrester/save-file-converter `0a9786f` (format reference implementation)

The buildbot repo mapping comes from `libretro-super/recipes/linux/cores-linux-x64-generic`.

GH links below use `blob/master` plus the line numbers at the commits above.

---

## 1. Direct answers

### genesis_plus_gx and Sega CD `.srm`

Genesis Plus GX writes no `{stem}.srm` for a normal (CD-boot, "Mode 2") Sega CD game. The core does all CD saving itself, in `.brm` files, from `bram_load()` / `bram_save()`.

- `retro_get_memory_data/size(RETRO_MEMORY_SAVE_RAM)` returns `sram.sram` / size only when `sram.on` is set (`libretro/libretro.c:3750-3783`).
- In CD mode `genesis.c:161-173` calls `scd_init()` instead of `md_cart_init()`, so `sram_init()` never runs.
- When the backup RAM cart is enabled, `cd_cart_init()` also runs `memset(&sram, 0, sizeof(T_SRAM))` (`core/cd_hw/cd_cart.c:183-199`). So `sram.on == 0`, the reported size is 0, and RetroArch writes no `.srm`.
- The exception is a real MD cartridge booted alongside the CD (Mode 1, `scd.cartridge.boot`). Then `md_cart_init()` runs (`cd_cart.c:249`), and that cartridge's SRAM goes through `.srm`.
- The CD saves live in:
  - Internal BRAM (8 KiB). The `genesis_plus_gx_system_bram` option picks the file. "per bios" (the default) gives `{savedir}/scd_U.brm`, `scd_E.brm` or `scd_J.brm`, chosen by the disc's region byte. "per game" gives `{savedir}/{content stem}.brm` (`libretro.c:1385-1403`).
  - Backup RAM cart. `genesis_plus_gx_cart_size` sets the size (default `4meg`, i.e. 512 KiB). `genesis_plus_gx_cart_bram` picks the file. "per cart" (the default) gives `{savedir}/{N}Kbit_cart.brm` / `{N}Mbit_cart.brm`, for example `4Mbit_cart.brm`. "per game" gives `{savedir}/{stem}_{N}Kbit_cart.brm` / `{stem}_{N}Mbit_cart.brm` (`libretro.c:1430-1502`).
- The core reads `.brm` files only in `retro_load_game` (`libretro.c:3674`) and writes them only in `retro_unload_game` (`libretro.c:3729`). A save happens only if the CRC changed and the "SEGA_CD_ROM/RAM_CARTRIDGE" signature is intact (`bram_save`, `libretro.c:1140-1180`). A crash loses the session's BRAM writes.
- The frontend never sees `.brm` files, so RetroArch cloud sync and save-protect do not cover them.
- Oddity (UNVERIFIED at runtime): `cart_size = "disabled"` stores `0xff` (`libretro.c:1411-1412`). `cd_cart_init` then sets `scd.cartridge.id = 0xff`, which is truthy, and computes `1 << (0xff+13)`. "Disabled" may not actually disable the cart.

### Beetle Saturn (mednafen_saturn)

- **Is `.srm` byte-identical to `.bkr`? Yes.**
  - In both modes the persisted object is the raw `uint8_t BackupRAM[32768]` (`mednafen/ss/ss.c:158`).
  - Libretro mode hands the frontend `BackupRAM` / `sizeof(BackupRAM)` (`libretro.c:1891-1910`).
  - Mednafen mode writes the same buffer with `cdstream_write(&brs, BackupRAM, sizeof(BackupRAM))` (`ss.c:2159-2185`).
  - Both are 32768 bytes, odd bytes only, collapsed. The option text says "rename `<game>.bkr` to `<game>.srm` once to migrate" (`libretro_core_options.h:826-840`).
  - Standalone Mednafen writes the same 32 KiB raw buffer (`mednafen-1.32.1/src/ss/ss.cpp:1878-1886`).
- **`beetle_saturn_save_method`**, values `libretro` (default) or `mednafen`.
  - Added 2026-05-25 in commit `a0c1c52f` ("memcards: add Save Method core option (.bkr vs .srm), default Mednafen"). Commit `0977d2c4` flipped the default to Libretro on 2026-05-26.
  - Before `a0c1c52f`, the core exposed `RETRO_MEMORY_SAVE_RAM` and also wrote `.bkr`. So both files existed in parallel with the same content, and the frontend `.srm` load landed on top of the `.bkr` read.
  - The exact commit that first exposed SAVE_RAM is UNVERIFIED. It was probably the 2026-05-11 "audit: omnibus fix pass" (`a29a316f`).
  - Before that exposure, only `.bkr` was written.
  - The two modes are now mutually exclusive. In libretro mode `SS_LoadBackupRAM` / `SS_SaveBackupRAM` return early (`ss.c:2168, 2240`).
- **`.smpc` contents.** 12 bytes (`SMPC_SaveNV`, `mednafen/ss/smpc.c:413-431`, identical in standalone `smpc.cpp:377-382`):
  - 1 byte `RTC.Valid`
  - 7 bytes `RTC.raw`, all BCD: year hi, year lo, (weekday<<4 | month), mday, hour, minute, second
  - 4 bytes SMPC `SaveMem` (SMEM). The BIOS keeps its settings here, and the language sits in the low nibble of `SaveMem[3]` (`smpc.c:466-467`).
  - With `ss.smpc.autortc` on, `SMPC_SetRTC` overwrites the clock and language at load (`ss.c:2086`). The file mostly matters when autortc is off.
- **Does each file have its own shared option? No, there are two options for three files.**
  - `MDFN_MakeFName` uses `MDFNMKF_SAV` for both `.bkr` and `.smpc`, and both follow `beetle_saturn_shared_int`.
  - `MDFNMKF_CART` is used for `.bcr` (and ST-V `.stveep`) and follows `beetle_saturn_shared_ext` (`libretro.c:2197-2215`).
  - Shared name is `mednafen_saturn_libretro_shared.{bkr,smpc,bcr}`. Per-game name is `{content basename}.{ext}`, all in the libretro save dir.
  - Consequence: in libretro save mode `shared_int` no longer affects the internal BRAM, because the frontend names `.srm` after the content. It then affects only `.smpc`.

---

## 2. Per (platform, emulator) rows

Legend for Format:

- raw = memory dump with no header
- expanded = each data byte sits on an odd address with a filler byte (0xFF or 0x00) before it (2x size)
- container = a filesystem holding many games' saves

| Platform | Emulator | Files + naming | Format | Scope + switching options | Per-game extraction / neutral form | Source |
|---|---|---|---|---|---|---|
| SMS / Game Gear | genesis_plus_gx (libretro) | `{stem}.srm` (frontend) | Raw `sram.sram` buffer, max 64 KiB. Size reported at save time = up to the last non-0xFF byte, so the length varies and trailing 0xFF is trimmed. Empty (all 0xFF) gives size 0 and no file. Initial fill 0xFF. | Per-game | Trivial. The neutral form is the raw bytes, right-padded with 0xFF to 64 KiB (or to cart RAM size) | `libretro/libretro.c:3750-3783`; `core/cart_hw/sms_cart.c:604-607` (always `sram.on=1`), `:1411,1438,1482` (mapper into `sram.sram`) |
| SMS / Game Gear | picodrive (libretro) | `{stem}.srm` | Raw, fixed 0x8000 (32 KiB, "2 banks of 16 KB") zero-filled. Reported only if any byte is nonzero | Per-game | Trivial. PicoDrive pads with 0x00 where GPGX pads with 0xFF. Converting means pad or trim; leading bytes match | `pico/cart.c:1390-1402`; `platform/libretro/libretro.c:1745-1762` |
| Genesis / MD | genesis_plus_gx (libretro) | `{stem}.srm` | Raw 64 KiB address image `sram.sram[addr & 0xffff]` (`sram.c:260-273`). 8-bit odd-byte SRAM is stored expanded (unused even bytes stay 0xFF). EEPROM games (I2C/SPI/93C) also live in `sram.sram`. Trailing 0xFF trimmed as above. Init 0xFF (0x00 for "Sonic 1 Remastered") | Per-game | Trivial. Neutral form is the 64 KiB address image, 0xFF padded | `core/cart_hw/sram.c:63-125, 260-273`; `libretro.c:3750-3783` |
| Genesis / MD | picodrive (libretro) | `{stem}.srm` | Raw `Pico.sv.data`. Size = `sv.end - sv.start + 1`, where the header start is aligned down to even and the end is forced odd, so odd-byte SRAM is expanded. EEPROM carts = 0x2000. calloc 0x00 fill. Reported only if nonzero | Per-game | Trivial. Byte layout matches GPGX from offset 0 when SRAM starts at 0x200000. Differences are pad value and length. UNVERIFIED for carts whose SRAM does not start at a 64 KiB boundary | `pico/cart.c:1300-1336`; `pico/memory.c:825-860` |
| Genesis / MD | ares | `save.ram` / `save.eeprom` in the game pak, persisted under ares's `Saves/` (`desktop-ui/emulator/emulator.cpp:24`: `{userData}/ares/Saves/{system}/`). The exact file name there is UNVERIFIED | Raw. Interleaving of word/upper/lower RAM (`Interface::save(wram, uram, lram)`) is UNVERIFIED | Per-game | Probably trivial. Layout UNVERIFIED | `ares/md/cartridge/board/standard.cpp:12-26` |
| Genesis / MD | Kega Fusion, BlastEm, standalone Mednafen MD | Mednafen MD: `sav/{stem}.srm`-style per `filesys.fname_sav` (`%f.%M%x`, default dir `sav`). Kega and BlastEm are UNVERIFIED | UNVERIFIED | Per-game | UNVERIFIED | mednafen.github.io/documentation (fname_sav) |
| Sega CD / Mega CD | genesis_plus_gx (libretro) | Internal: `scd_{U,E,J}.brm` (per bios, default) or `{stem}.brm` (per game). Cart: `{N}{K,M}bit_cart.brm` (per cart, default) or `{stem}_{N}{K,M}bit_cart.brm`. No `.srm` in CD-boot mode (section 1) | Raw BRAM filesystem. Internal is 8 KiB (0x2000). Cart is 8 KiB << id (128 Kbit to 4 Mbit, default 512 KiB), stored collapsed (odd bytes only). The format block sits in the last 0x40 bytes | Shared by default for both (per region, per cart size). `genesis_plus_gx_system_bram`, `genesis_plus_gx_cart_bram`, `genesis_plus_gx_cart_size`, all restart-required. The core owns the files, not the frontend | Feasible and lossless at the entry level (section 3.1). Neutral form: a freshly formatted 8 KiB BRAM holding only that game's entries, or the entry tuple (11-char name, protect flag, data blocks) | `libretro/libretro.c:1043-1180, 1385-1502, 3674, 3729`; `core/cd_hw/cd_cart.c:180-200`; option text `libretro/libretro_core_options.h:165-210` |
| Sega CD / Mega CD | picodrive (libretro) | `{stem}.srm` (frontend) | Default: raw internal BRAM, 0x2000 (8 KiB). With `picodrive_ramcart` = enabled: 0x12000 bytes, where [0,0x2000) is internal BRAM and [0x2000,0x12000) is a 64 KiB cart, collapsed. The libretro path does not sync `Pico_mcd->bram` into the first 8 KiB ("TODO" at `libretro.c:1715`). The option text warns that enabling it discards internal BRAM | Per-game (the frontend names it by content). No shared mode | Feasible (same BRAM filesystem). Converting to GPGX is a straight rename of the 8 KiB file | `platform/libretro/libretro.c:1710-1752`; `pico/cd/mcd.c:111-121`; `pico/cd/memory.c:677-712`; `platform/libretro/libretro_core_options.h:163-173` |
| Sega CD / Mega CD | picodrive (standalone) | `.brm` via `emu_save_load_game` (naming UNVERIFIED) | 8 KiB internal, or 0x12000 with the RAM cart (bram copied into the first 8 KiB). The non-cart file is not truncated because it "may contain RAM cart data after normal brm" | UNVERIFIED | As above | `platform/common/emu.c:965-1010` |
| Sega CD / Mega CD | ares | `backup.ram` in the **system** pak, so one BRAM for all Mega CD games | Raw 8 KiB (`bram.allocate(8_KiB)`) | Shared (per system/BIOS). No option found | Feasible (BRAM filesystem). Disk path UNVERIFIED | `ares/md/mcd/mcd.cpp:41, 79-80, 114` |
| Sega CD / Mega CD | Kega Fusion, Gens, BlastEm | UNVERIFIED | UNVERIFIED. Whether any of these write byte-expanded Sega CD files is unknown | UNVERIFIED | Same filesystem | `save-file-converter/frontend/src/util/SegaCd.js` |
| 32X | picodrive (libretro) | `{stem}.srm` | Same cart SRAM path as MD (`Pico.sv`) | Per-game | Trivial | `platform/libretro/libretro.c:1745-1762` (32X support for this path is UNVERIFIED line-by-line) |
| 32X | ares | `save.ram` / `save.eeprom` (game pak) | Raw | Per-game | Trivial | `mia/medium/mega-32x.cpp:55-63` |
| 32X | genesis_plus_gx | Not supported (no 32X emulation) | n/a | n/a | n/a | n/a |
| Saturn | Beetle Saturn (libretro), save_method=libretro (default since 2026-05-26) | Internal: `{stem}.srm` (frontend). RTC/SMEM: `{stem}.smpc` (core). Cart: `{stem}.bcr` (core) | `.srm` is raw 32768 B collapsed BRAM (64-byte blocks, 512 blocks). `.smpc` is 12 B. `.bcr` is raw 0x80000 (512 KiB) collapsed, 512-byte blocks, uncompressed | `.srm` is always per-game. `.smpc` is shared iff `beetle_saturn_shared_int`. `.bcr` is shared iff `beetle_saturn_shared_ext`. Cart presence comes from the internal DB (`ss.cart` auto, default "backup") | Feasible and lossless (section 3.2). Neutral form: `.BUP` (Vmem header + data) per save | `libretro.c:407-437, 1891-1910, 2197-2215`; `mednafen/ss/ss.c:158, 2159-2270, 2289-2440`; `mednafen/ss/cart/backup.c:36, 69-76`; `mednafen/ss/smpc.c:413-467`; git `a0c1c52f`, `0977d2c4` |
| Saturn | Beetle Saturn (libretro), save_method=mednafen | Internal: `{stem}.bkr` or `mednafen_saturn_libretro_shared.bkr`. `.smpc` and `.bcr` as above | `.bkr` is byte-identical to the `.srm` above | `.bkr` and `.smpc` follow `shared_int`. `.bcr` follows `shared_ext`. No `.srm` is exposed | As above | same |
| Saturn | Mednafen (standalone) | `{path_sav}/{stem}.bkr`, `.smpc`, `.bcr`. `filesys.fname_sav` defaults to `%f.%M%x`, where `%M` is empty first and becomes `{md5}.` only on a collision. Rotating backups via `MDFN_BackupSavFile(10, "bkr")` | `.bkr` is raw 32 KiB (same as Beetle). `.smpc` is 12 B (same). **`.bcr` is gzip-compressed** (`GZFileStream` WRITE, `ss.cpp:1952`). Its read goes through GZFileStream, which (via zlib) presumably also accepts raw input (UNVERIFIED) | Per-game. There is no shared mode | As Beetle. Moving `.bcr` from standalone to Beetle needs a gunzip: Beetle reads raw (`filestream_read`, `ss.c:2250-2310`) | `mednafen-1.32.1/src/ss/ss.cpp:1878-1996`; `src/ss/smpc.cpp:377-382`; mednafen.github.io/documentation/fname_format.txt |
| Saturn | Kronos (libretro, buildbot = libretro/yabause `kronos`) | Default: `{savedir}/kronos/saturn/{stem}.ram` (internal) and `{savedir}/kronos/saturn/{stem}-ext{512K,1M,2M,4M}.ram` (backup cart). With `kronos_use_beetle_saves` = enabled: `{savedir}/{stem}.bkr` and `{savedir}/{stem}.bcr` (forces a 4 Mbit cart) | Internal: `extend_backup=0` in libretro, so 0x8000 collapsed (`addr>>1` on odd addresses), the same byte layout as Beetle. Cart: 0x80000 etc., collapsed. A size mismatch at load triggers a reformat (`BackupInit`) | Per-game. No shared mode. `kronos_addon_cartridge` default `512K_backup_ram` | Feasible. Note that "share with beetle" targets `.bkr`, which Beetle only uses in save_method=mednafen. With Beetle's default libretro mode the files no longer meet | `yabause/src/libretro/libretro.c:916-923, 1299-1317, 1587, 1709-1712`; `sys/memory/src/memory.c:79-83, 507-570, 1283-1325`; `sys/memory/src/cs0.c:921-927, 1393-1411`; `libretro_core_options.h:82-107` |
| Saturn | Kronos (standalone) | UNVERIFIED path (Linux port default `./bup.ram`, `port/linux/main.c:194`) | Standard 0x8000 collapsed, or extended 0x800000 when `extend_backup` is set | UNVERIFIED | As above | `sys/memory/src/memory.c:1283-1325` |
| Saturn | yabause (libretro, master) | `{savedir}/{stem}.srm`, but **the core writes it**. `retro_get_memory_*` returns NULL/0 | 0x10000 (64 KiB) expanded: data on odd bytes, 0xFF on even bytes (`FormatBackupRam` writes the `0xFF,'B',0xFF,'a'...` header) | Per-game | Feasible. Collapse by taking odd bytes, which gives the Beetle/Kronos 32 KiB format, lossless both ways. No backup cart option (only 1M/4M RAM carts) | `yabause/src/libretro/libretro.c:698-708, 1059, 1116, 1176-1184`; `yabause/src/yabause.c:200-208, 446-477`; `yabause/src/memory.c:331-357, 1274-1296` |
| Saturn | Yaba Sanshiro (libretro, branch `yabasanshiro`) | `{savedir}/yabasanshiro/backup.bin`, **one file for every game** | Extended internal backup, memory-mapped. save-file-converter reports 0x800000 B expanded (0x400000 collapsed) with 64-byte blocks. `tweak_backup_file_size` value UNVERIFIED | Shared container, no option | Feasible via the BRAM filesystem (same directory structure, larger volume). This is the only way to get per-game data | `libretro/yabause@yabasanshiro: yabause/src/libretro/libretro.c:1000-1004, 1077`; `yabause/src/yabause.c:220-258`; `save-file-converter/.../SegaSaturn/Emulators/yabasanshiro.js` |
| Saturn | Yaba Sanshiro (standalone Android) | UNVERIFIED | Same expanded extended format (per save-file-converter) | Shared | As above | UNVERIFIED |
| Dreamcast | flycast (libretro, from flyinghead/flycast) | `reicast_per_content_vmus` = disabled (default): `{system}/dc/vmu_save_{A1..D2}.bin`, shared. "VMU A1": `{savedir}/{gameId}.A1.bin` for port A1 only. "All VMUs": `{savedir}/{gameId}.{A1..D2}.bin`. `gameId` is the IP.BIN product number with trailing whitespace trimmed and ` /\:*?\|<>` replaced by `_`. Legacy per-content name `{stem}.{port}.bin`: when found, the core **copies it to the gameId name and deletes the old file** (since commit `5fc84acd`, 2024-11-03). No `.srm` (no SAVE_RAM) | Raw VMU flash image, exactly 131072 B (`u8 flash_data[128_KB]`). An all-zero file is reformatted on load | Shared by default. Per-game via the option. Multi-disc games share one VMU through the product number | Feasible and lossless (section 3.3). Neutral form: VMS+VMI pair, or DCI | `shell/libretro/oslib.cpp:40-68`; `shell/libretro/libretro.cpp:843-860, 2242-2262`; `shell/libretro/libretro_core_options.h:1165-1178`; `core/emulator.cpp:858`; `core/hw/maple/maple_devs.cpp:353, 437-475` |
| Dreamcast | flycast (standalone) | `PerGameVmu` (default **true**) gives A1 = `{gameId}_vmu_save_A1.bin`. Others (and A1 when the option is off) are `vmu_save_{port}.bin`. Looked up in `VMUPath` if set, otherwise the writable data dir. Legacy fallback `{content fileName}_vmu_save_A1.bin` | Raw 128 KiB | A1 per-game by default. Other ports shared | As above | `core/oslib/oslib.cpp:50-100`; `core/cfg/option.cpp:234`; `core/stdclass.cpp:140-143` |
| Dreamcast | redream (standalone, closed source; the libretro core is abandoned) | `vmu0.bin` to `vmu3.bin` (ports A to D) in the redream data dir (libretro docs: in the save dir) | Raw 128 KiB VMU image (UNVERIFIED from source) | Shared across all games. No per-game option (LaunchBox plugins swap `vmu0.bin`) | Feasible via the VMU filesystem | docs.libretro.com/library/redream; forums.launchbox-app.com/files/file/5337-redream-per-game-vmus. Source UNVERIFIED |

---

## 3. Container formats and extraction

### 3.1 Sega CD / Mega CD BRAM

- Size is 8 KiB internal, and 64 KiB to 512 KiB for the RAM cart. Blocks are 64 B.
- The last 64 B hold the directory/format block. It contains:
  - volume name `"___________"`
  - free-block count and file count, each written 4 times
  - `"SEGA_CD_ROM\0\x01\0\0\0"`
  - `"RAM_CARTRIDGE___"` (`save-file-converter/frontend/src/util/SegaCd.js`; GPGX `brm_format`)
- GPGX validates only the last 0x20 bytes of that block (`libretro.c:1078, 1148`).
- Block 0 is reserved.
- File data grows upward from block 1.
- Directory entries (16 B plaintext: 11-char name `A-Z0-9_*`, protect/ECC flag, 2 B start block, 2 B size in blocks) grow downward from the second-to-last block, two per block.
- Directory entries are always ECC-encoded. File data is ECC-encoded when the entry's flag is set. The encoding is custom Reed-Solomon plus one 16-bit CRC stored twice, the second copy inverted, so 64 B hold 32 B of payload (superctr/buram, MIT; confirmed by a sigil encoder that reproduces real volumes).
- Reference implementations:
  - superctr/buram (C, reverse-engineered from the BIOS; list, extract, insert, delete)
  - save-file-converter `SegaCd/SegaCd.js`, `ReedSolomon.js`, `Crc16.js`
- Per-game extraction:
  - Save names are game-chosen and there is no product code, so mapping a save to a game needs a name table or heuristics.
  - Extraction and injection are lossless when the entry is copied with its encoded blocks, or decoded and re-encoded with the same flag.
  - Injection must recompute the directory counts and rewrite the 4 redundant copies.
- Byte-expanded variants are UNVERIFIED: no sample exists, and save-file-converter's Sega CD code has no expansion handling (checked 2026-09-28). GPGX, PicoDrive and ares all store collapsed 8 KiB.

### 3.2 Saturn backup RAM

- Internal: 32 KiB collapsed with 64-byte blocks (512 blocks). Carts: 512 KiB with 512-byte blocks (1024 blocks). Everything is big-endian.
- Block 0 is `"BackUpRam Format"` repeated. Mednafen fills the whole block, the BIOS writes 0x40 bytes. Block 1 is all 0x00.
- Archive (first) block layout:
  - `0x00` 0x80000000
  - `0x04` name, 11 B ASCII
  - `0x0F` language
  - `0x10` comment, 10 B Shift-JIS
  - `0x1A` date (u32 minutes since 1980-01-01)
  - `0x1E` data size in bytes
  - `0x22` list of u16 block numbers, terminated by 0x0000, which can continue into listed data blocks, then data
- Data blocks start with 0x00000000.
- There is no central directory, so a reader must scan every block. Source: save-file-converter `SegaSaturn/SegaSaturn.js` header comment, citing the SAROO docs and segaxtreme.
- Emulator on-disk variants:
  - Mednafen/Beetle/Kronos use collapsed raw.
  - Standalone Mednafen gzips cart saves.
  - Yabause uses 64 KiB expanded (0xFF filler).
  - Yaba Sanshiro uses 8 MiB expanded, one volume shared by all games.
- `.BUP` is the de facto single-save interchange format (Pseudo Saturn Kai / slinga-homebrew Save-Game-BUP-Scripts, `bup_header.h`). It is a 64-byte header followed by raw save data:
  - `"Vmem"` magic, save id, stats
  - name (12)
  - comment (11)
  - language
  - date (last saved)
  - size in bytes
  - size in blocks (0 in 1 of the 10 samples checked)
  - date 2
- `.BUP` is independent of block size, so it moves between internal and cart volumes.
- Tools:
  - save-file-converter (`SegaSaturn/IndividualSaves/Bup.js`, extract/combine for mednafen, yabause and yabasanshiro)
  - slinga-homebrew Save-Game-BUP-Scripts (Python)
  - Saroo tooling
- Lossless for what a volume keeps: name, language, comment, date and data round-trip, and block placement changes on re-injection, which carries no meaning. A .BUP header also carries fields a volume doesn't: the save id, statistics, the name's 12th byte, the second date and the block-count field. Those don't survive a trip through a volume; 2 of 10 sample .BUPs round-trip byte for byte, the rest on every field a volume keeps.
- Save names are usually a game prefix plus a suffix (for example `GRANDIA_001`). There is no product code, so game mapping is heuristic. Multiple saves per game are common.

### 3.2.1 Mapping Saturn and Sega CD saves to games

- No public table maps save names to product codes.
  - bucanero/saturn-save-tools `docs/` (GPLv3) lists save names for 84 games, with title and region but no product code: https://github.com/bucanero/saturn-save-tools/tree/master/docs
  - The PPCenter Pseudo Saturn Kai archive and the SegaXtreme save collection are unindexed uploads.
  - No Sega CD name list was found in superctr/buram, save-file-converter, GPGX issues or Zophar's `.brm` dumps.
- No tool guesses ownership. SAROO keys each save slot by a 16-byte game id captured when the game writes (https://github.com/tpunix/SAROO/issues/232). save-file-converter makes the user pick the game's track 1 and reads the id from it (`ConvertSegaSaturnSaroo.vue` at `0a9786f`).
- Names often don't resemble titles. `PANDRA_3_01` is Panzer Dragoon Saga, `SFORCE31_01` is Shining Force III, `TGKRPLY_RP1` is Touge King, `CLOCK__DATA` is Clockwork Knight. Bio Hazard writes both `BIO_DATA_01` and `BIODATA-01`. The JP and US Virtua Fighter 2 share `VFIGHTER2_X`. Fuzzy title matching is not viable.
- Saturn discs carry their save name prefix as a plain string. Six of six discs in the local corpus did (`ALBERT_G`, `B_RANGERS`, `GUARDIAN_HE`, `HOLY_ARK`, `CROCFISAVE`, `PANDRA_ZWEI`). Panzer Dragoon Zwei also contains Saga's `PANDRA_3_01`, so a string on a disc names saves the game reads as well as ones it writes. Sega CD track 1 scans found plausible names mixed with IP header strings and noise (UNVERIFIED as a method).
- The Saturn entry date depends on the emulator's RTC.
  - Beetle Saturn with `autortc` (default) sets the RTC from host time at game load, then advances it on emulated time (`mednafen/ss/ss.c:2063-2086`, `smpc.c:1111` at `65f05fa`).
  - Kronos libretro starts from host time at init and adds emulated frame time (`smpc.c:296-298` at `3791ffb`).
  - In both, pause, fast-forward and savestate loads push the RTC away from wall time.
  - Yaba Sanshiro reads live host time on every INTBACK (`smpc.c:271-274` at `09ed8e5`), so its dates track wall time. It is also the emulator with one shared `backup.bin`.
  - The game chooses the date it stores and may write 0 (UNVERIFIED).

### 3.2.2 Finding the names a disc writes

Tested 2026-09-28 on 45 Saturn discs (39 with ground truth from bucanero's list) and 65 Sega CD discs from the local corpus.

- Saturn `BupDir` is `filename[12]`, `comment[11]`, `language`, u32 `date`, u32 `datasize`, u16 `blocksize`. `BUP_Write(device, BupDir*, data, wmode)` goes through vector slot +16. `BUP_Read`, `BUP_Delete`, `BUP_Dir` and `BUP_Verify` (+20, +24, +28, +32) take a bare name (https://github.com/johannes-fetz/joengine/blob/cd84f41e5bb95b7711b872d32eee60482b6d9454/Compiler/COMMON/SGL_302j/INC/SEGA_BUP.H L73-80, L111-131).
- Sega CD `BRMWRITE` takes `name[11]`, a mode byte (0x00, or 0xFF protected) and a u16 block count. `BRMSERCH`, `BRMREAD` and `BRMDEL` take a name only (https://github.com/drojaazu/megadev/blob/6e4f822182045a00bf337fd2b3b6f14d9497bdb6/lib/sub/bram.def.h).
- Games rarely keep a filled `BupDir` on disc. Only After Burner II did. Most fill it at runtime from adjacent name and comment literals, so matching the record's shape finds almost nothing.
- Following SH-2 code references from each name to the BUP call it reaches does separate writes from reads. A name that reaches Write, Verify or Delete is owned. One that reaches only Read or Dir is referenced.
  - Zwei's `PANDRA_3_01` reaches only `BUP_Dir` and a compare. Burning Rangers' `NIGHTS___01` and Touring Car's `SEGARALLY_0` are referenced only. No foreign name came out owned (0 of 3).
  - The right owned name was found for 17 of 39 games (44%). The name appears in some BUP context for 24 of 39 (62%), and a plain string search finds it on about 31 of 39.
  - Failures:
    - Names built with `sprintf` (`CROCFISAVE%d`, `ROADRASH%03d`, `SFORCE3X`).
    - Overlays whose load address couldn't be inferred.
    - Writes done from a struct in a different function.
    - Comment literals taken as names.
    - XBAND middleware names on NetLink discs.
    - US and JP discs using different names (Resident Evil `BIOUDATA_`, Hang-On GP `_02` against the JP `_01`).
  - Sega CD, with no ground truth: the write shape plus 68000 references to `BRMWRITE` point at plausible names (`LODOSS_SAVE`, `DUN_EXPL_00`, `MORT_KOMBAT`). The shape alone also matches file tables, header strings and graphics. Three Wolf Team discs share one library name, `AISLE_LORD_`.

### 3.3 Dreamcast VMU

- The image is 128 KiB, 256 blocks of 512 B, little-endian. A standard VMU has this layout, but the root block records it, and readers must take it from there:
  - Blocks 0-199: user area. Real images also record 240 and 241 user blocks (root offset 0x50).
  - Blocks 200-240: unused on a standard VMU
  - Blocks 241-253: directory (13 blocks, 32-byte entries, 208 slots). The root records the directory's top block and it runs down from there; some tools record the lowest block and write upwards (the jsr-forward-dir-vmu sample).
  - Block 254: FAT (u16 per block; 0xFFFC free, 0xFFFA end of chain)
  - Block 255: system/root block, starting with sixteen 0x55 bytes
- Directory entry layout:
  - `0x00` type (0x33 data, 0xCC game, 0x00 empty)
  - `0x01` copy-protect (0xFF protected)
  - `0x02` u16 first block
  - `0x04` filename, 12 B Shift-JIS
  - `0x10` BCD timestamp, 8 B
  - `0x18` u16 size in blocks
  - `0x1A` u16 header block offset
  - `0x1C` 4 B unused
- The VMS file header inside the data holds: description (16 B + 32 B), creator, icon count, animation speed, eyecatch type, CRC, data size, palette and icons.
- Sources: mc.pp.se/dc/vms/flashmem.html; save-file-converter `Dreamcast/Components/*.js`.
- Per-save formats:
  - **VMS** is the raw file bytes (chain concatenated). It is paired with **VMI**, 108 B of metadata: checksum = first 4 bytes of the resource name AND "SEGA" (confirmed against real VMI files, 2026-09-28), description 32, copyright 32, timestamp 8, version, file number, resource name 8 (= the VMS base name), VMU filename 12, file mode (bit 1 game, bit 0 copy-protect), size.
  - **DCI** (Nexus) is the 32-byte directory entry followed by data, with every 4-byte word byte-swapped.
- Tools:
  - save-file-converter (`Dreamcast/IndividualSaves/VmiVms.js`, `Dci.js`)
  - bucanero/dc-save-converter (C, `vmufs.h`)
  - gyrovorbis/libevmu
  - DreamShell `vmu_manager`
  - VMU Explorer (Windows)
- Lossless: yes for data files. The directory entry fields round-trip through DCI exactly, and through VMI+VMS apart from the header-block offset, which VMI encodes as the game/data flag.
- Game-type files (VMU minigames, 0xCC) must start at block 0 and be contiguous, and only one can exist per VMU.
- The VMU filename (12 chars) is game-chosen, often matching the product code prefix (for example `SONICADV_SYS`). It is not guaranteed.

---

## 4. Notes for save sync

- Per-game raw files, safe to sync as opaque blobs:
  - GPGX / PicoDrive MD, SMS, GG `.srm`
  - PicoDrive CD `.srm`
  - GPGX CD per-game `.brm`
  - Beetle `.srm` / `.bkr`
  - Kronos `.ram` / `.bkr`
  - Flycast per-content VMU
  - Standalone Flycast A1
- Shared containers that need filesystem-level extraction/injection to sync per game:
  - GPGX default `scd_{U,E,J}.brm` and `*_cart.brm`
  - ares `backup.ram`
  - Beetle shared `.bkr` / `.bcr`
  - Yaba Sanshiro `backup.bin`
  - Flycast default `vmu_save_*.bin`
  - Redream `vmu0-3.bin`
- Cross-emulator Saturn internal BRAM uses one byte layout, 32 KiB collapsed (Beetle `.srm`/`.bkr`, Mednafen `.bkr`, Kronos `.ram`/`.bkr`). The yabause 64 KiB expanded file collapses losslessly to it.
- `.smpc` is not game data (clock plus BIOS language/settings). Treat it as device-local or skip it.
- Beetle `.bcr` is per-game raw 512 KiB. Standalone Mednafen `.bcr` is gzip of the same bytes.
- GPGX MD `.srm` length varies because trailing 0xFF is trimmed. Hashing for change detection should normalize (pad to 64 KiB with 0xFF) or accept length drift.
- Flycast libretro renames and deletes legacy `{stem}.A1.bin` in favour of `{gameId}.A1.bin`. A sync client watching the old name will see it disappear.
