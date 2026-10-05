# Go binding for argosy-sigil

cgo wrapper around the sigil C library. Drop-in replacement for
hand-rolled per-platform parsers in Go applications that need
per-game save/state file naming (e.g. Grout).

## Build

Sigil's C library has to be built first. The provided Makefile does
this:

```sh
cd bindings/go
make build      # cmake + go build
make test       # cmake + go test
```

Or invoke cmake yourself:

```sh
cmake -B build -S . -DSIGIL_BUILD_CLI=OFF -DSIGIL_BUILD_TESTS=OFF
cmake --build build --target sigil
cd bindings/go && go build ./...
```

The cgo `#cgo LDFLAGS` in `sigil.go` looks for the static libs at
`../../build/`; if your CMake build dir is elsewhere, override with
`CGO_LDFLAGS=-L/your/build/dir ...`.

On Windows, cgo links with gcc, so build sigil with MinGW-w64 from an
MSYS2 UCRT64 shell, not MSVC
([docs/building.md](../../docs/building.md#windows)), then run the cmake
commands above in that shell.

## Use

[docs/quickstart-guides/go.md](../../docs/quickstart-guides/go.md): identify, locate, hash, sync, with what
each call requires and returns.

## Notes

- `Extract` blocks on I/O. Call from a goroutine or worker.
- `r.Usage` matters: PSP and GameCube are PREFIX platforms, meaning a
  single game corresponds to multiple folders/files. Treating them as
  EXACT misses every save. [docs/identification.md](../../docs/identification.md)
  says how to apply `SaveID` for each usage.
- The cgo build pins to sigil's static libs; once linked, the Go
  binary has no runtime dependency on sigil's `.a`/`.so`.
