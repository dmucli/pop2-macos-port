#!/bin/zsh
# Cross-platform check: scripted runs of the game (deterministic clock, scripted keys) must
# end with the same 16 MB of emulated memory on macOS and on Linux.
#   tools/crosscheck.sh [arm64] [amd64]        (default: arm64; needs Docker)
# The macOS side is built with the Linux fallback font (-DPOP2_BUILTIN_FONT): the only
# intended difference between the two is the host font of menu titles and alerts.
# Env: HFV (disk image), VMS (virtual milliseconds per run, default 60000).
set -u
ROOT=${0:A:h:h}
HFV=${HFV:-$ROOT/../PoP2.hfv}
VMS=${VMS:-60000}
ARCHS=(${@:-arm64})
OUT=$ROOT/build/crosscheck
rm -rf $OUT; mkdir -p $OUT; cp $HFV $OUT/PoP2.hfv

echo "building..."
CFLAGS="-O2 -g -DPOP2_BUILTIN_FONT" make -s -C $ROOT -j8 BUILD=build/release-bf CORE=mini build/release-bf/pop2 >/dev/null || exit 1
for a in $ARCHS; do $ROOT/tools/build_linux.sh $a >/dev/null || exit 1; done

MOVES="22000:right:1500,23900:shift-right:1200,25500:up:300,26200:left:900,27500:shift-left:900,28800:down:400,29600:up:600,30600:right:700,31700:shiftkey:200,32300:right:1600"
typeset -A SCN
SCN[newgame]="|15000:cmd-n:200,$MOVES"
SCN[level7]="Level 7|15000:cmd-o:200,$MOVES"
SCN[level12]="Level 12|15000:cmd-o:200,$MOVES"

run_mac() {   # scenario file input
    env POP2_DETERMINISTIC=1 POP2_TEST_POKE="-0x56d8:2:1" POP2_INPUT="$3" POP2_OPENFILE="$2" POP2_EXIT_AT_MS=$VMS \
        POP2_HASH_AT_EXIT=1 POP2_OVERLAY=$OUT/ovl-mac-$1 $ROOT/build/release-bf/pop2 --headless $OUT/PoP2.hfv 2>&1 |
        sed -n 's/^memory hash //p' > $OUT/$1.mac
}
run_linux() {   # arch scenario file input
    docker run --rm --platform linux/$1 -v $ROOT:/src -v $OUT:/x -w /src -e HOME=/tmp \
        -e POP2_DETERMINISTIC=1 -e POP2_TEST_POKE="-0x56d8:2:1" -e POP2_INPUT="$4" -e POP2_OPENFILE="$3" \
        -e POP2_EXIT_AT_MS=$VMS -e POP2_HASH_AT_EXIT=1 -e POP2_OVERLAY=/x/ovl-$1-$2 \
        pop2-build-bullseye-$1 build/linux-$1/pop2 --headless /x/PoP2.hfv 2>&1 |
        sed -n 's/^memory hash //p' > $OUT/$2.$1
}

echo "running ${(k)SCN} for ${VMS} virtual ms..."
for s in ${(k)SCN}; do
    f=${SCN[$s]%%|*}; in=${SCN[$s]#*|}
    run_mac $s "$f" "$in" &
    for a in $ARCHS; do run_linux $a $s "$f" "$in" & done
done
wait

fail=0
for s in ${(ok)SCN}; do
    m=$(cat $OUT/$s.mac)
    line="$s: mac ${m:-none}"
    for a in $ARCHS; do
        l=$(cat $OUT/$s.$a)
        if [ -n "$m" ] && [ "$m" = "$l" ]; then line="$line, $a same"; else line="$line, $a ${l:-none} DIFFERENT"; fail=1; fi
    done
    echo $line
done
[ $fail = 0 ] && echo "ok: identical on every platform" || echo "MISMATCH"
exit $fail
