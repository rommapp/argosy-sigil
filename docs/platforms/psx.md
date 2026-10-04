# PlayStation

Status: synced
sigil reads the disc's serial, finds the memory cards for the layouts below, and syncs the game's saves on them with collect and restore.

## Identification

| Slug | Inputs | `title_id` example | `usage` |
|---|---|---|---|
| `psx` | `.iso`, `.bin`, `.chd` | `SLUS-12345` | file-prefix |

`ps1` and `playstation` resolve as aliases of `psx`, both as a platform
slug and as a layout row platform.

`raw_serial` keeps the ID exactly as it appears in the binary, before any
normalization (`SLUS_123.45`).

## Save layouts

| Layout | Files (role, option) | Shared | Source |
|---|---|---|---|
| `mednafen_psx_hw` | `{stem}.srm` primary when `beetle_psx_hw_use_mednafen_memcard0_method` = `libretro` (default); `{stem}.{left_index}.mcr` primary when `mednafen` and `beetle_psx_hw_shared_memory_cards` = `disabled` (default); `{stem}.{right_index}.mcr` sidecar when `beetle_psx_hw_enable_memcard1` = `enabled` and shared cards are off | `mednafen_psx_libretro_shared.{left_index}.mcr` when `beetle_psx_hw_shared_memory_cards` = `enabled` and the method is `mednafen`; `mednafen_psx_libretro_shared.{right_index}.mcr` when shared cards are on and `beetle_psx_hw_enable_memcard1` = `enabled` | beetle-psx-libretro `5718ab9` `libretro.c` L3096-3098, L3351-3367, L7314-7319 |
| `mednafen_psx` | Same files as `mednafen_psx_hw`, under the software build's `beetle_psx_*` keys (`beetle_psx_use_mednafen_memcard0_method`, `beetle_psx_enable_memcard1`, `beetle_psx_shared_memory_cards`) | Same as `mednafen_psx_hw`, under the same keys | beetle-psx-libretro `5718ab9` `libretro_options.h` L27-31 |
| `pcsx_rearmed` | `{stem}.srm` primary when `pcsx_rearmed_memcard1` = `libretro` (default); `{pcsx_serial}_1.mcd` primary when `pcsx_rearmed_memcard1` = `serial`; `{pcsx_serial}_2.mcd` sidecar when `pcsx_rearmed_memcard2` = `serial` | `pcsx-card1.mcd` when `pcsx_rearmed_memcard1` = `shared`; `pcsx-card2.mcd` when `pcsx_rearmed_memcard2` = `shared` (default) | pcsx_rearmed `ff81ed1` `frontend/libretro.c` L3739-3801, `libpcsxcore/misc.c` L495-507 |
| `vita_pops` | `PSP/SAVEDATA/{disc_id}/SCEVMC0.VMP` primary (slot 1); `PSP/SAVEDATA/{disc_id}/SCEVMC1.VMP` sidecar (slot 2). `{disc_id}` is the title id's letters and digits (`SLUS01040`), the EBOOT's `DISC_ID`. Signed `.vmp` cards; the folder's `PARAM.SFO` and `ICON0.PNG` stay as the console wrote them. POPS won't read a card whose folder has no `PARAM.SFO`, so a restore into a folder without one also writes it: the folder's name, the content's name as its title, signed as the PSP save utility signs it. POPS checks the hash at 0x70, not the console-keyed one at 0x20, and rewrites the file with all three when the game runs | | PS1 classics on a PSP, and on a Vita (official PS1 Classics and Adrenaline): the save root is the folder holding `PSP/` (`ms0:/` or `ux0:pspemu/`); subdir `PSP/SAVEDATA` |

`mednafen_psx_hw` and `mednafen_psx` match any platform; `vita_pops`
matches `psx` only.

`{left_index}` and `{right_index}` are `beetle_psx_hw_memcard_left_index`
(default `0`) and `beetle_psx_hw_memcard_right_index` (default `1`), or
`beetle_psx_memcard_left_index` and `beetle_psx_memcard_right_index` on
`mednafen_psx`. Shared cards rename every `.mcr` card and keep its index.
Under the default libretro method slot 1 is the game's `.srm`, which is
never shared, so turning shared cards on changes only slot 2. Slot 2
exists only with `enable_memcard1` on.
`{disc_id}` is `save_id` with only its letters and digits (`SLUS-01040`
gives `SLUS01040`), as a PSP EBOOT's `DISC_ID` names its save folder;
`title_id` stands in when `save_id` is empty. One EBOOT holds every disc
of a set, and its single `DISC_ID` names one folder for all of them: disc
1's serial for converted sets. A client syncing a later disc passes that
id as `save_id`, with every disc's id in `game_ids` as for any set.

Every row but `vita_pops` was read from the core's source or its libretro
docs page; the names are the core's literals. The PSP and Vita firmware
isn't open, so `vita_pops` was checked on a PS Vita running PS1 games under
Adrenaline: the folder names, the files POPS writes, its `.vmp` signatures
(a new card from sigil is byte-identical to the console's), and that POPS
boots a folder sigil created from nothing.

## Sync

The unit is one per-game card, a raw PS1 card.
Restore writes a PS1 card back in the form it found: a DexDrive `.gme`
keeps its header, with the frame copies following the directory and a
slot's comment kept only beside the save it was written for; a PSP or Vita
`.vmp` keeps its seed and is signed for its new contents. A new card named
`.VMP` (the `vita_pops` layout) is a signed `.vmp`. A `.vmp` whose
signature doesn't match its card, which the console refuses, is damaged.

A game that reads an earlier title's save, as a sequel reads its prequel's,
lists that title in `companions`. Multi-disc sets pass every disc's id in
`game_ids`. The general rules for both are in [sync](../sync.md).

## Emulator research

Research date 2026-09-26. Source links are permalinks to the commit that was HEAD on that date unless marked otherwise.

Pinned commits used below:

- pcsx_rearmed `ff81ed17a15241d2f3730cdd7585b38e172532ca` (PR = `https://github.com/libretro/pcsx_rearmed/blob/ff81ed17a15241d2f3730cdd7585b38e172532ca`)
- beetle-psx-libretro `5718ab9b829599687671503f11494ca6a0049c57` (BP)
- swanstation `b6c30a7b270a3f68ac41f268eafdfa678d17dea2` (SW)
- duckstation `0d8dda34d3785d8a5c9e910b9ef50caa85fcde0c` (DS)

Anything without a source link is marked UNVERIFIED.

### The PS1 memory card, common to every PSX emulator

- A card is 131072 bytes (128 KiB): 16 blocks of 8 KiB. Block 0 is the header and directory (15 x 128-byte directory frames plus a broken-sector list). Blocks 1..15 hold save data.
- One save = one directory frame (128 bytes: state byte 0x51 first / 0x52 middle / 0x53 last, size, next-block link, 20-byte filename like `BASLUS-00594xxxxxxx`) plus 1..15 linked 8 KiB blocks. Region prefix `BA`/`BE`/`BI` then the product code. MemcardRex parses exactly these fields: `https://github.com/ShendoXT/memcardrex/blob/master/MemcardRex.Core/ps1card.cs` (slot types L570-652, region prefixes L700-708).
- Every emulator below writes this raw 128 KiB layout. `.srm`, `.mcr`, `.mcd`, `.mc`, `.ddf`, `.psm`, `.ps` are all raw 128 KiB under different names. DuckStation's importer treats them identically: DS `src/core/memory_card_image.cpp` L636-641.
- Checked on device 2026-10-01 against a real card (sample `thps2-duckstation-mcd`, DuckStation Android `0.1-8969-g611bb8fb4`). The header, directory frames, broken-sector list (frames 16-35), reserved frames and free-block fill are byte-identical to the pcsx_rearmed `blank-card` sample and to `sigil_ps1_format`. The only frame that differs between emulators is frame 63, the write-test frame: DuckStation and pcsx_rearmed copy the header there, the retroarch `ctr-srm` sample leaves it zeroed, and a RomM-stored THPS2 card holds `03 00 00 00` there. Games never read it. Two cards with the same saves therefore differ by whole-file MD5 across emulators. `sigil_collect` rebuilds a PS1 unit on a freshly formatted card, so the unit's hash does not carry the difference. A client hashing the raw file does.

Wrapped card formats (for import/export, not written by the emulators below):

| Format | Origin | Layout | Source |
|---|---|---|---|
| `.gme` | DexDrive | 3904-byte (0xF40) header + 128 KiB raw | DS memory_card_image.cpp L525-579 (`static_assert(sizeof(GMEHeader) == 0xF40)`), MemcardRex ps1card.cs L384 |
| `.vmp` | PSP / Vita PS1 classics | 0x80 header (`\0PMV`, salt at 0x0C, HMAC-SHA1 at 0x20) + 128 KiB raw = 0x20080 bytes. Signed, so a rewrite needs re-signing | MemcardRex ps1card.cs L491-509 |
| `.mem` / `.vgs` | Connectix VGS | 64-byte `VgsM` header + 128 KiB | DS memory_card_image.cpp L581-602 |
| `.psx` (card) | Xploder / pSX | 256-byte `PSV` header + 128 KiB (DuckStation's reading) | DS memory_card_image.cpp L604-625 |
| MCX PocketStation `.bin` (Vita) | Vita | 0x80 bytes then raw card | MemcardRex ps1card.cs L468-481 |

Single-save (per-game) formats:

| Format | Layout | Source |
|---|---|---|
| `.mcs` (PSXGameEdit) | 128-byte directory frame + N x 8192 data blocks. This is the neutral per-save form: exactly what one save occupies on a card | DS memory_card_image.cpp L694-817 (`ImportSaveWithDirectoryFrame`, size check `(size-128) % 8192 == 0`, <= 15 blocks); DS `ExportSave` L671 |
| raw save (`.ps1` / headerless) | N x 8192 data blocks, no frame (name comes from the file name) | DS memory_card_image.cpp L818-820 (`ImportRawSave`) |
| `.psv` (PS3 export of a PS1 save) | 0x84-byte header (`\0VSP`, salt 0x08, HMAC-SHA1 0x1C, size at 0x40/0x5C, filename at 0x64) + data blocks. Signed | MemcardRex ps1card.cs L512-537 |
| `.mcb`, `.mcx`, `.pda`, `.psx` (single) | Smart Link, Datel, Xploder/AR/GS/Caetla | MemcardRex README "Supported single save formats" |

Per-game extraction from a shared PS1 card: the directory format is fully documented and small. Extract = copy the directory frame and follow the block links into a `.mcs`. Inject = find a free directory entry and enough free blocks, write the frame and blocks, rewrite the link fields. The save's bytes survive intact. The card image does not stay byte-identical, because block placement and link pointers can change and deleted-save remnants are not carried. Tools: MemcardRex (GUI, all formats above), DuckStation's built-in Memory Card Editor (`.mcs` import/export, DS memory_card_image.cpp L671/L805). Identity: the directory filename carries the product code (for example `BASLUS-00594`), so a card can be split by serial. Caveat: many PS1 games read other games' saves (sequel data carry-over), so a per-serial split can hide a save a game wants. No PS1 emulator here has a filter list like PCSX2's (UNVERIFIED that none exists; none was found in the sources read).

Multi-disc games save under disc 1's product code on every disc. Checked 2026-09-28 by reading each disc's `SYSTEM.CNF` serial and searching the raw image for `B[AEI]S..-NNNNN` literals in the local corpus. Every disc of a set carries the same literal:

- Xenogears (SLUS-00664, 00669): `BASLUS-00664`. Both discs also hold `BASLUS-01160` and `BISLPS-00800`, other games' codes it probes for.
- Metal Gear Solid v1.1 (SLUS-00594, 00776): `BASLUS-00594`.
- Resident Evil 2 (SLUS-00421, 00592): `BASLUS-0042100`.
- Parasite Eve (SLUS-00662, 00668): format string `bu%d0:BASLUS-00662000000%c%c`.
- Parasite Eve II (SLUS-01042, 01055): `BASLUS-01042________`.
- Koudelka (SLUS-01051, 01100-01102): `BASLUS-01051`.
- Galerians (SLUS-00986, 01098, 01099): `BASLUS-00986--GALE-1` to `-3`.
- Legend of Dragoon (SCUS-94491, 94584-94586): `BASCUS-94491`.
- Chrono Cross (SLUS-01041, 01080): `BASLUSP01041`, a template whose separator the game fills at runtime (UNVERIFIED how).
- Star Ocean 2 (SCUS-94421, 94422): only `BASCUS-9` appears; the rest is built at runtime (UNVERIFIED).
- FF7 (SCUS-94163-94165): no literal on disc. ff7tk lists one US name form, `BASCUS-94163FF7-S##` (https://github.com/sithlord48/ff7tk/issues/51).
- FF8, FF9, Valkyrie Profile: no literal found, likely compressed (UNVERIFIED).

A game opens `bu00:<name>` directly, so a later disc finds an earlier disc's save only because both use one name. Matching card entries by the running disc's serial finds nothing after disc 1. pcsx_rearmed's `serial` card mode keys the card per disc (`CdromId`), so a disc 1 save doesn't reach disc 2's card unless the player copies it.

### PS1 table

| Emulator | Files written, template, default location | Format | Scope and switching options | Per-game extraction | Source |
|---|---|---|---|---|---|
| RetroArch pcsx_rearmed | Slot 1 default: `<savedir>/<content>.srm` (frontend-managed SRAM). `pcsx_rearmed_memcard1=serial`: `<savedir>/<SERIAL-with-dash>_1.mcd` (for example `SCUS-00001_1.mcd`). `=shared`: `<savedir>/pcsx-card1.mcd`. Slot 2 default: `<savedir>/pcsx-card2.mcd`; `=serial`: `<savedir>/<SERIAL>_2.mcd` | Raw 128 KiB (`MCD_SIZE`) in all modes | `pcsx_rearmed_memcard1`: `libretro` (default) / `serial` / `shared` / `none`. `pcsx_rearmed_memcard2`: `serial` / `shared` (default) / `none`. Both added in commit `034a72c9d3` on 2026-05-02. Before that, memcard1 was always libretro `.srm`, and memcard2 was `disabled` (default) / `enabled` (shared `pcsx-card2.mcd`). So since May 2026 a fresh config gets a shared slot-2 card by default. A stored old value (`enabled`/`disabled`) is not in the new list, so RetroArch hands the core the default `shared` and writes `shared` back to the config when the core unloads (RetroArch `4ac19205` `core_option_manager.c` L1022-1056, L1915-1928; `runloop.c` L1170-1228). RetroArch matches stored values case-sensitively, and this applies to every core. A core without a `default_value`, or a legacy v0 core, falls back to the first listed value (`core_option_manager.c` L738-765, L1034-1056). Serial is per disc (`CdromId`), so multi-disc games get one card per disc in `serial` mode | Card-level split via MemcardRex / DuckStation editor to `.mcs` | PR `frontend/libretro_core_options.h` L153-183; PR `frontend/libretro.c` L3758-3800 (`load_memcards`), L2225-2245 (SAVE_RAM only in libretro mode); commit https://github.com/libretro/pcsx_rearmed/commit/034a72c9d3 |
| RetroArch mednafen_psx / mednafen_psx_hw (Beetle) | Slot 1 default: `<savedir>/<content>.srm`. `beetle_psx_use_mednafen_memcard0_method=mednafen`: `<savedir>/<content-base>.<N>.mcr` where N is the left index (0..63). Slot 2 when enabled: `<savedir>/<content-base>.<N>.mcr` (always `.mcr`). `shared_memory_cards=enabled` replaces the base with `mednafen_psx_libretro_shared`. The HW core uses `beetle_psx_hw_*` option keys; same code | Raw 128 KiB. Core option text: `.srm` and `.mcr` are "internally identical ... converted ... via renaming" | `beetle_psx(_hw)_use_mednafen_memcard0_method`: `libretro` (default) / `mednafen`. `beetle_psx(_hw)_enable_memcard1` (slot 2): `disabled` (default). Default changed from `enabled` to `disabled` in commit `b923925b4e` on 2025-12-27 ("Core option cleanups"). `beetle_psx(_hw)_shared_memory_cards`: `disabled` (default); only affects `.mcr` cards, so with libretro method only slot 2 becomes shared. `memcard_left_index` / `memcard_right_index` pick among numbered `.mcr` cards (hot-swappable, mednafen method only) | Same as pcsx_rearmed | BP `libretro_core_options.h` L1194-1250; BP `libretro.c` L3351-3367 (`%d.mcr`), L5430-5462, L7314-7319 (`MDFN_MakeFName` builds `<savedir>/<shared or content base>.<ext>`), L7183-7210 (SAVE_RAM only in libretro method); https://github.com/libretro/beetle-psx-libretro/commit/b923925b4e |
| RetroArch swanstation | Slot 1 default: `<savedir>/<content>.srm`. `Shared`: `<savedir>/duckstation_shared_card_<slot>.mcd`. `PerGame`: `<savedir>/<SERIAL>_<slot>.mcd`. `PerGameTitle`: `<savedir>/<sanitized title>_<slot>.mcd` | Raw 128 KiB. Option text: `.srm` and `PerGameTitle` `.mcd` differ only by name | `swanstation_MemoryCards_Card1Type`: `Libretro` (default) / `Shared` / `PerGame` / `PerGameTitle` / `None`. `swanstation_MemoryCards_Card2Type`: `None` (default) / `Shared` / `PerGame` / `PerGameTitle`. `swanstation_MemoryCards_UsePlaylistTitle` (default `true`): one card for all discs of an m3u. Per-game modes fall back to the shared card when the game has no serial or title | Same | SW `src/libretro/libretro_core_options.h` L810-855; SW `src/libretro/libretro_host_interface.cpp` L429-438; SW `src/core/system.cpp` L1426-1470 |
| DuckStation (standalone) | `<DataRoot>/memcards/`. Per-game: `<name>_<slot>.mcd` where name = sanitized title (default), serial, or file title. Shared: `shared_card_<slot>.mcd` (overridable path) | Raw 128 KiB | `[MemoryCards] Card1Type` default `PerGameTitle`, `Card2Type` default `None`. Values `None`, `Shared`, `PerGame` (serial), `PerGameTitle`, `PerGameFileTitle`, `NonPersistent`. `PerGameTitle` uses the disc-set title for multi-disc games, unless a per-disc card already exists or `UsePlaylistTitle=false` | Built-in Memory Card Editor imports/exports `.mcs` and raw saves, imports whole cards in `.mcd/.mcr/.gme/.mem/.vgs/.psx/.srm` | DS `src/core/settings.h` L626-627; DS `src/core/settings.cpp` L2585-2653, L2868; DS `src/core/system.cpp` L5957-5985; DS `src/core/memory_card_image.cpp` L627-660, L671, L805-825 |
| ePSXe (closed source) | Default shared: `memcards/epsxe000.mcr` (slot 1), `memcards/epsxe001.mcr` (slot 2). "Use individual memcards by game": `memcards/games/<SERIAL>-00.mcr` / `-01.mcr` with the serial in disc-file form (for example `SCUS_941.98-00.mcr`) | Raw 128 KiB (`.mcr`); also reads `.gme`, `.mc` | Shared by default; per-game via Options > Memory Cards checkbox. Android paths UNVERIFIED | Same | Forum and ePSXe help only: https://documentation.help/ePSXe/memorycards.htm, https://www.ngemu.com/threads/epsxe-2-0-5-use-individual-memory-cards-by-game.204217/ . Per-game path is forum-reported, UNVERIFIED against a binary |
| Mednafen (standalone) | `filesys.path_sav` (default `sav/`) + `filesys.fname_sav` (default `%f.%M%x`). For PSX `%x` is `<N>.mcr`, one per port with `psx.input.portN.memcard=1` (default on for all 8 ports). Typical result `sav/<game>.<md5>.0.mcr`, `.1.mcr` | Raw 128 KiB | Per content (file base + game hash). No shared mode except by editing `fname_sav` | Same | https://mednafen.github.io/documentation/ (filesys.fname_sav, path_sav), https://mednafen.github.io/documentation/fname_format.txt (`%M` = hash + period after first evaluation), https://mednafen.github.io/documentation/psx.html (`psx.input.portN.memcard`). Exact `%M` resolution (with vs without hash) is partly UNVERIFIED |

### Container split summary

| Container | Lossless per-game split? | How | Identity key |
|---|---|---|---|
| PS1 raw card (`.srm/.mcr/.mcd`, any emulator) | Save-level yes; card image not byte-identical after re-inject | MemcardRex, DuckStation editor, or a small parser (documented 128-byte frame + 8 KiB blocks) -> `.mcs` | Product code in the directory filename (`BASLUS-00594...`) |
| PS1 wrapped (`.gme`, `.vmp`, `.psv`) | Yes after unwrapping. `.vmp`/`.psv` need HMAC re-signing when rewritten | MemcardRex | Same |

## Open items

- ePSXe, AetherSX2 and NetherSX2 are closed source. Their per-game naming, Android paths and folder-card support are forum-sourced or UNVERIFIED. The rest of that gap concerns PS2; see [PlayStation 2](ps2.md#open-items).
- DuckStation standalone has no layout row. Its default `PerGameTitle` card name is the game database's save title, not the content stem, so `{stem}` cannot find it: a ROM named `thps2.chd` still gets `Tony Hawk's Pro Skater 2 (USA)_1.mcd`. A layout for it has to list `memcards/*_<slot>.mcd` and pick cards by the owner ids inside them.
- The Mednafen standalone `%M` hash resolution (whether the first save lands as `<game>.0.mcr` or `<game>.<md5>.0.mcr`) is not confirmed from source.
- Beetle with `.m3u` content: whether `retro_cd_base_name` is the m3u name (one card for all discs) is UNVERIFIED.
- Sony's own multi-disc PS1 Classics: whether the EBOOT's `DISC_ID` is disc 1's serial, as converted sets use, or another id. Check one on a PS Vita.
- pcsx_rearmed's serial cards take the whole boot path, so a disc that boots from a folder gets the folder's letters in its card name: Rhapsody's `cdrom:\MARL\SLUS_010.73` gives `MARLSLUS-0_1.mcd`. sigil keeps only `SLUS_010.73` and looks for `SLUS-01073_1.mcd`. A disc with no `SYSTEM.CNF` gets pcsx's `SLUS-99999`, and sigil has no id for it. Results rebuilt from stored columns need `raw_serial` too, or the card name falls back to `title_id` and misses lowercase and `SLUSP` boot files.
