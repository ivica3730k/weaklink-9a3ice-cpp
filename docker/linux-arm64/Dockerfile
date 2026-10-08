# Linux arm64 build image. Produces ./binaries/linux-arm64/weaklink-modem.
FROM --platform=linux/arm64 ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build git ca-certificates \
      libasound2-dev libpulse-dev pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY docker/build-linux.sh /usr/local/bin/weaklink-build
RUN chmod +x /usr/local/bin/weaklink-build

CMD ["weaklink-build"]
