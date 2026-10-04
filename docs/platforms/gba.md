# Game Boy Advance

Status: located
Sigil names the files each core keeps for a cart. It reads no GBA header, and collect and restore don't cover these carts.

## Save layouts

Rows from the [layout table](../save-units.md). The default row applies to any cart core with no row of its own.

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| default | `{stem}.srm` primary; `{stem}.rtc` rtc | | emulator source |
| `vba_next`, `gpsp` | `{stem}.srm` primary | | emulator source |

`vba_next` and `gpsp` expose no clock region, so their rows have no
`.rtc`. RetroArch's own rules for `.srm` and `.rtc` are on
[Game Boy](gb.md#retroarch-behaviour).

## Save formats

The save data is the same bytes everywhere, but the file length is not:
gpsp pads to 128 KiB, vba_next pads too, and mGBA pads to 128 KiB while
it still detects the save type. Trimmed to the chip size, the bytes
match. mGBA standalone appends a 16-byte clock footer; Mesen2 keeps a
separate `.rtc`.

## Open items

- Pizza Boy A/C: closed source, format and RTC handling unknown.
- Whether the Mesen2 GBA `.rtc` values are BCD.
