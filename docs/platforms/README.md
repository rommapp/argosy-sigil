# Platforms

One page per system. Each says how sigil identifies a game, which emulator
layouts it knows and how each was verified, what sync does there, and
what isn't covered yet.

Status:

- **synced**: collect and restore move the saves ([sync.md](../sync.md)).
- **located**: `sigil_save_resolve` names the save files ([save-units.md](../save-units.md)); collect and restore don't cover the system yet.
- **in development**: sigil may identify the game, but no layout names the saves yet.

## Systems

| System | Page | Slug | Reads the id | Status |
|---|---|---|---|---|
| PlayStation | [psx](psx.md) | `psx` | yes | synced |
| PlayStation 2 | [ps2](ps2.md) | `ps2` | yes | synced |
| PSP | [psp](psp.md) | `psp` | yes | in development |
| PS Vita | [psvita](psvita.md) | `psvita` | yes | synced |
| PlayStation 3 | [ps3](ps3.md) | `ps3` | yes | synced |
| Game Boy, Game Boy Color | [gb](gb.md) | `gb`, `gbc` | header facts | located |
| Game Boy Advance | [gba](gba.md) | | no | located |
| Super Nintendo | [snes](snes.md) | `snes` | header facts | located |
| NES, Famicom Disk System | [fds](fds.md) | `fds` (layout rows) | no | located |
| Nintendo DS | [nds](nds.md) | | no | located |
| Nintendo 3DS | [3ds](3ds.md) | `3ds` | yes | in development |
| Nintendo 64 | [n64](n64.md) | | no | in development |
| Pokémon Mini | [pokemini](pokemini.md) | | no | located |
| GameCube | [gamecube](gamecube.md) | `gamecube` | yes | synced |
| Wii | [wii](wii.md) | `wii` | yes | in development |
| Wii U | [wiiu](wiiu.md) | `wiiu` | yes | synced |
| Switch | [switch](switch.md) | `switch` | yes | synced |
| Mega Drive, 32X, Master System, Game Gear | [genesis](genesis.md) | | no | located |
| Sega CD | [segacd](segacd.md) | `segacd` (layout rows) | no | synced |
| Saturn | [saturn](saturn.md) | `saturn` (layout rows) | no | synced |
| Dreamcast | [dreamcast](dreamcast.md) | `dreamcast` | yes | synced |
| Xbox | [xbox](xbox.md) | `xbox` | yes | in development |
| Xbox 360 | [xbox360](xbox360.md) | `xbox360` | yes | in development |
| Arcade (FBNeo, MAME 2003-Plus) | [arcade](arcade.md) | | no | located |
| Neo Geo | [neogeo](neogeo.md) | | no | located |
| Neo Geo Pocket | [ngp](ngp.md) | | no | located |
| Atari Lynx | [lynx](lynx.md) | | no | located |
| Atari Jaguar | [jaguar](jaguar.md) | | no | located |
| PC Engine, TurboGrafx-16 | [pce](pce.md) | | no | located |
| WonderSwan | [wonderswan](wonderswan.md) | | no | located |
| 3DO | [3do](3do.md) | | no | located |
| DOS | [dos](dos.md) | | no | located |
| CD-i | [cdi](cdi.md) | | no | located |

## Layouts

Every call that finds or moves saves takes a layout id: the `core`
argument in the bindings, `layout` in C. It names the emulator running
the game, because each emulator keeps its saves differently.

- For a libretro core, pass the core's library name without `_libretro`:
  `genesis_plus_gx` for `genesis_plus_gx_libretro.so`.
- For a standalone emulator, pass its id from the table, such as
  `dolphin_standalone`, `pcsx2_standalone` or `eden`.

An id with no row here gets the libretro default row (`{stem}.srm`, plus
`{stem}.rtc` when the cart has a clock), documented on
[gb](gb.md#save-layouts). That fits a libretro core with no row of its
own. It does not fit a standalone emulator missing from the table: the
default row names a `.srm` the emulator never writes, so locate finds
nothing and restore writes a file the emulator doesn't read.

Every layout id `src/save_layout.c` defines, and the page that documents
it. A layout id without a row here, or missing from its page's Save layouts
section, fails `bindings/python/test_docs.py`.

| Layout | Page | Emulator | Kind |
|---|---|---|---|
| `vba_next` | [gba](gba.md) | VBA Next | libretro core |
| `gpsp` | [gba](gba.md) | gpSP | libretro core |
| `bsnes` | [snes](snes.md) | bsnes | libretro core |
| `genesis_plus_gx` | [segacd](segacd.md) | Genesis Plus GX, Sega CD games | libretro core |
| `mednafen_psx_hw` | [psx](psx.md) | Beetle PSX HW | libretro core |
| `mednafen_psx` | [psx](psx.md) | Beetle PSX | libretro core |
| `pcsx_rearmed` | [psx](psx.md) | PCSX ReARMed | libretro core |
| `vita_pops` | [psx](psx.md) | PS1 Classics on a PSP or PS Vita (POPS, Adrenaline) | console |
| `pcsx2` | [ps2](ps2.md) | LRPS2 | libretro core |
| `pcsx2_standalone` | [ps2](ps2.md) | PCSX2, AetherSX2, NetherSX2, ARMSX2 | standalone |
| `mednafen_saturn` | [saturn](saturn.md) | Beetle Saturn | libretro core |
| `kronos` | [saturn](saturn.md) | Kronos | libretro core |
| `dolphin` | [gamecube](gamecube.md) | Dolphin | libretro core |
| `dolphin_standalone` | [gamecube](gamecube.md) | Dolphin | standalone |
| `flycast` | [dreamcast](dreamcast.md) | Flycast | libretro core |
| `flycast_standalone` | [dreamcast](dreamcast.md) | Flycast | standalone |
| `yabause` | [saturn](saturn.md) | Yabause | libretro core |
| `yabasanshiro` | [saturn](saturn.md) | Yaba Sanshiro | libretro core |
| `mednafen_ngp` | [ngp](ngp.md) | Beetle NeoPop | libretro core |
| `opera` | [3do](3do.md) | Opera | libretro core |
| `pokemini` | [pokemini](pokemini.md) | PokeMini | libretro core |
| `handy` | [lynx](lynx.md) | Handy | libretro core |
| `melonds` | [nds](nds.md) | melonDS (the legacy core) | libretro core |
| `fbneo` | [arcade](arcade.md) | FBNeo | libretro core |
| `mame2003_plus` | [arcade](arcade.md) | MAME 2003-Plus | libretro core |
| `dosbox_pure` | [dos](dos.md) | DOSBox Pure | libretro core |
| `same_cdi` | [cdi](cdi.md) | SAME CDi | libretro core |
| `nestopia` | [fds](fds.md) | Nestopia, FDS disks | libretro core |
| `eden` | [switch](switch.md) | Eden | standalone |
| `citron` | [switch](switch.md) | Citron | standalone |
| `sudachi` | [switch](switch.md) | Sudachi | standalone |
| `yuzu` | [switch](switch.md) | yuzu | standalone |
| `cemu` | [wiiu](wiiu.md) | Cemu | standalone |
| `vita3k` | [psvita](psvita.md) | Vita3K | standalone |
| `rpcs3` | [ps3](ps3.md) | RPCS3, aPS3e | standalone |
