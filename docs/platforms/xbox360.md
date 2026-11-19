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
| `xenia_edge` | as `xenia_canary`, each header as `XCONTENT_AGGREGATE_DATA` (0x148 bytes) | | emulator source, samples |
| `xendroid` | as `xenia_edge`, under the `content/` folder of XenDroid's storage root | | emulator source |

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

Edge reads Canary's form (`content_manager.cc` `ReadContentHeaderFile`,
`vfs/devices/xcontent_container_device.cc` `ContentDataFromHeader`), and
Canary reads Edge's as its older header. Canary doesn't list a save whose
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
restore writes it in the form the target's row names, built the way that
emulator builds it from the other form:

- For Canary, Canary's `FillHeader`. From Edge's header of Forza Horizon 2
  this is byte for byte the header Canary wrote from it
  (`forza-horizon-2-xenia-canary`).
- For Edge and XenDroid, Edge's `ContentDataFromHeader`: device id 1, the
  content type, the English display name (else the first one there), the
  save's file name, the title id, and XUID 0, as Edge writes for a save a
  game makes by passing `XCONTENT_DATA`. Against the headers Edge wrote
  itself in the samples it differs only in the padding Edge leaves unset
  (0x134 and 0x144), which restore writes as 0.

A header already in the target's form is written as it came. A unit header
in neither form, which no Xenia reads, is `SIGIL_ERR_UNSUPPORTED_FORMAT`,
writing nothing. `identity_hash` covers what both forms carry, the
content type, display name, file name and title id, so a save is the same
save in either form; `content_hash` covers the unit's bytes.

**The maker.** `maker_profile` is the XUID the account saves' headers name
as the one they were made under: for a save Canary made, its
`profile_id`. Collect reads it from the headers on disk and restore from
the unit's, which keep it however long they're stored. Edge's headers name
none: it writes 0 there for a save a game makes by passing
`XCONTENT_DATA` (`content_manager.h` `XCONTENT_AGGREGATE_DATA`,
`content_manager.cc` `WriteContentHeaderFile` and `xam_content.cc`, the
same at `a7c39fa`), and every Edge header in the samples holds 0. Canary
keeps 0 too when it builds a header from Edge's. So for those saves
`maker_profile` is empty.

## Open items

- GoD containers and STFS packages are not read for identification.
- Some games bind a save to the profile it was made under, so a unit restored under another XUID has the right files in the right place and the game still won't use it. Forza Horizon 2 says "It appears your profile has been tampered with" under any other XUID and loads the save under its own, in Edge and in Canary. Charlie Murder writes the gamertag and XUID into its `game.sav` and, under another profile, starts as if there were no save. `maker_profile` names that XUID only for a save Canary made (sample `gears-of-war-2-xenia-canary-new`); for Edge's and XenDroid's saves it is empty ([sync](#sync)). A header naming another XUID doesn't stop the save loading: restored under another XUID, that sample's slot shows in Canary and in Edge.
- Achievements, and the settings a game keeps in its GPD rather than in a save, don't travel. A game that keeps all its progress there (Left 4 Dead 2 writes no save folder) has nothing to sync.
- A header restored in the other form keeps what both forms carry and loses the rest. From Canary's to Edge's or XenDroid's it loses every field of `XContentMetadata` but the content type, title id and one display name (`xcontent/xcontent.h` at `27d11a2`): the other display names and the descriptions, publisher and title name, the thumbnail and title thumbnail, the rest of the execution info, the console id, `profile_id` (so `maker_profile` is empty once the save is in Edge's form), the content size, the volume descriptor, the data file count and size, the online creator and category, and the container's licenses, signature and content id. From Edge's to Canary's it loses the device id and the XUID, which `FillHeader` keeps neither of, and any display name past 127 units. A save Canary made that goes to Edge and back comes back with the header `FillHeader` builds, the way Canary itself rebuilds an Edge header.
- Upstream Xenia (`xenia-project/xenia`) has no row. It keeps a save at `content/<title id>/<type>/<save name>/`, with no XUID folder and no header (`content_manager.cc` `ResolvePackageRoot` at `95a5c3e`), so none of the rows above reads or writes its tree.
- XenDroid's row is read from its source (`rfandango/XenDroid`); it hasn't been run.
- The extractor is experimental until it is validated against real-world samples.
