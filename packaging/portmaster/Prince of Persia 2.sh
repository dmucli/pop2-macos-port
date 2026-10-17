#!/bin/bash
# Prince of Persia 2 (Macintosh), unofficial native port, for PortMaster handhelds.
# The game data is your own disk image, in ports/pop2 (any name: .hfv, .dsk, .img, .image,
# .toast, .iso, .cdr, .bin); the program finds it there.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

$ESUDO chmod 666 /dev/tty1
$ESUDO chmod 666 /dev/uinput

GAMEDIR="/$directory/ports/pop2"
cd "$GAMEDIR"
mkdir -p "$GAMEDIR/saves"

export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export POP2_HOME="$GAMEDIR/saves" POP2_OVERLAY="$GAMEDIR/saves/disk"
export POP2_FULLSCREEN=1 POP2_HOTKEYS=1 POP2_HIDE_MENUBAR=1

$GPTOKEYB "pop2.${DEVICE_ARCH}" &
./pop2.${DEVICE_ARCH} 2>&1 | tee "$GAMEDIR/log.txt"
$ESUDO kill -9 $(pidof gptokeyb)
$ESUDO systemctl restart oga_events &
printf "\033c" > /dev/tty1
