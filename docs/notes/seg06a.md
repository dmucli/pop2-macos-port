# seg06a — CODE 6 "Characters 1", first half (0x2a644–0x2d58e)

All 56 functions are ported in `src/game/seg06a_characters1.c`, in address order.

## What the unit does

This is the character control layer. It turns the control inputs into sequences
(`JumpSeq`) for the current character record `gChar` (0x102faa).

* **Sequences.** `JumpSeq(seq)` checks that 'SEQS' `seq` exists (`PoPGetResource`) and
  stores it in +0x26 (sequence), +0x28 (last jump sequence) and +0x24 = 0 (position).
  For the flying heads (types 7/8) it also forces the facing for some sequences.
* **Falling and landing.** `Falling` plays the scream (`0x103060`), detects the floor
  (row*0x78+0x6a against y) and calls `HitFloor` or handles special floors (loose floor
  0xb `BreakLoose`, 0xf, 0x1a). `HitFloor` picks the landing sequence from the y
  velocity (soft < 0x32, hurt < 0x3f, dead), calls `DecLifePts`, `SetKidDeathMethod(3)`,
  and `FUN_0002aaea` pushes a dead body off an edge.
* **Generic control.** `GenCtrl` is the per-frame dispatcher. Dead characters go to
  `CutToKidRoom`, `seg9_037c12` or `FinlCheckDeadOpp`. Falling or jumping characters
  only reset the inputs. A drawn sword goes to `FUN_0002c7a6`. Types >= 2 go to
  `seg4_022466`. Otherwise it dispatches on the frame (compared unsigned):
  * standing → `FUN_0002b272`;
  * turning → `FUN_0002b654`;
  * start of a run → `FUN_0002bbf6`, which starts a running jump;
  * running (< 0xf) → `FUN_0002b824`;
  * standing jump frames → `FUN_0002bc1e`;
  * hanging (0x57–0x63) → `FUN_0002bf20`;
  * crouching (0x6d) → `FUN_0002afda`;
  * ledge climbing (0xf6–0x107) → `FUN_0002ae2e`;
  * exit stairs → `StairClimbing`;
  * dead positions → `FUN_0002b19a`, where the shadow merges with the kid;
  * others → `Sitting`, `seg9_03a418`.
* **Moves.**
  * Steps: `DoStepFwd(n)` (n = distance to the edge, 0 = compute it).
  * Turning: `FUN_0002b724`.
  * Crouching and climbing down: `seg6_02b4ce`, `FUN_0002b6a8`.
  * Jumps:
    * `DoJumpUp`, which picks one of `DoJumpHang`, `DoJumpBack` or `DoJumpHigh` by
      looking for ledges with `IsLedge`.
    * `seg6_02bc2a` (standing jump).
    * `DoRunJump(4|100)`, which corrects the take-off point to the edge.
  * Hanging: climb up (`seg6_02bfd8`) or drop (`seg6_02c0c4`, which uses
    `seg6_02c1be` to test-run `AnimChar`).
  * Picking up: `TryPickUp`/`PickItUp` pick up a flask (10) or a sword (0x16), using
    `RemoveObj`, `LoadSwordSet` and `AddSong`.
  * The exit stairs: `FUN_0002b476`/`FUN_0002b598`.
* **Sword fight.**
  * Drawing the sword: `DoEngarde` and `DoKidEngarde`. The latter steps back from edges
    and sliding rocks (`GetSlideRockX`). For the shadow at the end it calls `seg9_03a224`.
  * Control with the sword drawn: `FUN_0002c7a6`. For the kid it loads the target
    opponent with `LoadShadAsOpp`, then dispatches through `OnAlert`.
  * Moves: `DoStrike`, block `FUN_0002ce6a`, `DoAdvance`, `DoRetreat`, step back
    `seg6_02cc78`/`FUN_0002ccf8`, sword away `FUN_0002cbd2`/`FUN_0002cc2e`.
  * `OppToFight` says whether a live opponent is in the kid's room. It saves and restores
    `gChar` around the search.
* **Crushing.** `CrushChar` handles a falling object hitting the current character, and
  `CrushOppInfo(info, idx)` one hitting a stored opponent record.
* **Room opponents.** `FUN_0002d1be` loads the characters of the current room from their
  info records (`GetOppInfoIdx`) into `gChar`, then `seg4_022516`/SaveShad. It does this:
  * starts their sequences;
  * sets life points (`SetOppLifePts`, `SetOppMaxLifePts`);
  * loads the opponent set (`LoadOppSet`) for empty rooms.

## Data structures / globals

**Character record (0x3e bytes).** Named fields in the file are `CH_*`:
* +0 id (10 = the kid's slot);
* +2 face; +4 x; +6 y; +8 type; +0xa frame; +0xc col; +0xe row; +0x10 action;
* +0x12/+0x14 x/y velocity; +0x16 room; +0x18 flag;
* +0x1a sword drawn (1, 0, -1 = never);
* +0x1c alive (< 0) or death counter;
* +0x1e life; +0x20 max life; +0x22 pending life change;
* +0x24 seq position; +0x26 seq; +0x28 last JumpSeq;
* +0x32, +0x34, +0x36, +0x38, +0x3a target opponent index, +0x3c.

**Related records and tables.**
* `gOpp` 0x102f6c is the opponent (same layout), and `gKid` 0x102f2e is the saved kid.
  The room table is `gOppTab` 0x102df8.
* The opponent info records (from `GetOppInfoIdx`) have these fields: +0 row*10,
  +2 x, +4 face, +8 sequence, +0xa sequence position (0 = start a new one), +0xc bank,
  +0x10 sword, +0x12 life, +0x14, +0x16, +0x18 kind, +0x1a max life, +0x1c y / state,
  +0x22 y.

**Inputs.** These are the words at 0x104b78..0x104b80: forward, back, up, down, button.
They are already mirrored for the facing. A value < 0 means pressed; `seg4_02190a()`
returns the "used" value written back.

**Tables.** 0x103478 holds the column left x (0x10347a is the same table + 2), 0x1034b2
the row floor y, and 0x103468 the column step per facing.

**Game state.** `gs+0x21a4` is the opponent kind, and `gs+0x20e6 + room*0xc0` the
opponent count. `gs+0x3986 + room*0x44` is non-zero when the room needs an opponent set.

## Porting notes

* `DoStepFwd` decrements its own argument slot (`subq.w #4,(8,A6)`). The port uses no
  manual glue for this. It writes the argument into its slot (`a6 + 8`) at entry and
  works on the slot:
  * When it is entered from 68k code, that slot is the caller's real slot, because
    `ENTRY_LINK`'s `args_` is the glue SP. Rewriting the same value there is a no-op.
  * When it is called from C, the slot is the argument space that `ENTRY_LINK` reserves.
* `TryPickUp` copies the 20-byte cell record 0x104b50 to a local and straight back (a
  dead `beq` after `moveq #1`). `PickItUp` and `OppToFight` copy records to a local and
  restore them. All three keep the copies on the emulated stack at `a6 - k`.
* `SetOppLifePts` writes life (+0x1e) before the pending change (+0x22). The decompiler
  shows them the other way round.
* `DoJumpHigh` has a test that can never be true for room 8 (col < 0 && col > 4). It is
  kept as it is.
* Return values:
  * The following are reproduced exactly: `seg6_02c1be` (0/1), `FUN_0002bed2` (0/1) and
    `SetOppLifePts` (zero-extended +0x22).
  * The following are reproduced in the low word, with the high word taken from the
    callee as in the original: `HitFloor`, `DoRunJump`, `DoKidEngarde`, `seg6_02bfd8`,
    `TryPickUp`, `PickItUp` and `OppToFight`.

## Signature overrides (`tools/sig_overrides/seg06a.json`)

`ret: 0` is set for `JumpSeq`, `GenCtrl`, `FUN_0002b19a`, `FUN_0002b272`,
`FUN_0002b824`, `FUN_0002bc1e`, `OnAlert` and `seg6_02cc78`. Their D0 at exit is left
over from callees. For `JumpSeq` it is the D0 of `FUN_00020542`, whose port returns 0.

I checked every call site in the whole image, following tail calls, with a script that
scans the disassembly for reads of D0 after the call. None of them uses these values.
The only D0 uses of this unit's functions are word uses of `TryPickUp`, `PickItUp`,
`FUN_0002bed2`, `seg6_02c1be`, `DoRunJump` (from `DoOppRunJump`), `DoKidEngarde` and
`OppToFight` (from `FUN_00028d38`, `PlayAmbient`, `TimeForNewAmbient` and
`FUN_0002cbd2`).

No override was needed outside the unit. `RemoveOppFromRoom` (seg06b, manual glue) is
called as a plain C call from `CrushOppInfo`. The argument slots it modifies are popped
right after the call.

## Verification

* **Final standard run.** `JOBS=4 SECS=2400 tools/check.sh` ran all 22 scenarios on
  current master, with no mismatch and no timeout.
  * 37 of the 56 functions are reached by the scripted gameplay.
  * 24 of them are checked on their own entry. The rest run inside checked callers.
* **Grouped runs.** `POP2_NATIVE_ONLY` groups were chosen by colouring the unit's call
  graph, so that no function shares a group with its callers. They were run with
  `CALLS=100` and showed no mismatch.
* **Temporary self-test driver** (not committed), triggered at a chosen GenCtrl call.
  * **Method.** It snapshots memory. For each of the 56 functions it then runs 25
    iterations. Each iteration does three things:
    1. It randomises the current character record and the inputs. It also randomises
       the opponent, the cell record, the level type/number, the room and some flags.
       Frame ranges and argument values are chosen per function.
    2. It calls the function through its hooked 68k entry, so the call is checked
       against the original code.
    3. It restores memory.
  * **Results.** Every one of the 56 functions was checked this way, in several game
    states, with no mismatch.
  * **Coverage.** With `-fprofile-instr-generate`, the self-test and the scenarios
    executed about 86% of the lines and 68% of the branches of the port.
  * **Limitation.** In some scenarios a randomised state makes a callee run until the
    end of the run, in the native and the original code alike, so the self-test did not
    complete there.
  * **Not executed even by the self-test.** These paths were reviewed instruction by
    instruction against the disassembly instead:
    * paths that need level-specific state, such as sliding rocks, a falling mob in the
      character's cell and the shadow on level 14;
    * `PickItUp`'s actual pick-up of a sword or flask;
    * `FUN_0002ae2e`'s ledge-adjustment arithmetic.
* **Functions the scripted gameplay never reaches.** They are covered only by the
  self-test and the review: `FUN_0002ae2e`, `FUN_0002b19a`, `FUN_0002b598`,
  `StairClimbing`, `PickItUp`, `FUN_0002bd88`, `FUN_0002bed2`, `seg6_02bfd8`,
  `DoRunJump`, `DoKidEngarde`, `CrushOppInfo` and `FUN_0002d108`.
