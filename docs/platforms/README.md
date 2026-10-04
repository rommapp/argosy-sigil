# Platforms

One page per system. Each says how sigil identifies a game, which emulator
layouts it knows, what sync does there, the emulator research behind it,
and what isn't covered yet.

Status:

- **synced**: collect and restore move the saves ([sync.md](../sync.md)).
- **located**: `sigil_save_resolve` names the save files ([save-units.md](../save-units.md)); collect and restore don't cover the system yet.
- **in development**: research only; no layout names the saves.

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

Every layout id `src/save_layout.c` defines, and the page that documents
it. A layout id without a row here, or missing from its page's Save layouts
section, fails `bindings/python/test_contract.py`.

| Layout | Page |
|---|---|
| `vba_next` | [gba](gba.md) |
| `gpsp` | [gba](gba.md) |
| `bsnes` | [snes](snes.md) |
| `genesis_plus_gx` | [segacd](segacd.md) |
| `mednafen_psx_hw` | [psx](psx.md) |
| `mednafen_psx` | [psx](psx.md) |
| `pcsx_rearmed` | [psx](psx.md) |
| `vita_pops` | [psx](psx.md) |
| `pcsx2` | [ps2](ps2.md) |
| `pcsx2_standalone` | [ps2](ps2.md) |
| `mednafen_saturn` | [saturn](saturn.md) |
| `kronos` | [saturn](saturn.md) |
| `dolphin` | [gamecube](gamecube.md) |
| `dolphin_standalone` | [gamecube](gamecube.md) |
| `flycast` | [dreamcast](dreamcast.md) |
| `flycast_standalone` | [dreamcast](dreamcast.md) |
| `yabause` | [saturn](saturn.md) |
| `yabasanshiro` | [saturn](saturn.md) |
| `mednafen_ngp` | [ngp](ngp.md) |
| `opera` | [3do](3do.md) |
| `pokemini` | [pokemini](pokemini.md) |
| `handy` | [lynx](lynx.md) |
| `melonds` | [nds](nds.md) |
| `fbneo` | [arcade](arcade.md) |
| `mame2003_plus` | [arcade](arcade.md) |
| `dosbox_pure` | [dos](dos.md) |
| `same_cdi` | [cdi](cdi.md) |
| `nestopia` | [fds](fds.md) |
| `eden` | [switch](switch.md) |
| `citron` | [switch](switch.md) |
| `sudachi` | [switch](switch.md) |
| `yuzu` | [switch](switch.md) |
| `cemu` | [wiiu](wiiu.md) |
| `vita3k` | [psvita](psvita.md) |
| `rpcs3` | [ps3](ps3.md) |

A core with no row of its own uses the libretro default row (`{stem}.srm`,
plus `{stem}.rtc` when the cart has a clock), documented on
[gb](gb.md#save-layouts).
