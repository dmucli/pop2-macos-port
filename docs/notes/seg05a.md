# seg05a: CODE 5 "Modular Set 2", 0x24184-0x2746a

The C port is `src/game/seg05a_modularset2.c`, which has all 83 functions. Signature fixes are in `tools/sig_overrides/seg05a.json`.

## What the unit does

**Room geometry.** The game state block lives at `*0x102fec`.
- Piece ids: 30 words per room at `gs + (room-1)*0x3c`.
- Specs: 30 longs per room at `gs + 0x708 + (room-1)*0x78`.
- Room links {left, right, above, below}: at `gs + 0x2078 + room*8`.
- Number of rooms: `gs+0x2180`. Level type: `gs+0x2186`. Level number: `gs+0x218a`.

`CalcBlue(room)` points `0x1037e0` (tiles) and `0x1037dc` (specs) at one room. The current room and its 8 neighbours are words at `0x10303e..0x10304e`, in this order: cur, left, right, above, below, aboveL, aboveR, belowL, belowR.

**Drawing support.**
- `GetObject(room, col, row, &spec, dflt)` returns a piece id and its spec. Column -1 comes from the left-room cache (`0x103612`, filled by `seg5_024cd0`) and column 10 from the right-room cache (`0x103600`).
- `GetAboveAry` and `GetBelowAry` fill the row caches at `0x1035be` and `0x10357c`.
- `FUN_00024bcc` sets up the clip and the left/self/right pieces of the cell being drawn. That cell is col `0x103a08`, row `0x103a0a`.
- `PieceXxxId` and `GetRoomID` look up neighbouring pieces.
- `ComputeClipRect`, `ComputeOffsetCellRect` and `CellToCol/Row` compute cell rectangles.

**Trobs** are animated objects: 8 bytes `{cell, room, phase, type}`, at most 20, in the list at `0x103b72` with the count at `0x103c1a`. The current trob is copied to `0x103c12`.
- `AnimTrobs` runs `AnimTrob` on each one. `AnimTrob` dispatches on type, which covers gates, plates, flasks, exits, torches and many level-specific objects in other segments.
- `AnimTrobs` then compacts the list, dropping entries with phase < 0.
- `GetOS(cell)` loads the current object: spec long at `0x1037e8`, id at `0x1037ec`.
- The `RedrawXxx` functions mark the object's rectangle through `MarkBufsForRect(markFn, rect)`. That function calls `markFn(cell, rect)` for every touched cell.
- The mark functions are `MarkMove`, `MarkFloor`, `MarkFRedraw`, `MarkHalf` and `MarkRedraw`, with `MarkTop` for the row above the room. They write to the buffers pointed to by `0x103c1c..0x103c34`. `FUN_00026292` clears those buffers.

**Links and triggers.** Links are 10 bytes at `gs + 0x1680 + link*10`: `{room, cell, timer, before, next}`. A next value of -3 ends a chain.
- `Trigger(link, how)` walks a chain. For each entry it calls `TriggerObject`, which dispatches to `TriggerGate`, `TriggerDart` and the other trigger functions, then calls `AddTrob`.
- `PushPlate`, `BreakLoose` and `CheckPress` handle what the kid steps on. The kid record is at `0x102faa` (0x3e bytes). The current piece set by `ReadCell` is at `0x104b50..0x104b5a`: id, spec, cell, room, col, row.

**Mobs** are moving objects, here falling loose floors: 16 bytes each, at most 30. The list pointer is at `0x103b6a`, the index at `0x103b6e`, the count at `0x103b70`, and the current mob at `0x103b5a`. Fields: x, y, room, vel, ?, type, row, landed.
- `FUN_00026b68` animates all mobs and compacts the list. `FUN_00026c14` dispatches on type.
- `FUN_00026c90` makes a loose floor fall. `MakeRubble` handles what it lands on. `FUN_000270c2` handles a floor knocked down by another one.
- `seg5_0271fc` runs the per-type draw ("ATM") routines, for example `ATMLoose`.

**Rect procedures through pointers.** Four Pascal procedures are called through pointers stored at A5-0x1f64..-0x1f58 (`0x10609c`..`0x1060a8`): ClipRect(r), EmptyRect(r): Boolean, UnionRect(a,b,dst) and SectRect(a,b,dst): Boolean. The port calls them with a small Pascal-order helper, `pascal_ptr1`/`pascal_ptr3`.

## Porting notes

**Functions that write to their own stack arguments.** These write into the caller's frame, which the checker compares:
- `ComputeOffsetCellRect`, `GetObject`, `PieceAboveId`, `PieceAboveRightId`, `PieceBelowId`, `PieceLeftId`, `PieceRightId`, `MarkTop`, `Trigger`, `Get1stWallColInDir`, `PushPlate`.

Each one is written as `Name_args(maddr ap)` and works on the argument block in emulated memory. Its `"conv": "manual"` glue passes the real argument slots (`emu_sp()`). The C entry point `Name(...)` copies the arguments into a `FRAME` first.

**Return-size overrides.**
- **Set to 2.** For `GetObject`, `GetRoomID`, the `PieceXxxId` functions, `FUN_00025362` and `TriggerGate`, the result is a 16-bit value and the high word of D0 depends on the path. In some paths it is the caller's D0.
- **Set to 0.** For `FUN_00024bcc`, `AnimTrobs`, `RedrawTrob`, `seg5_025382`, `CheckPress`, `AnimTorch`, `StopGate`, `MarkBufsForRect`, `TriggerTorch` and `FUN_00026934`, D0 at exit is leftover from callees or comparisons. Their callers do not use it; I checked the call sites in Ghidra.
- **Set to 4.** `ComputeTrobMoveRect` returns `ComputeOffsetCellRect`'s rect pointer, and its callers test it.

**Full D0 reproduced.** These functions return the exact original D0, including the high word:
- `CellToCol` and `CellToRow`: ext.l of the remainder or quotient.
- The `GetLinkXxx` functions: the high word of the link address.
- `seg5_027190`: the high word of the last mob pointer.

**Undefined behaviour kept.**
- `PieceAboveRightId` with no room returns the caller's D3; the port reads `cpu_get(R_D3)`.
- `GetRoomID`'s range test `(cell >= 0 || cell < 0x1e)` is always true. It is kept, so the read is never skipped.
- A mob or trob type >= 0x8000 would make the original's jump tables in `FUN_00026c14` and `FUN_00027240` jump outside the table. The port treats it as "no action".

**Ignored argument.** Callers of `MarkBufsForRect` push a third word (-1) that it never reads. The port keeps the two-parameter prototype.

**Override outside the unit.** `Rnd` had no parameters in the index but takes a word (`Rnd(n)` = Random() mod (n+1), read from 8(A6)). I added `"Rnd": {"params": [[4, 2]]}`, because `TriggerFlask` and `TriggerTorch` call it with an argument.

## Verification

- **Standard run.** `tools/check.sh` gives no mismatches, but it only reaches about 32 functions of this unit. Most of them are called only from other functions in the unit, and those native C-to-C calls bypass the checker hook.
- **Grouped runs.** I ran three `POP2_NATIVE_ONLY` groups, chosen so that no function is in the same group as its callers. This adds AnimTrob and the gate/exit/mark paths. There were no mismatches.
- **Temporary self-test driver.** It was not committed. Inside a scenario it called every remaining function through its hooked 68k entry, with plausible arguments: link chains, plates, gates, torches, flasks, loose floors, synthetic falling mobs, PoPDiffRect rectangle cases, and corner cases of GetObject, GetRoomID and the PieceXxxId functions. With `CALLS=200` across all 12 scenarios, all 83 functions were checked and none mismatched.
