# seg02b — CODE 2 "Main", second half (0x14020–0x17656)

69 functions, all ported in `src/game/seg02b_main.c`.

## What the unit does

* **Start-up / shut-down**: `main` → `MRInit` (Toolbox init `FUN_000142bc`, menus `FUN_000142de`,
  `InitGame`, window, offscreen world `gOffWorld` at A5-0x523e) → `CheckAvailableMemory` →
  `MohawkInit`, `SetStartLevel` (Finder documents: open a saved game, refuse to print) →
  `ShowCheatMode` → `ResetGame` (outer loop) → `FUN_00014258` (dispose, `ExitGame`, `Terminate`).
* **Settings**: `MakeGameFile` opens/creates ":Data:Prince2.opt" (a resource file, type 'rsrc'
  creator 'PRNc'); its data fork holds the 16-byte record at A5-0x532a (`gSettings`: two words
  copied to/from A5-0x1f78 and A5-0x3738 (ambient sound), packed flag, window flags, window
  position) read/written by `GetSetDefaultSettings`.
* **Game loops**: `ResetGame` (attract mode `FUN_00014b48`: credits / hall of fame / intro cut
  scenes, or `Start`) → `Start` (75 minutes, 0x2cf frames per minute, start strength 3) →
  `RestartGame` (per level: `LoadLevel`, `FUN_00016124` places the kid, `FirstFrame`, then the
  frame loop `FUN_0001652c`) → `FUN_00016654` (one frame: kid `FUN_00016716`, opponents
  `FUN_000168bc`, objects, meters `ChangeMeters`, room change `PrepCut`, clock `KeepTime`) and
  `FrameAdv` (drawing, messages, restart prompt).
* **Input**: `FUN_00015a02` is the input poll procedure (its JT address is stored in A5-0x1df4):
  `ReadKeyboard` turns the GetKeys key map (GetKeys trap address in A5-0x1f68) into joystick
  globals A5-0x3498 (x), A5-0x3496 (y), A5-0x3494 (button), using `IsUp/Down/Left/RightKeyPressed`;
  `FUN_00014c16` handles command keys (esc pause `FUN_00015d20`, space = show time, 'v', 'l'),
  `CheckCheatKeys` the cheat keys (cheat mode A5-0x2824, set by `SetCheatMode` when KeyMap
  long 0 == 0x1c00 at start-up).
* **Data**: game state block `gGameState` (A5-0x5014): level type +0x2186, level number +0x218a,
  start room/cell +0x2198/+0x219a, rooms are 0xc0-byte records (opponent count at +0x20e6).
  Current character A5-0x5056 (0x3e bytes) and its saved copy A5-0x50d2; opponents table
  A5-0x5208 (0x3e bytes each; +0x1e strength, +0x20 max, +0x22 pending change).
  Kid strength A5-0x50b4 / max A5-0x50b2 / pending A5-0x50b0. Room A5-0x4fc2, next room
  A5-0x4fc4, level A5-0x4fd2, next level A5-0x4f92, quit A5-0x4fd0, minutes A5-0x4fce,
  frames A5-0x4fc6, message id/timer A5-0x4fcc / A5-0x4fca.

## Porting notes

* **Stack depth.** The hook pops the return address before the native glue runs, so ported code
  starts 4 bytes above the original's SP. Several callees store stack addresses in globals
  (the Mohawk file layer keeps a pointer to a stack parameter block in A5-0xb88) or copy
  uninitialised stack bytes into globals (the music note queue, `QNoteOff`). Without
  compensation the checker reports mismatches for `FUN_00014b48` and `FrameAdv`. Every function
  therefore starts with `ENTER(link, regmask)`, which rebuilds the stack image the original has after
  its prologue (return address, `link` with the old A6, `movem` register images; CPU A6 set to the
  new frame), and returns through `RETURN()`. Locals whose
  address escapes are at `a6 - offset` exactly as in the disassembly.
* PC-relative constants (file names, format strings in CODE 2) are addressed with `CODE2()`.
* `tc_sprintf` is variadic: called through its jump-table entry with `call_ptr`.
* `GetKeys` and the procedure in A5-0x1f64 are called through stored trap addresses with
  `call_ptr` (they are Pascal routines that pop their argument).
* `_FlushEvents` and `_MaxMem` (needs A0) are issued by hand (the generated wrappers do not pass
  the register arguments / do not return A0).
* THINK C `char` arguments pushed with `move.b` (`Sys6OrLater`, `PackAllData`,
  `tc_NGetTrapAddress`) are passed as `0x0100` (value in the high byte of the word slot).
  The Pascal Boolean result of `GetNumber` is in the high byte of the returned word.
* `QuickFloor` contains a test `(action == 2 && action == 5)` that is never true; kept as a
  comment. `CheckCheatKeys` has a dead `DrawMessage` (local pointer always NULL).

## Signature overrides (`tools/sig_overrides/seg02b.json`)

Outside this unit (needed by calls from here):
* `ErrorHandler`: two word parameters (it reads 0xa(A6); all callers push two words).
* `Rnd`: one word parameter (it reads 8(A6): result is Random mod (n+1)).
* `InvertOffWorld`: two long parameters (it reads 0xc(A6)).

Return sizes of functions of this unit (the high word of D0 is left over from earlier code, or
the result is unused by every caller): `ExitGame`, `SetWindowPos`, `SetStartLevel`,
`ShowCheatMode`, `KeepTime`, `ReadKeyboard`, `PrinceCopy`, `FUN_000168bc`, `FUN_00016f76`,
`CheckAlert` → 0; `MakeGameFile`, `FUN_00015a02`, `Is*KeyPressed`, `RestartGame`, `FirstFrame`,
`FUN_0001652c`, `FrameAdv` → 2; `Start` → 2 (it returns `RestartGame`'s result, which
`ResetGame` uses).

* C-to-C calls between functions of this unit push the arguments first (`callw`/`calll`/
  `callll`), so the callee's frame is where the original's is; `RestartGame` also stores its
  level variable back into its argument slot as the original does (`move.w D0,8(A6)`), because
  the checker compares the caller's stack above SP.

## Checker issues found (outside this unit)

1. **Functions that never return block the checker.** `main` (ends in `Terminate`), and the
   game loops `ResetGame`, `Start`, `RestartGame`, `FUN_0001652c` (return only when the game or
   level ends) are entered through the hook; the checker forks for their first call and sets
   `checking` for the whole run, so no other function is checked afterwards. With `main_` native,
   `tools/check.sh` reports 0 functions verified. These five are tagged `"nocheck": true` in the
   override file (currently ignored by `gen_port.py` / `native.c`); `native.c` should skip
   checking them. Until then run `POP2_NATIVE_EXCLUDE=main_,ResetGame,Start,RestartGame,FUN_0001652c tools/check.sh`.
2. **Ported code runs 4 bytes above the original's SP** (the hook pops the return address) and
   does not build the original's frame; see "Stack depth" above. A shared ENTER/RETURN helper in
   `port.h` would let every unit do the same.
3. **Trampolines bypass `_LoadSeg`.** `call68_fn` calls `fn_address()`, which loads a segment in C,
   while the original `jsr` through the jump table executes the `_LoadSeg` trap, which advances
   the deterministic clock by one trap. A ported caller that is the first to call into a segment
   therefore shifts virtual time by 8 us; over a long checked call this changes sound-callback
   counters (seen as a 1-count difference in a double-buffer counter in `ResetGame`'s full-run
   check). Calling through the jump-table entry (`g_a5 + jt_a5off`) removes it (tested
   locally, not committed).

## Verification

* `POP2_NATIVE_EXCLUDE=main_,ResetGame,Start,RestartGame,FUN_0001652c tools/check.sh`: no mismatch.
* Every function individually (`POP2_NATIVE_ONLY=<fn>`, all 12 scenarios, so it is entered from
  68k code): 0 mismatches. Never reached by the standard scenarios: `FUN_00014064` (window size
  menu item), `FUN_00014258` (quit; does not return), `FUN_00015828`/`FUN_0001584c`,
  `FUN_00015e26` / `seg2_015e56` (flip screen / 'I', 'W' cheat keys), `seg2_0175c4` (time up);
  reviewed against the disassembly.
* Extra runs (custom input/pokes): cheat mode on (`-0x2824:2:1`) with '-', space, k, g, esc,
  cmd-l, cmd-v, arrows: `CheckCheatKeys`, `FUN_00014c16`, `FUN_00015d20`, `Is*KeyPressed`
  checked on every call, ok; quit (cmd-q): `ExitGame`, `SetWindowPos` ok.
* Long-running functions checked as a whole with a quit (cmd-q) during play, so they return:
  `FUN_0001652c` (4 full level runs), `RestartGame` (new game and "Level 7"), `Start`: ok.
  `ResetGame`: only the sound counter difference of issue 3 (disappears with JT calls).
  An earlier run also showed differences in the 32-byte name buffer that `midiOutGetDevCaps`
  copies from an uninitialised stack buffer (stack garbage, depends on register images saved
  by callees); not reproduced with the final input set.
* `main_` cannot be checked (never returns); its callees are checked.
