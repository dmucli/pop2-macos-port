#!/bin/zsh
# Whole-game equivalence: run the same deterministic scenario with every function on the
# original 68k code (POP2_NATIVE=none) and fully native, and compare the sequences of
# Toolbox traps the two runs make.
#   tools/equiv.sh [scenario] [virtual ms]     scenario: newgame (default) or a check.sh save name
set -u
ROOT=${0:A:h:h}
HFV=${HFV:-$ROOT/../PoP2.hfv}
SC=${1:-newgame}; VMS=${2:-90000}
OUT=${OUT:-$ROOT/build/equiv}; mkdir -p $OUT
source <(sed -n '/^MOVES=/,/^}/p' $ROOT/tools/check.sh)
if [ "$SC" = newgame ]; then FILE=""; IN="15000:cmd-n:200,$(moves_from 19000),$(moves_from 40000)"
else FILE="${SC/:/"/"}"; IN="15000:cmd-o:200,$(moves_from 22000),$(moves_from 45000)"; fi
make -s -C $ROOT -j8 HOOK=1 all >/dev/null || exit 1
for m in orig native; do
  rm -rf $OUT/ovl_$m
  E=(POP2_X=1); [ $m = orig ] && E=(POP2_NATIVE=none)
  env $E POP2_TRAPLOG=$OUT/traps_$m.txt POP2_DETERMINISTIC=1 POP2_OVERLAY=$OUT/ovl_$m POP2_TEST_POKE="-0x56d8:2:1" \
      ${FILE:+POP2_OPENFILE=$FILE} POP2_INPUT="$IN" POP2_EXIT_AT_MS=$VMS $ROOT/build/pop2 --headless $HFV > $OUT/$m.log 2>&1 &
done
wait
awk '{print $2}' $OUT/traps_orig.txt > $OUT/w_orig.txt; awk '{print $2}' $OUT/traps_native.txt > $OUT/w_native.txt
n=$(wc -l < $OUT/w_orig.txt | tr -d ' ')
if cmp -s $OUT/w_orig.txt $OUT/w_native.txt; then echo "EQUIVALENT: same $n Toolbox traps"; exit 0; fi
L=$(cmp $OUT/w_orig.txt $OUT/w_native.txt | grep -o 'line [0-9]*' | awk '{print $2}')
echo "DIFFERENT at trap ${L:-?} (of $n); context (native, then original):"
sed -n "$((L-3)),$((L+2))p" $OUT/traps_native.txt; echo ---; sed -n "$((L-3)),$((L+2))p" $OUT/traps_orig.txt
exit 1
