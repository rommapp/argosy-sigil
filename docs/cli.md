# CLI

A reference `sigil(1)` is built alongside the library. Useful for
spot-checks during integration without writing any code:

```sh
$ sigil /path/to/game.xci --platform=switch --prod-keys=/path/to/prod.keys
platform=switch title_id=0100ABCD12345000 raw_serial=0100ABCD12345000 save_id=0100ABCD12345000 usage=folder-exact source=binary

$ sigil "/path/to/Ace Combat 04 (USA).chd" --platform=ps2
platform=ps2 title_id=SLUS-20152 raw_serial=SLUS_201.52 save_id=BASLUS-20152 usage=folder-prefix source=binary

# Read straight out of an archive, no extraction step
$ sigil "/path/to/Robotech - Invasion (USA).zip" --platform=xbox
platform=xbox title_id=TT-027 raw_serial=5454001B save_id=5454001B usage=folder-exact source=binary

# A Vita dump resolves by content, so it needs no hint
$ sigil "/path/to/Actual Sunlight [PCSE00695] [USA] [NoNpDrm].zip"
platform=psvita title_id=PCSE00695 raw_serial=PCSE00695 save_id=PCSE00695 usage=folder-exact source=binary

# An N64 cart prints a second line: the fields standalone emulators name saves from
$ sigil "/path/to/1080 Snowboarding (Japan, USA) (En,Ja).z64"
platform=n64 title_id=NTEA raw_serial=NTEA save_id=NTEA usage=file-prefix source=binary experimental=0 content_type=unknown title_version=0 features=-
n64_header=1080 SNOWBOARDING n64_md5=FA27089C425DBAB99F19245C5C997613 n64_md5_n64=10C93DD78B695CD32B6938534ED0EDD5
```

Pass `--platform=auto` (the default) to sniff from the file extension.
Extensions that name a container rather than a console (`.zip`, a bare
`.iso`) still need a hint unless the contents identify the platform on
their own, which is why the Vita example above does not take one. An
extension two consoles share is settled the same way. `.rvz` and
`.wbfs` hold either a Wii or a GameCube disc, and the header magic
decides which. A `--platform` you pass is never second-guessed: it
names the console outright, and the magic is consulted only when you
name nothing. Check `source` on the result: `binary` means the id came
from file content, `filename` means every binary path failed and a
naming pattern was scanned instead.

Build it with the library; `-DSIGIL_BUILD_CLI=OFF` skips it
([building.md](building.md)).
