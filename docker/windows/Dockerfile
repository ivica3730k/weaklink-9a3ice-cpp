# Windows x86_64 cross-build image (mingw-w64 on Linux).
#
# NOT IMPLEMENTED. The toolchain installs and the modem core compiles, but the
# live-audio layer still needs a Windows backend (WASAPI/WDM-KS via PortAudio)
# and the paplay/parec subprocess path has no Windows equivalent. The image is
# kept so the wiring (compose service, Makefile target, CI job) is already in
# place when that work lands.
FROM --platform=linux/amd64 ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      cmake ninja-build git ca-certificates \
      mingw-w64 g++-mingw-w64-x86-64 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
CMD ["bash", "-c", "echo 'windows build is not implemented yet'; exit 1"]
