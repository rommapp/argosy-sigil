# Sega CD / Mega CD

Status: synced
sigil collects and restores genesis_plus_gx's backup RAM volumes, per game and shared per BIOS region, with the internal BRAM and the RAM cart.

## Identification

sigil has no Sega CD extractor. Rebuild a result later, or build for a
platform sigil cannot extract (Sega CD returns
`SIGIL_ERR_UNKNOWN_PLATFORM`), by filling a zeroed result with the stored
`title_id`, `save_id` and `features` (see [Identification](../identification.md)).

Row platforms use the slugs in the platform table plus `segacd` and
`fds`. Callers may pass their own forms: `scd`, `sega_cd`, `sega-cd`,
`mega_cd`, `mega-cd`, `megacd` resolve to `segacd`.

## Save layouts

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| `genesis_plus_gx` | `{stem}.brm` primary when `genesis_plus_gx_system_bram` = `per game`; `{stem}_{cart_size}_cart.brm` sidecar when `genesis_plus_gx_cart_bram` = `per game`. No `.srm`: the core writes none for a disc | `scd_E.brm`, `scd_U.brm`, `scd_J.brm` when `system_bram` = `per bios` (default), one per disc region; `{cart_size}_cart.brm` when `cart_bram` = `per cart` (default) | emulator source; live save files |

`{cart_size}` comes from `genesis_plus_gx_cart_size` in the core's own
spelling: `128k` to `128Kbit`, `256k` to `256Kbit`, `512k` to `512Kbit`,
`1meg` to `1Mbit`, `2meg` to `2Mbit`, `4meg` to `4Mbit`, absent to
`4Mbit`. A cart file restore creates is that size (16 KiB for `128k` to
512 KiB for `4meg`), as is a Kronos cart for `kronos_addon_cartridge`,
whatever size the unit's cart was; saves that don't fit return
`SIGIL_ERR_NO_SPACE`.

genesis_plus_gx reads and writes its `.brm` files only when a game loads
and unloads, so a crash loses that session's backup RAM writes. A Mode 1
cartridge's own SRAM goes through `.srm` as usual.

## Sync

The generic collect and restore rules, holding units, claims and state
are in [Sync](../sync.md).

`sigil_card_list` reads Sega CD backup RAM (`SIGIL_CARD_FORMAT_SEGACD_BRAM`,
`.brm` or `.srm`, internal or cart).

On Saturn and Sega CD it is the game's internal
volume (`backup.ram`) when it has internal saves alone, else a zip named
`<stem>.zip` holding `backup.ram` and `cart.ram` as present. Every
volume in a unit is raw, whatever form the emulator stores it in; restore
writes each file back in the emulator's form (gzip, byte expansion), and a
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

`holding` (collect, Saturn and Sega CD) is a zip of the saves on a shared
volume with no known owner, and `unowned` names them. `claimed` takes the
names from `unowned` the user gave this game.

In managed mode, restore swaps each shared volume for one holding only the
game's saves, keeping the file's form (gzip, byte expansion). It refuses
with `SIGIL_ERR_UNCOLLECTED` while the volume holds a save that isn't in
the last holding unit or doesn't match its game's last collect: call
collect for the game that ran last, upload, then restore again. In
unmanaged mode, restore never swaps. It replaces only the game's saves,
and only when the volume is as the last collect saw it; a collect that
then finds the old saves back sets `restore_again`.

A managed restore also refuses a shared volume holding any
corrupt save, which the swap would drop; unmanaged keeps it in place.
A volume file sigil can't read as what its path holds (no card magic, cut
short, an internal volume where a cart goes) is damaged. Restore returns
`SIGIL_ERR_NO_TARGET` for a `cart.ram` when the core's settings keep no
cart file.

genesis_plus_gx picks `scd_E`, `scd_U` or `scd_J` by the disc's region.
sigil takes the region from `genesis_plus_gx_region_detect` when it is
forced, else from the content file name's first region tag, such as
`(USA)`, else from the only one of the three files present. Otherwise
collect and restore return `SIGIL_ERR_NOT_FOUND`.

The Sega CD backup RAM reader (`src/card_segacd.c`) takes its ECC layout
table and format-block bytes from superctr/buram, Copyright (c) 2022 Ian
Karlsson, used under the MIT license; the notice is in
[licenses/buram-MIT.txt](../../licenses/buram-MIT.txt).

## Open items

- The save-name table covers some games only: a scan of Saturn discs found the names a game writes for about 44% of them, and Sega CD is unmeasured. A save no name matches comes back in `holding` for the user to claim.
