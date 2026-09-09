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
                                                #   root plus every file under layout_subdirs(core), three
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
