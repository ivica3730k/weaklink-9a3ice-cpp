# Linux x86_64 build image. Produces ./binaries/linux-amd64/weaklink-modem.
# Ubuntu 22.04 keeps the glibc floor low enough for most current distros.
FROM --platform=linux/amd64 ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build git ca-certificates \
      libasound2-dev libpulse-dev pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY docker/build-linux.sh /usr/local/bin/weaklink-build
RUN chmod +x /usr/local/bin/weaklink-build

CMD ["weaklink-build"]
