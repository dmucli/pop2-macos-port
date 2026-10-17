Prince of Persia 2 (Macintosh) - unofficial native port for Linux
=================================================================

  ./pop2                       (the disk image is next to it, or in the current folder)
  ./pop2 /path/to/disk-image

The disk image is the one of YOUR OWN copy of the Macintosh version (floppy, CD or hard
disk: .hfv, .dsk, .img, .image, .toast, .iso, .cdr, .bin): no game data is included.
Needs SDL2 and zlib (Debian/Ubuntu: sudo apt install libsdl2-2.0-0 zlib1g).

Keys: arrows move, Shift (or keypad .) action, Control (or keypad 0) second button, Esc
pause, Space time left, F5 / F9 save state / load the newest, hold Delete to rewind,
F1 extras menu (save states with pictures, levels, cheats, display, statistics).
Game controllers work too (Guide button: extras menu).

Saves, save states and settings: ~/.local/share/pop2-port (or $POP2_HOME).
Options: POP2_FULLSCREEN=1, POP2_DISPLAY=pixels|sharp|smooth|crt.
Source and documentation: https://github.com/dmucli/pop2-port
