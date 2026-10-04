# Saturn

Status: synced
sigil collects and restores the backup RAM volumes of Beetle Saturn, Kronos, yabause and Yaba Sanshiro, internal memory and cart, per game and shared.

## Identification

sigil has no Saturn extractor. Build the result from the stored `title_id`
and `features` as for Sega CD (see [Identification](../identification.md)).
Layout rows take `saturn` as the platform slug.

## Save layouts

| Layout | Files (role, option) | Shared | Source |
|---|---|---|---|
| `mednafen_saturn` | `{stem}.srm` primary when `beetle_saturn_save_method` = `libretro` (default); `{stem}.bkr` primary when `mednafen` and `beetle_saturn_shared_int` = `disabled` (default); `{stem}.bcr` sidecar when `beetle_saturn_shared_ext` = `disabled` (default); `{stem}.smpc` sidecar when `shared_int` = `disabled` | `mednafen_saturn_libretro_shared.bkr` when `shared_int` = `enabled` and `save_method` = `mednafen`; `.smpc` when `shared_int` = `enabled`; `.bcr` when `shared_ext` = `enabled`. A new `.bkr` is 32 KiB and a new `.bcr` a 512 KiB cart; restore refuses with `SIGIL_ERR_NO_SPACE` saves that don't fit | `mednafen/ss/ss.c`, `libretro.c`; sega.md section 1 |
| `kronos` | `kronos/saturn/{stem}.ram` primary when `kronos_use_beetle_saves` = `disabled` (default); `{stem}.bkr` primary when `enabled`; `kronos/saturn/{stem}-ext512K.ram`, `-ext1M.ram`, `-ext2M.ram` or `-ext4M.ram` sidecar by `kronos_addon_cartridge` (`512K_backup_ram` default, `1M_`, `2M_`, `4M_backup_ram`); `{stem}.bcr` sidecar when `kronos_use_beetle_saves` = `enabled`, always a 512 KiB cart. Mode follows the content, not an option: a disc or `.m3u` is Saturn; an ST-V romset writes `kronos/stv/`, which has no row yet | | libretro/yabause `kronos` `libretro.c` `configure_saturn_addon_cart`; subdir `kronos/saturn` |
| `yabause` | `{stem}.srm` primary, 64 KiB byte-expanded | | `libretro.c` (master); the core writes the file itself |
| `yabasanshiro` | | `yabasanshiro/backup.bin`, one 8 MiB byte-expanded volume for every game | libretro/yabause `yabasanshiro` `libretro.c`; subdir `yabasanshiro` |

A cart file restore creates is that size (16 KiB for `128k` to
512 KiB for `4meg`), as is a Kronos cart for `kronos_addon_cartridge`,
whatever size the unit's cart was; saves that don't fit return
`SIGIL_ERR_NO_SPACE`.

The internal save's file depends on the Beetle Saturn build, and with no
option sent, `mednafen_saturn` takes the current one:

| Build | Internal save | Send |
|---|---|---|
| 2026-05-26 on | `{stem}.srm`, or `{stem}.bkr` with `beetle_saturn_save_method` = `mednafen` | nothing, or the user's value |
| 2026-05-25 | `{stem}.bkr` unless the option says `libretro` | `beetle_saturn_save_method` = `mednafen` |
| 2026-05-11 to 05-24 | both, the same bytes; RetroArch loads `.srm` over `.bkr` | nothing |
| before 2026-05-11 | `{stem}.bkr` alone; the option doesn't exist | `beetle_saturn_save_method` = `mednafen` |

sigil reads only the request, never the core, so the value works on
builds that predate the option. A collect or restore with no options on
a root holding `{stem}.bkr` and no `{stem}.srm` reports the `.bkr` in
`alternates` with that value ([alternates](../save-units.md#alternates)).
The four builds are from beetle-saturn-libretro `a29a316f` (save RAM first
given to the frontend), `a0c1c52f` (the option, default `mednafen`) and
`0977d2c4` (default `libretro`).

## Sync

The generic collect and restore rules, holding units, claims and state
are in [Sync](../sync.md).

`sigil_card_list` reads Saturn backup RAM (`SIGIL_CARD_FORMAT_SATURN_BACKUP`,
`.bkr`, `.bcr`, `.srm` or `backup.bin`, internal or cart).

On Saturn and Sega CD it is the game's internal
volume (`backup.ram`) when it has internal saves alone, else a zip named
`<stem>.zip` holding `backup.ram` and `cart.ram` as present. The member name, not the
size, says which device a volume is, since a 4 MiB Saturn volume can be
Yaba Sanshiro's internal memory or a 32 Mbit cart. Every
volume in a unit is raw, whatever form the emulator stores it in; restore
writes each file back in the emulator's form (gzip, byte expansion), and a
file the emulator hasn't created yet in the form and size its layout row
names. Every Saturn core but Yaba Sanshiro keeps 32 KiB of internal memory,
so a unit whose internal saves need more returns `SIGIL_ERR_NO_SPACE` there
with the blocks they lack.

Saturn, Sega CD and Dreamcast saves carry no game id. Every save on a
per-game volume (Beetle Saturn's `<stem>.srm` and `.bcr`, genesis_plus_gx
with `system_bram` = `per game`, flycast's per-game VMUs) is the game's. On
a shared volume (genesis_plus_gx's default `scd_U.brm`, Beetle's shared
volumes, Yaba Sanshiro's `backup.bin`, flycast's `vmu_save_A1.bin`) a save
belongs to the game the user claimed it for, else to the game the state
learned it belongs to, else, in managed mode, to the game the volume was
last swapped in for, else to the game the save-name table gives it by one
of the ids in `title_id` or `game_ids` (`src/save_names.c`; Saturn and
Sega CD product codes as the disc header spells them, Dreamcast product
numbers). The rest come back in `holding`, which the client
keeps where the user can claim them; `holding` and the unit both go up
before the state is stored.

`holding` (collect, Saturn and Sega CD) is a zip of the saves on a shared
volume with no known owner, and `unowned` names them. `claimed` takes the
names from `unowned` the user gave this game.

In managed mode, restore swaps each shared volume for one holding only the
game's saves, keeping the file's form (gzip, byte expansion). It refuses
with `SIGIL_ERR_UNCOLLECTED` while the volume holds a save that isn't in
the last holding unit or doesn't match its game's last collect: call
collect for the game that ran last, upload, then restore again. In
unmanaged mode, restore never swaps. It replaces only the game's saves,
and only when the volume is as the last collect saw it; a collect that
then finds the old saves back sets `restore_again`.

A managed restore also refuses a shared volume holding any
corrupt save, which the swap would drop; unmanaged keeps it in place.
A volume file sigil can't read as what its path holds (no card magic, cut
short, an internal volume where a cart goes) is damaged. Restore returns
`SIGIL_ERR_NO_TARGET` for a `cart.ram` when the core's settings keep no
cart file.

## Emulator research

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
  - SAVE_RAM was first exposed by the 2026-05-11 "audit: omnibus fix pass" (`a29a316f`): `libretro.cpp` names `RETRO_MEMORY_SAVE_RAM` there and not in its parent.
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

### Per (platform, emulator) rows

Legend for Format:

- raw = memory dump with no header
- expanded = each data byte sits on an odd address with a filler byte (0xFF or 0x00) before it (2x size)
- container = a filesystem holding many games' saves

| Platform | Emulator | Files + naming | Format | Scope + switching options | Per-game extraction / neutral form | Source |
|---|---|---|---|---|---|---|
| Saturn | Beetle Saturn (libretro), save_method=libretro (default since 2026-05-26) | Internal: `{stem}.srm` (frontend). RTC/SMEM: `{stem}.smpc` (core). Cart: `{stem}.bcr` (core) | `.srm` is raw 32768 B collapsed BRAM (64-byte blocks, 512 blocks). `.smpc` is 12 B. `.bcr` is raw 0x80000 (512 KiB) collapsed, 512-byte blocks, uncompressed | `.srm` is always per-game. `.smpc` is shared iff `beetle_saturn_shared_int`. `.bcr` is shared iff `beetle_saturn_shared_ext`. Cart presence comes from the internal DB (`ss.cart` auto, default "backup") | Feasible and lossless (section 3.2). Neutral form: `.BUP` (Vmem header + data) per save | `libretro.c:407-437, 1891-1910, 2197-2215`; `mednafen/ss/ss.c:158, 2159-2270, 2289-2440`; `mednafen/ss/cart/backup.c:36, 69-76`; `mednafen/ss/smpc.c:413-467`; git `a0c1c52f`, `0977d2c4` |
| Saturn | Beetle Saturn (libretro), save_method=mednafen | Internal: `{stem}.bkr` or `mednafen_saturn_libretro_shared.bkr`. `.smpc` and `.bcr` as above | `.bkr` is byte-identical to the `.srm` above | `.bkr` and `.smpc` follow `shared_int`. `.bcr` follows `shared_ext`. No `.srm` is exposed | As above | same |
| Saturn | Mednafen (standalone) | `{path_sav}/{stem}.bkr`, `.smpc`, `.bcr`. `filesys.fname_sav` defaults to `%f.%M%x`, where `%M` is empty first and becomes `{md5}.` only on a collision. Rotating backups via `MDFN_BackupSavFile(10, "bkr")` | `.bkr` is raw 32 KiB (same as Beetle). `.smpc` is 12 B (same). **`.bcr` is gzip-compressed** (`GZFileStream` WRITE, `ss.cpp:1952`). Its read goes through GZFileStream, which (via zlib) presumably also accepts raw input (UNVERIFIED) | Per-game. There is no shared mode | As Beetle. Moving `.bcr` from standalone to Beetle needs a gunzip: Beetle reads raw (`filestream_read`, `ss.c:2250-2310`) | `mednafen-1.32.1/src/ss/ss.cpp:1878-1996`; `src/ss/smpc.cpp:377-382`; mednafen.github.io/documentation/fname_format.txt |
| Saturn | Kronos (libretro, buildbot = libretro/yabause `kronos`) | Default: `{savedir}/kronos/saturn/{stem}.ram` (internal) and `{savedir}/kronos/saturn/{stem}-ext{512K,1M,2M,4M}.ram` (backup cart). With `kronos_use_beetle_saves` = enabled: `{savedir}/{stem}.bkr` and `{savedir}/{stem}.bcr` (forces a 4 Mbit cart) | Internal: `extend_backup=0` in libretro, so 0x8000 collapsed (`addr>>1` on odd addresses), the same byte layout as Beetle. Cart: 0x80000 etc., collapsed. A size mismatch at load triggers a reformat (`BackupInit`) | Per-game. No shared mode. `kronos_addon_cartridge` default `512K_backup_ram` | Feasible. Note that "share with beetle" targets `.bkr`, which Beetle only uses in save_method=mednafen. With Beetle's default libretro mode the files no longer meet | `yabause/src/libretro/libretro.c:916-923, 1299-1317, 1587, 1709-1712`; `sys/memory/src/memory.c:79-83, 507-570, 1283-1325`; `sys/memory/src/cs0.c:921-927, 1393-1411`; `libretro_core_options.h:82-107` |
| Saturn | Kronos (standalone) | UNVERIFIED path (Linux port default `./bup.ram`, `port/linux/main.c:194`) | Standard 0x8000 collapsed, or extended 0x800000 when `extend_backup` is set | UNVERIFIED | As above | `sys/memory/src/memory.c:1283-1325` |
| Saturn | yabause (libretro, master) | `{savedir}/{stem}.srm`, but **the core writes it**. `retro_get_memory_*` returns NULL/0 | 0x10000 (64 KiB) expanded: data on odd bytes, 0xFF on even bytes (`FormatBackupRam` writes the `0xFF,'B',0xFF,'a'...` header) | Per-game | Feasible. Collapse by taking odd bytes, which gives the Beetle/Kronos 32 KiB format, lossless both ways. No backup cart option (only 1M/4M RAM carts) | `yabause/src/libretro/libretro.c:698-708, 1059, 1116, 1176-1184`; `yabause/src/yabause.c:200-208, 446-477`; `yabause/src/memory.c:331-357, 1274-1296` |
| Saturn | Yaba Sanshiro (libretro, branch `yabasanshiro`) | `{savedir}/yabasanshiro/backup.bin`, **one file for every game** | Extended internal backup, memory-mapped. save-file-converter reports 0x800000 B expanded (0x400000 collapsed) with 64-byte blocks. `tweak_backup_file_size` value UNVERIFIED | Shared container, no option | Feasible via the BRAM filesystem (same directory structure, larger volume). This is the only way to get per-game data | `libretro/yabause@yabasanshiro: yabause/src/libretro/libretro.c:1000-1004, 1077`; `yabause/src/yabause.c:220-258`; `save-file-converter/.../SegaSaturn/Emulators/yabasanshiro.js` |
| Saturn | Yaba Sanshiro (standalone Android) | UNVERIFIED | Same expanded extended format (per save-file-converter) | Shared | As above | UNVERIFIED |

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

### Notes for save sync

- Per-game raw files, safe to sync as opaque blobs:
  - Beetle `.srm` / `.bkr`
  - Kronos `.ram` / `.bkr`
- Shared containers that need filesystem-level extraction/injection to sync per game:
  - Beetle shared `.bkr` / `.bcr`
  - Yaba Sanshiro `backup.bin`
- Cross-emulator Saturn internal BRAM uses one byte layout, 32 KiB collapsed (Beetle `.srm`/`.bkr`, Mednafen `.bkr`, Kronos `.ram`/`.bkr`). The yabause 64 KiB expanded file collapses losslessly to it.
- `.smpc` is not game data (clock plus BIOS language/settings). Treat it as device-local or skip it.
- Beetle `.bcr` is per-game raw 512 KiB. Standalone Mednafen `.bcr` is gzip of the same bytes.

## Open items

- Disc scan for save names (2026-09-28): owned name found for about 44% of 39 Saturn discs, 0 foreign names misattributed; Sega CD unmeasured. The scan's per-game names are in the session transcript, not in the repo.
- ST-V under Kronos (`kronos/stv/{stem}.ram`) is not a Saturn volume and has no row.
