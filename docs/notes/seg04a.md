# seg04a — CODE 4 "Characters 2", first half (0x1dad4–0x20e4e)

Output: `src/game/seg04a_characters2.c` (82 functions, all ported).

## What the unit does

* **Opponent sword-fight AI** (`EnGarde`, `WaitingEngarde`, `InRange`, `GenFight`,
  `FUN_0001ebea`/`ecd4`/`ed96`, `FUN_0001ea88`, `FUN_0001ddb0`, `FUN_0001dfcc`,
  `seg4_01e04c`, `DoOppRunJump`, `DoOppTumbleSeq`). The AI "presses buttons" by writing
  the auto-control words at 0x104b68..0x104b80 (`FUN_0001dad6`/`dae2`/`daf0`/`dafc`,
  `AutoPress`). Strike/block/retreat probabilities come from per-skill tables
  (0x104968, 0x104980, 0x104998, 0x1049b0, 0x1049c8: word[skill], compared with Rnd(255)),
  the skill being word +6 of the per-room character info record.
* **Skeletons** (`FUN_0001ef96` wakes them when the kid gets close via `OpponentClose`,
  `SkelDoneShaking`, `ShakeBones`, `SkelAddOpponent`, `SetSkelTimer`, `SetSkelShakes`,
  `seg4_01f12c`, `seg4_01f1de`), the **flame sword** (`seg4_01f6b4`, `seg4_01f706`,
  `FlameSwordDecLifePts`, `seg4_01f79e`), the **jinnee** (`TriggerJinnee`) and the
  **shadow** (`FUN_0001f9ee`, `seg4_01fa82`, `seg4_01fc44`, `FUN_0001fcd6`,
  `FUN_0002009e`, `IsShadowImbued`, `IsShadowBlinkOn`, `SetupShadowFlame`).
* **Cell lookup**: `ReadCell(room,col,row)` caches the last cell in 0x104b54..0x104b5a
  (index, room, col, row), follows room links with the recursive `Handler`
  (links: word at gs + room*8 + 0x2078/0x207a/0x207c/0x207e = left/right/up/down),
  and returns the tile id (0x104b50) and modifier (0x104b52), from the arrays at
  *(0x1037e0) and *(0x1037dc). Outside the level `GetUndefineCellId` decides wall/space.
  `FirstInRow` (table at 0x104af2), `IndexCell`, `IndexCell1`.
* **Sequence interpreter** `AnimChar`: reads words of the 'SEQS' resource of the
  current sequence (`hGetSequence`, `GetSeqOp`, position in Char+0x24, sequence in
  Char+0x26); negative words -25..-1 are opcodes (a switch with 25 cases: jump, random
  jump, dx/dy, up/down, turn, momentum, sounds via `AnimCharTap`, palette, ...), any
  other value is the next frame.
* `seg4_02055e` fetches the 10-byte frame definition (0x102dee) and the 8-byte sword
  definition (0x102de6) of the current frame.

## Data structures / globals

Character records are 0x3e bytes with one layout: `gChar` 0x102faa (the character being
processed), `gOpp` 0x102f6c (its opponent), `gKid` 0x102f2e (saved kid), and the room's
table `gOppTab` at 0x102df8 (entry i at 0x102df8 + i*0x3e). Fields: +0 id, +2 face
(0/-1), +4 x, +6 y, +8 type, +0xa frame, +0xc column, +0xe row, +0x10 action,
+0x12/+0x14 momentum, +0x16 room, +0x1e life, +0x20 max life, +0x24 seq position,
+0x26 sequence, +0x28 last jump sequence, +0x32 fight state, +0x34, +0x36 blink, +0x38 sword.
`LoadShad(i)` (seg04b) loads table entry i into gChar.

Game state block (*0x102fec): +0x2186 level type, +0x218a level number,
+0x21a4 opponent kind, +0x20e6 + room*0xc0 number of characters in the room,
+0x21a6 + room*0xc0 - 0xbe + id*0x26 per-room character info records, +0x2180 room count.

## Porting notes

* `AddCharX`, `AddFCharX`, `OpponentClose` and `IsShadowBlinkOn` modify their own
  argument slot on the caller's stack (`neg`/`add`/`subi` on (8,A6)/(0xa,A6)). They are
  `"conv": "manual"` with hand-written `glue_*` that write the final value back, so a call
  from 68k code leaves the stack exactly like the original; C callers use the plain
  C function.
* Stale registers in the original: `FUN_0001ef96` can use D4 before setting it (when the
  skeleton info has +0x1c == -1), `SkelDoneShaking` returns D4 unset when there is no
  info record, `GetSeqOp(NULL)` returns D3 unset. The port uses `cpu_get(R_D4/R_D3)`,
  which is exact when called from 68k code.
* `SetupShadowFlame` pushes Char.id as a third argument to `MarkBufsForRect`, which
  only reads two; the C call passes two.
* Return sizes fixed in `tools/sig_overrides/seg04a.json` (callers only use a word, or
  nothing): see the file. D0 of genuinely void functions is not reproduced.

## Overrides outside this unit

* `Rnd` (seg 17): `{"params": [[4, 2]]}` — it takes a word n and returns
  `Random() mod (n+1)`; the index had no parameter, so the C->68k trampoline would not
  have pushed the argument.

## Callee prototypes worth knowing (not changed)

`FUN_00021518`, `seg4_021562`, `FUN_00021076`, `GetCellsInFront`, `GetCharCol` return a
word in D0 although the index says ret 0; `seg4_0235de`, `IsFloor` etc. say 4 but return
words. Callers here only use the low word, so this works with trampolines; whoever ports
them should keep returning the value.

## Verification

`tools/check.sh` (all scenarios): no mismatch. Each function was also checked on its own
(`POP2_NATIVE_ONLY=<fn>`, so that 68k callers reach it through the hook): 41 functions
verified, 0 mismatches. Never exercised by the scenarios (reviewed instruction by
instruction against the disassembly): FUN_0001daf0, FUN_0001dafc, FUN_0001db44,
GetOppDeadSeq, FUN_0001dbee, DoOppTumbleSeq, GetTotalDeadOpps, seg4_01e04c, seg4_01e2ce,
DoOppRunJump, OpponentClose, FUN_0001ef96, seg4_01f12c, seg4_01f1de, IsExitChasingSkel,
SetSkelShakes, SetSkelTimer, ShakeBones, SkelAddOpponent, FUN_0001f556, SkelDoneShaking,
FUN_0001f66c, seg4_01f6b4, seg4_01f706, FlameSwordDecLifePts, FUN_0001f76e, seg4_01f79e,
seg4_01f834, TriggerJinnee, FUN_0001f9ee, seg4_01fb80, seg4_01fbda, FUN_0001fcd6,
CutToKidRoom, GetKidIdxAsOpp, FUN_0001ff82, IsShadowBlinkOn, IsShadowImbued,
FUN_0002009e, SetupShadowFlame, LoadShadwOpp (skeleton, shadow, jinnee and flame-sword
events do not happen in the scripted scenarios). The AnimChar switch body was
disassembled locally (Musashi) from the image bytes because Ghidra had not disassembled it.
