# Saturn

Status: synced
sigil collects and restores the backup RAM volumes of Beetle Saturn, Kronos, yabause and Yaba Sanshiro, internal memory and cart, per game and shared.

## Identification

sigil has no Saturn extractor. Build the result from the stored `title_id`
and `features` as for Sega CD (see [Identification](../identification.md)).
Layout rows take `saturn` as the platform slug.

## Save layouts

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| `mednafen_saturn` | `{stem}.srm` primary when `beetle_saturn_save_method` = `libretro` (default); `{stem}.bkr` primary when `mednafen` and `beetle_saturn_shared_int` = `disabled` (default); `{stem}.bcr` sidecar when `beetle_saturn_shared_ext` = `disabled` (default); `{stem}.smpc` sidecar when `shared_int` = `disabled` | `mednafen_saturn_libretro_shared.bkr` when `shared_int` = `enabled` and `save_method` = `mednafen`; `.smpc` when `shared_int` = `enabled`; `.bcr` when `shared_ext` = `enabled`. A new `.bkr` is 32 KiB and a new `.bcr` a 512 KiB cart; restore refuses with `SIGIL_ERR_NO_SPACE` saves that don't fit | emulator source; live save files |
| `kronos` | `kronos/saturn/{stem}.ram` primary when `kronos_use_beetle_saves` = `disabled` (default); `{stem}.bkr` primary when `enabled`; `kronos/saturn/{stem}-ext512K.ram`, `-ext1M.ram`, `-ext2M.ram` or `-ext4M.ram` sidecar by `kronos_addon_cartridge` (`512K_backup_ram` default, `1M_`, `2M_`, `4M_backup_ram`); `{stem}.bcr` sidecar when `kronos_use_beetle_saves` = `enabled`, always a 512 KiB cart. Mode follows the content, not an option: a disc or `.m3u` is Saturn; an ST-V romset writes `kronos/stv/`, which has no row yet | | emulator source; live save files |
| `yabause` | `{stem}.srm` primary, 64 KiB byte-expanded | | emulator source |
| `yabasanshiro` | | `yabasanshiro/backup.bin`, one 8 MiB byte-expanded volume for every game | emulator source |

Kronos's files sit under `kronos/saturn/` and Yaba Sanshiro's under
`yabasanshiro/`, which `sigil_save_layout_subdirs()` names. The yabause
core writes its `.srm` itself.

A cart file restore creates is that size (16 KiB for `128k` to
512 KiB for `4meg`), as is a Kronos cart for `kronos_addon_cartridge`,
whatever size the unit's cart was; saves that don't fit return
`SIGIL_ERR_NO_SPACE`.

The internal save's file depends on the Beetle Saturn build, and with no
option sent, `mednafen_saturn` takes the current one:

| Build | Internal save | Send |
|---|---|---|
| 2026-05-26 on | `{stem}.srm`, or `{stem}.bkr` with `beetle_saturn_save_method` = `mednafen` | nothing, or the user's value |
| 2026-05-25 | `{stem}.bkr` unless the option says `libretro` | `beetle_saturn_save_method` = `mednafen` |
| 2026-05-11 to 05-24 | both, the same bytes; RetroArch loads `.srm` over `.bkr` | nothing |
| before 2026-05-11 | `{stem}.bkr` alone; the option doesn't exist | `beetle_saturn_save_method` = `mednafen` |

sigil reads only the request, never the core, so the value works on
builds that predate the option. A collect or restore with no options on
a root holding `{stem}.bkr` and no `{stem}.srm` reports the `.bkr` in
`alternates` with that value ([alternates](../save-units.md#alternates)).

## Sync

The generic collect and restore rules, holding units, claims and state
are in [Sync](../sync.md).

`sigil_card_list` reads Saturn backup RAM (`SIGIL_CARD_FORMAT_SATURN_BACKUP`,
`.bkr`, `.bcr`, `.srm` or `backup.bin`, internal or cart).

On Saturn and Sega CD it is the game's internal
volume (`backup.ram`) when it has internal saves alone, else a zip named
`<stem>.zip` holding `backup.ram` and `cart.ram` as present. The member name, not the
size, says which device a volume is, since a 4 MiB Saturn volume can be
Yaba Sanshiro's internal memory or a 32 Mbit cart. Every
volume in a unit is raw, whatever form the emulator stores it in; restore
writes each file back in the emulator's form (gzip, byte expansion), and a
file the emulator hasn't created yet in the form and size its layout row
names. Every Saturn core but Yaba Sanshiro keeps 32 KiB of internal memory,
so a unit whose internal saves need more returns `SIGIL_ERR_NO_SPACE` there
with the blocks they lack.

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

## Open items

- The save-name table covers some games only: a scan of Saturn discs found the names a game writes for about 44% of them, and Sega CD is unmeasured. A save no name matches comes back in `holding` for the user to claim.
- ST-V under Kronos (`kronos/stv/{stem}.ram`) is not a Saturn volume and has no row.
