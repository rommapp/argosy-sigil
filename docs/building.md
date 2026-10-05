# Building and testing

## Building

```sh
cmake -B build -S .
cmake --build build
```

Every platform and container reader is always compiled. There is no
build-time toggle for what sigil can read, deliberately: sigil is
embedded by larger applications, and a consumer expects a format to work
rather than to find out at integration time that a flag dropped it.

The options that remain choose what gets produced, not what sigil
understands:

| Option | Effect |
|---|---|
| `-DSIGIL_BUILD_SHARED=ON` | Build a shared library (`libsigil.so`, `libsigil.dylib`, `sigil.dll`) instead of a static one |
| `-DSIGIL_BUILD_CLI=OFF` | Skip the `sigil(1)` reference CLI ([cli.md](cli.md)) |
| `-DSIGIL_BUILD_TESTS=OFF` | Skip tests |

### Windows

sigil builds with MSVC and with MinGW-w64. Which one depends on what links
it:

| Consumer | Build sigil with | Why |
|---|---|---|
| C or C++ built with MSVC | MSVC | `.lib` archives link only into MSVC builds |
| Python | MSVC | CPython on Windows compiles extensions with MSVC |
| Go | MinGW-w64 | cgo links with gcc, which can't read MSVC's `.lib` archives |

With MSVC, from a Developer Command Prompt or any shell where CMake finds
Visual Studio:

```sh
cmake -B build -S .
cmake --build build --config Release
ctest --test-dir build -C Release
```

The Visual Studio generator puts the libraries in `build\Release\`
(`sigil.lib`, `sigil_chdr.lib`, ...; `sigil.dll` with its import library
`sigil.lib` in a shared build). Link a shared build with `SIGIL_SHARED`
defined, which CMake's `target_link_libraries(... sigil)` does for you.

With MinGW-w64, from an MSYS2 UCRT64 shell (`pacman -S
mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake
mingw-w64-ucrt-x86_64-ninja`):

```sh
cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

Every path sigil takes is UTF-8, on Windows too: sigil opens files through
the wide-character calls, so a ROM named in Japanese opens. Pass paths
from `wchar_t` APIs through `WideCharToMultiByte(CP_UTF8, ...)`, not the
ANSI code page. The Python and Go bindings already pass UTF-8. Relative
save paths (`listing`, `write`, `remove`) use `/`; Windows accepts it.

CI builds and tests both: MSVC with the Python binding, and MinGW-w64
with the Go binding. The library reaches POSIX names through
`src/sigil_compat.h`, which CMake force-includes into every source on
MSVC; tests reach files and folders through `tests/test_fs.h`. To catch
a header or call Windows lacks before CI does, cross-build with
MinGW-w64 (`brew install mingw-w64` on macOS) using a toolchain file that
sets `CMAKE_SYSTEM_NAME` to `Windows` and the compilers to
`x86_64-w64-mingw32-gcc` and `x86_64-w64-mingw32-g++`:

```sh
cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=mingw.cmake
cmake --build build-win
```

MinGW ships some POSIX names MSVC lacks (`strcasecmp`, `dirent.h`), so a
clean cross-build doesn't prove the MSVC build; CI does.

## Bindings

All the bindings expose the same operations, options and fields.
`bindings/python/test_contract.py` (`make contract`, no toolchain
needed) holds the table of names and fails when a binding drops one, and
keeps the C enums, the JNI descriptors and the Kotlin constructors in
step. A new binding under `bindings/<lang>/` joins that table and gets
its own page under `docs/quickstart-guides/`. `make contract` also runs
`bindings/python/test_docs.py`: every layout row in `src/save_layout.c`
has a page under `docs/platforms/` that documents it, every platform page
states its status, and every relative link and anchor in the README and
`docs/` resolves.

The Kotlin binding's JVM tests (`bindings/android/src/test`) run on this
machine against the JNI library built for the host JDK. Build it with
`-DSIGIL_BUILD_JNI_HOST=ON`, which puts `libsigil-jni` in
`build/bindings/`. Then run `gradle -p tests/kotlin :sigil:testDebugUnitTest`
(Gradle 8.9 or later, with `ANDROID_HOME` set), passing
`-PsigilHostJniDir=<that folder>` when the build folder isn't `build/`. CI
runs the same on Linux.

## Testing

```sh
# Synthetic unit tests (fast, no ROMs needed)
cmake --build build && ctest --test-dir build

# Real-ROM integration tests. Point them at a directory with platform
# subdirs (psp/, psx/, ps2/, ps3/, switch/, 3ds/, wii/, wiiu/, ngc/,
# psvita/, dc/, xbox/, xbox360/).
SIGIL_ROM_DIR=/path/to/roms ctest --test-dir build -R integration

# Switch tests additionally need a prod.keys file
SIGIL_ROM_DIR=/path/to/roms \
SIGIL_PROD_KEYS=/path/to/prod.keys \
ctest --test-dir build -R integration_switch

# Cap samples per platform (default 25). Useful when iterating on a
# library with hundreds of CHDs per platform.
SIGIL_SAMPLE_LIMIT=10 SIGIL_ROM_DIR=/path/to/roms \
ctest --test-dir build -R integration
```

Integration tests skip cleanly with exit code 77 when env vars are
unset, so the public CI without ROMs can still run unit tests.

The same `ctest` run builds and tests the Go and Python bindings against
this build's static libraries when `go` is on the path and
`bindings/python/.venv` (or the Python named by `-DSIGIL_BINDING_PYTHON`)
has cffi and pytest. Shared and sanitized builds leave them out.

Save tests read real save files from `tests/fixtures/saves/`. The files
aren't committed, only their manifests, so a test skips when its
platform's samples are absent and fails when only some of them are. [tests/fixtures/saves/README.md](../tests/fixtures/saves/README.md)
explains how to add samples.

## Fuzzing

```sh
# Fuzz the card parsers. The default engine mutates the given samples
# under the address and undefined-behaviour sanitizers and works with any
# clang; pass -DSIGIL_FUZZ_ENGINE=libfuzzer with a clang that ships libFuzzer.
cmake -S . -B build-fuzz -DSIGIL_BUILD_FUZZERS=ON -DSIGIL_BUILD_TESTS=OFF \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-fuzz --target fuzz_card
./build-fuzz/fuzz_card 1000000 1 tests/fixtures/saves/psx/files/*/*

# Fuzz sync. fuzz_sync restores PS1 units with a companion; fuzz_sync_volumes
# runs each input through Sega CD and Saturn (per-game and shared volumes),
# flycast, and Dolphin's GCI folder and raw card. Both abort when a unit that restored
# doesn't collect back to the same saves, unchanged.
cmake --build build-fuzz --target fuzz_sync fuzz_sync_volumes
./build-fuzz/fuzz_sync 100000 1 tests/fixtures/saves/psx/files/*/*
./build-fuzz/fuzz_sync_volumes 20000 1 tests/fixtures/saves/saturn/files/*/* \
  tests/fixtures/saves/segacd/files/*/* tests/fixtures/saves/dc/files/*/*.bin \
  tests/fixtures/saves/ngc/files/*/*.gci

# fuzz_sync_profiles takes a unit's members as "name<TAB>data" lines and
# restores them into an Eden and a Cemu folder; it aborts on a write outside
# the game's folders, or a restore that doesn't collect back unchanged.
cmake --build build-fuzz --target fuzz_sync_profiles
./build-fuzz/fuzz_sync_profiles 300000 1 fuzz/seeds/sync_profiles*.txt
```

The fuzzers stay out of `ctest`: they need the fuzz build, and a useful
run takes minutes.
