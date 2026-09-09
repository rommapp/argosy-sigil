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

## Use

[docs/go.md](../../docs/go.md): identify, locate, hash, with what each
call requires and returns.

## Notes

- `Extract` blocks on I/O. Call from a goroutine or worker.
- `r.Usage` matters: PSP and GameCube are PREFIX platforms, meaning a
  single game corresponds to multiple folders/files. Treating them as
  EXACT silently misses every save — see the top-level README.
- The cgo build pins to sigil's static libs; once linked, the Go
  binary has no runtime dependency on sigil's `.a`/`.so`.
