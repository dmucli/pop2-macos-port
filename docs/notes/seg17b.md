# seg17b — CODE 17 "PoP Tools", second half (0x51ee8–0x554d0)

All 79 functions are ported in `src/game/seg17b_poptools.c`.

## What the unit does

* **LZSS** (Okumura-style, 1 KB ring buffer, match length 3..66):
  * `UnpackLZ` unpacks a packed resource handle in place. Header: +0 flag word (0x400 =
    LZ-packed), +0xc unpacked size, packed data at +0x10 of the packed copy.
  * `InsertNode`/`DeleteNode` maintain the encoder's binary tree. The arrays are
    `gLZBuf` 0x106472 (ring buffer), `gLZRson` 0x106476, `gLZLson` 0x10647a and
    `gLZDad` 0x10647e (word arrays, NIL = 0x400). The match is in 0x10646e (length) and
    0x106470 (position). The encoder itself, `PackLZ`/`PackShapeLZ`, is in seg17a.
* **"Pack all data" utility** (game menu or startup):
  * `PackAllData` walks the data folder with PBGetCatInfo. For every file of type
    `*0x106350` that is not a BG set file (`IsBGSetFile`, names in the table at 0x1038de,
    7 entries of 6 bytes), it calls `LZPackDataFile`. That packs every unpacked SHAP
    resource bigger than 30 bytes and writes it back with `PoPWriteResource`.
  * The progress window (WIND 0x195, `gPackWin` 0x10646a) has two bars: bar 0 at 0x106328
    (resources of the file; value/max 0x106464/0x106462) and bar 1 at 0x106330 (files;
    0x106460/0x10645e).
  * `gStopPacking` 0x106468 is set by escape (`HandleKeyDownInPackingWindow`).
    `GetEstTimeToPackData` times the packing of SHAP 0x7d01 of ThroneNIS.rsrc and caches
    the estimate in 0x102ce4.
  * From the game, `FUN_00053074` unloads the level first and `GoBackToGameAfterPacking`
    reloads it.
* **Offscreen worlds** (record of 0x22 bytes: +4 saved port, +0xc GrafPort of
  `BuildOffBitMap`, +0x10 saved GDevice, +0x14 GWorld, +0x18 bounds, +0x20 built flag):
  * `BuildSOffWorld`/`BuildOffGWorld` create an 8-bit GWorld. `SaveCurrentWorld` and
    `RestoreOriginalWorld` switch to it and back. `DisposeSOffWorld` frees it.
  * `DrawSOffWorldToWindow`/`DrawOffToOff` use CopyBits with the RGB colors at
    0x106482/0x106488.
  * `InvertOffWorld` swaps rows top to bottom. Its first "bottom" row is `rect.bottom`
    itself, one past the rectangle (original behaviour).
* **Saved rects**: `SaveRect` copies a rectangle of thePort into a handle in the raw
  shape format (type 0xb000). `RestoreRectKeep` draws it back with `DrawIt`, and
  `DeallocMapSave` frees it. The rectangle is first clipped with SectRect through the
  cached trap address at A5-0x1f58.
* **Shapes (SHAP)**: the header is +0 type/flags, +2 row bytes, +4 width, +6 height, and
  the data starts at +0xc.
  * The type is `word & 0xff80`: 0x9000 1-bit bitmap, 0x9100 bitmap + mask, 0xa000
    4-bit, 0xb000 raw bytes, 0xb100 RLE.
  * Flag 0x200 means byte-RLE packed; `GetShape` expands it into a new handle. Flag 0x400
    means LZ packed. Bits 0x10/0x20 mean flipped LR/TB, and the low nibble is the left
    padding.
  * `DrawIt(shape, bits, x, y, mode, color, outRect)` clips against thePort's visRgn
    bbox and dispatches by type:
    * raw shapes: `PaintCopy`, or `PaintTrans` when mode is 0x10 (color 0 transparent);
    * 4-bit shapes: `Paint4BitShape`;
    * RLE shapes: `PaintRLE`/`PaintRLELR`/`PaintRLETB` by flip bits. When color > 1,
      the seg17a bank painters `BankRLE`/`BankRLELR` are used instead, with color − 1;
    * bitmaps: StdBits, or CopyMask when mode is 0x10.
    * Without a shape, `bits` (a 1-bit BitMap record of type 0x1000) is painted as a
      region in `color`.
  * RLE rows: a word length, then run bytes. Bit 7 set means repeat the next byte
    (count−1 in the low 7 bits), otherwise it is a literal of count bytes. Color 0 runs
    are skipped when mode != 0.
  * `FlipSHAP` flips in place to the wanted orientation through `FlipSHAPLR` (tables from
    `FUN_00053d72`: bit-reversed bytes at 0x106492, nibble-swapped bytes at 0x106592,
    built flag 0x10648e) and `FlipSHAPTB`.
* **Shape lists**: `LoadShapeList` builds a pointer block: +0 SHPL handle, +4 SHPL id,
  +6 count, then items of 10 bytes from +8 (+6 = shape handle).
  * `PrinceGetnShapeItem(Size)` and `PrincePaintShapeIdx` access its items.
    `UnloadShapeList` frees it.
  * SHPL resource: +0 first SHAP id − 1, +2 count (`GetShpListItem`).
* **Colors**: `GetCTBLResource`, `RemapCTBL` (shifts the pixel values at +5 of each
  entry) and `SetCTBLColors`.
* **Misc**:
  * alerts: `MainAlert` (StopAlert with ParamText of a C string), `AlertOKOnly`,
    `AlertYesNo`, `AlertOKCancel`;
  * `Rnd(n)` = Random mod (n+1), lazily seeded by `SetRndSeed` (randSeed 0x106704,
    flag 0x106692);
  * `FUN_000550b8` polls events (key: the char, cmd-Q: 1);
  * `pstrcpy`, `AlterRect`, `EncloseRect` (UnionRect through A5-0x1f5c, then
    EqualRect), `MakeShapeRect`, `SetStatTextCtrl` and `PoPFillRect`.

## Porting notes

* **Arguments written by the callee** (`"conv": "manual"` with glue that writes the slots
  back):
  * `DrawPackingProgress`: cur/max = −1 are replaced by the stored values.
  * `SetPackingWindowFilename`: NULL is replaced by the address of the stored name.
  * `Paint4BitShape`: `andi.w #1,(0x1a,A6)`. Its glue also returns the entry D0, which
    the original restores with movem.
  * `PaintRLE`/`PaintRLELR`/`PaintRLETB`: the stripped shape pointer goes into (8,A6), and
    the row count is decremented in (0x14,A6).
  * `DrawIt`: color − 1 for the bank painters.
  * `PrinceGetnShapeItemSize`: the out pointer is cleared when the shape is missing.
* **Register dependencies**:
  * `UnpackLZ` tests the caller's A4 (`move.l A4,D0; bne`), which is the shape handle in
    `GetShape` → `UnpackShapeLZ`. The port reads `cpu_get(R_A4)`.
  * `GetShape` sets A4 to the handle around its call of the 68k `UnpackShapeLZ`, then
    restores it.
* **Extra pushed word**: `DrawIt` pushes a long color pattern for `BankRLELR`, which reads
  only its high word. The port pushes the low word itself before the call.
* `GetShape` calls the second GetResource with `MarkBGSetPurgable`'s argument still
  pushed; the port pushes that word too.
* **The RLE blitters** (`PaintRLE`, `PaintRLELR`, `PaintRLETB`) are hand-written
  assembly. They are translated register by register, including the 32-bit D0
  countdown, the word/long mix and `bfins`/`bfextu`.
  * D0's upper word at the start is SwapMMUMode's result: the runtime returns 1,
    zero-extended.
  * The `cmpa.l #$9000` in `DrawIt` compares a sign-extended word and is never true, so
    mode 0x10 always uses CopyMask for bitmaps.
  * `Paint4BitShape`'s odd-start branch continues after the first pixel even when the
    count goes negative (`bne`), as the original does.
* `FlipSHAP`'s jump table (0x544de) was decoded by hand:
  * LR: 0x01 0x10 0x23 0x32;
  * TB: 0x02 0x13 0x20 0x31;
  * both: 0x03 0x12 0x21 0x30.
* `FlipSHAPLR` uses the bit-reverse table, not the nibble table, for the middle byte of
  odd-width 4-bit shapes (original behaviour).
* Strings in the code segment are addressed with `fn_address(FN_x) + offset` (helper
  `cs()`). `FUN_00052c60` has no MacsBug name, but its strings follow it.

## Signature overrides (`tools/sig_overrides/seg17b.json`)

All of them are for functions of this unit.
* `BuildOffGWorld`: `params [[4,4],[8,4]]`. It reads the rect at 0xc(A6). This is
  identical to the entry in seg16.json.
* `"conv": "manual"`: `DrawPackingProgress`, `SetPackingWindowFilename`,
  `Paint4BitShape`, `PaintRLELR`, `PaintRLE`, `PaintRLETB`, `DrawIt` and
  `PrinceGetnShapeItemSize`.
* Return sizes (each checked against all callers):
  * `ret 0`: `FUN_00052c60`, `HandleUpdateEvtInPackingWindow`, `RestoreRectKeep`,
    `DeallocMapSave`, `GetMapSaveRect`, `Paint4BitShape`, `PaintCopy`, `PaintTrans` and
    `DrawIt` (D0 left over, never used); `RemapCTBL` and `SetStatTextCtrl` (no caller
    uses D0).
  * `ret 1`: `UnpackLZ` (the failure paths keep the upper bytes of the
    HandToHand/HUnlock D0), `LZPackDataFile` and `PackAllData` (callers test the byte).
  * `ret 2`: `GetEstTimeToPackData`, `IsBGSetFile` and `FUN_00053074` (callers use the
    word).
  * `ret 4`: `GetSHPLResource`, `GetCTBLResource` and `hGetTxt4` (they pass on
    PoPGetResource's handle, and their callers use it; the index said 0);
    `CheckPackDataFromGame`, `AlertOKCancel`, `AlertYesNo` and `EncloseRect` (D0 is
    exactly 0/1).
* Existing overrides elsewhere that concern this unit: `Rnd` (one word) and
  `InvertOffWorld` (two longs) in seg02a/02b/04a/05a/05b.

## Verification

`tools/check.sh` (all scenarios) reports no mismatch for this unit.
* An extra run with `POP2_NATIVE_ONLY` = this unit verified the functions that are
  otherwise always reached from inside other checks: `GetShape`, `BuildSOffWorld`,
  `DisposeSOffWorld`, `DeallocMapSave` and `UnloadShapeList`.
* A run with only `UnpackLZ`, `BuildOffGWorld`, `FUN_00053d72`, `hGetNMapShpList`,
  `GetSHPLResource` and `SetRndSeed` native verified those six.
* A temporary test hook (not committed) called the rest through their 68k entries with
  chosen arguments. That covered:
  * crafted 0xb000/0xa000/0x9000/0x9100/bits shapes in every flip;
  * RLE shapes in four orientations, including the bank painters;
  * SaveRect/RestoreRect, offscreen worlds, the packing window, the LZ round trip and
    the alerts (keys posted into the event queue);
  * the static text, and a full `PackAllData` run over the data folder (twice).

  All of these were ok, with the exceptions below.

**Checker limits found:**
* `DrawEmptyProgressBar` fills with `GetIndPattern(0, 4)`, but this runtime's System
  file has no 'PAT#' 0. The pattern is uninitialised stack, so `PackAllData` showed pixel
  differences. They disappear when the pattern bytes are cleared in both runs (test
  only).
* **Runtime bug (not in this unit):** `ResourceDispatch` (A822) selector 3,
  SetResourceSize, is unimplemented and does not pop its 8 bytes of arguments.
  * `PoPWriteResource` (seg17a) then restores D3/D4 from the wrong place with
    `movem.l (SP)+`. Its caller's registers are corrupted.
  * In the original `LZPackDataFile`, the CloseResFile/UseResFile at its end then get
    garbage refnums. The file stays open, and its changes are never written.
  * The C port uses the right values, so the check showed a mismatch for every file
    that was really packed.
  * With a temporary runtime patch (pop 8 bytes for selector 3, not committed), all 50
    `LZPackDataFile` calls and `PackAllData` verified ok. The overlay files written by
    the port and by the original were then identical.
  * The runtime should implement SetResourceSize.

**Not executed in any run**, reviewed against the disassembly only:
* `hGetTxt4` (there is no 'STR ' resource to load; same code as `GetCTBLResource`);
* the `UnpackLZ` failure path where `tc_HandToHand` fails;
* `Terminate` in `hGetNMapShpList`.
