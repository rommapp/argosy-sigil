# Identifying a game

You hand sigil a path to a ROM. Sigil reads the platform-native
identifier directly from the disc or cart binary and hands back a
`sigil_result`. Each platform's page under [platforms/](platforms/README.md)
says how it finds the id and where its `save_id` diverges.

## Fields

- `title_id` is the canonical identity of the game (`ULUS10064`,
  `SLUS-12345`, `0100ABCD12345000`). Use this for matching game
  records, deduplication, RomM/IGDB lookups, UI display. A Switch update
  or DLC gives its game's id.
- `save_id` is **the literal on-disk save folder/file name the emulator
  will create.** Use this for any filesystem operation against the
  save directory. For most platforms it equals `title_id`. Where it
  diverges, the platform's page says why: PS2 adds a region prefix
  ([ps2](platforms/ps2.md)), 3DS splits the id into a nested path
  ([3ds](platforms/3ds.md)), Wii and Wii U lowercase it
  ([wii](platforms/wii.md), [wiiu](platforms/wiiu.md)), GameCube uses the
  ASCII game id ([gamecube](platforms/gamecube.md)), a Vita title that
  shares another title's saves names that title
  ([psvita](platforms/psvita.md)), and the original Xbox uses the raw hex
  of a number `title_id` prints in decimal ([xbox](platforms/xbox.md)).
  Everything above it (a user directory, the emulator's save root,
  per-install id folders) is the emulator's prefix and the consumer's to
  supply, because the same title differs per emulator.
- `raw_serial` is the ID exactly as it appears in the binary, before
  any normalization (`ULUS-10064`, `SLUS_123.45`, `RZTE`). For a Switch
  update or DLC it is the content's own id. pcsx_rearmed names its
  per-disc cards from it, so store it with `title_id`.
- `usage` is how the platform uses `save_id` to lay out save artifacts
  on disk (one folder per game, multiple folders sharing a prefix,
  one file, multiple files sharing a prefix). See below.
- `source` is `binary` if the ID came from the file content
  (high-confidence, lockable) or `filename` if it had to fall back to
  scanning the filename for a community-naming bracket pattern.
- `experimental` is `1` for extractors that haven't been validated
  against real-world samples (PS3, Xbox, Xbox 360, Dreamcast, PSP-via-CSO).
  Consumers should surface this to users so a low-confidence
  result can be flagged in UI / logs.
- `features` holds `SIGIL_FEATURE_*` facts read from a cart header, such as a
  real-time clock on Game Boy and SNES carts; see
  [save-units.md](save-units.md#features).

Persist `save_id`, `raw_serial` and `usage` alongside `title_id` on your
game record once you extract them. `title_id` doesn't change, and
re-reading the disc to recompute `save_id` on every save sync is wasteful.

## What `usage` means for `save_id`

| Value | Meaning | Example |
|---|---|---|
| `folder-exact` | One folder per game named exactly `save_id` | `Switch/saves/0100ABCD12345000/` |
| `folder-prefix` | Multiple folders per game, all starting with `save_id` and a profile/slot suffix. Consumers MUST enumerate and bundle all matches. | PSP: `ULUS10064DATA00`, `ULUS10064SETTINGS`; PS2: `BASLUS-20642SYS`, `BASLUS-20642RD0` |
| `file-exact` | One file per game named with `save_id` | rare; emulator-specific |
| `file-prefix` | Multiple files per game, all containing `save_id` in the basename | GameCube GCI: `<maker>-<gameId>-<name>.gci` (e.g. `01-GZLE-Animal Crossing.gci`) |
| `folder-split` | `save_id` is a `/`-separated nested path, not a flat name; consumer creates the intermediate folders | 3DS: `save_id` = `00040000/00033500` |

Treating a `prefix` platform as `exact` silently misses every save for
that platform. Sigil emits this classification so dispatch is correct
without you re-deriving it.

## Platforms and slugs

| Slug | Platform | Inputs | `title_id` example | `usage` | Status |
|---|---|---|---|---|---|
| `psp` | PSP | `.iso`, `.chd`, `.cso` / `.ciso` | `ULUS10064` | folder-prefix | `.cso` experimental |
| `psx` | PlayStation | `.iso`, `.bin`, `.chd` | `SLUS-12345` | file-prefix | |
| `ps2` | PlayStation 2 | `.iso`, `.chd` | `SLUS-20675` | folder-prefix | |
| `ps3` | PlayStation 3 | `.iso` (needs hint), game folder (recursive) or `.sfo` | `BLUS31426` (TITLE_ID from PARAM.SFO) | folder-prefix | experimental |
| `psvita` | PS Vita | `.zip` dump, extracted folder, or `param.sfo` | `PCSE12345` (TITLE_ID from `sce_sys/param.sfo`) | folder-exact | filename fallback when no `param.sfo` is reachable |
| `switch` | Nintendo Switch | `.nsp`, `.xci` | `0100ABCD12345000` | folder-exact | |
| `3ds` | Nintendo 3DS | `.3ds`, `.cci`, `.cxi`, `.app`, `.z3ds`, `.zcci`, `.zcxi` | `0004000000123456` | folder-split | `.3dsx` / `.z3dsx` / `.elf` / `.axf` are homebrew and carry no title id |
| `wii` | Wii | `.iso`, `.rvz`, `.wbfs`, `.wad` | `525A5445` (hex of ASCII gameId); `.wad`: `00010001574B5445` (full 16-hex title id) | folder-exact; `.wad`: folder-split | |
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
[save-units.md](save-units.md#features)); `title_id` and `save_id` stay empty and
the emulator names the save after the content file.

The slugs are stable. Argosy's shorter internal identifiers (`dc`,
`ngc`, `gc`, `vita`, `n3ds`, `nsw`, `x360`, `xbx`, `sfc`, `sfam`) and
`ps1`, `playstation` resolve as aliases of the canonical slugs above.

A `.zip` holding any of the formats above is read in place, with no
extraction step: sigil opens the archive's member, resolves the platform
from it, and runs the normal extractor against it. Pass a platform hint
when the inner name is itself ambiguous (a bare `.iso`), exactly as you
would for a loose file. `.wua` and `.zar` are both ZArchive and are read
the same way. See [containers.md](containers.md) for what each costs
and how the member is chosen.

The C API uses the `sigil_platform` enum; `sigil_platform_from_slug()`
converts strings if your binding accepts user input.

## Hints and fallbacks

Pass `SIGIL_PLATFORM_AUTO` to sniff from the file extension. Extensions
that name a container rather than a console (`.zip`, a bare `.iso`) still
need a hint unless the contents identify the platform on their own, as a
Vita dump's `param.sfo` does. An extension two consoles share is settled
the same way. `.rvz` and `.wbfs` hold either a Wii or a GameCube disc, and
the header magic decides which. A platform you pass is never
second-guessed: it names the console outright, and the magic is consulted
only when you name nothing. Check `source` on the result: `binary` means
the id came from file content, `filename` means every binary path failed
and a naming pattern was scanned instead.

## What sigil isn't

- Not a hash-based game identifier. CRC32/MD5/SHA-1 matching against
  No-Intro / Redump / RetroAchievements is a different problem;
  hashing tells you "which dump is this," sigil tells you "what does
  the platform call this game."
- Not a generic ROM info library. Sigil extracts the identifier the
  platform's own save subsystem keys on, plus the few header facts save
  handling needs (a cart's clock, a Switch content's type and version).
  It does not give you region, language, a hash or a header dump.
