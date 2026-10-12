#!/bin/zsh
# Rebuild the Ghidra project for the PoP2 68k application from scratch.
#   build_project.sh <app resource fork> <multiversal defs dir> <work dir>
# Ghidra's GUI must be closed (project lock).
set -euo pipefail
HERE=${0:A:h}
APP=$1; DEFS=$2; W=$3
PY=(${=PYTHON:-python3})
GHIDRA=${GHIDRA_HOME:-/opt/homebrew/opt/ghidra/libexec}
export JAVA_HOME=${JAVA_HOME:-/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home}
mkdir -p $W/ghidra $W/ghidra_in
"${PY[@]}" $HERE/../traps.py $DEFS $W/traps.json
"${PY[@]}" $HERE/../build_image.py $APP $W/traps.json $W/ghidra_in
GHIDRA_HEADLESS_MAXMEM=8G $GHIDRA/support/analyzeHeadless $W/ghidra PoP2 \
  -import $W/ghidra_in/pop2_image.bin -overwrite -loader BinaryLoader -loader-baseAddr 0 \
  -processor 68000:BE:32:MacToolbox -cspec thinkc -scriptPath $HERE/scripts \
  -preScript MacSetup.java $W/ghidra_in/pop2_meta.json \
  -postScript MacPost.java $W/ghidra_in/pop2_meta.json -max-cpu 8 > $W/headless.log 2>&1
grep -E "MacSetup.java>|MacPost.java>|REPORT: (Import|Save)" $W/headless.log
