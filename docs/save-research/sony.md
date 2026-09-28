# Sony platforms: how emulators store game saves (not save states)

Research date 2026-09-26. Source links are permalinks to the commit that was HEAD on that date unless marked otherwise.

Pinned commits used below:

- pcsx_rearmed `ff81ed17a15241d2f3730cdd7585b38e172532ca` (PR = `https://github.com/libretro/pcsx_rearmed/blob/ff81ed17a15241d2f3730cdd7585b38e172532ca`)
- beetle-psx-libretro `5718ab9b829599687671503f11494ca6a0049c57` (BP)
- swanstation `b6c30a7b270a3f68ac41f268eafdfa678d17dea2` (SW)
- duckstation `0d8dda34d3785d8a5c9e910b9ef50caa85fcde0c` (DS)
- pcsx2 `2c804670c5f2c99d6a6b842904dd222de89aa493` (P2)
- libretro/ps2 (LRPS2) `e7704d2738303479d727b9a6dd234e4a3fafb7ee` (LR)
- ppsspp `cae623f4e6c197f45662358ffc4605e3fb97298e` (PP)
- Vita3K `bbd5c3624a06572fe4f16f67564f53a7540e1f42` (V3)
- rpcs3 `4514ad6216f61ac309f970f29c75fd7d586830dc` (R3)

Anything without a source link is marked UNVERIFIED.

---

## 1. PlayStation 1

### 1.1 The PS1 memory card, common to every PSX emulator

- A card is 131072 bytes (128 KiB): 16 blocks of 8 KiB. Block 0 is the header and directory (15 x 128-byte directory frames plus a broken-sector list). Blocks 1..15 hold save data.
- One save = one directory frame (128 bytes: state byte 0x51 first / 0x52 middle / 0x53 last, size, next-block link, 20-byte filename like `BASLUS-00594xxxxxxx`) plus 1..15 linked 8 KiB blocks. Region prefix `BA`/`BE`/`BI` then the product code. MemcardRex parses exactly these fields: `https://github.com/ShendoXT/memcardrex/blob/master/MemcardRex.Core/ps1card.cs` (slot types L570-652, region prefixes L700-708).
- Every emulator below writes this raw 128 KiB layout. `.srm`, `.mcr`, `.mcd`, `.mc`, `.ddf`, `.psm`, `.ps` are all raw 128 KiB under different names. DuckStation's importer treats them identically: DS `src/core/memory_card_image.cpp` L636-641.

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

### 1.2 PS1 table

| Emulator | Files written, template, default location | Format | Scope and switching options | Per-game extraction | Source |
|---|---|---|---|---|---|
| RetroArch pcsx_rearmed | Slot 1 default: `<savedir>/<content>.srm` (frontend-managed SRAM). `pcsx_rearmed_memcard1=serial`: `<savedir>/<SERIAL-with-dash>_1.mcd` (for example `SCUS-00001_1.mcd`). `=shared`: `<savedir>/pcsx-card1.mcd`. Slot 2 default: `<savedir>/pcsx-card2.mcd`; `=serial`: `<savedir>/<SERIAL>_2.mcd` | Raw 128 KiB (`MCD_SIZE`) in all modes | `pcsx_rearmed_memcard1`: `libretro` (default) / `serial` / `shared` / `none`. `pcsx_rearmed_memcard2`: `serial` / `shared` (default) / `none`. Both added in commit `034a72c9d3` on 2026-05-02. Before that, memcard1 was always libretro `.srm`, and memcard2 was `disabled` (default) / `enabled` (shared `pcsx-card2.mcd`). So since May 2026 a fresh config gets a shared slot-2 card by default. A stored old value (`enabled`/`disabled`) is not in the new list, so RetroArch hands the core the default `shared` and writes `shared` back to the config when the core unloads (RetroArch `4ac19205` `core_option_manager.c` L1022-1056, L1915-1928; `runloop.c` L1170-1228). RetroArch matches stored values case-sensitively, and this applies to every core. A core without a `default_value`, or a legacy v0 core, falls back to the first listed value (`core_option_manager.c` L738-765, L1034-1056). Serial is per disc (`CdromId`), so multi-disc games get one card per disc in `serial` mode | Card-level split via MemcardRex / DuckStation editor to `.mcs` | PR `frontend/libretro_core_options.h` L153-183; PR `frontend/libretro.c` L3758-3800 (`load_memcards`), L2225-2245 (SAVE_RAM only in libretro mode); commit https://github.com/libretro/pcsx_rearmed/commit/034a72c9d3 |
| RetroArch mednafen_psx / mednafen_psx_hw (Beetle) | Slot 1 default: `<savedir>/<content>.srm`. `beetle_psx_use_mednafen_memcard0_method=mednafen`: `<savedir>/<content-base>.<N>.mcr` where N is the left index (0..63). Slot 2 when enabled: `<savedir>/<content-base>.<N>.mcr` (always `.mcr`). `shared_memory_cards=enabled` replaces the base with `mednafen_psx_libretro_shared`. The HW core uses `beetle_psx_hw_*` option keys; same code | Raw 128 KiB. Core option text: `.srm` and `.mcr` are "internally identical ... converted ... via renaming" | `beetle_psx(_hw)_use_mednafen_memcard0_method`: `libretro` (default) / `mednafen`. `beetle_psx(_hw)_enable_memcard1` (slot 2): `disabled` (default). Default changed from `enabled` to `disabled` in commit `b923925b4e` on 2025-12-27 ("Core option cleanups"). `beetle_psx(_hw)_shared_memory_cards`: `disabled` (default); only affects `.mcr` cards, so with libretro method only slot 2 becomes shared. `memcard_left_index` / `memcard_right_index` pick among numbered `.mcr` cards (hot-swappable, mednafen method only) | Same as pcsx_rearmed | BP `libretro_core_options.h` L1194-1250; BP `libretro.c` L3351-3367 (`%d.mcr`), L5430-5462, L7314-7319 (`MDFN_MakeFName` builds `<savedir>/<shared or content base>.<ext>`), L7183-7210 (SAVE_RAM only in libretro method); https://github.com/libretro/beetle-psx-libretro/commit/b923925b4e |
| RetroArch swanstation | Slot 1 default: `<savedir>/<content>.srm`. `Shared`: `<savedir>/duckstation_shared_card_<slot>.mcd`. `PerGame`: `<savedir>/<SERIAL>_<slot>.mcd`. `PerGameTitle`: `<savedir>/<sanitized title>_<slot>.mcd` | Raw 128 KiB. Option text: `.srm` and `PerGameTitle` `.mcd` differ only by name | `swanstation_MemoryCards_Card1Type`: `Libretro` (default) / `Shared` / `PerGame` / `PerGameTitle` / `None`. `swanstation_MemoryCards_Card2Type`: `None` (default) / `Shared` / `PerGame` / `PerGameTitle`. `swanstation_MemoryCards_UsePlaylistTitle` (default `true`): one card for all discs of an m3u. Per-game modes fall back to the shared card when the game has no serial or title | Same | SW `src/libretro/libretro_core_options.h` L810-855; SW `src/libretro/libretro_host_interface.cpp` L429-438; SW `src/core/system.cpp` L1426-1470 |
| DuckStation (standalone) | `<DataRoot>/memcards/`. Per-game: `<name>_<slot>.mcd` where name = sanitized title (default), serial, or file title. Shared: `shared_card_<slot>.mcd` (overridable path) | Raw 128 KiB | `[MemoryCards] Card1Type` default `PerGameTitle`, `Card2Type` default `None`. Values `None`, `Shared`, `PerGame` (serial), `PerGameTitle`, `PerGameFileTitle`, `NonPersistent`. `PerGameTitle` uses the disc-set title for multi-disc games, unless a per-disc card already exists or `UsePlaylistTitle=false` | Built-in Memory Card Editor imports/exports `.mcs` and raw saves, imports whole cards in `.mcd/.mcr/.gme/.mem/.vgs/.psx/.srm` | DS `src/core/settings.h` L626-627; DS `src/core/settings.cpp` L2585-2653, L2868; DS `src/core/system.cpp` L5957-5985; DS `src/core/memory_card_image.cpp` L627-660, L671, L805-825 |
| ePSXe (closed source) | Default shared: `memcards/epsxe000.mcr` (slot 1), `memcards/epsxe001.mcr` (slot 2). "Use individual memcards by game": `memcards/games/<SERIAL>-00.mcr` / `-01.mcr` with the serial in disc-file form (for example `SCUS_941.98-00.mcr`) | Raw 128 KiB (`.mcr`); also reads `.gme`, `.mc` | Shared by default; per-game via Options > Memory Cards checkbox. Android paths UNVERIFIED | Same | Forum and ePSXe help only: https://documentation.help/ePSXe/memorycards.htm, https://www.ngemu.com/threads/epsxe-2-0-5-use-individual-memory-cards-by-game.204217/ . Per-game path is forum-reported, UNVERIFIED against a binary |
| Mednafen (standalone) | `filesys.path_sav` (default `sav/`) + `filesys.fname_sav` (default `%f.%M%x`). For PSX `%x` is `<N>.mcr`, one per port with `psx.input.portN.memcard=1` (default on for all 8 ports). Typical result `sav/<game>.<md5>.0.mcr`, `.1.mcr` | Raw 128 KiB | Per content (file base + game hash). No shared mode except by editing `fname_sav` | Same | https://mednafen.github.io/documentation/ (filesys.fname_sav, path_sav), https://mednafen.github.io/documentation/fname_format.txt (`%M` = hash + period after first evaluation), https://mednafen.github.io/documentation/psx.html (`psx.input.portN.memcard`). Exact `%M` resolution (with vs without hash) is partly UNVERIFIED |

---

## 2. PlayStation 2

### 2.1 PS2 card formats

- PCSX2 file card (`.ps2`): raw NAND dump with ECC. Page = 512 data + 16 spare bytes. `MC2_MBSIZE = 1024 * 528 * 2` = 1,081,344 bytes per "MB". Sizes offered 8/16/32/64 MB, so the default 8 MB card is 8,650,752 bytes. A 131072-byte `.ps2` is treated as a PS1 card. P2 `pcsx2/SIO/Memcard/MemoryCardFile.cpp` L29-31, L333, L1020.
- The on-card filesystem is the Sony PS2 MC filesystem (superblock, FAT with indirect FAT, 1 KiB clusters, directory entries with mode/timestamps). Documented by mymc/mymcplus source and by PCSX2's FolderMemoryCard, which builds it on the fly.
- Neutral per-save forms: `.psu` (EMS; keeps directory entries with timestamps and modes, uncompressed), `.max` (Action Replay MAX; compressed, carries names but fewer attributes, UNVERIFIED which attributes drop), `.sps`/`.xps` (SharkPort/X-Port), `.cbs` (CodeBreaker), `.psv` (PS3). mymc++ imports all of these and exports `.max` and `.psu`: https://github.com/Adubbz/mymcplusplus (README). A PCSX2 folder-card save directory is also a per-save form (see 2.2).
- Save directory names follow `B{A|E|I}{SERIAL-WITH-DASH}{suffix}`, for example `BASLUS-20312...`. That is how PCSX2 matches saves to games.

### 2.2 PCSX2 folder memory cards (verified in source)

- Location: a directory under `memcards/` with the card's name (default `Mcd001.ps2`, `Mcd002.ps2`). PCSX2 defaults to File cards named `Mcd001.ps2`/`Mcd002.ps2` in slots 1-2, and folder cards are "autodetected later" when the path is a directory. P2 `pcsx2/Pcsx2Config.cpp` L1971-1978; P2 MemoryCardFile.cpp L243-250 (`Mcd%03u.ps2`, multitap `Mcd-Multitap%u-Slot%02u.ps2`).
- Layout on disk:
  - `<card>/_pcsx2_superblock`: the card's raw superblock (formatted marker, size). P2 MemoryCardFolder.cpp L237.
  - `<card>/<SAVE_DIR>/`: one host directory per save, holding the save's files verbatim (`icon.sys`, icons, data).
  - `<card>/<SAVE_DIR>/_pcsx2_index`: YAML. `$ROOT` holds the directory's `timeCreated`/`timeModified`; each file entry has `order`, `timeCreated`, `timeModified`. P2 MemoryCardFolder.cpp L1278-1310, L1728-1749.
  - `<card>/<SAVE_DIR>/_pcsx2_meta_directory` and `_pcsx2_meta/<file>`: optional raw directory-entry metadata (mode bits, attributes). P2 MemoryCardFolder.cpp L507, L579, L2020.
  - Root-level files are skipped when filtering ("no official software stores files there"). L471-475.
- Consequence: each save directory is self-contained, including its timestamps and attributes. Copying one directory between folder cards is a lossless per-game move. PCSX2 regenerates the FAT and ECC at load.
- Filtering (serial matching): `FileMcd_EmuOpen` always calls `Mcd::implFolder.SetFiltering(true)`. No user toggle remains. P2 MemoryCardFile.cpp L605-615.
  - The filter string is built in `FolderMemoryCard::AddFolder`: `"DATA-SYSTEM/BWNETCNF/" + filter`, or `"DATA-SYSTEM/BWNETCNF"` when no serial is known. P2 MemoryCardFolder.cpp L448-463. So every game always sees the `DATA-SYSTEM` (system/"Your System Configuration") and `BWNETCNF` (network configuration) folders.
  - `FilterMatches` splits on `/` and keeps a directory when its name **contains** any token as a substring (`fileName.find(singleFilter)`). P2 MemoryCardFolder.cpp L424-446. `SLUS-20312` therefore matches `BASLUS-20312FFX`.
  - The filter token list comes from the GameIndex: `FileMcd_Reopen(memcardFilters.empty() ? s_disc_serial : memcardFilters)`. P2 `pcsx2/VMManager.cpp` L895-904, L1128, L1176. `memcardFiltersAsString()` joins the YAML list with `/`. P2 `pcsx2/GameDatabase.cpp` L44-46, L288-293. Schema doc: P2 `pcsx2/Docs/GameIndex.md` L55-57.
  - When a GameIndex entry has `memcardFilters`, the list **replaces** the disc serial. The game sees only the listed serials plus the two system folders.
  - Count at P2 HEAD (GameIndex.yaml last touched in `26c7b71b19`): **421** entries carry `memcardFilters` (not ~386). 359 of them list more than one serial. 71 do not list their own serial at all. Those are mostly bundles, demos and region variants that save under another product's serial. Examples: `PBPX-95503` (GT3 bundle) -> `PBPX-95503, SCUS-97102, SCUS-97512`; `PCPX-96301` -> 11 serials without itself. P2 `bin/resources/GameIndex.yaml` L677-684. Count computed from the file at that commit.
- `_pcsx2_index` details, read 2026-09-28 from P2 `pcsx2/SIO/Memcard/MemoryCardFolder.cpp` and `MemoryCardFolder.h`:
  - Keys: `$ROOT` holds the folder's `timeCreated`/`timeModified`. Each file has `order`, `timeCreated` and `timeModified` as Unix seconds. The older `%ROOT` key is still read and renamed to `$ROOT` (L1276-1309, L1845-1866).
  - The card stores dates as 8 bytes: unused, second, minute, hour, day, month, u16 year (`MemoryCardFolder.h` L42-50). PCSX2 converts them as UTC in both directions with `gmtime` and `timegm` (L67-101).
  - Files are ordered by `order`. Files missing from the index, or folders with no index, fall back to host timestamps and host listing order, listed first (L1772-1823).
  - Rebuilding a broken index logs that it "may not work for all games (ie. GTA)", so file order matters to some games (L1712-1752).
  - The YAML style differs between emulators (observed in real saves, 2026-09-28). AetherSX2 writes block style and single-quotes keys such as `'BASLUS-20152AC04'` while leaving `icon.sys` bare. ARMSX2 writes a single line of flow style, `{$ROOT: {timeCreated: ...,timeModified: ...},icon.sys: {order: 1,...}}`, with no quoting.
  - `_pcsx2_meta_directory` and `_pcsx2_meta/<file>` hold the raw directory entry. When present they replace the default mode and the index timestamps (L507-521, L579-594).
- Folder <-> file conversion: PCSX2 Qt `MemoryCardConvertWorker` (`ConvertToFolder`, `ConvertToFile`). File conversion opens the folder card with filtering off, so all saves go into the image. P2 `pcsx2-qt/Settings/MemoryCardConvertWorker.cpp` L26-98.

### 2.3 PS2 table

| Emulator | Files written, template, default location | Format | Scope and switching options | Per-game extraction | Source |
|---|---|---|---|---|---|
| PCSX2 (file card, default) | `<DataRoot>/memcards/Mcd001.ps2`, `Mcd002.ps2`. Both slots enabled by default; multitap slots `Mcd-Multitap<P>-Slot<NN>.ps2` | Raw NAND + ECC, 8 MB default = 8,650,752 bytes (16/32/64 MB options). A 128 KiB `.ps2` is a PS1 card | Shared container for every game. Per-game only by switching card files per game in game settings (per-game `[MemoryCards]` override, UNVERIFIED key names) | Documented. mymc / mymcplus / mymc++ export `.psu`/`.max` and import `.psu/.max/.sps/.xps/.cbs/.psv`. Split by serial from the directory names. Lossless at save level through `.psu`. Card-level ECC and FAT are rebuilt | P2 MemoryCardFile.cpp L29-31, L243-250, L1020; P2 Pcsx2Config.cpp L1971-1978 |
| PCSX2 (folder card) | `<DataRoot>/memcards/<name>.ps2/` directory, one subdirectory per save + `_pcsx2_superblock`, per-dir `_pcsx2_index` | Host filesystem tree, emulated as an 8 MB card at runtime | Physically shared, logically per-game. Filtering is always on and uses the disc serial or the GameIndex `memcardFilters`, plus `DATA-SYSTEM` and `BWNETCNF` | Native per-save unit: copy `<SAVE_DIR>/` with its `_pcsx2_index` (and `_pcsx2_meta*`). Lossless | Section 2.2 |
| AetherSX2 / NetherSX2 (Android, closed source; forks of PCSX2 ~2021-22) | `Android/data/xyz.aethersx2.android/files/memcards/Mcd001.ps2`, `Mcd002.ps2` | `.ps2` file cards (8 MB) and PCSX2 folder cards. RomM uploads from AetherSX2 hold folder-card save directories with `_pcsx2_index` files (observed in a real save store, 2026-09-28) | Shared by default. Per-game card assignment UNVERIFIED | Same as PCSX2 file cards (mymc++). One report says the files under `Android/data` only update on a manual export, which would make them unreliable to sync (reporter observation, not source) | https://github.com/Trixarian/NetherSX2-patch/issues/199 ; everything else UNVERIFIED (closed source) |
| ARMSX2 (Android/iOS, open PCSX2 fork) | Same code as PCSX2: `Mcd%03u.ps2`, folder cards, always-on filtering with `DATA-SYSTEM/BWNETCNF`. Default Android data path UNVERIFIED | Same as PCSX2 | Same as PCSX2 | Same as PCSX2 | https://github.com/ARMSX2/ARMSX2/blob/master/pcsx2/SIO/Memcard/MemoryCardFolder.cpp (L467/L471 filter), https://github.com/ARMSX2/ARMSX2/blob/master/pcsx2/SIO/Memcard/MemoryCardFile.cpp (L250, L651 `SetFiltering(true)`) |
| LRPS2 (libretro `pcsx2` core, repo libretro/ps2) | Shared (default): `<system>/pcsx2/memcards/Mcd001.ps2`, `Mcd002.ps2`. Per-content: `<savedir>/<content basename without extension>.ps2` in slot 1, slot 2 disabled | `.ps2` file card (PCSX2 code, default type File) | `pcsx2_shared_memory_cards`: `enabled` (default) / `disabled`. The frontend `.srm` is not used | Same as PCSX2 file cards | LR `libretro/libretro_core_options.h` L157-169; LR `libretro/main.cpp` L236, L1249, L2567-2574, L2694-2709; LR `pcsx2/VMManager.cpp` L303-307 (`Mcd[0].Filename = "<content>.ps2"; Mcd[1].Enabled = false`) |

---

## 3. PSP

| Emulator | Files written, template, default location | Format | Scope | Per-game extraction | Source |
|---|---|---|---|---|---|
| PPSSPP (standalone) | `<memstick>/PSP/SAVEDATA/<gameName><saveName>/` with `PARAM.SFO`, `ICON0.PNG`, optional `ICON1.PMF`, `PIC1.PNG`, `SND0.AT3`, plus the game's data files. `gameName` and `saveName` come from the game's `SceUtilitySavedataParam`. `gameName` is usually the disc ID (for example `ULUS10041`), and the suffix is game-chosen | Per-title folder. `PARAM.SFO` holds `SAVEDATA_DIRECTORY`, `SAVEDATA_FILE_LIST` (per-file hashes) and `SAVEDATA_PARAMS` (hash). With `EncryptSave=true` (default, per-game setting) secure-mode saves are encrypted the same way a real PSP does it, so folders stay portable to hardware | Per title. A game can read and list other titles' folders (sequel carry-over; `gameName` is whatever the game passes), so "files for serial X" is a naming convention the game chooses. The same `SAVEDATA/` root also holds **game-data installs** (`sceUtilityGameDataInstall`) at `SAVEDATA/<gameName><dataName>/`, which can be large and are not save progress | The folder is the neutral unit. Copy it whole; never edit files without updating the SFO hashes | PP `Core/Dialog/SavedataParam.cpp` L41-50 (`ms0:/PSP/SAVEDATA/`), L239-280, L455 (`bEncryptSave`), L495-548 (SFO fields, hash); PP `Core/Dialog/PSPGamedataInstallDialog.cpp` L33, L227, L264-267; PP `Core/Config.cpp` L1088 |
| PPSSPP libretro core | `<RetroArch savedir>/PSP/SAVEDATA/...` (memstick root = frontend save directory). System assets in `<system>/PPSSPP/` | Same | Same | Same | PP `libretro/libretro.cpp` L1221-1269 (`g_Config.memStickDirectory = retro_save_dir`) |

Default standalone memstick root: Windows `Documents/PPSSPP` or `memstick/` next to the exe when portable. Android is the user-chosen storage folder. Both UNVERIFIED.

---

## 4. PS Vita

| Emulator | Files written, template, default location | Format | Scope | Per-game extraction | Source |
|---|---|---|---|---|---|
| Vita3K | `<pref-path>/ux0/user/<user_id>/savedata/<SAVEDIR>/`. `SAVEDIR` = `INSTALL_DIR_SAVEDATA` from the app's `param.sfo`, falling back to `TITLE_ID` (for example `PCSE00123`). `savedata0:`/`savedata1:` are redirected there. `sceAppUtil` slot metadata is stored as `SlotParam_<n>.bin` inside the save folder. User id default `00` | Plain decrypted files as the game wrote them. No `sce_pfs/` encryption layer, and `SlotParam_N.bin` is Vita3K-specific (real firmware uses `sce_sys/` + PFS) | Per title. Titles that share `INSTALL_DIR_SAVEDATA` share a folder (the Vita mechanism for sequels and cross-region sharing) | Folder is the unit; lossless between Vita3K instances. Moving to or from a real Vita needs PFS re-encryption on a modded console (VitaShell / Apollo, UNVERIFIED) | V3 `vita3k/io/src/io.cpp` L159-173, L232-234; V3 `vita3k/packages/src/sfo.cpp` L87-88; V3 `vita3k/app/src/apps_list.cpp` L325-344, L584; V3 `vita3k/modules/SceAppUtil/SceAppUtil.cpp` L245-246 |

Default pref-path: Windows `%APPDATA%/Vita3K/Vita3K`, Linux `~/.local/share/Vita3K/Vita3K`, Android app storage. All UNVERIFIED.

---

## 5. PlayStation 3

| Emulator | Files written, template, default location | Format | Scope | Per-game extraction | Source |
|---|---|---|---|---|---|
| RPCS3 | `$(EmulatorDir)dev_hdd0/home/<userId %08u>/savedata/<DIRNAME>/` (default user `00000001`). `DIRNAME` is chosen by the game, normally `<TITLE_ID><suffix>` (for example `BLUS30443-AUTOSAVE`). Each folder: `PARAM.SFO` (`SAVEDATA_DIRECTORY`, etc.), `ICON0.PNG`, optional `ICON1.PAM`, `PIC1.PNG`, `SND0.AT3`, data files | Per-title folder, **unencrypted**. RPCS3 does not create `PARAM.PFD`. It records "protected" files as `*<name>=1` integer entries in `PARAM.SFO` and keeps an `RPCS3_BLIST` section | Per title. Games can list and read other `DIRNAME`s (the game supplies a prefix list), so sequels and region variants cross-read. `dev_hdd0/game/<TITLE_ID>/` is installed game data or updates, not saves | Folder is the unit; lossless between RPCS3 instances. Real PS3 saves are encrypted with `PARAM.PFD`, and RPCS3 has no crypto layer (issue #9580 open). Import or export needs Apollo Save Tool on a CFW/HEN PS3 (community method) | R3 `rpcs3/Emu/vfs_config.h` L13; R3 `rpcs3/Emu/Cell/Modules/cellSaveData.cpp` L155, L251-267, L312, L792, L1635, L1716-1724, L2263-2271; https://github.com/RPCS3/rpcs3/issues/9580 ; https://forums.rpcs3.net/archive/index.php/thread-206289.html |
| aPS3e (Android RPCS3 port) | `Android/data/<pkg>/files/aps3e/config/dev_hdd0/home/00000001/savedata/<DIRNAME>/` (app data dir = `getExternalFilesDir("aps3e")`; package id `aenu.aps3e` inferred from the Java namespace, UNVERIFIED) | Same as RPCS3 (bundled RPCS3 `cellSaveData.cpp`) | Same | Same | https://github.com/aenu1/aps3e/blob/main2/app/src/main/java/aenu/aps3e/MainActivity.java L95-103; .../Application.java L22; .../app/src/main/cpp/rpcs3/rpcs3/Emu/Cell/Modules/cellSaveData.cpp L293, L752 |

---

## 6. Container split summary

| Container | Lossless per-game split? | How | Identity key |
|---|---|---|---|
| PS1 raw card (`.srm/.mcr/.mcd`, any emulator) | Save-level yes; card image not byte-identical after re-inject | MemcardRex, DuckStation editor, or a small parser (documented 128-byte frame + 8 KiB blocks) -> `.mcs` | Product code in the directory filename (`BASLUS-00594...`) |
| PS1 wrapped (`.gme`, `.vmp`, `.psv`) | Yes after unwrapping. `.vmp`/`.psv` need HMAC re-signing when rewritten | MemcardRex | Same |
| PS2 `.ps2` file card | Save-level yes | mymc / mymcplus / mymc++ -> `.psu` (keeps timestamps/modes) | Directory name `B?<SERIAL>...`, plus GameIndex `memcardFilters` for cross-serial games |
| PS2 PCSX2 folder card | Yes, natively | Copy the save subdirectory with `_pcsx2_index` / `_pcsx2_meta*` | Same; PCSX2 itself matches by substring |
| PSP `SAVEDATA/` | Yes, folders are independent | Copy folder | Folder prefix = game-supplied `gameName`; separate game-data installs from saves |
| Vita `ux0/user/00/savedata/` | Yes | Copy folder | `INSTALL_DIR_SAVEDATA` or `TITLE_ID` |
| PS3 `dev_hdd0/home/*/savedata/` | Yes | Copy folder | `DIRNAME` prefix = title ID (game-chosen) |

## 7. Open gaps

- ePSXe, AetherSX2 and NetherSX2 are closed source. Their per-game naming, Android paths and folder-card support are forum-sourced or UNVERIFIED.
- The Mednafen standalone `%M` hash resolution (whether the first save lands as `<game>.0.mcr` or `<game>.<md5>.0.mcr`) is not confirmed from source.
- Beetle with `.m3u` content: whether `retro_cd_base_name` is the m3u name (one card for all discs) is UNVERIFIED.
- PCSX2 per-game file-card override key names and default data paths per OS are UNVERIFIED.
- Which attributes `.max` drops relative to `.psu` is UNVERIFIED.
- Vita3K and PPSSPP default roots per OS are UNVERIFIED.
- Real-hardware interop for Vita and PS3 needs encryption tooling outside RomM's reach. Emulator-to-emulator folder copies are lossless.
