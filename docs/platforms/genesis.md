# Mega Drive / Genesis, 32X, Master System and Game Gear

Status: synced
sigil names a cartridge's save through the libretro default row, `{stem}.srm`, which covers genesis_plus_gx and picodrive, and collect and restore carry it between the two cores. It has no extractor for these systems.

## Identification

The platform table has no Genesis, 32X, Master System or Game Gear slug, and carts carry no title id
sigil reads. The emulator names the save after the content file (see
[Identification](../identification.md)). Sync takes the platform by its
slug: `genesis`, `32x` (or `sega32`), `sms` (or `mastersystem`) and
`gamegear` (or `gg`).

## Save layouts

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| default | `{stem}.srm` primary; `{stem}.rtc` rtc | | emulator source, emulator run |

There is no Genesis-specific row. The `genesis_plus_gx` row is limited to
`segacd`, so a cartridge loaded in genesis_plus_gx falls back to the
default row, as does picodrive (see [Save units](../save-units.md)).

## Save formats

Read at genesis_plus_gx `a7985a9` and picodrive `733c711`. Both cores keep
each byte of a cart's save RAM at the address the game wrote it to, so 8-bit
RAM on the odd addresses has filler on the even ones, an EEPROM's bytes start
at 0, and a Master System or Game Gear cart's two 16 KiB banks follow each
other. On Master System and Game Gear, and for an EEPROM, that is the same
offset in both files. For Mega Drive RAM it is when the save's start
address, made even, is a multiple of 64 KiB, as `0x200000` is:
genesis_plus_gx indexes the address's low 16 bits (`core/cart_hw/sram.c`
`sram_read_byte`), picodrive the address less its start made even
(`pico/memory.c` `PicoRead8_sram`, `pico/cart.c` `PicoCartDetect`). The two
cores differ in what they fill the rest with and how much of it they save:

- genesis_plus_gx keeps the cart's 64 KiB save window (`core/cart_hw/sram.c`,
  `eeprom_i2c.c`, `sms_cart.c`) with `0xFF` where the game never wrote, and
  saves up to the last byte that isn't `0xFF` (`libretro.c`
  `retro_get_memory_size`), so its file length varies.
- picodrive keeps the window from the save's start address at the size the
  ROM header gives, 8 KiB for an EEPROM and 32 KiB for every Master System and
  Game Gear cart (`pico/cart.c` `PicoCartDetect`, `PicoCartDetectMS`; its
  `carthw.cfg` changes the range for a few carts), with `0x00` where the game
  never wrote (`0xFF` on the few carts `carthw.cfg` marks `filled_sram`), and
  saves all of it once any byte isn't `0x00`.

A frontend reads a shorter file into the start of the core's RAM and leaves
the rest as the core filled it, so each core reads the other's file. Read
that way, a genesis_plus_gx save gives picodrive `0x00` past its last byte
that isn't `0xFF`, where the game read `0xFF`.

## Sync

A cart travels as its save RAM, one file named `{stem}.srm`: the bytes as
the emulator's file holds them, so a picodrive save keeps its `0x00` fill
and its length. The identity leaves out trailing `0xFF`, so
genesis_plus_gx's trimmed file, a full 64 KiB copy of it, and the file
restore pads with `0xFF` for picodrive share one identity. So does the file
picodrive saves after loading one padded to picodrive's whole size, since it
writes the padding back: on Master System and Game Gear that is every
restore, and on Mega Drive and 32X a restore next to picodrive's own file.
picodrive allocates new RAM zeroed (`pico/cart.c` `PicoCartDetect` and
`PicoCartDetectMS`; the `filled_sram` carts aside), so a save it starts from
nothing has `0x00` where genesis_plus_gx has `0xFF`, and a file shorter than
its RAM, padded to a shorter genesis_plus_gx file's length or not padded,
comes back from picodrive with `0x00` past it. Each of those has an identity
of its own, not that of the same game state saved in genesis_plus_gx.
Restored for genesis_plus_gx, such a save keeps its bytes and that identity.

Restore writes the file in the form of the core the request names:

- `genesis_plus_gx`, and `genesis_plus_gx_wide`, which saves the same way:
  the RAM without trailing `0xFF`, as the core saves it.
  A RAM of `0xFF` alone keeps its length, so the file isn't empty.
- `picodrive`: the RAM padded with `0xFF`, so the game reads in picodrive
  the bytes it read in genesis_plus_gx. On Master System and Game Gear it is
  padded to 32 KiB, the one size picodrive keeps for them (`pico/cart.c`
  `PicoCartDetectMS`). On Mega Drive and 32X only the ROM header and
  `carthw.cfg` give picodrive's size, so restore pads to the length of the
  file already there when that file holds the whole save, else the RAM goes
  as it came. The file there is picodrive's size only if picodrive wrote it:
  in a saves folder both cores share, genesis_plus_gx may have written it,
  trimmed and shorter than the save, or at its whole 64 KiB. Restore never
  writes fewer bytes than the save holds.
- any other core: the RAM as it came.

Where restore knows the RAM a core keeps, the file stops there, since the
game can't read a byte past it: genesis_plus_gx's at its 64 KiB window, and
picodrive's at 32 KiB on Master System and Game Gear, so another frontend's
64 KiB Game Gear save gives picodrive 32 KiB. On Mega Drive and 32X restore
doesn't know picodrive's size, so picodrive's file runs past it when the save
or the file there is longer, as RetroArch's 64 KiB copy of a genesis_plus_gx
save is for a game picodrive keeps 16 KiB of. A frontend reads a file up to
the core's size (RetroArch `save.c` `content_load_ram_file` at `v1.22.2`), so
picodrive takes its own size of it.

The same save in the other core's form is written again in the request's
form; restore skips the write only when the file holds those bytes already.

Two choices here are sigil's, not what a core does on its own:

- The `0xFF` padding for picodrive. Handed a shorter file, picodrive keeps
  its own `0x00` past it. Padding with `0xFF` gives the game the bytes it read
  in genesis_plus_gx, and a round trip through picodrive comes back to
  genesis_plus_gx the same size and bytes. Once picodrive has loaded a file
  padded to its whole size, it saves the same bytes back.
- A RAM of `0xFF` alone, which genesis_plus_gx wouldn't save at all, is
  written at its length rather than left out.

A save that goes to the other core and back comes back the same size and
bytes whenever restore knows picodrive's size: the cart is a Master System or
Game Gear one, or picodrive's own file is there. Restored on Mega Drive and
32X into an empty folder, or next to a file shorter than the save, a
genesis_plus_gx save goes to picodrive as it came; next to a longer
genesis_plus_gx file it is padded to that file's length. When that leaves the
file shorter than picodrive's RAM, picodrive fills the rest with `0x00` and
saves all of it, and that save comes back to genesis_plus_gx at picodrive's
size, with the `0x00` where the game had read `0xFF`. Both cores load it.
Restore would need the ROM header and `carthw.cfg` to pad it.

Restore also reads a raw `.srm` from either core, as clients uploaded before
sigil, and a zip holding the one save as `save.sram` or an emulator's `.srm`
or `.sav`; it refuses a zip of anything else.

## Open items

- No Genesis, 32X, Master System or Game Gear platform slug or extractor.
- Restore for picodrive on Mega Drive and 32X doesn't write picodrive's own file, since only the ROM header and `carthw.cfg` give its size. Into an empty folder or next to a file shorter than the save it writes the RAM as it came, without the `0xFF` padding; next to a genesis_plus_gx file that holds the save, the padding stops at that file's length; and the file can be longer than picodrive's RAM, of which picodrive reads its own size.
- Master System and Game Gear carts with a serial EEPROM (93C46) save only in genesis_plus_gx; picodrive has no EEPROM for them.
- Master System and Game Gear carts on the Codemasters mapper that switch on their 8 KiB of cart RAM, Ernie Els Golf among them, save it in genesis_plus_gx (`core/cart_hw/sms_cart.c` `mapper_16k_w`, lines 1474-1482 at `a7985a9`), while picodrive maps that RAM into its video RAM, outside its save (`pico/sms.c` `write_bank_codem`, lines 720-723 at `733c711`). Restore still writes picodrive's 32 KiB file, which the game doesn't read there, so the save doesn't reach the game in picodrive.
- On Mega Drive, RAM sits at the same offsets in the two cores only when both place it alike, from a start address that, made even, is a multiple of 64 KiB. genesis_plus_gx moves a header start at or above `0x800000` to `0x200000` and keeps 64 KiB of a wider header range (`core/cart_hw/sram.c` `sram_init`, lines 124-136 at `a7985a9`); picodrive takes the header's range as it is, or `carthw.cfg`'s (`pico/cart.c` `PicoCartDetect`). sigil reads no ROM header and moves nothing, so such a save may not cross. No such cart has been checked.
- The 32X runs in picodrive alone; no other core's 32X save has been read.
- A core with no Sega form here gets the RAM as it came, in the file its row's primary member names. The `fbneo` row has no platform, so a cart sent with `fbneo` gets it as `fbneo/{romset}.fs`; FBNeo's own form for these carts hasn't been read.
- The ares save file name, its RAM interleaving, and the Kega Fusion and BlastEm layouts are unconfirmed.
