# sigil Python binding

cffi-based (API mode) Python binding for the sigil C library. The extension
statically links libsigil and its bundled decompression/crypto libs, so the
resulting module has no runtime dependency on the C build tree.

## Build

Requires Python >= 3.10, cffi, CMake, and a C compiler.

```sh
cd bindings/python
python3 -m venv .venv && . .venv/bin/activate
pip install cffi setuptools pytest
make build        # cmake the static libs into ../../build-python, then compile the extension in-place
make test         # build + pytest
```

On Windows, without `make`, build sigil with MSVC and point
`build_sigil.py` at the configuration's folder
([docs/building.md](../../docs/building.md#windows)):

```bat
cd bindings\python
python -m venv .venv && .venv\Scripts\activate
pip install cffi setuptools pytest
cmake -B ..\..\build-python -S ..\.. -DSIGIL_BUILD_CLI=OFF -DSIGIL_BUILD_TESTS=OFF
cmake --build ..\..\build-python --config Release --target sigil
set SIGIL_LIB_DIR=..\..\build-python\Release
python build_sigil.py
python -m pytest test_sigil.py
```

## Usage

[docs/quickstart-guides/python.md](../../docs/quickstart-guides/python.md): identify, locate, hash, sync,
with what each call requires and returns.
