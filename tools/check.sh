#!/bin/zsh
# Differential check of ported functions against the original 68k code.
#   tools/check.sh [scenario ...]        (default: all scenarios)
# Scenarios: intro newgame save:"Level 7" ...  Each runs the game deterministically with
# scripted input and POP2_CHECK; every call of a ported function (first $CALLS calls per
# scenario) is replayed on the original code in a forked process and the memory compared.
# Env: HFV (disk image), SECS (host seconds per scenario, default 35), CALLS (default 30).
set -u
ROOT=${0:A:h:h}
HFV=${HFV:-$ROOT/../PoP2.hfv}; [ -f "$HFV" ] || HFV=$ROOT/../PoP2.hfv
BIN=${BIN:-$ROOT/build/pop2}
SECS=${SECS:-35}
CALLS=${CALLS:-30}
OUT=${OUT:-$ROOT/build/check}
MOVES="right:1500,shift-right:1200,up:300,left:900,shift-left:900,down:400,up:600,right:700,shift:200,right:1600,up:300,left:2500,shift-up:300,right:300,down:300,shift:300"
# turn a relative move list into absolute times starting at $1 ms
moves_from() {
  local t=$1 out="" k d
  for m in ${(s:,:)MOVES}; do
    k=${m%%:*}; d=${m##*:}
    [ "$k" = "shift" ] && k=shiftkey
    out="$out,$t:$k:$d"; t=$((t + d + 400))
  done
  echo ${out#,}
}
typeset -A SCN
SCN[intro]="intro|"
SCN[newgame]="newgame|15000:cmd-n:200,$(moves_from 19000),$(moves_from 40000)"
for s in "Level" "Level 5:2" "Level 5:3" "Level 7" "Level 7:2" "Level 8" "Level 8:2" "Level 11:2" "Level 13:2" "Level 15"; do
  SCN[save:$s]="$s|15000:cmd-o:200,$(moves_from 22000),$(moves_from 45000)"
done
list=("$@"); [ ${#list} -eq 0 ] && list=(${(k)SCN})
mkdir -p $OUT
pids=()
for sc in $list; do
  spec=${SCN[$sc]:-}; [ -z "$spec" ] && { echo "unknown scenario $sc"; continue; }
  file=${spec%%|*}; input=${spec#*|}
  tag=${sc//[^A-Za-z0-9]/_}; rm -rf $OUT/ovl_$tag
  ( env POP2_CHECK=$CALLS POP2_DETERMINISTIC=1 POP2_OVERLAY=$OUT/ovl_$tag POP2_TEST_POKE="-0x56d8:2:1" \
        POP2_OPENFILE="${file/:/\/}" POP2_INPUT="$input" POP2_NATIVE_STATS=1 \
        $BIN --headless $HFV > $OUT/$tag.log 2>&1 & pid=$!; sleep $SECS; kill -TERM $pid 2>/dev/null; wait $pid 2>/dev/null ) &
  pids+=($!)
done
wait
echo "== results (logs in $OUT) =="
cat $OUT/*.log | grep -a "^\[check\] MISMATCH" | sed 's/ (call.*//' | sort | uniq -c | sort -rn | head -40
echo "-- ok: $(cat $OUT/*.log | grep -a '^\[check\] .* ok$' | sort -u | wc -l | tr -d ' ') functions verified"
cat $OUT/*.log | grep -a "^\[native\]" | awk '{a[$2]+=$3} END {for (k in a) if (a[k]==0) print "   never called: " k}' | sort | head -60
grep -a -l "exception vector" $OUT/*.log | sed 's/^/   CRASH in /'
