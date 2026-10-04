# SPDX-License-Identifier: MPL-2.0
# Convenience wrapper: build sigil's static libs, then the cffi extension.
# Consumers can also invoke cmake themselves and run build_sigil.py directly.

SIGIL_ROOT := $(abspath ../..)
BUILD_DIR  := $(SIGIL_ROOT)/build-python
PYTHON     ?= python3

.PHONY: all build lib test contract clean

all: build

# Always handed to cmake, which knows when a C source changed; make would
# only see that the archive exists.
lib:
	cmake -B $(BUILD_DIR) -S $(SIGIL_ROOT) \
		-DSIGIL_BUILD_CLI=OFF -DSIGIL_BUILD_TESTS=OFF
	cmake --build $(BUILD_DIR) --target sigil

build: lib
	$(PYTHON) build_sigil.py

test: build
	$(PYTHON) -m pytest test_sigil.py -v

# Cross-language source and docs checks; no compiled extension or Android toolchain.
contract:
	$(PYTHON) -m pytest test_contract.py test_docs.py -v

clean:
	rm -rf $(BUILD_DIR) sigil/_sigil.c sigil/_sigil.o sigil/_sigil.*.so \
		sigil/_sigil.*.pyd __pycache__ sigil/__pycache__ .pytest_cache
