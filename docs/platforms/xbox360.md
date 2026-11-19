# Xbox 360

Status: synced
sigil reads the XEX title id and syncs a game's saves on Xenia Canary,
Xenia Edge and XenDroid with collect and restore, per profile.

## Identification

| Slug | Platform | Inputs | `title_id` example | `usage` | Status |
|---|---|---|---|---|---|
| `xbox360` | Xbox 360 | `.zar`, `.iso` (needs hint), extracted game folder or `.xex` | `4D5307DC` (4-byte XEX title_id, hex) | folder-exact | experimental |

See [Identification](../identification.md) for the fields.

### XEX title id

**Xbox 360 reads the XEX title id (experimental).** The 4-byte execution-info
title ID is returned as 8-char uppercase hex (`4D5307DC`), which is what
the console, Xenia and XenDroid all use, so `title_id` and `save_id` are
the same string. Optional header values are offsets from the XEX start
rather than from the file, so a XEX embedded in a disc image has its base
added back to each one.

Reachable four ways: a bare `default.xex`, an extracted game folder, a
disc image through the XDVDFS walker, or a `.zar`. GoD containers and
STFS packages are not implemented.

The XDVDFS walker is shared with the original Xbox; see [Xbox](xbox.md#xdvdfs).
A `.zar` is a ZArchive written by Xenia; see [Containers](../containers.md).

## Save layouts

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| `xenia_canary` | folders per profile: `content/{profile}/{save_id}/00000001/<save>/`, each save's header in `content/{profile}/{save_id}/Headers/00000001/` as the container header (0xA000 bytes); the same under the all-zero XUID for a save no profile owns | | emulator source, samples |
| `xenia_edge` | as `xenia_canary`, each header in either form, as `XCONTENT_AGGREGATE_DATA` (0x148 bytes) for a save Edge made | | emulator source, samples |
| `xendroid` | as `xenia_canary`, under the `content/` folder of XenDroid's storage root, each header as `XCONTENT_AGGREGATE_DATA` | | emulator source |

The rows list `content`, which `sigil_save_layout_subdirs()` names, and
match `xbox360` only. Xenia Canary, Xenia Edge and XenDroid keep the same
tree and differ in the form of a save's header ([save formats](#save-formats)).
Each keeps a game's saves per profile, and a save the game marks shared,
or makes with no one signed in, under the all-zero XUID:

| Layout | Account | Device | Profile list |
|---|---|---|---|
| `xenia_canary`, `xenia_edge`, `xendroid` | `content/{profile}/{save_id}/00000001/` and `content/{profile}/{save_id}/Headers/00000001/` | the same under `0000000000000000` | `content/{profile}/FFFE07D1/00010000/{profile}/Account`; ids only |

The base folder is the one holding `content/`, Xenia's storage root
unless `--content_root` moves it; a moved content root is still found when
its folder is named `content`. A profile id is the
profile's XUID as Xenia names its folder, 16 uppercase hex digits
(`E030000043414231`). How sigil picks the profile is in
[profiles](../save-units.md#profiles).

**Profiles are listed by id alone.** The profile's name, its gamertag, is
in the `Account` file, which is encrypted with a key of the console's
that sigil doesn't carry, so every profile's `name` is empty. A client
asks the user which XUID is theirs, or uses the one it knows.

The list holds the profiles the emulators sign in with. `FindProfiles`
takes each folder of `content/` named by 16 uppercase hex digits, other
than the all-zero XUID, that holds the profile package; `LoadAccount`
reads the `Account` in it, and `Login` signs in only a profile whose
`Account` loaded (`profile_manager.cc` in Edge at `4ad875a`, Canary at
`27d11a2` and XenDroid at `779680a`). So sigil lists each such folder
holding an `Account`, and a folder without one, or named otherwise, is no
profile. A `profile` the list doesn't hold is used as given when it has
that form. One that doesn't (lowercase, the wrong length, not hex, or the
all-zero XUID) names a folder no Xenia reads, and collect and restore
refuse it with `SIGIL_ERR_INVALID_ARG`, writing nothing.

## Save formats

Xenia keeps each save as an extracted folder,
`content/<XUID>/<title id>/00000001/<save name>/`, with its metadata in
`content/<XUID>/<title id>/Headers/00000001/<save name>.header`, in one of
two forms:

- **Edge and XenDroid** write `XCONTENT_AGGREGATE_DATA` alone, 0x148
  bytes: device id, content type, display name, file name, XUID and title
  id (`content_manager.cc` `WriteContentHeaderFile`, Edge at `4ad875a`,
  XenDroid at `779680a`; `content_manager.h`). XenDroid reads no other form
  (`ReadContentHeaderFile` there).
- **Canary** writes the whole container header with a file name of its
  own, padded to 0xA000 bytes (`content_manager.cc`
  `ExtractContentHeader` at `27d11a2`). For a save it makes, it fills it
  from the game: execution info, title name, the title's icon as
  thumbnail, and the signed-in XUID as `profile_id`
  (`xcontent/xcontent_package.cc` `InitializePackageHeader`). For a save
  whose header is Edge's, it builds one from that when the game closes the
  save: content type, title id, the English display name and the file
  name, every other byte zero (`xcontent/xcontent_package_directory.cc`
  `FillHeader`).

How each reads a header file, by its size and content:

- **Edge** (`content_manager.cc` `ReadContentHeaderFile` at `4ad875a`)
  reads one of at least 0x971A bytes whose magic is `CON `, `LIVE` or
  `PIRS` as the container (`vfs/devices/xcontent_container_device.cc`
  `ContentDataFromHeader`), and any other of at least 0x148 bytes by its
  first 0x148 as the aggregate. It lists a save whose header is shorter by
  its folder's name (`ListContent`).
- **XenDroid** (`content_manager.cc` `ReadContentHeaderFile` at `779680a`)
  reads the first 0x148 bytes of any header that long as the aggregate, and
  lists a save whose header is shorter by its folder's name.
- **Canary** (`xcontent/xcontent_package_directory.cc`
  `ReadContentHeaderFile` at `27d11a2`) reads one of at least 0x148 bytes
  and fewer than 0x971A as the aggregate, and one of exactly 0x971A or
  0xA000 bytes as the container without checking its magic; any other size
  it doesn't read. It lists a save only when its header has a valid magic
  and content type (`ListContent`, `xcontent/xcontent_package.cc`
  `IsValidPackage`).

Canary doesn't list a save whose
header is missing, so the header travels with the save. XenDroid uses
Canary's tree under its own storage root's `content/`. Copying the folder
and its header moves a save between Xenia installs; moving one to a
console needs the STFS package rebuilt and re-signed.

A profile is `content/<XUID>/FFFE07D1/00010000/<XUID>/`: the `Account`,
the dashboard's `FFFE07D1.gpd` and one `<title id>.gpd` per game played,
which holds the game's achievements and the settings some games keep
there. A unit carries none of them.

## Sync

On a layout with profiles the unit is a zip of the game's save folders,
and `companions` is `SIGIL_ERR_INVALID_ARG`. An Xbox 360 unit holds
`<title id>/00000001/...` and `<title id>/Headers/00000001/...` for the
profile's saves and `device/<title id>/...` for the all-zero XUID's,
never a XUID, so it restores under any profile. The general rules for
profile layouts are in [sync](../sync.md).

Restore also reads Xenia's `content/` folder zipped as it is, as clients
uploaded before sigil: `<XUID>/<title id>/00000001/...`,
`<XUID>/<title id>/Headers/00000001/...` and
`0000000000000000/<title id>/...`. It writes those saves as it writes
sigil's own unit, under the profile the request picks, and leaves out the
profile folders and gamer pictures such a zip holds (each
`<XUID>/FFFE07D1/`).

**Headers.** The unit carries each header as its emulator wrote it, and
restore writes what the target writes for that save:

- **Canary** gets the container. From Edge's aggregate it is built the way
  Canary's `FillHeader` builds it, which Canary does itself when the game
  closes the save; from Edge's header of Forza Horizon 2 this is byte for
  byte the header Canary wrote from it (`forza-horizon-2-xenia-canary`). A
  container comes as it is, except that under another profile its
  `profile_id` names that profile: Canary hands `profile_id` to the game as
  the save's XUID (`xcontent/xcontent_package.cc` `GetContentMetadata`,
  through `xam_content_aggregate.cc`), opens content in that XUID's folder
  when it isn't 0 (`content_manager.cc` `ResolvePackagePath`), and writes
  the creating profile's for a save it makes (`InitializePackageHeader`).
- **Edge** gets the header as the unit holds it, in either form: it reads
  both, and writes a header only for a save it creates (`xam_content.cc`
  `xeXamContentCreate` at `4ad875a`). It never hands a header's XUID to a
  game: its enumerators copy each save as `XCONTENT_DATA`, or a cross-title
  entry without the XUID (`xenumerator.h` `ContentEnumerator`,
  `xam_content_aggregate.cc`), so a header naming another XUID is found
  under the profile it sits in.
- **XenDroid** gets the aggregate, the one form it reads. From a container
  it is built the way Edge's `ContentDataFromHeader` reads one: device id
  1, the content type, the English display name (else the first one there),
  the save's file name, the title id, and XUID 0. Against headers Edge
  wrote for the same saves it differs only in padding Edge leaves unset,
  which restore writes as 0. An aggregate comes as it is; XenDroid's
  enumerators, like Edge's, never hand its XUID to a game (`xenumerator.h`,
  `xam_content_aggregate.cc` at `779680a`).

A header in neither form, which Edge and XenDroid read by size alone and
Canary doesn't read, goes as it is to every Xenia, the way collect carries
it. `identity_hash` covers what both forms carry, the content type, display
name, file name and title id, so a save is the same save in either form;
for a header in neither form it covers the bytes. `content_hash` covers the
unit's bytes, so it differs with the emulator that collected the save.

**The maker.** `maker_profile` is the XUID the account saves' headers name
as the one they were made under: for a save Canary made, its `profile_id`;
for a save Edge or XenDroid made, the aggregate's XUID, which
`WriteContentHeaderFile` fills from the signed-in profile only when the
game passes an aggregate whose XUID is -1 (`content_manager.cc` and
`xam_content.cc` at `4ad875a`, the same at `a7c39fa`). A game that passes
`XCONTENT_DATA` gets 0 there, as every Edge header in the samples holds, so
for those saves `maker_profile` is empty. Canary keeps 0 too when it builds
a header from Edge's. Collect reads the maker from the headers on disk and
restore from the unit's, which keep it however long they're stored.

## Open items

- GoD containers and STFS packages are not read for identification.
- Some games bind a save to the profile it was made under, so a unit restored under another XUID has the right files in the right place and the game still won't use it. Forza Horizon 2 says "It appears your profile has been tampered with" under any other XUID and loads the save under its own, in Edge and in Canary. Charlie Murder writes the gamertag and XUID into its `game.sav` and, under another profile, starts as if there were no save. `maker_profile` names that XUID for a save Canary made (sample `gears-of-war-2-xenia-canary-new`), and for one Edge or XenDroid made only when the game passed an aggregate whose XUID was -1; for the others it is empty ([sync](#sync)). Restored to Canary under another profile, a Canary header names that profile instead; the unit keeps the maker, which restore reports.
- Achievements, and the settings a game keeps in its GPD rather than in a save, don't travel. A game that keeps all its progress there (Left 4 Dead 2 writes no save folder) has nothing to sync.
- A header built in the other form keeps what both forms carry and loses the rest. For XenDroid, from Canary's container, it loses every field of `XContentMetadata` but the content type, title id and one display name (`xcontent/xcontent.h` at `27d11a2`): `metadata_version`, the content size, the rest of the execution info, the console id, `profile_id` (so `maker_profile` is empty once the save is in XenDroid's form), the volume descriptor, the data file count and size, `volume_type`, the online creator and category, the media or avatar data, the metadata's own device id, the other display names and the descriptions, publisher and title name, the transfer flags, the thumbnail and title thumbnail, and the container's licenses, signature and content id. For Canary, from an aggregate, it loses the device id and the XUID, which `FillHeader` keeps neither of, and any display name past 127 units. Edge gets every header as it is, so a save that passes through Edge keeps its header.
- Canary lists no save whose header it can't read, as with a missing header: one shorter than 0x148 bytes, of a size it has no form for, or a container whose magic isn't valid. Such a header travels as it is, and Edge and XenDroid list the save by its folder's name.
- A profile Canary keeps as one STFS package file at `content/<XUID>/FFFE07D1/00010000/<XUID>`, as Install Content leaves a read-only package (`emulator.cc` `InstallContentPackage`, `content_manager.cc` `InstallContentPackage` and `OpenPackage` at `27d11a2`, which opens any path that isn't a folder as a package), isn't listed: sigil reads no STFS, so it can't see the `Account` inside. Its XUID passed as `profile` is used as given.
- Upstream Xenia (`xenia-project/xenia`) has no row. It keeps a save at `content/<title id>/<type>/<save name>/`, with no XUID folder and no header (`content_manager.cc` `ResolvePackageRoot` at `95a5c3e`), so none of the rows above reads or writes its tree.
- XenDroid's row is read from its source (`rfandango/XenDroid`); it hasn't been run.
- The extractor is experimental until it is validated against real-world samples.
