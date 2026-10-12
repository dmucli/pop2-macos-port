# seg06b — CODE 6 "Characters 1", second part (0x2d58e–0x3059e)

File: `src/game/seg06b_characters1.c` — all 42 functions ported, in address order.

## What the unit does

* **Per-room opponent info records** (0x26 bytes, at most 5 per room) live in the game
  state block: record i of room r at `gs + 0x21a6 + r*0xc0 - 0xbe + i*0x26`
  (`GetOppInfoIdx`), count word at `gs + r*0xc0 + 0x20e6`. Fields: +0 cell, +2 x,
  +4 face, +6, +8 sequence, +0xa sequence position, +0xc palette bank (opponent kind),
  +0xe index, +0x10 (char +0x1a), +0x12 life, +0x14 sword, +0x16, +0x18 skill (from the
  per-type table 0x102d96), +0x1a, +0x1c..+0x25 type-specific (`SetSpecialOppInfo`,
  snake/Jaffar handlers in seg 7/9).
  `AddOppToRoom` / `RemoveOppFromRoom` (also shifts the room's character table
  `gOppTab` 0x102df8 when the room is the current one) / `UpdateOppInfo` (stores the
  current character `gChar` 0x102faa back, or moves it to the room its position falls in,
  found by `FUN_0002ed6a` which follows the room links gs+0x2078+room*8) /
  `TransferOppInfo` (the character follows the kid through the last cut) /
  `CheckSideRoomsForVisibleOpps` (guards of the left/right rooms that are visible).
* **Room cuts**: `FUN_0002e9c2` (per frame, delay counter 0x103038) calls `CutChar`,
  which decides from the kid's position and image box (0x104a8c right edge, 0x104a8e left
  edge, 0x104a90 width, 0x104a92 height; flag 0x107e48) whether he leaves the room
  (direction in 0x10306c: 0 left, 1 right, 2 up, 3 down) and moves him with `Cut`.
  `CutAllOpponents` then decides per character whether it follows (TransferOppInfo) or
  stays (UpdateOppInfo); `CutOpponent` saves all characters when the kid leaves.
  `NoWallInDirOfChar`, `IsCharInKidRoom`, `FUN_0002ec7c` (room not full, character may
  follow).
* **Opponent generation points**: per room at `gs + 0x3986 + room*0x44` (count word),
  0x14-byte entries from +8: +2 max opponents near, +4 row, +6 col, +8 timer,
  +0xa timer reload, +0xc sword, +0xe second row, +0x10 opponents left, +0x12 skill (low 4
  bits) / 0x80 tumble. `CheckOppGenPts` → `GetOppGenPtOnRow` → `IsActiveOppGenPt`,
  `IsTimeToGenOpp`, `GenerateOpponent` (creates the info record and initialises `gChar`).
* **Palette banks**: `SetCharacterBank`, `PresetOppBanks` (opponent kinds loaded in banks
  0x20/0x30 at 0x103078[2]; char +0x36 = bank), `FUN_0002e81c` installs their palettes.
* **Control and fights**: `AutoCtrl` (per character type: guards `seg6_02f5a0`, skeletons
  `seg4_01f12c`, others in seg 7/9/10; AI timers 0x104962/64/66), `FUN_0002f55e` clears
  the inputs. `seg6_02fdf2` runs `TestStrike` for every character against the kid and
  back; `GetCharStrikeRanges` gives the hit window, `IsStrikingPos` / `IsTheStrikePos`
  the striking frames. A hit sets the victim's action to 10; `CheckStab` then runs
  `FUN_0002f832` (life loss, push-back, death via `seg6_02fba2`, sounds).
  `SetTensionLevel` drives the music tension (0x10495e level, 0x104960 amount).
  `IsAdvancableId` tells whether a cell can be walked onto.

Globals: `gKid` 0x102f2e and `gOpp` 0x102f6c (same record layout as `gChar`; names as
in seg04a), current room 0x10303e, neighbours 0x103040 (left) / 0x103042 (right) /
0x103046 (below), next room 0x10303c, level 0x10302e / next level 0x10306e, opponent
meter id 0x102f68, kid's target 0x102fa6.

## Porting notes

* `RemoveOppFromRoom` increments its own index argument slot (`addq.w #1,(0xa,A6)`) and
  `PresetOppBanks` replaces a 0 argument by 1 (`move.w D0,(8,A6)`): both `"conv":
  "manual"` with hand-written glue writing the final slot value back.
* `FUN_0002f832` reads D5 before setting it when the opponent is a snake (type 11) and
  the character is not (`JumpSeq(D5)` / `D5 != 0x2d`). Its only caller, `CheckStab`,
  holds the room's character count in D5; the port passes that explicitly and the 68k
  entry uses `cpu_get(R_D5)`.
* `IsTimeToGenOpp` tests `frame % 3` with `divu; swap; tst.l`, i.e. quotient and
  remainder together: it only bails out when the frame counter (0x104b66) is 0. Kept.
* `UpdateOppInfo` clears `gChar`'s life (+0x1e) on its normal exit paths (original).
* Switch tables decoded by hand: `AutoCtrl` (type 1..11 at 0x2f510), `seg6_02fba2`
  (opponent type 2..11 at 0x2fd4e).
* `IsCharInKidRoom` saves `gChar` on the emulated stack (a6-0x40) like the original.

## Signature overrides (tools/sig_overrides/seg06b.json), all inside this unit

* `conv: manual`: RemoveOppFromRoom, PresetOppBanks.
* `ret: 2` (the high word of D0 is left over from callees or address arithmetic; every
  caller tests only the word): UpdateOppInfo, IsTimeToGenOpp, IsOppGenPtWithTumbleOn,
  FUN_0002ed6a.
* `ret: 0` (D0 is a callee's leftover; no caller uses it — checked in Ghidra):
  FUN_0002e9c2, AutoCtrl, seg6_02f5a0, seg6_02fba2, seg6_02fdf2, TestStrike,
  GetCharStrikeRanges.
* Exact D0 is reproduced for the others (pointers from GetOppInfoIdx / GetOppGenPtIdx /
  GetOppGenPtOnRow, ext.l id from GetOppIdxClosestToKid, PresetOppBanks' bank, 0/1
  results).
No override was needed outside the unit.

## Verification

* `tools/check.sh` (default): no mismatch, 28 of the 42 functions verified directly.
* Grouped `POP2_NATIVE_ONLY` runs (leaves in one group, their callers in another, so the
  inner functions are entered from 68k code), on all scenarios and on the long intro
  (`VMS=1800000`, demo recordings): no mismatch.
* A temporary test driver (not committed) called from `CheckOppGenPts` after its check
  budget called every function of the unit through its hooked 68k entry, from a memory
  snapshot restored before each call, with many variants (all character/opponent type
  combinations for the strike functions, out-of-room columns/rows for UpdateOppInfo /
  FUN_0002ed6a / IsCharInKidRoom, all cut directions, stabbed characters, kid positions
  and image boxes for CutChar / FUN_0002e9c2, synthetic generation points, palette bank
  states). With `CALLS=300` on all 12 scenarios: 41/42 verified in that run, no mismatch
  (SetSpecialOppInfo was verified in the grouped runs). Every function has been verified
  at least once. A deliberately broken SetTensionLevel was caught, as a sanity check.
* Paths not reached by any run (reviewed against the disassembly): CutChar's level/room
  special cases (level 9 room 8 next-level, seg23_06e69e on the rooftops, FUN_000380da
  in level type 6 rooms 7/8, the level 5 / 13 death cuts), FUN_0002e9c2's level-5
  rooftop fall and the AddKidScream/DecLifePts fall out of the level.
