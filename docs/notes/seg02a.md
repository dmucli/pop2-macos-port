# seg02a — CODE 2 "Main", first half (0x10434–0x1400a)

All 71 functions are ported in `src/game/seg02a_main.c`.

## What the unit does

* **NIS (cut-scene) triggering**: `IsTimeForNIS` decides which NIS is due. It uses the NIS
  progression state (`0x103056`), the level (`0x10302e`/`0x10306e`), the minutes left
  (`0x103032`) and the last "time is running out" NIS (`0x10582e`). `CheckPlayNIS` and
  `PlayNIS` play it. `DrawRoomFromNIS` loads a level and draws a room as an NIS backdrop.
  `FUN_00010518` says whether an NIS is pending in the current room.
* **Dialog framework**: `InitDialog` installs `FUN_00010d56` as the key reader (`0x10620c`).
  Dialogs are drawn into the off-screen world (`0x102dc2`) by `DrawDialogFrame`,
  `DialogDrawStr` (3-layer shadowed text, font 0xd5 size 20), `DialogDrawStrId`,
  `DialogDrawNumber` and `DialogDrawBox`. They are shown with `DialogFadeIn`/`DialogFadeOut`
  (CTBL 8000) and removed with `DialogResetToGamePlay`. The game rect is `0x106204`.
* **Saved games**: `GetSaveGameLocation` calls SFPutFile/SFGetFile and remembers the
  name/vRefNum in `0x102b4e`/`0x102c4e`. `ResumeGame` and `SaveGame` read and write the
  file (creator 'PRNc', type at `0x102910`). `SetCheckSavedGameCheckSumValues` handles
  checksums at +0x58/+0x60; on a mismatch `FUN_00011a70` arms a tamper-penalty timer
  (`0x10292a`) that `FUN_00011a00` enforces.
  * The record (`InitSavedGameStruct`) has these fields: +4 rebirth room, +6 ID string,
    +0x26 minutes, +0x28 ticks, +0x2a/+0x46 random filler, +0x42 level, +0x44 max HP,
    +0x52, +0x54, +0x56, +0x5c, +0x5e, and from +0x66 the level map / objects copies.
* **Rebirth point**: `gSavedGame` (`0x102fe8`, 0x3766 bytes) holds copies of the game state
  block. `SaveRebirthPtStatus` saves it, `SetRebirthPtInitials` resets the tile states,
  `FUN_00011d52`/`FUN_00011dea` restore it, and `FUN_00011bce` frees it.
* **Copy protection**: `CopyProtectionDialog` covers 'COPY' 8005 entries, the 5x2 symbol
  grid (`ComputeSymbolPt/Rect`, `DrawSymbolBox`, `PtInSymbolRect`) and
  `GetCopyProtectionSymbol`. `0x102928` means passed.
* **Hall of Fame**: the file is named at `0x102946`. Its table is a count word followed
  by 5 entries of 0x1e bytes (name at +2, score at +0x1e). `GetHallOfFameName` is a
  TextEdit field.
* **Credits**: `DoCreditsDialog`, `DrawCreditsPage`, `DrawCreditCategory` ('TEXT'
  resources) and `GetRectToBlink` (key help).
* **Events/menus**: `HandleEvent` handles the event record at `0x102cae` (16 bytes), and
  `HandleMouseDown` handles mouse clicks. Menu choices go to `HandleMenuChoice` (menus
  0x190–0x193: Apple/File/Edit/Game; handles at `0x102cf4/0x102cf0/0x102cec/0x102ce8`).
  `e2MAC_Hide/ShowMenuBar` save the menu-bar height in `0x102c5e`.

## Findings / porting notes

* **Stack depth matters.** The file layer in CODE 21 (`fileGetSpec`, `fileGetInfo`, ...)
  stores stack addresses in globals (e.g. `0xdfd388`). Time Manager and sound callbacks
  run at trap boundaries on the current stack. A port that uses less stack than the
  original therefore leaves different bytes in memory, and the checker reports a mismatch
  (this happened first in `CheckPlayNIS`).
  * Every non-leaf function now starts with `ENTER(link, nregs)` / `ENTER_NOLINK(nregs)`.
    This reserves the return address, the LINK frame and the saved registers, and the
    locals sit at their original A6 offsets.
  * Calls between ported C functions use `CALLC(argbytes, ...)`, which reserves the
    argument bytes the original pushed.
  * Other units will need the same treatment as soon as their code reaches the file layer.
* **Trampolines skip the _LoadSeg trap (infrastructure issue).** The generated C→68k
  trampolines (`call68_fn` → `fn_address`) load a missing segment by calling the native
  `LoadSeg()` directly. The original `jsr n(A5)` goes through the `_LoadSeg` trap, which
  runs the THINK C LoadSeg patch (CREL relocation) and advances the deterministic virtual
  clock by one trap step. In `PlayNIS` the NIS segment is unloaded (UnloadLevel). Through
  the trampoline, `osGetTime`'s Time Manager measurement came out 16 µs different
  (`PlayNIS` mismatch). Calling `NIS` through its jump-table entry (`nis_jt`) fixes it.
  Suggested general fix: have `call68_fn` jump through `JT_ADDR(i)` when the function has
  a jump-table slot.
* `InitSavedGameStruct` and `HOFAddPlayer` write the new pointer back into their own
  argument slot. They are `"conv": "manual"` with a glue that does the same write.
* Original quirk kept in `CopyProtectionDialog`: from the second try on, the symbol
  count is read through A0. At that point A0 still holds `A5-0x1CEC` (the timer table),
  left by `GetTimer` inside `GetCopyProtectionSymbol` → `WaitTimerNoPoll`. The C code
  reads `GH(0x106314)` on later iterations.
* `HandleEvent` uses `_FlushEvents` with D0 built on the stack (whichMask = 0xffff,
  stopMask = event mask). The generated wrapper does not set D0, so the trap is invoked
  by hand. PPostEvent (A12F) has no wrapper and is called with `tb_trap`.
* The jump tables (switches) at 0x10636, 0x134d4, 0x13a9a and 0x13bf0 were decoded by
  hand. The decompiler's version of `IsTimeForNIS` returns from inside the switch, which
  is wrong: the cases fall through to the common tail.
* `tc_sprintf` is variadic: it is called through its jump-table entry with `call_ptr`.
  `CheckOSErr` takes the message pointer as two 16-bit prototype slots.
  `StrItem` takes the item number in the high byte of its word.

## Signature overrides (`tools/sig_overrides/seg02a.json`)

* Return sizes were reduced where D0's high word is garbage and every caller uses only
  the word or ignores it:
  * set to 2: `FUN_00010518`, `PlayNIS`, `DialogFadeIn/Out`, `FUN_00010d56`,
    `FUN_00010f52`, `ResumeGame`, `SaveGame`, `GetCopyProtectionSymbol`, `PtInSymbolRect`,
    `HandleEvent`, `HandleMouseDown`, `HandleMenuChoice`
  * set to 0: `CopyProtectionDialog`, `DrawCopyProtectionDialog`, `HandleGameMenuChoice`,
    `CheckSoundChanges`, `e2MAC_HideMenuBar`, `e2MAC_ShowMenuBar`
* `InitSavedGameStruct`, `HOFAddPlayer`: `"conv": "manual"`.
* **Outside the unit**:
  * `Rnd`: `params [[4,2]]`. It reads its range word at 8(A6), and the index had no
    parameter.
  * `FadeInColors` and `FadeOutColors`: `params [[4,4],[8,4],[12,4]]`. The third long
    (speed, e.g. 0x40001) is read at 0x10(A6).

## Verification

`tools/check.sh` reports no mismatch: 13 functions in this unit are verified directly. Extra
runs with `POP2_NATIVE_EXCLUDE` (so children are called from 68k code and checked
individually), with a longer intro (`VMS=600000`, attract mode with credits and Hall of
Fame) and with extra key input (cmd-o, cmd-s, game-menu keys, no copy-protection poke)
verified 35 more individually, all without mismatch. Another 6 are covered only inside
checked callers: `DrawCreditCategory`, `DialogDrawStrId`, `SaveGameDialog`, `SaveGame`,
`IsDAWindow` and `ComputeSymbolPt`.

Not exercised by any run, so reviewed against the disassembly only:
* `DrawRoomFromNIS`
* `HallOfFameDialog`, `GetHallOfFameName`, `HOFAddPlayer`, `WriteHallOfFameFile` (they
  need a finished game)
* `GetCopyProtectionSymbol`, `PtInSymbolRect`, `DrawSymbolBox`, `ComputeSymbolRect` (they
  need a mouse click)
* `SaveRebirthPtStatus`, `SetRebirthPtInitials`, `FUN_00011a70`, `FUN_000110f2`
* `HandleMouseDown`, `HandleAppleChoice`, `HandleEditChoice`, `e2MAC_ShowMenuBar`
