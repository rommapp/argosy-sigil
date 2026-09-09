# SPDX-License-Identifier: MPL-2.0
"""Tests for the Python binding. Requires the compiled extension (make build)."""

import hashlib
import struct

import pytest

import sigil


def test_version_is_string():
    v = sigil.version()
    assert isinstance(v, str)
    assert v


def test_platform_slug_roundtrip():
    for slug in ("ps2", "psp", "switch", "xbox360"):
        p = sigil.platform_from_slug(slug)
        assert p != sigil.PLATFORM_AUTO
        assert sigil.platform_to_slug(p) == slug


def test_unknown_slug_maps_to_auto():
    p = sigil.platform_from_slug("commodore64")
    assert p == sigil.PLATFORM_AUTO
    assert sigil.platform_to_slug(p) == "auto"


def test_nonexistent_path_raises_io_error(tmp_path):
    missing = tmp_path / "no-such-file.iso"
    with pytest.raises(sigil.SigilIOError) as excinfo:
        sigil.extract(missing, platform="ps2")
    assert excinfo.value.code < 0


def _write_xex_fixture(path):
    # Minimal XEX2: one optional header (exec info) pointing at 0x100,
    # title ID bytes at 0x10C. Mirrors tests/unit_xex.c.
    buf = bytearray(1024)
    buf[0:4] = b"XEX2"
    buf[0x14:0x18] = struct.pack(">I", 1)
    buf[0x18:0x1C] = struct.pack(">I", 0x00040006)
    buf[0x1C:0x20] = struct.pack(">I", 0x100)
    buf[0x10C:0x110] = bytes([0x41, 0x4D, 0x07, 0xD1])
    path.write_bytes(bytes(buf))


def test_xex_extraction(tmp_path):
    xex = tmp_path / "default.xex"
    _write_xex_fixture(xex)

    result = sigil.extract(xex, platform="xbox360")
    assert result.title_id == "414D07D1"
    assert result.platform == "xbox360"
    assert result.source == "binary"
    assert result.experimental is True


def _romm_zip_hash(entries):
    lines = "\n".join(
        f"{name}:{hashlib.md5(data).hexdigest()}" for name, data in sorted(entries.items())
    )
    return hashlib.md5(lines.encode("utf-8")).hexdigest()


def test_content_stem_uses_the_archive_member():
    assert sigil.content_stem("roms/Game (USA).zip#Game (USA).gbc") == "Game (USA)"
    assert sigil.content_stem("roms/Game (USA).gbc") == "Game (USA)"


def test_layout_subdirs_names_the_core_folders():
    assert sigil.layout_subdirs("fbneo") == ["fbneo"]
    assert sigil.layout_subdirs("gambatte") == []


GB = sigil.SigilResult.persisted("gb", "", "", 0)
GB_RTC = sigil.SigilResult.persisted("gbc", "", "", sigil.FEATURE_RTC)


def test_persisted_result_carries_what_locate_needs():
    stored = sigil.SigilResult.persisted("psp", title_id="ULUS10064", save_id="ULUS10064", features=0)
    assert (stored.platform, stored.title_id, stored.save_id) == ("psp", "ULUS10064", "ULUS10064")
    assert stored.has_rtc is False
    assert GB_RTC.has_rtc is True


def test_locate_reads_no_files_and_hash_fills_the_hashes(tmp_path):
    srm = b"\x01\x02\x03" * 64
    (tmp_path / "game.srm").write_bytes(srm)

    located = sigil.locate_saves(GB, "gambatte", "game.gbc", save_root=tmp_path)
    assert located.shape == "single"
    assert located.key == "game"
    assert located.artifact == "game.srm"
    assert [m.path for m in located.members] == ["game.srm"]
    assert located.expected == ()
    assert (located.content_hash, located.identity_hash) == ("", "")

    hashed = sigil.hash_saves(located, tmp_path)
    assert hashed.content_hash == hashlib.md5(srm).hexdigest()
    assert hashed.identity_hash == hashed.content_hash
    assert hashed.members == located.members


def test_rtc_cart_bundles_the_clock_and_keeps_identity_over_the_save(tmp_path):
    srm = b"s" * 32768
    rtc = b"\x07" * 48
    (tmp_path / "game.srm").write_bytes(srm)
    (tmp_path / "game.rtc").write_bytes(rtc)

    saves = sigil.hash_saves(sigil.locate_saves(GB_RTC, "gambatte", "game.gbc", save_root=tmp_path), tmp_path)

    assert saves.shape == "multi"
    assert saves.artifact == "game.srm.zip"
    assert {m.entry: m.role for m in saves.members} == {"game.srm": "primary", "game.rtc": "rtc"}
    assert saves.content_hash == _romm_zip_hash({"game.srm": srm, "game.rtc": rtc})
    assert saves.identity_hash == hashlib.md5(srm).hexdigest()


def test_a_clock_tick_changes_content_but_not_identity(tmp_path):
    srm = b"s" * 512
    (tmp_path / "game.srm").write_bytes(srm)
    (tmp_path / "game.rtc").write_bytes(b"\x01" * 48)
    before = sigil.hash_saves(sigil.locate_saves(GB_RTC, "gambatte", "game.gbc", save_root=tmp_path), tmp_path)

    (tmp_path / "game.rtc").write_bytes(b"\x02" * 48)
    after = sigil.hash_saves(sigil.locate_saves(GB_RTC, "gambatte", "game.gbc", save_root=tmp_path), tmp_path)

    assert before.content_hash != after.content_hash
    assert before.identity_hash == after.identity_hash


def test_missing_rtc_is_expected_only_when_the_cart_has_a_clock():
    plain = sigil.locate_saves(GB, "gambatte", "game.gbc", listing=["game.srm"])
    clocked = sigil.locate_saves(GB_RTC, "gambatte", "game.gbc", listing=["game.srm"])

    assert plain.expected == ()
    assert [m.path for m in clocked.expected] == ["game.rtc"]


def test_hash_of_a_missing_member_is_an_io_error(tmp_path):
    located = sigil.locate_saves(GB, "gambatte", "game.gbc", listing=["game.srm"])
    with pytest.raises(sigil.SigilIOError):
        sigil.hash_saves(located, tmp_path)


def test_option_gated_sidecar_and_shared_files():
    listing = ["game.srm", "game.brm", "scd_U.brm"]
    segacd = sigil.SigilResult.persisted("segacd", "", "", 0)
    scd = sigil.SigilResult.persisted("scd", "", "", 0)

    default = sigil.locate_saves(segacd, "genesis_plus_gx", "game.chd", listing=listing)
    per_game = sigil.locate_saves(
        scd, "genesis_plus_gx", "game.chd", listing=listing,
        options={"genesis_plus_gx_system_bram": "per game"},
    )

    assert [m.path for m in default.members] == ["game.srm"]
    assert default.unkeyed == ("scd_U.brm",)
    assert [m.path for m in per_game.members] == ["game.srm", "game.brm"]
    assert per_game.unkeyed == ()


def test_folder_layout_lists_and_hashes_the_subfolder(tmp_path):
    folder = tmp_path / "same_cdi" / "nvram" / "ULUS10064DATA00"
    folder.mkdir(parents=True)
    (folder / "PARAM.SFO").write_bytes(b"sfo")
    (folder / "DATA.BIN").write_bytes(b"data")
    cdi = sigil.SigilResult.persisted("cdi", "", "", 0)

    saves = sigil.hash_saves(sigil.locate_saves(cdi, "same_cdi", "ULUS10064DATA00.chd", save_root=tmp_path), tmp_path)

    assert saves.shape == "folder"
    assert saves.artifact == "ULUS10064DATA00.zip"
    assert sorted(m.entry for m in saves.members) == ["ULUS10064DATA00/DATA.BIN", "ULUS10064DATA00/PARAM.SFO"]
    assert saves.content_hash == _romm_zip_hash(
        {"ULUS10064DATA00/DATA.BIN": b"data", "ULUS10064DATA00/PARAM.SFO": b"sfo"}
    )
