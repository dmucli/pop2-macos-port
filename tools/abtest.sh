#!/bin/zsh
# A/B test: run the deterministic game twice (68k reference vs ported code) and compare frames.
#   tools/abtest.sh <hfv> <outdir> <seconds-of-host-time> [input-script]
set -u
HFV=$1; OUT=$2; SECS=${3:-30}; INPUT=${4:-"15000:cmd-n:200,19000:right:3000,23000:shift-up:300,26000:left:2500,30000:up:400,33000:right:600,35000:shift-right:2000"}
BIN=${BIN:-build/pop2}
rm -rf $OUT; mkdir -p $OUT/ref $OUT/new $OUT/ovl_ref $OUT/ovl_new
run() {  # $1 = tag, $2 = native mode
  POP2_OVERLAY=$OUT/ovl_$1 POP2_DETERMINISTIC=1 POP2_SNAP=$OUT/$1 POP2_SNAP_MS=${SNAP_MS:-500} POP2_INPUT="$INPUT" POP2_NATIVE=$2 \
    $BIN --headless $HFV > $OUT/$1.log 2>&1 &
  local pid=$!; sleep $SECS; kill -TERM $pid 2>/dev/null; wait $pid 2>/dev/null
}
run ref none &
run new ${NEWMODE:-all} &
wait
n=0; first=""
nr=$(ls $OUT/ref | wc -l); nn=$(ls $OUT/new | wc -l); lim=$(( (nr < nn ? nr : nn) - 1 ))
for f in $(ls $OUT/ref | sort | head -n $lim); do
  [ -f $OUT/new/$f ] || break
  if ! cmp -s $OUT/ref/$f $OUT/new/$f; then n=$((n+1)); [ -z "$first" ] && first=$f; fi
  last=$f
done
echo "compared up to $last: $n differing frames${first:+ (first: $first)}"
grep -a -h "native:" $OUT/new.log | head -1
grep -a -h "exception\|unimpl" $OUT/ref.log $OUT/new.log | head -5
grep -a -h "\[native\]" $OUT/new.log | head -40
