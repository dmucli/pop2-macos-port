# Build environment for the Linux binaries (arm64: Raspberry Pi 4/5, handhelds; amd64: PCs).
# Debian 11: glibc 2.31, so the binary also runs on older systems (Recalbox has 2.39,
# ArkOS-based handheld firmwares 2.30+); SDL2 and zlib are linked dynamically and come
# from the system at run time.
FROM debian:bullseye-slim
# Debian 11 is archived: its packages come from archive.debian.org
RUN printf 'deb http://archive.debian.org/debian bullseye main\ndeb http://archive.debian.org/debian-security bullseye-security main\n' \
        > /etc/apt/sources.list \
    && apt-get -o Acquire::Check-Valid-Until=false update && apt-get install -y --no-install-recommends \
        build-essential python3 libsdl2-dev zlib1g-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
