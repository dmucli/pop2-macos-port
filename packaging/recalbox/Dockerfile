# Build environment for the Linux / Recalbox binary (arm64 = Raspberry Pi 4/5).
# Debian 12: glibc 2.36, older than Recalbox's (2.39), so the binary runs there; SDL2 and
# zlib are linked dynamically and come from the Recalbox system at run time.
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential python3 libsdl2-dev zlib1g-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
