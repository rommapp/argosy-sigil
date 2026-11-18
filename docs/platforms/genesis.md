# Mega Drive / Genesis, 32X, Master System and Game Gear

Status: synced
sigil names a cartridge's save through the libretro default row, `{stem}.srm`, which covers genesis_plus_gx and picodrive, and collect and restore carry it between the two cores. It has no extractor for these systems.

## Identification

The platform table has no Genesis, 32X, Master System or Game Gear slug, and carts carry no title id
sigil reads. The emulator names the save after the content file (see
[Identification](../identification.md)). Sync takes the platform by its
slug: `genesis` (or `megadrive`), `32x` (or `sega32`, `sega32x`), `sms` (or
`mastersystem`) and `gamegear` (or `gg`).

## Save layouts

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| default | `{stem}.srm` primary; `{stem}.rtc` rtc | | emulator source, emulator run |

There is no Genesis-specific row. The `genesis_plus_gx` row is limited to
`segacd`, so a cartridge loaded in genesis_plus_gx falls back to the
default row, as does picodrive (see [Save units](../save-units.md)).

## Save formats

Both cores keep a cart's save RAM at the same offsets: each byte at the
address the game wrote it to, so 8-bit RAM on the odd addresses has filler
on the even ones, an EEPROM's bytes start at 0, and a Master System or Game
Gear cart's two 16 KiB banks follow each other. They differ in what they
fill the rest with and how much of it they save:

- genesis_plus_gx keeps the cart's 64 KiB save window (`core/cart_hw/sram.c`,
  `eeprom_i2c.c`, `sms_cart.c`) with `0xFF` where the game never wrote, and
  saves up to the last byte that isn't `0xFF` (`libretro.c`
  `retro_get_memory_size`), so its file length varies.
- picodrive keeps the window from the save's start address at the size the
  ROM header gives, 8 KiB for an EEPROM and 32 KiB for every Master System and
  Game Gear cart (`pico/cart.c` `PicoCartDetect`, `PicoCartDetectMS`; its
  `carthw.cfg` changes the range for a few carts), with `0x00` where the game
  never wrote, and saves all of it once any byte isn't `0x00`.

A frontend reads a shorter file into the start of the core's RAM and leaves
the rest as the core filled it, so each core reads the other's file. Read
that way, a genesis_plus_gx save gives picodrive `0x00` past its last byte
that isn't `0xFF`, where the game read `0xFF`.

## Sync

A cart travels as its save RAM, one file named `{stem}.srm`: the bytes as
the emulator's file holds them, so a picodrive save keeps its `0x00` fill
and its length. The identity leaves out trailing `0xFF`, so a save has one
identity in either core, padded or not.

Restore writes the file in the form of the core the request names:

- `genesis_plus_gx`, and `genesis_plus_gx_wide`, which saves the same way:
  the RAM without trailing `0xFF`, as the core saves it.
  A RAM of `0xFF` alone keeps its length, so the file isn't empty.
- `picodrive`: the RAM padded with `0xFF` to picodrive's size, so the game
  reads in picodrive the bytes it read in genesis_plus_gx. The size is the
  picodrive file already there; else 32 KiB on Master System and Game Gear,
  the one size picodrive keeps for them; else, on Mega Drive and 32X, where
  only the ROM header gives the size, the RAM as it came.
- any other core: the RAM as it came.

Neither core's file runs past the RAM the core keeps: picodrive's stops at
its size when restore knows it, genesis_plus_gx's at its 64 KiB window, since
the game can't read a byte past them there. Another frontend that saved a
Game Gear cart at 64 KiB gives picodrive 32 KiB.

The same save in the other core's form is written again in the request's
form; restore skips the write only when the file holds those bytes already.

Two choices here are sigil's, not what a core does on its own:

- The `0xFF` padding for picodrive. Handed a shorter file, picodrive keeps
  its own `0x00` past it. Padding with `0xFF` gives the game the bytes it read
  in genesis_plus_gx, and a round trip through picodrive comes back to
  genesis_plus_gx the same size and bytes. Once picodrive saves a padded file
  it writes the same bytes back.
- A RAM of `0xFF` alone, which genesis_plus_gx wouldn't save at all, is
  written at its length rather than left out.

A save that goes to the other core and back comes back the same size and
bytes whenever restore knows picodrive's size: its file is there, or the cart
is a Master System or Game Gear one. Restored into an empty folder on Mega
Drive and 32X, a genesis_plus_gx save goes to picodrive as it came; picodrive
loads it, fills the rest of its RAM with `0x00` and saves all of it, and that
save comes back to genesis_plus_gx at picodrive's size, with the `0x00` where
the game had read `0xFF`. Both cores load it. Restore would need the ROM
header to pad it.

Restore also reads a raw `.srm` from either core, as clients uploaded before
sigil, and a zip holding the one save as `save.sram` or an emulator's `.srm`
or `.sav`; it refuses a zip of anything else.

## Open items

- No Genesis, 32X, Master System or Game Gear platform slug or extractor.
- Restore into an empty folder for picodrive on Mega Drive and 32X writes the RAM as it came, without picodrive's `0xFF` padding, since only the ROM header gives the size.
- Master System and Game Gear carts with a serial EEPROM (93C46) save only in genesis_plus_gx; picodrive has no EEPROM for them.
- The 32X runs in picodrive alone; no other core's 32X save has been read.
- The ares save file name, its RAM interleaving, and the Kega Fusion and BlastEm layouts are unconfirmed.
