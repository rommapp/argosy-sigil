# Mega Drive / Genesis, 32X, Master System and Game Gear

Status: located
sigil names a cartridge's save through the libretro default row, `{stem}.srm`, which covers genesis_plus_gx and picodrive; it has no extractor or sync kind for these systems.

## Identification

The platform table has no Genesis, 32X, Master System or Game Gear slug, and carts carry no title id
sigil reads. The emulator names the save after the content file (see
[Identification](../identification.md)).

## Save layouts

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| default | `{stem}.srm` primary; `{stem}.rtc` rtc | | emulator source |

There is no Genesis-specific row. The `genesis_plus_gx` row is limited to
`segacd`, so a cartridge loaded in genesis_plus_gx falls back to the
default row, as does picodrive (see [Save units](../save-units.md)).

## Save formats

Cartridge SRAM differs in length between cores. genesis_plus_gx trims
trailing `0xFF` bytes, so its file length varies; picodrive pads with
`0x00` to a fixed size. Odd-byte SRAM is stored expanded in both. Padded
with `0xFF` to the full size, the saves match.

## Open items

- No Genesis, 32X, Master System or Game Gear platform slug or extractor.
- The ares save file name, its RAM interleaving, and the Kega Fusion and BlastEm layouts are unconfirmed.
