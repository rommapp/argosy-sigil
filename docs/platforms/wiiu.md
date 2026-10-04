# Wii U

Status: synced
sigil identifies Wii U WUA archives and collects and restores Cemu save folders, per account and shared by every account.

## Identification

| Slug | Platform | Inputs | `title_id` example | `usage` | Status |
|---|---|---|---|---|---|
| `wiiu` | Wii U | `.wua` | `10143500` (last 8 of folder name) | folder-exact | |

See [Identification](../identification.md) for the result fields. A `.wua` is a ZArchive; [Containers](../containers.md) covers how sigil reads it.

Wii and Wii U diverge the same way and for the same reason: Dolphin
writes `Wii/title/00010000/525a4445` and Cemu writes
`mlc01/usr/save/00050000/1010ec00`, both with `{:08x}`, so `save_id` is
the lowercase form of `title_id` on those two platforms
(`title_id=525A4445`, `save_id=525a4445`).

**Wii U uses the last 8 of 16 hex digits.** WUA archives carry a top-level folder
named `00050000<8 hex>_v0`. The full 16 hex is the formal title ID;
the last 8 chars are what the save system keys on. Sigil emits the
last 8 as `title_id`, the full 16 as `raw_serial`.

## Save layouts

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| `cemu` | folders per profile: `mlc01/usr/save/00050000/{save_id}/user/{profile}/` account, `.../user/common/` and `.../meta/` device | | emulator source; on device (Cemu on Android) |

The layout lists `mlc01/usr/save/00050000` and `mlc01/usr/save/system/act`,
which `sigil_save_layout_subdirs()` names.

Cemu keeps a game's saves per user profile, and also keeps saves every profile on the device shares. [Save units](../save-units.md#profiles) has the rules for picking a profile.

| Layout | Account | Device | Profile list |
|---|---|---|---|
| `cemu` | `mlc01/usr/save/00050000/{save_id}/user/{profile}/` | `user/common/` and `meta/` beside it | `mlc01/usr/save/system/act/{profile}/account.dat` (`PersistentId`, `MiiName`) |

A profile id is named as its save folder is, such as `80000001`. The root is the emulator's base folder, the one holding `mlc01/`.

A Cemu unit holds
`<save_id>/meta/`, `<save_id>/user/account/` and `<save_id>/user/common/`; an
older unit naming the account folder by its id restores as the account
save.

The `meta/meta.xml` in each save folder declares `common_save_size` and
`account_save_size`, and they predict where the save sits: a game that
declares a common size and no account size keeps its whole save in
`user/common/`, and the reverse keeps it under the account. Checked
against live Cemu saves.

## Sync

On a layout
with profiles the unit is a zip of the game's save folders under the names
above, and `companions` is `SIGIL_ERR_INVALID_ARG`.

On restore, Cemu's `meta/`, which
Cemu writes again by itself, is skipped instead of failing with `SIGIL_ERR_NO_TARGET` when no folder of the game takes it.
[Sync](../sync.md) has the rest of collect and restore.

## Open items

1. Read `meta.xml` from a Wii U WUA in the corpus and confirm the `common_save_size` and `account_save_size` fields.
