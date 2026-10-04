# Sega CD / Mega CD

Status: synced
sigil collects and restores genesis_plus_gx's backup RAM volumes, per game and shared per BIOS region, with the internal BRAM and the RAM cart.

## Identification

sigil has no Sega CD extractor. Rebuild a result later, or build for a
platform sigil cannot extract (Sega CD returns
`SIGIL_ERR_UNKNOWN_PLATFORM`), by filling a zeroed result with the stored
`title_id`, `save_id` and `features` (see [Identification](../identification.md)).

Row platforms use the slugs in the platform table plus `segacd` and
`fds`. Callers may pass their own forms: `scd`, `sega_cd`, `sega-cd`,
`mega_cd`, `mega-cd`, `megacd` resolve to `segacd`.

## Save layouts

| Layout | Files (role, option) | Shared | Source |
|---|---|---|---|
| `genesis_plus_gx` | `{stem}.brm` primary when `genesis_plus_gx_system_bram` = `per game`; `{stem}_{cart_size}_cart.brm` sidecar when `genesis_plus_gx_cart_bram` = `per game`. No `.srm`: the core writes none for a disc | `scd_E.brm`, `scd_U.brm`, `scd_J.brm` when `system_bram` = `per bios` (default), one per disc region; `{cart_size}_cart.brm` when `cart_bram` = `per cart` (default) | `libretro/libretro.c` `check_variables`, `bram_load`, `bram_save` |

`{cart_size}` comes from `genesis_plus_gx_cart_size` in the core's own
spelling: `128k` to `128Kbit`, `256k` to `256Kbit`, `512k` to `512Kbit`,
`1meg` to `1Mbit`, `2meg` to `2Mbit`, `4meg` to `4Mbit`, absent to
`4Mbit`. A cart file restore creates is that size (16 KiB for `128k` to
512 KiB for `4meg`), as is a Kronos cart for `kronos_addon_cartridge`,
whatever size the unit's cart was; saves that don't fit return
`SIGIL_ERR_NO_SPACE`.

## Sync

The generic collect and restore rules, holding units, claims and state
are in [Sync](../sync.md).

`sigil_card_list` reads Sega CD backup RAM (`SIGIL_CARD_FORMAT_SEGACD_BRAM`,
`.brm` or `.srm`, internal or cart).

On Saturn and Sega CD it is the game's internal
volume (`backup.ram`) when it has internal saves alone, else a zip named
`<stem>.zip` holding `backup.ram` and `cart.ram` as present. Every
volume in a unit is raw, whatever form the emulator stores it in; restore
writes each file back in the emulator's form (gzip, byte expansion), and a
file the emulator hasn't created yet in the form and size its layout row
names.

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

genesis_plus_gx picks `scd_E`, `scd_U` or `scd_J` by the disc's region.
sigil takes the region from `genesis_plus_gx_region_detect` when it is
forced, else from the content file name's first region tag, such as
`(USA)`, else from the only one of the three files present. Otherwise
collect and restore return `SIGIL_ERR_NOT_FOUND`.

Mapping Sega CD saves to games and finding the names a disc writes are
covered with Saturn, in
[Mapping Saturn and Sega CD saves to games](saturn.md#321-mapping-saturn-and-sega-cd-saves-to-games)
and [Finding the names a disc writes](saturn.md#322-finding-the-names-a-disc-writes).

The Sega CD backup RAM reader (`src/card_segacd.c`) takes its ECC layout
table and format-block bytes from superctr/buram, Copyright (c) 2022 Ian
Karlsson, used under the MIT license; the notice is in
[licenses/buram-MIT.txt](../../licenses/buram-MIT.txt).

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

### genesis_plus_gx and Sega CD `.srm`

Genesis Plus GX writes no `{stem}.srm` for a normal (CD-boot, "Mode 2") Sega CD game. The core does all CD saving itself, in `.brm` files, from `bram_load()` / `bram_save()`.

- `retro_get_memory_data/size(RETRO_MEMORY_SAVE_RAM)` returns `sram.sram` / size only when `sram.on` is set (`libretro/libretro.c:3750-3783`).
- In CD mode `genesis.c:161-173` calls `scd_init()` instead of `md_cart_init()`, so `sram_init()` never runs.
- When the backup RAM cart is enabled, `cd_cart_init()` also runs `memset(&sram, 0, sizeof(T_SRAM))` (`core/cd_hw/cd_cart.c:183-199`). So `sram.on == 0`, the reported size is 0, and RetroArch writes no `.srm`.
- The exception is a real MD cartridge booted alongside the CD (Mode 1, `scd.cartridge.boot`). Then `md_cart_init()` runs (`cd_cart.c:249`), and that cartridge's SRAM goes through `.srm`.
- The CD saves live in:
  - Internal BRAM (8 KiB). The `genesis_plus_gx_system_bram` option picks the file. "per bios" (the default) gives `{savedir}/scd_U.brm`, `scd_E.brm` or `scd_J.brm`, chosen by the disc's region byte. `bram_load` switches on `region_code` (Japan, Europe, USA; any other region loads nothing), and `genesis_plus_gx_region_detect` = `ntsc-u`, `pal` or `ntsc-j` forces it; `auto`, the default, reads the disc (`libretro.c` L1043-1060, L1574-1585, master read 2026-09-29). "per game" gives `{savedir}/{content stem}.brm` (`libretro.c:1385-1403`).
  - Backup RAM cart. `genesis_plus_gx_cart_size` sets the size (default `4meg`, i.e. 512 KiB). `genesis_plus_gx_cart_bram` picks the file. "per cart" (the default) gives `{savedir}/{N}Kbit_cart.brm` / `{N}Mbit_cart.brm`, for example `4Mbit_cart.brm`. "per game" gives `{savedir}/{stem}_{N}Kbit_cart.brm` / `{stem}_{N}Mbit_cart.brm` (`libretro.c:1430-1502`).
- The core reads `.brm` files only in `retro_load_game` (`libretro.c:3674`) and writes them only in `retro_unload_game` (`libretro.c:3729`). A save happens only if the CRC changed and the "SEGA_CD_ROM/RAM_CARTRIDGE" signature is intact (`bram_save`, `libretro.c:1140-1180`). A crash loses the session's BRAM writes.
- The frontend never sees `.brm` files, so RetroArch cloud sync and save-protect do not cover them.
- Oddity (UNVERIFIED at runtime): `cart_size = "disabled"` stores `0xff` (`libretro.c:1411-1412`). `cd_cart_init` then sets `scd.cartridge.id = 0xff`, which is truthy, and computes `1 << (0xff+13)`. "Disabled" may not actually disable the cart.

### Per (platform, emulator) rows

Legend for Format:

- raw = memory dump with no header
- expanded = each data byte sits on an odd address with a filler byte (0xFF or 0x00) before it (2x size)
- container = a filesystem holding many games' saves

| Platform | Emulator | Files + naming | Format | Scope + switching options | Per-game extraction / neutral form | Source |
|---|---|---|---|---|---|---|
| Sega CD / Mega CD | genesis_plus_gx (libretro) | Internal: `scd_{U,E,J}.brm` (per bios, default) or `{stem}.brm` (per game). Cart: `{N}{K,M}bit_cart.brm` (per cart, default) or `{stem}_{N}{K,M}bit_cart.brm`. No `.srm` in CD-boot mode (section 1) | Raw BRAM filesystem. Internal is 8 KiB (0x2000). Cart is 8 KiB << id (128 Kbit to 4 Mbit, default 512 KiB), stored collapsed (odd bytes only). The format block sits in the last 0x40 bytes | Shared by default for both (per region, per cart size). `genesis_plus_gx_system_bram`, `genesis_plus_gx_cart_bram`, `genesis_plus_gx_cart_size`, all restart-required. The core owns the files, not the frontend | Feasible and lossless at the entry level (section 3.1). Neutral form: a freshly formatted 8 KiB BRAM holding only that game's entries, or the entry tuple (11-char name, protect flag, data blocks) | `libretro/libretro.c:1043-1180, 1385-1502, 3674, 3729`; `core/cd_hw/cd_cart.c:180-200`; option text `libretro/libretro_core_options.h:165-210` |
| Sega CD / Mega CD | picodrive (libretro) | `{stem}.srm` (frontend) | Default: raw internal BRAM, 0x2000 (8 KiB). With `picodrive_ramcart` = enabled: 0x12000 bytes, where [0,0x2000) is internal BRAM and [0x2000,0x12000) is a 64 KiB cart, collapsed. The libretro path does not sync `Pico_mcd->bram` into the first 8 KiB ("TODO" at `libretro.c:1715`). The option text warns that enabling it discards internal BRAM | Per-game (the frontend names it by content). No shared mode | Feasible (same BRAM filesystem). Converting to GPGX is a straight rename of the 8 KiB file | `platform/libretro/libretro.c:1710-1752`; `pico/cd/mcd.c:111-121`; `pico/cd/memory.c:677-712`; `platform/libretro/libretro_core_options.h:163-173` |
| Sega CD / Mega CD | picodrive (standalone) | `.brm` via `emu_save_load_game` (naming UNVERIFIED) | 8 KiB internal, or 0x12000 with the RAM cart (bram copied into the first 8 KiB). The non-cart file is not truncated because it "may contain RAM cart data after normal brm" | UNVERIFIED | As above | `platform/common/emu.c:965-1010` |
| Sega CD / Mega CD | ares | `backup.ram` in the **system** pak, so one BRAM for all Mega CD games | Raw 8 KiB (`bram.allocate(8_KiB)`) | Shared (per system/BIOS). No option found | Feasible (BRAM filesystem). Disk path UNVERIFIED | `ares/md/mcd/mcd.cpp:41, 79-80, 114` |
| Sega CD / Mega CD | Kega Fusion, Gens, BlastEm | UNVERIFIED | UNVERIFIED. Whether any of these write byte-expanded Sega CD files is unknown | UNVERIFIED | Same filesystem | `save-file-converter/frontend/src/util/SegaCd.js` |

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

### Notes for save sync

- Per-game raw files, safe to sync as opaque blobs:
  - PicoDrive CD `.srm`
  - GPGX CD per-game `.brm`
- Shared containers that need filesystem-level extraction/injection to sync per game:
  - GPGX default `scd_{U,E,J}.brm` and `*_cart.brm`
  - ares `backup.ram`

## Open items

- Disc scan for save names (2026-09-28): owned name found for about 44% of 39 Saturn discs, 0 foreign names misattributed; Sega CD unmeasured. The scan's per-game names are in the session transcript, not in the repo.
