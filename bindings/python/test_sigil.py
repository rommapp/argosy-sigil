# SPDX-License-Identifier: MPL-2.0
"""Tests for the Python binding. Requires the compiled extension (make build)."""

import hashlib
import struct
from pathlib import Path

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


def _ps1_card(saves):
    """A raw PS1 card: `saves` is a list of (directory name, block count), laid out in order."""
    card = bytearray(128 * 1024)
    card[0:2] = b"MC"
    for frame in range(1, 16):
        card[frame * 128] = 0xA0
        card[frame * 128 + 8 : frame * 128 + 10] = b"\xff\xff"
    block = 1
    for name, blocks in saves:
        for i in range(blocks):
            frame = (block + i) * 128
            state = 0x51 if i == 0 else (0x53 if i == blocks - 1 else 0x52)
            link = 0xFFFF if i == blocks - 1 else block + i  # next frame, zero-based
            card[frame : frame + 4] = struct.pack("<I", state)
            card[frame + 4 : frame + 8] = struct.pack("<I", blocks * 8192 if i == 0 else 0)
            card[frame + 8 : frame + 10] = struct.pack("<H", link)
            if i == 0:
                card[frame + 10 : frame + 10 + len(name)] = name.encode("ascii")
        block += blocks
    return bytes(card)


def test_list_card_reports_saves_owners_and_space(tmp_path):
    path = tmp_path / "card.mcr"
    path.write_bytes(_ps1_card([("BASLUSP01041CROSS", 2), ("OPTIONS0000", 1)]))
    listing = sigil.list_card(path)
    assert listing.format == "ps1-raw"
    assert listing.total_blocks == 15
    assert listing.free_slots == 12
    assert listing.corrupt_count == 0
    assert [(e.name, e.owner_id, e.blocks, e.first_block) for e in listing.entries] == [
        ("BASLUSP01041CROSS", "SLUS-01041", 2, 1),
        ("OPTIONS0000", "", 1, 3),
    ]


def test_list_card_rejects_a_file_that_is_not_a_card(tmp_path):
    path = tmp_path / "save.mcs"
    path.write_bytes(b"Q\0\0\0" + bytes(8316))
    with pytest.raises(sigil.SigilUnsupportedFormatError):
        sigil.list_card(path)


_CROSS = sigil.SigilResult.persisted("psx", "SLUS-01041", "SLUS-01041", 0)


def test_collect_gathers_only_the_games_saves(tmp_path):
    (tmp_path / "Chrono Cross.srm").write_bytes(_ps1_card([("BASLUSP01041CROSS", 2), ("BASCUS-94426SLOTS", 1)]))
    result = sigil.collect(_CROSS, "pcsx_rearmed", "Chrono Cross.cue", tmp_path)
    assert result.artifact == "Chrono Cross.srm"
    assert result.shape == "single"
    assert result.changed
    assert result.data is not None
    unit = tmp_path / "unit.srm"
    unit.write_bytes(result.data)
    assert [e.name for e in sigil.list_card(unit).entries] == ["BASLUSP01041CROSS"]


def test_restore_writes_the_unit_and_refuses_over_unsynced_changes(tmp_path):
    source = tmp_path / "source"
    target = tmp_path / "target"
    source.mkdir()
    target.mkdir()
    (source / "Chrono Cross.srm").write_bytes(_ps1_card([("BASLUSP01041CROSS", 2)]))
    unit = sigil.collect(_CROSS, "pcsx_rearmed", "Chrono Cross.cue", source)
    assert unit.data is not None

    restored = sigil.restore(unit.data, _CROSS, "pcsx_rearmed", "Chrono Cross.cue", target)
    assert restored.identity_hash == unit.identity_hash
    assert [e.name for e in sigil.list_card(target / "Chrono Cross.srm").entries] == ["BASLUSP01041CROSS"]

    (target / "Chrono Cross.srm").write_bytes(_ps1_card([("BASLUSP01041OTHER", 1)]))
    with pytest.raises(sigil.SigilConflictError):
        sigil.restore(unit.data, _CROSS, "pcsx_rearmed", "Chrono Cross.cue", target)
    forced = sigil.restore(unit.data, _CROSS, "pcsx_rearmed", "Chrono Cross.cue", target, overwrite_local=True)
    assert forced.identity_hash == unit.identity_hash


_MC01 = Path(__file__).resolve().parents[2] / "tests/fixtures/saves/ps2/files/mymc-mc01/mc01.ps2"
_REZ = sigil.SigilResult.persisted("ps2", "SCES-50501", "SCES-50501", 0)


@pytest.mark.skipif(not _MC01.exists(), reason="PS2 save samples missing")
def test_ps2_shared_card_round_trips_through_collect_and_restore(tmp_path):
    source = tmp_path / "source"
    target = tmp_path / "target"
    source.mkdir()
    target.mkdir()
    (source / "Mcd001.ps2").write_bytes(_MC01.read_bytes())
    unit = sigil.collect(_REZ, "pcsx2", "Rez.iso", source)
    assert unit.artifact == "Rez.ps2"
    assert unit.data is not None and len(unit.data) == 8650752
    card = tmp_path / "unit.ps2"
    card.write_bytes(unit.data)
    assert [e.name for e in sigil.list_card(card).entries] == ["BESCES-50501REZ"]

    restored = sigil.restore(unit.data, _REZ, "pcsx2", "Rez.iso", target)
    assert restored.identity_hash == unit.identity_hash
    assert [e.name for e in sigil.list_card(target / "Mcd001.ps2").entries] == ["BESCES-50501REZ"]


_MULTI_BRM = Path(__file__).resolve().parents[2] / "tests/fixtures/saves/segacd/files/multi-titles-brm/Multiple titles.brm"
_LUNAR = sigil.SigilResult.persisted("segacd", "", "", 0)


@pytest.mark.skipif(not _MULTI_BRM.exists(), reason="Sega CD save samples missing")
def test_segacd_shared_volume_holds_unclaimed_saves_back(tmp_path):
    (tmp_path / "scd_U.brm").write_bytes(_MULTI_BRM.read_bytes())
    held = sigil.collect(_LUNAR, "genesis_plus_gx", "Lunar (USA).cue", tmp_path)
    assert held.data is None
    assert held.holding is not None and held.holding[:4] == b"PK\x03\x04"
    assert "SFCD_DAT_09" in held.unowned

    claimed = sigil.collect(_LUNAR, "genesis_plus_gx", "Lunar (USA).cue", tmp_path, claimed=["SFCD_DAT_09"])
    assert claimed.artifact == "backup.ram"
    assert "SFCD_DAT_09" not in claimed.unowned
    assert claimed.data is not None
    unit = tmp_path / "unit.brm"
    unit.write_bytes(claimed.data)
    assert [e.name for e in sigil.list_card(unit).entries] == ["SFCD_DAT_09"]

    with pytest.raises(sigil.SigilUncollectedError):
        sigil.restore(claimed.data, _LUNAR, "genesis_plus_gx", "Lunar (USA).cue", tmp_path)


_HYPER_DUEL = Path(__file__).resolve().parents[2] / "tests/fixtures/saves/saturn/files/hyper-duel-bkr/Hyper Duel (Japan).bkr"


@pytest.mark.skipif(not _HYPER_DUEL.exists(), reason="Saturn save samples missing")
def test_a_save_name_with_raw_bytes_can_be_claimed(tmp_path):
    volume = bytearray(_HYPER_DUEL.read_bytes())
    at = volume.find(b"HYPERDUEL_0")
    assert at > 0
    volume[at + 9] = 0xB1
    (tmp_path / "mednafen_saturn_libretro_shared.bkr").write_bytes(bytes(volume))
    game = sigil.SigilResult.persisted("saturn", "", "", 0)
    shared = {"beetle_saturn_save_method": "mednafen", "beetle_saturn_shared_int": "enabled"}

    held = sigil.collect(game, "mednafen_saturn", "Hyper Duel (Japan).cue", tmp_path, options=shared)
    assert len(held.unowned) == 1
    claimed = sigil.collect(game, "mednafen_saturn", "Hyper Duel (Japan).cue", tmp_path, options=shared,
                            claimed=held.unowned)
    assert claimed.data is not None
    assert claimed.unowned == ()
    unit = tmp_path / "unit.bkr"
    unit.write_bytes(claimed.data)
    assert tuple(e.name for e in sigil.list_card(unit).entries) == held.unowned


_FZERO_DIR = Path(__file__).resolve().parents[2] / "tests/fixtures/saves/ngc/files/fzero-gx-dolphin-gci-set"
_FZERO = sigil.SigilResult.persisted("gamecube", "47465A45", "GFZE", 0)


@pytest.mark.skipif(not _FZERO_DIR.exists(), reason="GameCube save samples missing")
def test_gamecube_folder_restore_removes_saves_the_unit_lacks(tmp_path):
    card_a = tmp_path / "User" / "GC" / "USA" / "Card A"
    card_a.mkdir(parents=True)
    for gci in _FZERO_DIR.glob("*.gci"):
        if gci.name != "8P-GFZE-fzc.dat.gci":
            (card_a / gci.name).write_bytes(gci.read_bytes())
    smaller = sigil.collect(_FZERO, "dolphin", "F-Zero GX (USA).rvz", tmp_path)
    assert smaller.shape == "multi"

    (card_a / "8P-GFZE-fzc.dat.gci").write_bytes((_FZERO_DIR / "8P-GFZE-fzc.dat.gci").read_bytes())
    sigil.restore(smaller.data, _FZERO, "dolphin", "F-Zero GX (USA).rvz", tmp_path, overwrite_local=True)
    assert not (card_a / "8P-GFZE-fzc.dat.gci").exists()
    assert not (card_a / "8P-GFZE-f_zero.dat0.gci").exists()
    assert len(list(card_a.glob("*.gci"))) == 4


_ACE_DIR = Path(__file__).resolve().parents[2] / "tests/fixtures/saves/ps2/files/ace-combat-04-aethersx2/BASLUS-20152AC04"
_ACE = sigil.SigilResult.persisted("ps2", "SLUS-20152", "SLUS-20152", 0)


@pytest.mark.skipif(not _ACE_DIR.exists(), reason="PS2 save samples missing")
def test_pcsx2_folder_card_syncs_through_the_default_listing(tmp_path):
    source = tmp_path / "source" / "memcards" / "Mcd001.ps2" / "BASLUS-20152AC04"
    source.mkdir(parents=True)
    for f in _ACE_DIR.iterdir():
        (source / f.name).write_bytes(f.read_bytes())
    unit = sigil.collect(_ACE, "pcsx2_standalone", "Ace Combat 04 (USA).iso", tmp_path / "source")
    assert unit.artifact == "Ace Combat 04 (USA).ps2"
    assert unit.data is not None

    card = tmp_path / "target" / "memcards" / "Mcd001.ps2"
    card.mkdir(parents=True)
    (card / "_pcsx2_superblock").write_bytes(b"")
    restored = sigil.restore(unit.data, _ACE, "pcsx2_standalone", "Ace Combat 04 (USA).iso", tmp_path / "target")
    assert restored.identity_hash == unit.identity_hash
    assert (card / "_pcsx2_superblock").stat().st_size == 0x2000
    assert (card / "BASLUS-20152AC04" / "_pcsx2_index").exists()

    meta = card / "BASLUS-20152AC04" / "_pcsx2_meta"
    meta.mkdir()
    (meta / "icon.sys").write_bytes(b"\0" * 512)
    listed = sigil.list_save_root(tmp_path / "target", "pcsx2_standalone")
    assert "memcards/Mcd001.ps2/BASLUS-20152AC04/_pcsx2_meta/icon.sys" in listed


def test_companion_saves_go_on_the_card_and_come_back_as_their_unit(tmp_path):
    prequel = sigil.SigilResult.persisted("psx", "SLUS-00453", "SLUS-00453", 0)
    sequel = sigil.SigilResult.persisted("psx", "SLUS-01334", "SLUS-01334", 0)
    src = tmp_path / "src"
    src.mkdir()
    (src / "Prequel.srm").write_bytes(_ps1_card([("BASLUS-00453LEGENDS", 1)]))
    (src / "Sequel.srm").write_bytes(_ps1_card([("BASLUS-01334LEGENDS2", 1)]))
    first = sigil.collect(prequel, "pcsx_rearmed", "Prequel.cue", src)
    second = sigil.collect(sequel, "pcsx_rearmed", "Sequel.cue", src)

    target = tmp_path / "target"
    target.mkdir()
    companion = sigil.SigilCompanion(game_ids=("SLUS-00453",), unit=first.data)
    restored = sigil.restore(second.data, sequel, "pcsx_rearmed", "Sequel.cue", target, companions=[companion])
    names = sorted(e.name for e in sigil.list_card(target / "Sequel.srm").entries)
    assert names == ["BASLUS-00453LEGENDS", "BASLUS-01334LEGENDS2"]

    back = sigil.collect(sequel, "pcsx_rearmed", "Sequel.cue", target, state=restored.state,
                         companions=[sigil.SigilCompanion(game_ids=("SLUS-00453",))])
    assert back.identity_hash == second.identity_hash
    assert len(back.companions) == 1
    assert back.companions[0].identity_hash == first.identity_hash
    assert not back.companions[0].changed

    full = tmp_path / "full"
    full.mkdir()
    (full / "Sequel.srm").write_bytes(_ps1_card([("BASLUS-99999OTHER", 15)]))
    with pytest.raises(sigil.SigilNoSpaceError) as excinfo:
        sigil.restore(second.data, sequel, "pcsx_rearmed", "Sequel.cue", full, overwrite_local=True)
    assert excinfo.value.overflow == "BASLUS-01334LEGENDS2"
    assert excinfo.value.overflow_blocks == 1


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

    assert [m.path for m in default.members] == []
    assert default.unkeyed == ("scd_U.brm",)
    assert [m.path for m in per_game.members] == ["game.brm"]
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
