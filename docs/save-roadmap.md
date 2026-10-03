# Save handling roadmap

Sigil identifies games and resolves libretro save units today. RomM's save sync is moving to a model where every client and the RomM server store a game's save in one neutral form per platform, holding that game's data and nothing else. Sigil is the library that gets them there, so the same save packs, hashes and restores the same way in Argosy, Tender, RomM's streaming and RomM's own migrations.

This page lists the planned work. Nothing here exists yet unless it says so. The sourced emulator research behind it is in [save-research](save-research/README.md).

## Rules for all of it

- One implementation. The logic lives in the C library and reaches every consumer through the Kotlin, Python and Go bindings. No consumer carries its own copy of a format.
- A small client interface. Format work, ownership decisions and sync bookkeeping all happen inside sigil. A client makes a few calls and never needs to know how a card, a volume or a folder card works.
- No filesystem access and no stored state. Sigil keeps the current model: the caller lists directories and supplies an open callback. Writing gets the same treatment, a write callback, so sigil never picks paths or touches storage. What sigil has to remember between calls comes back as an opaque state blob that the client stores and passes in next time.
- Deterministic output. The same saves always produce the same bytes and the same hash, wherever they are packed.
- Tested before it ships. Every format lands with captured sample files in `tests/fixtures/`. Extracting a save and injecting it back must return the same save bytes and leave every other save on the card unchanged. Building a card twice from the same inputs must give the same bytes. Converting to the neutral form and back must round-trip exactly.
- Fuzzed before the server runs it. The RomM server will parse uploaded files with sigil, so every parser gets a fuzz harness first.
- Clean licensing. Sigil is MPL-2.0. Code comes from our own work, public-domain sources or permissive licenses. GPL code, such as Dolphin's `.gci` handling or mymcplus, is read for understanding and never copied.

## Client interface

sigil does the format work, decides who owns each save, and keeps the sync bookkeeping. A client does four things sigil can't:

1. Lists directories and provides the `open` and `write` callbacks.
2. Says when a session has ended. Only the client can see the emulator close or die.
3. Stores the state blob and moves units to and from RomM.
4. Decides which side wins when the local and the remote saves both changed, shows the user saves with no known owner, and passes the answers back. sigil reports what it finds and never picks a winner.

The public calls:

- `collect(request, listing, state, open)` returns the units to upload, whether each changed since the last sync, saves with no known owner, and a new state.
- `restore(request, listing, units, state, open, write)` writes units into place, verifies them, and returns a new state. It refuses with a conflict, writing nothing, to overwrite a save that changed locally since the last sync, unless the request says to overwrite local saves.
- `list(request, listing, open)` returns the entries on a card or volume for a card explorer.

The request carries what sigil can't discover by itself:

- the emulator, its version and its core options
- the content, with every disc of a multi-disc set
- companion games, when a game imports saves from an earlier title
- `compatible_with` links, when the user chose to use another release's save
- the mode: managed when the client launches the game, unmanaged when it only syncs
- PCSX2's `memcardFilters` list, when the client has it
- the client's answers: whether a restore overwrites local saves that changed, and which saves with no known owner the user claimed

From those, sigil works out the kind of target (folder card, shared card or per-game card), whether to swap a volume or inject into it, which entries are safe to delete, and whether a change is local, remote or both.

The state blob holds the last-synced `identity_hash` of each unit, the owner learned for each backup RAM entry, which game each shared volume was swapped in for, the identity of the saves last passed on in a holding unit, the snapshot of each volume from the last `collect`, and, in unmanaged mode, what the last `restore` wrote and replaced. The client never reads it. It is text, one fact per line, and lines a version doesn't know are kept, so a blob from an older sigil still loads.

Everything else on this page happens inside those calls.

`collect` and `restore` exist in every binding, tested end to end against the real samples, for:

- PS1 memory cards under pcsx_rearmed and Beetle PSX
- PS2 file cards under LRPS2 (`pcsx2`), shared or per content
- PS2 file and folder cards under standalone PCSX2, AetherSX2, NetherSX2 and ARMSX2 (`pcsx2_standalone`), slot 1 and slot 2 each either kind
- Saturn backup RAM under Beetle Saturn (per game or shared), Kronos (512 KiB to 4 MiB carts, and its Beetle-named mode), the yabause core (64 KiB expanded) and Yaba Sanshiro (one shared 8 MiB expanded `backup.bin`)
- Sega CD backup RAM under genesis_plus_gx, per game or per BIOS region, with the managed swap, the holding unit, claims and the unmanaged inject described below

- Dreamcast VMUs under flycast libretro (shared, VMU A1 per game, every port per game) and standalone flycast
- GameCube under Dolphin, libretro and standalone: the GCI folder (default) and raw cards of every size, units of `.gci` files either way, F-Zero GX rebound to the target card

Companions for stitching go through the same card, volume and folder code for each of these, with tests on PS1 cards, Saturn volumes and Dolphin's GCI folder. Still to come: the remaining standalone emulators. `list` exists as `sigil_card_list`. `sigil_save_resolve` stays public as the lower-level call that locates and hashes one unit, and `collect` is built on it.

## 1. Layout data

The layout table is C arrays in `src/save_layout.c` today, libretro cores only, current versions only.

- Move rows to versioned data that generates both the C table and the README table.
- Add standalone emulators: Dolphin, DuckStation, PCSX2, PPSSPP, melonDS, Cemu, the yuzu forks, Ryujinx, Vita3K, RPCS3, xemu, Xenia.
- Add version ranges. A row applies to a range of emulator versions, because options and file names change between releases. Beetle Saturn wrote `.bkr`, and for a while also exposed `.srm` with the same bytes, until `beetle_saturn_save_method` arrived on 2026-05-25 with `mednafen` as its default. The default flipped to `libretro` on 2026-05-26. pcsx_rearmed reworked `memcard1` and `memcard2` on 2026-05-02. Beetle PSX turned `enable_memcard1` off by default on 2025-12-27.
- Add a scope to each member: per game, or shared by every game.
- Resolve an option value the row doesn't list to the option's default, the way RetroArch does. RetroArch matches values case-sensitively, falls back to `default_value` (or the first listed value for cores without one), and writes the default back to the config on unload.
- Add a `settings` role for members that hold device configuration, such as Beetle Saturn's `.smpc` (clock and console language). Settings members are reported and never bundled. This changes today's behaviour: the Beetle Saturn row bundles `.smpc` as a sidecar (`src/save_layout.c` L45).
- Start from [emu-atlas](https://github.com/danielcopper/emu-atlas) (MIT), which records the governing option, scope and role for many libretro cores and standalone emulators, pinned to upstream commits.

API: `sigil_save_request` gains `emulator_version`. `sigil_save_unit` gains a scope per member, and moves `.smpc` from its sidecars to a separate settings list.

## 2. Normalization before hashing

- Strip RetroArch's RZIP compression and Mednafen's gzip before hashing, so a compressed and an uncompressed copy of one save hash the same.
- The unit's hashes cover the neutral form, not the native files. `identity_hash` leaves out the clock member, since it changes when the save data doesn't.
- For a card or backup RAM volume, `identity_hash` covers each entry's name and data, not the card bytes, so entry timestamps and block placement don't read as a new save. This changes the README's definition (`README.md` L230, L256), which today is `content_hash` of the artifact without its rtc members.
- Switch units hash only the committed save tree. For Ryujinx that's `<id>/0/`, or `<id>/1/` for non-journaling saves, which have no `0/`. For the yuzu forks it's the legacy title folder or the newer `.../0/` folder, without the size sidecar. Ryujinx's working copy, `ExtraData0`/`ExtraData1` and `.lock` stay out. Ryujinx copies `0/` over `1/` every time it mounts a save, even when the game only reads it.

## 3. Conversion to the neutral form

One neutral form per platform. sigil converts native files into it before upload and back out on restore. RomM only ever holds neutral forms, so any client can read what it stores without knowing which emulator wrote it.

| Platform | Neutral form | Native forms to convert |
|---|---|---|
| NES | `save.sram` | Mesen2's MMC5 and Namco163 `.sav`, which append mapper RAM after the save RAM |
| FDS | `disk.fds` (the rebuilt disk image) | fceumm's headerless disk image; Mesen2's IPS and Nestopia's UPS or IPS against the original ROM, which RomM holds |
| SNES | `save.sram`; `clock.rtc` for the S-RTC and SPC7110 games | snes9x's 20-byte, bsnes's 16-byte and Mesen2's 24-byte `.rtc`, which don't match each other (field mapping UNVERIFIED) |
| Genesis, SMS, GG, 32X | `save.sram`, padded with `0xFF` to the full size | genesis_plus_gx trims trailing `0xFF`, so its length varies. picodrive pads with `0x00` to a fixed size. Odd-byte SRAM is stored expanded in both |
| GB, GBC | `save.sram`, `clock.rtc` (48-byte VBA layout, with current regs valid at the UTC time in the stamp) | `.sav` with an appended clock footer; `.srm` plus `.rtc`; mGBA's latched-regs pairing; gambatte's 8-byte (loses halt and carry), Mesen2's 13-byte (loses latched regs) and TGB Dual's 4-byte (no wall time) clocks. The neutral clock covers MBC3. HuC3, TAMA5 and TPP1 clocks have emulator-specific forms (SameBoy, mGBA) and need their own definitions |
| GBA | `save.sram`, trimmed to the chip size; `clock.rtc` (BCD calendar, control byte, UTC time) | gpsp's 128 KiB padding, vba_next's padding, mgba libretro's 128 KiB while the save type is still autodetected, mGBA's 16-byte clock footer (local time, loses PM in 12-hour mode), Mesen2's 19-byte `.rtc` |
| N64 | `eeprom`, `pak1` to `pak4`, `sram`, `flash` | the libretro 0x48800-byte blob (eeprom 0x0, paks 0x800, sram 0x20800, flash 0x28800); mupen64plus standalone's single 4-pak `.mpk`; Project64's per-controller paks and sparse `.eep`, `.sra` and `.fla` files, padded with `0xFF` |
| NDS | `save.raw` | DeSmuME's `.dsv` with its 122-byte footer |
| Saturn | `backup.ram` (internal, 32 KiB collapsed), `cart.ram` (backup cart, collapsed) | `.srm` and `.bkr` (same bytes); `.bcr`, gzipped by standalone Mednafen; Kronos `.ram` and `-ext*.ram`; Yabause's 64 KiB expanded volume with `0xFF` filler; Yaba Sanshiro's expanded `backup.bin`, reported as 8 MiB (size UNVERIFIED) |
| Sega CD | `backup.ram` (internal, 8 KiB), `cart.ram` (RAM cart, collapsed) | genesis_plus_gx `scd_*.brm` and cart `.brm`; picodrive's `.srm`, 8 KiB, or 0x12000 bytes with the 64 KiB cart after the first 8 KiB. picodrive's option text warns that enabling the cart discards internal BRAM, yet the one combined sample (writer unknown) holds a valid internal part, so sigil reads both parts; byte-expanded variants (UNVERIFIED) |

mGBA, VBA-M, SameBoy and Gearboy share the 48-byte GB layout, but mGBA pairs its timestamp with the latched registers and the others with the current ones. The neutral clock pins one meaning: current registers, including day bit 8, halt and carry, valid at the UTC time in the stamp. Latched registers are carried but advisory, because games latch again before they read. Converters read mGBA's latched slot as current and write mGBA's latched slot equal to current. Per-emulator detail is in [nintendo-cart.md](save-research/nintendo-cart.md) section 2.1.

`collect` converts native files to the neutral form, and `restore` converts back to what the target emulator writes. Both report when a conversion lost data. Folder saves on PSP, Vita, PS3, Wii, Wii U, 3DS, Switch, Xbox (`UDATA` and `TDATA` trees) and Xbox 360 (Xenia's folder plus its `.header`) are already their neutral form and upload as they are. So does any platform with only one native form that sigil knows of, such as the per-game NGP, 3DO, Lynx, Pokemon Mini, arcade, DOS and CD-i units sigil resolves today (`README.md` L318-326). A second emulator with a different form turns that platform into a table row here. The rule against uploading covers only a platform with several native forms and no converter yet. Those saves wait until the converter exists.

## 4. Containers

Take one game's saves out of a shared card, put them back without disturbing the other games, and build a per-game card holding that game's saves plus any saves it imports from other games.

The per-game result is the neutral form. PS1, PS2 and Dreamcast travel as one per-game card or VMU (`SINGLE`). Saturn and Sega CD travel as a per-game internal volume, plus a per-game cart volume when the game has entries on the cart: `SINGLE` (`backup.ram`) with internal entries alone, otherwise `MULTI`, a zip whose member names say which volume is which. A 4 MiB Saturn volume is Yaba Sanshiro's internal memory or a 32 Mbit cart, so size can't. The two are separate devices, and a game's `BUP_Write` names the device it writes to. GameCube travels as the game's `.gci` files: `SINGLE` for one file, `MULTI` for more.

Emulators that keep saves as folders convert at the edges. Dolphin's GCI folder mode uses the `.gci` files as they are. PS2 has three kinds of target, each handled differently:

| Target | Upload | Restore |
|---|---|---|
| Folder card (PCSX2, AetherSX2, NetherSX2, ARMSX2) | Pack the game's save folders into a per-game card | Unpack into save folders, each with an `_pcsx2_index` built from the card's entries, writing only files whose bytes differ. Files of the game's folders that the unit lacks, and the game's folders it lacks, are removed. Other games' folders and the system folders stay as they are. A `_pcsx2_superblock` missing or shorter than 0x2000 bytes, or with byte 0x16 not `0x6F`, makes PCSX2 show the card as unformatted and hide every save. On a new card, one with no save folders, restore writes a full formatted one first. On a card that holds saves it is damage: restore returns `SIGIL_ERR_DAMAGED` naming it and writes it only with `repair`. Never write it empty. A save folder whose `_pcsx2_index` doesn't parse is damage too: with `repair`, collect packs it without the index and restore writes a fresh one. sigil works on the card PCSX2 shows the game: the game's folders and the system folders packed into an 8 MB card, so another game's folders never count against its space |
| Shared file card (`Mcd001.ps2`) | Extract the game's entries into a per-game card | Inject the entries into the shared card, rebuilding its FAT and ECC |
| Per-game file card (LRPS2 per-content `<content>.ps2`, a PCSX2 per-game card) | Extract, dropping other games' entries | Inject, as for a shared card, so another game's save the user put there stays |

PS2 saves already in RomM as folder zips move to per-game cards in a RomM migration.

### Ownership

A save belongs to the game whose id it carries. On a PS2 card that's the serial in the folder name, matched the way PCSX2 does it. On a PS1 card it's the product code in the directory frame. On a GameCube card it's the game code in the directory entry, which a `.gci` carries as its header. Saves a game only reads, like a sequel reading the previous game's save, belong to the other game.

Dreamcast VMU entries carry a 12-character name the game chooses. It often starts with the product code, but not always, so Dreamcast follows the Saturn and Sega CD rules below.

Every multi-disc PS1 set checked saves under disc 1's product code on every disc. FF7 has the same name in a published list, and FF8, FF9, Valkyrie Profile and Star Ocean 2 are UNVERIFIED ([sony.md](save-research/sony.md) section 1.1). So a card entry matches a game when its code matches any serial of the disc set. sigil identifies one disc at a time today and doesn't read `.m3u` files. The request carries the set, as an `.m3u` or a list of disc paths, and sigil identifies each disc in it.

Some PS2 games save under a serial other than their own. PCSX2's `memcardFilters` in `GameIndex.yaml` lists 421 of them, and 71 of those don't list their own serial at all. That file is GPL-3, so sigil doesn't ship it. A client that has the list passes it in the request.

### Other releases of the same game

Saves stay keyed to the exact content: the ROM hash on cartridge platforms, and the serial or game code on disc platforms. Regional releases, revisions and translations can share a save format or break it in ways a size or checksum check can't see, such as a different character set for stored names ([variants.md](save-research/variants.md)). No existing data groups releases by save compatibility, and the sources that come close are GPL or share-alike.

- sigil never shares a save across releases on its own. It has no data saying which releases belong together. The client or RomM knows that from its own metadata, offers the other release's save to the user as a candidate, and asks.
- When the user says to use it, the request carries a `compatible_with` link naming the other release and its unit. sigil then rewrites the save's id for the target: the PS1 product code, the GameCube game code and region folder, the PS2 folder serial, or the Saturn or Sega CD entry name.
- sigil refuses when the two releases' headers state different save sizes, and warns that names stored in the save may use a different character set.
- A `compatible_with` link overrides the GameCube region rule below, since the user asked for the cross-region move.

### Stitching a card

A game that imports a save from an earlier title needs that save on its card. The request lists the companion games (`companions`), each with its ids and, on restore, its unit from RomM. The client picks the companions, usually from a user action. sigil has no list of which games read which.

`restore` puts the game's saves and each companion's saves on the game's card, volume or GCI folder. Each unit speaks only for its own game: a companion's save inside the game's unit is ignored, and so is the game's save inside a companion's unit. A companion given without a unit keeps whatever saves it already has there. One given with a unit has its saves replaced, under the same conflict rule as the game's own.

Companion saves stay out of the game's unit and hash. `collect` returns one unit per companion with the saves it owns on the game's card, so a companion save the game rewrote goes back to the companion's unit with `changed` set. On cards with ids, the id says whose a save is. On Saturn, Sega CD and Dreamcast volumes, the owner `restore` recorded in the state does.

PS2 `DATA-SYSTEM` and `BWNETCNF` carry no serial and every game sees them. They belong to no game, so `restore` leaves them as they are on the target card and never brings them.

`restore` refuses before writing when:

- the saves don't fit the card's blocks or directory entries, with `SIGIL_ERR_NO_SPACE`. The result's `problem` names the save and `blocks_short` the blocks it lacked.
- a GameCube companion's save belongs to another Dolphin region than the game, with `SIGIL_ERR_REGION`, `problem` naming the save. The game can't read it, and Dolphin keeps it in another region's folder or card. A `compatible_with` link will lift this once it exists.

### Saturn, Sega CD and Dreamcast

Backup RAM and VMU entries carry a name the game chooses and no serial, so the id rule above doesn't apply. In managed mode, swapping volumes per launch avoids needing it:

1. Before launch, `restore` writes a fresh internal volume and, when the emulator has a cart, a fresh cart volume. Each holds only the game's entries for that device, plus any companions, with the format block in place.
2. After the session ends, `collect` assigns every entry on both volumes to the running game, except companion entries, which return to their owners by name.

In managed mode, the one time ownership has to be inferred is the first sync of a volume that already holds saves, such as a shared `backup.bin` the user brings in. `restore` can't upload, so it never swaps away a save that hasn't gone up: it refuses with `SIGIL_ERR_UNCOLLECTED` until every save on the volume either matches its game's last collect or sits in the last holding unit. `collect` returns the saves with no known owner as that holding unit, and the client uploads it before storing the state. Each entry gets an owner in this order:

1. The user's claim in the request.
2. The owner the state learned when a collect or restore placed the save.
3. In managed mode, the game the volume was last swapped in for.
4. A table in the layout data mapping product codes to the save names each game writes. It is built offline by following SH-2 and 68000 code from each name to the BIOS call it reaches, filled out from shared save archives, and checked by playing the rest ([sega.md](save-research/sega.md) sections 3.2.1 and 3.2.2). Matching is by prefix, which covers names games build at runtime. The code scan alone found the right name for 17 of 39 Saturn games, and the Sega CD method is untested, so the table depends on the archives and play-testing to fill in. A name shared by several products can't be split by the table: the Japanese and US Virtua Fighter 2 both write `VFIGHTER2_X`, and three Wolf Team discs share `AISLE_LORD_`. Those entries fall through to the next step. The only existing name list, bucanero's, is GPLv3, so it is a reference for checking, not a source to copy. The table is `src/save_names.c`, seeded with the eight products whose code we read off a dump and whose save name a sample or the scan confirms; a name two products write needs both rows and then matches neither. Rows grow as samples arrive.
5. The user picks an owner. `collect` reports the entry in `unowned`, the client asks, and the answer goes in the next request's `claimed`.

Entries left unowned stay in the holding unit and are never deleted without having gone up in one.

genesis_plus_gx keeps one per-BIOS volume per region, `scd_E.brm`, `scd_U.brm` and `scd_J.brm`, and picks by the disc's region byte, or by `genesis_plus_gx_region_detect` when it's forced (`libretro.c` L1043-1060, L1574-1585). sigil doesn't read Sega CD discs, so it takes the forced option, then the content name's region tag, then the only one of the three files present.

### Internal operations

`collect` and `restore` are built from these. Only `list` is public.

These exist for PS1 and PS2 cards, GameCube raw cards, Dreamcast VMUs, and Saturn and Sega CD backup RAM: `sigil_card_list` is public in every binding, and extract, build, inject, delete and verify are internal in `src/card_*.c`. The single-save forms are `.mcs`, PS2 save folders, `.gci`, `.dci`, `.BUP`, and a sigil-internal Sega CD unit that keeps the stored ECC-encoded blocks. The public listing of picodrive's combined Sega CD file shows only its internal part today. PS2 also formats an 8 MB card byte for byte as mymc does, writes ECC, keeps the spare bytes of pages it didn't change (real cards carry erased and stale spares), packs and unpacks PCSX2 folder-card saves, and builds and checks a folder card's `_pcsx2_superblock`. Tests check all of them against the real samples in `tests/fixtures/saves/`, and `fuzz/` fuzzes them.

- Extract: split a card or volume into units by owner.
- Build: make a per-game card or volume from units, as described above.
- Inject: write a unit into a card or volume. It deletes only entries it knows are the game's, from their id on PS1, PS2 and GameCube cards, or from the owners the state records for Saturn, Sega CD and Dreamcast. It never deletes an entry whose owner is unknown. It fails before writing anything when the unit doesn't fit.
- Verify: read the card or volume back and compare each injected entry's bytes with the unit. Entries are compared by name, because block placement differs between volumes.
- List: report every entry on a card or volume for the card explorer. Each entry has its owner id or none, its raw name, the title it carries (the PS1 block header title, PS2 `icon.sys`, the GameCube comment, the Saturn comment), blocks used, timestamps where the format has them, and a flag for system entries. The listing also gives free blocks and free directory slots. Icons are left out of the first version. The explorer only reads, and changes go through inject.

### Card layout

Built cards are deterministic. The game's saves come first, then companions in the order given. Blocks are allocated from the first free one, and unused space holds what the format's own format writes: zero on PS1 and Saturn, `0xFF` on GameCube and in PS2 file-cluster tails. Card-level fields such as the format time and card serial take fixed values. Each save keeps its own timestamps and attributes, and a GameCube save keeps its copy counter; only its first-block field changes with placement.

F-Zero GX and PSO bind their GameCube saves to the card serial. Dolphin rewrites the bound bytes on import, and sigil does the same when it builds a card: inject leaves the save as it is, and the build step rebinds F-Zero GX afterwards. PSO rebinding waits for a sample to test it against.

A PCSX2 folder card keeps each save's timestamps and file order in `_pcsx2_index`, which AetherSX2, NetherSX2 and ARMSX2 also write. Packing folders into a card and unpacking them has to match PCSX2 exactly (`pcsx2/SIO/Memcard/MemoryCardFolder.cpp` at `2c804670`, cited in [sony.md](save-research/sony.md) section 2.2):

- The index holds `$ROOT` times for the folder and `order`, `timeCreated` and `timeModified` for each file. PCSX2 still reads the older `%ROOT` key.
- Card dates convert to Unix seconds as UTC, as PCSX2 does, so a round trip doesn't shift the time.
- `order` follows the card's directory entry order. Some games, GTA among them, break when it changes.
- `_pcsx2_meta/<file>` and `_pcsx2_meta_directory` hold raw directory entries and take precedence over the index. sigil writes them only for entries whose mode isn't PCSX2's default. Otherwise PCSX2 applies that default when it loads. PCSX2 reads a meta file of any length over the entry it would build, and sigil does the same. Two deliberate differences: a full meta file names the entry in PCSX2, while sigil keeps the host file's name so a folder packs back to the files it unpacked to; and a file the index doesn't list takes the host file's times in PCSX2, while sigil, which sees names but not times, gives it none. sigil orders such files as PCSX2 does, after the indexed ones in reverse listing order.
- `_pcsx2_superblock` at the card folder's root is the one card-level file. PCSX2 reads all 0x2000 bytes and treats the card as formatted only when byte 0x16 is `0x6F`, so a missing, empty or short file hides every save ([sony.md](save-research/sony.md) section 2.2). sigil builds it from the same superblock page `sigil_ps2_card_format` writes, zero-padded to the erase block, which is mymc's blank card's first block. A superblock that already passes the check stays as it is, since it may describe a larger card.
- The emulators don't agree on the index's YAML style. AetherSX2 writes block style and quotes some keys; ARMSX2 writes one line of flow style. PCSX2's YAML reader takes either, so sigil reads both and writes one fixed style. The index is never part of what RomM stores, since PS2 saves travel as cards.

`.vmp` and `.psv` are signed with an HMAC-SHA1 whose key comes from a seed in the header. sigil checks a `.vmp`'s signature when it reads one and signs it again under the same seed when it writes one. A `.psv` holds a single save and isn't a card, so sigil only reads it.

| Order | Container | Source for the format |
|---|---|---|
| 1 | PS2 `.ps2` card image, and PCSX2 folder cards | mymc (public domain) for the card. A Python adaptation in progress for RomM's memory card tab is the reference to test against. PCSX2's `MemoryCardFolder.cpp` for the folder form, read and not copied (GPL-3), checked against ARMSX2 for Android |
| 2 | PS1 card (`.mcr`, `.mcd`, `.srm`, DexDrive `.gme`, PSP and Vita `.vmp`) | MemcardRex and DuckStation for the documented layout; the `.vmp` signature and the `.gme` header copies checked against real files |
| 3 | Saturn and Sega CD backup RAM. Yaba Sanshiro keeps one `backup.bin` for every game with no option to split it | emulator source |
| 4 | Dreamcast VMU. redream shares its VMUs with no per-game option | VMS/VMI format documentation |
| 5 | GameCube raw card. Dolphin's default GCI folder mode is already per game, so only users who switched to raw cards need this | YAGCD and the Dolphin wiki (Dolphin's code is GPL) |
| 6 | PC Engine BRAM dumps from real hardware (every PCE emulator already keeps one BRAM per game), Neo Geo memory cards and Neo Geo CD save RAM. The per-game form is the PCE HUBM entries, or the Neo Geo entries tagged with the game's NGH number | HUBM entries; NGH-tagged card entries |
| 7 | 3DO NVRAM | Documented (the Opera linked-memory filesystem) but no existing tool |
| 8 | CD-i NVRAM, Jaguar Memory Track, PC Engine MB128 | Format unknown; synced whole until researched |

## 5. Disk images

- Read and write one title's saves inside xemu's qcow2 image and its FATX filesystem, at `E:\UDATA\<TitleID>` and, for titles that use it, `E:\TDATA\<TitleID>`.
- Non-roamable titles sign their saves with the console's HDKey from `eeprom.bin`. Their saves validate only on an install with the same EEPROM, so moving them needs re-signing or the same `eeprom.bin`.
- Port Argosy's Kotlin implementation (`data/sync/xbox/`: `Qcow2Image`, `Qcow2Writer`, `FatxVolume`, `XboxHddImage`), which already does this for its own sync.

Console identity files in those NANDs (3DS `movable.sed`, OTP, `SecureInfo`, friend code seed; Switch profiles; Cemu accounts) are a separate per-user layer and never part of a save unit. [user-identity.md](user-identity.md) holds the parked proposal.

Emulated NAND on Wii, Wii U, 3DS and Switch is already a folder per title on the host, so it needs no image support. Finding and creating those folders still takes work:

- Ryujinx names each save folder with an allocated id, not the title id. The title and user map to it through `bis/system/save/8000000000000000/0/imkvdb.arc`. A title that has never booted has no entry, and creating one also takes `ExtraData0` and `ExtraData1`.
- The yuzu forks name the user folder in the legacy layout with the profile UUID's bytes reversed.
- Cemu keys saves by the account's persistent id, and its `user/common/` folder belongs to the save too.
- A 3DS save needs its `.metadata` file beside it. The extdata ids a title uses don't follow from its title id in general (UNVERIFIED).

## 6. Sessions and modes

sigil can only pack what has reached disk. On Android, HOME pauses RetroArch and leaves the core loaded, and a low-memory kill runs no code at all ([android.md](save-research/android.md)).

### Managed mode

The client launches the game. It calls `restore` before launch and `collect` after the session ends.

- A session ends when the content closes or the process dies, never on HOME.
- Before calling `restore`, the client makes sure the previous content has closed. Otherwise RetroArch unloads the old core after the restore, and that unload writes the old volume over the new one.
- The client never launches RetroArch with the `QUITFOCUS` extra, because it exits without saving.
- genesis_plus_gx Sega CD BRAM, Kronos, Beetle Saturn `.smpc` and flycast VMUs reach disk only when the game unloads. If Android kills a backgrounded RetroArch, the last in-game save on those is lost, so the client tells users to exit the game before leaving the emulator.

### Unmanaged mode

The user plays outside the client, which only syncs. It calls `collect` on each sync pass and `restore` when RomM has something newer. sigil changes its behaviour for this mode:

- Cards with ids (PS1, PS2, GameCube raw) and folder saves sync in place. Inject works per entry or per folder and never touches other games' saves.
- Saturn and Sega CD volumes and Dreamcast VMUs are never swapped, because the next launch outside the client would lose every other game's saves. Entries with a known owner sync normally. Entries with no known owner wait in a holding unit until the user claims them. `collect` reports these platforms as partially synced, and the client shows that.
- `restore` injects only when the target hasn't changed since the previous `collect`. The next `collect` checks that the injected entries survived, since a core that unloads after the inject writes its in-memory copy over the file. When `collect` finds the entries the restore replaced back on disk, it sets `restore_again`. When the local entries match neither those nor the restored ones, the player changed them in the meantime: `collect` returns them as a changed unit, and the client decides which side wins.

## Open questions

- Where standalone Yaba Sanshiro on Android keeps `backup.bin`, and whether it sits in `Android/data`, where other apps can't reach it.
- Where ares keeps its Mega CD `backup.ram` on disk.

## When this lands

The README's "What it isn't" section says sigil is not a save manager. That stays true, since sigil still never syncs or stores anything, but the section should say what sigil now does with save files once conversion and containers ship.
