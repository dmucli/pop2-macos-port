# Prince of Persia 2 (Mac, 1994) — port to modern macOS

## What the original is

- 68k Mac application, THINK C / Symantec C++ 7 (16-bit `int`, A5-relative globals,
  near-model CODE segments with a jump table). 23 CODE segments, ~400 KB of code,
  1,377 functions carry MacsBug names in the binary.
- Built on Broderbund's **Mohawk** portability layer (segments "Mohawk Libs 1-4",
  "Anim & Audio", "PoP Tools"): file*/mem*/res*/timer*/os* APIs, a Windows-style
  `waveOut*`/`midiOut*` audio layer, LZSS packing, RLE sprite blitters.
- Music: Halestorm *SoundMusicSys* "MIDI Synth 3.45" driver, shipped as an
  encrypted + LZSS-compressed 68k code resource (`MDRV` 11), plus `SMOD` filters.
- Data: resource forks in `Prince2/Data/*.rsrc`, sound banks in `*.dat` data forks.

## Port architecture

```
 +--------------------------------------------------------------+
 | game/      ported game + Mohawk code, C, original names       |
 +--------------------------------------------------------------+
 | mac/       Toolbox reimplementation (Memory, Resource, File,  |
 |            QuickDraw 8-bit, Palette, Events, Time, Sound...)  |
 +--------------------------------------------------------------+
 | platform/  SDL2: window, 8-bit framebuffer -> texture, audio, |
 |            keyboard/mouse                                      |
 +--------------------------------------------------------------+
 | cpu/       Musashi 68020 core + loader  (oracle / fallback)    |
 +--------------------------------------------------------------+
```

**Memory model.** The game keeps running against a flat, big-endian 32-bit
"classic Mac" address space (`mac/mem.h`: `rd16(addr)`, `wr32(addr, v)` ...).
Handles, pointers, Toolbox records (GrafPort, PixMap, GDevice, EventRecord ...)
and the A5 globals live there with their original layouts. This keeps exact
16-bit arithmetic, struct offsets and save-game/resource formats, and lets
ported C and not-yet-ported 68k code share data during the transition.

**Incremental port.** The Musashi core runs the original code with all line-A
traps serviced natively by `mac/`. Ported C functions are registered by their
original address; when 68k code calls one, a line-F thunk diverts to C. When C
calls something not yet ported, it runs it on the CPU. Each ported function can
be differential-tested against the original (same inputs, compare memory and
registers) — the "prove it by emulating the original" method.

**End state.** Every game/Mohawk function is C; the CPU core is only used by the
test harness (and to run the MDRV synth until it is ported too).

## Rules

- No publisher data in the repo. The port reads the user's own `PoP2.hfv` (or an
  extracted folder) at run time.
- Keep original function names (MacsBug symbols). Name globals `g_<name>` once
  understood; until then `G16(off)`/`G32(off)` with the A5 offset.
- THINK C `int` is 16-bit: port with `int16_t`/`uint16_t` and keep wraparound.

## Phases

1. Ghidra project + pipeline (done: `tools/ghidra/build_project.sh`).
2. Runtime: memory, loader, CPU glue, trap dispatch, Toolbox HLE, SDL platform.
   Goal: original code boots to the title screen and plays.
3. Port segment by segment from the Ghidra decompile, with differential tests.
4. Port the MDRV synth; package `.app`; docs.

## Status (2026-10-13)

- Every function of the game is ported (CODE 1–23: 2,161 of 2,165; the 4 left are THINK C
  helpers that change their own return path and are not needed), plus the MDRV music driver.
- Verified call by call (`tools/check.sh`) and as a whole (`tools/equiv.sh`: same Toolbox
  call sequence as the original on every saved level, 60 s each, except one byte of stack
  garbage the original passes in `BuildOffGWorld` on Level 4).
- The release build (`make app BUILD=build/release CORE=mini`) contains no 68k emulator:
  `src/cpu/minicore.c` executes only the dispatch instructions that remain (hook words,
  traps, jump-table entries, two small stubs).
- macOS app: native menu bar, native Open/Save dialogs, disk-image chooser, Dock icon from
  the user's copy; no publisher data in the bundle.
