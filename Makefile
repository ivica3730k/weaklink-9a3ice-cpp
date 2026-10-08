# Build entry points. Every target drops its binaries in ./binaries (gitignored)
# so CI can collect them without knowing the generator's layout.
#
# macOS is built natively: Apple's SDK cannot legally or technically run inside
# a Linux container, so there is no mac Docker image. Linux and Windows are
# built in containers (see docker/ and docker-compose.yml) and can be driven
# from GitHub Actions with the same `make` call used locally.

SHELL        := /bin/bash
BIN_DIR      := binaries
BUILD_DIR    := build
CMAKE_FLAGS  ?= -DCMAKE_BUILD_TYPE=Release
JOBS         ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
COMPOSE      ?= docker compose

.PHONY: help build-mac build-mac-arm build-mac-host build-linux build-linux-arm \
        build-windows test test-mac interop clean distclean

help:
	@echo "Targets:"
	@echo "  build-mac        macOS x86_64 (Intel)"
	@echo "  build-mac-arm    macOS arm64 (Apple silicon)"
	@echo "  build-mac-host   macOS for whatever the build machine is"
	@echo "  build-linux      Linux x86_64 via docker/linux-amd64"
	@echo "  build-linux-arm  Linux arm64  via docker/linux-arm64"
	@echo "  build-windows    Windows x86_64 (not implemented yet)"
	@echo "  test             configure, build and run the Catch2 suite"
	@echo "  interop          cross-validate Python WAV <-> C++ WAV"
	@echo "  clean            remove build trees, keep binaries"
	@echo "  distclean        remove build trees and binaries"

# ---- macOS (native; Docker cannot host the Apple SDK) ----------------------

# Both architectures are named explicitly so a build is reproducible wherever
# it runs. Apple's SDK ships both slices, so either one cross-builds from
# either machine -- only running the result needs the matching hardware (or
# Rosetta).

build-mac:
	cmake -S . -B $(BUILD_DIR)/mac-x86_64 -G Ninja $(CMAKE_FLAGS) -DWEAKLINK_BUILD_TESTS=OFF \
	      -DCMAKE_OSX_ARCHITECTURES=x86_64 \
	      -DWEAKLINK_BIN_DIR=$(CURDIR)/$(BIN_DIR)/macos-x86_64
	cmake --build $(BUILD_DIR)/mac-x86_64 -j $(JOBS)

build-mac-arm:
	cmake -S . -B $(BUILD_DIR)/mac-arm64 -G Ninja $(CMAKE_FLAGS) -DWEAKLINK_BUILD_TESTS=OFF \
	      -DCMAKE_OSX_ARCHITECTURES=arm64 \
	      -DWEAKLINK_BIN_DIR=$(CURDIR)/$(BIN_DIR)/macos-arm64
	cmake --build $(BUILD_DIR)/mac-arm64 -j $(JOBS)

# Whatever the build machine is. Handy while iterating; CI should name an
# architecture instead so the artifact is not a surprise.
build-mac-host:
	cmake -S . -B $(BUILD_DIR)/mac -G Ninja $(CMAKE_FLAGS) -DWEAKLINK_BUILD_TESTS=OFF \
	      -DWEAKLINK_BIN_DIR=$(CURDIR)/$(BIN_DIR)/macos-$(shell uname -m)
	cmake --build $(BUILD_DIR)/mac -j $(JOBS)

# ---- Linux (containerised) -------------------------------------------------

build-linux:
	$(COMPOSE) run --rm --build linux-amd64

build-linux-arm:
	$(COMPOSE) run --rm --build linux-arm64

# ---- Windows ---------------------------------------------------------------

build-windows:
	@echo "build-windows is not implemented yet."
	@echo "Scaffolding lives in docker/windows/Dockerfile (mingw-w64 cross-compile)."
	@exit 1

# ---- Tests -----------------------------------------------------------------

test: test-mac

test-mac:
	cmake -S . -B $(BUILD_DIR)/test -G Ninja $(CMAKE_FLAGS) -DWEAKLINK_BUILD_TESTS=ON \
	      -DWEAKLINK_BIN_DIR=$(CURDIR)/$(BIN_DIR)/test
	cmake --build $(BUILD_DIR)/test -j $(JOBS)
	ctest --test-dir $(BUILD_DIR)/test --output-on-failure

interop: test-mac
	python3 tools/interop_check.py --cpp-bin $(CURDIR)/$(BIN_DIR)/test/weaklink-modem

clean:
	rm -rf $(BUILD_DIR)

distclean: clean
	rm -rf $(BIN_DIR)
