# DOS

Status: located
`sigil_save_resolve()` names the `.pure.zip` DOSBox Pure keeps for one content file; collect and restore return `SIGIL_ERR_UNSUPPORTED_FORMAT` for it.

## Save layouts

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| `dosbox_pure` | `{stem}.pure.zip` primary | | emulator source |

DOSBox Pure keeps one zip per content file, holding every file the game
created or changed on its C: drive plus a `FILEMODS.DBP` list of
deletions and renames. It travels as one file. A `.pure.zip` is one raw
member and hashes as a zip because the server does the same. See
[Save units](../save-units.md) for how rows expand and hash.

## Open items

- The row has no member for a `.SAVENAME` redirect or the legacy `<content name>.sav`.
