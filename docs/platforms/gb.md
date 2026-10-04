# Game Boy and Game Boy Color

Status: located
Sigil reads the cart header and names the files each core keeps for a cart. Collect and restore don't cover these carts.

## Identification

| Slug | Platform | Inputs | `title_id` example | `usage` | Status |
|---|---|---|---|---|---|
| `gb` | Game Boy | `.gb`, `.sgb` | none; sets `features` | file-prefix | |
| `gbc` | Game Boy Color | `.gbc` | none; sets `features` | file-prefix | |

Game Boy and SNES carts carry no title id. Sigil validates the header
and reports what the cart holds in `features` (see
[Save units](../save-units.md)); `title_id` and `save_id` stay empty and
the emulator names the save after the content file.

### Features

`SIGIL_FEATURE_RTC` on `gb` / `gbc`: header byte `0x147` is `0x0F`
(MBC3+TIMER+BATTERY), `0x10` (MBC3+TIMER+RAM+BATTERY), `0xFD` (TAMA5) or
`0xFE` (HuC3). The header checksum over `0x134..0x14C` has to match byte
`0x14D` first; the boot ROM refuses a cart that fails it, so a mismatch
means the bytes are not a Game Boy header. gambatte, mGBA and VBA-M key
their `RETRO_MEMORY_RTC` region on the same values.

## Save layouts

Rows from the [layout table](../save-units.md). The default row applies to any cart core with no row of its own.

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| default | `{stem}.srm` primary; `{stem}.rtc` rtc | | emulator source |
| `vba_next`, `gpsp` | `{stem}.srm` primary | | emulator source |

`vba_next` and `gpsp` expose no clock region, so their rows have no
`.rtc`.

## RetroArch behaviour

These apply to every libretro core that exposes save memory.

- RetroArch writes `RETRO_MEMORY_SAVE_RAM` as `{stem}.srm` and
  `RETRO_MEMORY_RTC` as `{stem}.rtc`, the bytes exactly as the core
  exposes them, with no header.
- With `save_file_compression` on (off by default), RetroArch wraps the
  file in RZIP, which starts with `#RZIPv`. A sync server must detect
  `#RZIPv` and inflate before comparing bytes. RetroArch reads
  uncompressed files either way.
- `sort_savefiles_enable` (on by default), `sort_savefiles_by_content_enable`
  and `savefiles_in_content_dir` change the folder, not the bytes.
- Some cores write their own files through the save folder instead
  (bsnes, DeSmuME, the legacy melonDS core, Nestopia FDS, Mesen FDS,
  Citra/Azahar). RetroArch's compression setting doesn't apply to those.

## Clock formats

Cart RAM is the same raw bytes in every emulator. The MBC3 clock is not:
mGBA, VBA-M, SameBoy and Gearboy write the same 48-byte layout (as a
footer on `.sav` standalone, or a separate `.rtc` in the mGBA and VBA-M
cores), gambatte stores only an 8-byte anchor time, Mesen2 a 13-byte
form, and TGB Dual a 4-byte emulated count. The 48-byte forms pair the
timestamp with different registers, so the same bytes can mean
different clocks between emulators; roadmap section 3 defines one
neutral clock for conversion.

## Open items

- Pizza Boy A/C: closed source, format and RTC handling unknown.
- When RetroArch's `sort_savefiles_enable` default became `true`.
