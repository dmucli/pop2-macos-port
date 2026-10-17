#!/bin/zsh
# Build the Linux release binary in a Debian container (Docker), for Recalbox.
#   tools/build_linux.sh [arm64|amd64]     (default arm64: Raspberry Pi 4/5)
# Output: build/linux-<arch>/pop2
set -eu
ROOT=${0:A:h:h}
ARCH=${1:-arm64}
IMAGE=pop2-build-$ARCH
docker build -q --platform linux/$ARCH -t $IMAGE $ROOT/packaging/recalbox >/dev/null
# -o: the generated glue is up to date in the repository (regenerating it needs the Ghidra side)
docker run --rm --platform linux/$ARCH -v $ROOT:/src -w /src $IMAGE \
    make -j$(sysctl -n hw.ncpu) BUILD=build/linux-$ARCH CORE=mini \
         -o src/game/gen/glue.c -o src/game/gen/proto.h build/linux-$ARCH/pop2
docker run --rm --platform linux/$ARCH -v $ROOT:/src -w /src $IMAGE \
    sh -c "strip -o build/linux-$ARCH/pop2.stripped build/linux-$ARCH/pop2 && file build/linux-$ARCH/pop2 2>/dev/null; ldd build/linux-$ARCH/pop2 | sed 's/ (0x.*//'"
