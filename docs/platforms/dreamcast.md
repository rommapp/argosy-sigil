# Dreamcast

Status: synced
sigil reads the product number from a Dreamcast disc and collects and restores flycast's VMUs, per game and shared.

## Identification

| Slug | Platform | Inputs | `title_id` example | `usage` | Status |
|---|---|---|---|---|---|
| `dreamcast` | Dreamcast | `.chd`, `.iso`, data track `.bin` (`.gdi` track 3) | `T-8111N` (IP.BIN product number) | file-prefix | experimental |

Argosy's shorter identifier `dc` resolves as an alias of `dreamcast`, for
extraction and for layout rows. The extractor is flagged `experimental`
(see [Identification](../identification.md)).

### IP.BIN product number, found by scanning

The boot
header IP.BIN starts the data track: `SEGA SEGAKATANA ` at offset 0,
then a 10-byte ASCII product number at 0x40 (`T-8111N`, `MK-51035`,
`HDR-0038`) padded with trailing spaces. Flycast trims that padding and
then truncates at the first NUL, because some discs leave garbage after
the terminator; sigil reproduces that order exactly, since the result is
the name flycast gives the per-game VMU file. Composing the filename
(the `.A1.bin` port suffix) is the consumer's job, and because a game
can own more than one port's VMU the `usage` is `file-prefix`.

A GD-ROM keeps its data track third and a CHD packs tracks contiguously
from frame 0, so IP.BIN is neither at offset 0 nor at the physical
GD-area LBA 45000. Sigil checks offset 0 first, which covers a raw data
track or a plain ISO, then scans sector boundaries for the magic across
the first 20000 frames. Tracks 1 and 2 live in the single-density area,
which spans the first four minutes of the disc (18000 frames), so the
bound holds for any conformant dump while keeping a miss cheap. A `.gdi`
is a text index naming its track files rather than a disc image, so pass
`track03.bin` (or a CHD) for binary extraction. `.bin` is ambiguous
across platforms and needs an explicit `dreamcast` hint; raw 2352-byte
MODE1 tracks are cooked to 2048 on the way in. `.gdi` and `.cdi` sniff
as Dreamcast so the platform resolves without a hint, but neither
container's own layout is parsed: a `.cdi` only extracts when its
sectors happen to land on 2048-byte boundaries.

## Save layouts

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| `flycast` | `{dc_vmu_id}.A1.bin` primary when `reicast_per_content_vmus` = `VMU A1` or `All VMUs`, else the legacy `{stem}.A1.bin` when only that exists; `{dc_vmu_id}.{A2..D2}.bin` (or legacy `{stem}.{port}.bin`) sidecars when `All VMUs` | `vmu_save_{A1..D2}.bin` when `disabled` (default). The core keeps them in `<system>/dc/`, so pass that folder as the save root. Under `VMU A1` the other ports stay there too and don't sync | emulator source |
| `flycast_standalone` | `{dc_vmu_id}_vmu_save_A1.bin` primary, or the legacy `{stem}_vmu_save_A1.bin`, when `PerGameVmu` = `yes` (default) | `vmu_save_A1.bin` when `PerGameVmu` = `no`; `vmu_save_{A2..D2}.bin` | emulator source |

`{dc_vmu_id}` is the Dreamcast product number (`title_id`) with each of
` /\:*?|<>` replaced by `_`, as flycast names a per-game VMU. For
`flycast_standalone` the save root is the VMU folder.

The flycast core copies a legacy `{stem}.A1.bin` to the product-number
name and deletes the old file, so a client watching the old name sees it
disappear.

## Sync

The generic collect and restore rules, holding units, claims and state
are in [Sync](../sync.md).

`sigil_card_list` reads Dreamcast VMUs (`SIGIL_CARD_FORMAT_DREAMCAST_VMU`,
`.bin` or `.vmu`, a 128 KiB VMU flash image).

On Dreamcast
it is the game's VMU A1 (`vmu_A1.bin`) when it has saves there alone, else
a zip of `vmu_A1.bin` to `vmu_D2.bin` as present. Every
volume in a unit is raw, whatever form the emulator stores it in; restore
writes each file back in the emulator's form, and a
file the emulator hasn't created yet in the form and size its layout row
names.

Saturn, Sega CD and Dreamcast saves carry no game id. Every save on a
per-game volume (Beetle Saturn's `<stem>.srm` and `.bcr`, genesis_plus_gx
with `system_bram` = `per game`, flycast's per-game VMUs) is the game's. On
a shared volume (genesis_plus_gx's default `scd_U.brm`, Beetle's shared
volumes, Yaba Sanshiro's `backup.bin`, flycast's `vmu_save_A1.bin`) a save
belongs to the game the user claimed it for, else to the game the state
learned it belongs to, else, in managed mode, to the game the volume was
last swapped in for, else to the game the save-name table gives it by one
of the ids in `title_id` or `game_ids` (`src/save_names.c`; Saturn and
Sega CD product codes as the disc header spells them, Dreamcast product
numbers). The rest come back in `holding`, which the client
keeps where the user can claim them; `holding` and the unit both go up
before the state is stored.

`claimed` takes the names from `unowned` the user gave this game.

In managed mode, restore swaps each shared volume for one holding only the
game's saves. It refuses
with `SIGIL_ERR_UNCOLLECTED` while the volume holds a save that isn't in
the last holding unit or doesn't match its game's last collect: call
collect for the game that ran last, upload, then restore again. In
unmanaged mode, restore never swaps. It replaces only the game's saves,
and only when the volume is as the last collect saw it; a collect that
then finds the old saves back sets `restore_again`.

A managed restore also refuses a shared volume holding any
corrupt save, which the swap would drop; unmanaged keeps it in place.
Restore returns `SIGIL_ERR_NO_TARGET` for a VMU port flycast doesn't keep
per game. On `SIGIL_ERR_NO_SPACE`, `blocks_short` is 0 when other saves
hold the blocks a Dreamcast game file must start at.

## Open items

- The extractor is `experimental`: it hasn't been validated against real-world samples.
- A `.gdi` or `.cdi` container's own layout is not parsed.
- redream has no layout row.
