# SPDX-License-Identifier: MPL-2.0
"""Pythonic facade over the sigil C library (title ID extraction)."""

from __future__ import annotations

import os
from dataclasses import dataclass
from typing import Literal

from sigil._sigil import ffi, lib

__all__ = [
    "PLATFORM_AUTO",
    "SigilCryptoError",
    "SigilError",
    "SigilIOError",
    "SigilInvalidArgError",
    "SigilNeedsKeyError",
    "SigilNotFoundError",
    "SigilOOMError",
    "SigilResult",
    "SigilUnknownPlatformError",
    "SigilUnsupportedFormatError",
    "extract",
    "load_header_key_from_prod_keys",
    "platform_from_slug",
    "platform_to_slug",
    "version",
]

PLATFORM_AUTO: int = lib.SIGIL_PLATFORM_AUTO


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
