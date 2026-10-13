# seg17a — CODE 17 "PoP Tools", first part (0x4e444–0x51ede)

All 82 functions are ported in `src/game/seg17a_poptools.c`.

## What the unit does

* **RLE blitters** `BankRLE` / `BankRLELR` (hand-written assembly). The shape pointer is
  passed through `StripAddress`. Rows start at +0xc as `{word len; data}`, and `index` is
  the first row. Each control byte has bit 7 = fill run (next byte, 0 = transparent), else a
  literal run, with count+1 pixels. `skip` pixels are skipped and `width` pixels are drawn
  per row. A color bank is added to each pixel: a byte for LR, which draws mirrored, and a
  long for `BankRLE`, which copies runs of 8 or more a long at a time. The pixmap base comes
  from `GetPixBaseAddr`, and the code switches MMU mode around the loop.
* **Palette layer**:
  * `gColorTable` (0x102cbe) is a 256-entry CTabHandle, and `gPalette` (0x102cc2) is the
    game window's palette, both created by `SetUpPalette`.
  * A "vCtab" is a handle holding a word count, then 4-byte entries `{r, g, b, index}`
    ('CTBL' resources).
  * `SetColors(vctab, flags, num, den)` writes a vCtab into gColorTable. Flags: 1/2/4 keep
    R/G/B, 8 gray, 0x10 invert, 0x20 scale by num/den, 0x40 no black.
  * `ShowColors` calls AnimatePalette.
  * `gSHAPColors` (0x1060ac) holds the current shape 'CTBL' (`GetSHAPColors`,
    `SetSHAPColors`, `SetNthvRGB`).
  * Fades:
    * `FadeInColors`/`FadeOutColors` take a speed long. Bits 16..18 give the step divisor,
      and the slot keeps only the low 5 bits.
    * `FadeToWhite`, `RapidFadeALL` (halve 15 times) and `FadeOutAllColors` (via
      `ColorTableTovCtab`).
  * Transitions: `VenetianOut`/`VenetianIn`.
  * Standard colors: `gStdColors` (0x1060b0, 8 RGBColors), `ResetStandardColors`,
    `SetWholePaletteToColor`, `StandardColors`, `SetFore/BackColor`. These also store the
    index in thePort (0x106782) fgColor/bkColor.
  * Saving and restoring the port colors: `FUN_0004fbe2`, `seg17_04fc04`, `FUN_0004fc26`,
    `seg17_04fc46`.
  * Others: `GiveSystemColors`, `CombineCtabs`, `TakeBackReleaseColors` (switch in/out).
* **Timers**: 6-byte records `{long start, word duration}` at 0x106310/0x106314.
  * `SetTimer(n, hi, dur)`: the middle word is the high word of a pushed long.
  * `GetTimer` returns the ticks left.
  * `WaitTimer` polls the key reader `gReadKeyProc` (0x10620c) through `CheckWaitFunc` and
    returns the key.
  * `WaitTimerNoPoll` and `Wait` (`Wait` returns WaitTimer's key).
  * `WaitFor`, `ButtonOrWaitFor` and `CompareTicks` are tick waits.
* **Windows/dialogs**:
  * `GameWindow`, `CenterWindow` (screen bounds at 0x10670e) and `SetGameRect` (512x384).
  * `DrawAbout`, `FrameDefault` and `ToggleCheckBox`.
  * `Sys6OrLater`/`RecheckSystem` check the system version, Color QuickDraw and the main
    screen depth against `gScreenDepth` (0x102ca8). They set `gPauseMark` 0x102db6.
* **Text**:
  * `TextInRect(str, len, rect, just, valign)` word-wraps; `TextInRectHeight` measures.
  * `StringRect` (C string), `CStrWidth`, `StrItem`, `Concactenate`.
* **Errors/exit**: `ErrorHandler` (alert 0x191 with code-segment strings), `CheckOSErr`,
  `Terminate` (restores the volume and menu bar, then `exit`) and `ShowErrorAndExit`
  (Pascal).
* **Resources/memory**:
  * `PoPGetResource`, `PoPNewHandle`, `PoPNewPtr` and `PoPNewHiPtr` retry after
    `MarkBGSetPurgable(1)`, then quit.
  * `OpenRsrcVerify`/`CloseRsrcFile` work on `{refnum, Str name}` records.
  * `PoPWriteResource`.
* **LZSS** (`PackLZ`, N=1024, F=66, threshold 2):
  * Buffers: text 0x106472, dad 0x10647e, lson 0x10647a, rson 0x106476.
  * Match length and position: 0x10646e/0x106470.
  * `InsertNode`/`DeleteNode`/`UnpackLZ` are in the rest of CODE 17.
  * `PackShapeLZ` packs in place and sets flag 0x400 in the first word.

## Porting notes

* **Arguments written by the callee** go through the ENTRY frame (`a6 + 8...` / `args_`),
  instead of manual glue. From 68k code these are the caller's real slots. From C they are
  the argument bytes that ENTRY reserves. The slot is first written with the argument value,
  so slot reads also work when the function is called from C.
  * `BankRLE*`: the stripped pointer in slot 4, and the row count counting down.
  * `FadeIn/OutColors`: `andi.l #$1f` on the speed.
  * `GiveSystemColors`: mode −= 0x20f.
  * `Randomize`: 0 becomes 2.
  * `Concactenate`: the CR count goes down to 0.
  * `CompareTicks`: negates d.
  * `TextInRectHeight`: width −= 2.
  * `TextInRect`/`TextInRectHeight` read the low byte of `len` from its slot.
  * `PoPGetResource` copies the type from its slot.
* `char` arguments live in the high byte of their word: `GetNthvRGB`, `SetNthvRGB`,
  `SetAndShowNthRGB`, `ShowNthvRGB`, `StandardColors`, `TakeBackReleaseColors`,
  `VenetianOut/In` (flag), `StrItem`, `Sys6OrLater`, `RecheckSystem` and
  `CheckFreeMemory`. NSetPalette gets `0x0100` (move.b #1).
* `_SwapMMUMode` takes its mode in D0. The original passes the high byte of the word 1, that
  is 0. A local helper sets D0.
* `GetTimer`/`SetTimer` leave A0 on the duration table (0x106314) as the original does. A
  68k caller (CopyProtectionDialog) is known to use it.
* `GetTimer`'s D0 keeps the high word of the possibly negative difference.
* `GiveSystemColors` uses D7 uninitialised for modes other than 0, 1000 and 0x20f..0x214.
  The port reads `cpu_get(R_D7)`.
* `Sys6OrLater` tests A3 even when GetMainDevice is unimplemented (A3 is then never set).
  The port reads `cpu_get(R_A3)` for that case.
* SectRect/ClipRect called through the cached trap pointers (0x1060a8/0x10609c) are called
  directly (`_SectRect`, `_ClipRect`).
* `ErrorHandler`'s jump table (0x5055a, 8 cases) was decoded by hand. Codes out of range go
  to case 0.
* Code-segment strings are addressed with `fn_address(FN_x) + offset`.
* Functions whose index result size is 0 still return the original D0 where callers use it:
  `Wait` (the key), `PollUser0`, `UnpackShapeLZ`, and trap results elsewhere.

## Signature overrides (`tools/sig_overrides/seg17a.json`)

All of them are for functions in this unit.
* `FadeOutAllColors`: `params [[4,4],[8,4]]`. The callers push the window and a speed long,
  and the function reads 0xc(A6).
* `ret 2`:
  * `Wait`: callers use the returned key; the index had 0.
  * `PollUser0`: FUN_00014c16 uses it.
* `ret 0`: `TakeBackReleaseColors`, `GiveSystemColors`, `GameWindow`, `CheckOSErr`. D0 is
  trap or caller garbage, and callers ignore it (checked in Ghidra).
* `ret 1`: `NotSameRects`, `CompareTicks`, `RectValid`, `Sys6OrLater`. These return a
  boolean in D0.b with garbage in the upper bytes on one path. InitGame tests `& 0xff`.
* `nocheck`: `ErrorHandler`, `Terminate`, `ShowErrorAndExit`. They never return (exit).

Overrides that already existed elsewhere and still apply: `FadeInColors`/`FadeOutColors`
`params [[4,4],[8,4],[12,4]]` (seg02a), and `ErrorHandler` two words (seg02b).

## Verification

* `JOBS=4 SECS=2400 tools/check.sh` after merging master: no mismatch and no timeout, with
  575 functions verified in total.
* In the full run, most functions of this unit are nested inside checks of their ported
  callers. They were also verified on their own:
  * `POP2_NATIVE_ONLY=<the 82 functions>` with `CALLS=100`, on all scenarios.
  * Group runs (`NormalizeRect,CenterWindow,TextInRect,TextInRectHeight`,
    `StrItem,CombineCtabs,UnpackShapeLZ`) with their callers on the 68k core.
* A temporary test driver (not committed) called the remaining functions through their
  checked entries with chosen arguments. It was triggered from the 600th BankRLELR call.
  * Palette: `CycleColors`, `ReactivatePalette`, `AddWindow`, `TakeBackReleaseColors` (both
    modes), `Get/SetSHAPColors`, `Get/SetNthvRGB`, and `SetColors` with all flag
    combinations.
  * Fades and transitions: `NOFadeSetColors`, `FadeToWhite`, `StandardColors`,
    `SetAndShowNthRGB`, `VenetianOut/In` (both orientations), `GiveSystemColors` (0, 1000,
    0x210, 0x214, other), `FadeOutAllColors`.
  * Rects, waits and text: rect functions, `Randomize` (0, 10, 1), `WaitFor`,
    `ButtonOrWaitFor`, `Concactenate`, `CompareTicks` (negative, both outcomes), `CStrWidth`.
  * Dialogs and system: `FrameDefault`, `ToggleCheckBox`, `RecheckSystem`,
    `CheckFreeMemory` (both), `DrawAbout` (with a posted Return key),
    `Sys6OrLater` with the flag.
  * Resources and memory: `PoPWriteResource(NULL)`, `PackLZ`/`PackShapeLZ` (packable and
    unpackable data), `PoPNewPtr/HiPtr`, `CloseRsrcFile`.
  * No mismatch.
* 78 of the 82 functions were verified directly. The other four were reviewed against the
  disassembly only:
  * `ErrorHandler`, `Terminate` and `ShowErrorAndExit` exit the application.
  * `FUN_0005165e` waits for a key from the key reader.
* Paths that were not executed:
  * The failure paths of `PoPGetResource`/`PoPNewHandle`/`PoPNewPtr`/`PoPNewHiPtr`/
    `OpenRsrcVerify`.
  * `CheckOSErr` with an error.
  * `Sys6OrLater`'s dialog.
  * `PoPWriteResource` with a real resource.
