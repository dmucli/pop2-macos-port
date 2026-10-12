# seg03a: CODE 3 "Modular Set 1", 0x17674-0x1a8ae

File: `src/game/seg03a_modularset1.c`. All 61 functions are ported.

## What the unit does

This is the room renderer's item bookkeeping and drawing.

* **BF items** (0x26 bytes each) are background and foreground pieces collected per cell.
  - The tables are `gBackTable` (0x10335e, max 0x82 items, count at 0x1037d2) and `gForeTable` (0x10335a, max 0x78 items, count at 0x1037d4).
  - The item being filled is `gCurBFItem` (0x103074).
  - Fields: +0 shape set, +2/+4 col/row of the drawn cell, +6, +8 flip, +a layer (1 or 2 = foreground, otherwise background), +c, +e/+10 col/row of the source cell, +12 piece, +14 info, +18 shape, +1a x, +1c y, +1e clipped rect.
  - `ResetNextBFItem`/`CheckNextBFItem` start an item.
  - `DrawBackImg` (passes 0-3: below-left, below, left, own cell) and `DrawForeImg` (passes left, own) fill the col/row/piece/info fields and call the per-piece draw routine from the table at `0x103552`.
  - The per-piece routine sets the shape and calls `AddNextBFItemToTable`. That function positions the item (x += colX[col], y += rowY[row] - shape height), intersects it with `gDrawRect` (0x103a00) and commits it.
  - `DumpTable(0/1/3)` draws the back table, the fore table and the mid table with `DrawBFItem`/`DrawMidItem`.
* **Mid items** (0x18 bytes, `AddMid`) are objects. They live in table 0x103356 (count 0x1037d8), or in table 0x103352 (count 0x1037d0) when the FChar kind (0x104b38) is 100.
* **Character objects** (0x1c bytes, table 0x10334e, count 0x1037da, max 30) are added by `AddCharObj` from the "FChar" globals 0x104b38..0x104b4e.
  - `FUN_00017db8` handles the kid, his sword/flame/weapon and level effects (splash, sand, carpet, flame).
  - `Opponent1` handles the opponents of the room (table 0x102e0e, 0x3e-byte entries).
  - `SetupOpponent`, `SetupSword`, `FUN_00018214` (head), `FUN_0001839a` (flame) and the other setup functions fill those globals.
* **Peels** (0x12 bytes, table 0x10307e, count 0x10307c, max 40) are save-under rectangles: +0 saved bits, +4 rect, +c id, +e kind, +10 keep flag.
  - `AddPeel` saves the area.
  - `Peel` restores the areas, last first, and compacts the table.
  - `MarkPeelForeBufs` marks the buffers under the peels.
* **Custom rooms** use 'CUST' resource id room*25+0xf87. The locked data is in 0x103362 and the handle in 0x103366.
  - Data fields: +0 template id, +2 shape base, +4 max shape, +6 loaded, +8/+a sounds, +e, +1c piece count, +1e pieces of 0x20 bytes.
  - Piece fields: +0 shape (-1 = none), +2 y, +4 x, +6 layer, +8, +a shape id, +c handle, +10 rect, +18, +1a kind (1 or 2 = peel, 3 = desert stone).
  - The room template (0x10345e) holds the {load hook, unload hook} pair taken from the table 0x1033d2 (default 0x1033ca).
* **Fast redraw**: `RedrawCellFast`/`RedrawTopFast` redraw what the per-cell flag tables at 0x103c1c..0x103c34 ask for.
* `seg3_019120` computes the neighbour rooms. The room links are 4 words (left, right, up, down) at gamestate+0x2080+(room-1)*8, and the results go to 0x103040..0x10304e. `IsCustomizedRoom` sets 0x103050.

The globals the code calls through (Pascal convention, `jsr (A0)`) are 0x10609c ClipRect, 0x1060a0 EmptyRect, 0x1060a4 UnionRect and 0x1060a8 SectRect. The names come from usage. These calls go through the local helper `pas_call`, which reserves the result word, pushes the arguments and lets the callee pop them.

## Signature overrides (tools/sig_overrides/seg03a.json)

* `AddNextBFItemToTable` and `AddMid` get `ret: 2`. They return a 16-bit flag (`move.w Dn,D0`), and their callers test only `D0.w`. The high word of D0 is whatever was there before.
* `FUN_00018138`, `FUN_000183f8`, `RedrawCellFast`, `DrawCustomRoomImgs`, `DrawCustomRoomImg`, `CheckGateInFore` and `DrawBFItem` get `ret: 0`. Their D0 is leftover junk. I checked every caller (`DrawCaveBars`, `DrawDsrt*`, `DrawFinl*`, `SetupSand`, `CutToKidRoom`, `EnemyCollide`, `FUN_00037e56`, `FUN_0001a8d0`, `DrawFloorBuf`, `DrawHalf`, ...): none of them uses D0 after the call.
* The other non-void functions return exactly the original D0, including oddities:
  - `FUN_00018dd0` keeps the high word of the custom-room pointer.
  - `RestoreKeepCustomPeel` returns 0xFFFF0000|index.
  - `FUN_00018f66` returns 0x23.
  - `UnloadCustomRoomShapes` returns DisposePtr's D0.
  - `FUN_0001a2e0`/`FUN_0001a30c` return 0 or 0x18.
  - `FUN_00018e62` returns the piece count.
* No override was needed outside the unit. `MarkBufsForRect` is called with an extra third word: the callers push the peel id or the character id, but `MarkBufsForRect` never reads it. The port passes only the two arguments in the prototype.

## Faithfulness notes

* `Opponent1` keeps its "last char object" pointer in A2 and sets it only when the action is not 9. On other paths it writes through the A2 it inherited. The port starts from `cpu_get(R_A2)` to reproduce that.
* `CheckGateInFore` reads the object type from the character entry *after* the matching one, because the pointer has already been advanced. This is reproduced.
* `LoadCustomRoomShapes` passes the code-segment string "Shape not found in LoadCustomRoomShapes()" (0x18da6) to `Terminate` through `fn_address(FN_LoadCustomRoomShapes) + 0x30c`.

## Verification

`tools/check.sh` results:
* 24 functions of the unit are verified ok. This includes `DumpTable` and `FUN_00017792` in the scenarios where they match. The total of 26 that check.sh prints also counts 2 seg23 functions.
* `TransferBFItemMidFields` is entered from 68k code, but only inside other checked calls, so the checker never checked it on its own.
* 25 functions are only called from other ported code in this file. They run inside the checked calls of their callers.
* 11 never execute in the scenarios: `GetPeelIdxForCharIdx`, `FUN_0001839a`, `FUN_0001850c`, `AddCustomPiecePeel`, `DrawCustomRoomImgAtInfo`, `CustomPieceOffsetToColRow`, `GetCustomPieceShape`, `GetCustomPieceSize`, `RestoreKeepCustomPeel`, `FUN_00018e62` and `SetCustomPieceRect`. I measured this with temporary entry counters. They were reviewed line by line against the disassembly.

Remaining reported mismatches:
* `DumpTable` in newgame, Level 7 and Level 8.
* `FUN_00017792` in newgame, call 20.

They are not translation errors:
* The only differing bytes (0x174e5f, 0x174e6f, 0x1754bf) are in the two 1616-byte buffers allocated by `SetupMIDIPort`.
* They are written by the MIDI driver's Time Manager task while it runs during the long call. That task reads uninitialised stack, and the stack pointer differs between the native call and the original. The original `DumpTable` does `link` + `jsr` before its callee, which is 8 bytes deeper.
* Proof: temporarily making `DumpTable` and `FUN_00017792` reproduce the original stack layout (SP adjusted, saved A6 and return addresses written) made all of these mismatches disappear. I did not commit that hack.
* With the idle-slice clock increment disabled, a full 30 s "Level 7" run gives identical trap sequences, natively and with `POP2_NATIVE=none` (3,749,953 traps).
