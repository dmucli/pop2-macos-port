# seg04b — CODE 4 "Characters 2" (0x20e5e–0x24180)

File: `src/game/seg04b_characters2.c` — all 90 functions ported, in address order.

## What the unit does

Character physics/control helpers operating on the *current character* record and the
collision-detection ("barrier") machinery.

### Character records (0x3e bytes)
* `gChar` 0x102faa (A5-0x5056) — the current character. Fields used here:
  +0 id, +2 face (-1 left / 0 right, 0x56 = none), +4 x, +6 y, +8 type (0 kid, 1, 2, 4, 6,
  7/8 flying heads, 10, 0xb, 0xc ...), +0xa frame, +0xc col, +0xe row, +0x10 action,
  +0x12 xvel, +0x14 yvel, +0x16 room, +0x18 flag, +0x1a (0x102fc4: 1 = riding a sliding
  rock), +0x1c alive (<0 alive, >=0 death counter), +0x1e life, +0x20 max life,
  +0x22 pending life change, +0x26 (0x102fd0), +0x28 seq frame, +0x2a Rect (char rect),
  +0x34 (0x102fde).
* `gSaved` 0x102f6c and `gOpp` 0x102f2e are two more records of the same layout;
  `gCharTab` 0x102df8 is the per-id table (`LoadShad`/`seg4_022516` = SaveShad).
* Level type words in the game state block: `+0x2186` and `+0x218a`; `+0x2191` (byte)
  = level has sliding rocks; `+0x21a4`.

### Cells
`ReadCell(room, col, row)` (seg 4a) leaves the last cell in 0x104b50 (kind), 0x104b52
(info), 0x104b54 (index in room), 0x104b56 (room), 0x104b58 (column), 0x104b5a (row).
`GetCellsInFront`, `FUN_00021518`, `FUN_00021ac8`, `seg4_021ae0`, `seg4_021b0e`,
`FUN_00021076` read cells around the character (step tables 0x103468 / 0x10346c by face).

### Input
0x104b68/6a/6c joystick x/y/button; 0x104b78..0x104b80 edge-triggered inputs
(left/right/up/down/button), 0x104b6e saved copy (FUN_00021920/FUN_00021930).
FUN_000218ce/FUN_000218ec mirror the inputs for a character facing right / upside down.

### Collision detection
`CheckBarr` computes, for rows row-1/row/row+1 and the columns covered by the character
image (`GetCharEdges`: 0x104a9c left edge, 0x104a9a right edge, 0x104aa0 image height,
0x104a9e width), a room per column (word[10] at 0x104a56 / 0x104a42 / 0x104a2e) and a
barrier flag word (0x0f: barrier left side is left of the char's right edge, 0xf0: right
side right of the char's left edge) at 0x104ab6 / 0x104a1a / 0x104a06. `InitColDetBufs`
keeps the previous frame's row in 0x104a6a / 0x104aa2. Newly entered barriers are
recorded in 0x104a86/0x104a88 and handled by FUN_00022faa → FUN_00022fea / FUN_00023056
→ `CheckCollide1` → `CheckCollide` → `Collide` → `GroundBump`. Barrier geometry per code
(`GetBarrCode`) comes from the left/right offset tables 0x1049e0 / 0x1049f2;
0x104a04 is the x of the column being examined.

## Porting notes / pitfalls
* `GetRow` and `AdjustRoom` modify their own argument slot on the stack
  (`subq.w #5,(8,A6)`, `subi.w #0x1fe,(0xa,A6)`). They are `"conv": "manual"` with a
  hand-written `glue_GetRow` / `glue_AdjustRoom` that reproduce the slot write for 68k
  callers (C callers' slots are dead anyway).
* `MakeFCharClipRect` and `GetCharEdges` call a Pascal routine through the pointer at
  A5-0x1f58 (SectRect-like, `(src1, src2, dst): Boolean`, result slot above the args);
  `sect_rect_ptr()` does that call by hand and returns the result byte.
* `seg4_022516` (SaveShad) passes a string that lives in the code segment
  (PC-relative, "Error in SaveShad(). Quit?"): `fn_address(FN_seg4_022516) + 0x5e`.
* The 0x104a9a/0x104a9c naming: 0x104a9c is the *left* edge, 0x104a9a the *right* edge.

## Signature overrides (tools/sig_overrides/seg04b.json), all inside this unit
* `conv: manual`: GetRow, AdjustRoom.
* `ret: 2` (D0 low word is a meaningful result that callers use; the index had 0 or a
  wrong size): FUN_00021076, seg4_021452, GetCellsInFront, FUN_00021518, seg4_021542,
  seg4_021562, FUN_00021ac8, seg4_021ae0, seg4_021b0e, CheckCollide1, IsDeadPos,
  FUN_0002219e, DecLifePts, seg4_0235de, GetBarrDistFromBack. `ret: 4`: GetCharCol
  (returns GetCol's D0).
* `ret: 0` (D0 is leftover garbage, no caller uses it): seg4_02108a, seg4_021158,
  seg4_02129a, FUN_00021866, FUN_00021fe8, FUN_000220ac, FUN_00022402, CheckBarr,
  FUN_00022fea, GroundBump, EnemyCollide.

Note: `make` does not track `tools/sig_overrides/*.json`; touch a source file to force
glue regeneration after editing overrides.

## Verification
`tools/check.sh` (default CALLS=30): no mismatch, 54 functions verified directly.
Re-running with `POP2_NATIVE_ONLY=<functions not yet entered from 68k>` iteratively
(so that their callers run on the 68k core) verified 20 more: 74/90 verified
individually, every one ok. Never exercised by the scenarios (reviewed instruction by
instruction against the disassembly): FUN_0002121a, FUN_00021600, FUN_000218ec,
FUN_000220ac, FUN_0002219e, FUN_000223c8, FUN_00022402, FUN_00022baa, FUN_00022fea,
FUN_00023338, FUN_000235ae, FUN_00023746, FUN_00023cd2, GetBarrDistFromBack, RemoveObj,
seg4_021452.

With CALLS=300 the per-frame functions (`seg4_021658` / `PlayerCtrl`, whichever is the
outermost native one) show a 1-byte difference at 0x1754bf/0x1754cf at the same calls
(newgame 157/230, Level 8 83/197) even when *only* that function is native
(`POP2_NATIVE_ONLY=PlayerCtrl`, whose own body is trivial and verified on the earlier
calls). `POP2_WATCH` shows the byte is written by `QNoteOff` (seg 11, the live-music VBL
task queue) with a different note value: the asynchronous music task runs with the
interrupted CPU register context, which differs between native and 68k execution. This
is a checker artifact, not a translation difference.
