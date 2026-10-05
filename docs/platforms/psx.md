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

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| `mednafen_psx_hw` | `{stem}.srm` primary when `beetle_psx_hw_use_mednafen_memcard0_method` = `libretro` (default); `{stem}.{left_index}.mcr` primary when `mednafen` and `beetle_psx_hw_shared_memory_cards` = `disabled` (default); `{stem}.{right_index}.mcr` sidecar when `beetle_psx_hw_enable_memcard1` = `enabled` and shared cards are off | `mednafen_psx_libretro_shared.{left_index}.mcr` when `beetle_psx_hw_shared_memory_cards` = `enabled` and the method is `mednafen`; `mednafen_psx_libretro_shared.{right_index}.mcr` when shared cards are on and `beetle_psx_hw_enable_memcard1` = `enabled` | emulator source |
| `mednafen_psx` | Same files as `mednafen_psx_hw`, under the software build's `beetle_psx_*` keys (`beetle_psx_use_mednafen_memcard0_method`, `beetle_psx_enable_memcard1`, `beetle_psx_shared_memory_cards`) | Same as `mednafen_psx_hw`, under the same keys | emulator source |
| `pcsx_rearmed` | `{stem}.srm` primary when `pcsx_rearmed_memcard1` = `libretro` (default); `{pcsx_serial}_1.mcd` primary when `pcsx_rearmed_memcard1` = `serial`; `{pcsx_serial}_2.mcd` sidecar when `pcsx_rearmed_memcard2` = `serial` | `pcsx-card1.mcd` when `pcsx_rearmed_memcard1` = `shared`; `pcsx-card2.mcd` when `pcsx_rearmed_memcard2` = `shared` (default) | emulator source; live memory cards |
| `swanstation` | `{stem}.srm` primary when `swanstation_MemoryCards_Card1Type` = `Libretro` (default); `{title_id}_1.mcd` primary when `PerGame`; `{stem}_1.mcd` primary when `PerGameTitle`; `{title_id}_2.mcd`, `{stem}_2.mcd` sidecar when `swanstation_MemoryCards_Card2Type` = `PerGame`, `PerGameTitle` | `duckstation_shared_card_1.mcd`, `duckstation_shared_card_2.mcd` when the slot's type is `Shared` | emulator source |
| `duckstation` | `memcards/{stem}_1.mcd` primary when `Card1Type` = `PerGameTitle` (default) or `PerGameFileTitle`; `memcards/{title_id}_1.mcd` primary when `PerGame`; the same `_2.mcd` names sidecar for `Card2Type` | `memcards/shared_card_1.mcd`, `memcards/shared_card_2.mcd` when the slot's type is `Shared` | emulator source |
| `armsx1` | | `slot1.mcd`, `slot2.mcd` in the app's private files folder | emulator source |
| `vita_pops` | `PSP/SAVEDATA/{disc_id}/SCEVMC0.VMP` primary (slot 1); `PSP/SAVEDATA/{disc_id}/SCEVMC1.VMP` sidecar (slot 2). `{disc_id}` is the title id's letters and digits (`SLUS01040`), the EBOOT's `DISC_ID`. Signed `.vmp` cards; the folder's `PARAM.SFO` and `ICON0.PNG` stay as the console wrote them. POPS won't read a card whose folder has no `PARAM.SFO`, so a restore into a folder without one also writes it: the folder's name, the content's name as its title, signed as the PSP save utility signs it. POPS checks the hash at 0x70, not the console-keyed one at 0x20, and rewrites the file with all three when the game runs | | on device (PS Vita) |

`mednafen_psx_hw` and `mednafen_psx` match any platform; `vita_pops`
matches `psx` only. `vita_pops` covers PS1 classics on a PSP, and on a
Vita (official PS1 Classics and Adrenaline): the save root is the folder
holding `PSP/` (`ms0:/` or `ux0:pspemu/`), and the layout lists
`PSP/SAVEDATA`.

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

Every row but `vita_pops` was read from the core's source; the names are
the core's literals. `vita_pops` was checked on a PS Vita running PS1
games under Adrenaline: the folder names, the files POPS writes, its
`.vmp` signatures (a new card from sigil is byte-identical to the
console's), and that POPS boots a folder sigil created from nothing.

## Sync

The unit is one per-game card, a raw PS1 card, holding the game's saves
from its own card and from the shared cards beside it
([whose saves](../sync.md#whose-saves)).
Restore writes a PS1 card back in the form it found: a DexDrive `.gme`
keeps its header, with the frame copies following the directory and a
slot's comment kept only beside the save it was written for; a PSP or Vita
`.vmp` keeps its seed and is signed for its new contents. A new card named
`.VMP` (the `vita_pops` layout) is a signed `.vmp`. A `.vmp` whose
signature doesn't match its card, which the console refuses, is damaged.

A multi-disc game saves under disc 1's product code on every disc, so a
later disc finds the save only by that code. A game that reads an
earlier title's save, as a sequel reads its prequel's, lists that title
in `companions`. Multi-disc sets pass every disc's id in `game_ids`. The
general rules for both are in [sync](../sync.md).

## Open items

- ePSXe, AetherSX2 and NetherSX2 are closed source. Their per-game naming, Android paths and folder-card support are unconfirmed. The rest of that gap concerns PS2; see [PlayStation 2](ps2.md#open-items).
- DuckStation's default `PerGameTitle` names a card after the game database's save title for any game the database knows, so the `duckstation` row's `{stem}` finds only cards of games it doesn't: a ROM named `thps2.chd` still gets `Tony Hawk's Pro Skater 2 (USA)_1.mcd`. Finding those means listing `memcards/*_<slot>.mcd` and picking cards by the owner ids inside them. `PerGame` uses the database's own serial for the 867 entries that map several disc codes to one, and DuckStation drops control characters, and `/`, `*` (and `:` on macOS, more on Windows), from names.
- ARMSX1 creates a missing card as 128 KiB of zeros rather than a formatted card, and its source writes cards back through a pointer to a string that no longer exists; whether its saves reach the card files is unconfirmed on a device.
- SwanStation's PerGameTitle on a multi-disc PBP takes the title inside the PBP, not the file's stem.
- Whether Mednafen standalone's first save lands as `<game>.0.mcr` or `<game>.<md5>.0.mcr` is unconfirmed.
- Beetle with `.m3u` content: whether the card is named after the m3u (one card for all discs) is unconfirmed.
- Sony's own multi-disc PS1 Classics: whether the EBOOT's `DISC_ID` is disc 1's serial, as converted sets use, or another id. Check one on a PS Vita.
- pcsx_rearmed's serial cards take the whole boot path, so a disc that boots from a folder gets the folder's letters in its card name: Rhapsody's `cdrom:\MARL\SLUS_010.73` gives `MARLSLUS-0_1.mcd`. sigil keeps only `SLUS_010.73` and looks for `SLUS-01073_1.mcd`. A disc with no `SYSTEM.CNF` gets pcsx's `SLUS-99999`, and sigil has no id for it. Results rebuilt from stored columns need `raw_serial` too, or the card name falls back to `title_id` and misses lowercase and `SLUSP` boot files.
