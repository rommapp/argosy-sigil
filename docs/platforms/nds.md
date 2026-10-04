# Nintendo DS

Status: located
Sigil names the file melonDS keeps for a cart. It reads no DS header, and collect and restore don't cover these carts.

## Save layouts

Row from the [layout table](../save-units.md).

| Layout | Files (role, option) | Shared | Source |
|---|---|---|---|
| `melonds` | `{stem}.sav` primary | | legacy `libretro.cpp`, flushed on the core's own timer |

## Emulator research

Scope: battery/flash/EEPROM saves and RTC data, not save states. Researched 2026-09-26 against the default branch of each repo (shallow clones). Line numbers refer to those heads and will drift; the function names are the stable anchor.

`{stem}` = content filename without extension. "RA" = RetroArch frontend. RetroArch's own behaviour is in [RetroArch frontend behaviour](gb.md#retroarch-frontend-behaviour-applies-to-every-libretro-core-that-exposes-memory).

### Emulators

| Platform | Emulator | Files and naming | Format | Scope | Options / versions that change files | Lossless conversion | Source |
|---|---|---|---|---|---|---|---|
| NDS | DeSmuME (standalone) | `{stem}.dsv` (Battery path). Falls back to importing `{stem}.sav` if there is no `.dsv`. Keeps a `.dsv.bak`. | **Raw + 122-byte footer**: 82 ASCII bytes `\|<--Snip above here to create a raw sav by excluding this DeSmuME savedata footer:`, then 6 x u32 LE (`size` actually written, `padSize`, `type`, `addr_size`, `mem_size`, `version`=0), then the 16-byte cookie `\|-DESMUME SAVE-\|`. The data part is padded to `padSize` (a power-of-two save size). | per-game | none | To raw `.sav`: drop the last 122 B, keeping `padSize` bytes. From raw: append the footer with fields from the size table. Lossless. | [mc.cpp L72-73, L234-281, `ensure` L943-980](https://github.com/TASEmulators/desmume/blob/master/desmume/src/mc.cpp#L943-L980), [mc.h L59-75](https://github.com/TASEmulators/desmume/blob/master/desmume/src/mc.h#L59-L75) |
| NDS | desmume (libretro) | Core-written `{stem}.dsv` in the RA save dir (`path.BATTERY` set from `GET_SAVE_DIRECTORY`). No `.srm`: `retro_get_memory_size` returns 0 for SAVE_RAM. | Same 122 B footer format | per-game | none | As standalone | [libretro/desmume path.cpp L199](https://github.com/libretro/desmume/blob/master/desmume/src/path.cpp#L199), [mc.cpp L234-235](https://github.com/libretro/desmume/blob/master/desmume/src/mc.cpp#L234), [frontend/libretro/libretro.cpp L2473-2488](https://github.com/libretro/desmume/blob/master/desmume/src/frontend/libretro/libretro.cpp#L2473-L2488) |
| NDS | melonDS (standalone) | `{stem}.sav` (next to the ROM or in `SaveFilePath`). GBA slot-2 cart: `{gbastem}.sav`. DSiWare saves live inside the NAND image; the Title Manager imports/exports `public.sav`/`private.sav`/`banner.sav`. | Raw, exact chip size, no footer | per-game (DSiWare: in the shared NAND container) | `SaveFilePath` | Equal to the DeSmuME data portion. To DeSmuME: append the footer. | [EmuInstance.cpp L1886, L2045](https://github.com/melonDS-emu/melonDS/blob/master/src/frontend/qt_sdl/EmuInstance.cpp#L1886), [Config.cpp L302](https://github.com/melonDS-emu/melonDS/blob/master/src/frontend/qt_sdl/Config.cpp#L302) |
| NDS | melonds (old libretro) | Core-written `{stem}.sav` in the RA save dir. GBA slot: `{gbastem}.srm`. | Raw | per-game | none | Same bytes as melonDS standalone | [libretro.cpp L895, L915](https://github.com/libretro/melonDS/blob/master/src/libretro/libretro.cpp#L895-L915) |
| NDS | melondsds (libretro) | `{stem}.srm` via RA (NDS SRAM). DSiWare: core-written `{stem}.public.sav`, `.private.sav`, `.banner.sav` in the RA save dir. GBA slot save is flushed by the core (path UNVERIFIED). | Raw | per-game | none | `.srm` = melonDS `.sav` bytes | [core.cpp L937-962](https://github.com/JesseTG/melonds-ds/blob/main/src/libretro/core/core.cpp#L937-L962), [config/console.cpp L925-957](https://github.com/JesseTG/melonds-ds/blob/main/src/libretro/config/console.cpp#L925-L957), [sram.cpp L133-153](https://github.com/JesseTG/melonds-ds/blob/main/src/libretro/sram.cpp#L133-L153) |
| NDS | DraStic (Android) | `DraStic/backup/{stem}.dsv` | Community reports say it is **raw, full power-of-two size, no footer**; DeSmuME rejects it until the footer is appended. UNVERIFIED for current DraStic versions (a forum thread titled "Save Format Toggle" exists). | per-game | UNVERIFIED | Detect by the trailing cookie `\|-DESMUME SAVE-\|`. If absent, treat as raw; this is lossless both ways. | [DeSmuME forum guide](https://www.forums.desmume.org/viewtopic.php?id=10341), [DraStic forum t=4691](https://drastic-ds.com/viewtopic.php?t=4691) (both community sources; UNVERIFIED) |

### Conversion notes

- **NDS**: raw data plus the optional 122 B DeSmuME footer, detected by the trailing `|-DESMUME SAVE-|`. melonDS, melondsds, old melonDS-lr and (reportedly) DraStic are raw.

### User profiles

- **verified**: no per-user saves. Wii saves are per title in the NAND, NDS saves per cart. Nothing to split.

## Open items

- DraStic current-version footer behaviour, and whether an option toggles it.
- melondsds GBA-slot save path.
