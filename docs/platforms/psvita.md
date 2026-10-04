# PS Vita

Status: synced
sigil reads `TITLE_ID` from the dump's `param.sfo`, finds the save folder under the Vita3K user profile, and syncs it with collect and restore.

PS1 classics running on a Vita (official PS1 Classics and Adrenaline) use the `vita_pops` layout, which is on the [PlayStation](psx.md) page.

## Identification

| Slug | Inputs | `title_id` example | `usage` | Status |
|---|---|---|---|---|
| `psvita` | `.zip` dump, extracted folder, or `param.sfo` | `PCSE12345` (TITLE_ID from `sce_sys/param.sfo`); `save_id` is `INSTALL_DIR_SAVEDATA` when set | folder-exact | filename fallback when no `param.sfo` is reachable |

`vita` resolves as an alias of `psvita`, both as a platform slug and as a
layout row platform.

**PS Vita reads param.sfo, not the filename.** The identifier is `TITLE_ID`
in `sce_sys/param.sfo`, the same file Vita3K reads to identify installed
content. Dumps in circulation keep it at `app/<TITLEID>/sce_sys/param.sfo`
inside a zip, so sigil addresses that member by path suffix rather than
by the largest-member rule the generic archive branch uses.

Finding that member is also what identifies the dump as Vita at all. A
`.zip` names no platform, and the bracketed-serial convention in these
filenames is shared with PSP, so a name-based guess resolves the wrong
platform. Detection is by content and needs no hint.

Two ordering details matter. A dump can carry a second `param.sfo` under
`savedata/`, which describes a save rather than the title and has no
`TITLE_ID`; sigil prefers the shallowest match, in a zip and in an
extracted folder alike, because a title's own metadata always sits above
anything subordinate to it. Two equally deep go to the smaller path. And the filename
scanner still runs, but only when no `param.sfo` can be reached, so it is
a fallback rather than the primary path; `source` tells you which one
answered.

Saves are one exact directory per title, hence `folder-exact`. The
directory is `ux0:user/00/savedata/<save_id>`. `save_id` is `TITLE_ID`
unless `param.sfo` sets `INSTALL_DIR_SAVEDATA`. A sequel or another
region that shares an earlier title's saves sets that key to the earlier
title's id, and the firmware and Vita3K both save there. `title_id` and
`raw_serial` stay the dump's own `TITLE_ID`.

A dump identified from its filename has no `param.sfo` to read, so its
`save_id` is the `TITLE_ID` in the name. For a title that shares another
title's saves, that names the wrong folder, and collect finds nothing.
A client holding an incomplete or renamed dump of such a title can set
`save_id` to the shared id on the result it passes to collect and restore.

```sh
# A Vita dump resolves by content, so it needs no hint
$ sigil "/path/to/Actual Sunlight [PCSE00695] [USA] [NoNpDrm].zip"
platform=psvita title_id=PCSE00695 raw_serial=PCSE00695 save_id=PCSE00695 usage=folder-exact source=binary
```

## Save layouts

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| `vita3k` | folders per profile: `ux0/user/{profile}/savedata/{save_id}/` | | emulator source |

The layout lists `ux0/user`, which `sigil_save_layout_subdirs()` names.
The row matches `psvita` only. Vita3K keeps a game's saves per user
profile, with no device area:

| Layout | Account | Device | Profile list |
|---|---|---|---|
| `vita3k` | `ux0/user/{profile}/savedata/{save_id}/` | | `ux0/user/{profile}/user.xml` |

The base folder is the one holding `ux0/`, and a profile id is the user
folder's name (`00`). How sigil picks the profile is in
[profiles](../save-units.md#profiles).

Vita3K writes save files decrypted, so copying a folder moves a save
between Vita3K installs without loss. A real Vita encrypts its saves,
and moving one to or from hardware needs a modded console's tools.

**PS Vita on Vita3K** (`org.vita3k.emulator`).
`vita/ux0/user/00/savedata/<save_id>/`, one directory per title, matching
`folder-exact`. Vita3K's default user is `00`, and it maps the game's
save device to `ux0:user/<user>/savedata/<SAVEDIR>`, where `SAVEDIR` is
`INSTALL_DIR_SAVEDATA` or `TITLE_ID`.
The root sits under `/storage/emulated/0/Android/data/<package>/files/`;
see [saves on Android](../saves-on-android.md).

## Sync

On a layout with profiles the unit is a zip of the game's save folders,
and `companions` is `SIGIL_ERR_INVALID_ARG`. The general rules for profile
layouts are in [sync](../sync.md).

## Open items

- Vita3K's default root per OS is unconfirmed.
- Real-hardware interop for Vita and PS3 needs encryption tooling outside RomM's reach. Emulator-to-emulator folder copies are lossless.
- A dump identified by filename alone keeps `save_id = TITLE_ID`, which misses titles that share another title's save folder through `INSTALL_DIR_SAVEDATA`. See [identification](#identification).
