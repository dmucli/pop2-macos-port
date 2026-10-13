# seg10: CODE 10 "Temple", 0x3a4a4-0x3d4dc

The C port is `src/game/seg10_temple.c`, which has all 62 functions. Signature fixes are in `tools/sig_overrides/seg10.json`. The temple is level type 2 (`gs+0x2186`), which covers levels 10 to 13. `LoadCodeOverlays` calls `seg10_03b736` for level type 2.

## What the unit does

**Piece draw procedures.** These are entries of the per-level-type piece-proc table `0x103552` (see the seg03a/seg03b notes). Each one works on the current BF item `*0x103074`:
- Item fields: +0xa pass/kind (0 = back wall, 5/2 = front, 1 = floor, 0xb = sacred flame), +0xe/+0x10 col/row, +0x14 info, +0x16 back-wall info.
- `DrawTempBackWall` draws the wall shape in `info2 & 0x1f`. It skips the wall when the left cell's back-table image (`GetBackTableImgRow`) covers the clip, and unions that image's rect into `gDrawClip` (0x103a00).
  - It clips the top to the row (`0x1034b0[row]`) when the piece above is not visible, then adds the decorations: 0x1054d4, and 0x105532 + 6*((info>>12)&3).
- The other procs, in address order:
  - exits: `DrawTempExit1`, `FUN_0003aa02`, `DrawTempExitMovable` (the door, raised by `info & 0xff`), `FUN_0003a766`, `seg10_03b514`
  - gates: `seg10_03ad86`, `DrawTempGateMovable`
  - floor and wall edges: `FUN_0003ac12`, `FUN_0003ace8`
  - posts/columns: `FUN_0003aefe` (shape tables 0x105492/0x1054aa by column parity), `DrawTempPosts`
  - pressure plates: `DrawTempDownPlate`, `DrawTempUpPlate`
  - torches: `seg10_03b580`, `DrawTempTorch` (flame frame is `info & 0xf`)
  - sacred flame: `DrawTempSacredFlame`
  - others: `seg10_03b006`, `seg10_03b06e`, `FUN_0003b3fe`, `seg10_03b452`, `seg10_03b4a6`, `FUN_0003b6e0`
- Shapes come from the 'PIEC' table `*0x103462`, in 6-byte {shape, x, y} entries.

**Torches.**
- `TempGetTorchFrame` returns `Rnd(5)`, but never the previous frame.
- `TempVisibleTorch` handles torches in the bottom row of the room above. It rewrites its own argument slots: room = current room, cell += 20.

**Sliding rock.** The rock is a mob of type 6, stored in the mob record `gMob` (0x103b5a):
- +0 x, +2 y, +4 room, +6 speed (-1 = done, 1 = falling into a hole), +8 accel, +0xa type, +0xc row, +0xe stop x.
- The accel field is 0 when idle, -2 when triggered, 1..9 when accelerating and 0x64 when stopped.
- `TriggerSlideRock(room, cell, how)` creates or arms the rock. It may replace its room argument slot by the current room.
- `FUN_0003c1fc` is the move proc. It moves the rock left, stops it against walls, makes it fall into holes, and crushes the kid (`seg5_027e6e`, `seg6_02c0c4`).
- `ATMSlideRock` draws it. Its pieces are objects of types 6, 7, 8 and 9, using `FUN_0003b86a`, offsets 0x105790.. and `DrawObjSlideRock`.
- `CheckSlideRockCrush` and `FUN_0003bfa4` push the character back against the rock or a wall, or crush him (seq 0x47).
- Barrier support for the collision code (seg04b):
  - `GetSlideRockOnRoomRow` returns the mob with `GetMobTypeItem(6, n)`.
  - `GetSlideRockBarrDist` gives the rock's right side (dir 0) or left side (dir -1).
  - `GetSlideRockDetData` writes rooms/codes for `CheckBarr` and the previous-frame codes at 0x104aa2.
  - Also `ComputeSlideRockRect`, `SlideRockInCell`, `SlideRockNearSpikes` and `TempGetSlideRockX`.
- `IsAllFloorsInDirToRock(drow, dist)` takes two words, not one (override). It stores the clipped distance back into its `dist` slot.

**Trap door.** The trap door is a mob of type 10. Its frame is `gMobState & 0x1f`, mapped through 0x10579a to the 6-byte {shape, dx, dy} entries at 0x1057be.
- `TriggerTrapDoor` sets the cell spec to 0x81 and the tile to 0, then creates the mob.
- `seg10_03c9e0` (the character stands on it) and `seg10_03ca4a` (a shaking floor) trigger it.
- `FUN_0003c99a` animates it. Past frame 0x10, `ATMTrapDoor` opens the floor (tile 0x1a) and draws it. `DrawObjTrapDoor` is the DrawObj proc.

**Level 13 altar (room 4).**
- `seg10_03cc5a` runs every frame and calls `FUN_0003cf02`, which handles the kid falling near the flames, the music, and the shadow's appearance.
- `FUN_0003cfd2` turns the kid record into a new opponent record (the shadow).
- `seg10_03ccf0` (from seg06 `FUN_0002f584`) drives the cut-scene by frame. `FUN_0003cc78` is the merge with the shadow.
- `IsAltarShadowActive` returns whether a room character has type 1.

**Flames.**
- `FUN_0003d37c` loads the flame shapes and palettes. `seg10_03d4c8` is a room hook that sets the palette.
- `GetFlameImgHnd` and `GetFlameImgSize` handle images 0xfed..0x101a, whose handles are at `*0x105480`. `GetFlameImgSize` stores 0 into its pointer argument slot on failure.
- `MakeFlameFCharClipRect` sets the object clip 0x104b44. `SetupFlame` builds the flame sword's flame object.
- `FUN_0003d162` is the trob proc of the sacred flame (custom room template 0x20, frame table 0x105484).

## Porting notes

**Own-argument writes.** `TempVisibleTorch`, `ComputeSlideRockRect`, `IsAllFloorsInDirToRock`, `TriggerSlideRock` and `GetFlameImgSize` write into their own argument slots. They do it through `args_` from `ENTRY_LINK`, which is the real argument slots when the function is entered from 68k code and the dead reserved area when it is called from C. No hand-written glue is needed.

**Return sizes (`ret` overrides).**
- `ret: 0`: D0 is leftover (AddNextBFItemToTable's high word, EmptyRect's byte, or trap results), and no caller uses it. Every caller was checked: the piece-proc table callers in seg03a/seg03b, `DrawScythe2`/`FUN_00029c48`, `CheckBarr`, seg02b and `Falling`/`ShakeEm`. The functions are `DrawTempBackWall`, `FUN_0003a766`, `DrawTempExit1`, `FUN_0003ac12`, `FUN_0003ace8`, `DrawTempGateMovable`, `FUN_0003aefe`, `DrawTempTorch`, `GetSlideRockDetData`, `FUN_0003c1fc`, `seg10_03c9e0` and `seg10_03ca4a`.
- `ret: 2`:
  - `seg10_03b452`, because `FUN_000299d8` (seg05b) returns its D0 (AddNextBFItemToTable's result, or the item kind).
  - `SlideRockInCell`, because its callers test only the word.
- **Exact D0 kept.**
  - `TempGetTorchFrame`: Rnd's high word.
  - `IsAllFloorsInDirToRock` and `FUN_0003baf6`: the quotient's high word.
  - `CheckSlideRockCrush`: the low word on every path (0xffff after the dbra copy).
  - `SlideRockNearSpikes`: zero-extended.
  - `MakeFlameFCharClipRect`: the 0x104b44 pointer.

**Extra word.** The original pushes an extra word (-1 or gChar.id) to `MarkBufsForRect`. As in the other units, the port passes only the two parameters.

## Finding outside this unit (affects the check)

`VisibleAbovePiece` (seg03b, ported) does not reproduce its `clr.w D3`. In the original, `PieceAboveRightId` returns its caller's D3 when there is no room above, and that is 0 there. The ported `PieceAboveRightId` (seg05a) returns `cpu_get(R_D3)`, so when VisibleAbovePiece is native it returns a stale register instead.

This shows up when `DrawTempBackWall` is native. Its own D3 (the drawn row) is no longer the value the 68k version left. `DoCleanCut` then mismatches in the temple saves (Level 10, 13, 13:2): 12 BF item rects differ.

Evidence:
- With `POP2_NATIVE_EXCLUDE=VisibleAbovePiece` every check passes.
- So does a local, uncommitted change of `VisibleAbovePiece` that clears the low word of D3 around its body: `savd3 = cpu_get(R_D3); cpu_set(R_D3, savd3 & 0xffff0000)` ... `cpu_set(R_D3, savd3)`.
- The minimal native set that reproduces the mismatch, found with ddmin, is {DoCleanCut, DrawTempBackWall, VisibleAbovePiece, main_, ResetGame}.

The fix belongs in seg03b. I did not edit it.

## Verification

- `tools/check.sh` (master's version with all levels) shows only the DoCleanCut mismatch described above. With `POP2_NATIVE_EXCLUDE=VisibleAbovePiece` there are no mismatches.
- 31 functions are verified by ordinary play, using `POP2_NATIVE_ONLY` groups ordered by in-unit call depth and `CALLS=300` on the temple saves: the piece procs, torches, the rock barrier queries, `seg10_03c9e0`, `seg10_03cc5a` and `seg10_03b736`.
- The rest are not reached by the scenarios: the sliding-rock movement/crush, trap doors, the altar, flames and the sacred flame.
  - I verified them with a temporary self-test driver, which was not committed. It was hooked into `seg10_03cc5a` at frame 600/700 and called the functions through their 68k entries with crafted state:
    - BF items of every kind and many info values for all piece procs (3450 calls)
    - rocks and trap doors created in the kid's room
    - frames/faces for the altar code
    - a fake custom room for the sacred flame
    - flame shapes
  - All 62 functions were checked with no mismatch.
  - A crash after the trap-door tests also happens with the trap-door functions on the 68k core. It comes from the crafted state.
