# Prince of Persia 2 on Recalbox

The port runs natively on Recalbox (Raspberry Pi 4/400 and 5/500, 64-bit) as a port
system of its own, "Prince of Persia 2", next to the built-in ports. Everything lives
on the share partition, so it survives Recalbox updates. As with the Mac app, **no game
data is included**: you need the HFS disk image of your own copy of the Macintosh version.

## Install

1. Get `PoP2-Port-Recalbox-arm64.zip` (or build it, see below).
2. Copy its contents into the Recalbox share: over the network (`\\RECALBOX\share` on
   Windows, `smb://recalbox/share` on a Mac) or onto the SHARE partition of the SD card.
   It holds:
   - `roms/ports/Prince of Persia 2/` (the program in `pop2/`, the game list, a readme)
   - `system/.emulationstation/systemlist.xml` (adds the system). **If that file already
     exists in your share, do not replace it**: add the `<system>` element of ours to it.
3. Copy your disk image into `roms/ports/Prince of Persia 2/` and name it `PoP2.hfv`.
4. Restart EmulationStation (or reboot). Prince of Persia 2 appears with the ports.

## Controls

| Pad | |
| --- | --- |
| D-pad / left stick | move |
| A | action: grab, strike, careful step |
| B | second button |
| Start | pause; on the title screen, start a game |
| Select | show the time left |
| Hotkey + Y | save state (the last 10 are kept) |
| Hotkey + X | load the newest save state (from the title screen too) |
| Hotkey + Left (hold) | rewind |
| Hotkey + Up | add one health (a flask when the meter is full) |
| Hotkey + Right | add 5 minutes |
| Hotkey + Start | quit |
| in a dialog | A or Start: OK, B: Cancel |

The pads are the ones configured in Recalbox (read from `es_input.cfg`).

## Files

`/recalbox/share/saves/pop2/` holds the game's own files (`disk/`), the save states
(`Save States/`), the settings (`settings.txt`) and the log of the last run (`pop2.log`).
To change the display, write `pixels`, `sharp` (whole multiples), `smooth` or `crt` into
`display.txt` there.

## How it works

EmulationStation runs the command of the system entry, which starts `pop2/pop2.sh` from the
folder of the game file. The script copies the program to `/tmp` (the share partition may
not allow running programs) and starts it full screen with Recalbox's own SDL2, with the
Recalbox hotkeys, the pad mappings from `es_input.cfg`, and its files in `saves/pop2`.

## Build

```sh
make recalbox      # Docker: builds the arm64 binary on Debian 12, then build/PoP2-Port-Recalbox-arm64.zip
```

The Linux build plays exactly like the Mac one: scripted runs of the game end with the same
emulated memory on both (macOS with the same fallback font, `-DPOP2_BUILTIN_FONT`).
