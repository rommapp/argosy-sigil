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
    platform: str = "auto",                 # optional. Slug from the README platform table. "auto" sniffs
                                            #   the extension; a bare .iso or .zip needs it.
    *,
    prod_keys_path: str | PathLike = None,  # optional. Switch prod.keys file.
    prod_keys_text: str | bytes = None,     # optional. Switch prod.keys contents. Wins over prod_keys_path.
    header_key: bytes = None,               # optional. Switch header key, 32 bytes. Wins over both.
    filename_fallback: bool = False,        # optional. Scan the file name when the binary gives nothing;
                                            #   source reports which happened.
    allow_3ds_homebrew: bool = False,       # optional.
) -> SigilResult                            # Raises when nothing identified the file.
```

```python
SigilResult(
    title_id: str,              # "" on gb, gbc, snes.
    raw_serial: str,            # As found in the binary.
    save_id: str,               # On-disk name the emulator keys the save by. "" on gb, gbc, snes.
    platform: str,
    source: str,                # "binary", "filename".
    usage: str,                 # "folder-exact", "folder-prefix", "file-exact", "file-prefix",
                                #   "folder-split". README "usage" says how to apply save_id for each.
    experimental: bool,
    switch_content_type: str,   # "unknown", "application", "patch", "addon".
    title_version: int,         # Switch only.
    features: int,              # Bit set. FEATURE_RTC: cart has a clock. has_rtc reads it.
)
```

Store `title_id`, `save_id`, `platform`, `features`. Rebuild later, or
build for a platform sigil cannot extract (Sega CD raises). Every field
is passed every time; a new frozen value comes back, nothing is kept
between calls:

```python
sigil.SigilResult.persisted(
    platform: str,      # required. Slug from the README platform table, or segacd, fds.
    title_id: str,      # required. Stored title_id, or "" when the platform has none.
    save_id: str,       # required. Stored save_id, or "" when the platform has none.
    features: int,      # required. Stored features, or 0.
) -> SigilResult
```

## 2. Locate the saves

At sync time. No file is read.

```python
sigil.locate_saves(
    game: SigilResult,                          # required. Step 1.
    core: str,                                  # required. Libretro core name without _libretro:
                                                #   genesis_plus_gx, mednafen_psx_hw, mame2003_plus. A core
                                                #   without a README layout row gets the default row
                                                #   (<stem>.srm, plus <stem>.rtc when the cart has a clock).
    content_path: str,                          # required. The path you handed the emulator, verbatim:
                                                #   rom, .m3u, .cue, .chd, or archive.zip#member.ext when a
                                                #   member was loaded. Only the file name part is used.
    *,
    save_root: str | PathLike = None,           # optional. Directory the emulator writes this game's save
                                                #   into. RetroArch: savefile_directory, plus the core-named
                                                #   subfolder when sort_savefiles_enable is on. Sigil lists
                                                #   it and the subfolders the core writes into.
    listing: Iterable[str] = None,              # optional. Instead of save_root: every file directly in the
                                                #   root plus every file under layout_subdirs(core), four
                                                #   levels deep, as root-relative / paths. list_save_root
                                                #   builds this.
    options: Mapping[str, str] = None,          # optional. Core option key to value, the strings the core
                                                #   defines and RetroArch writes to <core>.opt, never display
                                                #   labels: {"genesis_plus_gx_system_bram": "per game"}.
                                                #   Pass all of them; only the keys the row names are read.
) -> SigilSaveUnit                              # Hashes empty.
```

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
)

SigilSaveMember(
    path: str,      # Root-relative.
    entry: str,     # Archive entry name.
    role: str,      # "primary", "sidecar", "rtc".
    present: bool,
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
content_hash: str     # What the RomM server computes for the artifact.
identity_hash: str    # The same over the non-rtc members. Different content_hash, same
                      #   identity_hash: a clock tick, not a new save.
```

## Upload and restore

Upload by `shape`. `single` sends the member as is. `multi` zips the
members flat, each under its `entry`. `folder` zips the `key` folder so
entries read `<key>/<file>`. Name the upload `artifact`. Hash rules and
the layout table: README, "Save units".

Restore by `path`. Unzip a `multi` artifact so every entry lands at its
member's `path` under the root. Unzip a `folder` artifact from the
root's parent of the key folder. `expected` says where a primary goes
when the emulator has not created one yet.

## Memory cards

List the saves on a memory card or backup RAM volume: PS1 cards (the raw
card as `.mcr`, `.mcd` or `.srm`, DexDrive `.gme`, PSP or Vita `.vmp`),
PS2 `.ps2` file cards, GameCube raw cards, Dreamcast VMUs, and Saturn and
Sega CD backup RAM.

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
    entries: tuple[SigilCardEntry, ...],   # Live saves, in directory order.
    corrupt_entries: tuple[SigilCardEntry, ...],   # The left-out saves the card still names; blocks is 0.
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

## Sync

`collect` gathers one game's saves into the unit that travels to RomM;
`restore` puts a unit back and reads it back, removing files where a save
folder holds a save the unit lacks. PS1 and PS2 memory cards, PCSX2 folder
cards, GameCube cards and Dolphin's GCI folder, Saturn and Sega CD backup RAM, and
Dreamcast VMUs work today. `restore` raises
`SigilNotFoundError` for a unit holding none of the game's saves, and
ignores other games' saves inside a unit. What a unit holds, how Saturn and
Sega CD saves find their owner, and how genesis_plus_gx's region file is
picked: [c.md](c.md), "Sync". For Saturn and Sega CD, build the game with
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
    claimed: Iterable[str] = (),            # Saturn, Sega CD: names from `unowned` the user gave this game.
    companions: Iterable[SigilCompanion] = (),   # Games whose saves this game reads, in the order they go on.
    repair: bool = False,                   # Rebuild what SigilDamagedError named, where sigil can.
) -> SigilSyncResult
    # Raises SigilDamagedError when a file holding the saves is damaged and repair is False,
    #   or isn't a card sigil can read at all (repair doesn't change that).

sigil.restore(unit: bytes, ..., overwrite_local: bool = False) -> SigilSyncResult
    # Each of these writes nothing: SigilConflictError (the saves under save_root changed since
    #   the last sync), SigilUncollectedError (a shared volume holds saves no collect has passed
    #   on yet), SigilNoSpaceError (the saves don't fit; `blocks_short` says by how much),
    #   SigilRegionError (a companion's save from another region), SigilNoTargetError (the
    #   unit holds a volume the emulator's settings keep no file for), SigilAmbiguousError (more
    #   than one file could be the emulator's card) and SigilDamagedError. The last five name
    #   the save, member or files in `problem`. c.md, "Sync", has the table.

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
    conflict: bool,
    state: bytes,               # Store it once every upload succeeded; pass it back next time.
    holding: bytes | None,      # Saturn, Sega CD: zip of the saves on a shared volume with no known
                                #   owner. Upload it with the unit.
    unowned: tuple[str, ...],   # The names of the saves in holding, decoded as SigilCardEntry.name
                                #   is. Pass them to `claimed` as they are.
    restore_again: bool,        # Unmanaged: the saves the last restore wrote were overwritten.
                                #   Restore again instead of uploading.
    companions: tuple[SigilCompanionResult, ...],
)
```

A companion's saves go on the game's card beside the game's own and stay
out of the game's unit; [c.md](c.md), "Sync", has the rules.

## Helpers

```python
sigil.content_stem(content_path: str) -> str                  # Stem the save is named after.
sigil.layout_subdirs(core: str) -> list[str]                   # Subfolders the core writes into.
sigil.list_save_root(root: str | PathLike, core: str) -> list[str]
sigil.platform_from_slug(slug: str) -> int                     # PLATFORM_AUTO when unknown.
sigil.platform_to_slug(platform: int) -> str
sigil.load_header_key_from_prod_keys(path: str | PathLike) -> bytes   # 32 bytes.
sigil.version() -> str
```
