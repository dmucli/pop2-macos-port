# seg09 — CODE 9 "Final Battle" (0x37bb4–0x3a4a0) and CODE 18 "Desert" (0x554e4–0x563a4)

All 83 functions are ported: 57 in `src/game/seg09_finalbattle.c` (CODE 9) and 26 in
`src/game/seg18_desert.c` (CODE 18). Signature fixes are in `tools/sig_overrides/seg09.json`.

## What the unit does

Both segments are level-type code overlays (`LoadCodeOverlays` loads CODE 18 for level
type 1, the desert/beach of level 2, and CODE 9 for level type 6, the final level 14).
They provide per-frame hooks (table `0x103908` indexed by level type, called by
`FUN_00017554`: `seg18_0556c0` for type 1, `seg9_038204` for type 6), level-load hooks
(`FUN_0005591c`, `seg9_037c90`), piece draw procedures, trob (animated object) routines
and trigger routines.

### CODE 9, the final battle

* **Jaffar copies.** Room 3 generates copies of Jaffar: `FinlWatchOpps` →
  `FinlCheckGenOpp(n)` → `FinlGenerateOpp(col, row)`, which fills a per-room character
  info record (`GetOppInfoIdx(3, id)`; the next id is the word at gs+0x2326) and makes it
  the current character (`gChar` 0x102faa). Dying copies are handled by
  `FinlCheckDeadOpp` (maybe drops a flask, shimmers away with sequence 0xef) and
  `AllOtherJaffarsShimmerAway`.
* **Jaffar in rooms 7/8** moves between four fixed positions. The table at `0x1055fa`
  (`gJafPos`) has four 0x16-byte entries: +0 column (counted across rooms 7 and 8, 0–19),
  +2 row, +4 face, +6/+8 x range, +0xa/+0xc rows of the two "range" areas, +0xe/+0x10 their
  min columns, +0x12/+0x14 their max columns (`ShadowInRange`). The per-character info record
  holds at +0x1c {mode (1 evade, 2 attack), target pos, current pos, y}.
  `FUN_000392ae` picks the mode, `FUN_000386d8`/`FUN_000385f2` run it,
  `JaffarSetToNextPos`/`JaffarSetToPrevPos` change the target, `JaffarGoToPos` dispatches to
  `JaffarGoToPos0..3` (they return a sequence for `JumpSeq` or -1) and
  `JaffarHoneInOnExactPos` steps into the x range. `FUN_00038756` tells whether the opponent
  (the kid) can be struck from here, `FUN_0003857a` turns to face him, `FUN_00039078` kills
  him. `JaffarUpdateOppInfo` repositions a copy when its target changed.
* **Fireballs** are mobs of type 0xc (mob globals at 0x103b5a: x, y, room, speed, x velocity,
  type, row, state; state bit 3 = exploding, bits 0–2 frame). `FUN_0003a430` launches one,
  `seg9_03a350` moves it, `FUN_0003a194` stops it at a wall, `ATMFireball` draws it,
  `DrawObjFireball` draws a fireball object, `FUN_0003a09e` lets a copy (type 6) be hit,
  `seg9_03a224` is Jaffar hit by his own fireball.
* **Room objects**: the spider (custom room 0x19: `AnimFinlSpider`, `DrawFinlSpider`), the
  crystals (0x1c: `AnimFinlCrystal`, `DrawFinlCrystal`, `TriggerFinlCrystal`, count at
  0x10578e), the gargoyles (0x1d/0x1e: `AnimFinlGargoyle`, `DrawFinlGargoyle`,
  `TriggerFinlGargoyle`, next state at 0x10578c), custom room 0x1a's piece 3
  (`seg9_0396ea`/`seg9_039754`). Their trob state word (0x1037e8) is
  0x1000 | 0x800 (moving) | counter << 4 | index; the per-frame offsets are word pairs
  {dy, dx} at 0x105684 (crystals) and 0x1056c8 (gargoyles).
* **Story**: `FUN_00038310` (kid leaves room 2 → `FUN_000382ca`: NIS 5, level reload,
  `FUN_00038182` drops the kid into room 4; or the kid is hit in room 5), `seg9_037c12`,
  `FUN_000395aa`, palette/music hooks `seg9_037c76`, `seg9_039640`, `FUN_00039934`,
  `seg9_0395fc` (screen flash).

### CODE 18, the desert

* Globals: `0x104b9c` stone set (-1 until chosen), `0x104b9e` index of the special stone,
  `0x104c20` last stone column the kid marked, `0x104ba0` exit opened, `0x104ba2` song
  started. The desert state word is gs+0x7a8 (bits 0–4 exit opening step, bits 8–12
  exit sequence step); the stone states are words at gs+0x780+(room-1)*0x78+cell*4
  (bits 0–3 frame, bits 4–8 wait counter).
* `seg18_0556c0` (per frame) → `FUN_00055660` (sand color cycling), `FUN_00055760` (kid on
  the stones of room 1, leaving to the left, the right edge of room 3), `DsrtCheckExit`
  (opens the exit when only the special stone is up: `TriggerDsrtExit`).
* Stones: `IsMarkStonePos`, `TriggerStone(room, cell, how)`, `SinkDsrtStone`,
  `FUN_00055ecc` (stone trob), `RedrawStone`, `RemoveLastStonePeel`,
  `AddDsrtStoneUpPeels`, `FUN_000561e6` (kid death on the stones), `seg18_055f88` (draw).
* Exit `FUN_00055b5c`/`DrawDsrtExit`, planks `FUN_00055cf8`/`DrawDsrtPlanks`, rocks
  `FUN_00055daa`/`DrawDsrtRocks`, sand swirl `SetupSand`, `seg18_055506` (clip FChar image).

## Porting notes

* `ShadowInRange` normalises its direction argument in its own stack slot (`move.w D3,
  ($a,A6)`): `"conv": "manual"` with `glue_ShadowInRange`.
* `JaffarGoToPos` leaves D4 unset for a target outside 0..3 (then uses the caller's D4);
  reproduced with `cpu_get(R_D4)`. Targets are always 0..3 in practice.
* Locals that the original keeps in its frame (rect copies passed to `_OffsetRect` /
  `MarkBufsForRect`, the 0x3e-byte character copy of `AllOtherJaffarsShimmerAway`, words
  spilled to `-4(A6)`) are written at their original `a6 - k` offsets.
* Callers of `MarkBufsForRect` push an extra -1 word; `mark_bufs_for_rect` pushes it too.
* ClipRect and SectRect are called through the cached trap pointers (A5-0x1f64, A5-0x1f58).
* The 68k jump table of `seg9_038204` (rooms 3–8) was decoded by hand: room 3 → song 0x10c,
  rooms 4/5 → 0x10b, room 6 → 0x10d logic, room 7 → 0x106 if 0x10300e, room 8 sets
  0x10300e then falls into room 7.

## Signature overrides (`tools/sig_overrides/seg09.json`)

All inside the unit; no override outside it.
* `ShadowInRange`: `"conv": "manual"`.
* `ret: 2` (16-bit result, high word of D0 is left over; callers use the word):
  `FUN_00038756` (index said 1), `JaffarGoToPos1/2/3` (index said 4), `seg9_03a224`
  (its caller does `move.w D0,D7`), `SinkDsrtStone`, `TriggerStone`.
* `ret: 0` (D0 is leftover, every caller ignores it): `seg9_037c12`, `AnimFinlCrystal`,
  `AnimFinlGargoyle` (AnimTrob dispatch), `seg9_03a418` (end of GenCtrl, whose result is
  unused), `FUN_00055660`, `DsrtCheckExit`, `FUN_0005591c` (LoadCodeOverlays),
  `RemoveLastStonePeel`, `seg9_038204` (per-frame hook, table entry 0x103920, called by
  `FUN_00017554`, which ignores D0), `FUN_00039934` (room-template hook at 0x1033ba, called
  by `jsr (A2)` at 0x18a68/0x18fa0 which ignore D0).
* Where the hooks live (found by scanning the A5 world at run time): room-template
  {load, unload} hooks `seg9_037c76` (0x103382), `seg9_039640` (0x1033b2), `FUN_00039934`
  (0x1033ba), `seg9_0395fc` (0x1033c2); piece draw procedures `FUN_00037bb4`
  (0x1056e0/0x1056e4/0x105730), `seg9_039754` (0x10575c), `DrawFinlSpider` (0x105780),
  `DrawDsrtExit` (0x104bb4), `seg18_055f88` (0x104c1c). None of their callers uses D0.
* D0 reproduced: `FUN_00037bb4` (word: 8, the level type, 0 or `DrawFinlGargoyle`'s D0),
  `FinlCheckDeadOpp` (word: gChar+0x18, re-read at the end), `FUN_000380da`,
  `DrawFinlGargoyle` (layer word, 0 or 0x10), `GetFireballImg`, `IsMarkStonePos`,
  `FUN_00055c96`, `seg18_055506`, `seg18_05588a`.

## Verification

* `tools/check.sh` (after merging master): no mismatch; 14 functions of the unit verified in
  the standard scenarios (the save "Level"/"Level 15" is level 14, the final level).
* Desert (level 2, type 1): new game, `cmd-l` to skip to level 2, keys to pass the cut scene,
  then walking left/right (stones, exit, planks, rocks, sand). All desert functions verified
  (some with `POP2_NATIVE_ONLY` so that their callers run as 68k code).
* A temporary test driver (not committed), run from the per-frame hooks after the check
  budget of the hook was used up and with `POP2_NATIVE_ONLY=<unit>`, called every function
  of the unit with randomised character / opponent / mob / trob / custom-room state (600
  cases per group, `CALLS=100`), restoring the globals and the game state block between
  cases. Result: every one of the 83 functions was checked individually by the
  differential checker, no mismatch (`ShadowInRange` through its 68k callers;
  `FUN_000382ca`/`FUN_00038182`, which play NIS 5 and reload the level, once each).

Mismatches seen that are not in this unit:
* `PrinceSndOn` (seg05b), D0 high word, when called from native code (e.g.
  `seg18_0556c0`): `SoundItemOn` returns with the high word of D0 it was entered with
  (`clr.w D4; move.w D4,D0` path) and the original enters it with D0 = snd << 3, but the
  port does not set the CPU's D0 before calling it. Fix in seg05b: `cpu_set(R_D0, d0)` before
  `SoundItemOn`.
* `FUN_00016716` (seg02b) mismatches on the frame where `cmd-l` (skip level) is pressed,
  with only original code for this unit.
* `DrawHorse` (seg03b) mismatches on every call with the save "Level 10".
