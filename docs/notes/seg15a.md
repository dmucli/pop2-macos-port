# seg15a — CODE 15 "NISs", first half (0x43fa4–0x480aa)

All 46 functions are ported in `src/game/seg15a_niss.c`. Signature fixes are in
`tools/sig_overrides/seg15a.json`.

## What the unit does

The NIS (cut-scene) player, the scene scripts of half the NISs, the "dissolve" transition
and the colour-table helpers that every scene uses. The other half of CODE 15 (seg15b:
`Opening1/2`, `NISTemple`, `NISTower`, `NISTitle`, `NISThroneRoom`, `HappyFunc`, the
`TransFunc4..11` frame hooks) calls into it constantly.

* **Entry**: `NIS(n)` (called by `PlayNIS`, seg02a, through its jump-table slot):
  * opens the NIS resource file (record `0x10591a`) and the Mohawk sound/music files
    (`0x1059b6`, `0x105b66`; the previous `gSndFile`/`gMidiFile` 0x10608e/0x10608a are
    restored at the end), prepares the dissolve (`InitDissolve`: 1 for NIS 6, 0 for 7, 8, 10),
  * installs the NIS key reader (`FUN_00044c22`: `gReadKeyProc` 0x10620c :=
    `FUN_00044bd6`, the old one is kept in 0x105ba8),
  * dispatches: 1..6 `Transition` (1 `NISCarpet`, 2 `NISTemple`, 3 `NISTower`, 4 `NISTitle`,
    5 `TransPrincessRoom`, 6 `NISThroneRoom`), 7 `Opening1`, 8 `Opening2`, 9 `AboardShip`,
    10 `Discovery`, 11 `HappyEnding`, 12..20 `TreeCheck(n - 11)` (`gNISSub` 0x105ba4).
  * Every scene returns non-zero when interrupted (key reader: 1 quit, 2 abort flag
    0x105830, 0x1b Esc/space/click).
* **Scene scripts** are linear: draw SHAPs into the off-screen world (`AddNIS(id, x, y,
  mode, rect)`, `ClearNIS(world, colour, rect)`), copy to the window (`CopyNIS(rect)`) or
  dissolve it in (`DissolveNIS(ticks)`), play/wait for sounds and music cues (seg16:
  `CreateSound`, `SetSoundId`, `WaitCue`, `WaitSndId`, `WaitCurrentSound`), timers
  (`SetTimer`/`WaitTimer`), animations (`PlayTheAnimation(id, sub, …, frame hook, …)`; the
  hooks `TreeFunc`, `TransFunc1/3`, `TransFuncJaffarMagic`, `TransFuncSetCTABframe1` get
  the frame number and return non-zero to stop), and text (`NISTextDraw(TEXT id, item)`,
  `NISDialog(id, item, snd)` = play + text + wait). The pictures sit in `gNISRect` 0x10583a
  moved by (0x40, 0x3e); the text box is `gTextRect` 0x105832 (background mode words
  0x105822/0x105824, saved-rect handle 0x105826). A speaker badge (SHAP 0x105ba0 at
  offset 0x105b9e/0x105b9c) is drawn with the next text when 0x105ba2 is set.
* **Colour tables**: three CTBL slots `gCTab1/2/3` (0x105bac/0x105bb0/0x105bb4).
  `GetSetFadeCTAB(id, flags, speed)` loads one (flag 1, 2 or 0x1000; 0x20 fades the old
  one out; 4/8/0x10 SetColors ranges; 0x40 ShowColors). `SetNISCTAB(flags, level)` sets
  colours from slot 1, 2, both combined (3), or slot 1/2 blended with slot 3 at `level`%
  (0x2000/0x4000, `BlendCtabs(a, b, pct)`). `FadeInBothCTBLs`/`FadeBothCTBLs(speed)` fade
  to/from slots 1+2 combined; `FUN_00046940` releases the three; `FadeIndex(idx, r, g, b,
  ticks)` fades one index to black.
* **Dissolve**: `gDisData` 0x105bb8 is a handle {+0 top, +2 left, +4 rect, +0xc delay base,
  +0x10 delay range, +0x14 pairs {src, dst} per long word of the area}. `MakeDisolveData`
  builds it (offsets, shuffled with `RandBig`); `LoadSaveDisRes` loads/saves it as 'DSLV'
  (rowBytes*10 + kind) in ":Data:Prince2.opt"; `SetDisolveDelays` calibrates it with
  `DissolveTime` (a dry run of the copy loop against a dummy word, timed with TickCount),
  which also turns the offsets into addresses on first use. `DoDissolve` (time taken from
  its caller's stack) → `DrawDisolveData(world, data, ticks)` copies the words in two passes
  in 32-bit mode, with a busy-wait per word from `DisolveDelay` adjusted against TickCount.
* `UnPackAndMapSHAP` (Pascal, used by `KBAnimation::prepare`) unpacks an RLE SHAP
  (header 0xb100) and adds a colour offset to every non-zero pixel. `FlipHorses`/`FlipKids`
  flip ranges of the SHAP table (`gShapeTable` 0x105d74).

## Porting notes

* **Arguments the index missed** (all read by the code): `DissolveNIS` (word),
  `FadeInBothCTBLs`/`FadeBothCTBLs` (long speed), `GetSetFadeCTAB` (third long, speed),
  `FadeIndex` (word + 4 longs), `BlendCtabs` (third word, percent), `DrawDisolveData`
  (three longs; its body at 0x46ff0 is entered with `jmp` after `_StripAddress` and was
  disassembled by hand).
* `DoDissolve` reads a long time at 8(A6) too, but its prototype was kept without
  parameters because `DoCreditsDialog` (seg02a, already merged) calls `DoDissolve()` after
  pushing the long itself. The port reads it from the argument area (`R32(a6 + 8)`), which
  holds it on both paths; `DissolveNIS` pushes it the same way.
* `FadeOutAllColors` (seg17a) takes two longs but the index lists one; `HappyEnding` calls
  it through its jump-table slot with `call_ptr` so the call keeps compiling whichever way
  seg17a fixes it. No override outside the unit.
* Extra words pushed and ignored: most callers of `NISDialog` push a fourth word; the port
  pushes it (`nis_dialog`). `DissolveTime` clamps its third argument in its own slot: the
  port writes the slot (`a6 + 0x10`) like the original, so no manual glue is needed.
* `_SwapMMUMode` takes/returns D0.b (the generated wrapper does not set D0):
  `swap_mmu_mode`. ClipRect is called through the trap pointer cached at A5-0x1f64.
* Jump tables decoded by hand: `NIS` (0x44044), `Transition` (0x447a0).
* Return sizes: D0 high words are leftovers in most functions; `ret 2` where callers use
  the word (`NISDialog`, `TransPrincessRoom`, the frame hooks — `PlayTheAnimation` does
  `move.w D0,…` —, `FUN_00044bd6`, `WaitCurrentSound`, `DoDissolve`, `DissolveNIS`), `ret 0`
  where no caller uses D0 (`TreeCheck`, `InitDissolve`, `SetNISCTAB`, `GetSetFadeCTAB`,
  `SetDisolveDelays`).

## Verification

* `tools/check.sh`: see the final run below.
* With `POP2_NATIVE_ONLY` = the unit (minus the callers whose checks nest the callees),
  intro + new game: `NIS` (Opening1/2), then every callee individually (NISTextDraw,
  ClearNIS, DissolveNIS, NISDialog, the key reader, DoDissolve, DrawDisolveData,
  DisolveDelay, InitDissolve, LoadSaveDisRes, SetDisolveDelays, DissolveTime, FadeIndex,
  GetSetFadeCTAB, SetNISCTAB, …): no mismatch.
* A temporary hook (not committed) in `CheckPlayNIS` forced each NIS 1..6, 9..11 and the
  tree NISs 12..20 during a new game: `NISCarpet`, `TransPrincessRoom`, `AboardShip`,
  `Discovery`, `HappyEnding`, `TreeCheck` (subs 1..9), `TreeFunc`, `TransFunc1`,
  `TransFuncJaffarMagic`, `TransFuncSetCTABframe1`, `FlipHorses`, `FlipKids`,
  `DrawSkyForNIS`, `FadeInBothCTBLs`, `MakeDisolveData` (NIS 6 with no saved data),
  `RandBig`: no mismatch.
* The same hook called the remaining functions directly through their 68k entries:
  `SetNISCTAB` with every flag combination, `BlendCtabs` (equal and different sizes, null
  handles), `AddShpNIS`, `UnPackAndMapSHAP` on packed (0xb100) shapes, `TransFunc3(0/1)`,
  `RandBig` edge cases, `PlaySoundWait`, `InitDissolve(2)`: no mismatch.
