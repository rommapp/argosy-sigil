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
emulator keeps its saves; sigil never searches the drive. Each column
reads `yes` (sigil does it), `no` (the system needs it and sigil doesn't
do it yet) or `n/a` (the system has nothing of that kind).

- **Game id**: sigil reads the system's own id out of the ROM. `n/a` where
  the cart or disc carries none and emulators name saves after the file.
- **Save files**: sigil knows which files in the save folder are this
  game's, for each emulator it lists. The client can upload them as they
  are ([members](docs/quickstart-guides/c.md#without-collect-and-restore)).
- **Shared card**: the emulator can keep every game's saves on one memory
  card or backup RAM file, and sigil takes one game's saves out of it and
  puts them back without touching the others. `n/a` where each game's
  saves are already files of their own.
- **Unit**: what travels to RomM: one file, several files zipped, or the
  game's save folders zipped. "single or multi" depends on the emulator
  or the cart (a clock file, a second card).
- **Collect and restore**: sigil builds the unit, writes a downloaded one
  back and tracks what changed since the last sync
  ([sync](docs/sync.md)). Where it's `no`, the client uploads the save
  files as they are.

| System | Game id | Save files | Shared card | Unit | Collect and restore |
|---|---|---|---|---|---|
| [PlayStation](docs/platforms/psx.md) | yes | yes | yes | single | yes |
| [PlayStation 2](docs/platforms/ps2.md) | yes | yes | yes | single | yes |
| [PSP](docs/platforms/psp.md) | yes | yes | n/a | folder | yes |
| [PS Vita](docs/platforms/psvita.md) | yes | yes | n/a | folder | yes |
| [PlayStation 3](docs/platforms/ps3.md) | yes | yes | n/a | folder | yes |
| [Game Boy / Color](docs/platforms/gb.md) | n/a | yes | n/a | single or multi | yes |
| [Game Boy Advance](docs/platforms/gba.md) | no | yes | n/a | single or multi | no |
| [Super Nintendo](docs/platforms/snes.md) | n/a | yes | n/a | single or multi | no |
| [Famicom Disk System](docs/platforms/fds.md) | no | yes | n/a | single | no |
| [Nintendo DS](docs/platforms/nds.md) | no | yes | n/a | single | no |
| [Nintendo 3DS](docs/platforms/3ds.md) | yes | yes | n/a | folder | yes |
| [Nintendo 64](docs/platforms/n64.md) | yes | yes | n/a | multi | yes |
| [GameCube](docs/platforms/gamecube.md) | yes | yes | yes | single or multi | yes |
| [Wii](docs/platforms/wii.md) | yes | no | n/a | folder | no |
| [Wii U](docs/platforms/wiiu.md) | yes | yes | n/a | folder | yes |
| [Switch](docs/platforms/switch.md) | yes | yes | n/a | folder | yes |
| [Mega Drive, 32X, Master System, Game Gear](docs/platforms/genesis.md) | no | yes | n/a | single | no |
| [Sega CD](docs/platforms/segacd.md) | no | yes | yes | single or multi | yes |
| [Saturn](docs/platforms/saturn.md) | no | yes | yes | single or multi | yes |
| [Dreamcast](docs/platforms/dreamcast.md) | yes | yes | yes | single or multi | yes |
| [Xbox](docs/platforms/xbox.md) | yes | no | n/a | folder | no |
| [Xbox 360](docs/platforms/xbox360.md) | yes | no | n/a | folder | no |
| [Arcade](docs/platforms/arcade.md) | n/a | yes | no | single or multi | no |
| [Neo Geo](docs/platforms/neogeo.md) | no | yes | no | single or multi | no |
| [Neo Geo Pocket](docs/platforms/ngp.md) | no | yes | n/a | single | no |
| [PC Engine](docs/platforms/pce.md) | n/a | yes | n/a | single | no |
| [WonderSwan](docs/platforms/wonderswan.md) | no | yes | n/a | single | no |
| [Lynx](docs/platforms/lynx.md) | n/a | yes | n/a | single | no |
| [Jaguar](docs/platforms/jaguar.md) | n/a | yes | no | single | no |
| [Pokémon Mini](docs/platforms/pokemini.md) | no | yes | n/a | single | no |
| [3DO](docs/platforms/3do.md) | n/a | yes | no | single | no |
| [DOS](docs/platforms/dos.md) | n/a | yes | n/a | single | no |
| [CD-i](docs/platforms/cdi.md) | n/a | yes | n/a | folder | no |

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
