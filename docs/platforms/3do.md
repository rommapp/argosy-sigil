# 3DO

Status: located
`sigil_save_resolve()` names the NVRAM image Opera keeps for one game; collect and restore return `SIGIL_ERR_UNSUPPORTED_FORMAT` for it.

## Save layouts

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| `opera` | `opera/per_game/{stem}.{nvram_version}.srm` primary when `opera_nvram_storage` = `per game` (default) | `opera/shared/nvram.{nvram_version}.srm` when `shared` | emulator source |

`{nvram_version}` is `opera_nvram_version` (default `0`). The layout
writes into `opera/per_game` and `opera/shared`, which
`sigil_save_layout_subdirs()` names. See [Save units](../save-units.md)
for how rows expand.
