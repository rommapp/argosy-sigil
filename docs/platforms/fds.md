# Famicom Disk System and NES

Status: located
Sigil names the files Nestopia keeps for a disk. It reads no NES or FDS header, and collect and restore don't cover these games.

## Identification

Sigil extracts no id from an NES cart or an FDS disk. Layout rows use the `fds` platform slug, and callers may pass `famicom_disk_system` for it.

## Save layouts

Row from the [layout table](../save-units.md). An NES cart core with no row of its own uses the default row (`{stem}.srm` primary; `{stem}.rtc` rtc), listed on [gb.md](gb.md#save-layouts).

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| `nestopia` | `{stem}.sav` primary when `nestopia_fds_savefile_format` = `sav_ups` (default); `{stem}.ups` primary when `ups`; `{stem}.ips` primary when `ips` | | emulator source |

A disk has no save RAM, so RetroArch writes no `{stem}.srm` under
Nestopia; the core writes the patch itself. A `{stem}.srm` beside it is
fceumm's full disk image from before a core switch, which Nestopia never
reads, so the row leaves it out. Builds before the format option
(2025-10-15) always wrote `{stem}.sav` as UPS, the same as the default
now.

## Save formats

There is no common FDS save format. fceumm keeps the whole modified disk
without its header; Mesen keeps an IPS patch against the original file;
Nestopia keeps a UPS or IPS patch against it. Each converts to the
others without loss, given the exact original disk.

## Open items

- Legacy Mesen libretro: whether the core writes `.sav` alongside RA's `.srm`.
