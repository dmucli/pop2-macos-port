# Prince of Persia 2 — native port

*Prince of Persia 2: The Shadow and the Flame*, the 1993 Macintosh version, running
natively on **macOS, Linux, Recalbox and retro handhelds** (unofficial). The original 68k
program was decompiled with Ghidra and translated, function by function, into C; a
reimplementation of the classic Mac Toolbox runs it on top of SDL. Save states with
pictures, rewind, an on-screen menu, statistics, game controllers.

This is an unofficial fan project, not affiliated with or endorsed by the rights holders.
**No game data is included**: you play from your own copy of the game.

## Start playing

1. **Your game's disk image.** The disk image of your original Macintosh Prince of Persia 2
   (floppy, CD or hard disk), in any common format: `.hfv`, `.dsk`, `.img`, `.image`
   (Disk Copy), `.toast`, `.iso`, `.cdr`, `.bin` (MacBinary). See [the disk image](#the-disk-image).
2. **Download** the package for your system from the [latest release](../../releases/latest):

   | System | File | Then |
   | --- | --- | --- |
   | Mac (Apple Silicon, macOS 12+) | `PoP2-Port-macOS.zip` | unzip, move *PoP2 Port* to Applications, open it and choose your disk image |
   | Recalbox, Raspberry Pi 4/5 | `PoP2-Port-Recalbox-arm64.zip` | unzip into the Recalbox share, put the disk image in `roms/ports/Prince of Persia 2`, restart: see [Recalbox](packaging/recalbox/README.md) |
   | Recalbox on a PC | `PoP2-Port-Recalbox-x86_64.zip` | same as above |
   | Handhelds with PortMaster (ArkOS, ROCKNIX, muOS...) | `PoP2-Port-PortMaster.zip` | unzip into `ports/`, put the disk image in `ports/pop2`: see [PortMaster](packaging/portmaster/README.md) |
   | Linux PC / Raspberry Pi desktop | `PoP2-Port-Linux-x86_64.tar.gz` / `-arm64` | extract, put the disk image next to `pop2`, run `./pop2` |

3. **Play.** Start (or any key) on the title screen starts a game. F1, or Hotkey + B on
   Recalbox and handhelds, opens the on-screen menu: save states, levels, extras, display.

The disk image is never modified: saved games, save states and settings go to a folder of
their own (on a Mac `~/Library/Application Support/PoP2-port`).

| | Keyboard | Controller |
| --- | --- | --- |
| move | arrows | d-pad / left stick |
| action (grab, strike, careful step) | Shift | A |
| second button | Control | B |
| pause / time left | Esc / Space | Start / Select |
| save state / load the newest | F5 / F9 | Hotkey + Y / X (Recalbox, handhelds) |
| rewind (hold) | Delete | Hotkey + Left |
| on-screen menu | F1 | Guide, or Hotkey + B |

On a Mac, if macOS says it cannot check the app: open it once, then System Settings >
Privacy & Security > **Open Anyway**.

### The disk image

The port reads the game from a disk image of the original: the files of the Macintosh
version (the *Prince of Persia 2* application and its *Data* folder) on an HFS disk.
- **From the CD**: on a Mac, Disk Utility > File > New Image > Image from the CD, format
  *DVD/CD master* (a `.cdr` file); on Linux, `dd if=/dev/cdrom of=pop2.iso`.
- **An emulator disk** (Basilisk II, SheepShaver, Mini vMac) with the game installed: use
  that disk file (`.hfv`, `.dsk`, `.img`) as is.
- **Disk Copy images** of the floppies and **MacBinary** downloads of your own copy work
  as they are.

Whatever its name, the port finds the game on the disk.

## Extras

Additions of this port, outside the original game (they are off in the verification runs):

| Where | What |
| --- | --- |
| Extras > Infinite Health | the kid's life is refilled every frame (falls and traps still kill) |
| Extras > Infinite Time | the clock stops |
| Extras > Cheat Mode | the game's own debug mode (as when 1 2 3 are held at launch): `-`/`+` change the time, Command-L next level on any level; the letter keys no longer move |
| Extras > Skip to Next Level (Option-Command-N) | turns on Cheat Mode and uses the game's Command-L |
| Extras > Go to Level | jump to any level 1-14 while playing |
| Extras > Add 5 Minutes | 5 more minutes on the clock (as the game's own `+` cheat key, which adds 1), shown at once |
| Extras > Add One Health | one more life point; when the meter is full, one more flask (12 at most). Both can also get a key or a controller button in Extras > Controls |
| F5 / F9 (Option-Command-S / -L) | save state / load the newest one (keys can be changed in Extras > Controls). A state can be loaded in any level, after a restart too, and from the title screen (a game is started first) |
| Extras > Save States... | the last 10 save states with a picture, the level, the time left and the date; click a name to rename it; Load, Delete. They are kept in `~/Library/Application Support/PoP2-port/Save States` |
| hold Delete | rewind (about 15 seconds; Extras > Rewind turns it off) |
| Extras > Skip Copy Protection | on by default: the manual question is never asked (as ScummVM does for games of that era) |
| F1 (or a controller's Guide button) | on-screen menu, for controllers and keyboards: save states with pictures, go to level, add time / health, cheats, display, Mac menu bar, statistics |
| Statistics | per level: best time of a clean run (no extras, save state, rewind or cheat), completions, deaths; all time and this session |
| Messages | "State saved", "State loaded", health, rewind indicator: drawn over the picture, never into the game |
| Extras > Controls... | choose the keys and the game controller buttons; shows the connected controller with its own button names (Xbox, PlayStation, Switch). Controller defaults: d-pad or left stick move, A action, B second button, Start pause, Back time left, left shoulder rewind, right shoulder / Y quick save / load |
| View | Pixels, Sharp Pixels (whole multiples), Smooth, CRT Scanlines |

Game keys: arrows (or keypad) move, Shift or keypad `.` is the action button (grab,
strike, careful step), Control or keypad `0` the second button, Esc pauses, Space shows the
time left.

## Build

Requirements: Xcode command line tools, SDL2 from Homebrew (`brew install sdl2`).

```sh
make app BUILD=build/release CORE=mini     # build/release/PoP2 Port.app (no 68k emulator)
# release: Developer ID signature, notarization (credentials from `xcrun notarytool
# store-credentials notary`), zip in build/release/PoP2-Port-macOS.zip
make dist BUILD=build/release CORE=mini SIGN_ID="Developer ID Application: ..."
make packages          # Linux packages (Docker): Recalbox, desktop tarballs, PortMaster
tools/crosscheck.sh arm64 amd64   # scripted runs: same emulated memory on macOS and Linux
tools/release.sh 0.3   # all of the above, then a draft GitHub release with every package
```

Development builds keep the Musashi 68k emulator (MIT licence, `third_party/musashi`),
which runs the original code side by side with the port for verification:

```sh
make -j8 HOOK=1                            # build/pop2: game with the emulator available
build/pop2 path/to/PoP2.hfv                # run (window); --headless for tests
tools/check.sh                             # differential check, call by call (docs/PORTING.md)
tools/equiv.sh "Level 7" 60000             # whole-game equivalence: native vs original 68k code
```

Regenerating the function index and the Toolbox wrappers needs the Ghidra project and
`traps.json` (from the Multiversal Interfaces), which are kept outside this repository.

## How it is verified

- **Call by call:** in the check build every call of a ported function is replayed on
  the original 68k code in a forked process, and the complete 16 MB memory images and
  results are compared (`tools/check.sh`, 22 scripted scenarios: intro, new game, one
  saved game per level).
- **Whole game:** `tools/equiv.sh` runs a scenario fully native and fully on the original
  code with a deterministic clock and compares the sequences of Toolbox calls (millions
  of calls per scenario).

## Licence

MIT for the code written for this port (runtime, platform layer, tools). `src/game/` is a
translation of the original program and is not licensed; see [LICENSE](LICENSE).

## Layout

| Path | What |
| --- | --- |
| `src/game/` | the ported game, Mohawk libraries, THINK C runtime and music driver (original names) |
| `src/mac/` | Toolbox reimplementation |
| `src/cpu/` | loader, trap dispatch, Musashi glue, `minicore.c` (release core) |
| `src/fs/` | HFS image reader with a writable overlay |
| `src/platform/` | SDL video/audio/input, Cocoa menus, dialogs and app glue |
| `tools/` | Ghidra pipeline, index and glue generators, checkers |
| `docs/` | `PLAN.md`, `PORTING.md` (porting contract), per-unit notes |
