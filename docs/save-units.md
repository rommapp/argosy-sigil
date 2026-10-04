# Save units

A save unit is every file under an emulator's save root that belongs to
one game, named so a client can archive it and hash it the way the RomM
server will. `sigil_save_resolve()` takes a `sigil_save_request` and
returns a `sigil_save_unit`. Sigil never touches the filesystem: the
caller lists the root (plus the subfolders `sigil_save_layout_subdirs()`
names for the layout) and passes the relative paths in; hashing opens
members through the request's `open` callback and is skipped when it is
`NULL`.

Each emulator's files are a layout row. The rows are listed per system in
[platforms/](platforms/README.md#layouts); this page has the rules every
row follows. [sync.md](sync.md) covers collect and restore, which build on
units.

## Request

| Field | Meaning |
|---|---|
| `layout` | libretro core id (`genesis_plus_gx`, `melonds`) or emulator id. An id with no row uses the libretro default row. |
| `platform` | platform slug, or `NULL`. Rows limited to one platform match through the aliases below. |
| `content_name` | the name the emulator loaded: rom, `.m3u`, `.cue`, `.chd`, or `archive.zip#member.ext` |
| `result` / `features` | the `sigil_result` for the content, or `NULL` with `features` carried from an earlier extraction |
| `options` | core option key/value pairs; only the keys the layout row names matter |
| `listing` | relative paths under the root, `/` separated |
| `root_path` | the root's own path, or `NULL`. Layouts with profiles read where the root sits from it (see [Profiles](#profiles)) |
| `profile` | layouts with profiles: the profile whose saves to take, or `NULL` |

## Unit

| Field | Meaning |
|---|---|
| `key` | the stem for file layouts, `save_id` for folder layouts |
| `shape` | `SINGLE` one member travels raw; `MULTI` two or more travel as a flat zip with every member at the root; `FOLDER` a zip of the `save_id` folder. `NONE` when nothing is present. |
| `members` | present members in archive order, each with its root-relative path, archive entry name, role and area (`ACCOUNT` or `DEVICE` on layouts with profiles, `NONE` elsewhere) |
| `expected` | absent members the layout says this game should have: every applicable primary, plus the rtc member when `features` has `SIGIL_FEATURE_RTC` |
| `unkeyed` | shared files seen in the root that belong to every game at once; reported, never bundled |
| `artifact` | the file name the unit travels under: the member's own name for `SINGLE`, the first member's name plus `.zip` for `MULTI`, `<key>.zip` for `FOLDER` |
| `content_hash` | RomM `content_hash` of the artifact |
| `identity_hash` | the same hash over the non-rtc members, so a clock tick alone does not read as a new save |
| `alternates` | listed files the layout would take with other option values, each with those values; see [alternates](#alternates) |

Roles: `PRIMARY` is the member that names the unit, `SIDECAR` is a
core-owned companion file, `RTC` is `RETRO_MEMORY_RTC`.

Folder members are archived from the folder's parent, so the zip holds
`<folder>/<file>` the way a zipped save folder does.

## Stem

`sigil_content_stem()` applies RetroArch's `runloop_path_set_basename`
rule: the loaded path's file name without its extension. For
`archive.zip#member.ext` the member name is the stem, so a rom loaded
from inside a zip saves under the entry name, not the archive name. An
`.m3u` saves under the playlist name.

## Hash

The server hashes with MD5 (RomM `assets_handler.compute_content_hash`).
A raw file hashes as its bytes. A zip hashes as the MD5 of the lines
`<entry name>:<entry md5>`, one per file entry sorted by byte order,
joined with `\n` and no trailing newline, directories excluded
(`_compute_zip_hash`). Whether a
file is a zip is decided the way Python's `zipfile.is_zipfile` decides
it: an end-of-central-directory record inside the comment window at the
tail. Entry names are part of the hash, which is why the archive shape
is fixed per layout. A `.pure.zip` is one raw member and hashes as a
zip because the server does the same.

`identity_hash` is `content_hash` computed as if the rtc members were
absent. With one non-rtc member left it is that member's raw hash.

## Features

`sigil_result.features` (struct version 3) is a bitfield read from the
cart header. `SIGIL_FEATURE_RTC` says the cart carries a real-time clock,
so a libretro core exposes `RETRO_MEMORY_RTC` and RetroArch persists it
beside the save as `<stem>.rtc`. The header rules that set it are on the
[Game Boy](platforms/gb.md) and [SNES](platforms/snes.md) pages.

## Layout rows

Each row names one core (`layout`), optionally one platform, its member
templates, the shared files it may write, and the subfolders it writes
into. Template variables: `{stem}`, `{romset}` (same as the stem),
`{title_id}`, `{save_id}`, `{cart_size}`, `{nvram_version}`,
`{left_index}`, `{right_index}`, `{dc_vmu_id}`, `{disc_id}`,
`{pcsx_serial}`, `{gc_region}`. A template ending in `/` names a folder
whose whole subtree is the member. A template with an option key applies
only while that core option holds the given value; the row's default
flag says whether an absent option counts as holding it, so a caller
sends only the options it has changed. Expansion fails, and the member
is skipped, when a variable has no value.

`{cart_size}` comes from `genesis_plus_gx_cart_size` in the core's own
spelling: `128k` to `128Kbit`, `256k` to `256Kbit`, `512k` to `512Kbit`,
`1meg` to `1Mbit`, `2meg` to `2Mbit`, `4meg` to `4Mbit`, absent to
`4Mbit`. A cart file restore creates is that size (16 KiB for `128k` to
512 KiB for `4meg`), as is a Kronos cart for `kronos_addon_cartridge`,
whatever size the unit's cart was; saves that don't fit return
`SIGIL_ERR_NO_SPACE`. `{nvram_version}` is `opera_nvram_version` (default `0`).
`{left_index}` and `{right_index}` are `beetle_psx_hw_memcard_left_index`
(default `0`) and `beetle_psx_hw_memcard_right_index` (default `1`), or
the `beetle_psx_` keys of the same names on `mednafen_psx`.
`{dc_vmu_id}` is the Dreamcast product number (`title_id`) with each of
` /\:*?|<>` replaced by `_`, as flycast names a per-game VMU.
`{disc_id}` is `save_id` with only its letters and digits (`SLUS-01040`
gives `SLUS01040`), as a PSP EBOOT's `DISC_ID` names its save folder;
`title_id` when `save_id` is empty. A multi-disc set's later discs pass
the EBOOT's id as `save_id` ([PlayStation](platforms/psx.md#save-layouts)).
`{pcsx_serial}` is pcsx_rearmed's name for a disc: the letters and digits
of `raw_serial` as written (`title_id` when `raw_serial` is empty), cut at
nine, with a dash before the first digit. `slus_005.94` gives `slus-00594`
and `SLUSP012.06` gives `SLUSP-0120`.
`{gc_region}` is Dolphin's GameCube region folder from the region letter
ending the game code: `E` gives `USA`, `J` and `K` give `JAP`, any other
letter `EUR`. Dolphin reads the disc's region field, which follows the
letter on retail discs.

Every row but `vita_pops` was read from the core's source or its libretro
docs page; the names are the core's literals. The PSP and Vita firmware
isn't open, so `vita_pops` was checked on a PS Vita running PS1 games under
Adrenaline: the folder names, the files POPS writes, its `.vmp` signatures
(a new card from sigil is byte-identical to the console's), and that POPS
boots a folder sigil created from nothing.

Row platforms use the slugs in the platform table plus `segacd` and
`fds`. Callers may pass their own forms: `scd`, `sega_cd`, `sega-cd`,
`mega_cd`, `mega-cd`, `megacd` resolve to `segacd`; `sfc`, `sfam` to
`snes`; `ps1`, `playstation` to `psx`; `famicom_disk_system` to `fds`;
`dc` to `dreamcast`; `ngc`, `gc` to `gamecube`; `vita` to `psvita`.

## Alternates

A row reads the files its options select, and an absent option counts as
the core's current default. A save kept under another mode, or by an
older build of the core that wrote another file, is then not one of the
unit's members. sigil reports it in `alternates` on the unit and on
collect and restore results. Each alternate gives the file, whether every
game shares it, and the option values that take it. A client can ask the
user, or send those values and call again. sigil never picks one on its
own, and a client that ignores `alternates` gets the modern default.

The files are the row's own templates expanded for this game with the
values sent, so a file another option value would rename (a different
card index) is not found. Folder members and layouts with profiles report
none.

Beetle Saturn is the case this exists for. Builds from before 2026-05-11
keep the internal save in `{stem}.bkr` and have no save-method option. A
collect with no options takes `{stem}.srm`, finds nothing, and reports
`{stem}.bkr` with `beetle_saturn_save_method=mednafen`. Sending that value
reads and writes the `.bkr`, whatever the core's age. See
[Saturn](platforms/saturn.md#save-layouts).

## Profiles

The Switch, Wii U, Vita and PS3 emulators keep a game's saves per user
profile, and the Switch and Wii U also keep saves every profile on the
device shares. Their rows name each folder a game's saves sit in, with the
area it belongs to:

| Layout | Account | Device | Profile list |
|---|---|---|---|
| yuzu forks | `nand/user/save/0000000000000000/{profile}/{save_id}/` | the same under the all-zero user | `nand/system/save/8000000000000010/su/avators/profiles.dat`; the folder is the profile's UUID with its bytes reversed |
| `cemu` | `mlc01/usr/save/00050000/{save_id}/user/{profile}/` | `user/common/` and `meta/` beside it | `mlc01/usr/save/system/act/{profile}/account.dat` (`PersistentId`, `MiiName`) |
| `vita3k` | `ux0/user/{profile}/savedata/{save_id}/` | | `ux0/user/{profile}/user.xml` |
| `rpcs3` | `dev_hdd0/home/{profile}/savedata/{save_id}*/` | | `dev_hdd0/home/{profile}/localusername` |

The profile comes from the request, else from the root path when the root
lies inside a profile's folder, else from the emulator's list when it holds
one profile. Folders the list doesn't name are not a profile's. With two or
more profiles and none picked, collect returns `SIGIL_ERR_AMBIGUOUS` when one
of them holds the game's saves, and restore when the unit holds an account
save, with `problem` listing them as `id name`, one per line; a device save
alone needs no profile. A client that doesn't know which profile the user
plays as should ask them: `sigil_save_profiles()` lists the emulator's
profiles without a game, and the result's `profiles` holds the same list.
Pass the answer as `profile` and store it per user.
With the list out of the root's reach, the profile folders holding files
stand for it. Results carry `profiles` and the `profile` chosen.

A unit never carries a profile id, so it restores under any profile. A
yuzu-fork unit holds `<save_id>/...` for the account save and
`device/<save_id>/...` for the device save. A Cemu unit holds
`<save_id>/meta/`, `<save_id>/user/account/` and `<save_id>/user/common/`; an
older unit naming the account folder by its id restores as the account
save. The size file each yuzu fork keeps in a save folder
(`.yuzu_save_size`, `.citron_save_size`, `.sudachi_save_size`,
`.suyu_save_size`) is never collected, written or removed.

Restore replaces each area the unit carries and leaves the others as they
are: a unit with no device save leaves the device's alone. It writes only
the files whose bytes differ, removes the area's files the unit lacks
through `remove`, and refuses with `SIGIL_ERR_CONFLICT` when an area changed
since its last sync. The state keeps the account area's hash per profile and
the device area's per device, so another profile's sync doesn't read as a
change. A unit member no folder of the game takes, or one whose folder lies
outside the root, is `SIGIL_ERR_NO_TARGET` naming it; Cemu's `meta/`, which
Cemu writes again by itself, is skipped instead.

The root may sit anywhere around the emulator's base folder, the one
holding the row's top folder (`nand`, `mlc01`, `ux0`, `dev_hdd0`). Above it,
sigil finds the base in the listing; two candidates are
`SIGIL_ERR_AMBIGUOUS` naming them. Inside it, `root_path` says where: a root
inside a profile's folder picks that profile, and folders outside the root
are out of reach. A root inside one of the game's save folders is
`SIGIL_ERR_INVALID_ARG`, since collect would take part of the save for all
of it. `sigil_save_base()` returns the base and profile for a path, and the
bindings re-root there themselves; `sigil_save_layout_top()` names the top
folder, so a binding can find a base below the root it was given.
