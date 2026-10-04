# Pokemon Mini

Status: located
Sigil names the file the PokeMini core keeps for a cart. It reads no Pokemon Mini header, and collect and restore don't cover these carts.

## Save layouts

Row from the [layout table](../save-units.md).

| Layout | Files (role, option) | Shared | Source |
|---|---|---|---|
| `pokemini` | `{stem}.eep` primary | | `libretro.c`, written at unload |
