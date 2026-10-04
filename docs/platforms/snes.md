# Super Nintendo

Status: located
Sigil reads the cart header and names the files each core keeps for a cart. Collect and restore don't cover these carts.

## Identification

| Slug | Platform | Inputs | `title_id` example | `usage` | Status |
|---|---|---|---|---|---|
| `snes` | Super Nintendo | `.sfc`, `.smc` | none; sets `features` | file-prefix | |

Game Boy and SNES carts carry no title id. Sigil validates the header
and reports what the cart holds in `features` (see
[Save units](../save-units.md)); `title_id` and `save_id` stay empty and
the emulator names the save after the content file.

Callers may pass `sfc` or `sfam` for `snes`.

### Features

`SIGIL_FEATURE_RTC` on `snes`: the internal header sits at `0x7FC0`
(LoROM), `0xFFC0` (HiROM) or `0x40FFC0` (ExHiROM), plus a 512-byte skew
when the file size leaves that remainder (copier header). The header's
checksum and complement pair has to xor to `0xFFFF`; bases are tried
deepest first because an ExHiROM image also carries plausible bytes at
the HiROM base. `(ROMType << 8) | ROMSpeed` from bytes `0x16` and `0x15`
equal to `0x5535` (S-RTC) or `0xF93A` (SPC7110 with RTC) sets the flag,
the same carts snes9x enables its clock chips for.

RetroArch writes `RETRO_MEMORY_SAVE_RAM` as `<stem>.srm` and
`RETRO_MEMORY_RTC` as `<stem>.rtc`; its other rules are on
[Game Boy](gb.md#retroarch-behaviour).

## Save layouts

Rows from the [layout table](../save-units.md). The default row applies to any cart core with no row of its own.

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| default | `{stem}.srm` primary; `{stem}.rtc` rtc | | emulator source |
| `bsnes` | `{stem}.srm` primary; `{stem}.rtc` rtc; `{stem}.psr` sidecar | | emulator source |

bsnes writes all three files itself. `{stem}.rtc` is the 16-byte clock of
an S-RTC or SPC7110 cart with an Epson RTC (Daikaijuu Monogatari II,
Tengai Makyou Zero); the cart header marks those carts, so their result
carries `SIGIL_FEATURE_RTC` and locate expects the `.rtc`. `{stem}.psr` is
the BS-X Satellaview cart's download RAM.

## Save formats

`.srm` is the same bytes in snes9x, bsnes, Mesen2 and Mesen-S. The `.rtc`
formats differ: 20 bytes in snes9x, 16 in bsnes and 24 in Mesen2. Only
the S-RTC game and the SPC7110 game with a clock are affected.

## Open items

- The conversion mapping between snes9x (20 B), bsnes (16 B) and Mesen (24 B) `.rtc`.
