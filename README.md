# Prince of Persia 2 for modern macOS (unofficial port)

A port of the 1993 Macintosh version of *Prince of Persia 2: The Shadow and the Flame*
to current macOS. The original 68k program was decompiled with Ghidra and translated,
function by function, into C (about 2,160 functions plus the SoundMusicSys music driver);
a reimplementation of the classic Mac Toolbox (memory, resources, files, QuickDraw,
windows, menus, events, sound) runs it on top of SDL.

**No game data is included.** The app reads everything from your own copy of the original
game: an HFS disk image of the Macintosh version (for example `PoP2.hfv`). On first launch
it asks for that file and remembers it. Saved games and settings are written to
`~/Library/Application Support/PoP2-port/disk`; the disk image is never modified.

This is an unofficial fan project, not affiliated with or endorsed by the rights holders.

## Download

Get `PoP2-Port-macOS.zip` from the [Releases](../../releases) page (Apple Silicon Macs,
macOS 12 or later), unzip it and move `PoP2 Port.app` to Applications.

The app is not signed with an Apple Developer ID, so the first time macOS blocks it:
open it once, then in System Settings > Privacy & Security click **Open Anyway** (or run
`xattr -dr com.apple.quarantine "/Applications/PoP2 Port.app"`). It then asks for your
disk image of the original game.

## Extras

Additions of this port, outside the original game (they are off in the verification runs):

| Where | What |
| --- | --- |
| Extras > Infinite Health | the kid's life is refilled every frame (falls and traps still kill) |
| Extras > Infinite Time | the clock stops |
| Extras > Cheat Mode | the game's own debug mode (as when 1 2 3 are held at launch): `-`/`+` change the time, Command-L next level on any level; the letter keys no longer move |
| Extras > Skip to Next Level (Option-Command-N) | turns on Cheat Mode and uses the game's Command-L |
| Extras > Go to Level | jump to any level 1-14 while playing |
| F5 / F9 (Option-Command-S / -L) | quick save / quick load, within the level being played |
| hold Delete | rewind (about 15 seconds; Extras > Rewind turns it off) |
| Extras > Skip Copy Protection | on by default: the manual question is never asked (as ScummVM does for games of that era) |
| Extras > Controls... | choose the keys; game controllers work too (d-pad/stick, A action, B second button, Start pause, left shoulder rewind, right shoulder / Y quick save / load) |
| View | Pixels, Sharp Pixels (whole multiples), Smooth, CRT Scanlines |

Game keys: arrows (or keypad) move, Shift or keypad `.` is the action button (grab,
strike, careful step), Control or keypad `0` the second button, Esc pauses, Space shows the
time left.

## Build

Requirements: Xcode command line tools, SDL2 from Homebrew (`brew install sdl2`).

```sh
make app BUILD=build/release CORE=mini     # build/release/PoP2 Port.app (no 68k emulator)
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
