# Game Boy and Game Boy Color

Status: synced

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

`SIGIL_FEATURE_MBC2`: header byte `0x147` is `0x05` (MBC2) or `0x06`
(MBC2+BATTERY).

## Save layouts

Rows from the [layout table](../save-units.md). The default row applies to any cart core with no row of its own.

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| default | `{stem}.srm` primary; `{stem}.rtc` rtc | | emulator source |
| `vba_next`, `gpsp` | `{stem}.srm` primary | | emulator source |
| `mgba_standalone`, `vbam_standalone`, `sameboy_standalone`, `gearboy_standalone` | `{stem}.sav` primary: the RAM, then the clock on a clock cart | | emulator source |

`vba_next` and `gpsp` expose no clock region, so their rows have no
`.rtc`.

The standalone emulators keep the clock at the end of the `.sav` in the
48-byte VBA layout: VBA-M, SameBoy and Gearboy pair the time with the
current registers, mGBA with the latched ones. None writes a separate
`.rtc`. mGBA, VBA-M and SameBoy save beside the ROM unless a save folder
is set (mGBA `savegamePath`, VBA-M `BatteryDir`; SameBoy has no setting).
Gearboy saves in its own folder by default (`SaveFilesDirOption`), its
preferences folder unless set to the ROM's. Pass whichever folder holds
the `.sav` as the save root. mGBA leaves an empty `.sav` for a cart
without RAM; it holds no save.

## RetroArch behaviour

These apply to every libretro core that exposes save memory.

- RetroArch writes `RETRO_MEMORY_SAVE_RAM` as `{stem}.srm` and
  `RETRO_MEMORY_RTC` as `{stem}.rtc`, the bytes exactly as the core
  exposes them, with no header.
- With `save_file_compression` on (off by default), RetroArch wraps the
  file in RZIP, which starts with `#RZIPv`. Collect, restore and
  `sigil_save_hash` read through it ([sync](../sync.md#compressed-saves)).
  RetroArch reads uncompressed files either way.
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

## Sync

A cart without a clock travels as its RAM, one file named
`{stem}.srm`. A cart with one (`SIGIL_FEATURE_RTC`) travels as a zip of
`save.sram` and, when the emulator kept a clock, `clock.rtc`: the current
registers valid at the UTC time in its stamp, in the 48-byte VBA layout.
The identity covers the RAM only, so a clock that ticked isn't a new
save, and a clock cart's identity is the same with or without
`clock.rtc`.

Collect reads the clock in the format of the core the request names:
`gambatte` 8 bytes, `mgba` 48 (its time paired with the latched
registers), `sameboy` 32, `tgbdual` 4, `vbam` 48. For another core it
goes by the `.rtc`'s size, 48 bytes reading as VBA. Restore writes the RAM
to the `.srm` and the clock in the format of the `.rtc` already there,
else the core's. For a core sigil has no format for and no `.rtc` there,
it writes the RAM alone. A unit without `clock.rtc` leaves the clock on
disk as it is.

MBC2's RAM is 512 four-bit cells, stored three ways: mGBA packs two cells
to a byte (256 bytes, the even cell in the low nibble); SameBoy, VBA-M,
Gearboy and Mesen2 keep one per byte in the low nibble (512, with 0xF, 0x0
or whatever the game wrote above it); gambatte and TGB Dual keep the
cart's whole 8 KiB range, the cells in its first 512 bytes. None of them
reads another's form: VBA-M stops saving the cart for the session when
the file isn't 512 bytes, and mGBA reads a 512-byte file as packed pairs.
The unit holds the 512 cells, each in the low nibble with 0xF above it,
so the same save gives the same unit from any of them. Restore writes the
form of the file already there, else the core's: 256 for `mgba` and
`mgba_standalone`, 8 KiB for `gambatte` and `tgbdual` (0xFF past the
cells), 512 for any other. A 256 or 512-byte RAM is taken as MBC2's
whatever the request says, since no other cart has RAM of that size; an
8 KiB RAM only with `SIGIL_FEATURE_MBC2`.

On the standalone rows restore writes one `.sav`: the RAM, and on a clock
cart the unit's clock in the emulator's footer, or the clock already there
when the unit has none. A cart without a clock never gets a footer, since
VBA-M stops saving a cart whose file runs past its RAM.

Restore also reads a `.sav` with the clock appended and a zip of an
emulator's `.srm` and `.rtc`, as clients uploaded before sigil.

## Open items

- A clock format can't hold all of the neutral clock: gambatte drops halt and carry, Mesen2 the latched registers, TGB Dual the wall time. Restore writes what the format holds and doesn't report what it dropped.
- A header saying 2 KiB of RAM (`0x149` = `0x01`, unused by licensed carts) gives 8 KiB in mGBA and Gearboy and 2 KiB in SameBoy and VBA-M; sigil doesn't convert between them.
- A game that writes MBC2's RAM through its mirrors (`0xA200` to `0xBFFF`) lands elsewhere in gambatte's and TGB Dual's 8 KiB file than in the 512-byte emulators; sigil takes the first 512 bytes.
- HuC3, TAMA5 and TPP1 clocks are left in the RAM file as each emulator wrote them, not converted.

- Pizza Boy A/C: closed source, format and RTC handling unknown.
- When RetroArch's `sort_savefiles_enable` default became `true`.
