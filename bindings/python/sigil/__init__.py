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
    "SigilCryptoError",
    "SigilError",
    "SigilIOError",
    "SigilInvalidArgError",
    "SigilNeedsKeyError",
    "SigilNotFoundError",
    "SigilOOMError",
    "SigilResult",
    "SigilSaveMember",
    "SigilSaveUnit",
    "SigilUnknownPlatformError",
    "SigilUnsupportedFormatError",
    "content_stem",
    "extract",
    "hash_saves",
    "layout_subdirs",
    "list_save_root",
    "load_header_key_from_prod_keys",
    "locate_saves",
    "platform_from_slug",
    "platform_to_slug",
    "version",
]

PLATFORM_AUTO: int = lib.SIGIL_PLATFORM_AUTO
FEATURE_RTC: int = lib.SIGIL_FEATURE_RTC


class SigilError(Exception):
    """Base error for sigil failures. `code` holds the C error code."""

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


_ERROR_CLASSES = {
    lib.SIGIL_ERR_INVALID_ARG: SigilInvalidArgError,
    lib.SIGIL_ERR_IO: SigilIOError,
    lib.SIGIL_ERR_UNKNOWN_PLATFORM: SigilUnknownPlatformError,
    lib.SIGIL_ERR_UNSUPPORTED_FORMAT: SigilUnsupportedFormatError,
    lib.SIGIL_ERR_NOT_FOUND: SigilNotFoundError,
    lib.SIGIL_ERR_NEEDS_KEY: SigilNeedsKeyError,
    lib.SIGIL_ERR_CRYPTO: SigilCryptoError,
    lib.SIGIL_ERR_OOM: SigilOOMError,
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

_SUBDIR_LIST_DEPTH = 3
_SUBDIR_CAP = 16


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

    @property
    def has_rtc(self) -> bool:
        """The cart carries a real-time clock; a libretro frontend persists it as ``<stem>.rtc``."""
        return bool(self.features & lib.SIGIL_FEATURE_RTC)

    @classmethod
    def persisted(cls, platform: str, title_id: str, save_id: str, features: int) -> SigilResult:
        """A result rebuilt from stored columns, or built for a platform that has no title id."""
        return cls(
            title_id=title_id,
            raw_serial="",
            save_id=save_id,
            platform=platform,
            source="binary",
            usage="folder-exact",
            experimental=False,
            switch_content_type="unknown",
            title_version=0,
            features=features,
        )


@dataclass(frozen=True)
class SigilSaveMember:
    """One file of a save unit. `path` is relative to the save root; `entry` is its archive name."""

    path: str
    entry: str
    role: SaveRole
    present: bool


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
    filename_fallback: bool = False,
    allow_3ds_homebrew: bool = False,
) -> SigilResult:
    """Extract the title ID from a ROM file.

    `platform` is a slug ("ps2", "switch", ...); "auto" sniffs from the
    file extension. Filename fallback is OFF by default, inverting the C
    default: only binary-derived facts unless explicitly opted in.
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
    result.struct_version = lib.SIGIL_RESULT_V3

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
    )


def content_stem(content_path: str) -> str:
    """The base name RetroArch names save files after; see README, "Save units"."""
    out = ffi.new("char[]", lib.SIGIL_SAVE_ENTRY_MAX)
    lib.sigil_content_stem(content_path.encode("utf-8"), out, lib.SIGIL_SAVE_ENTRY_MAX)
    return ffi.string(out).decode("utf-8", "replace")


def layout_subdirs(layout: str) -> list[str]:
    """Subfolders under the save root a layout writes into, so the caller knows what to list."""
    out = ffi.new("const char *[]", _SUBDIR_CAP)
    n = lib.sigil_save_layout_subdirs(layout.encode("utf-8"), out, _SUBDIR_CAP)
    return [ffi.string(out[i]).decode("utf-8") for i in range(n)]


def list_save_root(root: str | os.PathLike[str], layout: str) -> list[str]:
    """Root-relative paths of the files directly in `root` plus those under the layout's subfolders."""
    root = os.fspath(root)
    out: list[str] = []
    if os.path.isdir(root):
        with os.scandir(root) as it:
            out.extend(e.name for e in it if e.is_file())
    for subdir in layout_subdirs(layout):
        _list_recursive(os.path.join(root, subdir), subdir, _SUBDIR_LIST_DEPTH, out)
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


def locate_saves(
    game: SigilResult,
    core: str,
    content_path: str,
    *,
    save_root: str | os.PathLike[str] | None = None,
    listing: Iterable[str] | None = None,
    options: Mapping[str, str] | None = None,
) -> SigilSaveUnit:
    """The files under a save root that belong to `game` when `core` runs `content_path`.

    Names only; no file is read. docs/python.md defines every input.
    """
    keepalive: list[object] = []

    def c_str(value: str):
        buf = ffi.new("char[]", value.encode("utf-8"))
        keepalive.append(buf)
        return buf

    if listing is None:
        listing = list_save_root(save_root, core) if save_root is not None else []
    paths = [c_str(p) for p in listing]
    option_items = list((options or {}).items())

    result = ffi.new("sigil_result *")
    result.struct_version = lib.SIGIL_RESULT_V3
    result.title_id = game.title_id.encode("utf-8")
    result.save_id = game.save_id.encode("utf-8")
    result.features = game.features
    keepalive.append(result)

    req = ffi.new("sigil_save_request *")
    req.struct_version = lib.SIGIL_SAVE_REQUEST_V1
    req.layout = c_str(core)
    req.platform = c_str(game.platform) if game.platform else ffi.NULL
    req.content_path = c_str(content_path)
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

    out = ffi.new("sigil_save_unit **")
    rc = lib.sigil_save_resolve(req, out)
    if rc != lib.SIGIL_OK:
        _raise_error(rc)
    unit = out[0]
    try:
        return SigilSaveUnit(
            key=_text(unit.key),
            shape=_SHAPE_NAMES.get(unit.shape, "none"),
            members=tuple(_member(unit.members[i]) for i in range(unit.member_count)),
            expected=tuple(_member(unit.expected[i]) for i in range(unit.expected_count)),
            unkeyed=tuple(_text(unit.unkeyed[i]) for i in range(unit.unkeyed_count)),
            artifact=_text(unit.artifact),
            content_hash="",
            identity_hash="",
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
        members[i].path = m.path.encode("utf-8")
        members[i].entry = m.entry.encode("utf-8")
        members[i].role = role_codes[m.role]
        members[i].present = 1

    unit = ffi.new("sigil_save_unit *")
    unit.struct_version = lib.SIGIL_SAVE_UNIT_V1
    unit.key = saves.key.encode("utf-8")
    unit.shape = shape_code
    unit.members = members
    unit.member_count = len(saves.members)

    root_bytes = os.fsencode(save_root)

    @ffi.callback("sigil_io *(void *, const char *)")
    def open_member(_ctx, relative_path):
        return lib.sigil_io_open_file(os.path.join(root_bytes, ffi.string(relative_path)))

    rc = lib.sigil_save_hash(unit, open_member, ffi.NULL)
    if rc != lib.SIGIL_OK:
        _raise_error(rc)
    return replace(saves, content_hash=_text(unit.content_hash), identity_hash=_text(unit.identity_hash))


def _text(chars) -> str:
    return ffi.string(chars).decode("utf-8", "replace")


def _member(m) -> SigilSaveMember:
    return SigilSaveMember(
        path=_text(m.path),
        entry=_text(m.entry),
        role=_ROLE_NAMES.get(m.role, "sidecar"),
        present=bool(m.present),
    )
