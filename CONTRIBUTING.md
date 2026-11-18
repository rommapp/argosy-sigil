# Contributing to sigil

PRs are welcome, including AI-assisted ones. What matters is that you've
reviewed what you submit and understand it.

The rules for coding agents, and the definition of done for a new emulator
or format, live in [AGENTS.md](AGENTS.md). Point your tooling at it. This
file covers what a reviewer looks for.

## Evidence

Sigil's correctness is about real files, so a PR that changes a format or a
sync path needs more than passing tests:

- **The upstream source.** Which emulator source the format comes from,
  with a file and commit or tag.
- **Real files.** Saves written by the emulators involved, recorded as
  sample rows in `tests/fixtures/saves/<platform>/manifest.tsv`. See
  [tests/fixtures/saves/README.md](tests/fixtures/saves/README.md). Don't
  commit the files themselves.
- **A round trip.** For a conversion, the save loaded in each emulator after
  crossing, and what the game showed.
- **A test that failed.** What you broke on purpose to see the new test fail.

## Smells

Reviews cite these by number. They're the patterns that cost a review cycle.

- **S-1 Format by file name.** Choosing how to write a file from its name or
  extension instead of its layout row. Two emulators can share an extension
  and disagree on the contents.
- **S-2 Analogized format.** A format inferred from another emulator, or from
  what files look like, without a source citation.
- **S-3 Undeclared loss.** A conversion that doesn't round-trip, or doesn't
  write what the target emulator writes, without saying so on the platform
  page.
- **S-4 Silent guess.** Filling in the common case for a value sigil can't
  establish. It looks valid, so nothing flags it, and saves land where the
  emulator never looks.
- **S-5 Second copy.** A new derivation beside an existing one for the same
  concept. The two drift.
- **S-6 Self-verified test.** A test whose expected value came from sigil's
  own output, or one never seen failing.
- **S-7 Binding drift.** A C API change without its Python, Go and Kotlin
  forms and its `test_contract.py` row.
- **S-8 Unit break.** A change to what a unit holds without restoring the old
  shape, or without saying what RomM must change.

## RomM

Sigil defines the units RomM stores, and RomM follows sigil's format. If
your change adds or alters a unit, say in the PR what RomM needs to change,
and link the RomM PR when there is one.

## AI assistance

If you used AI, say so in the PR and how much it did. It only tells the
reviewer where to focus.

## Process

- Fill in the PR template.
- PR titles are lowercase and scope-prefixed: `nds: ...`, `sync: ...`,
  `docs: ...`.
- Squash-merge is the house style.
