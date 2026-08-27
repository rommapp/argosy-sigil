# argosy-sigil

A native C helper library that derives the game-native serial / title ID
from a ROM file. Designed for applications that want per-game save and
state files instead of per-platform — pair a local save to the right
upstream game record by reading the platform's own ID out of the ROM,
not by guessing from filenames.

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

The slugs are stable. Argosy's shorter internal identifiers (`dc`,
`ngc`, `gc`, `vita`, `n3ds`, `nsw`, `x360`, `xbx`) resolve as aliases of
the canonical slugs above.

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
`FormatTitleId()` does.

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
  argosy-launcher.
- [`bindings/go/`](bindings/go/) — cgo wrapper for Go consumers
  (Grout). `go test` against `/tmp/roms/roms/` passes for all 9
  platforms including encrypted Switch XCIs.

Additional bindings (Rust via bindgen, Python via cffi, etc.) can be
added under `bindings/<lang>/` — sigil's small public API and stable
enum numbering keep these straightforward.

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
