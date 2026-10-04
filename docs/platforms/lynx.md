# Atari Lynx

Status: located
`sigil_save_resolve()` names the EEPROM file Handy keeps for one game; collect and restore return `SIGIL_ERR_UNSUPPORTED_FORMAT` for it.

## Save layouts

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| `handy` | `{stem}.eeprom` primary | | emulator source |

Handy writes the EEPROM when the core unloads. See
[Save units](../save-units.md) for how rows expand.
