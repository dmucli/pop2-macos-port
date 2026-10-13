# seg15b — CODE 15 "NISs", second half (0x480ba–0x4b5e4)

All 25 functions are ported in `src/game/seg15b_niss.c`. Signature fixes are in
`tools/sig_overrides/seg15b.json`.

## What the unit does

These are the scripts of the cut scenes (NIS). `NIS(n)` (seg15a, called by `PlayNIS`)
dispatches: NIS 1–6 → `Transition(n)` (1 `NISCarpet`, 2 `NISTemple`, 3 `NISTower`,
4 `NISTitle`, 5 `TransPrincessRoom`, 6 `NISThroneRoom`), 7 `Opening1`, 8 `Opening2`,
9 `AboardShip`, 10 `Discovery`, 11 `HappyEnding` (→ `JaffarBurnBabyBurn`, then the
happy ending animation with `HappyFunc`). The scripts are long chains of
"if nothing was interrupted (result 0) do the next step" built from the seg15a
primitives: `AddNIS` (draw NIS shape id at h, v into the off-screen world), `ClearNIS`,
`CopyNIS` (off-screen → window), `DissolveNIS(time)`, `NISTextDraw(STR#, index)` /
`NISDialog(STR#, index, sound)` (subtitles with speech), `GetSetFadeCTAB(id, mode, speed)`,
`SetNISCTAB`, `CreateSound`/`SetSoundId`/`WaitCue`/`WaitSndId`/`WaitCurrentSound`,
`SetTimer`/`WaitTimer`/`Wait`. Every wait returns non-zero when the player interrupts;
the scripts then skip to the end (fade out, `ResetSoundItem`, restore the key reader
`gReadKeyProc` 0x10620c from 0x105ba8).

* `Opening1` / `Opening2`: the opening story (STR# 27000 = 0x6978 subtitles, shapes
  0x6979–0x69a7). The NIS frame rect is 0x10583a moved to (0x40, 0x3e). `Opening2` has two
  hand-animated parts: a 25-step scroll of four shapes (locals at A6-4/-8/-0xc kept in the
  frame) and an 8-step "flight" driven by the tables at 0x105cc2 (shape id), 0x105cd2 (h),
  0x105ce2 (v), 0x105cf2 (ticks) with the step counter in the global 0x105d02. A missing
  shape makes it return 1 immediately.
* Animation-based scenes call `PlayTheAnimation(id, sub, 1, &TransFuncN, ...)` (seg16).
  The callback gets the frame number and returns a word (non-zero stops the animation):
  `TransFunc4..8` for the temple (horses `FlipHorses`, kid `FlipKids`/`FlipKid`, palette
  steps), `TransFunc9..11` for the tower, `TitleFunc` for the title, `HappyFunc` for the
  ending. `gAnimFrame` 0x106092 is the running animation's current frame; `TitleFunc`
  switches on it (frames 0x65–0x73) and writes it to jump. `gNISClipRect` 0x105f58 is the
  area the animation redraws. 0x105d74 is the handle of the animation's shape handles
  (`FlipKid` flips entry 16).
* `NISThroneRoom`: the queen (rect 0x105d04, saved with `SaveRect` in `gMapSave2`
  0x10582a and restored by `QueenAddNIS` before each new queen shape), the dead kid
  (`DrawDeadKid`, `FUN_0004ae9c`: x from the kid's x 0x102f32 and facing 0x102f30),
  STR# 32000 (0x7d00) dialog, and the backdrop for level type 4 (`FUN_0004ace6`).
  `FUN_0004adc4` loads the level's two colour tables (25000+type, 1000+type), remaps them
  and combines them into `gCTab1` 0x105bac. `gInThroneNIS` 0x105824 is 1 while it runs.
* `NISTitle` / `TitleFunc` / `FUN_0004b11a`: the title (sky colour 0xa0, clouds/stars from
  seg17, the two title word arts at rects 0x105d0c / 0x105d14 united into `gMapSave1`
  0x105826).
* `HappyFunc`: fades by frame number and the five closing lines (STR# 30000 = 0x7530,
  sounds 0x7534+); state in 0x105cbc (wait), 0x105cbe (line), 0x105cc0 (frame offset).
  `HappyDialog` draws a line with 0x105822 = 1.

## Porting notes

* `HappyFunc` adds the frame offset to its own argument slot (`add.w D0,(8,A6)`):
  `"conv": "manual"`, `glue_HappyFunc` passes the slot address and the C code writes it at
  the same point as the original.
* Every caller of `NISDialog` pushes a fourth word that it never reads (it looks like a
  duration): `nis_dialog()` pushes it around the call.
* `NISThroneRoom` pushes `MoveRect`'s last argument (0xd2) before calling `FUN_0004ae9c`;
  the port does the same.
* `PlayTheAnimation`'s Boolean arguments (p14, p18) and `ShowNthvRGB`'s colour bytes are
  pushed with `move.b`: passed as `x << 8` (`B()`).
* **Callee-saved registers leak into memory in `NISThroneRoom`.** The original keeps its
  result in D5 (and constants in D3/D4) while it calls 68k code; deeper callees save those
  registers in their frames (e.g. the `Wait` polling loop), and later `SetSoundId` →
  `SetSoundItem` → the Mohawk audio start code builds a 0x32-byte local record that is only
  partly initialised and `BlockMove`s it into the "HALESTORM Synth" record on the heap. One
  stale stack byte (a saved D5 word) thus ended up in the heap (port 06, original 00). The
  port now sets the emulated D3/D4/D5 exactly where the original does (`set_dw`, `d5w`),
  writes the saved registers into its frame like `movem.l`, and restores them on return.
  Other units may hit the same effect wherever audio is started from ported code.
* ClipRect / UnionRect are called through the pointers cached at A5-0x1f64 / A5-0x1f5c.
* The decompiler output of the big scripts is unreliable (wrong argument counts for the
  seg15a primitives, merged branches); everything was translated from the disassembly.

## Signature overrides (`tools/sig_overrides/seg15b.json`)

Inside the unit:
* `HappyFunc`: `"conv": "manual"`.
* `QueenAddNIS`: 4 params, the last a long (callers push `move.l`; it reads the low word
  at 0x10(A6) and passes it to `DissolveNIS`); `ret: 2` (its result is `DissolveNIS`'s and
  `NISThroneRoom` uses it).
* `ret: 2` (word result, high word left over; `PlayTheAnimation` stores the callback
  result with `move.w D0,(-0x34,A6)`, the NIS/Transition callers use `move.w D0,Dn`):
  `JaffarBurnBabyBurn`, `Opening1`, `TransFunc4/5/6/7/8/10/11`.
* `ret: 0`: `DrawDeadKid` (both callers ignore D0).

**Outside the unit** (all seg15a except `FadeOutAllColors`, seg17a; checked against
their disassembly — whoever ports them must agree):
* `DissolveNIS`: `params [[4,2]]` (reads the dissolve time at 8(A6); index had none).
* `GetSetFadeCTAB`: `params [[4,2],[6,2],[8,4]]` (the fade speed long at 0xc(A6)).
* `FadeInBothCTBLs`, `FadeBothCTBLs`: `params [[4,4]]` (speed long passed to
  FadeIn/OutColors).
* `FadeIndex`: `params [[4,2],[6,4],[10,4],[14,4],[18,4]]` (reads the byte at 9(A6) and
  longs at 0xa/0xe/0x12/0x16(A6)).
* `FadeOutAllColors` (seg17a): `params [[4,4],[8,4]]`.

## Verification

(see below)
