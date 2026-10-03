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
                                //   every file under LayoutSubdirs(core), four levels deep, as
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

## Memory cards

List the saves on a memory card or backup RAM volume: PS1 cards (the raw
card as `.mcr`, `.mcd` or `.srm`, DexDrive `.gme`, PSP or Vita `.vmp`),
PS2 `.ps2` file cards, GameCube raw cards, Dreamcast VMUs, and Saturn and
Sega CD backup RAM.

```go
sigil.ListCard(
    path string,        // required. The card file. Its format is detected from the content.
) (*CardListing, error) // sigil.ErrUnsupportedFormat when the file is not a card sigil reads.

type CardListing struct {
    Format       CardFormat  // CardFormatPS1Raw, CardFormatPS1GME, CardFormatPS1VMP, CardFormatPS2,
                             //   CardFormatGameCubeRaw, CardFormatDreamcastVMU, CardFormatSaturnBackup,
                             //   CardFormatSegaCDBRAM.
    TotalBlocks  uint32
    FreeBlocks   uint32      // Blocks a new save can use.
    FreeSlots    uint32      // Directory slots a new save can use.
    CorruptCount uint32      // Saves left out because their block chain is broken.
    Entries      []CardEntry // Live saves, in directory order.
    CorruptEntries []CardEntry // The left-out saves the card still names; Blocks is 0.
}

type CardEntry struct {
    Name       string // As stored on the card, e.g. "BASLUSP01041USCHRO00".
    OwnerID    string // The game id the save carries, as Extract reports it: PS1 and PS2 "SLUS-01041",
                      //   GameCube "47465A45". "" when the format has none.
    Blocks     uint32 // In the card's own block size.
    FirstBlock uint32
}
```

## Sync

`Collect` gathers one game's saves into the unit that travels to RomM;
`Restore` puts a unit back and reads it back, removing files where a save
folder holds a save the unit lacks. PS1 and PS2 memory cards, PCSX2 folder
cards, GameCube cards and Dolphin's GCI folder, Saturn and Sega CD backup RAM, and
Dreamcast VMUs work today. `Restore` returns
`sigil.ErrNotFound` for a unit holding none of the game's saves, and
ignores other games' saves inside a unit. What a unit holds, how Saturn and
Sega CD saves find their owner, and how genesis_plus_gx's region file is
picked: [c.md](c.md), "Sync". For Saturn and Sega CD, build the game with
`PersistedResult("saturn", "", "", 0)` or `("segacd", ...)`.

```go
sigil.Collect(game *Result, core, contentPath, saveRoot string, opts *SyncOptions) (*SyncResult, error)
sigil.Restore(unit []byte, game *Result, core, contentPath, saveRoot string, opts *SyncOptions) (*SyncResult, error)
    // Each of these writes nothing: sigil.ErrConflict (the saves under saveRoot changed since
    //   the last sync), sigil.ErrUncollected (a shared volume holds saves no collect has passed
    //   on yet), and as a *sigil.ProblemError, which matches its error with errors.Is:
    //   sigil.ErrNoSpace (BlocksShort says by how much), sigil.ErrRegion (a companion's save
    //   from another region), sigil.ErrNoTarget (the unit holds a volume the emulator's
    //   settings keep no file for), sigil.ErrAmbiguous (more than one file could be the
    //   emulator's card) and sigil.ErrDamaged (unless Repair rebuilt it; a card sigil can't read
    //   stays ErrDamaged). Problem names the save, member or files. Collect returns ErrDamaged
    //   and ErrAmbiguous the same way. c.md, "Sync", has the table.

type ProblemError struct {
    Err         error   // sigil.ErrNoSpace, ErrRegion, ErrNoTarget, ErrAmbiguous or ErrDamaged.
    Problem     string  // The save, unit member or file at fault; for ErrAmbiguous the files, one per line.
    BlocksShort uint32  // ErrNoSpace: blocks the save lacked; 0 when a directory slot ran out.
}

type Companion struct {
    GameIDs []string // The companion's ids, as for GameIDs.
    Unit    []byte   // Restore: its unit from RomM, or nil to leave its saves as they are.
}

type CompanionResult struct {
    Data         []byte // The companion's unit. nil when none of its saves are there.
    ContentHash  string
    IdentityHash string
    Changed      bool   // IdentityHash differs from the companion's last sync.
}

type SyncOptions struct {
    Listing        []string          // Root-relative paths; nil lists saveRoot.
    Options        map[string]string // The core's current option values.
    GameIDs        []string          // Every id the game's saves may carry: all discs of a set.
    State          []byte            // What the last call returned for this platform and emulator.
    Unmanaged      bool              // The game runs outside the caller.
    OverwriteLocal bool              // Restore: the user chose to replace saves that changed locally.
    Repair         bool              // Rebuild what ErrDamaged named, where sigil can.
    Claimed        []string          // Saturn, Sega CD: names from Unowned the user gave this game.
    Companions     []Companion       // Games whose saves this game reads, in the order they go on.
}

type SyncResult struct {
    Artifact     string    // File name the unit travels under.
    Shape        SaveShape
    Data         []byte    // Collect: the unit. nil when the game has no saves.
    ContentHash  string    // RomM content_hash of the unit.
    IdentityHash string    // Over the saves themselves; placement and timestamps don't move it.
    Changed      bool      // IdentityHash differs from the last sync.
    Conflict     bool
    State        []byte    // Store it once every upload succeeded; pass it back next time.
    Holding      []byte    // Saturn, Sega CD: zip of the saves on a shared volume with no known
                           //   owner. Upload it with the unit.
    Unowned      []string  // The names of the saves in Holding, for the user to claim.
    RestoreAgain bool      // Unmanaged: the saves the last Restore wrote were overwritten.
                           //   Restore again instead of uploading.
    Companions   []CompanionResult // Collect: one per SyncOptions.Companions, in order.
}
```

A companion's saves go on the game's card beside the game's own and stay
out of the game's unit; [c.md](c.md), "Sync", has the rules.

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
