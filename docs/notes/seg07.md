# seg07: CODE 7 "Ruins" (0x305b4-0x33fb2)

All 66 functions are ported in `src/game/seg07_ruins.c`. Signature fixes are in
`tools/sig_overrides/seg07.json`.

## What the unit does

The ruins levels are levels 6 to 9 (level type 4 at `gs+0x2186`, opponent kind 5 at
`gs+0x21a4`). The unit has three parts: the flying heads, the snakes and the piece drawing
procedures of the ruins.

### Characters

The unit uses the usual character records (0x3e bytes): `gChar` 0x102faa (the character
being processed), `gOpp` 0x102f6c (its opponent, here the kid) and `gKid` 0x102f2e. The
fields it uses are +2 face, +4 x, +6 y, +8 type, +0xa frame, +0xc col, +0xe row, +0x16 room,
+0x1a, +0x1c alive (<0 alive), +0x1e life, +0x28 current sequence (set by `JumpSeq`),
+0x32 and +0x34 (per-type state words).

Per-room character info records (0x26 bytes, `GetOppInfoIdx(room, id)`) are at
`gs + 0x21a6 + room*0xc0 - 0xbe + id*0x26`, and the count is at `gs + 0x20e6 + room*0xc0`.
The fields are:
* +0 cell, +2 x, +4 face, +6 skill, +8 initial sequence (0x82 for heads, 0xaa for snakes)
* +0x18 kind (5/6 heads, 8 snakes)
* +0x1c..+0x24 the head or snake state

`HeadAddOpponent` is called by `FUN_0002d1be` for types 7, 8 (heads) and 0xb (snakes).

### Flying heads (types 7 and 8)

`seg7_0318d4` is the per-frame control. Its state block `p` is the info record + 0x1c:
* +0 program: 0 idle, 1 attack, 2 flee, 3 locked on the kid's neck
* +2/+4 destination column/row
* +6 "sees the kid" flag, or the flee countdown
* +8 "flee song played" flag

The programs are run by `FUN_00031818` (idle wandering, sequences 0x83-0x8a),
`HeadAttackProg`, `HeadFleeProg` and `HeadLockedProg`. The state changes come from
`HeadGetAngry`, `HeadGetPeaceful`, `HeadDoneAttackingKid` and `seg7_031a04` (flee after
being hurt, which needs `IsAbleToFleeInRoom`).

The helpers are:
* `CharCanSeeOpp`: a Bresenham line of sight in half-cell steps. It returns -1 when blocked,
  4, or a direction (0 left, 1 right, 2 down, 3 up).
* `HeadClearInDir`, `HeadGoToDest`, `FUN_0003120e` (turn towards the kid),
  `HeadFocusOnNeck`, `ChompAgain` (biting chances from the tables at 0x10532e/0x105346 by
  skill), `FUN_00030eaa` (the kid's sword hits the head, called by `TestStrike`).
* `HeadEnemyCollide` (barrier collision, called by `EnemyCollide`), `AlignHeadOnKidNeck`,
  `FUN_00031cba`, `X/YWithinMiddleOfCell`, `IsHeadLockedOn`.
* `seg7_033b26` is the `ClipChar` hook for heads.

### Snakes (type 0xb)

`FUN_00031dd4` is the per-frame control. The info record's +0x1c is a countdown while the
snake is in its hole (`SnakeComeOutOfHole`) and -1 once it is out (`FUN_00031e2e` strikes at
the kid; `SnakeGoIntoHole`; `FUN_00032376` tells whether it faces the kid).
`SnakeUpdateOppInfo` saves its state. The repeated "solid floor in front" test is the static
helper `snake_floor`.

### Ruins pieces

These are piece procedures from the per-level-type table `gPieceDrawProcs` (0x103552), plus
their helpers. They build BF items through `gCurBFItem` (0x103074). The item's layer is +0xa
(0 back wall, 1, 2, 5) and the info is +0x14. They use the piece table `gLevelPieces`
(0x103462) and the shape/x/y triples at 0x105364, 0x10540c and 0x1053be.

* `DrawRuinBackWall` draws the back wall. It skips the wall when the image is already in the
  back table: `GetBackTableImgRow`, then SectRect/UnionRect through the Pascal pointers at
  0x1060a8/0x1060a4.
* `FUN_00032dde` adds the decorations.
* The other procedures draw:
  * crevices: `DrawRuinCrevice`, `FUN_00032756`
  * the exit: `FUN_00032a7a`, `seg7_032b64`, `DrawRuinExitMovable` (door)
  * the gate: `seg7_032fce`, `DrawRuinGateMovable`, and `FUN_00033dea` (custom image 6,
    used in room 9 of level 8)
  * plates: `DrawRuinDownPlate`, `FUN_000334d6`
  * posts and rubble: `DrawRuinPosts`, `DrawRuinRubble`, `FUN_000337ac`
  * floors and walls: `seg7_032f0e`, `FUN_00033186`, `seg7_032864`, `seg7_0332c0`,
    `seg7_033352`, `seg7_033866`
* `gNoTopRow` (0x1037ce) is set while the row above the room is drawn.
* Crevices: `FUN_000339b4` → `RuinChangeCrevices` → `RuinSetCreviceRevealed` (spec bit
  0x80). `seg7_033c50` makes a crevice cell fall as a mob of type 3.

### Level hooks

`seg7_033cee` is the `CutChar` hook. It puts the kid in room 3, column 4. The room template
hooks are `seg7_033d56`, `seg7_033d66`, `seg7_033d86`, `seg7_033e14` and `seg7_033e54` (they
set palettes and start music). `seg7_033e7a` plays NIS 6, then reloads level 8 and keeps the
game state block.

## Porting notes

* **Functions that write their own argument slots.** These are `"conv": "manual"` with a
  hand-written glue that writes the slot back:
  * `XWithinMiddleOfCell` and `YWithinMiddleOfCell` store the position within the cell into
    their first argument.
  * `RuinChangeCrevices` uses its column argument as the loop counter.

  All their callers are in this unit, so the glue only runs when those callers are on the
  68k core.
* `RuinChangeCrevices` computes its column range as `max(col+3, 10)` and `min(col-2, 0)`.
  This is the original's behaviour (probably an inverted MIN/MAX), and it is kept.
* `seg7_033e7a` pushes a third word (10) to `MarkBufsForRect`, which does not read it. The
  port pushes that word too, so the callee's stack is where the original's is.
* `pascal_ptr3` (SectRect/UnionRect through the cached pointers) is the same helper as in
  seg05a.
* No override outside the unit was needed.

## Signature overrides

* `conv: manual`: `XWithinMiddleOfCell`, `YWithinMiddleOfCell`, `RuinChangeCrevices`.
* `ret: 2`: `CharCanSeeOpp` and `ChompAgain`. They return a word with garbage in the high
  word, and their callers only test the word.
* `ret: 0`: D0 is garbage and no caller uses it (all callers checked in Ghidra):
  * head and snake functions: `FUN_0003120e`, `HeadGoToDest`, `seg7_031a04` (callers
    `FUN_0002f832` and `FUN_00030eaa` ignore it), `FUN_00031e2e`, `SnakeComeOutOfHole`,
    `SnakeUpdateOppInfo` (`SetSpecialOppInfo` ignores it)
  * piece procedures: `DrawRuinBackWall`, `FUN_00032756`, `DrawRuinCrevice`, `FUN_00032dde`,
    `DrawRuinGateMovable`, `FUN_00033186`. They are called through the piece table, whose
    callers ignore D0.
  * `RuinSetCreviceRevealed`
* The other non-void functions reproduce D0 exactly:
  * `HeadAddOpponent` returns its argument.
  * `IsHeadLockedOn`, `FUN_00031cba` and `X/YWithinMiddleOfCell` return 0 or 1 as a long.
  * `seg7_033d86` and `seg7_033e14` return the word of the last value read, AddSong's D0,
    or 1.

## Verification

* **Standard run.** `tools/check.sh` (master version, 22 scenarios) shows no mismatch and no
  crash. 37 functions of this unit are verified directly. The ruins levels are 6-9, and the
  scenarios start in rooms with snakes but no heads.
* **Extra runs with temporary hooks (not committed).**
  * Levels 6, 7, 8 and 9 were loaded, and the kid was teleported into every room 1-32 by
    setting his room and `gNextRoom` (0x10303c), so that `PrepCut` switched rooms. These
    runs were done with CALLS=5 and with the usual moves: 128 runs, no mismatch.
  * Runs with `POP2_NATIVE_ONLY=<all functions of the unit>` let the 68k callers
    (`DrawBackImg`, `ClipChar`, the per-type dispatch) reach every function through its
    hook: no mismatch.
  * A temporary driver called the rarely reached functions through their checked entries:
    * `HeadFleeProg` in all its branches
    * `FUN_00030eaa`, `seg7_031a04` and `IsAbleToFleeInRoom`
    * the manual glues of `X/YWithinMiddleOfCell` and `RuinChangeCrevices`, called from
      68k
    * `seg7_033cee`, `seg7_033d56`, `seg7_033d66`, `seg7_033d86` (every path),
      `seg7_033e14` (every path) and `seg7_033e54`
    * `seg7_033e7a` (the full NIS 6 and level 8 reload)

    All of these were verified ok.
* **Every function of the unit was verified by the checker except `FUN_00033dea`.** The
  levels on the disk have no custom room data for room 9 of level 8. `FUN_00033dea` was
  reviewed against the disassembly instruction by instruction. The empty functions
  `FUN_000305b4`, `FUN_00031816` and `seg7_0338d8` are trivially correct.
