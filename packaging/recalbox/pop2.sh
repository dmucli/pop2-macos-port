#!/bin/sh
# Prince of Persia 2 (Macintosh) on Recalbox. EmulationStation runs this with the game's
# disk image as $1 (the other arguments it adds are ignored).
#   saves, save states, settings and the log: /recalbox/share/saves/pop2
HERE=$(cd "$(dirname "$0")" && pwd)
SAVES=/recalbox/share/saves/pop2
mkdir -p "$SAVES"
# the share partition may not allow running programs: run a copy from /tmp
BIN=/tmp/pop2-port
cp "$HERE/pop2" "$BIN" && chmod 755 "$BIN" || exit 1
export POP2_HOME="$SAVES" POP2_OVERLAY="$SAVES/disk" POP2_FULLSCREEN=1 POP2_HOTKEYS=1 POP2_HIDE_MENUBAR=1 \
       POP2_ES_INPUT=/recalbox/share/system/.emulationstation/es_input.cfg
[ -f "$SAVES/display.txt" ] && export POP2_DISPLAY=$(cat "$SAVES/display.txt")
exec "$BIN" "$1" > "$SAVES/pop2.log" 2>&1
