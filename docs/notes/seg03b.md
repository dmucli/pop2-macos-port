# seg03b: CODE 3 "Modular Set 1", second half (0x1a8be-0x1dab6)

All 74 functions are ported in `src/game/seg03b_modularset1.c`.

## What the unit does

- **Room redraw and dirty rects.** `FUN_0001a8be`/`FUN_0001a8d0` redraw every cell of the room (rows 2..0, columns 0..9 through `RedrawCellFast`), then the bottom row of the room above (`RedrawTopFast` with `gCurRoom` temporarily set to `0x103044`). The objects are drawn last, through `DrawObjs(-1)`.
  - Dirty rects are kept in `gDRects` (0x103664, up to 45 Rects) and counted by `gNumDRects` (0x1037cc). `AddDRect` merges an overlapping rect (inset by -1) or appends one. `FUN_0001bb60` copies all of them to the screen (`PrinceCopy`). `seg3_01a9a0` flushes a frame: it peels and dumps the tables.
- **Cell drawing.** The current cell is described by these globals:
  - `gCellCol`/`gCellRow` (0x103a08/0x103a0a) and its rect `gCellRect` (0x103a00).
  - `gCellPiece` (0x1037ec) and `gCellSpec` (long, 0x1037e8).
  - The left neighbour: `gLeftPiece`/`gLeftSpec` (0x10362a/0x10362c).

  Pieces are drawn by procs from the per-level-type table `gPieceDrawProcs` (0x103552), which `SetPrinceLevelSpecPtrs` selects from 0x103516/0x103532. `DrawHalf`/`ComputeHalfRect` draw the "half" pieces 0x87-0x90 (rects at 0x1034ba). `DrawFloorBuf` draws the floor of the current and the left cell, clipped against the slide rock on level type 2. `FUN_0001bd6c` (flask with bubbles, `GetFlaskBubbleColor`) and `DrawSword` are piece procs that build BF items through `gCurBFItem` (0x103074).
- **Objects.**
  - `gObjTable` (0x10334e) holds objects of 0x1c bytes, and there are `gNumObjs` (0x1037da) of them. The fields are +0 x, +2 y, +4 shape list, +6 shape, +8 piece, +0xa, +0xc kind, +0xe Rect, +0x16 linked object (-2 = already drawn, 100 = none), +0x18, +0x1a.
  - `DrawObjs(-1)` fills `gSortedObjs` (0x103632) and bubble-sorts it with `CompareObj`, which uses the kind priorities in `GetObjForePriority` and then y/x. It then draws each object and its linked object with `DrawObj`.
  - `LoadObject` copies an object into the `gObj*` globals at 0x104b38-0x104b4e. `DrawObj` dispatches on the kind to `AddMid` or to the `DrawObj*` routines of other segments.
- **Status bar.** `DrawMessage`/`DrawRestartMessage` draw text with a shadow (`StringRect`, font 0xd4 size 14) into `gStatusRect` (0x103024). `ClrMessage`/`ClrLifeBar` erase it. `DrawKidMeter` and `DrawOppMeter` draw the life bulbs with `PrincePaintShapeIdx` (shapes 0x129-0x12c of `gMeterShapes` 0x103570).
- **Horse** (levels 9, 10, 14): `GetHorseShape`, `GetHorsePtInPlane` and `DrawHorse` (tables 0x10335e/0x10335a, 0x26 bytes per item).
- **Piece classes:** `IsFloor`, `IsImpassable`, `IsLandOnable`, `IsMobFloor`, `IsSpace`, `IsWall`, `VisibleAbovePiece`.
- **Palettes.**
  - `PoPSetPalette` installs a 'CTBL' (optionally remapped). While a game-play palette is saved in `gSavedPalette` (0x103564), it only patches the saved copy.
  - `SaveGamePlayPalette` saves the palette, `FUN_0001c5a0` restores and frees it, and `SetPaletteToColorIdx` fades everything to one color.
- **Loading and unloading.**
  - `LoadLevel` does the following, in order:
    1. Shows the watch cursor.
    2. Calls `UnloadLevel(3)`.
    3. Loads the 'LEVL' map (`LoadLevelMap`) into the game state block (`gGameState` 0x102fec; type at +0x2186, number at +0x218a).
    4. Loads the code overlays, then the sets: flask, BG, kid, opponent and sword.
    5. Loads the sounds per level (tables at 0x103e9e) and the piece table 'PIEC' 0xdac (`gLevelPieces`).
  - Shape lists live in `gShapeLists[5]` (0x103568): 0 sword, 1 flask, 2 kid, 3 opponent, 4 BG.
  - The BG set table `gBGSetTable` (0x103578) has an 8-byte header with the count at +6, then 10-byte entries (+2 "needs load" flag, +6 shape handle). `MarkBGSetPurgable` purges or reloads it.
  - Resource files: level type 0x1038dc, main 0x10385a, Mohawk 0x1037ee and opponent sets 0x1039b6. The unload depth of `UnloadLevel` and `UnloadFrom` is 0 = everything to 4 = level extras only.

## Porting decisions

- **The stack is mirrored exactly.**
  - When a ported function is entered from 68k code, the return address is already popped, so `emu_sp()` is the original entry SP + 4.
  - `ENTER_LINK(N, R)` and `ENTER_REGS(R)` reserve exactly the original's stack use: the return address slot, the LINK frame and the saved registers. They release it automatically on return (`__attribute__((cleanup))`). `a6 - x` is the original's `(-x,A6)`.
  - This was needed: `FUN_0001d382` → `OpenMohawkRsrcVerify` stores a stack address in a global at A5-0xb88, and stale-stack reads in callees mismatched without it.
  - Native-to-native calls inside the unit do not push their arguments, so a nested callee runs 4-12 bytes higher than in the original. Every function was also verified on its own, with only it native.
- **Some originals write into their own argument slots,** which are in the 68k caller's frame: `ComputeHalfRect`, `DrawOppMeter`, `PoPSetPalette` and `GetBGSetImgSize`. They are `"conv": "manual"` with a hand-written `glue_*` that reproduces the writes. C callers go through a version without slots.
- **Cached trap pointers.** At startup (0x4ddf6) the game stores the addresses of SectRect, UnionRect, EmptyRect and ClipRect at A5-0x1f58/-0x1f5c/-0x1f60/-0x1f64 and calls the traps through them. The port calls `_SectRect`, `_UnionRect`, `_EmptyRect` and `_ClipRect` directly.
- **Argument fixes.**
  - `DrawObjLoose` takes a word argument that the decompiler missed (override `params [[4,2]]`).
  - `DrawObj`/`DrawObjLoose` push a word for `DrawObjCarpet`, `DrawObjTrapDoor`, `DrawObjBrgFloor`, `DrawObjFireball`, `DrawObjGlass` and `DrawObjCaveLoose`, which ignore it. Their prototypes are left as they are, and `call_pushing_word` pushes the word so their stack matches.
  - `GetNthvRGB` gets its index as a pushed byte, which is the high byte of the word.
- **Code-segment string.** The "DRects Overflow.  Quit?" literal in `AddDRect` is addressed at runtime: `fn_address(FN_AddDRect) + 0xa0`.
- **`ret: 0` overrides.** These were applied where D0 is left over from a trap, from the caller, or from a callee's garbage upper word (`AddMid`'s D0 carries the caller's D0 upper word), and every caller was checked to ignore D0: `CheckAddDRect`, `DrawHalf`, `DrawFloorBuf`, `DrawObj`, `DrawRestartMessage`, `LoadFlameShapes`, `LoadLevel`, `LoadSwordSet`, `SetKidPalette` and `LoadOppSet`. All other return values are reproduced.
- **Oddities kept as they are.**
  - `GetBGSetImgSize` does not store the shape it loads back into the table.
  - `DrawHorse` clears `gCurBFItem` when it is set.
  - `DrawFloorBuf` has a dead block at 0x1af84.
  - `UnloadLevel` passes the level *type* as the "old level" to `LoadLevelSounds`.

## Verification

`tools/check.sh` passes with no mismatches. The default run uses `CALLS=30`; runs with `CALLS=200`, and with `POP2_NATIVE_ONLY=<fn>` (`CALLS=150`) for every function that is only called from other ported code, pass too.

These were never exercised by the scenarios and were reviewed again against the disassembly: `DrawHorse`, `DrawMessage`, `DrawObjLoose`, `DrawSword`, `FUN_0001c0ac`, `FUN_0001c94a`, `GetFlaskBubbleColor`, `GetHorseShape`, `SetPaletteToColorIdx` and `TweakKidShpList` (empty). `FUN_0001bd6c` ran only 6 times.
