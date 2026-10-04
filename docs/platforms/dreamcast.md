# Dreamcast

Status: synced
sigil reads the product number from a Dreamcast disc and collects and restores flycast's VMUs, per game and shared.

## Identification

| Slug | Platform | Inputs | `title_id` example | `usage` | Status |
|---|---|---|---|---|---|
| `dreamcast` | Dreamcast | `.chd`, `.iso`, data track `.bin` (`.gdi` track 3) | `T-8111N` (IP.BIN product number) | file-prefix | experimental |

Argosy's shorter identifier `dc` resolves as an alias of `dreamcast`, for
extraction and for layout rows. The extractor is flagged `experimental`
(see [Identification](../identification.md)).

### IP.BIN product number, found by scanning

The boot
header IP.BIN starts the data track: `SEGA SEGAKATANA ` at offset 0,
then a 10-byte ASCII product number at 0x40 (`T-8111N`, `MK-51035`,
`HDR-0038`) padded with trailing spaces. Flycast trims that padding and
then truncates at the first NUL, because some discs leave garbage after
the terminator; sigil reproduces that order exactly, since the result is
the name flycast gives the per-game VMU file. Composing the filename
(the `.A1.bin` port suffix) is the consumer's job, and because a game
can own more than one port's VMU the `usage` is `file-prefix`.

A GD-ROM keeps its data track third and a CHD packs tracks contiguously
from frame 0, so IP.BIN is neither at offset 0 nor at the physical
GD-area LBA 45000. Sigil checks offset 0 first, which covers a raw data
track or a plain ISO, then scans sector boundaries for the magic across
the first 20000 frames. Tracks 1 and 2 live in the single-density area,
which spans the first four minutes of the disc (18000 frames), so the
bound holds for any conformant dump while keeping a miss cheap. A `.gdi`
is a text index naming its track files rather than a disc image, so pass
`track03.bin` (or a CHD) for binary extraction. `.bin` is ambiguous
across platforms and needs an explicit `dreamcast` hint; raw 2352-byte
MODE1 tracks are cooked to 2048 on the way in. `.gdi` and `.cdi` sniff
as Dreamcast so the platform resolves without a hint, but neither
container's own layout is parsed: a `.cdi` only extracts when its
sectors happen to land on 2048-byte boundaries.

## Save layouts

| Layout | Files (role, option) | Shared | Source |
|---|---|---|---|
| `flycast` | `{dc_vmu_id}.A1.bin` primary when `reicast_per_content_vmus` = `VMU A1` or `All VMUs`, else the legacy `{stem}.A1.bin` when only that exists; `{dc_vmu_id}.{A2..D2}.bin` (or legacy `{stem}.{port}.bin`) sidecars when `All VMUs` | `vmu_save_{A1..D2}.bin` when `disabled` (default). The core keeps them in `<system>/dc/`, so pass that folder as the save root. Under `VMU A1` the other ports stay there too and don't sync | flyinghead/flycast `shell/libretro/oslib.cpp` `getVmuPath` |
| `flycast_standalone` | `{dc_vmu_id}_vmu_save_A1.bin` primary, or the legacy `{stem}_vmu_save_A1.bin`, when `PerGameVmu` = `yes` (default) | `vmu_save_A1.bin` when `PerGameVmu` = `no`; `vmu_save_{A2..D2}.bin` | flycast `core/oslib/oslib.cpp`; the save root is the VMU folder |

`{dc_vmu_id}` is the Dreamcast product number (`title_id`) with each of
` /\:*?|<>` replaced by `_`, as flycast names a per-game VMU.

## Sync

The generic collect and restore rules, holding units, claims and state
are in [Sync](../sync.md).

`sigil_card_list` reads Dreamcast VMUs (`SIGIL_CARD_FORMAT_DREAMCAST_VMU`,
`.bin` or `.vmu`, a 128 KiB VMU flash image).

On Dreamcast
it is the game's VMU A1 (`vmu_A1.bin`) when it has saves there alone, else
a zip of `vmu_A1.bin` to `vmu_D2.bin` as present. Every
volume in a unit is raw, whatever form the emulator stores it in; restore
writes each file back in the emulator's form, and a
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

`claimed` takes the names from `unowned` the user gave this game.

In managed mode, restore swaps each shared volume for one holding only the
game's saves. It refuses
with `SIGIL_ERR_UNCOLLECTED` while the volume holds a save that isn't in
the last holding unit or doesn't match its game's last collect: call
collect for the game that ran last, upload, then restore again. In
unmanaged mode, restore never swaps. It replaces only the game's saves,
and only when the volume is as the last collect saw it; a collect that
then finds the old saves back sets `restore_again`.

A managed restore also refuses a shared volume holding any
corrupt save, which the swap would drop; unmanaged keeps it in place.
Restore returns `SIGIL_ERR_NO_TARGET` for a VMU port flycast doesn't keep
per game. On `SIGIL_ERR_NO_SPACE`, `blocks_short` is 0 when other saves
hold the blocks a Dreamcast game file must start at.

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

### Per (platform, emulator) rows

Legend for Format:

- raw = memory dump with no header
- expanded = each data byte sits on an odd address with a filler byte (0xFF or 0x00) before it (2x size)
- container = a filesystem holding many games' saves

| Platform | Emulator | Files + naming | Format | Scope + switching options | Per-game extraction / neutral form | Source |
|---|---|---|---|---|---|---|
| Dreamcast | flycast (libretro, from flyinghead/flycast) | `reicast_per_content_vmus` = disabled (default): `{system}/dc/vmu_save_{A1..D2}.bin`, shared. "VMU A1": `{savedir}/{gameId}.A1.bin` for port A1 only. "All VMUs": `{savedir}/{gameId}.{A1..D2}.bin`. `gameId` is the IP.BIN product number with trailing whitespace trimmed and ` /\:*?\|<>` replaced by `_`. Legacy per-content name `{stem}.{port}.bin`: when found, the core **copies it to the gameId name and deletes the old file** (since commit `5fc84acd`, 2024-11-03). No `.srm` (no SAVE_RAM) | Raw VMU flash image, exactly 131072 B (`u8 flash_data[128_KB]`). An all-zero file is reformatted on load | Shared by default. Per-game via the option. Multi-disc games share one VMU through the product number | Feasible and lossless (section 3.3). Neutral form: VMS+VMI pair, or DCI | `shell/libretro/oslib.cpp:40-68`; `shell/libretro/libretro.cpp:843-860, 2242-2262`; `shell/libretro/libretro_core_options.h:1165-1178`; `core/emulator.cpp:858`; `core/hw/maple/maple_devs.cpp:353, 437-475` |
| Dreamcast | flycast (standalone) | `PerGameVmu` (default **true**) gives A1 = `{gameId}_vmu_save_A1.bin`. Others (and A1 when the option is off) are `vmu_save_{port}.bin`. Looked up in `VMUPath` if set, otherwise the writable data dir. Legacy fallback `{content fileName}_vmu_save_A1.bin` | Raw 128 KiB | A1 per-game by default. Other ports shared | As above | `core/oslib/oslib.cpp:50-100`; `core/cfg/option.cpp:234`; `core/stdclass.cpp:140-143` |
| Dreamcast | redream (standalone, closed source; the libretro core is abandoned) | `vmu0.bin` to `vmu3.bin` (ports A to D) in the redream data dir (libretro docs: in the save dir) | Raw 128 KiB VMU image (UNVERIFIED from source) | Shared across all games. No per-game option (LaunchBox plugins swap `vmu0.bin`) | Feasible via the VMU filesystem | docs.libretro.com/library/redream; forums.launchbox-app.com/files/file/5337-redream-per-game-vmus. Source UNVERIFIED |

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

### Notes for save sync

- Per-game raw files, safe to sync as opaque blobs:
  - Flycast per-content VMU
  - Standalone Flycast A1
- Shared containers that need filesystem-level extraction/injection to sync per game:
  - Flycast default `vmu_save_*.bin`
  - Redream `vmu0-3.bin`
- Flycast libretro renames and deletes legacy `{stem}.A1.bin` in favour of `{gameId}.A1.bin`. A sync client watching the old name will see it disappear.

## Open items

- The extractor is `experimental`: it hasn't been validated against real-world samples.
- A `.gdi` or `.cdi` container's own layout is not parsed.
- redream has no layout row.
