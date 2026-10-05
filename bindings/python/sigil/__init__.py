# SPDX-License-Identifier: MPL-2.0
"""Pythonic facade over the sigil C library: title ID extraction and save units."""

from __future__ import annotations

import os
from collections.abc import Iterable, Mapping
from dataclasses import dataclass, replace
from typing import Literal

from sigil._sigil import ffi, lib

__all__ = [
    "FEATURE_RTC",
    "PLATFORM_AUTO",
    "SigilCardEntry",
    "SigilCardListing",
    "SigilConflictError",
    "SigilCryptoError",
    "SigilDamagedError",
    "SigilExistsError",
    "SigilRegionError",
    "SigilNoTargetError",
    "SigilAmbiguousError",
    "SigilNoSpaceError",
    "SigilCompanion",
    "SigilCompanionResult",
    "SigilSyncResult",
    "SigilUncollectedError",
    "collect",
    "restore",
    "SigilError",
    "SigilIOError",
    "SigilInvalidArgError",
    "SigilNeedsKeyError",
    "SigilKeysIncompatibleError",
    "SigilNotFoundError",
    "SigilOOMError",
    "SigilProfile",
    "SigilResult",
    "SigilSaveAlternate",
    "SigilSaveMember",
    "SigilSaveUnit",
    "SigilUnknownPlatformError",
    "SigilUnsupportedFormatError",
    "content_stem",
    "extract",
    "hash_saves",
    "layout_subdirs",
    "list_card",
    "list_profiles",
    "list_save_root",
    "load_header_key_from_prod_keys",
    "locate_saves",
    "platform_from_slug",
    "platform_to_slug",
    "save_base",
    "version",
]

PLATFORM_AUTO: int = lib.SIGIL_PLATFORM_AUTO
FEATURE_RTC: int = lib.SIGIL_FEATURE_RTC


class SigilError(Exception):
    """Base error for sigil failures. `code` holds the C error code. After collect or restore,
    `problem` names the save, unit member or files at fault when the error has one (no space,
    region, damaged, no target, ambiguous, exists), decoded as SigilCardEntry.name is."""

    problem: str = ""
    profiles: tuple[SigilProfile, ...] = ()

    def __init__(self, code: int, message: str):
        super().__init__(message)
        self.code = code


class SigilInvalidArgError(SigilError):
    pass


class SigilIOError(SigilError):
    pass


class SigilUnknownPlatformError(SigilError):
    pass


class SigilUnsupportedFormatError(SigilError):
    pass


class SigilNotFoundError(SigilError):
    pass


class SigilNeedsKeyError(SigilError):
    pass


class SigilCryptoError(SigilError):
    pass


class SigilOOMError(SigilError):
    pass


class SigilConflictError(SigilError):
    """The saves on disk changed since the last sync; restore wrote nothing."""


class SigilExistsError(SigilError):
    pass


class SigilNoSpaceError(SigilError):
    """The saves don't fit; `problem` names the save that didn't and `blocks_short` the blocks
    it lacked (0 when the free blocks were there but a directory slot or a Dreamcast game file's
    starting blocks weren't)."""

    blocks_short: int = 0


class SigilUncollectedError(SigilError):
    """A shared volume holds saves no collect has passed on yet; restore wrote nothing."""


class SigilDamagedError(SigilError):
    """A file the saves are in is damaged; `problem` names it. `repair=True` rebuilds it
    where sigil can; a card sigil can't read at all stays refused."""


class SigilRegionError(SigilError):
    """A companion's save, named in `problem`, belongs to another region than the game."""


class SigilNoTargetError(SigilError):
    """The unit holds a volume or member, named in `problem`, that has no file to go in: the
    emulator's settings keep none, no profile is there for an account save, or the folder lies
    outside the save root. Restore wrote nothing."""


class SigilAmbiguousError(SigilError):
    """More than one file could be the card the emulator uses, more than one profile and none
    picked, or more than one emulator folder under the save root; `problem` names them, one per
    line, and `profiles` lists the profiles. Nothing was written. When you don't know which
    profile the user plays as, ask them from `profiles` (or list_profiles) and pass `profile`."""


class SigilKeysIncompatibleError(SigilError):
    """The keys given don't open this content: the key file lacks the key for its key
    generation, or its header key is wrong."""


_ERROR_CLASSES = {
    lib.SIGIL_ERR_INVALID_ARG: SigilInvalidArgError,
    lib.SIGIL_ERR_IO: SigilIOError,
    lib.SIGIL_ERR_UNKNOWN_PLATFORM: SigilUnknownPlatformError,
    lib.SIGIL_ERR_UNSUPPORTED_FORMAT: SigilUnsupportedFormatError,
    lib.SIGIL_ERR_NOT_FOUND: SigilNotFoundError,
    lib.SIGIL_ERR_NEEDS_KEY: SigilNeedsKeyError,
    lib.SIGIL_ERR_CRYPTO: SigilCryptoError,
    lib.SIGIL_ERR_OOM: SigilOOMError,
    lib.SIGIL_ERR_CONFLICT: SigilConflictError,
    lib.SIGIL_ERR_EXISTS: SigilExistsError,
    lib.SIGIL_ERR_NO_SPACE: SigilNoSpaceError,
    lib.SIGIL_ERR_UNCOLLECTED: SigilUncollectedError,
    lib.SIGIL_ERR_DAMAGED: SigilDamagedError,
    lib.SIGIL_ERR_REGION: SigilRegionError,
    lib.SIGIL_ERR_NO_TARGET: SigilNoTargetError,
    lib.SIGIL_ERR_AMBIGUOUS: SigilAmbiguousError,
    lib.SIGIL_ERR_KEYS_INCOMPATIBLE: SigilKeysIncompatibleError,
}

_SOURCE_NAMES: dict[int, Literal["binary", "filename"]] = {
    lib.SIGIL_SOURCE_BINARY: "binary",
    lib.SIGIL_SOURCE_FILENAME: "filename",
}

_USAGE_NAMES = {
    lib.SIGIL_USAGE_FOLDER_EXACT: "folder-exact",
    lib.SIGIL_USAGE_FOLDER_PREFIX: "folder-prefix",
    lib.SIGIL_USAGE_FILE_EXACT: "file-exact",
    lib.SIGIL_USAGE_FILE_PREFIX: "file-prefix",
    lib.SIGIL_USAGE_FOLDER_SPLIT: "folder-split",
}

_SWITCH_CONTENT_NAMES: dict[int, Literal["unknown", "application", "patch", "addon"]] = {
    lib.SIGIL_SWITCH_CONTENT_UNKNOWN: "unknown",
    lib.SIGIL_SWITCH_CONTENT_APPLICATION: "application",
    lib.SIGIL_SWITCH_CONTENT_PATCH: "patch",
    lib.SIGIL_SWITCH_CONTENT_ADDON: "addon",
}

SaveShape = Literal["none", "single", "multi", "folder"]
SaveRole = Literal["primary", "sidecar", "rtc"]
SaveArea = Literal["none", "account", "device"]

_AREA_NAMES: dict[int, SaveArea] = {
    lib.SIGIL_SAVE_AREA_NONE: "none",
    lib.SIGIL_SAVE_AREA_ACCOUNT: "account",
    lib.SIGIL_SAVE_AREA_DEVICE: "device",
}

_SHAPE_NAMES: dict[int, SaveShape] = {
    lib.SIGIL_SAVE_SHAPE_NONE: "none",
    lib.SIGIL_SAVE_SHAPE_SINGLE: "single",
    lib.SIGIL_SAVE_SHAPE_MULTI: "multi",
    lib.SIGIL_SAVE_SHAPE_FOLDER: "folder",
}

_ROLE_NAMES: dict[int, SaveRole] = {
    lib.SIGIL_SAVE_ROLE_PRIMARY: "primary",
    lib.SIGIL_SAVE_ROLE_SIDECAR: "sidecar",
    lib.SIGIL_SAVE_ROLE_RTC: "rtc",
}

CardFormat = Literal[
    "unknown",
    "ps1-raw",
    "ps1-gme",
    "ps1-vmp",
    "ps2",
    "gamecube-raw",
    "dreamcast-vmu",
    "saturn-backup",
    "segacd-bram",
]

_CARD_FORMAT_NAMES: dict[int, CardFormat] = {
    lib.SIGIL_CARD_FORMAT_UNKNOWN: "unknown",
    lib.SIGIL_CARD_FORMAT_PS1_RAW: "ps1-raw",
    lib.SIGIL_CARD_FORMAT_PS1_GME: "ps1-gme",
    lib.SIGIL_CARD_FORMAT_PS1_VMP: "ps1-vmp",
    lib.SIGIL_CARD_FORMAT_PS2: "ps2",
    lib.SIGIL_CARD_FORMAT_GAMECUBE_RAW: "gamecube-raw",
    lib.SIGIL_CARD_FORMAT_DREAMCAST_VMU: "dreamcast-vmu",
    lib.SIGIL_CARD_FORMAT_SATURN_BACKUP: "saturn-backup",
    lib.SIGIL_CARD_FORMAT_SEGACD_BRAM: "segacd-bram",
}

_SUBDIR_LIST_DEPTH = 12
_SUBDIR_CAP = 16
_BASE_SEARCH_DEPTH = 5
_PATH_CAP = 4096


@dataclass(frozen=True)
class SigilResult:
    """A successful extraction."""

    title_id: str
    raw_serial: str
    save_id: str
    platform: str
    source: Literal["binary", "filename"]
    usage: str
    experimental: bool
    switch_content_type: Literal["unknown", "application", "patch", "addon"]
    title_version: int
    features: int = 0
    n64_header: str = ""
    n64_md5: str = ""
    n64_md5_n64: str = ""

    @property
    def has_rtc(self) -> bool:
        """The cart carries a real-time clock; a libretro frontend persists it as ``<stem>.rtc``."""
        return bool(self.features & lib.SIGIL_FEATURE_RTC)

    @classmethod
    def persisted(cls, platform: str, title_id: str, save_id: str, features: int, raw_serial: str = "",
                  n64_header: str = "", n64_md5: str = "", n64_md5_n64: str = "") -> SigilResult:
        """A result rebuilt from stored columns, or built for a platform that has no title id.

        ``raw_serial`` names pcsx_rearmed's per-disc cards, which follow the boot file as written.
        The ``n64_*`` fields name the standalone N64 emulators' saves.
        """
        return cls(
            title_id=title_id,
            raw_serial=raw_serial,
            save_id=save_id,
            platform=platform,
            source="binary",
            usage="folder-exact",
            experimental=False,
            switch_content_type="unknown",
            title_version=0,
            features=features,
            n64_header=n64_header,
            n64_md5=n64_md5,
            n64_md5_n64=n64_md5_n64,
        )


@dataclass(frozen=True)
class SigilSaveMember:
    """One file of a save unit. `path` is relative to the save root; `entry` is its archive name.
    `area` says whose it is on an emulator that keeps saves per user profile."""

    path: str
    entry: str
    role: SaveRole
    present: bool
    area: SaveArea = "none"


@dataclass(frozen=True)
class SigilProfile:
    """A user profile the emulator lists. `id` is how its save folder is named."""

    id: str
    name: str


@dataclass(frozen=True)
class SigilSaveAlternate:
    """A file under the save root the layout would take with other option values: a save kept under
    another mode or by an older build of the core. Passing `options` takes it."""

    path: str
    shared: bool
    options: Mapping[str, str]


@dataclass(frozen=True)
class SigilSaveUnit:
    """Every file under a save root that belongs to one game, and the hashes RomM computes for it."""

    key: str
    shape: SaveShape
    members: tuple[SigilSaveMember, ...]
    expected: tuple[SigilSaveMember, ...]
    unkeyed: tuple[str, ...]
    artifact: str
    content_hash: str
    identity_hash: str
    alternates: tuple[SigilSaveAlternate, ...] = ()


@dataclass(frozen=True)
class SigilCardEntry:
    """One save on a memory card. `owner_id` is the product code it carries, or "" when it has none."""

    name: str
    owner_id: str
    blocks: int
    first_block: int


@dataclass(frozen=True)
class SigilCardListing:
    """The saves on a memory card and the space left on it."""

    format: CardFormat
    total_blocks: int
    free_blocks: int
    free_slots: int
    corrupt_count: int
    entries: tuple[SigilCardEntry, ...]
    corrupt_entries: tuple[SigilCardEntry, ...]   # left-out saves the card still names; blocks is 0


@dataclass(frozen=True)
class SigilCompanion:
    """A game whose saves this game reads, as a sequel reads its prequel's. `unit` is its unit
    from RomM for restore, or None to leave its saves as they are."""

    game_ids: tuple[str, ...]
    unit: bytes | None = None


@dataclass(frozen=True)
class SigilCompanionResult:
    """A companion's saves found with the game's, as its unit; data is None when none are there."""

    data: bytes | None
    content_hash: str
    identity_hash: str
    changed: bool


@dataclass(frozen=True)
class SigilSyncResult:
    """What collect or restore produced. Store `state` and pass it to the next call for this game."""

    artifact: str
    shape: SaveShape
    data: bytes | None
    content_hash: str
    identity_hash: str
    changed: bool
    state: bytes
    holding: bytes | None
    unowned: tuple[str, ...]
    restore_again: bool
    companions: tuple[SigilCompanionResult, ...]
    profiles: tuple[SigilProfile, ...] = ()   # every profile the emulator lists
    profile: str = ""                         # the profile whose saves were taken or written
    alternates: tuple[SigilSaveAlternate, ...] = ()  # files other option values would take


def _raise_error(code: int) -> None:
    message = ffi.string(lib.sigil_strerror(code)).decode("utf-8", "replace")
    raise _ERROR_CLASSES.get(code, SigilError)(code, message)


def version() -> str:
    """Version string of the underlying C library."""
    return ffi.string(lib.sigil_version()).decode("utf-8")


def platform_from_slug(slug: str) -> int:
    """Parse a platform slug; unknown slugs return PLATFORM_AUTO."""
    return lib.sigil_platform_from_slug(slug.encode("utf-8"))


def platform_to_slug(platform: int) -> str:
    """Canonical slug for a platform value; invalid values return "auto"."""
    return ffi.string(lib.sigil_platform_to_slug(platform)).decode("utf-8")


def load_header_key_from_prod_keys(path: str | os.PathLike[str]) -> bytes:
    """Read the 32-byte Switch header key from a prod.keys file."""
    out = ffi.new("uint8_t[32]")
    rc = lib.sigil_load_header_key_from_prod_keys(os.fsencode(path), out)
    if rc != lib.SIGIL_OK:
        _raise_error(rc)
    return bytes(ffi.buffer(out))


def extract(
    path: str | os.PathLike[str],
    platform: str = "auto",
    *,
    prod_keys_path: str | os.PathLike[str] | None = None,
    prod_keys_text: str | bytes | None = None,
    header_key: bytes | None = None,
    filename_fallback: bool = True,
    allow_3ds_homebrew: bool = False,
) -> SigilResult:
    """Extract the title ID from a ROM file.

    `platform` is a slug ("ps2", "switch", ...); "auto" sniffs from the
    file extension. When the binary gives nothing, the file name is
    scanned unless `filename_fallback` is False; `source` reports which.
    """
    keepalive: list[object] = []

    opts = ffi.new("sigil_options *")
    opts.struct_version = lib.SIGIL_OPTIONS_V1
    flags = 0
    if filename_fallback:
        flags |= lib.SIGIL_FLAG_FILENAME_FALLBACK
    if allow_3ds_homebrew:
        flags |= lib.SIGIL_FLAG_3DS_ALLOW_HOMEBREW
    opts.flags = flags

    if header_key is not None or prod_keys_path is not None or prod_keys_text is not None:
        support = ffi.new("sigil_support *")
        support.struct_version = lib.SIGIL_SUPPORT_V1
        keepalive.append(support)

        if header_key is not None:
            if len(header_key) != 32:
                raise ValueError(f"header_key must be 32 bytes, got {len(header_key)}")
            key_buf = ffi.new("uint8_t[32]", bytes(header_key))
            keepalive.append(key_buf)
            support.switch_header_key = key_buf
        if prod_keys_path is not None:
            keys_path = ffi.new("char[]", os.fsencode(prod_keys_path))
            keepalive.append(keys_path)
            support.switch_prod_keys_path = keys_path
        if prod_keys_text is not None:
            text = (
                prod_keys_text.encode("utf-8")
                if isinstance(prod_keys_text, str)
                else bytes(prod_keys_text)
            )
            text_buf = ffi.new("char[]", text)
            keepalive.append(text_buf)
            support.switch_prod_keys_text = text_buf
            support.switch_prod_keys_text_len = len(text)

        opts.support = support

    result = ffi.new("sigil_result *")
    result.struct_version = lib.SIGIL_RESULT_V4

    rc = lib.sigil_extract_from_path(
        os.fsencode(path), platform_from_slug(platform), opts, result
    )
    if rc != lib.SIGIL_OK:
        _raise_error(rc)

    return SigilResult(
        title_id=ffi.string(result.title_id).decode("utf-8", "replace"),
        raw_serial=ffi.string(result.raw_serial).decode("utf-8", "replace"),
        save_id=ffi.string(result.save_id).decode("utf-8", "replace"),
        platform=platform_to_slug(result.platform),
        source=_SOURCE_NAMES.get(result.source, "binary"),
        usage=_USAGE_NAMES.get(result.usage, "folder-exact"),
        experimental=bool(result.experimental),
        switch_content_type=_SWITCH_CONTENT_NAMES.get(result.switch_content_type, "unknown"),
        title_version=int(result.title_version),
        features=int(result.features),
        n64_header=ffi.string(result.n64_header).decode("utf-8", "replace"),
        n64_md5=ffi.string(result.n64_md5).decode("utf-8", "replace"),
        n64_md5_n64=ffi.string(result.n64_md5_n64).decode("utf-8", "replace"),
    )


def content_stem(content_path: str) -> str:
    """The base name RetroArch names save files after; see docs/save-units.md, "Stem"."""
    out = ffi.new("char[]", lib.SIGIL_SAVE_ENTRY_MAX)
    lib.sigil_content_stem(_path_bytes(content_path), out, lib.SIGIL_SAVE_ENTRY_MAX)
    return _path_text(out)


def layout_subdirs(layout: str) -> list[str]:
    """Subfolders under the save root a layout writes into, so the caller knows what to list."""
    out = ffi.new("const char *[]", _SUBDIR_CAP)
    n = lib.sigil_save_layout_subdirs(layout.encode("utf-8"), out, _SUBDIR_CAP)
    return [ffi.string(out[i]).decode("utf-8") for i in range(n)]


def save_base(layout: str, path: str | os.PathLike[str]) -> tuple[str, str]:
    """On a layout with profiles, the emulator's base folder for `path` and the profile folder
    `path` lies in ("" for none); collect and restore re-root there themselves. On other layouts,
    `path` itself and ""."""
    base = ffi.new("char[]", _PATH_CAP)
    profile = ffi.new("char[]", lib.SIGIL_PROFILE_ID_MAX)
    rc = lib.sigil_save_base(layout.encode("utf-8"), os.fsencode(path), base, _PATH_CAP, profile,
                             lib.SIGIL_PROFILE_ID_MAX)
    if rc != lib.SIGIL_OK:
        _raise_error(rc)
    return os.fsdecode(ffi.string(base)), _text(profile)


def list_profiles(core: str, save_root: str | os.PathLike[str]) -> tuple[SigilProfile, ...]:
    """The profiles the emulator lists around `save_root`, for asking the user which one they play
    as when collect or restore raised SigilAmbiguousError. Raises SigilUnsupportedFormatError for a
    core whose saves aren't kept per profile."""
    keepalive: list[object] = []
    root, _ = _rooted(core, save_root, None)
    paths = [ffi.new("char[]", _path_bytes(p)) for p in list_save_root(root, core)]
    keepalive.extend(paths)
    listing = ffi.new("char *[]", paths if paths else [ffi.NULL])
    req = ffi.new("sigil_save_request *")
    req.struct_version = lib.SIGIL_SAVE_REQUEST_V1
    layout = ffi.new("char[]", core.encode("utf-8"))
    root_buf = ffi.new("char[]", os.fsencode(root))
    opener = _open_callback(os.fsencode(root))
    keepalive.extend([listing, layout, root_buf, opener])
    req.layout = layout
    req.listing = listing
    req.listing_count = len(paths)
    req.root_path = root_buf
    req.open = opener
    out = ffi.new("sigil_save_profile **")
    count = ffi.new("size_t *")
    rc = lib.sigil_save_profiles(req, out, count)
    if rc != lib.SIGIL_OK:
        _raise_error(rc)
    try:
        return tuple(SigilProfile(id=_text(out[0][i].id), name=_text(out[0][i].name)) for i in range(count[0]))
    finally:
        lib.sigil_save_profiles_free(out[0])


def _layout_top(layout: str) -> str | None:
    top = lib.sigil_save_layout_top(layout.encode("utf-8"))
    return ffi.string(top).decode("utf-8") if top != ffi.NULL else None


def _bases_below(root: str, top: str) -> list[str]:
    """Root-relative folders under `root` that hold the layout's top folder."""
    found: list[str] = []

    def walk(directory: str, relative: str, depth: int) -> None:
        if depth == 0:
            return
        try:
            with os.scandir(directory) as it:
                entries = [e for e in it if e.is_dir(follow_symlinks=False)]
        except OSError:
            return
        for entry in entries:
            if entry.name == top and relative:
                found.append(relative)
            elif entry.name != top:
                walk(entry.path, f"{relative}/{entry.name}" if relative else entry.name, depth - 1)

    walk(root, "", _BASE_SEARCH_DEPTH)
    return found


def list_save_root(root: str | os.PathLike[str], layout: str) -> list[str]:
    """Root-relative paths of the files directly in `root` plus those under the layout's subfolders.
    On a layout with profiles whose base sits below `root`, the subfolders under each such base."""
    root = os.fspath(root)
    out: list[str] = []
    if os.path.isdir(root):
        with os.scandir(root) as it:
            out.extend(e.name for e in it if e.is_file())
    subdirs = layout_subdirs(layout)
    top = _layout_top(layout)
    bases = [""]
    if top and not os.path.isdir(os.path.join(root, top)):
        bases = _bases_below(root, top) or [""]
    for base in bases:
        for subdir in subdirs:
            relative = f"{base}/{subdir}" if base else subdir
            _list_recursive(os.path.join(root, relative), relative, _SUBDIR_LIST_DEPTH, out)
    return out


def _list_recursive(directory: str, relative: str, depth: int, out: list[str]) -> None:
    if depth == 0 or not os.path.isdir(directory):
        return
    with os.scandir(directory) as it:
        for entry in it:
            rel = f"{relative}/{entry.name}"
            if entry.is_file():
                out.append(rel)
            elif entry.is_dir():
                _list_recursive(entry.path, rel, depth - 1, out)


def _fill_save_request(req, keepalive: list[object], game: SigilResult, core: str, content_path: str,
                       listing: Iterable[str], options: Mapping[str, str] | None, root: str | None = None,
                       profile: str | None = None) -> None:
    """Fills a sigil_save_request; every buffer it points at goes into `keepalive`."""

    def c_str(value: str | bytes):
        buf = ffi.new("char[]", value if isinstance(value, bytes) else value.encode("utf-8"))
        keepalive.append(buf)
        return buf

    paths = [c_str(_path_bytes(p)) for p in listing]
    option_items = list((options or {}).items())

    result = ffi.new("sigil_result *")
    result.struct_version = lib.SIGIL_RESULT_V4
    result.title_id = game.title_id.encode("utf-8")
    result.raw_serial = game.raw_serial.encode("utf-8")
    result.save_id = game.save_id.encode("utf-8")
    result.platform = platform_from_slug(game.platform) if game.platform else lib.SIGIL_PLATFORM_AUTO
    result.features = game.features
    result.n64_header = game.n64_header.encode("utf-8")
    result.n64_md5 = game.n64_md5.encode("utf-8")
    result.n64_md5_n64 = game.n64_md5_n64.encode("utf-8")
    keepalive.append(result)

    req.struct_version = lib.SIGIL_SAVE_REQUEST_V1
    req.layout = c_str(core)
    req.platform = c_str(game.platform) if game.platform else ffi.NULL
    req.content_path = c_str(_path_bytes(content_path))
    req.result = result
    req.features = game.features

    option_array = ffi.new("sigil_save_option[]", max(len(option_items), 1))
    for i, (key, value) in enumerate(option_items):
        option_array[i].key = c_str(key)
        option_array[i].value = c_str(value)
    keepalive.append(option_array)
    req.options = option_array
    req.option_count = len(option_items)

    listing_array = ffi.new("char *[]", paths if paths else [ffi.NULL])
    keepalive.append(listing_array)
    req.listing = listing_array
    req.listing_count = len(paths)
    req.open = ffi.NULL
    req.open_ctx = ffi.NULL
    if root is not None:
        root_buf = ffi.new("char[]", os.fsencode(root))
        keepalive.append(root_buf)
        req.root_path = root_buf
    if profile:
        req.profile = c_str(profile)


def _rooted(core: str, save_root: str | os.PathLike[str], profile: str | None) -> tuple[str, str | None]:
    """The folder to list and write under for `save_root`, and the profile: one given wins over
    the one `save_root` lies in."""
    base, implied = save_base(core, save_root)
    return base, profile or implied or None


def _open_callback(root_bytes: bytes):
    @ffi.callback("sigil_io *(void *, const char *)")
    def open_member(_ctx, relative_path):
        return lib.sigil_io_open_file(os.path.join(root_bytes, ffi.string(relative_path)))

    return open_member


def locate_saves(
    game: SigilResult,
    core: str,
    content_path: str,
    *,
    save_root: str | os.PathLike[str] | None = None,
    listing: Iterable[str] | None = None,
    options: Mapping[str, str] | None = None,
    profile: str | None = None,
) -> SigilSaveUnit:
    """The files under a save root that belong to `game` when `core` runs `content_path`.

    No save is read; on a layout with profiles the emulator's profile list is, when `save_root`
    is given. docs/quickstart-guides/python.md defines every input.
    """
    keepalive: list[object] = []
    root = None
    if save_root is not None:
        root, profile = _rooted(core, save_root, profile)
    if listing is None:
        listing = list_save_root(root, core) if root is not None else []
    req = ffi.new("sigil_save_request *")
    _fill_save_request(req, keepalive, game, core, content_path, listing, options, root, profile)
    if root is not None and _layout_top(core):
        opener = _open_callback(os.fsencode(root))
        keepalive.append(opener)
        req.open = opener

    out = ffi.new("sigil_save_unit **")
    rc = lib.sigil_save_resolve(req, out)
    if rc != lib.SIGIL_OK:
        _raise_error(rc)
    unit = out[0]
    try:
        return SigilSaveUnit(
            key=_path_text(unit.key),
            shape=_SHAPE_NAMES.get(unit.shape, "none"),
            members=tuple(_member(unit.members[i]) for i in range(unit.member_count)),
            expected=tuple(_member(unit.expected[i]) for i in range(unit.expected_count)),
            unkeyed=tuple(_path_text(unit.unkeyed[i]) for i in range(unit.unkeyed_count)),
            artifact=_path_text(unit.artifact),
            content_hash=_text(unit.content_hash),
            identity_hash=_text(unit.identity_hash),
            alternates=_alternates(unit),
        )
    finally:
        lib.sigil_save_unit_free(unit)


def hash_saves(saves: SigilSaveUnit, save_root: str | os.PathLike[str]) -> SigilSaveUnit:
    """`saves` with `content_hash` and `identity_hash` computed from the files under `save_root`."""
    if not saves.members:
        return saves
    shape_code = next(code for code, name in _SHAPE_NAMES.items() if name == saves.shape)
    role_codes = {name: code for code, name in _ROLE_NAMES.items()}

    members = ffi.new("sigil_save_member[]", len(saves.members))
    for i, m in enumerate(saves.members):
        members[i].path = _path_bytes(m.path)
        members[i].entry = _path_bytes(m.entry)
        members[i].role = role_codes[m.role]
        members[i].present = 1

    unit = ffi.new("sigil_save_unit *")
    unit.struct_version = lib.SIGIL_SAVE_UNIT_V1
    unit.key = _path_bytes(saves.key)
    unit.shape = shape_code
    unit.members = members
    unit.member_count = len(saves.members)

    rc = lib.sigil_save_hash(unit, _open_callback(os.fsencode(save_root)), ffi.NULL)
    if rc != lib.SIGIL_OK:
        _raise_error(rc)
    return replace(saves, content_hash=_text(unit.content_hash), identity_hash=_text(unit.identity_hash))


_SYNC_MODES = {"managed": lib.SIGIL_SYNC_MANAGED, "unmanaged": lib.SIGIL_SYNC_UNMANAGED}


def _sync(
    unit: bytes | None,
    game: SigilResult,
    core: str,
    content_path: str,
    save_root: str | os.PathLike[str],
    listing: Iterable[str] | None,
    options: Mapping[str, str] | None,
    game_ids: Iterable[str],
    state: bytes | None,
    mode: Literal["managed", "unmanaged"],
    overwrite_local: bool,
    claimed: Iterable[str],
    companions: Iterable[SigilCompanion],
    repair: bool,
    profile: str | None,
) -> SigilSyncResult:
    keepalive: list[object] = []
    root, profile = _rooted(core, save_root, profile)
    root_bytes = os.fsencode(root)
    if listing is None:
        listing = list_save_root(root, core)

    req = ffi.new("sigil_sync_request *")
    req.struct_version = lib.SIGIL_SYNC_REQUEST_V1
    _fill_save_request(req.save, keepalive, game, core, content_path, list(listing), options, root, profile)

    ids = [ffi.new("char[]", i.encode("utf-8")) for i in game_ids]
    keepalive.extend(ids)
    id_array = ffi.new("char *[]", ids if ids else [ffi.NULL])
    keepalive.append(id_array)
    req.game_ids = id_array
    req.game_id_count = len(ids)
    names = [ffi.new("char[]", n.encode("utf-8", "surrogateescape")) for n in claimed]
    keepalive.extend(names)
    name_array = ffi.new("char *[]", names if names else [ffi.NULL])
    keepalive.append(name_array)
    req.claimed = name_array
    req.claimed_count = len(names)
    companion_list = list(companions)
    if companion_list:
        array = ffi.new("sigil_sync_companion[]", len(companion_list))
        keepalive.append(array)
        for i, companion in enumerate(companion_list):
            cids = [ffi.new("char[]", g.encode("utf-8")) for g in companion.game_ids]
            keepalive.extend(cids)
            cid_array = ffi.new("char *[]", cids if cids else [ffi.NULL])
            keepalive.append(cid_array)
            array[i].game_ids = cid_array
            array[i].game_id_count = len(cids)
            if companion.unit is not None:
                unit_data = ffi.new("uint8_t[]", companion.unit)
                keepalive.append(unit_data)
                array[i].unit = unit_data
                array[i].unit_len = len(companion.unit)
        req.companions = array
        req.companion_count = len(companion_list)
    req.mode = _SYNC_MODES[mode]
    req.overwrite_local = 1 if overwrite_local else 0
    req.repair = 1 if repair else 0
    if state:
        state_buf = ffi.new("uint8_t[]", state)
        keepalive.append(state_buf)
        req.state = state_buf
        req.state_len = len(state)

    open_member = _open_callback(root_bytes)

    @ffi.callback("int(void *, const char *, const uint8_t *, size_t)")
    def write_member(_ctx, relative_path, data, length):
        path = os.path.join(root_bytes, ffi.string(relative_path))
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(ffi.buffer(data, length))
        except OSError:
            return -1
        return 0

    @ffi.callback("int(void *, const char *)")
    def remove_member(_ctx, relative_path):
        relative = ffi.string(relative_path)
        try:
            if relative.endswith(b"/"):
                os.rmdir(os.path.join(root_bytes, relative))
            else:
                os.remove(os.path.join(root_bytes, relative))
        except OSError:
            return -1
        return 0

    req.save.open = open_member
    req.write = write_member
    req.remove = remove_member

    out = ffi.new("sigil_sync_result **")
    if unit is None:
        rc = lib.sigil_collect(req, out)
    else:
        unit_buf = ffi.new("uint8_t[]", unit)
        rc = lib.sigil_restore(req, unit_buf, len(unit), out)
    if rc != lib.SIGIL_OK:
        message = ffi.string(lib.sigil_strerror(rc)).decode("utf-8", "replace")
        error = _ERROR_CLASSES.get(rc, SigilError)(rc, message)
        if out[0] != ffi.NULL:
            error.problem = _save_name(out[0].problem)
            error.profiles = _profiles(out[0])
            if isinstance(error, SigilNoSpaceError):
                error.blocks_short = int(out[0].blocks_short)
            lib.sigil_sync_result_free(out[0])
        raise error
    r = out[0]
    try:
        return SigilSyncResult(
            artifact=_path_text(r.artifact),
            shape=_SHAPE_NAMES.get(r.shape, "none"),
            data=bytes(ffi.buffer(r.data, r.len)) if r.data != ffi.NULL else None,
            content_hash=_text(r.content_hash),
            identity_hash=_text(r.identity_hash),
            changed=bool(r.changed),
            state=bytes(ffi.buffer(r.state, r.state_len)) if r.state != ffi.NULL else b"",
            holding=bytes(ffi.buffer(r.holding, r.holding_len)) if r.holding != ffi.NULL else None,
            unowned=tuple(_save_name(r.unowned[i]) for i in range(r.unowned_count)),
            restore_again=bool(r.restore_again),
            companions=tuple(
                SigilCompanionResult(
                    data=bytes(ffi.buffer(c.data, c.len)) if c.data != ffi.NULL else None,
                    content_hash=_text(c.content_hash),
                    identity_hash=_text(c.identity_hash),
                    changed=bool(c.changed),
                )
                for c in (r.companions[i] for i in range(r.companion_count))
            ),
            profiles=_profiles(r),
            profile=_text(r.profile),
            alternates=_alternates(r),
        )
    finally:
        lib.sigil_sync_result_free(r)


def _profiles(r) -> tuple[SigilProfile, ...]:
    return tuple(
        SigilProfile(id=_text(p.id), name=_text(p.name)) for p in (r.profiles[i] for i in range(r.profile_count))
    )


def _alternates(r) -> tuple[SigilSaveAlternate, ...]:
    return tuple(
        SigilSaveAlternate(
            path=_path_text(a.path),
            shared=bool(a.shared),
            options={ffi.string(o.key).decode(): ffi.string(o.value).decode() for o in a.options[0 : a.option_count]},
        )
        for a in (r.alternates[i] for i in range(r.alternate_count))
    )


def collect(
    game: SigilResult,
    core: str,
    content_path: str,
    save_root: str | os.PathLike[str],
    *,
    listing: Iterable[str] | None = None,
    options: Mapping[str, str] | None = None,
    game_ids: Iterable[str] = (),
    state: bytes | None = None,
    mode: Literal["managed", "unmanaged"] = "managed",
    claimed: Iterable[str] = (),
    companions: Iterable[SigilCompanion] = (),
    repair: bool = False,
    profile: str | None = None,
) -> SigilSyncResult:
    """`game`'s saves under `save_root` gathered into the unit that travels to RomM.

    Store the returned `state` once the unit, `holding` and each changed companion unit reached
    RomM. Raises SigilDamagedError and SigilAmbiguousError as `restore` does. docs/quickstart-guides/python.md
    defines every input.
    """
    return _sync(None, game, core, content_path, save_root, listing, options, game_ids, state, mode, False, claimed,
                 companions, repair, profile)


def restore(
    unit: bytes,
    game: SigilResult,
    core: str,
    content_path: str,
    save_root: str | os.PathLike[str],
    *,
    listing: Iterable[str] | None = None,
    options: Mapping[str, str] | None = None,
    game_ids: Iterable[str] = (),
    state: bytes | None = None,
    mode: Literal["managed", "unmanaged"] = "managed",
    overwrite_local: bool = False,
    claimed: Iterable[str] = (),
    companions: Iterable[SigilCompanion] = (),
    repair: bool = False,
    profile: str | None = None,
) -> SigilSyncResult:
    """Puts `unit`, and each companion's unit given, back under `save_root` and reads them back.

    Each of these writes nothing: SigilConflictError when the saves there changed since the last
    sync and `overwrite_local` is False; SigilUncollectedError when a shared Saturn or Sega CD
    volume holds saves no collect has passed on yet; SigilNoSpaceError when the saves don't fit;
    SigilRegionError for a companion's save from another region; SigilNoTargetError when the
    unit holds a volume or member with no file to go in; SigilAmbiguousError when more than one
    file could be the emulator's card, or more than one profile could take the saves;
    SigilDamagedError when a file the saves go in is damaged and `repair` is False, isn't a card
    sigil can read at all, or holds a corrupt save of the game or a companion (`repair` changes
    neither of the last two); SigilExistsError when Dolphin's GCI folder has no free name for a
    new save. The last six name the save, member or files in `problem`.
    """
    return _sync(unit, game, core, content_path, save_root, listing, options, game_ids, state, mode, overwrite_local,
                 claimed, companions, repair, profile)


def _card_entries(entries, count: int) -> tuple[SigilCardEntry, ...]:
    return tuple(
        SigilCardEntry(
            name=_save_name(e.name),
            owner_id=_text(e.owner_id),
            blocks=int(e.blocks),
            first_block=int(e.first_block),
        )
        for e in (entries[i] for i in range(count))
    )


def list_card(path: str | os.PathLike[str]) -> SigilCardListing:
    """The saves on the memory card at `path`. The card format is detected from its content."""
    io = lib.sigil_io_open_file(os.fsencode(path))
    if io == ffi.NULL:
        _raise_error(lib.SIGIL_ERR_IO)
    out = ffi.new("sigil_card_listing **")
    try:
        rc = lib.sigil_card_list(io, out)
    finally:
        lib.sigil_io_close(io)
    if rc != lib.SIGIL_OK:
        _raise_error(rc)
    listing = out[0]
    try:
        return SigilCardListing(
            format=_CARD_FORMAT_NAMES.get(listing.format, "unknown"),
            total_blocks=int(listing.total_blocks),
            free_blocks=int(listing.free_blocks),
            free_slots=int(listing.free_slots),
            corrupt_count=int(listing.corrupt_count),
            entries=_card_entries(listing.entries, listing.entry_count),
            corrupt_entries=_card_entries(listing.corrupt_entries, listing.corrupt_entry_count),
        )
    finally:
        lib.sigil_card_listing_free(listing)


def _text(chars) -> str:
    return ffi.string(chars).decode("utf-8", "replace")


def _path_bytes(path: str) -> bytes:
    """A file system name as the OS spells it: a name os.scandir decoded with surrogates for bytes
    that aren't UTF-8 goes back to those bytes."""
    return os.fsencode(path)


def _path_text(chars) -> str:
    """A path sigil handed back, decoded as os.scandir decodes names, so it opens the same file."""
    return os.fsdecode(ffi.string(chars))


def _save_name(chars) -> str:
    """A name stored on a card or volume. Bytes that aren't UTF-8 decode as surrogates, so the
    name passed back in `claimed` is the same bytes."""
    return ffi.string(chars).decode("utf-8", "surrogateescape")


def _member(m) -> SigilSaveMember:
    return SigilSaveMember(
        path=_path_text(m.path),
        entry=_path_text(m.entry),
        role=_ROLE_NAMES.get(m.role, "sidecar"),
        present=bool(m.present),
        area=_AREA_NAMES.get(m.area, "none"),
    )
