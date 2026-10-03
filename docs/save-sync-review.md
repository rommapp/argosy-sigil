# Save sync test review

Six reviewers mutation-tested the sync and card tests at `52fbb39`, one per platform. This file triages every finding and tracks it to closure. A finding closes when a test fails against the unfixed code (or the mutation that survived), then passes. Update the status column as work lands.

Kinds: **bug** is wrong behavior today. **gap** is right behavior no test pins. **decision** needs a call before work. **clean** is a structure or dead-code fix. **drop** is not a finding, with the reason.

## Order of work

1. D6 refactor: split `src/sync.c`, fold duplicated helpers, no behavior change. Full suite, ASan, bindings green. Own commit.
2. Test harness: `mem_root` faulty-write mode (X1), missing samples fail once a manifest loads (X2).
3. API changes in one pass across C and bindings: `problem` (renamed `overflow`), `SIGIL_ERR_DAMAGED`, `SIGIL_ERR_REGION`, request `repair`.
4. Bugs and decided behavior, test first: D2 (P2-7), D3 (SA-9, SC-7), D4, D1 pin (SA-2).
5. Gaps by platform: PS1, PS2, Saturn, Sega CD, Dreamcast, GameCube, then cross-platform X3-X8.
6. D5: SHA-1/HMAC, `.vmp` and `.gme` writing, `vita_pops` row, compliance tests.
7. Re-run the six reviewers on the result; anything they still find goes back in this file.

## Decisions

| ID | Question | Recommendation | Status |
|---|---|---|---|
| D1 | After an unmanaged restore, the core overwrites the restored saves and the player then plays on, so the local saves match neither side. The roadmap says this comes back as a conflict; `finish()` uploads it as a change, overwriting the remote progress the restore brought. | Decided: sigil finds and places; the client decides which save wins. Collect reports facts only (`changed`, `restore_again`) and never a conflict. Pin the diverged case (local matches neither the restored nor the replaced saves: `changed = 1`, `restore_again = 0`, the unit is the local saves) and correct `docs/save-roadmap.md:246`. Restore's `SIGIL_ERR_CONFLICT` stays: it writes nothing and the client's `overwrite_local` decides. | decided |
| D2 | A PCSX2 save folder whose `_pcsx2_index` doesn't parse is skipped today, so restore overwrites it with no conflict. | Decided: damaged structures sigil can rebuild return `SIGIL_ERR_DAMAGED`, writing nothing, with the result naming the file; the client re-runs with a request flag `repair`. With `repair`: collect packs a folder whose `_pcsx2_index` doesn't parse without it (listing order, default times); restore counts that folder as the game's local save for the conflict rule and writes the unit's folder with a fresh index; restore writes a formatted `_pcsx2_superblock` over an unusable one on a card that holds save folders. A folder card with no save folders and no usable superblock is a new card: restore creates the superblock without `repair`. The result's `overflow` buffer becomes a general `problem` name, used by NO_SPACE and DAMAGED. `repair` and DAMAGED are generic so P1-6 and SC-9 can use them. All bindings. | decided |
| D3 | A unit's cart volume size differs from the emulator's configured cart (Sega CD `cart_size`, Kronos `kronos_addon_cartridge`). Today the unit's size is written under the configured name. | Decided: restore builds the cart the emulator's option names (size and block size) and injects the unit's saves; when they don't fit, `SIGIL_ERR_NO_SPACE` with `problem` and the blocks short, writing nothing. The client triages: change the core's cart option and re-run with it in `options` (sigil builds the larger cart), or restore a unit with fewer saves. sigil never writes a cart of a size the option doesn't name. Layout rows carry a size per option value. | decided |
| D4 | A GameCube companion from another region returns `SIGIL_ERR_INVALID_ARG`, which a client can't tell apart from a bad request. | Decided: add `SIGIL_ERR_REGION`, writing nothing, with `problem` naming the companion save, across C and all bindings, since the placement would fail the user's expectation (the game can't read it). | decided |
| D5 | PS1 `.gme` and `.vmp` cards are listed as read-only, but `ps1_writable` is the guard and nothing tests it. Restore to one refuses or converts? | Research done (prototype `scratchpad/gmevmp/proto.py`, reproduced both `.vmp` and the `.psv` stored signatures exactly). `.vmp`: 0x80 header (`\0PMV`, seed at 0x0C, HMAC-SHA1 at 0x20) over the whole file with the signature zeroed; key from the seed through AES-128 with public constants; any seed accepted, no console binding; keep the seed and re-sign. Live target: Vita/PSTV and Adrenaline `ux0:pspemu/PSP/SAVEDATA/<TITLE…>/SCEVMC0.VMP` (slot 1) and `SCEVMC1.VMP`. `.gme`: 0xF40 header; on write, refresh the per-slot state and link copies (0x16+i, 0x26+i; deleted frames as 0xA0/0xFF) and keep a slot's comment only while the same save holds it; a short file pads to the full card. No emulator uses `.gme` live. Also found: sigil doesn't check a `.vmp` signature on load. Proposal: write both; verify the `.vmp` signature on load and return `SIGIL_ERR_DAMAGED` on a mismatch (D2's `repair` re-signs); SHA-1 and AES-128 decrypt added to sigil; a Vita layout row. Decided: all four (write both; verify `.vmp` on load with DAMAGED/`repair`; SHA-1 and HMAC-SHA1 from FIPS 180-4; `vita_pops` row). Compliance tests: FIPS 180-4 and RFC 2202 vectors; both `.vmp` samples verify, and re-signing an unchanged card reproduces the stored bytes exactly; after a restore the header keeps bytes 0x00-0x1F and the seed, the size stays 0x20080, and the new signature verifies; a flipped byte reads DAMAGED and `repair` re-signs. `.gme`: every header byte outside the frame copies and comments is kept; the copies match the directory by the DexDrive rule (deleted as 0xA0/0xFF); a comment stays only with its save; the short file writes back full size; read(write(card)) equals the card. | decided |
| D6 | `src/sync.c` is 2717 lines: state, six card kinds, request, saves, units, card path, PCSX2 folder cards, volume path, GCI folder, collect, restore. | Split by responsibility behind `src/sync_internal.h`: `sync_state.c`, `sync_kinds.c` (one kind table per platform), `sync_units.c`, `sync_cards.c` (with folder cards), `sync_volumes.c`, `sync_folders.c`, and `sync.c` keeping collect/restore. Do it first, test-neutral, so the new tests land in the final layout. | done: `sync_internal.h`; `sync.c` (collect, restore, conflict rule), `sync_state.c`, `sync_kinds.c` plus `sync_kind_sony.c`/`_sega.c`/`_gamecube.c`, `sync_units.c`, `sync_cards.c`, `sync_folder_cards.c`, `sync_volumes.c`, `sync_gci_folder.c`. Cross-file symbols are `sigil_sync_*`. Folded: one file reader (`sigil_sync_read_file`, `_file_holds`), `_listed`, one zip-unit and single-unit builder, `_copy_image`, owner state accessors, one `blocks_restore` for game and companions, designated kind tables. Suite, ASan, Python, Go, contract, fuzz build and AAR green. |

## Cross-platform

| ID | Kind | Finding | Status |
|---|---|---|---|
| X1 | gap | `mem_root` writes never fail or corrupt, so verify-after-write is unpinned everywhere (PS1 `sigil_ps1_verify` memcmp, `write_and_verify`, folder read-backs, Dreamcast header-only verify). Add a faulty-write mode to `mem_root` and a test per path. | harness done (`fail_write`, `corrupt_write`); per-path tests land with each platform | 
| X2 | gap | Silent skips: checks `return` when a sample or setup is missing, and each `main` probes one sample. Make missing samples a `fail()` once the platform's manifest loaded; keep exit 77 only when the platform's files are absent. | done: one loader `corpus_sample` in `tests/save_corpus.h` counts misses (the seven per-test copies removed), card tests check every manifest file with `corpus_count_missing`, `corpus_exit` fails the run; both paths watched failing |
| X3 | gap | Option values tested only at defaults across layouts (flycast, PerGameVmu, SlotA, pcsx_rearmed_memcard2, beetle shared names, cart_size). Table-driven layout tests in `unit_save_unit.c` for every row and option value; it has no PS2, GameCube or Dreamcast cases today. | open |
| X4 | gap | Owner precedence pinned only against "no owner". Test each adjacent pair in both orders: claim > learned > per-game file > prepared > name table. Includes "learned owner on a per-game file only counts for companions". | open |
| X5 | gap | Conflict rule arrangements: equal case (local changed since sync but equals incoming), emptied-after-sync, state present and changed, for game and companion. | open |
| X6 | gap | Fuzzers: Saturn sync kind never fuzzed; shared volumes never fuzzed; no oracles. Add Saturn and shared modes, and assert collect-after-restore identity where defined. Fuzzers stay out of ctest (they need the fuzz build); document how to run them. | open |
| X7 | clean | Python and Go folder tests compare an identity restore copies from the unit. Re-collect and compare instead. | open |
| X8 | clean | Python and Go tests not in ctest. Add ctest entries that run them when the toolchain exists. | open |

## PS1

| ID | Kind | Finding | Status |
|---|---|---|---|
| P1-1 | gap | Companion conflict rule (`sync.c` check_restore companion loop) deletable. | open |
| P1-2 | gap | `already_there` ignoring companions survives: game unchanged, companion unit newer writes nothing. | open |
| P1-3 | gap | `clear_game` skipping a restored companion's old saves survives; every companion restore targets an empty root. | open |
| P1-4 | gap | Equal case and emptied-after-sync conflict branches (see X5). | open |
| P1-5 | gap | `.gme`/`.vmp` writable guard (D5). | open |
| P1-6 | gap | A unit holding a corrupt chain isn't refused in any test. | open |
| P1-7 | gap | `walk_chain` state check and link range check unpinned; the range check passes by reading 2.4 MB past the image. Add fixtures with in-range links to free/foreign blocks and an out-of-range link. | open |
| P1-8 | gap | Overflow name and blocks pinned only in Python/Go; pin in C. | open |
| P1-9 | gap | Slot-2 placement memory, other games' saves in a unit, NOT_FOUND unit: caught only by PS2 tests. Add PS1 cases. | open |
| P1-10 | gap | `owned_by_game` via `title_id` alone: C tests always put the title in `game_ids`. | open |
| P1-11 | gap | `card_load` accepts a raw "MC" file over 128 KiB; inject accepts a `.mcs` whose state isn't 0x51. Decide by format spec, then pin. | open |
| P1-12 | gap | `overwrite_local` never set in `integration_sync_ps1.c`; conflict only with `state = NULL`. Companion overwrite check counts saves, not bytes. | open |
| P1-13 | gap | Public vs internal listing compares only `entry_count`. | open |

## PS2

| ID | Kind | Finding | Status |
|---|---|---|---|
| P2-1 | gap | ECC on changed pages: `mark_cluster_dirty` no-op survives. Check ECC of every page after inject and delete on a loaded card. | open |
| P2-2 | gap | memcardFilters: `ACE_IDS` unused; ownership by extra ids untested, in file and folder cards. | open |
| P2-3 | gap | `_pcsx2_meta/<file>` emit and read both unpinned; modes lost silently. Pin modes through unpack/pack and through a folder-card restore. | open |
| P2-4 | gap | Timestamps: the file modified time isn't varied in "times are not a change". | open |
| P2-5 | gap | `$ROOT` folder times dropped stays green; `%ROOT` has no case. | open |
| P2-6 | gap | Folder card: only-differing-files writes, superblock-first order, refuse-before-any-write across two slots. | open |
| P2-7 | bug | Unparseable `_pcsx2_index` (D2). | open |
| P2-8 | gap | Companions on folder cards: filter includes them, unrestored ones aren't rewritten. | open |
| P2-9 | gap | Equal case conflict on PS2 (X5). | open |
| P2-10 | gap | DATA-SYSTEM/BWNETCNF in the space filter, Mcd002-only folder card, pack(unpack) guard. | open |
| P2-11 | gap | `fuzz_card_ps2` has no oracle; assert pack(unpack) identity. | open |
| P2-12 | clean | `integration_sync_ps2.c:615` two statements on one line. | open |

## Saturn

| ID | Kind | Finding | Status |
|---|---|---|---|
| SA-1 | gap | `check_swappable` held-hash compare droppable: an unowned save changed after its holding unit went up is swapped away. | open |
| SA-2 | gap | Unmanaged local change after restore (D1, not a bug); `restore_again` partial check droppable. | open |
| SA-3 | gap | Holding/game unit size fallback over 32 KiB untested; the 685-block TGKRPLY_RP1 sample exists. | open |
| SA-4 | gap | Managed companion re-inject on a shared volume untested. | open |
| SA-5 | gap | `check_unchanged` seen-hash compare droppable. | open |
| SA-6 | gap | Owner precedence pairs (X4). | open |
| SA-7 | gap | No Saturn unmanaged test; `note_restored` skipping `seen` survives. | open |
| SA-8 | gap | Holding unit keeps the source volume's size: unpinned. | open |
| SA-9 | bug | Kronos unit cart size vs configured cart (D3). | open |
| SA-10 | gap | Existing file's expanded form must come from the file, not the layout. | open |
| SA-11 | gap | 4 MiB cart 1024-byte blocks pinned only by a constant; add a sync round trip on a formatted 4 MiB cart. | open |
| SA-12 | gap | Verify ignores the archive date; prepared honored in unmanaged mode. Decide by spec, then pin. | open |

## Sega CD

| ID | Kind | Finding | Status |
|---|---|---|---|
| SC-1 | gap | `check_swappable` held compare (shared with SA-1). | open |
| SC-2 | gap | `remove_game_saves` no-op or single-removal survives: unmanaged restore with the game's saves already on a shared volume. | open |
| SC-3 | gap | `check_unchanged` (shared with SA-5). | open |
| SC-4 | gap | Claim over learned owner (X4). | open |
| SC-5 | gap | New volume ignoring the source's size and form on swap. | open |
| SC-6 | gap | Region: `pal`/`ntsc-u`/Europe values, scd_E tag, "(USA, Europe)", NOT_FOUND with no region and no file, which file Lunar targets. | open |
| SC-7 | bug | Cart volumes: no sync test; `cart_size` mapping tested at default only; restore ignores the configured size (D3). | open |
| SC-8 | gap | Inject exact fit: directory-block cost and `data_end` guard. | open |
| SC-9 | gap | "AI " prefix in the name table; reading one directory count copy; second CRC copy; zeroing freed blocks. | open |
| SC-10 | gap | Claim test never checks the holding unit drops the claimed save. | open |

## Dreamcast

| ID | Kind | Finding | Status |
|---|---|---|---|
| DC-1 | gap | `vmu_identity` replaceable by a constant: no data-change or timestamp-only re-collect. | open |
| DC-2 | gap | `{dc_vmu_id}` file must win over a legacy `{stem}` file when both exist (`save_unit.c` first-present rule, all layouts). | open |
| DC-3 | gap | Zip member names and port mapping: check member names and bytes per port, not a symmetric round trip. | open |
| DC-4 | gap | Option values (X3); a B1 per-game row under "VMU A1" contradicts README. | open |
| DC-5 | gap | Legacy names `{stem}_vmu_save_A1.bin` and `{stem}.A1.bin` under "All VMUs". | open |
| DC-6 | gap | `sigil_dreamcast_verify` header-only compare survives (X1). | open |
| DC-7 | gap | Standalone per-game A1 beside a shared B1. | open |
| DC-8 | gap | Empty title_id fallback to `{stem}`, every sanitized character, the T1219N row. | open |
| DC-9 | clean | `GAME_ID` "T-13301N" in the sync test isn't the real product number T13301N. | open |
| DC-10 | gap | No unmanaged Dreamcast test. | open |

## GameCube

| ID | Kind | Finding | Status |
|---|---|---|---|
| GC-1 | gap | Digit suffix when another game's file has Dolphin's name: data loss when broken. | open |
| GC-2 | gap | A restored companion's files the unit lacks are removed. | open |
| GC-3 | gap | New raw card size (2043 blocks) unchecked; `.1019/.507/.251/.123/.59.raw` names unused anywhere. | open |
| GC-4 | gap | Region mapping E/J/K/other and the title_id fallback; Shift-JIS form for a new Japanese raw card. | open |
| GC-5 | gap | Foreign check tested only USA game with JAP companion. | open |
| GC-6 | gap | `folder_files` `.gci` and no-subfolder filters. | open |
| GC-7 | gap | Identity masks: mtime and copy counter never varied. | open |
| GC-8 | gap | Escaping: control and other illegal characters. | open |
| GC-9 | gap | Maker code in `gc_save_key`; explicit `SlotA = 8`. | open |
| GC-10 | clean | `gc_file_name` all-dots branch is dead: the name always starts with `maker-gamecode-`. Delete it and its comment. | done in the D6 refactor |
| GC-11 | gap | Raw-card unit restored into a GCI folder (docs/c.md says either way). Companion in raw mode. | open |

## Drop

None yet. A finding moves here only with the reason it isn't real.
