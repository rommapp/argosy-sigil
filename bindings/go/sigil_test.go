// SPDX-License-Identifier: MPL-2.0

package sigil

import (
	"bytes"
	"crypto/md5"
	"encoding/hex"
	"errors"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strings"
	"testing"
)

func TestVersion(t *testing.T) {
	if Version() == "" {
		t.Fatal("Version() returned empty string")
	}
}

func TestPlatformSlugRoundtrip(t *testing.T) {
	cases := []struct {
		p    Platform
		slug string
	}{
		{PlatformPSP, "psp"},
		{PlatformPSX, "psx"},
		{PlatformPS2, "ps2"},
		{PlatformPSVita, "psvita"},
		{PlatformSwitch, "switch"},
		{Platform3DS, "3ds"},
		{PlatformWii, "wii"},
		{PlatformWiiU, "wiiu"},
		{PlatformGameCube, "gamecube"},
		{PlatformPS3, "ps3"},
		{PlatformXbox360, "xbox360"},
		{PlatformDreamcast, "dreamcast"},
		{PlatformXbox, "xbox"},
		{PlatformGB, "gb"},
		{PlatformGBC, "gbc"},
		{PlatformSNES, "snes"},
	}
	for _, c := range cases {
		if got := c.p.Slug(); got != c.slug {
			t.Errorf("%v.Slug() = %q, want %q", c.p, got, c.slug)
		}
		if got := PlatformFromSlug(c.slug); got != c.p {
			t.Errorf("PlatformFromSlug(%q) = %v, want %v", c.slug, got, c.p)
		}
	}
}

func TestUnknownSlug(t *testing.T) {
	if got := PlatformFromSlug("plystation"); got != PlatformAuto {
		t.Errorf("got %v, want PlatformAuto", got)
	}
}

type cardSave struct {
	name   string
	blocks int
}

// ps1Card builds a raw PS1 card holding saves laid out in order.
func ps1Card(saves []cardSave) []byte {
	card := make([]byte, 128*1024)
	copy(card, "MC")
	for frame := 1; frame < 16; frame++ {
		card[frame*128] = 0xA0
		card[frame*128+8], card[frame*128+9] = 0xFF, 0xFF
	}
	block := 1
	for _, s := range saves {
		for i := 0; i < s.blocks; i++ {
			f := (block + i) * 128
			state, link := byte(0x52), uint16(block+i)
			if i == 0 {
				state = 0x51
				copy(card[f+10:], s.name)
			} else if i == s.blocks-1 {
				state = 0x53
			}
			if i == s.blocks-1 {
				link = 0xFFFF
			}
			card[f] = state
			card[f+8], card[f+9] = byte(link), byte(link>>8)
		}
		block += s.blocks
	}
	return card
}

func TestListCardReportsSavesOwnersAndSpace(t *testing.T) {
	path := filepath.Join(t.TempDir(), "card.mcr")
	if err := os.WriteFile(path, ps1Card([]cardSave{{"BASLUSP01041CROSS", 2}, {"OPTIONS0000", 1}}), 0o644); err != nil {
		t.Fatal(err)
	}
	listing, err := ListCard(path)
	if err != nil {
		t.Fatal(err)
	}
	if listing.Format != CardFormatPS1Raw || listing.TotalBlocks != 15 || listing.FreeSlots != 12 || listing.CorruptCount != 0 {
		t.Fatalf("listing = %+v", listing)
	}
	want := []CardEntry{
		{Name: "BASLUSP01041CROSS", OwnerID: "SLUS-01041", Blocks: 2, FirstBlock: 1},
		{Name: "OPTIONS0000", OwnerID: "", Blocks: 1, FirstBlock: 3},
	}
	if !reflect.DeepEqual(listing.Entries, want) {
		t.Fatalf("entries = %+v, want %+v", listing.Entries, want)
	}
}

func TestListCardRejectsAFileThatIsNotACard(t *testing.T) {
	path := filepath.Join(t.TempDir(), "save.mcs")
	if err := os.WriteFile(path, append([]byte("Q\x00\x00\x00"), make([]byte, 8316)...), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := ListCard(path); !errors.Is(err, ErrUnsupportedFormat) {
		t.Fatalf("err = %v, want ErrUnsupportedFormat", err)
	}
}

var chronoCross = PersistedResult("psx", "SLUS-01041", "SLUS-01041", 0)

func TestCollectGathersOnlyTheGamesSaves(t *testing.T) {
	root := t.TempDir()
	card := ps1Card([]cardSave{{"BASLUSP01041CROSS", 2}, {"BASCUS-94426SLOTS", 1}})
	if err := os.WriteFile(filepath.Join(root, "Chrono Cross.srm"), card, 0o644); err != nil {
		t.Fatal(err)
	}
	result, err := Collect(chronoCross, "pcsx_rearmed", "Chrono Cross.cue", root, nil)
	if err != nil {
		t.Fatal(err)
	}
	if result.Artifact != "Chrono Cross.srm" || result.Shape != SaveShapeSingle || !result.Changed || len(result.State) == 0 {
		t.Fatalf("result = %+v", result)
	}
	unit := filepath.Join(t.TempDir(), "unit.srm")
	if err := os.WriteFile(unit, result.Data, 0o644); err != nil {
		t.Fatal(err)
	}
	listing, err := ListCard(unit)
	if err != nil || len(listing.Entries) != 1 || listing.Entries[0].Name != "BASLUSP01041CROSS" {
		t.Fatalf("unit entries = %+v, %v", listing, err)
	}
}

func TestRestoreWritesTheUnitAndRefusesOverUnsyncedChanges(t *testing.T) {
	source, target := t.TempDir(), t.TempDir()
	if err := os.WriteFile(filepath.Join(source, "Chrono Cross.srm"), ps1Card([]cardSave{{"BASLUSP01041CROSS", 2}}), 0o644); err != nil {
		t.Fatal(err)
	}
	unit, err := Collect(chronoCross, "pcsx_rearmed", "Chrono Cross.cue", source, nil)
	if err != nil {
		t.Fatal(err)
	}
	restored, err := Restore(unit.Data, chronoCross, "pcsx_rearmed", "Chrono Cross.cue", target, nil)
	if err != nil || restored.IdentityHash != unit.IdentityHash {
		t.Fatalf("restore = %+v, %v", restored, err)
	}
	other := ps1Card([]cardSave{{"BASLUSP01041OTHER", 1}})
	if err := os.WriteFile(filepath.Join(target, "Chrono Cross.srm"), other, 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := Restore(unit.Data, chronoCross, "pcsx_rearmed", "Chrono Cross.cue", target, nil); !errors.Is(err, ErrConflict) {
		t.Fatalf("err = %v, want ErrConflict", err)
	}
	forced, err := Restore(unit.Data, chronoCross, "pcsx_rearmed", "Chrono Cross.cue", target, &SyncOptions{OverwriteLocal: true})
	if err != nil || forced.IdentityHash != unit.IdentityHash {
		t.Fatalf("forced restore = %+v, %v", forced, err)
	}
}

func TestSegaCDSharedVolumeHoldsUnclaimedSavesBack(t *testing.T) {
	sample, err := os.ReadFile(filepath.Join("..", "..", "tests", "fixtures", "saves", "segacd", "files", "multi-titles-brm", "Multiple titles.brm"))
	if err != nil {
		t.Skip("Sega CD save samples missing")
	}
	root := t.TempDir()
	if err := os.WriteFile(filepath.Join(root, "scd_U.brm"), sample, 0o644); err != nil {
		t.Fatal(err)
	}
	lunar := PersistedResult("segacd", "", "", 0)
	held, err := Collect(lunar, "genesis_plus_gx", "Lunar (USA).cue", root, nil)
	if err != nil || held.Data != nil || !bytes.HasPrefix(held.Holding, []byte("PK\x03\x04")) {
		t.Fatalf("collect = %+v, %v", held, err)
	}
	found := false
	for _, name := range held.Unowned {
		found = found || name == "SFCD_DAT_09"
	}
	if !found {
		t.Fatalf("unowned = %v, want SFCD_DAT_09 among them", held.Unowned)
	}
	claimed, err := Collect(lunar, "genesis_plus_gx", "Lunar (USA).cue", root, &SyncOptions{Claimed: []string{"SFCD_DAT_09"}})
	if err != nil || claimed.Data == nil || claimed.Artifact != "backup.ram" || len(claimed.Unowned) != len(held.Unowned)-1 {
		t.Fatalf("claimed collect = %+v, %v", claimed, err)
	}
	if _, err := Restore(claimed.Data, lunar, "genesis_plus_gx", "Lunar (USA).cue", root, nil); !errors.Is(err, ErrUncollected) {
		t.Fatalf("err = %v, want ErrUncollected", err)
	}
}

func TestASaveNameWithRawBytesCanBeClaimed(t *testing.T) {
	volume, err := os.ReadFile(filepath.Join("..", "..", "tests", "fixtures", "saves", "saturn", "files", "hyper-duel-bkr", "Hyper Duel (Japan).bkr"))
	if err != nil {
		t.Skip("Saturn save samples missing")
	}
	at := bytes.Index(volume, []byte("HYPERDUEL_0"))
	if at < 0 {
		t.Fatal("sample has no HYPERDUEL_0")
	}
	volume[at+9] = 0xB1
	root := t.TempDir()
	if err := os.WriteFile(filepath.Join(root, "mednafen_saturn_libretro_shared.bkr"), volume, 0o644); err != nil {
		t.Fatal(err)
	}
	game := PersistedResult("saturn", "", "", 0)
	shared := map[string]string{"beetle_saturn_save_method": "mednafen", "beetle_saturn_shared_int": "enabled"}
	held, err := Collect(game, "mednafen_saturn", "Hyper Duel (Japan).cue", root, &SyncOptions{Options: shared})
	if err != nil || len(held.Unowned) != 1 {
		t.Fatalf("collect = %+v, %v", held, err)
	}
	claimed, err := Collect(game, "mednafen_saturn", "Hyper Duel (Japan).cue", root, &SyncOptions{Options: shared, Claimed: held.Unowned})
	if err != nil || claimed.Data == nil || len(claimed.Unowned) != 0 {
		t.Fatalf("claimed collect = %+v, %v", claimed, err)
	}
}

func TestGameCubeFolderRestoreRemovesSavesTheUnitLacks(t *testing.T) {
	samples := filepath.Join("..", "..", "tests", "fixtures", "saves", "ngc", "files", "fzero-gx-dolphin-gci-set")
	files, err := filepath.Glob(filepath.Join(samples, "*.gci"))
	if err != nil || len(files) == 0 {
		t.Skip("GameCube save samples missing")
	}
	root := t.TempDir()
	cardA := filepath.Join(root, "User", "GC", "USA", "Card A")
	if err := os.MkdirAll(cardA, 0o755); err != nil {
		t.Fatal(err)
	}
	copyIn := func(name string) {
		data, err := os.ReadFile(filepath.Join(samples, name))
		if err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(cardA, name), data, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	for _, f := range files {
		if filepath.Base(f) != "8P-GFZE-fzc.dat.gci" {
			copyIn(filepath.Base(f))
		}
	}
	fzero := PersistedResult("gamecube", "47465A45", "GFZE", 0)
	smaller, err := Collect(fzero, "dolphin", "F-Zero GX (USA).rvz", root, nil)
	if err != nil || smaller.Shape != SaveShapeMulti {
		t.Fatalf("collect = %+v, %v", smaller, err)
	}
	copyIn("8P-GFZE-fzc.dat.gci")
	if _, err := Restore(smaller.Data, fzero, "dolphin", "F-Zero GX (USA).rvz", root, &SyncOptions{OverwriteLocal: true}); err != nil {
		t.Fatal(err)
	}
	left, _ := filepath.Glob(filepath.Join(cardA, "*.gci"))
	if len(left) != 4 {
		t.Fatalf("files left = %v, want the unit's four", left)
	}
}

// copyAceFolder copies the Ace Combat 04 save folder sample into dst.
func copyAceFolder(t *testing.T, dst string) {
	t.Helper()
	samples := filepath.Join("..", "..", "tests", "fixtures", "saves", "ps2", "files", "ace-combat-04-aethersx2", "BASLUS-20152AC04")
	files, err := os.ReadDir(samples)
	if err != nil || len(files) == 0 {
		t.Skip("PS2 save samples missing")
	}
	if err := os.MkdirAll(dst, 0o755); err != nil {
		t.Fatal(err)
	}
	for _, f := range files {
		data, err := os.ReadFile(filepath.Join(samples, f.Name()))
		if err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(dst, f.Name()), data, 0o644); err != nil {
			t.Fatal(err)
		}
	}
}

func TestPCSX2FolderRestoreRemovesADroppedFolderAndItsDirectory(t *testing.T) {
	source := t.TempDir()
	copyAceFolder(t, filepath.Join(source, "memcards", "Mcd001.ps2", "BASLUS-20152AC04"))
	ace := PersistedResult("ps2", "SLUS-20152", "SLUS-20152", 0)
	unit, err := Collect(ace, "pcsx2_standalone", "Ace Combat 04 (USA).iso", source, nil)
	if err != nil || unit.Data == nil {
		t.Fatalf("collect = %+v, %v", unit, err)
	}
	target := t.TempDir()
	card := filepath.Join(target, "memcards", "Mcd001.ps2")
	dropped := filepath.Join(card, "BASLUS-20152XX")
	copyAceFolder(t, dropped)
	if err := os.WriteFile(filepath.Join(card, "_pcsx2_superblock"), nil, 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := Restore(unit.Data, ace, "pcsx2_standalone", "Ace Combat 04 (USA).iso", target,
		&SyncOptions{OverwriteLocal: true, Repair: true}); err != nil {
		t.Fatalf("restore: %v", err)
	}
	if _, err := os.Stat(dropped); !os.IsNotExist(err) {
		t.Fatalf("the dropped folder's directory is still there: %v", err)
	}
}

func TestPCSX2FolderCardSyncsThroughTheDefaultListing(t *testing.T) {
	samples := filepath.Join("..", "..", "tests", "fixtures", "saves", "ps2", "files", "ace-combat-04-aethersx2", "BASLUS-20152AC04")
	files, err := os.ReadDir(samples)
	if err != nil || len(files) == 0 {
		t.Skip("PS2 save samples missing")
	}
	source := t.TempDir()
	folder := filepath.Join(source, "memcards", "Mcd001.ps2", "BASLUS-20152AC04")
	if err := os.MkdirAll(folder, 0o755); err != nil {
		t.Fatal(err)
	}
	for _, f := range files {
		data, err := os.ReadFile(filepath.Join(samples, f.Name()))
		if err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(folder, f.Name()), data, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	ace := PersistedResult("ps2", "SLUS-20152", "SLUS-20152", 0)
	unit, err := Collect(ace, "pcsx2_standalone", "Ace Combat 04 (USA).iso", source, nil)
	if err != nil || unit.Data == nil || unit.Artifact != "Ace Combat 04 (USA).ps2" {
		t.Fatalf("collect = %+v, %v", unit, err)
	}

	target := t.TempDir()
	card := filepath.Join(target, "memcards", "Mcd001.ps2")
	if err := os.MkdirAll(card, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(card, "_pcsx2_superblock"), nil, 0o644); err != nil {
		t.Fatal(err)
	}
	restored, err := Restore(unit.Data, ace, "pcsx2_standalone", "Ace Combat 04 (USA).iso", target, nil)
	if err != nil {
		t.Fatalf("restore = %+v, %v", restored, err)
	}
	if info, err := os.Stat(filepath.Join(card, "_pcsx2_superblock")); err != nil || info.Size() != 0x2000 {
		t.Fatalf("superblock = %v, %v; want 0x2000 bytes", info, err)
	}
	if _, err := os.Stat(filepath.Join(card, "BASLUS-20152AC04", "_pcsx2_index")); err != nil {
		t.Fatalf("index: %v", err)
	}
	back, err := Collect(ace, "pcsx2_standalone", "Ace Combat 04 (USA).iso", target, &SyncOptions{State: restored.State})
	if err != nil || back.IdentityHash != unit.IdentityHash || back.Changed {
		t.Fatalf("collect back = %+v, %v; want the unit's saves, unchanged", back, err)
	}

	meta := filepath.Join(card, "BASLUS-20152AC04", "_pcsx2_meta")
	if err := os.MkdirAll(meta, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(meta, "icon.sys"), make([]byte, 512), 0o644); err != nil {
		t.Fatal(err)
	}
	listed, err := ListSaveRoot(target, "pcsx2_standalone")
	if err != nil {
		t.Fatal(err)
	}
	want := "memcards/Mcd001.ps2/BASLUS-20152AC04/_pcsx2_meta/icon.sys"
	found := false
	for _, p := range listed {
		found = found || p == want
	}
	if !found {
		t.Fatalf("listing %v lacks %s", listed, want)
	}
}

func TestCompanionSavesGoOnTheCardAndComeBackAsTheirUnit(t *testing.T) {
	prequel := PersistedResult("psx", "SLUS-00453", "SLUS-00453", 0)
	sequel := PersistedResult("psx", "SLUS-01334", "SLUS-01334", 0)
	src, target, full := t.TempDir(), t.TempDir(), t.TempDir()
	write := func(dir, name string, data []byte) {
		if err := os.WriteFile(filepath.Join(dir, name), data, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	write(src, "Prequel.srm", ps1Card([]cardSave{{"BASLUS-00453LEGENDS", 1}}))
	write(src, "Sequel.srm", ps1Card([]cardSave{{"BASLUS-01334LEGENDS2", 1}}))
	first, err := Collect(prequel, "pcsx_rearmed", "Prequel.cue", src, nil)
	if err != nil {
		t.Fatal(err)
	}
	second, err := Collect(sequel, "pcsx_rearmed", "Sequel.cue", src, nil)
	if err != nil {
		t.Fatal(err)
	}
	restored, err := Restore(second.Data, sequel, "pcsx_rearmed", "Sequel.cue", target, &SyncOptions{
		Companions: []Companion{{GameIDs: []string{"SLUS-00453"}, Unit: first.Data}},
	})
	if err != nil {
		t.Fatal(err)
	}
	back, err := Collect(sequel, "pcsx_rearmed", "Sequel.cue", target, &SyncOptions{
		State:      restored.State,
		Companions: []Companion{{GameIDs: []string{"SLUS-00453"}}},
	})
	if err != nil || back.IdentityHash != second.IdentityHash || len(back.Companions) != 1 ||
		back.Companions[0].IdentityHash != first.IdentityHash || back.Companions[0].Changed {
		t.Fatalf("collect = %+v, %v", back, err)
	}

	write(full, "Sequel.srm", ps1Card([]cardSave{{"BASLUS-99999OTHER", 15}}))
	_, err = Restore(second.Data, sequel, "pcsx_rearmed", "Sequel.cue", full, &SyncOptions{OverwriteLocal: true})
	var overflow *ProblemError
	if !errors.Is(err, ErrNoSpace) || !errors.As(err, &overflow) || overflow.Problem != "BASLUS-01334LEGENDS2" || overflow.BlocksShort != 1 {
		t.Fatalf("err = %v, want an overflow naming the sequel's save", err)
	}
}

func md5hex(data []byte) string {
	sum := md5.Sum(data)
	return hex.EncodeToString(sum[:])
}

func rommZipHash(entries map[string][]byte) string {
	names := make([]string, 0, len(entries))
	for name := range entries {
		names = append(names, name)
	}
	sort.Strings(names)
	lines := make([]string, 0, len(names))
	for _, name := range names {
		lines = append(lines, name+":"+md5hex(entries[name]))
	}
	return md5hex([]byte(strings.Join(lines, "\n")))
}

func TestContentStemUsesTheArchiveMember(t *testing.T) {
	if got := ContentStem("roms/Game (USA).zip#Game (USA).gbc"); got != "Game (USA)" {
		t.Errorf("ContentStem = %q", got)
	}
	if got := ContentStem("roms/Game (USA).gbc"); got != "Game (USA)" {
		t.Errorf("ContentStem = %q", got)
	}
}

func TestLayoutSubdirsNamesTheCoreFolders(t *testing.T) {
	if got := LayoutSubdirs("fbneo"); !reflect.DeepEqual(got, []string{"fbneo"}) {
		t.Errorf("LayoutSubdirs(fbneo) = %v", got)
	}
	if got := LayoutSubdirs("gambatte"); len(got) != 0 {
		t.Errorf("LayoutSubdirs(gambatte) = %v", got)
	}
}

var (
	gb    = PersistedResult("gb", "", "", 0)
	gbRTC = PersistedResult("gbc", "", "", FeatureRTC)
)

func writeFiles(t *testing.T, root string, files map[string][]byte) {
	t.Helper()
	for name, data := range files {
		path := filepath.Join(root, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, data, 0o644); err != nil {
			t.Fatal(err)
		}
	}
}

func locateAndHash(t *testing.T, game *Result, core, content, root string) *SaveUnit {
	t.Helper()
	unit, err := LocateSaves(game, core, content, &LocateOptions{SaveRoot: root})
	if err != nil {
		t.Fatal(err)
	}
	if err := HashSaves(unit, root); err != nil {
		t.Fatal(err)
	}
	return unit
}

func TestPersistedResultCarriesWhatLocateNeeds(t *testing.T) {
	stored := PersistedResult("psp", "ULUS10064", "ULUS10064", 0)
	if stored.PlatformSlug != "psp" || stored.Platform != PlatformPSP || stored.TitleID != "ULUS10064" || stored.HasRTC() {
		t.Errorf("stored = %+v", stored)
	}
	if !gbRTC.HasRTC() {
		t.Error("gbRTC.HasRTC() = false")
	}
}

func TestLocateReadsNoFilesAndHashFillsTheHashes(t *testing.T) {
	root := t.TempDir()
	srm := bytes.Repeat([]byte{1, 2, 3}, 64)
	writeFiles(t, root, map[string][]byte{"game.srm": srm})

	located, err := LocateSaves(gb, "gambatte", "game.gbc", &LocateOptions{SaveRoot: root})
	if err != nil {
		t.Fatal(err)
	}
	if located.Shape != SaveShapeSingle || located.Key != "game" || located.Artifact != "game.srm" {
		t.Errorf("located = %+v", located)
	}
	if len(located.Expected) != 0 || located.ContentHash != "" || located.IdentityHash != "" {
		t.Errorf("located = %+v", located)
	}

	if err := HashSaves(located, root); err != nil {
		t.Fatal(err)
	}
	if located.ContentHash != md5hex(srm) || located.IdentityHash != located.ContentHash {
		t.Errorf("hashes = %s / %s", located.ContentHash, located.IdentityHash)
	}
}

func TestRTCCartBundlesTheClockAndKeepsIdentityOverTheSave(t *testing.T) {
	root := t.TempDir()
	srm := bytes.Repeat([]byte("s"), 32768)
	rtc := bytes.Repeat([]byte{7}, 48)
	writeFiles(t, root, map[string][]byte{"game.srm": srm, "game.rtc": rtc})

	unit := locateAndHash(t, gbRTC, "gambatte", "game.gbc", root)
	if unit.Shape != SaveShapeMulti || unit.Artifact != "game.srm.zip" {
		t.Errorf("unit = %+v", unit)
	}
	roles := map[string]SaveRole{}
	for _, m := range unit.Members {
		roles[m.Entry] = m.Role
	}
	if !reflect.DeepEqual(roles, map[string]SaveRole{"game.srm": SaveRolePrimary, "game.rtc": SaveRoleRTC}) {
		t.Errorf("roles = %v", roles)
	}
	if want := rommZipHash(map[string][]byte{"game.srm": srm, "game.rtc": rtc}); unit.ContentHash != want {
		t.Errorf("ContentHash = %s, want %s", unit.ContentHash, want)
	}
	if unit.IdentityHash != md5hex(srm) {
		t.Errorf("IdentityHash = %s", unit.IdentityHash)
	}
}

func TestAClockTickChangesContentButNotIdentity(t *testing.T) {
	root := t.TempDir()
	writeFiles(t, root, map[string][]byte{"game.srm": bytes.Repeat([]byte("s"), 512), "game.rtc": bytes.Repeat([]byte{1}, 48)})
	before := locateAndHash(t, gbRTC, "gambatte", "game.gbc", root)

	writeFiles(t, root, map[string][]byte{"game.rtc": bytes.Repeat([]byte{2}, 48)})
	after := locateAndHash(t, gbRTC, "gambatte", "game.gbc", root)

	if before.ContentHash == after.ContentHash {
		t.Error("ContentHash did not change with the clock")
	}
	if before.IdentityHash != after.IdentityHash {
		t.Errorf("IdentityHash changed: %s -> %s", before.IdentityHash, after.IdentityHash)
	}
}

func TestMissingRTCIsExpectedOnlyWhenTheCartHasAClock(t *testing.T) {
	plain, err := LocateSaves(gb, "gambatte", "game.gbc", &LocateOptions{Listing: []string{"game.srm"}})
	if err != nil {
		t.Fatal(err)
	}
	clocked, err := LocateSaves(gbRTC, "gambatte", "game.gbc", &LocateOptions{Listing: []string{"game.srm"}})
	if err != nil {
		t.Fatal(err)
	}
	if len(plain.Expected) != 0 {
		t.Errorf("plain expected = %+v", plain.Expected)
	}
	if len(clocked.Expected) != 1 || clocked.Expected[0].Path != "game.rtc" {
		t.Errorf("clocked expected = %+v", clocked.Expected)
	}
}

func TestHashOfAMissingMemberIsAnIOError(t *testing.T) {
	located, err := LocateSaves(gb, "gambatte", "game.gbc", &LocateOptions{Listing: []string{"game.srm"}})
	if err != nil {
		t.Fatal(err)
	}
	if err := HashSaves(located, t.TempDir()); !errors.Is(err, ErrIO) {
		t.Errorf("HashSaves = %v, want ErrIO", err)
	}
}

func TestFolderLayoutListsAndHashesTheSubfolder(t *testing.T) {
	root := t.TempDir()
	writeFiles(t, root, map[string][]byte{
		"same_cdi/nvram/ULUS10064DATA00/PARAM.SFO": []byte("sfo"),
		"same_cdi/nvram/ULUS10064DATA00/DATA.BIN":  []byte("data"),
	})

	unit := locateAndHash(t, PersistedResult("cdi", "", "", 0), "same_cdi", "ULUS10064DATA00.chd", root)
	if unit.Shape != SaveShapeFolder || unit.Artifact != "ULUS10064DATA00.zip" {
		t.Errorf("unit = %+v", unit)
	}
	entries := []string{}
	for _, m := range unit.Members {
		entries = append(entries, m.Entry)
	}
	sort.Strings(entries)
	if !reflect.DeepEqual(entries, []string{"ULUS10064DATA00/DATA.BIN", "ULUS10064DATA00/PARAM.SFO"}) {
		t.Errorf("entries = %v", entries)
	}
	want := rommZipHash(map[string][]byte{"ULUS10064DATA00/DATA.BIN": []byte("data"), "ULUS10064DATA00/PARAM.SFO": []byte("sfo")})
	if unit.ContentHash != want {
		t.Errorf("ContentHash = %s, want %s", unit.ContentHash, want)
	}
}

func TestOptionGatedSidecarAndSharedFiles(t *testing.T) {
	listing := []string{"game.srm", "game.brm", "scd_U.brm"}

	def, err := LocateSaves(PersistedResult("segacd", "", "", 0), "genesis_plus_gx", "game.chd", &LocateOptions{Listing: listing})
	if err != nil {
		t.Fatal(err)
	}
	perGame, err := LocateSaves(PersistedResult("scd", "", "", 0), "genesis_plus_gx", "game.chd", &LocateOptions{
		Listing: listing,
		Options: map[string]string{"genesis_plus_gx_system_bram": "per game"},
	})
	if err != nil {
		t.Fatal(err)
	}

	if paths := memberPaths(def.Members); len(paths) != 0 {
		t.Errorf("default members = %v", paths)
	}
	if !reflect.DeepEqual(def.Unkeyed, []string{"scd_U.brm"}) {
		t.Errorf("default unkeyed = %v", def.Unkeyed)
	}
	if paths := memberPaths(perGame.Members); !reflect.DeepEqual(paths, []string{"game.brm"}) {
		t.Errorf("per-game members = %v", paths)
	}
	if len(perGame.Unkeyed) != 0 {
		t.Errorf("per-game unkeyed = %v", perGame.Unkeyed)
	}
}

func memberPaths(members []SaveMember) []string {
	out := make([]string, 0, len(members))
	for _, m := range members {
		out = append(out, m.Path)
	}
	return out
}

func TestIntegration(t *testing.T) {
	dir := os.Getenv("SIGIL_ROM_DIR")
	if dir == "" {
		t.Skip("SIGIL_ROM_DIR not set")
	}

	cases := []struct {
		platform     Platform
		subdir       string
		exts         []string
		expectBinary bool // false for filename-only platforms (Vita)
	}{
		{PlatformPSP, "psp", []string{".chd", ".iso"}, true},
		{PlatformPSX, "psx", []string{".chd", ".bin", ".iso"}, true},
		{PlatformPS2, "ps2", []string{".chd", ".iso"}, true},
		{PlatformPSVita, "psvita", []string{".zip"}, false},
		{Platform3DS, "3ds", []string{".3ds", ".cci"}, true},
		{PlatformWii, "wii", []string{".rvz", ".iso"}, true},
		{PlatformGameCube, "ngc", []string{".rvz", ".iso"}, true},
		{PlatformWiiU, "wiiu", []string{".wua"}, true},
	}

	const limit = 5

	for _, c := range cases {
		t.Run(c.platform.Slug(), func(t *testing.T) {
			platDir := filepath.Join(dir, c.subdir)
			entries, err := os.ReadDir(platDir)
			if err != nil {
				t.Skipf("no %s samples: %v", c.platform, err)
			}

			seen := 0
			for _, e := range entries {
				if seen >= limit {
					break
				}
				if e.IsDir() || strings.HasPrefix(e.Name(), ".") {
					continue
				}
				ext := strings.ToLower(filepath.Ext(e.Name()))
				keep := false
				for _, want := range c.exts {
					if ext == want {
						keep = true
						break
					}
				}
				if !keep {
					continue
				}

				path := filepath.Join(platDir, e.Name())
				r, err := Extract(path, c.platform, nil)
				if err != nil {
					t.Errorf("%s: %v", e.Name(), err)
					continue
				}
				if r.TitleID == "" {
					t.Errorf("%s: empty TitleID", e.Name())
				}
				if c.expectBinary && r.Source != SourceBinary {
					t.Errorf("%s: source=%v, want binary", e.Name(), r.Source)
				}
				seen++
			}
			if seen == 0 {
				t.Skipf("no matching files in %s", platDir)
			}
		})
	}
}

func TestSwitchWithKeys(t *testing.T) {
	dir := os.Getenv("SIGIL_ROM_DIR")
	keys := os.Getenv("SIGIL_PROD_KEYS")
	if dir == "" || keys == "" {
		t.Skip("SIGIL_ROM_DIR or SIGIL_PROD_KEYS not set")
	}

	platDir := filepath.Join(dir, "switch")
	entries, err := os.ReadDir(platDir)
	if err != nil {
		t.Skipf("no Switch samples: %v", err)
	}

	opts := &Options{SwitchProdKeysPath: keys}

	const limit = 5
	seen := 0
	for _, e := range entries {
		if seen >= limit {
			break
		}
		if e.IsDir() || !strings.EqualFold(filepath.Ext(e.Name()), ".xci") {
			continue
		}
		path := filepath.Join(platDir, e.Name())
		r, err := Extract(path, PlatformSwitch, opts)
		if err != nil {
			t.Errorf("%s: %v", e.Name(), err)
			continue
		}
		if len(r.TitleID) != 16 || !strings.HasPrefix(r.TitleID, "01") {
			t.Errorf("%s: title_id=%q, want 16 hex starting 01", e.Name(), r.TitleID)
		}
		if r.Source != SourceBinary {
			t.Errorf("%s: source=%v, want binary", e.Name(), r.Source)
		}
		seen++
	}
	if seen == 0 {
		t.Skip("no Switch .xci samples")
	}
}
