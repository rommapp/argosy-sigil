# Per-user and device-wide saves

Status: research for the next area of work, after the Vita `vita_pops` check. Nothing here is implemented. Each claim is marked **verified** (read from source, with the citation in the linked research file) or **lead** (from memory or community docs, to check before relying on it).

## The problem

Some platforms keep a game's saves per user account on the device, and also keep data that belongs to the device rather than to any user. A few games split one game's state across both, such as Mario Kart 8 Deluxe and Animal Crossing: New Horizons on Switch. sigil syncs a game's saves as one unit per game today. On these platforms a client has to know:

1. which user's saves to collect, and where to put them on restore;
2. whether the game also keeps device-wide data, and where;
3. whether that device-wide data is the same everywhere (safe to share across RomM users) or tied to one person (unsafe to overwrite from another user's sync).

sigil can't decide (3) for the client ([sigil reports, the client decides](../save-roadmap.md)). It may be able to report (2) from title metadata, which is the point of this investigation.

## Platform by platform

### Switch

- **verified**: saves are per application per user, plus "device" saves with no user. yuzu forks keep device saves under user `0` (32 zeros) in the legacy layout, and under `user/save/device/<TITLEID>/0/` in the newer layout. Ryujinx keys every save by `(ProgramId, UserId, Type)` in `imkvdb.arc`, where the type tells account from device saves (enum values UNVERIFIED). See [nintendo-disc.md](nintendo-disc.md) rows 7 and 8.
- **verified**: JKSV backups carry `.nx_save_meta.bin` with `saveDataType`, the account id and the declared save size, so a JKSV unit says whether it is an account or device save. See row 9.
- **lead**: each game's control data (NACP, in the control NCA's RomFS as `control.nacp`) declares the save areas the game asks the system to create: user account save size and journal size, device save size and journal size, BCAT delivery cache size, temporary storage size, cache storage size, plus `StartupUserAccount` (whether the game requires a user to be picked at boot). A nonzero device save size would be the flag that a game keeps device-wide data. Field offsets are on switchbrew's NACP page; check them before use.
- **lead**: Animal Crossing: New Horizons keeps the island for every resident in one save, and its island transfer tool exists because the island is tied to the console. Whether that save is an account save, a device save or both is what the NACP check above would answer. Mario Kart 8 Deluxe: same question, not checked.
- What sigil reads today: Switch NSP, XCI and NCA headers, decrypted with the header key, for the title id (`src/switch_nca.c`). Reading the NACP needs the control NCA's section decrypted, which needs the key-area keys (and the title key for NCAs with a rights id) from the user's `prod.keys` and `title.keys`. That is a larger step than anything sigil does with keys now.

### Wii U

- **verified**: each title's save directory holds `user/<persistentId>/` per account and `user/common/` shared by every account on the console, plus `meta/`. Cemu's own export drops `common/`, so a client must carry it itself. See [nintendo-disc.md](nintendo-disc.md) row 6.
- **lead**: the title's `meta/meta.xml` declares `common_save_size` and `account_save_size`. A nonzero common size would be the flag that a game uses the shared area. sigil already opens WUA archives (`src/wiiu_wua.c`), which are decrypted, so reading `meta.xml` from a WUA looks cheap. Encrypted WUD/WUX would need keys.

### 3DS

- **verified**: no user accounts. A title's save is `title/<high>/<low>/data/00000001/`, plus any extra data (extdata) folders it uses. See [nintendo-cart.md](nintendo-cart.md) note 7.
- **lead**: a title's extended header names the extdata id it may use; some extdata is shared between titles (a series sharing data, system extdata). That id is the flag for "this game keeps data outside its save". Reading it from a retail image needs the NCCH decrypted.

### Wii, NDS

- **verified**: no per-user saves. Wii saves are per title in the NAND, NDS saves per cart. Nothing to split.

### Vita

- **verified**: Vita3K keeps saves under `ux0/user/<user_id>/savedata/<SAVEDIR>/`, `SAVEDIR` from the app's `param.sfo`. See [sony.md](sony.md). The real console has one user. Nothing to split beyond picking the user folder.

### PS3

- **verified**: RPCS3 keeps saves under `dev_hdd0/home/<userId %08u>/savedata/<DIRNAME>/`, default user `00000001`; each save folder has its own `PARAM.SFO`. See [sony.md](sony.md).
- **lead**: a PS3 save's `PARAM.SFO` carries the owning account id and a copy-protection flag that stops a save being used by another account on real hardware. Game data installs (`dev_hdd0/game/<id>/`) are per console, not per user, and some games keep progress-like data there. Not checked.

### PS4

- **lead**: saves are per user (shadPS4 keeps them under a user id folder). Device-wide game data exists. Nothing in this repo has been checked yet.

### Xbox 360

- **verified**: Xenia Canary keeps saves under `<content_root>/<XUID>/<TitleID>/00000001/<name>/`. Content with no user goes under XUID `0000000000000000`. On upgrade, Xenia moves saved games to the user and other content types to the common XUID. See [other.md](other.md) section 12.
- **lead**: some games write the profile's XUID inside their own save files and refuse a save from another profile. That can't be read from metadata; it would need per-title notes.

## What sigil could offer

Options for the discussion, not decisions:

1. **Report declared save areas per title.** For each game, the save areas its metadata declares (account, device or common, cache, extdata) and their sizes, from the NACP, `meta.xml` or exheader. The client then knows a game keeps device-wide data before it syncs anything. This needs the metadata readers listed above, and keys for Switch and 3DS.
2. **Split units by area.** Collect returns the account part and the device or common part as separate units (or separate members of one unit, tagged by area), so the client can sync them under different rules: per RomM user, per device, or once for everyone. Restore takes them back the same way.
3. **Map RomM users to local profiles.** Every platform above needs a local user id on restore: a Wii U persistent id, a Switch profile UUID, an Xbox XUID, a PS3 user number. sigil would take it as an input, the way it takes the save root today, and never pick one itself.
4. **Leave it to the client.** Document the layouts (as the research files do) and stop there.

Open questions for the discussion:

- Is device-wide data synced per RomM user, per device, or once for everyone? It differs per game, which is why reporting beats deciding.
- When a device save exists and a second RomM user syncs on the same device, which one wins? Under the current model sigil reports a conflict and the client decides.
- How much key handling is in scope for reading the Switch NACP and the 3DS exheader?

## Next checks, cheapest first

1. Read `meta.xml` from a Wii U WUA in the corpus and confirm the `common_save_size` and `account_save_size` fields.
2. Read the NACP of a few Switch titles with a known split (Animal Crossing: New Horizons, Mario Kart 8 Deluxe) and of a few without one, using the local ROM corpus and its `prod.keys`, and confirm the field offsets.
3. Check how Ryujinx and the yuzu forks lay out a device save on disk for one of those titles.
4. Check one 3DS title known to use shared extdata.
