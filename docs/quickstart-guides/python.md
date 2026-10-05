# Python

`import sigil` from `bindings/python` (cffi; `make build` compiles the
extension). Failures raise a `SigilError` subclass per C code
(`SigilNotFoundError`, `SigilNeedsKeyError`, `SigilIOError`, ...) with
the code on `.code`.

## 1. Identify the game

Once, at import. Skip when the result is already stored.

```python
sigil.extract(
    path: str | PathLike,                   # required. Rom file.
    platform: str = "auto",                 # optional. Slug from identification.md. "auto" sniffs
                                            #   the extension; a bare .iso or .zip needs it.
    *,
    prod_keys_path: str | PathLike = None,  # optional. Switch prod.keys file.
    prod_keys_text: str | bytes = None,     # optional. Switch prod.keys contents. Wins over prod_keys_path.
    header_key: bytes = None,               # optional. Switch header key, 32 bytes. Wins over both.
    filename_fallback: bool = True,         # optional. Scan the file name when the binary gives nothing;
                                            #   source reports which happened.
    allow_3ds_homebrew: bool = False,       # optional.
) -> SigilResult                            # Raises when nothing identified the file.
```

```python
SigilResult(
    title_id: str,              # "" on gb, gbc, snes. A Switch update or DLC gives its game's id.
    raw_serial: str,            # As found in the binary; a Switch update's or DLC's own id.
    save_id: str,               # On-disk name the emulator keys the save by. "" on gb, gbc, snes.
    platform: str,
    source: str,                # "binary", "filename".
    usage: str,                 # "folder-exact", "folder-prefix", "file-exact", "file-prefix",
                                #   "folder-split". identification.md, "usage", says how to apply save_id for each.
    experimental: bool,
    switch_content_type: str,   # "unknown", "application", "patch", "addon".
    title_version: int,         # Switch only.
    features: int,              # Bit set. FEATURE_RTC: cart has a clock. has_rtc reads it.
    n64_header: str,            # N64 only. The cart's name; "" when not plain ASCII.
    n64_md5: str,               # N64 only. The ROM's MD5 in .z64 byte order, uppercase.
    n64_md5_n64: str,           # N64 only. The same in .n64 byte order, as Project64 hashes it.
)
```

For an N64 ROM, `extract` reads the whole file to fill the two MD5s; the
standalone N64 emulators name saves from them.

A Switch XCI or NSP needs keys: without them `extract` raises
`SigilNeedsKeyError`, and with keys that don't open the content (a key
file older than the dump, or a wrong header key)
`SigilKeysIncompatibleError`.

Store `title_id`, `save_id`, `raw_serial`, `platform`, `features` and,
for N64, the three `n64_*` fields. Rebuild the result from them later, or
build one for a platform sigil cannot extract (Sega CD raises):

```python
sigil.SigilResult.persisted(
    platform: str,      # required. Slug from identification.md, or segacd, fds.
    title_id: str,      # required. Stored title_id, or "" when the platform has none.
    save_id: str,       # required. Stored save_id, or "" when the platform has none.
    features: int,      # required. Stored features, or 0.
    raw_serial: str = "",  # optional. Stored raw_serial; pcsx_rearmed's serial cards are named from it.
    n64_header: str = "",  # optional. The stored N64 fields; the standalone N64 emulators'
    n64_md5: str = "",     #   saves are named from them.
    n64_md5_n64: str = "",
) -> SigilResult
```

## 2. Locate the saves

At sync time. No save is read.

```python
sigil.locate_saves(
    game: SigilResult,                          # required. Step 1.
    core: str,                                  # required. Layout id of the emulator running the game;
                                                #   see below.
    content_path: str,                          # required. The path you handed the emulator, verbatim:
                                                #   rom, .m3u, .cue, .chd, or archive.zip#member.ext when a
                                                #   member was loaded. Only the file name part is used.
    *,
    save_root: str | PathLike = None,           # optional. Directory the emulator writes this game's save
                                                #   into. RetroArch: savefile_directory, plus the core-named
                                                #   subfolder when sort_savefiles_enable is on. Sigil lists
                                                #   it and the subfolders the core writes into.
    listing: Iterable[str] = None,              # optional. Instead of save_root: every file directly in the
                                                #   root plus every file under layout_subdirs(core), twelve
                                                #   levels deep, as root-relative / paths. list_save_root
                                                #   builds this.
    options: Mapping[str, str] = None,          # optional. Core option key to value, the strings the core
                                                #   defines and RetroArch writes to <core>.opt, never display
                                                #   labels: {"genesis_plus_gx_system_bram": "per game"}.
                                                #   Pass all of them; only the keys the row names are read.
    profile: str = None,                        # optional. Layouts with profiles: the profile whose saves
                                                #   to take, by SigilProfile.id.
) -> SigilSaveUnit                              # Hashes empty, except on a layout with profiles given a
                                                #   save_root, where sigil reads the profile list and fills
                                                #   them.
```

`core` names the emulator, because each keeps its saves differently. For
a libretro core, pass the core's name without `_libretro`
(`genesis_plus_gx`, `mednafen_psx_hw`). For a standalone emulator, pass
its layout id (`dolphin_standalone`, `pcsx2_standalone`, `eden`).
[platforms/](../platforms/README.md#layouts) lists every id with its
emulator. An id with no row gets the libretro default (`<stem>.srm`, plus
`<stem>.rtc` when the cart has a clock), which fits an unlisted libretro
core but names nothing an unlisted standalone emulator writes.

If `alternates` is not empty, the root holds this game's saves under
other option values, such as Beetle Saturn's `.bkr` from a build older
than its save-method option. Ask the user, or call again with each
alternate's `options`; sigil never picks one itself.

On a layout with profiles (`eden`, `citron`, `sudachi`, `yuzu`, `cemu`,
`vita3k`, `rpcs3`) and on the PSP layouts (`ppsspp`, `ppsspp_standalone`,
`psp_console`, which keep no profiles), `save_root` may be any folder
around the emulator's own: its base (the folder holding `nand/`, `mlc01/`,
`ux0/`, `dev_hdd0/` or `PSP/`), a folder above it, or one inside it such
as a profile's save folder. Sigil re-roots at the base and takes the profile the root lies in.
Member paths are then relative to the base, which `save_base` returns.
[save-units.md](../save-units.md#profiles) has the folders and the profile rules.

With two or more profiles and none picked, collect and restore raise
`SigilAmbiguousError`. If you don't know which profile the user plays as,
ask them: `list_profiles(core, save_root)` lists the emulator's profiles
without a game, and the error's `profiles` holds the same list. Pass the
answer as `profile` and keep it per user.

```python
SigilSaveUnit(
    key: str,                               # Stem, or save_id for folder layouts.
    shape: str,                             # "single", "multi", "folder". "none" when nothing is there.
    members: tuple[SigilSaveMember, ...],   # Files present.
    expected: tuple[SigilSaveMember, ...],  # Files the core should have written but has not: every
                                            #   applicable primary, plus the rtc file when the cart has a clock.
    unkeyed: tuple[str, ...],               # Root files shared by every game. Never bundle them.
    artifact: str,                          # File name the upload travels under.
    content_hash: str,                      # "" until step 3.
    identity_hash: str,                     # "" until step 3.
    alternates: tuple[SigilSaveAlternate, ...],  # Listed files other option values would take.
)

SigilSaveAlternate(
    path: str,                  # Root-relative.
    shared: bool,               # A file every game shares.
    options: Mapping[str, str], # The values that take it; pass them as options to read it.
)

SigilSaveMember(
    path: str,      # Root-relative.
    entry: str,     # Archive entry name.
    role: str,      # "primary", "sidecar", "rtc".
    present: bool,
    area: str,      # "account" or "device" on a layout with profiles, "none" elsewhere.
)
```

## 3. Hash the saves

When you need to compare with the server.

```python
sigil.hash_saves(
    saves: SigilSaveUnit,           # required. Step 2.
    save_root: str | PathLike,      # required. Directory its paths are relative to.
) -> SigilSaveUnit                  # Same unit with content_hash and identity_hash filled.
                                    #   Raises SigilIOError when a member cannot be opened.
```

```python
content_hash: str     # What RomM stores for the artifact and compares against.
identity_hash: str    # The same over the saves alone, leaving out the clock file.
```

Compare `content_hash` with RomM's. `identity_hash` is sigil's own: RomM
never sees it, and sync's `changed` is already built on it, so a clock
that ticked doesn't read as a new save. Most clients never read it.

## Upload and restore

`collect` and `restore` (see [Sync](#sync)) build and unpack the
artifact for you. One round trip, with `romm` and `store` standing for
your own server client and storage:

```python
import sigil

CORE, CONTENT, ROOT = "pcsx_rearmed", "Chrono Cross (USA).cue", "/saves/psx"

# After the game closes: collect, upload what changed, then keep the state.
result = sigil.collect(game, CORE, CONTENT, ROOT, state=store.state(game))
if result.changed and result.data is not None:
    romm.upload_save(game, result.artifact, result.data, result.content_hash)
    if result.holding is not None:
        romm.upload_save(game, "holding.zip", result.holding)
    store.set_state(game, result.state)      # only once every upload succeeded

# Before the next launch: put the server's save back.
unit = romm.download_save(game)
try:
    result = sigil.restore(unit, game, CORE, CONTENT, ROOT, state=store.state(game))
except sigil.SigilConflictError:
    # The saves on disk changed since the last sync. Ask the user, then:
    result = sigil.restore(unit, game, CORE, CONTENT, ROOT, state=store.state(game),
                           overwrite_local=True)
except sigil.SigilAmbiguousError as e:
    # More than one profile could take the saves. Ask which is theirs:
    choice = ask_user(e.profiles)
    result = sigil.restore(unit, game, CORE, CONTENT, ROOT, state=store.state(game),
                           profile=choice.id)
store.set_state(game, result.state)
```

Upload `data` under the name `artifact`; RomM computes the same
`content_hash`. Pass back the `state` the last call returned every time,
so sigil can tell a local change from its own last restore.

### Without collect and restore

`locate_saves` doesn't build an upload. It gives you `members`, the
files that make up the game's save, and you package them yourself:

1. Upload by `shape`. `"single"`: send the one member's file as it is.
   `"multi"`: zip the members yourself, each stored at the zip's root
   under its `entry`. `"folder"`: zip the `key` folder so entries read
   `<key>/<file>`. Name the upload `artifact`.
2. Compare with RomM by the `content_hash` from step 3; it matches what
   RomM computes for that upload.
3. To restore, unpack the artifact yourself. `"single"`: write it to
   the member's `path`. `"multi"`: write each zip entry to the `path` of
   the member with that `entry`. `"folder"`: unzip into the key folder's
   parent. When the emulator hasn't created a primary yet, `expected`
   gives its `path`.

This path writes whole files, so it can't merge a game's saves into a
shared memory card or a profile folder the way `restore` does. Use
`collect` and `restore` wherever they cover the system. Hash rules:
[save-units.md](../save-units.md#hash).

## Sync

`collect` gathers one game's saves into the unit that travels to RomM;
`restore` puts a unit back and reads it back, removing files where a save
folder holds a save the unit lacks. PS1 and PS2 memory cards, PCSX2 folder
cards, GameCube cards and Dolphin's GCI folder, Saturn and Sega CD backup RAM,
Dreamcast VMUs, the save folders the yuzu forks, Cemu, Vita3K and RPCS3
keep per user profile, and PSP save folders work today. `restore` raises
`SigilNotFoundError` for a unit holding none of the game's saves, and
ignores other games' saves inside a unit. The rules every system shares are
in [sync.md](../sync.md); what a unit holds, how Saturn and Sega CD saves find
their owner, and how genesis_plus_gx's region file is picked are on each
system's page under [platforms/](../platforms/README.md). For Saturn and Sega CD, build the game with
`SigilResult.persisted("saturn", "", "", 0)` or `("segacd", ...)`.

```python
sigil.collect(
    game: SigilResult, core: str, content_path: str, save_root: str | PathLike,
    *,
    listing: Iterable[str] | None = None,   # Root-relative paths; None lists save_root.
    options: Mapping[str, str] | None = None,
    game_ids: Iterable[str] = (),           # Every id the game's saves may carry: all discs of a set.
    state: bytes | None = None,             # What the last call returned for this platform and emulator.
    mode: "managed" | "unmanaged" = "managed",
    claimed: Iterable[str] = (),            # Saturn, Sega CD, Dreamcast: names from `unowned` the user gave this game.
    companions: Iterable[SigilCompanion] = (),   # Games whose saves this game reads, in the order they go on.
    repair: bool = False,                   # Rebuild what SigilDamagedError named, where sigil can.
    profile: str | None = None,             # Layouts with profiles: the profile whose saves to take.
) -> SigilSyncResult

sigil.restore(unit: bytes, ..., overwrite_local: bool = False) -> SigilSyncResult

SigilCompanion(
    game_ids: tuple[str, ...],  # The companion's ids, as for game_ids.
    unit: bytes | None = None,  # restore: its unit from RomM, or None to leave its saves as they are.
)

SigilCompanionResult(           # collect: one per companion, in request order.
    data: bytes | None,         # The companion's unit. None when none of its saves are there.
    content_hash: str,
    identity_hash: str,
    changed: bool,              # identity_hash differs from the companion's last sync.
)

SigilSyncResult(
    artifact: str,              # File name the unit travels under.
    shape: str,
    data: bytes | None,         # collect: the unit. None when the game has no saves.
    content_hash: str,          # RomM content_hash of the unit.
    identity_hash: str,         # Over the saves themselves; placement and timestamps don't move it.
    changed: bool,              # identity_hash differs from the last sync.
    state: bytes,               # Store it once every upload succeeded; pass it back next time.
    holding: bytes | None,      # Saturn, Sega CD, Dreamcast: zip of the saves on a shared volume with no known
                                #   owner. Upload it with the unit.
    unowned: tuple[str, ...],   # The names of the saves in holding, decoded as SigilCardEntry.name
                                #   is. Pass them to `claimed` as they are.
    restore_again: bool,        # Unmanaged: the saves the last restore wrote were overwritten.
                                #   Restore again instead of uploading.
    companions: tuple[SigilCompanionResult, ...],
    profiles: tuple[SigilProfile, ...],   # Layouts with profiles: every profile the emulator lists.
    profile: str,               # The profile whose saves were taken or written; "" for none.
    alternates: tuple[SigilSaveAlternate, ...],  # Listed files other option values would take, as on SigilSaveUnit.
)

SigilProfile(
    id: str,                    # As its save folder is named.
    name: str,                  # The nickname; "" when the emulator keeps none.
)
```

A companion's saves go on the game's card beside the game's own and stay
out of the game's unit; [sync.md](../sync.md#companions) has the rules.

### Refusals

`restore` writes nothing when it raises one of these. `collect` raises
`SigilDamagedError`, `SigilAmbiguousError` and `SigilIOError` the same
way. Every error carries `problem`, naming the save, member or files at
fault when there is one. [sync.md](../sync.md#refusals) has when each one
happens.

| Error | Meaning |
|---|---|
| `SigilConflictError` | the saves under `save_root` changed since the last sync; pass `overwrite_local=True` once the user agrees |
| `SigilUncollectedError` | a shared volume holds saves no collect has passed on yet; collect for the game that ran last first |
| `SigilNoSpaceError` | the saves don't fit; `blocks_short` says by how much |
| `SigilRegionError` | a companion's save is from another region |
| `SigilNoTargetError` | the unit holds a volume or member with no file to go in |
| `SigilAmbiguousError` | more than one card file or profile could take the saves; `profiles` lists the profiles, so ask the user and pass the choice |
| `SigilDamagedError` | a file the saves are in is damaged; pass `repair=True` once the user agrees |
| `SigilExistsError` | Dolphin's GCI folder has no free name for a new save |
| `SigilIOError` | a file the listing holds won't open, or a member's path would leave the root |

## Memory cards

List the saves on a memory card or backup RAM volume: PS1 cards (the raw
card as `.mcr`, `.mcd` or `.srm`, DexDrive `.gme`, PSP or Vita `.vmp`),
PS2 `.ps2` file cards, GameCube raw cards, Dreamcast VMUs, and Saturn and
Sega CD backup RAM. Sync doesn't need it; it's for showing the user
what a card holds.

```python
sigil.list_card(
    path: str | PathLike,           # required. The card file. Its format is detected from the content.
) -> SigilCardListing               # Raises SigilUnsupportedFormatError when the file is not a card
                                    #   sigil reads.

SigilCardListing(
    format: str,                    # "ps1-raw", "ps1-gme", "ps1-vmp", "ps2", "gamecube-raw",
                                    #   "dreamcast-vmu", "saturn-backup", "segacd-bram".
    total_blocks: int,
    free_blocks: int,               # Blocks a new save can use.
    free_slots: int,                # Directory slots a new save can use.
    corrupt_count: int,             # Saves left out because their block chain is broken.
    entries: tuple[SigilCardEntry, ...],          # Live saves, in directory order.
    corrupt_entries: tuple[SigilCardEntry, ...],  # The left-out saves the card still names; blocks is 0.
)

SigilCardEntry(
    name: str,                      # As stored on the card, e.g. "BASLUSP01041USCHRO00". Bytes that
                                    #   aren't UTF-8 decode with surrogateescape.
    owner_id: str,                  # The game id the save carries, as extract reports it: PS1 and PS2
                                    #   "SLUS-01041", GameCube "47465A45". "" when the format has none.
    blocks: int,                    # In the card's own block size.
    first_block: int,
)
```

## Helpers

```python
sigil.content_stem(content_path: str) -> str                  # Stem the save is named after.
sigil.layout_subdirs(core: str) -> list[str]                   # Subfolders the core writes into.
sigil.list_save_root(root: str | PathLike, core: str) -> list[str]   # Below root too, where a
                                                                     #   layout with profiles has its base.
sigil.save_base(core: str, path: str | PathLike) -> tuple[str, str]  # (base, profile) for a path.
sigil.list_profiles(core: str, save_root: str | PathLike) -> tuple[SigilProfile, ...]
                                                     # The emulator's profiles; SigilUnsupportedFormatError
                                                     #   for a core without profiles.
sigil.platform_from_slug(slug: str) -> int                     # PLATFORM_AUTO when unknown.
sigil.platform_to_slug(platform: int) -> str
sigil.load_header_key_from_prod_keys(path: str | PathLike) -> bytes   # 32 bytes.
sigil.version() -> str
```
