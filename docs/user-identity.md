# User identity

Status: proposal, parked. Mostly a RomM server and client feature; sigil's part is small and listed at the end. Tracks [rommapp/romm#4388](https://github.com/rommapp/romm/issues/4388).

## The three layers

An emulator's system data splits into three layers with different owners:

| Layer | Owner | Examples |
|---|---|---|
| Firmware | Shared by every user of a platform | 3DS system titles (`nand/<id>/title/`), `nand/dbs/ticket.db`, `sysdata/seeddb.bin`; Switch firmware; Wii IOS |
| Identity | One user, one console | 3DS `nand/private/movable.sed`, `sysdata/otp.bin`, `nand/rw/sys/SecureInfo_A`, `nand/rw/sys/LocalFriendCodeSeed_B`, `sysdata/mac.txt`, the system saves under `nand/data/<id>/sysdata/` (`00010017` is CFG: settings and user name); xemu `eeprom.bin`; Switch `profiles.dat`; Cemu `account.dat` |
| Save | One game, per user | What sigil collects and restores |

RomM stores firmware per platform today and has no place for identity. Users who need it put it in the firmware, which hands their identity to every user of the server.

## Why identity is its own layer

- It is a credential. A 3DS identity dropped into a fresh Azahar install connects to Pretendo as that console. The same holds for Wiimmfi, Nintendo Network replacements, Xbox Live replacements, and PSN on PS3 (UNVERIFIED per service).
- The emulator runs without it. Azahar boots on firmware alone and generates an identity when none is given.
- Some saves depend on it. Checked on two Azahar installs (2026-09-29): Azahar keeps SD and NAND data under the all-zeros id folders even with a real `movable.sed` present, and keeps saves as plain host files. So the dependence is not the folder path or at-rest encryption. It is either a game checking console-specific values at runtime, or saves first made on real hardware. Which games, and which of the two, is UNVERIFIED; one failing save with its title id settles it.

## RomM

- `platform.user_identities`: a collection per platform. Each entry is one identity bundle, named by its owner ("my o3DS", "Azahar generated"), owned by exactly one user. A user can own several per platform: two consoles, a dumped and a generated identity, several Switch profiles.
- Only the owner can download a bundle. It never appears in shared firmware, public links or another user's client. Admins get no browse or download path by default.
- Encrypted at rest, kept out of logs and out of exports that leave the server.
- The client installs the chosen bundle beside the firmware and keeps no other copy.
- One identity online on two devices at once is two consoles with one id. Whether the online services flag it is UNVERIFIED. The client warns, or keeps a bundle active on one device at a time.
- Each save unit records the fingerprint of the bundle it was made under: a hash over the bundle's identity files, never the files. A client restoring a save can then install the right bundle first, or warn that the device lacks it.

## sigil

- Identity files are never members of a save unit. Layout rows for NAND trees (3DS, Switch, Wii, Wii U) mark them so a folder member can't sweep them up.
- A classifier that sorts a NAND or SD tree into firmware, identity and saves, for clients building uploads.
- An identity fingerprint over a platform's identity files, for the save unit to carry.

Nothing here is built. It waits until the save and memory card work lands.

## Per-user and device-wide saves

Status: implemented for the yuzu forks, Cemu, Vita3K and RPCS3 ([save-units.md](save-units.md#profiles)). Ryubing, Xenia, the yuzu forks' newer layout and the NACP and exheader reads are not. Each system's findings, marked **verified** (read from source) or **lead** (from memory or community docs, to check before relying on it), are on its page: [switch](platforms/switch.md), [wiiu](platforms/wiiu.md), [3ds](platforms/3ds.md), [wii](platforms/wii.md), [nds](platforms/nds.md), [psvita](platforms/psvita.md), [ps3](platforms/ps3.md), [xbox360](platforms/xbox360.md).

### The problem

Some platforms keep a game's saves per user account on the device, and also keep data that belongs to the device rather than to any user. A few games split one game's state across both, such as Mario Kart 8 Deluxe and Animal Crossing: New Horizons on Switch. On these platforms a client has to know:

1. which user's saves to collect, and where to put them on restore;
2. whether the game also keeps device-wide data, and where;
3. whether that device-wide data is the same everywhere (safe to share across RomM users) or tied to one person (unsafe to overwrite from another user's sync).

sigil can't decide (3) for the client ([sigil reports, the client decides](save-roadmap.md)). It may be able to report (2) from title metadata.

### What sigil could offer

Options considered before the direction below was chosen:

1. **Report declared save areas per title.** For each game, the save areas its metadata declares (account, device or common, cache, extdata) and their sizes, from the NACP, `meta.xml` or exheader. The client then knows a game keeps device-wide data before it syncs anything. This needs the metadata readers on the system pages, and keys for Switch and 3DS.
2. **Split units by area.** Collect returns the account part and the device or common part as separate units (or separate members of one unit, tagged by area), so the client can sync them under different rules: per RomM user, per device, or once for everyone. Restore takes them back the same way.
3. **Map RomM users to local profiles.** Every platform above needs a local user id on restore: a Wii U persistent id, a Switch profile UUID, an Xbox XUID, a PS3 user number. sigil would take it as an input, the way it takes the save root today, and never pick one itself.
4. **Leave it to the client.** Document the layouts and stop there.

Open questions:

- Is device-wide data synced per RomM user, per device, or once for everyone? It differs per game, which is why reporting beats deciding.
- When a device save exists and a second RomM user syncs on the same device, which one wins? Under the current model sigil reports a conflict and the client decides.
- How much key handling is in scope for reading the Switch NACP and the 3DS exheader?

### Direction

- The client gives the base save path, as today. The user profile follows the rule Dolphin's card files already use: a profile id the client passes wins; else the one user profile under the base path; with two or more and none given, `SIGIL_ERR_AMBIGUOUS` listing them; with none, account saves have no target (`SIGIL_ERR_NO_TARGET`). The device or system profile needs no choice. sigil reads the profiles from the emulator's own list (yuzu forks `profiles.dat`, Cemu `act/` accounts, Ryubing `Profiles.json`).
- The account-versus-device split is per save, not per game. Collect knows each save's kind from where it sits (a user's folder or the device location; Ryubing's indexer records the type). The unit records each part's kind, likely as a top-level folder per kind in the zip, so restore puts account saves under the given profile and device saves in the device location without asking for a profile.
- Folder units already carry `save_id` and its usage (exact, prefix, or the 3DS split `00040000/00033500`); the profile only decides the parent folder.

### Next checks, cheapest first

1. Read `meta.xml` from a Wii U WUA in the corpus and confirm the `common_save_size` and `account_save_size` fields.
2. Read the NACP of a few Switch titles with a known split (Animal Crossing: New Horizons, Mario Kart 8 Deluxe) and of a few without one, using the local ROM corpus and its `prod.keys`, and confirm the field offsets.
3. Check how Ryujinx and the yuzu forks lay out a device save on disk for one of those titles.
4. Check one 3DS title known to use shared extdata.
