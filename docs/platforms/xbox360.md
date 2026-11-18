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
| `xenia`, `xenia_canary`, `xenia_edge` | folders per profile: `content/{profile}/{save_id}/00000001/<save>/`, each save's header in `content/{profile}/{save_id}/Headers/00000001/`; the same under the all-zero XUID for a save no profile owns | | emulator source |
| `xendroid` | as `xenia`, under the `content/` folder of XenDroid's storage root | | emulator source |

The rows list `content`, which `sigil_save_layout_subdirs()` names, and
match `xbox360` only. `xenia` is Xenia Canary's and Xenia Edge's tree;
`xenia_canary`, `xenia_edge` and `xendroid` are the same rows under the
names those emulators go by. Xenia keeps a game's saves per profile, and
a save the game marks shared, or makes with no one signed in, under the
all-zero XUID:

| Layout | Account | Device | Profile list |
|---|---|---|---|
| `xenia`, `xenia_canary`, `xenia_edge`, `xendroid` | `content/{profile}/{save_id}/00000001/` and `content/{profile}/{save_id}/Headers/00000001/` | the same under `0000000000000000` | `content/{profile}/FFFE07D1/00010000/{profile}/Account`; ids only |

The base folder is the one holding `content/`, Xenia's storage root
unless `--content_root` moves it; a moved content root is still found when
its folder is named `content`. A profile id is the
profile's XUID as Xenia names its folder, 16 uppercase hex digits
(`E030000043414231`). How sigil picks the profile is in
[profiles](../save-units.md#profiles).

**Profiles are listed by id alone.** The profile's name, its gamertag, is
in the `Account` file, which is encrypted with a key of the console's
that sigil doesn't carry, so every profile's `name` is empty. A client
asks the user which XUID is theirs, or uses the one it knows. A profile
folder without an `Account` file is not a profile, as a user folder
without `localusername` isn't on RPCS3. A XUID a client passes that the
emulator doesn't list is used as given: pass it in uppercase, as Xenia
names its folders.

## Save formats

Xenia keeps each save as an extracted folder,
`content/<XUID>/<title id>/00000001/<save name>/`, with its metadata in
`content/<XUID>/<title id>/Headers/00000001/<save name>.header`. Xenia
Edge writes the header as `XCONTENT_AGGREGATE_DATA` alone (328 bytes);
Xenia Canary rewrites it as the whole container header, padded to 4 KiB,
when the game closes the save. Each reads the other's form
(`content_manager.cc` in Edge, `xcontent_package_directory.cc` in
Canary), so a save and its header move between them unchanged. Canary
doesn't list a save whose header is missing, so the header travels with
the save. XenDroid uses Canary's tree under its own storage root's
`content/`. Copying the folder and its header moves a save between Xenia
installs; moving one to a console needs the STFS package rebuilt and
re-signed.

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

## Open items

- GoD containers and STFS packages are not read for identification.
- Some games bind a save to the profile it was made under, so a unit restored under another XUID has the right files in the right place and the game still won't use it. Forza Horizon 2 says "It appears your profile has been tampered with" under any other XUID and loads the save under its own. Charlie Murder writes the gamertag and XUID into its `game.sav` and, under another profile, starts as if there were no save. Such a save moves between emulators under the XUID it was made under: pass that XUID as `profile`.
- Achievements, and the settings a game keeps in its GPD rather than in a save, don't travel. A game that keeps all its progress there (Left 4 Dead 2 writes no save folder) has nothing to sync.
- Restore writes each save's header as the source emulator wrote it: Edge's 328 bytes into a Canary folder, Canary's 40,960 into an Edge folder. Each reads the other's form; Canary rewrites an Edge header in its own form once the game saves, so until then the file isn't the one Canary would write itself.
- XenDroid's row is read from its source (`rfandango/XenDroid`); it hasn't been run.
- The extractor is experimental until it is validated against real-world samples.
