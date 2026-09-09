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

	if paths := memberPaths(def.Members); !reflect.DeepEqual(paths, []string{"game.srm"}) {
		t.Errorf("default members = %v", paths)
	}
	if !reflect.DeepEqual(def.Unkeyed, []string{"scd_U.brm"}) {
		t.Errorf("default unkeyed = %v", def.Unkeyed)
	}
	if paths := memberPaths(perGame.Members); !reflect.DeepEqual(paths, []string{"game.srm", "game.brm"}) {
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
