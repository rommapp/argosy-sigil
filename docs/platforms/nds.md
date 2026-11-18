# Nintendo DS

Status: synced
Sigil names the files melonDS and DeSmuME keep for a cart, and collect and
restore convert between them. It reads no DS header.

## Save layouts

Rows from the [layout table](../save-units.md). The default row applies to any cart core with no row of its own.

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| default | `{stem}.srm` primary | | emulator source |
| `melonds` | `{stem}.sav` primary | | emulator source |
| `desmume` | `{stem}.dsv` primary: the save, then DeSmuME's footer | | emulator source |

The legacy melonDS core writes `{stem}.sav` itself, on its own timer.
melonDS DS (`melondsds`) has no row: it hands the save to RetroArch as
`RETRO_MEMORY_SAVE_RAM`, which RetroArch writes as `{stem}.srm`. The
DeSmuME core writes `{stem}.dsv` itself in the save folder the frontend
gives it; RetroArch's compression setting doesn't apply to it.

## Save formats

melonDS, melonDS DS and the legacy melonDS core keep the raw save at the
chip's size, which melonDS takes from its own game list, not from the
file: it reads that many bytes from the start of the file. DeSmuME keeps
the same bytes in `{stem}.dsv` with a 122-byte footer: the text
`|<--Snip above here to create a raw sav by excluding this DeSmuME savedata footer:`
(82 bytes), six little-endian u32s, then the cookie `|-DESMUME SAVE-|`.

| Field | Value |
|---|---|
| size | bytes of the save the game had written when the file last grew |
| padSize | the save's length in the file, before the footer |
| type | the index of padSize in DeSmuME's save types, 0 for none |
| addr_size | address bytes the game's commands use: 1 for 512 bytes, 2 up to 64 KiB, 3 above |
| mem_size | padSize again |
| version | 0 |

DeSmuME takes a footer by its cookie and version 0 alone, and reads the
save as everything before it. A `.dsv` without the footer isn't taken as
a save: DeSmuME detects the chip again, and the game's first write fills
the file with 0xFF from the start, over the save. DraStic is reported to
write raw `.dsv` files without the footer.

## Sync

The unit is the raw save, one file named `{stem}.srm`. Collect reads the
layout's file and drops DeSmuME's footer when it has one, so a save gives
the same unit from melonDS and from DeSmuME.

Restore writes the raw save to the layout's file. On the `desmume` row it
writes what DeSmuME writes when it imports a raw save (`mc.cpp`
`import_raw`, `saveBuffer`, `ensure`): the save padded with 0xFF to the
next of DeSmuME's sizes (512 bytes, 8, 32 and 64 KiB, 256 KiB, then each
power of two to 64 MiB), then the footer, with size the save's own length,
and type and addr_size from that padded size. A `.dsv` already holding the
same save without the footer is written again with it. The footer comes
from the layout row, never the file's name: DraStic also writes `.dsv`.

Restore also reads a `.dsv` with its footer, as clients uploaded before
sigil.

## Open items

- DeSmuME has no 1 Mbit type. A 128 KiB save (melonDS's 1 Mbit EEPROM, 14 games in its list) goes to DeSmuME padded to 256 KiB with a 2 Mbit footer, as DeSmuME's own import writes it and as DeSmuME grows its own file for such a game. Written without the padding and footer, DeSmuME would take the file as empty (above). The save comes back to melonDS at 256 KiB; melonDS reads its 128 KiB from the start and writes 128 KiB the next time the game saves, so until then the unit's identity differs between the two.
- DeSmuME addresses a few chips with another address size than their size gives (`mc.cpp` lists Over the Hedge: Hammy Goes Nuts, 1 byte, and Puzzler World, 2 bytes). Restore writes the address size by the size.
- NAND saves (8 to 64 MiB, melonDS types 8 to 10) get the footer by the same rule; neither emulator has been run with one. For 16 and 64 MiB DeSmuME's import writes an addr_size of 0xFFFFFFFF (`addr_size_for_old_save_size` has no case for them); restore writes 3, from its save types.
- DeSmuME standalone and the `desmume2015` core have no rows; neither was read.
- DraStic current-version footer behaviour, and whether an option toggles it.
- melondsds GBA-slot save path.
