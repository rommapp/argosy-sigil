# Philips CD-i

Status: located
`sigil_save_resolve()` names the NVRAM folder SAME CDi keeps for one disc; collect and restore return `SIGIL_ERR_UNSUPPORTED_FORMAT` for it.

## Save layouts

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| `same_cdi` | `same_cdi/nvram/{stem}/` primary (folder) when `same_cdi_nvram_saves` = `enabled` (default) | | emulator source |

The folder holds the 8 KiB timekeeper NVRAM (`cdimono1/mk48t08`), clock
registers included; sync it whole. The layout writes into
`same_cdi/nvram`, which `sigil_save_layout_subdirs()` names. See
[Save units](../save-units.md) for how rows expand and how folder
members archive.

## Open items

- No row for the shared NVRAM (`same_cdi_nvram_saves` = `disabled`).
