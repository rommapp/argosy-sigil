# Sigil agent guide

Sigil identifies games and moves their saves for RomM clients: it reads a
game's id from its ROM, finds the files an emulator keeps for it, and
collects and restores them as a unit that travels to RomM. This file is the
law for any coding agent working here. [CONTRIBUTING.md](CONTRIBUTING.md)
covers the human side and the smell list reviews cite. Each law states its
exception and where the exception ends.

## Laws

1. **Formats come from upstream, cited.** Every format fact (a save path, an
   offset, a footer field, an option key, a size table) is read from the
   emulator's or the console SDK's source and cited in the comment above the
   code, with the file and a commit or tag: `mc.cpp ensure`,
   `libretro.c at 5718ab9`. Never infer a format from another emulator's or
   from what a file looks like. Exception: a format no source documents may
   rest on real files, cited by their sample id and how they were checked.

2. **The layout row decides the format.** Where a file lives and what form it
   takes come from its row in `src/save_layout.c`. Code never branches on a
   file's name or extension to choose how to write it; two emulators can share
   an extension and disagree on its contents (DeSmuME and DraStic both write
   `.dsv`). Reading may detect a format by its content, the way the emulator
   does (a footer's cookie), so files written before sigil still read.

3. **A conversion writes what the target emulator writes.** Restore produces
   the file the target emulator would produce from the same save, and a round
   trip returns the save unchanged. Where that's impossible, the loss is
   listed under Open items on the platform page.

4. **Never guess.** When sigil can't establish a value, it reports nothing or
   refuses, rather than filling in the common case. A guessed value looks
   valid and sends saves to a folder the emulator never reads (a Wii disc's
   category, before it was read from the ticket). The filename fallback is the
   one sanctioned guess, and it says so in `source`.

5. **Sigil reports, the client decides.** No option chooses which save wins.
   A refusal names the file or member at fault in `problem`, and the client
   asks the user and calls again with the user's choice (`overwrite_local`,
   `claimed`, `repair`, `profile`). Sigil never writes over a save it can't
   read, and repair changes only what the card's own records prove.

6. **Units already on a server keep restoring.** A unit's shape (what it
   holds and how members are named) is a format other systems store. A change
   keeps restoring the old shape, with a test for it ("Units from before
   sigil" in `docs/sync.md`), and states what RomM must change to match.
   Sigil defines the unit; RomM follows it.

7. **One API, every binding.** A public C call or field ships with its
   Python (`bindings/python`), Go (`bindings/go`) and Kotlin/JNI
   (`bindings/android`) form in the same change, and with its row in
   `bindings/python/test_contract.py`, which fails when the bindings drift.

8. **One home per concept.** Before writing a derivation (a size from an
   option, a path from an id, a save's owner), find where it already lives
   and extend it. Two copies of one rule drift apart: the Dolphin card size
   once lived in two places, and new cards came out at the wrong size.

9. **Comments state facts, not history.** A comment says what upstream does
   and where, or what a function's contract is. It never narrates the change,
   argues for it, or mentions a ticket. Public declarations in
   `include/sigil.h` carry their contract in full.

## Definition of done: a new emulator or format

1. The upstream source is read and cited (law 1).
2. The row is in `src/save_layout.c`, given its platform when it serves one.
3. If the emulator stores a save differently from others on the platform,
   the conversion lives in the platform's sync file (`src/sync_gb.c`,
   `src/sync_n64.c`), the unit holds the neutral form, and restore writes the
   target's form (laws 2 and 3).
4. Tests:
   - a case in `tests/unit_save_unit.c` for the row's files;
   - a synthetic sync test CI runs, covering both directions;
   - real samples recorded in `tests/fixtures/saves/<platform>/manifest.tsv`
     (the files are never committed);
   - each test seen failing with the code broken on purpose.
5. Docs:
   - the platform page in `docs/platforms/`;
   - both tables in `docs/platforms/README.md`;
   - the tables in `docs/sync.md` for a converter;
   - the README table when a status changes.

   `bindings/python/test_docs.py` checks several of these.

## Structural index

- `include/sigil.h`: the public API and its contracts.
- `src/sigil.c`, `src/<platform>.c`: identification, dispatched by
  extension and header magic.
- `src/save_layout.c`: the layout rows. `src/save_unit.c`: resolving a
  request to its files, template variables and the options they read.
- `src/sync.c`: collect and restore, dispatching by platform and layout.
  - memory cards: `src/sync_cards.c` with `src/sync_kind_*.c` and `src/card_*.c`;
  - backup RAM volumes: `src/sync_volumes.c`;
  - profile and folder saves: `src/sync_profiles.c` and `src/save_profiles.c`;
  - converted cartridges: `src/sync_gb.c`, `src/sync_n64.c`, `src/sync_nds.c`,
    `src/sync_genesis.c`, sharing the save file and its name through
    `src/sync_cart.c`.
- `bindings/`: Python (cffi; `build_sigil.py` mirrors the header), Go (cgo),
  Kotlin (JNI in `bindings/android/src/main/cpp/sigil_jni.c`).
- `tests/`: `unit_*` (synthetic, always run), `integration_*` (real samples
  or ROMs, skip when absent), `fuzz/`.
- `docs/`: `sync.md` (collect, restore, refusals), `save-units.md` (units,
  hashes, template variables), `platforms/<slug>.md` (per-platform facts,
  with a status line and Open items).

## Build and test

`docs/building.md` has the full instructions. The short form:

```sh
cmake -B build -S . && cmake --build build && ctest --test-dir build
```

The Go and Python binding tests run inside `ctest` when their toolchains
are present. Real-sample and ROM tests skip unless `SIGIL_ROM_DIR` or the
samples are present; run them locally for any change they cover.
