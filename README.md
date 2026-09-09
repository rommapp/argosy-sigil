# argosy-sigil

A native C helper library that derives the game-native serial / title ID
from a ROM file. Designed for applications that want per-game save and
state files instead of per-platform — pair a local save to the right
upstream game record by reading the platform's own ID out of the ROM,
not by guessing from filenames. It also resolves the set of files an
emulator keeps for one game so a client can archive and hash them the
way the RomM server does ([Save units](#save-units)).

Integrating is three calls: identify the game, locate its saves, hash
them. One page per language walks them with what each call requires,
takes optionally, and returns: [Kotlin](docs/kotlin.md),
[Python](docs/python.md), [Go](docs/go.md), [C](docs/c.md).

## What it does

You hand sigil a path to a ROM. Sigil reads the platform-native
identifier directly from the disc/cart binary and hands back:

- `title_id` — the canonical identity of the game (`ULUS10064`,
  `SLUS-12345`, `0100ABCD12345000`). Use this for matching game
  records, deduplication, RomM/IGDB lookups, UI display.
- `save_id` — **the literal on-disk save folder/file name the emulator
  will create.** Use this for any filesystem operation against the
  save directory. For most platforms it equals `title_id`. For PS2 it
  diverges (`title_id=SLUS-20152`, `save_id=BASLUS-20152`) because the
  game's runtime prefixes the serial with a region letter
  (`BA`/`BE`/`BI`) and appends a per-artifact suffix (`AC04`, `SYS`).
  save_id is the region-prefixed stem and `usage` is `folder-prefix`,
  so consumers enumerate every folder starting with it. For the 3DS,
  `save_id` is itself a `/`-separated path (`00040000/00033500`, the
  16-hex title id split into two 32-bit halves) and `usage` is
  `folder-split` to flag that: the consumer creates the nested folders
  and never has to know where to split. Both 3DS segments are lowercase
  hex because that is the case Azahar writes them in, and storage that
  distinguishes case would otherwise hold two of every directory;
  `title_id` and `raw_serial` stay uppercase. Everything above it (a user
  directory, `sdmc/Nintendo 3DS`, the `title/` root, per-install id
  folders) is the emulator's prefix and the consumer's to supply,
  because the same title differs per emulator.
  When only a filename is available and it carries the low half of a 3DS
  id alone (`[0011C500]`), no path can be formed and `save_id` stays
  empty; the high half is `0004xxxx`, not a fixed `00040000`, so it is
  never assumed.
  Wii and Wii U diverge the same way and for the same reason: Dolphin
  writes `Wii/title/00010000/525a4445` and Cemu writes
  `mlc01/usr/save/00050000/1010ec00`, both with `{:08x}`, so `save_id` is
  the lowercase form of `title_id` on those two platforms
  (`title_id=525A4445`, `save_id=525a4445`). GameCube keeps the uppercase
  form because its artifacts are `.gci` files matched by prefix, not a
  NAND directory. The original Xbox diverges for the opposite reason:
  there the id is a 32-bit number and the two fields are two renderings
  of it. `save_id` is the raw hex the console names its directory after
  (`E:\UDATA\4D530064`), while `title_id` is the serial everything else
  prints, two publisher letters and the low half in decimal
  (`title_id=MS-100`, `save_id=4D530064`).
- `raw_serial` — the ID exactly as it appears in the binary, before
  any normalization (`ULUS-10064`, `SLUS_123.45`, `RZTE`). Mostly
  useful for logging.
- `usage` — how the platform uses `save_id` to lay out save artifacts
  on disk (one folder per game, multiple folders sharing a prefix,
  one file, multiple files sharing a prefix).
- `source` — `binary` if the ID came from the file content
  (high-confidence, lockable) or `filename` if it had to fall back to
  scanning the filename for a community-naming bracket pattern.
- `experimental` — `1` for extractors that haven't been validated
  against real-world samples (PS3, Xbox, Xbox 360, Dreamcast, PSP-via-CSO at
  time of writing). Consumers should surface this to users so a low-confidence
  result can be flagged in UI / logs.

Persist `save_id` and `usage` alongside `title_id` on your game
record once you extract them — `title_id` doesn't change, and re-reading the disc to
recompute `save_id` on every save sync is wasteful.

## What it isn't

- Not a hash-based game identifier. CRC32/MD5/SHA-1 matching against
  No-Intro / Redump / RetroAchievements is a different problem;
  hashing tells you "which dump is this," sigil tells you "what does
  the platform call this game."
- Not a save manager or sync client. Sigil tells you the ID; what you
  do with it (find a save folder, upload it somewhere, restore it) is
  on you.
- Not a generic ROM info library. Sigil extracts one thing — the
  identifier the platform's own save/state subsystem keys on. It does
  not give you region, language, version, hash, header dump, etc.

## Status

Pre-1.0. The API may evolve before 1.0. Every public struct has a
`struct_version` field so new fields can be added without breaking
existing consumers — but the function signatures and existing field
layouts can still change. Consumers vendoring sigil should pin a
specific commit; once 1.0 ships the C ABI freezes.

## License

MPL-2.0. See [LICENSE](LICENSE) for the full text and
[TRADEMARKS.md](TRADEMARKS.md) for the project naming policy. Anyone
can use sigil in any application (proprietary or open); modifications
to sigil's own files must remain MPL-2.0.

## Supported platforms

| Slug | Platform | Inputs | `title_id` example | `usage` | Status |
|---|---|---|---|---|---|
| `psp` | PSP | `.iso`, `.chd`, `.cso` / `.ciso` | `ULUS10064` | folder-prefix | `.cso` experimental |
| `psx` | PlayStation | `.iso`, `.bin`, `.chd` | `SLUS-12345` | file-prefix | |
| `ps2` | PlayStation 2 | `.iso`, `.chd` | `SLUS-20675` | folder-prefix | |
| `ps3` | PlayStation 3 | `.iso` (needs hint), game folder (recursive) or `.sfo` | `BLUS31426` (TITLE_ID from PARAM.SFO) | folder-prefix | experimental |
| `psvita` | PS Vita | `.zip` dump, extracted folder, or `param.sfo` | `PCSE12345` (TITLE_ID from `sce_sys/param.sfo`) | folder-exact | filename fallback when no `param.sfo` is reachable |
| `switch` | Nintendo Switch | `.nsp`, `.xci` | `0100ABCD12345000` | folder-exact | |
| `3ds` | Nintendo 3DS | `.3ds`, `.cci`, `.cxi`, `.app`, `.z3ds`, `.zcci`, `.zcxi` | `0004000000123456` | folder-split | `.3dsx` / `.z3dsx` / `.elf` / `.axf` are homebrew and carry no title id |
| `wii` | Wii | `.iso`, `.rvz`, `.wbfs` | `525A5445` (hex of ASCII gameId) | folder-exact | |
| `wiiu` | Wii U | `.wua` | `10143500` (last 8 of folder name) | folder-exact | |
| `gamecube` | GameCube | `.iso`, `.rvz`, `.wbfs` | `475A4C45` (hex of ASCII gameId) | file-prefix | |
| `xbox` | Xbox | `.xiso`, `.xiso.iso`, `.iso` (needs hint), extracted game folder or `.xbe` | `TT-027` (XBE certificate title id) | folder-exact | experimental |
| `xbox360` | Xbox 360 | `.zar`, `.iso` (needs hint), extracted game folder or `.xex` | `4D5307DC` (4-byte XEX title_id, hex) | folder-exact | experimental |
| `dreamcast` | Dreamcast | `.chd`, `.iso`, data track `.bin` (`.gdi` track 3) | `T-8111N` (IP.BIN product number) | file-prefix | experimental |
| `gb` | Game Boy | `.gb`, `.sgb` | none; sets `features` | file-prefix | |
| `gbc` | Game Boy Color | `.gbc` | none; sets `features` | file-prefix | |
| `snes` | Super Nintendo | `.sfc`, `.smc` | none; sets `features` | file-prefix | |

Game Boy and SNES carts carry no title id. Sigil validates the header
and reports what the cart holds in `features` (see
[Save units](#save-units)); `title_id` and `save_id` stay empty and
the emulator names the save after the content file.

The slugs are stable. Argosy's shorter internal identifiers (`dc`,
`ngc`, `gc`, `vita`, `n3ds`, `nsw`, `x360`, `xbx`, `sfc`, `sfam`)
resolve as aliases of the canonical slugs above.

A `.zip` holding any of the formats above is read in place, with no
extraction step: sigil opens the archive's member, resolves the platform
from it, and runs the normal extractor against it. Pass a platform hint
when the inner name is itself ambiguous (a bare `.iso`), exactly as you
would for a loose file. `.wua` and `.zar` are both ZArchive and are read
the same way. See [Container notes](#container-notes) for what each costs
and how the member is chosen.

The C API uses the `sigil_platform` enum; `sigil_platform_from_slug()`
converts strings if your binding accepts user input.

## `usage` — what to do with `save_id`

| Value | Meaning | Example |
|---|---|---|
| `folder-exact` | One folder per game named exactly `save_id` | `Switch/saves/0100ABCD12345000/` |
| `folder-prefix` | Multiple folders per game, all starting with `save_id` and a profile/slot suffix. Consumers MUST enumerate and bundle all matches. | PSP: `ULUS10064DATA00`, `ULUS10064SETTINGS`; PS2: `BASLUS-20642SYS`, `BASLUS-20642RD0` |
| `file-exact` | One file per game named with `save_id` | rare; emulator-specific |
| `file-prefix` | Multiple files per game, all containing `save_id` in the basename | GameCube GCI: `<maker>-<gameId>-<name>.gci` (e.g. `01-GZLE-Animal Crossing.gci`) |
| `folder-split` | `save_id` is a `/`-separated nested path, not a flat name; consumer creates the intermediate folders | 3DS: `save_id` = `00040000/00033500` |

Treating a `prefix` platform as `exact` silently misses every save for
that platform — sigil emits this classification so dispatch is correct
without you re-deriving it.

## Where saves land on Android

`save_id` names one component of a path. Everything above it belongs to
the emulator and differs per app, so a consumer needs both halves before
it can find a file. The roots below all sit under
`/storage/emulated/0/Android/data/<package>/files/`.

**PS Vita — Vita3K** (`org.vita3k.emulator`).
`vita/ux0/user/00/savedata/<save_id>/`, one directory per title, matching
`folder-exact`. The user id is fixed at `00`: `io.cpp` redirects
`savedata0:` to `ux0:user/00/savedata/<title_id>`.

**PS3 — aPS3e** (`aenu.aps3e`).
`aps3e/config/dev_hdd0/home/00000001/savedata/`. Directories begin with
`save_id` and carry a per-artifact suffix, so enumerate by prefix as
`folder-prefix` says. The user is hardcoded to `00000001`, unlike desktop
RPCS3 where several can exist.

**Xbox 360 — XenDroid** (`xendroid.compose`).
`compose/content/<XUID>/<save_id>/00000001/<package>/`. Content is keyed
by profile first and title second, so `save_id` is the *second* component
and the 16-hex XUID directories above it have to be enumerated.
`00000001` is the saved-game content type, against `00000002` for DLC and
`000B0000` for title updates. Non-profile content lives under the machine
XUID `0000000000000000`, so a tree containing only that XUID holds no
saves. The layout is Xenia's, from `ResolvePackageRoot()`.

**Xbox — X1 BOX** (`com.izzy2lost.x1box`). No host path exists. Saves are
written to `E:\UDATA\<save_id>\` inside a FATX filesystem within the
`.qcow2` or `.img` hard-disk image, and the user supplies that image
through the setup wizard rather than the app placing it anywhere fixed.
Reaching a save means reading FATX out of that image; X1 BOX's own FATX
code only imports a dashboard and exports nothing. Treat `save_id` on
this platform as an identifier for matching, not as a locator. Desktop
xemu has the same property for the same reason.

## Save units

A save unit is every file under an emulator's save root that belongs to
one game, named so a client can archive it and hash it the way the RomM
server will. `sigil_save_resolve()` takes a `sigil_save_request` and
returns a `sigil_save_unit`. Sigil never touches the filesystem: the
caller lists the root (plus the subfolders `sigil_save_layout_subdirs()`
names for the layout) and passes the relative paths in; hashing opens
members through the request's `open` callback and is skipped when it is
`NULL`.

### Request

| Field | Meaning |
|---|---|
| `layout` | libretro core id (`genesis_plus_gx`, `melonds`) or emulator id. An id with no row uses the libretro default row. |
| `platform` | platform slug, or `NULL`. Rows limited to one platform match through the aliases below. |
| `content_name` | the name the emulator loaded: rom, `.m3u`, `.cue`, `.chd`, or `archive.zip#member.ext` |
| `result` / `features` | the `sigil_result` for the content, or `NULL` with `features` carried from an earlier extraction |
| `options` | core option key/value pairs; only the keys the layout row names matter |
| `listing` | relative paths under the root, `/` separated |

### Unit

| Field | Meaning |
|---|---|
| `key` | the stem for file layouts, `save_id` for folder layouts |
| `shape` | `SINGLE` one member travels raw; `MULTI` two or more travel as a flat zip with every member at the root; `FOLDER` a zip of the `save_id` folder. `NONE` when nothing is present. |
| `members` | present members in archive order, each with its root-relative path, archive entry name and role |
| `expected` | absent members the layout says this game should have: every applicable primary, plus the rtc member when `features` has `SIGIL_FEATURE_RTC` |
| `unkeyed` | shared files seen in the root that belong to every game at once; reported, never bundled |
| `artifact` | the file name the unit travels under: the member's own name for `SINGLE`, the first member's name plus `.zip` for `MULTI`, `<key>.zip` for `FOLDER` |
| `content_hash` | RomM `content_hash` of the artifact |
| `identity_hash` | the same hash over the non-rtc members, so a clock tick alone does not read as a new save |

Roles: `PRIMARY` is the member that names the unit, `SIDECAR` is a
core-owned companion file, `RTC` is `RETRO_MEMORY_RTC`.

### Stem

`sigil_content_stem()` applies RetroArch's `runloop_path_set_basename`
rule: the loaded path's file name without its extension. For
`archive.zip#member.ext` the member name is the stem, so a rom loaded
from inside a zip saves under the entry name, not the archive name. An
`.m3u` saves under the playlist name.

### Hash

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

### Features

`sigil_result.features` (struct version 3) is a bitfield read from the
cart header.

`SIGIL_FEATURE_RTC` on `gb` / `gbc`: header byte `0x147` is `0x0F`
(MBC3+TIMER+BATTERY), `0x10` (MBC3+TIMER+RAM+BATTERY), `0xFD` (TAMA5) or
`0xFE` (HuC3). The header checksum over `0x134..0x14C` has to match byte
`0x14D` first; the boot ROM refuses a cart that fails it, so a mismatch
means the bytes are not a Game Boy header. gambatte
(`cartridge_libretro.cpp` `hasRtc`), mGBA (`GB_MBC3_RTC`) and VBA-M
(`gbRTCPresent`) key their `RETRO_MEMORY_RTC` region on the same values.

`SIGIL_FEATURE_RTC` on `snes`: the internal header sits at `0x7FC0`
(LoROM), `0xFFC0` (HiROM) or `0x40FFC0` (ExHiROM), plus a 512-byte skew
when the file size leaves that remainder (copier header). The header's
checksum and complement pair has to xor to `0xFFFF`; bases are tried
deepest first because an ExHiROM image also carries plausible bytes at
the HiROM base. `(ROMType << 8) | ROMSpeed` from bytes `0x16` and `0x15`
equal to `0x5535` (S-RTC) or `0xF93A` (SPC7110 with RTC) sets the flag,
which is what snes9x `memmap.cpp` `InitROM` enables its clock chips on.

RetroArch writes `RETRO_MEMORY_SAVE_RAM` as `<stem>.srm` and
`RETRO_MEMORY_RTC` as `<stem>.rtc` (`save.c`,
`path_init_savefile_rtc`). Clock sizes: mGBA GB 48 bytes, gambatte 8,
snes9x 20.

### Layout table

Each row names one core (`layout`), optionally one platform, its member
templates, the shared files it may write, and the subfolders it writes
into. Template variables: `{stem}`, `{romset}` (same as the stem),
`{title_id}`, `{save_id}`, `{cart_size}`, `{nvram_version}`,
`{left_index}`, `{right_index}`. A template ending in `/` names a folder
whose whole subtree is the member. A template with an option key applies
only while that core option holds the given value; the row's default
flag says whether an absent option counts as holding it, so a caller
sends only the options it has changed. Expansion fails, and the member
is skipped, when a variable has no value.

`{cart_size}` comes from `genesis_plus_gx_cart_size` in the core's own
spelling: `128k` to `128Kbit`, `256k` to `256Kbit`, `512k` to `512Kbit`,
`1meg` to `1Mbit`, `2meg` to `2Mbit`, `4meg` to `4Mbit`, absent to
`4Mbit`. `{nvram_version}` is `opera_nvram_version` (default `0`).
`{left_index}` and `{right_index}` are `beetle_psx_hw_memcard_left_index`
(default `0`) and `beetle_psx_hw_memcard_right_index` (default `1`).

Every row was read from the core's source or its libretro docs page; the
names are the core's literals.

| Layout | Platform | Members (role, option) | Shared | Source |
|---|---|---|---|---|
| default | any | `{stem}.srm` primary; `{stem}.rtc` rtc | | RetroArch `save.c` |
| `vba_next`, `gpsp` | any | `{stem}.srm` primary | | no RTC region (`libretro.c` memory maps) |
| `bsnes` | `snes` | `{stem}.srm` primary | | `program.cpp`: `.rtc` only for Game Boy carts; SNES clock chips persist nothing |
| `genesis_plus_gx` | `segacd` | `{stem}.srm` primary; `{stem}.brm` sidecar when `genesis_plus_gx_system_bram` = `per game`; `{stem}_{cart_size}_cart.brm` sidecar when `genesis_plus_gx_cart_bram` = `per game` | `scd_E.brm`, `scd_U.brm`, `scd_J.brm` when `system_bram` = `per bios` (default); `{cart_size}_cart.brm` when `cart_bram` = `per cart` (default) | `libretro/libretro.c` `check_variables`, `bram_save` |
| `mednafen_psx_hw` | any | `{stem}.srm` primary when `beetle_psx_hw_use_mednafen_memcard0_method` = `libretro` (default); `{stem}.{left_index}.mcr` primary when `mednafen`; `{stem}.{right_index}.mcr` sidecar when `beetle_psx_hw_enable_memcard1` = `enabled` | `mednafen_psx_libretro_shared.0.mcr`, `.1.mcr` when `beetle_psx_hw_shared_memory_cards` = `enabled` | commit `707d1be`; docs.libretro.com/library/beetle_psx_hw |
| `pcsx_rearmed` | any | `{stem}.srm` primary | `pcsx-card2.mcd` when `pcsx_rearmed_memcard2` = `shared` (default) | observed on device |
| `mednafen_saturn` | any | `{stem}.srm` primary when `beetle_saturn_save_method` = `libretro` (default); `{stem}.bkr` primary when `mednafen`; `{stem}.bcr` sidecar; `{stem}.smpc` sidecar | `mednafen_saturn_libretro_shared.bkr`, `.smpc` when `beetle_saturn_shared_int` = `enabled`; `.bcr` when `beetle_saturn_shared_ext` = `enabled` | `mednafen/ss/ss.c` |
| `mednafen_ngp` | any | `{stem}.flash` primary | | `mednafen/ngp/system.c` `system_io_flash_write` |
| `opera` | any | `opera/per_game/{stem}.{nvram_version}.srm` primary when `opera_nvram_storage` = `per game` (default) | `opera/shared/nvram.{nvram_version}.srm` when `shared` | `opera_lr_nvram.c`; subdirs `opera/per_game`, `opera/shared` |
| `pokemini` | any | `{stem}.eep` primary | | `libretro.c`, written at unload |
| `handy` | any | `{stem}.eeprom` primary | | `libretro.cpp`, written at `retro_deinit` |
| `melonds` | any | `{stem}.sav` primary | | legacy `libretro.cpp`, flushed on the core's own timer |
| `fbneo` | any | `fbneo/{romset}.fs` primary; `fbneo/{romset}.nv` sidecar; `fbneo/{romset}.memcard` sidecar when `fbneo-memcard-mode` = `per-game` | `fbneo/shared.memcard` when `shared` | `retro_common.cpp`, `eeprom.cpp`; subdir `fbneo`; option values are the English table of a localized option |
| `mame2003_plus` | any | `mame2003-plus/nvram/{romset}.nv` primary and `mame2003-plus/hi/{romset}.hi` sidecar when `mame2003-plus_core_save_subfolder` = `enabled` (default); `nvram/{romset}.nv` and `hi/{romset}.hi` when `disabled` | | `fileio.c`; subdirs `mame2003-plus/nvram`, `mame2003-plus/hi`, `nvram`, `hi` |
| `dosbox_pure` | any | `{stem}.pure.zip` primary | | `DBP_GetSaveFile`; one zip per content, travels as a file |
| `same_cdi` | any | `same_cdi/nvram/{stem}/` primary (folder) when `same_cdi_nvram_saves` = `enabled` (default) | | `retro_init.cpp`; subdir `same_cdi/nvram` |
| `nestopia` | `fds` | `{stem}.srm` primary; `{stem}.sav` sidecar when `nestopia_fds_savefile_format` = `sav_ups` (default); `{stem}.ups` when `ups`; `{stem}.ips` when `ips` | | `libretro.cpp` `SAVE_FDS` |

Row platforms use the slugs in the platform table plus `segacd` and
`fds`. Callers may pass their own forms: `scd`, `sega_cd`, `sega-cd`,
`mega_cd`, `mega-cd`, `megacd` resolve to `segacd`; `sfc`, `sfam` to
`snes`; `ps1`, `playstation` to `psx`; `famicom_disk_system` to `fds`.

Folder members are archived from the folder's parent, so the zip holds
`<folder>/<file>` the way a zipped save folder does.

## Quick example (C)

```c
#include <sigil.h>
#include <stdio.h>

int main(void) {
    sigil_result r;
    int rc = sigil_extract_from_path("/path/to/game.iso",
                                     SIGIL_PLATFORM_AUTO, NULL, &r);
    if (rc != SIGIL_OK) {
        fprintf(stderr, "sigil: %s\n", sigil_strerror(rc));
        return 1;
    }
    printf("platform=%s\n",   sigil_platform_to_slug(r.platform));
    printf("title_id=%s\n",   r.title_id);     /* game identity, persist this */
    printf("save_id=%s\n",    r.save_id);      /* on-disk save folder/file name */
    printf("raw_serial=%s\n", r.raw_serial);   /* as it appears in the binary */
    printf("source=%s\n",     r.source == SIGIL_SOURCE_BINARY ? "binary" : "filename");
    /* r.usage: SIGIL_USAGE_FOLDER_EXACT | _FOLDER_PREFIX | _FILE_EXACT | _FILE_PREFIX | _FOLDER_SPLIT */
    return 0;
}
```

## CLI

A reference `sigil(1)` is built alongside the library. Useful for
spot-checks during integration without writing any code:

```sh
$ sigil /path/to/game.xci --platform=switch --prod-keys=/path/to/prod.keys
platform=switch title_id=0100ABCD12345000 raw_serial=0100ABCD12345000 save_id=0100ABCD12345000 usage=folder-exact source=binary

$ sigil "/path/to/Ace Combat 04 (USA).chd" --platform=ps2
platform=ps2 title_id=SLUS-20152 raw_serial=SLUS_201.52 save_id=BASLUS-20152 usage=folder-prefix source=binary

# Read straight out of an archive, no extraction step
$ sigil "/path/to/Robotech - Invasion (USA).zip" --platform=xbox
platform=xbox title_id=TT-027 raw_serial=5454001B save_id=5454001B usage=folder-exact source=binary

# A Vita dump resolves by content, so it needs no hint
$ sigil "/path/to/Actual Sunlight [PCSE00695] [USA] [NoNpDrm].zip"
platform=psvita title_id=PCSE00695 raw_serial=PCSE00695 save_id=PCSE00695 usage=folder-exact source=binary
```

Pass `--platform=auto` (the default) to sniff from the file extension.
Extensions that name a container rather than a console (`.zip`, a bare
`.iso`) still need a hint unless the contents identify the platform on
their own, which is why the Vita example above does not take one. Check
`source` on the result: `binary` means the id came from file content,
`filename` means every binary path failed and a naming pattern was
scanned instead.

## Switch keys

Switch NCAs are encrypted; extracting the title ID from a retail XCI
or NSP requires the `header_key` from a `prod.keys` file. Pass it via
the support struct in any of three forms — sigil resolves them in
this priority order:

```c
sigil_support sup = {
    .struct_version = SIGIL_SUPPORT_V1,

    /* (1) Highest priority — raw 32-byte key, for callers that
     *     already loaded prod.keys themselves. */
    .switch_header_key = my_header_key_bytes,

    /* (2) In-memory text blob — for sandboxed environments like
     *     Android SAF where direct file I/O is mediated. */
    .switch_prod_keys_text = prod_keys_blob,
    .switch_prod_keys_text_len = prod_keys_blob_len,

    /* (3) Path on disk — sigil opens and parses the file. */
    .switch_prod_keys_path = "/path/to/prod.keys",
};
sigil_options opts = { .struct_version = SIGIL_OPTIONS_V1, .support = &sup };
sigil_extract_from_path("game.xci", SIGIL_PLATFORM_SWITCH, &opts, &r);
```

If no key is provided, sigil tries the unencrypted-NCA fallback path
(NSPs / XCIs whose NCA filenames are themselves the 16-hex title ID).
This works for decrypted dumps and homebrew but encrypted retail
content will fall through to the filename source — set
`SIGIL_FLAG_FILENAME_FALLBACK` in `opts.flags` to allow that, or
unset it to fail cleanly.

## Platform-specific notes

**PSP — folder prefix.** A single game produces multiple sibling
folders under `PSP/SAVEDATA/`, e.g. `ULUS10064DATA00`,
`ULUS10064SETTINGS`, `ULUS10064SAVE01`. Sigil emits the 9-char prefix
(`ULUS10064`); consumers MUST enumerate every folder under the parent
that starts with that prefix.

**GameCube — file prefix.** Saves are `.gci` files with the
convention `<makerCode>-<gameId>-<internalName>.gci`. Sigil emits the
hex-encoded ASCII gameId (`475A4C45` for `GZLE`); consumers match
files whose basename contains `-<gameId>-`. argosy's `GciSaveHandler`
is a reference implementation.

**PS2 — region prefix + folder-prefix enumeration.** `title_id` is the
ROM serial (`SLUS-20152`). `save_id` is the region-prefixed stem
(`BASLUS-20152`): `BA` for NTSC-U (`SLUS`), `BE` for PAL (`SLES`), `BI`
for `SLPS`/`SLPM`/`SLKA`, derived from the serial's region letter. The
folders the game creates on AetherSX2/NetherSX2/PCSX2 append a
per-artifact suffix (Ace Combat 04 = `BASLUS-20152AC04`; Champions of
Norrath splits into `BASLUS-20642SYS` + `BASLUS-20642RD0`), so `usage`
is `folder-prefix` and consumers enumerate every memory-card folder
whose name starts with `save_id`. The suffix is not derivable from the
disc, and it does not need to be: prefix matching captures it.

**Wii / GameCube — title ID is hex of ASCII.** The disc header
carries a 4-character ASCII gameId (`RZTE`, `GZLE`). The save form
is the hex encoding of those bytes (`52535445`, `475A4C45`) — that's
what Dolphin's NAND structure uses. `raw_serial` preserves the ASCII
form for human-readable logging; use `title_id` for actual save
lookup.

**Wii `.wbfs` — the disc header moves, it does not disappear.** A WBFS
file wraps a real disc header behind its own container header; the
wrapped header starts at the first hd sector, whose size the container
records as a shift at offset 8. Sigil reads the id there only when a
console magic backs it (Wii `5D1C9EA3` at +0x18, GameCube `C2339F3D`
at +0x1C). That check is not decoration: `WBFS` is four uppercase ASCII
bytes, so without it the container magic itself passes as a game id and
every wbfs dump collapses to the same bogus `57424653`.

**Wii U — last 8 of 16-hex.** WUA archives carry a top-level folder
named `00050000<8 hex>_v0`. The full 16 hex is the formal title ID;
the last 8 chars are what the save system keys on. Sigil emits the
last 8 as `title_id`, the full 16 as `raw_serial`.

**3DS — `0004` retail filter.** Program IDs not starting with `0004`
are filtered as non-retail (system titles, CIAs, etc.). Set
`SIGIL_FLAG_3DS_ALLOW_HOMEBREW` in `opts.flags` to disable the gate
for CIA/homebrew workflows.

**3DS — container shapes.** NCSD images (`.3ds`, `.cci`) hold the
program id inside partition 0's NCCH; NCCH images (`.cxi`, `.app`)
hold it at +0x118 of the file itself. The `z`-prefixed extensions are
an Azahar Z3DS wrapper — a 0x20-byte header, then metadata, then a
seekable-zstd payload — and the wrapper's `underlying_magic` names the
inner container, so a mislabelled extension still resolves. `.3dsx`,
`.z3dsx`, `.elf` and `.axf` are homebrew: they carry no title id and
sigil reports none rather than inventing one. `.elf` / `.axf` are too
generic to sniff, so they need an explicit `3ds` hint.

**PSP `.cso` / `.ciso` — experimental.** v1 CSO with raw-deflate
blocks is decompressed transparently and fed to the standard PSP
extractor. v2 (LZ4) is not supported. Flagged `experimental=1` on the
result until validated against a real CSO sample.

**PS3 — experimental, PARAM.SFO.** Reads the `TITLE_ID` string
(`BLUS31426`) from PARAM.SFO, which is reached three ways: the SFO file
directly, an extracted-game folder (sigil walks up to 4 levels looking
for it), or a disc image. Which shape it is gets decided from the `\0PSF`
magic rather than assumed, so a disc image is never parsed as though its
first sector were an SFO.

On a disc the file sits at `PS3_GAME/PARAM.SFO`, with a copy at the root
on some releases; sigil checks both, the same two locations aPS3e looks
in. PS3 discs carry a plain ISO9660 descriptor for their directory
structure, so no UDF reader is involved. `.iso` is ambiguous across half
a dozen platforms and needs an explicit `ps3` hint.

Saves land in `dev_hdd0/home/<user>/savedata` under directories that
start with the title id and carry a per-artifact suffix
(`BCUS99086GAMEDATA`), so `usage` is `folder-prefix` and consumers
enumerate by prefix. PKG and encrypted-EBOOT inputs are not supported.

**PS Vita — param.sfo, not the filename.** The identifier is `TITLE_ID`
in `sce_sys/param.sfo`, the same file Vita3K reads to identify installed
content. Dumps in circulation keep it at `app/<TITLEID>/sce_sys/param.sfo`
inside a zip, so sigil addresses that member by path suffix rather than
by the largest-member rule the generic archive branch uses.

Finding that member is also what identifies the dump as Vita at all. A
`.zip` names no platform, and the bracketed-serial convention in these
filenames is shared with PSP, so a name-based guess resolves the wrong
platform. Detection is by content and needs no hint.

Two ordering details matter. A dump can carry a second `param.sfo` under
`savedata/`, which describes a save rather than the title and has no
`TITLE_ID`; sigil prefers the shallowest match, because a title's own
metadata always sits above anything subordinate to it. And the filename
scanner still runs, but only when no `param.sfo` can be reached, so it is
a fallback rather than the primary path — `source` tells you which one
answered.

Saves are one exact directory per title at
`ux0:user/00/savedata/<TITLEID>`, hence `folder-exact`.

**Dreamcast — IP.BIN product number, found by scanning.** The boot
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

**Xbox — experimental, XBE certificate.** The certificate in the XBE
holds a 32-bit title id, and two forms of it matter. The console names
its save directory after the raw hex (`E:\UDATA\4D530064`), while the
serial every tool prints is two publisher letters, a hyphen, and the low
half in decimal (`MS-100`). So `title_id` is the formatted serial and
`save_id` is the hex, diverging the way PS2 and 3DS do. Ids whose prefix
bytes are not `A-Z` — the dashboard, XDK samples — fall back to plain
8-digit hex in both fields, which is what Cxbx-Reloaded's
`FormatTitleId()` does. That directory lives inside a FATX disk image
rather than on the host filesystem, so `save_id` identifies a save here
without locating one; see
[Where saves land on Android](#where-saves-land-on-android).

The certificate is addressed by the virtual address the image loads at,
so its file offset is that address minus the image base. Pass a bare
`default.xbe`, an extracted game folder, or a disc image.

**Xbox / Xbox 360 — one filesystem, XDVDFS.** Both consoles use the same
filesystem, so one walker serves `default.xbe` and `default.xex` alike.
What differs is where the game partition starts, and that is probed
rather than assumed: 0 for a trimmed image, then the XGD3, XGD2 and XGD1
bases in ascending order. Ascending matters because an archive-backed IO
seeks forward cheaply and rewinds expensively. Both copies of the
`MICROSOFT*XBOX*MEDIA` magic are checked, the one at the start of the
volume descriptor and the one at `+0x7EC`, so a stray copy of that string
inside game data cannot pass as a partition header.

Directory entries form a binary search tree rather than a flat list, so
the ISO9660 helpers do not carry over. Three details bite: subtree
offsets are in 4-byte units, the absent-child sentinel is `0` or `0xff`
despite the field being 16 bits wide, and names are WINDOWS-1252 compared
case-insensitively. `.xiso` and the compound `.xiso.iso` resolve without
a hint; a bare `.iso` is ambiguous and needs one.

**Xbox 360 — experimental, XEX title id.** The 4-byte execution-info
title ID is returned as 8-char uppercase hex (`4D5307DC`), which is what
the console, Xenia and XenDroid all use, so `title_id` and `save_id` are
the same string. Optional header values are offsets from the XEX start
rather than from the file, so a XEX embedded in a disc image has its base
added back to each one.

Reachable four ways: a bare `default.xex`, an extracted game folder, a
disc image through the XDVDFS walker, or a `.zar`. GoD containers and
STFS packages are not implemented.

## Container notes

**ZArchive (`.wua`, `.zar`) — cheap random access.** One container under
two names: Cemu writes it for Wii U, Xenia writes it for Xbox 360, and
both vendor the same library, so the metadata parsing is shared. Contents
sit in fixed 64 KiB blocks with an offset record for every sixteen, where
each record holds a full 64-bit base offset and the compressed size of
each block minus one. Reaching an arbitrary byte therefore costs one
record read, one block read and one zstd call regardless of how deep it
sits. Identifying a title out of a multi-gigabyte archive takes a couple
of blocks, not a decompression pass.

**Zip — streaming, with an emulated seek.** `sigil_io` is a random-access
contract and deflate is a forward-only stream, so the shim inflates and
discards to reach a forward offset and restarts the decoder to reach a
backward one. The disc walkers touch a handful of ascending offsets, so a
restart is rare and never more than one per extraction. Cost scales with
how far into the member the identifier sits rather than with the member's
size, which is why a trimmed image in a zip is far cheaper than a full
redump in one. Stored (uncompressed) members skip all of it and read
through directly.

ZIP64 is handled, and required rather than optional: a redump exceeds
every 32-bit field in the classic records, so the real sizes and offsets
live only in the ZIP64 extra field.

Members are chosen one of two ways. By default the largest non-directory
member wins, which picks the disc image out of an archive that also holds
a readme. A caller can instead ask for a member by path suffix, which is
how a metadata file under a directory named for the title is reached; the
shallowest match wins there, so a nested copy of the same filename cannot
shadow the real one.

`.7z` is recognised as an archive but has no reader. Adding one means
vendoring the LZMA SDK's container sources (`7zArcIn.c`, `7zDec.c`,
`Lzma2Dec.c` and friends); `third_party` currently carries only the
`LzmaDec.c` libchdr needs. Until then a `.7z` falls through to the
filename scanner. Note that LZMA offers no offset table and 7z is solid
by default, so it could never be as cheap to seek into as ZArchive is.

## Building

```sh
cmake -B build -S .
cmake --build build
```

Every platform and container reader is always compiled. There is no
build-time toggle for what sigil can read, deliberately: sigil is
embedded by larger applications, and a consumer expects a format to work
rather than to find out at integration time that a flag dropped it.

The options that remain choose what gets produced, not what sigil
understands:

| Option | Effect |
|---|---|
| `-DSIGIL_BUILD_SHARED=ON` | Build `libsigil.so` instead of `.a` |
| `-DSIGIL_BUILD_CLI=OFF` | Skip the `sigil(1)` reference CLI |
| `-DSIGIL_BUILD_TESTS=OFF` | Skip tests |

## Bindings

- [`bindings/android/`](bindings/android/) — Gradle library module
  wrapping the C ABI for Kotlin/Java consumers via JNI. Used by
  argosy-launcher. Guide: [docs/kotlin.md](docs/kotlin.md).
- [`bindings/go/`](bindings/go/) — cgo wrapper for Go consumers (Grout).
  Guide: [docs/go.md](docs/go.md).
- [`bindings/python/`](bindings/python/) — cffi wrapper. Guide:
  [docs/python.md](docs/python.md).

All three expose the same operations, options and fields.
`bindings/python/test_contract.py` (`make contract`, no toolchain
needed) holds the table of names and fails when a binding drops one, and
keeps the C enums, the JNI descriptors and the Kotlin constructors in
step. A new binding under `bindings/<lang>/` joins that table and gets
its own page under `docs/`.

## Testing

```sh
# Synthetic unit tests (fast, no ROMs needed)
cmake --build build && ctest --test-dir build

# Real-ROM integration tests — point at a directory with platform
# subdirs (psp/, psx/, ps2/, ps3/, switch/, 3ds/, wii/, wiiu/, ngc/,
# psvita/, dc/, xbox/, xbox360/).
SIGIL_ROM_DIR=/path/to/roms ctest --test-dir build -R integration

# Switch tests additionally need a prod.keys file
SIGIL_ROM_DIR=/path/to/roms \
SIGIL_PROD_KEYS=/path/to/prod.keys \
ctest --test-dir build -R integration_switch

# Cap samples per platform (default 25). Useful when iterating on a
# library with hundreds of CHDs per platform.
SIGIL_SAMPLE_LIMIT=10 SIGIL_ROM_DIR=/path/to/roms \
ctest --test-dir build -R integration
```

Integration tests skip cleanly with exit code 77 when env vars are
unset, so the public CI without ROMs can still run unit tests.

## Contributing

PRs welcome. See [TRADEMARKS.md](TRADEMARKS.md) for the naming
policy: forks-for-contribution (standard fork → PR-upstream flow)
are encouraged; forks-and-republish-as-a-separate-project under the
`argosy-sigil` name are not.
