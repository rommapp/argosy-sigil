# argosy-sigil

A native C library that reads the game-native title id out of a ROM
(`ULUS10064`, `SLUS-20152`, `0100ABCD12345000`), finds the save files an
emulator keeps for that game, and moves them between the emulator and a
RomM server. A client pairs a local save with the right game record by
the platform's own id instead of guessing from file names, archives and
hashes the save the way RomM does, and syncs it back without touching
other games' saves.

## What it handles

The client always tells sigil which emulator runs the game and where that
emulator keeps its saves; sigil never searches the drive. From there:

- **Game id**: sigil reads the platform's own id out of the ROM.
- **Names the save files**: sigil knows which files in that save folder are
  this game's, for each emulator it lists. The client uploads them as they
  are ([members](docs/quickstart-guides/c.md#without-collect-and-restore)).
- **Builds the upload**: sigil packs the game's saves into the unit that
  goes to RomM and writes a downloaded one back, conflicts included
  ([sync](docs/sync.md)). Where the emulator keeps every game on one
  memory card or backup RAM file, this is what takes one game's saves out
  of the card and puts them back without touching the others.

| System | Game id | Names the save files | Builds the upload |
|---|---|---|---|
| [PlayStation](docs/platforms/psx.md) | yes | yes | yes |
| [PlayStation 2](docs/platforms/ps2.md) | yes | yes | yes |
| [PSP](docs/platforms/psp.md) | yes | yes | yes |
| [PS Vita](docs/platforms/psvita.md) | yes | yes | yes |
| [PlayStation 3](docs/platforms/ps3.md) | yes | yes | yes |
| [Game Boy / Color](docs/platforms/gb.md) | header facts | yes | |
| [Game Boy Advance](docs/platforms/gba.md) | | yes | |
| [Super Nintendo](docs/platforms/snes.md) | header facts | yes | |
| [Famicom Disk System](docs/platforms/fds.md) | | yes | |
| [Nintendo DS](docs/platforms/nds.md) | | yes | |
| [Nintendo 3DS](docs/platforms/3ds.md) | yes | | |
| [GameCube](docs/platforms/gamecube.md) | yes | yes | yes |
| [Wii](docs/platforms/wii.md) | yes | | |
| [Wii U](docs/platforms/wiiu.md) | yes | yes | yes |
| [Switch](docs/platforms/switch.md) | yes | yes | yes |
| [Mega Drive, 32X, Master System, Game Gear](docs/platforms/genesis.md) | | yes | |
| [Sega CD](docs/platforms/segacd.md) | | yes | yes |
| [Saturn](docs/platforms/saturn.md) | | yes | yes |
| [Dreamcast](docs/platforms/dreamcast.md) | yes | yes | yes |
| [Xbox](docs/platforms/xbox.md) | yes | | |
| [Xbox 360](docs/platforms/xbox360.md) | yes | | |
| [Arcade](docs/platforms/arcade.md), [Neo Geo](docs/platforms/neogeo.md), [Neo Geo Pocket](docs/platforms/ngp.md), [PC Engine](docs/platforms/pce.md), [WonderSwan](docs/platforms/wonderswan.md), [Lynx](docs/platforms/lynx.md), [Jaguar](docs/platforms/jaguar.md), [Pokémon Mini](docs/platforms/pokemini.md), [3DO](docs/platforms/3do.md), [DOS](docs/platforms/dos.md), [CD-i](docs/platforms/cdi.md) | | yes | |
| [Nintendo 64](docs/platforms/n64.md) | yes | yes | |

[docs/platforms/](docs/platforms/README.md) lists every system, the
emulators each one covers, and the ones still in development. Formats and
containers sigil reads: [identification](docs/identification.md),
[containers](docs/containers.md).

## Bindings

- C: [include/sigil.h](include/sigil.h). Guide: [C](docs/quickstart-guides/c.md).
- Kotlin/Java (Android, JNI): [`bindings/android/`](bindings/android/).
  Guide: [Kotlin](docs/quickstart-guides/kotlin.md).
- Go (cgo): [`bindings/go/`](bindings/go/). Guide: [Go](docs/quickstart-guides/go.md).
- Python (cffi): [`bindings/python/`](bindings/python/). Guide:
  [Python](docs/quickstart-guides/python.md).

Every binding exposes the same operations, options and fields. Each guide
walks the three steps (identify the game, locate its saves, hash or sync
them) with what each call requires, takes optionally, and returns.

## Usage

```sh
cmake -B build -S . && cmake --build build
```

Identify a game:

```c
#include <sigil.h>
#include <stdio.h>

int main(void) {
    sigil_result r;
    int rc = sigil_extract_from_path("/path/to/game.iso",
                                     SIGIL_PLATFORM_AUTO, NULL, &r);
    if (rc != SIGIL_OK) {
        fprintf(stderr, "sigil: %s\n", sigil_strerror(rc));
        return 1;
    }
    printf("platform=%s\n",   sigil_platform_to_slug(r.platform));
    printf("title_id=%s\n",   r.title_id);     /* game identity, persist this */
    printf("save_id=%s\n",    r.save_id);      /* on-disk save folder/file name */
    printf("raw_serial=%s\n", r.raw_serial);   /* as it appears in the binary */
    printf("source=%s\n",     r.source == SIGIL_SOURCE_BINARY ? "binary" : "filename");
    /* r.usage: SIGIL_USAGE_FOLDER_EXACT | _FOLDER_PREFIX | _FILE_EXACT | _FILE_PREFIX | _FOLDER_SPLIT */
    return 0;
}
```

Collect a game's saves and put them back, in Python:

```python
import sigil

game = sigil.extract("Chrono Cross (USA).chd", "psx")
unit = sigil.collect(game, "pcsx_rearmed", "Chrono Cross (USA).cue", "/saves/psx")
# upload unit.data as unit.artifact, store unit.state
sigil.restore(unit.data, game, "pcsx_rearmed", "Chrono Cross (USA).cue", "/saves/psx", state=unit.state)
```

More: [identification](docs/identification.md) (the fields and what to
do with `save_id`), [save units](docs/save-units.md) (which files belong
to a game, how they're hashed), [sync](docs/sync.md) (collect, restore,
conflicts), [CLI](docs/cli.md), [building and testing](docs/building.md).

## Status

Pre-1.0. The API may evolve before 1.0. Every public struct has a
`struct_version` field so new fields can be added without breaking
existing consumers, but the function signatures and existing field
layouts can still change. Consumers vendoring sigil should pin a
specific commit; once 1.0 ships the C ABI freezes.

## Issues and contributing

Report bugs and platform gaps in the GitHub issues, with the platform,
the emulator, and the title id or the file that fails. A save sample
helps most; [tests/fixtures/saves/README.md](tests/fixtures/saves/README.md)
explains how samples are added.

PRs welcome. See [TRADEMARKS.md](TRADEMARKS.md) for the naming
policy: forks-for-contribution (standard fork → PR-upstream flow)
are encouraged; forks-and-republish-as-a-separate-project under the
`argosy-sigil` name are not.

## License

MPL-2.0. See [LICENSE](LICENSE) for the full text and
[TRADEMARKS.md](TRADEMARKS.md) for the project naming policy. Anyone
can use sigil in any application (proprietary or open); modifications
to sigil's own files must remain MPL-2.0.

The Sega CD backup RAM reader (`src/card_segacd.c`) takes its ECC layout
table and format-block bytes from superctr/buram, Copyright (c) 2022 Ian
Karlsson, used under the MIT license; the notice is in
[licenses/buram-MIT.txt](licenses/buram-MIT.txt).
