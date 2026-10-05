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

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| `pcsx2` | `{stem}.ps2` primary when `pcsx2_shared_memory_cards` = `disabled` | `Mcd001.ps2`, `Mcd002.ps2` when `enabled` (default). The core keeps them in `<system>/pcsx2/memcards/`, so pass that folder as the save root | emulator source |
| `pcsx2_standalone` | | `memcards/{pcsx2_slot1}`, `memcards/{pcsx2_slot2}`: the cards `Slot1_Filename` and `Slot2_Filename` name (`Mcd001.ps2`, `Mcd002.ps2` by default), each a file card or a folder card (a directory of save folders and `_pcsx2_superblock`). Covers PCSX2, AetherSX2, NetherSX2 and ARMSX2; the save root is the folder that holds `memcards/` | emulator source; live memory cards |

`pcsx2` matches any platform; `pcsx2_standalone` matches `ps2` only. The
`pcsx2_standalone` layout lists `memcards`, which
`sigil_save_layout_subdirs()` names.

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

## Open items

- ePSXe, AetherSX2 and NetherSX2 are closed source, so their per-game naming, Android paths and folder-card support are unconfirmed. NetherSX2 on Android keeps two 8 MB file cards, `Mcd001.ps2` and `Mcd002.ps2`, under its app folder's `memcards/`; a card it creates and never formats has every byte 0xFF, so `sigil_card_list` reports `SIGIL_ERR_UNSUPPORTED_FORMAT` for it, while collect and restore count it as no card, as an empty file. A real PCSX2-family file card with saves is still wanted.
- Pass the slot's card name as an option when the user picked a card other than `Mcd001.ps2` or `Mcd002.ps2`: `Slot1_Filename`, `Slot2_Filename`, as PCSX2's `[MemoryCards]` section names it, the file or folder name under `memcards/`.
- File cards of 16, 32 and 64 MB load (the card's superblock gives its size), but no sample of one has been through collect and restore; a new card restore creates is always 8 MB.
- PCSX2 per-game file-card override key names and default data paths per OS are unconfirmed.
- Which attributes `.max` drops relative to `.psu` is unconfirmed.
