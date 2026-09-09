# Go

`import sigil "github.com/rommforge/argosy-sigil/bindings/go"` (cgo;
`make build` compiles the static libs it links). Failures return a
sentinel per C code (`sigil.ErrNotFound`, `sigil.ErrNeedsKey`,
`sigil.ErrIO`, ...) for `errors.Is`.

## 1. Identify the game

Once, at import. Skip when the result is already stored.

```go
sigil.Extract(
    path string,          // required. Rom file.
    platform Platform,    // required. sigil.PlatformPSP etc., or sigil.PlatformFromSlug(slug).
                          //   sigil.PlatformAuto sniffs the extension; a bare .iso or .zip needs one.
    opts *Options,        // optional, nil for defaults.
) (*Result, error)        // Error when nothing identified the file.

type Options struct {
    SwitchProdKeysPath      string   // optional. Switch prod.keys file.
    SwitchProdKeysBlob      []byte   // optional. Switch prod.keys contents. Wins over the path.
    SwitchHeaderKey         []byte   // optional. Switch header key, 32 bytes. Wins over both.
    DisableFilenameFallback bool     // optional. Fallback is on unless set: the file name is scanned
                                     //   when the binary gives nothing; Source reports which happened.
    Allow3DSHomebrew        bool     // optional.
}
```

```go
type Result struct {
    TitleID           string             // "" on gb, gbc, snes.
    RawSerial         string             // As found in the binary.
    SaveID            string             // On-disk name the emulator keys the save by. "" on gb, gbc, snes.
    Platform          Platform
    PlatformSlug      string
    Source            Source             // SourceBinary, SourceFilename.
    Usage             Usage              // UsageFolderExact, UsageFolderPrefix, UsageFileExact,
                                         //   UsageFilePrefix, UsageFolderSplit. README "usage" says how
                                         //   to apply SaveID for each.
    Experimental      bool
    SwitchContentType SwitchContentType  // SwitchContentUnknown, Application, Patch, Addon.
    TitleVersion      uint32             // Switch only.
    Features          uint32             // Bit set. FeatureRTC: cart has a clock. HasRTC() reads it.
}
```

Store `TitleID`, `SaveID`, `PlatformSlug`, `Features`. Rebuild later, or
build for a platform sigil cannot extract (Sega CD returns an error).
Every field is passed every time; a new struct comes back, nothing is
kept between calls:

```go
sigil.PersistedResult(
    platformSlug string,   // required. Slug from the README platform table, or segacd, fds.
    titleID string,        // required. Stored TitleID, or "" when the platform has none.
    saveID string,         // required. Stored SaveID, or "" when the platform has none.
    features uint32,       // required. Stored Features, or 0.
) *Result
```

## 2. Locate the saves

At sync time. No file is read.

```go
sigil.LocateSaves(
    game *Result,          // required. Step 1.
    core string,           // required. Libretro core name without _libretro: genesis_plus_gx,
                           //   mednafen_psx_hw, mame2003_plus. A core without a README layout row
                           //   gets the default row (<stem>.srm, plus <stem>.rtc when the cart has
                           //   a clock).
    contentPath string,    // required. The path you handed the emulator, verbatim: rom, .m3u, .cue,
                           //   .chd, or archive.zip#member.ext when a member was loaded. Only the
                           //   file name part is used.
    opts *LocateOptions,   // optional, nil for none.
) (*SaveUnit, error)       // Hashes empty.

type LocateOptions struct {
    SaveRoot string             // optional. Directory the emulator writes this game's save into.
                                //   RetroArch: savefile_directory, plus the core-named subfolder when
                                //   sort_savefiles_enable is on. Sigil lists it and the subfolders the
                                //   core writes into.
    Listing  []string           // optional. Instead of SaveRoot: every file directly in the root plus
                                //   every file under LayoutSubdirs(core), three levels deep, as
                                //   root-relative / paths. ListSaveRoot builds this.
    Options  map[string]string  // optional. Core option key to value, the strings the core defines
                                //   and RetroArch writes to <core>.opt, never display labels:
                                //   {"genesis_plus_gx_system_bram": "per game"}. Pass all of them;
                                //   only the keys the row names are read.
}
```

```go
type SaveUnit struct {
    Key          string        // Stem, or SaveID for folder layouts.
    Shape        SaveShape     // SaveShapeSingle, SaveShapeMulti, SaveShapeFolder. SaveShapeNone
                               //   when nothing is there.
    Members      []SaveMember  // Files present.
    Expected     []SaveMember  // Files the core should have written but has not: every applicable
                               //   primary, plus the rtc file when the cart has a clock.
    Unkeyed      []string      // Root files shared by every game. Never bundle them.
    Artifact     string        // File name the upload travels under.
    ContentHash  string        // "" until step 3.
    IdentityHash string        // "" until step 3.
}

type SaveMember struct {
    Path    string    // Root-relative.
    Entry   string    // Archive entry name.
    Role    SaveRole  // SaveRolePrimary, SaveRoleSidecar, SaveRoleRTC.
    Present bool
}
```

## 3. Hash the saves

When you need to compare with the server.

```go
sigil.HashSaves(
    unit *SaveUnit,     // required. Step 2. Filled in place.
    saveRoot string,    // required. Directory its paths are relative to.
) error                 // sigil.ErrIO when a member cannot be opened.
```

```go
unit.ContentHash   // What the RomM server computes for the artifact.
unit.IdentityHash  // The same over the non-rtc members. Different ContentHash, same
                   //   IdentityHash: a clock tick, not a new save.
```

## Upload and restore

Upload by `Shape`. `Single` sends the member as is. `Multi` zips the
members flat, each under its `Entry`. `Folder` zips the `Key` folder so
entries read `<key>/<file>`. Name the upload `Artifact`. Hash rules and
the layout table: README, "Save units".

Restore by `Path`. Unzip a `Multi` artifact so every entry lands at its
member's `Path` under the root. Unzip a `Folder` artifact from the
root's parent of the key folder. `Expected` says where a primary goes
when the emulator has not created one yet.

## Helpers

```go
sigil.ContentStem(contentPath string) string             // Stem the save is named after.
sigil.LayoutSubdirs(core string) []string                // Subfolders the core writes into.
sigil.ListSaveRoot(root, core string) ([]string, error)
sigil.PlatformFromSlug(slug string) Platform             // PlatformAuto when unknown.
func (p Platform) Slug() string
sigil.LoadHeaderKeyFromProdKeys(path string) ([]byte, error)   // 32 bytes.
sigil.Version() string
```
