# Arcade (FBNeo, MAME 2003-Plus, current MAME)

Status: located
`sigil_save_resolve()` names the files FBNeo and MAME 2003-Plus keep for one romset; collect and restore return `SIGIL_ERR_UNSUPPORTED_FORMAT` for them.

## Save layouts

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| `fbneo` | `fbneo/{romset}.fs` primary; `fbneo/{romset}.nv` sidecar; `fbneo/{romset}.memcard` sidecar when `fbneo-memcard-mode` = `per-game` | `fbneo/shared.memcard` when `shared` | emulator source |
| `mame2003_plus` | `mame2003-plus/nvram/{romset}.nv` primary and `mame2003-plus/hi/{romset}.hi` sidecar when `mame2003-plus_core_save_subfolder` = `enabled` (default); `nvram/{romset}.nv` and `hi/{romset}.hi` when `disabled` | | emulator source |

`{romset}` (same as the stem). The `fbneo-memcard-mode` values are the
option's English values; FBNeo localizes the option's labels. The
layouts write into `fbneo`, `mame2003-plus/nvram`, `mame2003-plus/hi`,
`nvram` and `hi`, which `sigil_save_layout_subdirs()` names. See
[Save units](../save-units.md) for how rows expand.

Arcade saves are NVRAM and EEPROM contents, hiscores and Neo Geo memory
cards, keyed by romset, so each is per game. Their layouts are
driver-specific and don't carry over between FBNeo, mame2003-plus and
MAME. Current MAME keeps hiscores in its system folder, not the save
folder.

Neo Geo carts, cards and the Neo Geo CD are on [Neo Geo](neogeo.md).

## Open items

- No row for current MAME (libretro `mame` or standalone).
- The `fbneo` row has no `.hi` member, and the `mame2003_plus` row has no `memcard/MEMCARD.<NNN>` member, though both cores write them.
