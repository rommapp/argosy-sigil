// SPDX-License-Identifier: MPL-2.0

package sigil

import (
	"bytes"
	"crypto/md5"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strings"
	"testing"
)

func TestListSaveRootFollowsLinks(t *testing.T) {
	elsewhere := t.TempDir()
	if err := os.WriteFile(filepath.Join(elsewhere, "mslug.fs"), []byte("fs"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(elsewhere, "game.srm"), []byte("srm"), 0o644); err != nil {
		t.Fatal(err)
	}
	deeper := t.TempDir()
	if err := os.WriteFile(filepath.Join(deeper, "kof98.fs"), []byte("fs"), 0o644); err != nil {
		t.Fatal(err)
	}
	root := t.TempDir()
	links := map[string]string{
		filepath.Join(root, "fbneo"):      elsewhere,
		filepath.Join(root, "linked.srm"): filepath.Join(elsewhere, "game.srm"),
		filepath.Join(root, "gone.srm"):   filepath.Join(elsewhere, "missing.srm"),
		filepath.Join(elsewhere, "more"):  deeper,
	}
	for link, target := range links {
		if err := os.Symlink(target, link); err != nil {
			t.Skipf("no symlinks here: %v", err)
		}
	}
	listed, err := ListSaveRoot(root, "fbneo")
	if err != nil {
		t.Fatal(err)
	}
	sort.Strings(listed)
	want := []string{"fbneo/game.srm", "fbneo/more/kof98.fs", "fbneo/mslug.fs", "linked.srm"}
	if !reflect.DeepEqual(listed, want) {
		t.Errorf("listed = %v, want %v", listed, want)
	}
}

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

// brokenCrossCard holds Chrono Cross's 2-block save with its first block
// linking to a free one.
func brokenCrossCard() []byte {
	card := ps1Card([]cardSave{{"BASLUSP01041CROSS", 2}})
	card[128+8], card[128+9] = 4, 0
	return card
}

func TestListCardNamesACorruptSave(t *testing.T) {
	path := filepath.Join(t.TempDir(), "card.mcr")
	if err := os.WriteFile(path, brokenCrossCard(), 0o644); err != nil {
		t.Fatal(err)
	}
	listing, err := ListCard(path)
	if err != nil || len(listing.Entries) != 0 || listing.CorruptCount != 1 || len(listing.CorruptEntries) != 1 ||
		listing.CorruptEntries[0].Name != "BASLUSP01041CROSS" || listing.CorruptEntries[0].OwnerID != "SLUS-01041" {
		t.Fatalf("listing = %+v, %v", listing, err)
	}
}

func TestABrokenSaveOfTheGameIsDamagedNamingTheCard(t *testing.T) {
	root := t.TempDir()
	if err := os.WriteFile(filepath.Join(root, "Chrono Cross.srm"), brokenCrossCard(), 0o644); err != nil {
		t.Fatal(err)
	}
	_, err := Collect(chronoCross, "pcsx_rearmed", "Chrono Cross.cue", root, &SyncOptions{Repair: true})
	var damaged *ProblemError
	if !errors.Is(err, ErrDamaged) || !errors.As(err, &damaged) || damaged.Problem != "Chrono Cross.srm" {
		t.Fatalf("err = %v, want ErrDamaged naming the card", err)
	}
}

func TestTwoDolphinCardSizesAreAmbiguousNamingBoth(t *testing.T) {
	raw, err := os.ReadFile(filepath.Join("..", "..", "tests", "fixtures", "saves", "ngc", "files", "card-raw-usa", "memcard-image.raw"))
	if err != nil {
		t.Skip("GameCube card sample missing")
	}
	root := t.TempDir()
	gc := filepath.Join(root, "User", "GC")
	if err := os.MkdirAll(gc, 0o755); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"MemoryCardA.USA.raw", "MemoryCardA.USA.59.raw"} {
		if err := os.WriteFile(filepath.Join(gc, name), raw, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	fzero := PersistedResult("gamecube", "47465A45", "GFZE", 0)
	_, err = Collect(fzero, "dolphin", "F-Zero GX (USA).rvz", root, &SyncOptions{Options: map[string]string{"SlotA": "1"}})
	var ambiguous *ProblemError
	if !errors.Is(err, ErrAmbiguous) || !errors.As(err, &ambiguous) {
		t.Fatalf("err = %v, want ErrAmbiguous", err)
	}
	lines := strings.Split(ambiguous.Problem, "\n")
	sort.Strings(lines)
	if strings.Join(lines, ",") != "User/GC/MemoryCardA.USA.59.raw,User/GC/MemoryCardA.USA.raw" {
		t.Fatalf("problem = %q", ambiguous.Problem)
	}
}

func TestAVMUTheSettingsKeepNoFileForIsNoTarget(t *testing.T) {
	samples := filepath.Join("..", "..", "tests", "fixtures", "saves", "dc", "files")
	a1, _ := filepath.Glob(filepath.Join(samples, "gundam-0079-flycast", "*.bin"))
	b1, err := os.ReadFile(filepath.Join(samples, "vmoooo-vmu", "vmoooo.bin"))
	if len(a1) == 0 || err != nil {
		t.Skip("Dreamcast samples missing")
	}
	a1Data, err := os.ReadFile(a1[0])
	if err != nil {
		t.Fatal(err)
	}
	source, target := t.TempDir(), t.TempDir()
	if os.WriteFile(filepath.Join(source, "T13301N.A1.bin"), a1Data, 0o644) != nil ||
		os.WriteFile(filepath.Join(source, "T13301N.B1.bin"), b1, 0o644) != nil {
		t.Fatal("setup failed")
	}
	gundam := PersistedResult("dc", "T13301N", "T13301N", 0)
	unit, err := Collect(gundam, "flycast", "Gundam.gdi", source,
		&SyncOptions{Options: map[string]string{"reicast_per_content_vmus": "All VMUs"}})
	if err != nil {
		t.Fatal(err)
	}
	_, err = Restore(unit.Data, gundam, "flycast", "Gundam.gdi", target,
		&SyncOptions{Options: map[string]string{"reicast_per_content_vmus": "VMU A1"}})
	var missing *ProblemError
	if !errors.Is(err, ErrNoTarget) || !errors.As(err, &missing) || missing.Problem != "vmu_B1.bin" {
		t.Fatalf("err = %v, want ErrNoTarget naming vmu_B1.bin", err)
	}
	if left, _ := os.ReadDir(target); len(left) != 0 {
		t.Fatalf("restore wrote %d files", len(left))
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
	if err != nil || forced.IdentityHash != unit.IdentityHash || forced.HardcoreMarker {
		t.Fatalf("forced restore = %+v, %v", forced, err)
	}
}

func TestRestoreLeavesOutAndReportsArgosysHardcoreMarker(t *testing.T) {
	source, plain, target := t.TempDir(), t.TempDir(), t.TempDir()
	if err := os.WriteFile(filepath.Join(source, "Chrono Cross.srm"), ps1Card([]cardSave{{"BASLUSP01041CROSS", 2}}), 0o644); err != nil {
		t.Fatal(err)
	}
	unit, err := Collect(chronoCross, "pcsx_rearmed", "Chrono Cross.cue", source, nil)
	if err != nil {
		t.Fatal(err)
	}
	json := []byte(`{"h":true,"v":1}`)
	marked := append(append([]byte{}, unit.Data...), json...)
	marked = binary.LittleEndian.AppendUint32(marked, uint32(len(json)))
	marked = append(marked, "ARGOSY\x01\x00"...)
	if _, err := Restore(unit.Data, chronoCross, "pcsx_rearmed", "Chrono Cross.cue", plain, nil); err != nil {
		t.Fatal(err)
	}
	restored, err := Restore(marked, chronoCross, "pcsx_rearmed", "Chrono Cross.cue", target, nil)
	if err != nil || !restored.HardcoreMarker {
		t.Fatalf("restore = %+v, %v", restored, err)
	}
	want, _ := os.ReadFile(filepath.Join(plain, "Chrono Cross.srm"))
	got, err := os.ReadFile(filepath.Join(target, "Chrono Cross.srm"))
	if err != nil || !bytes.Equal(got, want) {
		t.Fatalf("the card differs from the unmarked unit's: %v", err)
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

func TestASaveUnderAnotherModeIsReportedWithTheOptionsThatTakeIt(t *testing.T) {
	volume, err := os.ReadFile(filepath.Join("..", "..", "tests", "fixtures", "saves", "saturn", "files", "hyper-duel-bkr", "Hyper Duel (Japan).bkr"))
	if err != nil {
		t.Skip("Saturn save samples missing")
	}
	root := t.TempDir()
	if err := os.WriteFile(filepath.Join(root, "Hyper Duel (Japan).bkr"), volume, 0o644); err != nil {
		t.Fatal(err)
	}
	game := PersistedResult("saturn", "", "", 0)
	want := []SaveAlternate{{Path: "Hyper Duel (Japan).bkr", Options: map[string]string{"beetle_saturn_save_method": "mednafen"}}}

	modern, err := Collect(game, "mednafen_saturn", "Hyper Duel (Japan).cue", root, nil)
	if err != nil || modern.Data != nil || !reflect.DeepEqual(modern.Alternates, want) {
		t.Fatalf("modern collect = %+v, %v", modern, err)
	}
	located, err := LocateSaves(game, "mednafen_saturn", "Hyper Duel (Japan).cue", &LocateOptions{SaveRoot: root})
	if err != nil || !reflect.DeepEqual(located.Alternates, want) {
		t.Fatalf("locate = %+v, %v", located, err)
	}
	legacy, err := Collect(game, "mednafen_saturn", "Hyper Duel (Japan).cue", root, &SyncOptions{Options: modern.Alternates[0].Options})
	if err != nil || legacy.Data == nil || len(legacy.Alternates) != 0 {
		t.Fatalf("legacy collect = %+v, %v", legacy, err)
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

func TestRawSerialNamesPcsxSerialCards(t *testing.T) {
	mgs := PersistedResult("psx", "SLUS-00594", "SLUS-00594", 0)
	mgs.RawSerial = "slus_005.94"
	located, err := LocateSaves(mgs, "pcsx_rearmed", "Metal Gear Solid (USA) (Disc 1).cue", &LocateOptions{
		Listing: []string{"slus-00594_1.mcd", "SLUS-00594_1.mcd"},
		Options: map[string]string{"pcsx_rearmed_memcard1": "serial"},
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(located.Members) != 1 || located.Members[0].Path != "slus-00594_1.mcd" {
		t.Errorf("members = %+v", located.Members)
	}
}

func n64Rom() []byte {
	rom := make([]byte, 0x1000)
	copy(rom, []byte{0x80, 0x37, 0x12, 0x40})
	copy(rom[0x20:], "1080 SNOWBOARDING   ")
	copy(rom[0x3B:], "NTEA")
	return rom
}

func TestExtractReadsTheN64FieldsStandaloneEmulatorsNameSavesBy(t *testing.T) {
	rom := n64Rom()
	path := filepath.Join(t.TempDir(), "1080.z64")
	if err := os.WriteFile(path, rom, 0o644); err != nil {
		t.Fatal(err)
	}
	n64Order := make([]byte, len(rom))
	for i := 0; i < len(rom); i += 4 {
		n64Order[i], n64Order[i+1], n64Order[i+2], n64Order[i+3] = rom[i+3], rom[i+2], rom[i+1], rom[i]
	}
	r, err := Extract(path, PlatformAuto, nil)
	if err != nil {
		t.Fatal(err)
	}
	if r.TitleID != "NTEA" || r.N64Header != "1080 SNOWBOARDING" || r.N64MD5 != strings.ToUpper(md5hex(rom)) ||
		r.N64MD5N64 != strings.ToUpper(md5hex(n64Order)) {
		t.Errorf("result = %+v", r)
	}
}

func TestAStoredN64ResultFindsTheStandaloneSaves(t *testing.T) {
	game := PersistedResult("n64", "NTEA", "NTEA", 0)
	game.N64Header = "1080 SNOWBOARDING"
	game.N64MD5 = "FA27089C425DBAB99F19245C5C997613"
	game.N64MD5N64 = "10C93DD78B695CD32B6938534ED0EDD5"
	listing := []string{
		"1080 Snowboarding (JU) [!]-FA27089C.eep", "Other-12345678.eep",
		"Save/1080 SNOWBOARDING-10C93DD78B695CD32B6938534ED0EDD5/1080 SNOWBOARDING.eep",
	}
	for core, want := range map[string]string{"mupen64plus_standalone": listing[0], "project64": listing[2]} {
		located, err := LocateSaves(game, core, "1080.z64", &LocateOptions{Listing: listing})
		if err != nil {
			t.Fatal(err)
		}
		if len(located.Members) != 1 || located.Members[0].Path != want {
			t.Errorf("%s members = %+v", core, located.Members)
		}
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

const (
	edenUser    = "125D2DBAEBDEB11000296E1E1ECBF401"
	citronUser  = "735DA01FA7EAE565C8FAA61E710195E5"
	avatorsDir  = "nand/system/save/8000000000000010/su/avators"
	switchSaves = "nand/user/save/0000000000000000"
)

var botw = PersistedResult("switch", "01007EF00011E000", "01007EF00011E000", 0)

func switchSamples() string {
	return filepath.Join("..", "..", "tests", "fixtures", "saves", "switch", "files")
}

// emulator lays out an emulator folder: the sample's profile list, and Breath
// of the Wild under botwUser when it isn't "".
func emulator(t *testing.T, base, profiles, botwUser string) {
	t.Helper()
	list, err := os.ReadFile(filepath.Join(switchSamples(), profiles, "profiles.dat"))
	if err != nil {
		t.Skip("Switch profile samples missing")
	}
	if os.MkdirAll(filepath.Join(base, avatorsDir), 0o755) != nil ||
		os.WriteFile(filepath.Join(base, avatorsDir, "profiles.dat"), list, 0o644) != nil {
		t.Fatal("setup failed")
	}
	if botwUser == "" {
		return
	}
	source := filepath.Join(switchSamples(), "botw-eden")
	err = filepath.WalkDir(source, func(path string, d os.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return err
		}
		rel, _ := filepath.Rel(source, path)
		target := filepath.Join(base, switchSaves, botwUser, rel)
		data, err := os.ReadFile(path)
		if err == nil {
			err = os.MkdirAll(filepath.Dir(target), 0o755)
		}
		if err == nil {
			err = os.WriteFile(target, data, 0o644)
		}
		return err
	})
	if err != nil {
		t.Fatal(err)
	}
}

func TestARootAtAnyDepthCollectsTheSameSaveWithoutTheProfile(t *testing.T) {
	top := t.TempDir()
	files := filepath.Join(top, "Android", "data", "dev.eden.eden_emulator", "files")
	emulator(t, files, "eden-profile", edenUser)

	var results []*SyncResult
	for _, root := range []string{files, top, filepath.Join(files, switchSaves, edenUser)} {
		r, err := Collect(botw, "eden", "botw.nsp", root, nil)
		if err != nil {
			t.Fatalf("%s: %v", root, err)
		}
		results = append(results, r)
	}
	for _, r := range results {
		if r.Profile != edenUser || r.IdentityHash != results[0].IdentityHash {
			t.Fatalf("profile %q identity %s, want %s and %s", r.Profile, r.IdentityHash, edenUser, results[0].IdentityHash)
		}
	}
	if !reflect.DeepEqual(results[0].Profiles, []Profile{{edenUser, "Eden"}}) || results[0].Artifact != "01007EF00011E000.zip" {
		t.Fatalf("profiles %v artifact %s", results[0].Profiles, results[0].Artifact)
	}
	if base, profile, err := SaveBase("eden", filepath.Join(files, switchSaves, edenUser)); err != nil || base != files ||
		profile != edenUser {
		t.Fatalf("SaveBase = %q, %q, %v", base, profile, err)
	}

	located, err := LocateSaves(botw, "eden", "botw.nsp", &LocateOptions{SaveRoot: top})
	if err != nil || located.Shape != SaveShapeFolder || located.ContentHash != results[0].ContentHash {
		t.Fatalf("LocateSaves = %+v, %v", located, err)
	}
	for _, m := range located.Members {
		if m.Area != SaveAreaAccount || !strings.HasPrefix(m.Entry, "01007EF00011E000/") {
			t.Fatalf("member %+v", m)
		}
	}
}

func TestRestorePutsTheSaveUnderTheTargetsProfile(t *testing.T) {
	eden, citron := t.TempDir(), t.TempDir()
	emulator(t, eden, "eden-profile", edenUser)
	emulator(t, citron, "citron-profile", "")
	unit, err := Collect(botw, "eden", "botw.nsp", eden, nil)
	if err != nil {
		t.Fatal(err)
	}
	written, err := Restore(unit.Data, botw, "citron", "botw.nsp", citron, nil)
	if err != nil || written.Profile != citronUser {
		t.Fatalf("Restore = %+v, %v", written, err)
	}
	want, _ := os.ReadFile(filepath.Join(eden, switchSaves, edenUser, "01007EF00011E000", "option.sav"))
	got, err := os.ReadFile(filepath.Join(citron, switchSaves, citronUser, "01007EF00011E000", "option.sav"))
	if err != nil || !bytes.Equal(got, want) {
		t.Fatalf("option.sav not under the target's profile: %v", err)
	}
}

func TestTwoProfilesAndNonePickedIsAmbiguousListingThem(t *testing.T) {
	root := t.TempDir()
	emulator(t, root, "eden-profile", edenUser)
	data, _ := os.ReadFile(filepath.Join(root, avatorsDir, "profiles.dat"))
	citron, err := os.ReadFile(filepath.Join(switchSamples(), "citron-profile", "profiles.dat"))
	if err != nil {
		t.Skip("Switch profile samples missing")
	}
	copy(data[0x10+0xC8:0x10+2*0xC8], citron[0x10:0x10+0xC8])
	if os.WriteFile(filepath.Join(root, avatorsDir, "profiles.dat"), data, 0o644) != nil {
		t.Fatal("setup failed")
	}

	_, err = Collect(botw, "eden", "botw.nsp", root, nil)
	var problem *ProblemError
	if !errors.Is(err, ErrAmbiguous) || !errors.As(err, &problem) ||
		problem.Problem != edenUser+" Eden\n"+citronUser+" citron" || len(problem.Profiles) != 2 {
		t.Fatalf("err = %v", err)
	}
	picked, err := Collect(botw, "eden", "botw.nsp", root, &SyncOptions{Profile: strings.ToLower(edenUser)})
	if err != nil || picked.Profile != edenUser {
		t.Fatalf("picked = %+v, %v", picked, err)
	}
	inside, err := Collect(botw, "eden", "botw.nsp", filepath.Join(root, switchSaves, edenUser), nil)
	if err != nil || inside.Profile != edenUser || inside.IdentityHash != picked.IdentityHash {
		t.Fatalf("inside = %+v, %v", inside, err)
	}
	listed, err := ListProfiles("eden", root)
	if err != nil || !reflect.DeepEqual(listed, []Profile{{edenUser, "Eden"}, {citronUser, "citron"}}) {
		t.Fatalf("ListProfiles = %v, %v", listed, err)
	}
	if _, err := ListProfiles("pcsx_rearmed", root); !errors.Is(err, ErrUnsupportedFormat) {
		t.Fatalf("ListProfiles without profiles = %v", err)
	}
}
