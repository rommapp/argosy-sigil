# PlayStation 2

Status: synced
sigil reads the disc's serial, finds the memory cards for the layouts below, and syncs the game's saves on them with collect and restore.

## Identification

| Slug | Inputs | `title_id` example | `usage` |
|---|---|---|---|
| `ps2` | `.iso`, `.chd` | `SLUS-20675` | folder-prefix |

**PS2 adds a region prefix and enumerates folders by prefix.** `title_id` is the
ROM serial (`SLUS-20152`). `save_id` is the region-prefixed stem
(`BASLUS-20152`): `BA` for NTSC-U (`SLUS`), `BE` for PAL (`SLES`), `BI`
for `SLPS`/`SLPM`/`SLKA`, derived from the serial's region letter. The
folders the game creates on AetherSX2/NetherSX2/PCSX2 append a
per-artifact suffix (Ace Combat 04 = `BASLUS-20152AC04`; Champions of
Norrath splits into `BASLUS-20642SYS` + `BASLUS-20642RD0`), so `usage`
is `folder-prefix` and consumers enumerate every memory-card folder
whose name starts with `save_id`. The suffix is not derivable from the
disc, and it does not need to be: prefix matching captures it.

For PS2 `save_id` diverges from `title_id` (`title_id=SLUS-20152`,
`save_id=BASLUS-20152`) because the game's runtime prefixes the serial
with a region letter (`BA`/`BE`/`BI`) and appends a per-artifact suffix
(`AC04`, `SYS`). save_id is the region-prefixed stem and `usage` is
`folder-prefix`, so consumers enumerate every folder starting with it.
`folder-prefix` example: `BASLUS-20642SYS`, `BASLUS-20642RD0`.

```sh
$ sigil "/path/to/Ace Combat 04 (USA).chd" --platform=ps2
platform=ps2 title_id=SLUS-20152 raw_serial=SLUS_201.52 save_id=BASLUS-20152 usage=folder-prefix source=binary
```

## Save layouts

| Layout | Files (role, option) | Shared | Source |
|---|---|---|---|
| `pcsx2` | `{stem}.ps2` primary when `pcsx2_shared_memory_cards` = `disabled` | `Mcd001.ps2`, `Mcd002.ps2` when `enabled` (default). The core keeps them in `<system>/pcsx2/memcards/`, so pass that folder as the save root | libretro/ps2 `libretro/main.cpp`, `pcsx2/VMManager.cpp` |
| `pcsx2_standalone` | | `memcards/Mcd001.ps2`, `memcards/Mcd002.ps2`, each a file card or a folder card (a directory of save folders and `_pcsx2_superblock`). Covers PCSX2, AetherSX2, NetherSX2 and ARMSX2; the save root is the folder that holds `memcards/` | PCSX2 `pcsx2/Pcsx2Config.cpp`, `pcsx2/SIO/Memcard/MemoryCardFolder.cpp`; subdir `memcards` |

`pcsx2` matches any platform; `pcsx2_standalone` matches `ps2` only.

## Sync

The unit is one per-game card, an 8 MB `.ps2` card with ECC.
A PCSX2 folder card gives the same unit as a file card: restore unpacks it
into the game's save folders and removes files of the game's folders the
unit lacks (through `remove`). It formats a new folder card, one with no
save folders, by writing its `_pcsx2_superblock`. A save folder whose
`_pcsx2_index` doesn't parse is damaged: collect and restore return
`SIGIL_ERR_DAMAGED` naming the index, and with `repair` read the folder
without it and write a fresh one. Restore refuses the same way to write onto
a card that holds saves behind an unusable superblock, and with `repair`
writes a new superblock; collect reads such a card's folders as they are.
A save folder of the game or a companion that sigil can't
pack (a subdirectory, a file name longer than a card entry holds), which
PCSX2 still shows, is damaged too.

`remove` is required here: PCSX2 folder cards need it to drop a save the
unit lacks, and restore refuses with `SIGIL_ERR_INVALID_ARG`, writing
nothing, when it must remove a file and `remove` is NULL. A path ending in
'/' is a directory sigil emptied (a dropped PCSX2 save folder, which PCSX2
would still show): remove it.

General refusals and `companions` are in [sync](../sync.md).

## Emulator research

Research date 2026-09-26. Source links are permalinks to the commit that was HEAD on that date unless marked otherwise.

Pinned commits used below:

- pcsx2 `2c804670c5f2c99d6a6b842904dd222de89aa493` (P2)
- libretro/ps2 (LRPS2) `e7704d2738303479d727b9a6dd234e4a3fafb7ee` (LR)

Anything without a source link is marked UNVERIFIED.

### PS2 card formats

- PCSX2 file card (`.ps2`): raw NAND dump with ECC. Page = 512 data + 16 spare bytes. `MC2_MBSIZE = 1024 * 528 * 2` = 1,081,344 bytes per "MB". Sizes offered 8/16/32/64 MB, so the default 8 MB card is 8,650,752 bytes. A 131072-byte `.ps2` is treated as a PS1 card. P2 `pcsx2/SIO/Memcard/MemoryCardFile.cpp` L29-31, L333, L1020.
- The on-card filesystem is the Sony PS2 MC filesystem (superblock, FAT with indirect FAT, 1 KiB clusters, directory entries with mode/timestamps). Documented by mymc/mymcplus source and by PCSX2's FolderMemoryCard, which builds it on the fly.
- Neutral per-save forms: `.psu` (EMS; keeps directory entries with timestamps and modes, uncompressed), `.max` (Action Replay MAX; compressed, carries names but fewer attributes, UNVERIFIED which attributes drop), `.sps`/`.xps` (SharkPort/X-Port), `.cbs` (CodeBreaker), `.psv` (PS3). mymc++ imports all of these and exports `.max` and `.psu`: https://github.com/Adubbz/mymcplusplus (README). A PCSX2 folder-card save directory is also a per-save form (see 2.2).
- Save directory names follow `B{A|E|I}{SERIAL-WITH-DASH}{suffix}`, for example `BASLUS-20312...`. That is how PCSX2 matches saves to games.

### PCSX2 folder memory cards (verified in source)

- Location: a directory under `memcards/` with the card's name (default `Mcd001.ps2`, `Mcd002.ps2`). PCSX2 defaults to File cards named `Mcd001.ps2`/`Mcd002.ps2` in slots 1-2, and folder cards are "autodetected later" when the path is a directory. P2 `pcsx2/Pcsx2Config.cpp` L1971-1978; P2 MemoryCardFile.cpp L243-250 (`Mcd%03u.ps2`, multitap `Mcd-Multitap%u-Slot%02u.ps2`).
- Layout on disk:
  - `<card>/_pcsx2_superblock`: the card's raw superblock (formatted marker, size). P2 MemoryCardFolder.cpp L237.
  - `<card>/<SAVE_DIR>/`: one host directory per save, holding the save's files verbatim (`icon.sys`, icons, data).
  - `<card>/<SAVE_DIR>/_pcsx2_index`: YAML. `$ROOT` holds the directory's `timeCreated`/`timeModified`; each file entry has `order`, `timeCreated`, `timeModified`. P2 MemoryCardFolder.cpp L1278-1310, L1728-1749.
  - `<card>/<SAVE_DIR>/_pcsx2_meta_directory` and `_pcsx2_meta/<file>`: optional raw directory-entry metadata (mode bits, attributes). P2 MemoryCardFolder.cpp L507, L579, L2020.
  - Root-level files are skipped when filtering ("no official software stores files there"). L471-475.
- Consequence: each save directory is self-contained, including its timestamps and attributes. Copying one directory between folder cards is a lossless per-game move. PCSX2 regenerates the FAT and ECC at load.
- The destination card needs a formatted superblock, or the copied directory is invisible. Read 2026-09-29 from P2 master `pcsx2/SIO/Memcard/MemoryCardFolder.cpp` and `MemoryCardFolder.h`:
  - `LoadMemoryCardData` (L232-275) `fread`s the whole superblock union, `raw[BlockSize]` = 0x2000 bytes (`MemoryCardFolder.h` L251-255), and indexes the save directories only when that read succeeds and `IsFormatted()` holds.
  - `IsFormatted()` (L133-137) is `raw[0x16] == 0x6F`, the `o` of `Format` in the magic.
  - A missing, empty or short `_pcsx2_superblock` therefore reads as an unformatted card: the BIOS and games see no saves, and offer to format. Argosy shipped exactly this from v2.0.0 to v2.18.0, creating an empty superblock in a fresh card before restoring into it.
  - A superblock written from scratch works: the standard 8 MB values in P2 `pcsx2/Reference/PS2-MemoryCardFileSystem.htm` (magic `Sony PS2 Memory Card Format `, version `1.2.0.0`, page_len 512, 2 pages/cluster, 16 pages/block, 0xFF00, 8192 clusters, alloc_offset 41, alloc_end 8135, rootdir 0, backup blocks 1023/1022, ifc_list `[8, 0...]`, bad_block_list all -1, card_type 2, card_flags 0x52), padded to 0x2000 bytes. Both mymc samples (`mymc-mc01`, `mymc-mc02`) carry card_flags `0x2B` instead, and sigil writes `0x2B` to match them byte for byte. `IsFormatted()` doesn't read the flags. sigil pads with zeros, as the rest of mymc's first erase block reads; what PCSX2 itself writes after the superblock page is UNVERIFIED until a real `_pcsx2_superblock` sample exists. `SetSizeInClusters` (L1674-1693) derives alloc_offset, alloc_end and the backup blocks with the same formulas. The FAT and directory entries are rebuilt from the host folders at load, so the superblock is the only card-level file a restore has to supply. Live-emulator proof is still pending.
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

### PS2 table

| Emulator | Files written, template, default location | Format | Scope and switching options | Per-game extraction | Source |
|---|---|---|---|---|---|
| PCSX2 (file card, default) | `<DataRoot>/memcards/Mcd001.ps2`, `Mcd002.ps2`. Both slots enabled by default; multitap slots `Mcd-Multitap<P>-Slot<NN>.ps2` | Raw NAND + ECC, 8 MB default = 8,650,752 bytes (16/32/64 MB options). A 128 KiB `.ps2` is a PS1 card | Shared container for every game. Per-game only by switching card files per game in game settings (per-game `[MemoryCards]` override, UNVERIFIED key names) | Documented. mymc / mymcplus / mymc++ export `.psu`/`.max` and import `.psu/.max/.sps/.xps/.cbs/.psv`. Split by serial from the directory names. Lossless at save level through `.psu`. Card-level ECC and FAT are rebuilt | P2 MemoryCardFile.cpp L29-31, L243-250, L1020; P2 Pcsx2Config.cpp L1971-1978 |
| PCSX2 (folder card) | `<DataRoot>/memcards/<name>.ps2/` directory, one subdirectory per save + `_pcsx2_superblock`, per-dir `_pcsx2_index` | Host filesystem tree, emulated as an 8 MB card at runtime | Physically shared, logically per-game. Filtering is always on and uses the disc serial or the GameIndex `memcardFilters`, plus `DATA-SYSTEM` and `BWNETCNF` | Native per-save unit: copy `<SAVE_DIR>/` with its `_pcsx2_index` (and `_pcsx2_meta*`). Lossless | Section 2.2 |
| AetherSX2 / NetherSX2 (Android, closed source; forks of PCSX2 ~2021-22) | `Android/data/xyz.aethersx2.android/files/memcards/Mcd001.ps2`, `Mcd002.ps2` | `.ps2` file cards (8 MB) and PCSX2 folder cards. RomM uploads from AetherSX2 hold folder-card save directories with `_pcsx2_index` files (observed in a real save store, 2026-09-28) | Shared by default. Per-game card assignment UNVERIFIED | Same as PCSX2 file cards (mymc++). One report says the files under `Android/data` only update on a manual export, which would make them unreliable to sync (reporter observation, not source) | https://github.com/Trixarian/NetherSX2-patch/issues/199 ; everything else UNVERIFIED (closed source) |
| ARMSX2 (Android/iOS, open PCSX2 fork) | Same code as PCSX2: `Mcd%03u.ps2`, folder cards, always-on filtering with `DATA-SYSTEM/BWNETCNF`. Default Android data path UNVERIFIED | Same as PCSX2 | Same as PCSX2 | Same as PCSX2 | https://github.com/ARMSX2/ARMSX2/blob/master/pcsx2/SIO/Memcard/MemoryCardFolder.cpp (L467/L471 filter), https://github.com/ARMSX2/ARMSX2/blob/master/pcsx2/SIO/Memcard/MemoryCardFile.cpp (L250, L651 `SetFiltering(true)`) |
| LRPS2 (libretro `pcsx2` core, repo libretro/ps2) | Shared (default): `<system>/pcsx2/memcards/Mcd001.ps2`, `Mcd002.ps2`. Per-content: `<savedir>/<content basename without extension>.ps2` in slot 1, slot 2 disabled | `.ps2` file card (PCSX2 code, default type File) | `pcsx2_shared_memory_cards`: `enabled` (default) / `disabled`. The frontend `.srm` is not used | Same as PCSX2 file cards | LR `libretro/libretro_core_options.h` L157-169; LR `libretro/main.cpp` L236, L1249, L2567-2574, L2694-2709; LR `pcsx2/VMManager.cpp` L303-307 (`Mcd[0].Filename = "<content>.ps2"; Mcd[1].Enabled = false`) |

"Section 2.2" and "see 2.2" above refer to [PCSX2 folder memory cards](#pcsx2-folder-memory-cards-verified-in-source).

### Container split summary

| Container | Lossless per-game split? | How | Identity key |
|---|---|---|---|
| PS2 `.ps2` file card | Save-level yes | mymc / mymcplus / mymc++ -> `.psu` (keeps timestamps/modes) | Directory name `B?<SERIAL>...`, plus GameIndex `memcardFilters` for cross-serial games |
| PS2 PCSX2 folder card | Yes, natively | Copy the save subdirectory with `_pcsx2_index` / `_pcsx2_meta*` | Same; PCSX2 itself matches by substring |

## Open items

- ePSXe, AetherSX2 and NetherSX2 are closed source. Their per-game naming, Android paths and folder-card support are forum-sourced or UNVERIFIED. Observed on an AYN Thor 2026-10-01: NetherSX2 (`xyz.aethersx2.android`) keeps `MemoryCards/Slot1_Filename` = `Mcd001.ps2` and `Slot2_Filename` = `Mcd002.ps2` in its shared preferences, both 8650752-byte (8 MB with ECC) file cards under `Android/data/xyz.aethersx2.android/files/memcards/`. `Mcd001.ps2` there is 0xFF in every byte: created at the card's full size and never formatted, so it has no superblock and `sigil_card_list` reports `SIGIL_ERR_UNSUPPORTED_FORMAT`. A real PCSX2-family file card with saves is still wanted.
- PCSX2 per-game file-card override key names and default data paths per OS are UNVERIFIED.
- Which attributes `.max` drops relative to `.psu` is UNVERIFIED.
