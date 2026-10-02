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
