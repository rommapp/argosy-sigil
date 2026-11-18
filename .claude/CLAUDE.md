# sigil

@../AGENTS.md is the law of this project: read it as binding before changing
code. This file adds only Claude-specific behavior.

- A change to a save format or a sync path is done when a real file has been
  through it, not when the code reads right. Record the samples you used as
  manifest rows and say what you checked.
- Fetch the upstream emulator source before writing a format fact, and cite
  the commit you read. Don't answer a format question from memory.
- Before opening a PR, fill in `.github/PULL_REQUEST_TEMPLATE.md` and check
  the diff against the smells in `CONTRIBUTING.md`.
