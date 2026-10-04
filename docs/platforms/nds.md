# Nintendo DS

Status: located
Sigil names the file melonDS keeps for a cart. It reads no DS header, and collect and restore don't cover these carts.

## Save layouts

Row from the [layout table](../save-units.md).

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| `melonds` | `{stem}.sav` primary | | emulator source |

The legacy melonDS core writes `{stem}.sav` itself, on its own timer.

## Save formats

melonDS, melonDS DS and the legacy melonDS core keep the raw save at the
chip's size. DeSmuME keeps the same bytes in `{stem}.dsv` with a 122-byte
footer that ends in `|-DESMUME SAVE-|`; dropping or adding the footer
converts without loss. DraStic is reported to write raw `.dsv` files
without the footer.

## Open items

- DraStic current-version footer behaviour, and whether an option toggles it.
- melondsds GBA-slot save path.
