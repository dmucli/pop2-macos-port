# seg21a — CODE 21 "Mohawk Libs 3", first part (0x5fb64–0x63134)

All 68 functions are ported in `src/game/seg21a_mohawklibs3.c`.

## What the unit does

This is the Mohawk **file layer**: a portable file API over the Mac File Manager, used by
the resource layer (`resOpenFile`, `resRead`, `resSeek`...), the config files (seg11b),
MIDI maps and Mohawk startup. The last two functions belong to the memory layer.

* **fileSpec** (C++ class, 0x106 bytes): +0 vRefNum, +2 dirID, +6 name (256 bytes). The
  name starts as a C string, possibly Unix-style ("/vol/dir/x", "../x", "./x"). Once
  `qualify__8fileSpec` has run, the first 0x46 bytes are an FSSpec, so the name is Pascal.
  * Constructors: `()`, `(const char *)`, `(short vRefNum, const char *path)`,
    `(FSSpec &)` and `(fileSpec dir, const char *name)`. The last one takes the directory
    **by value**: 0x106 bytes on the stack, modified in place.
  * Accessors: `equal` (0 or 0x2844), `getName`, `getVol`, `getPath` (Unix-style
    "/dir/..."), `getOSSpec` (full Mac path).
  * `validName` limits a name to 0x100 bytes with components of at most 31 characters.
  * `fileXlatSpecToMac` translates Unix-style paths to Mac paths.
* **Errors**:
  * `fileMapOSError` maps an OSErr to a Mohawk code: 0x283c..0x2849, 0x284b..0x284d,
    0x286f (no drive) and 0x2843 (bad spec). It also sets an error class byte and
    `gFileOSError` (0x107246).
  * Every API call stores its result in `gFileError` (0x107248). `fileGetError` and
    `fileGetOSError` read them back.
  * **Retry**: after a failure, `fileCheckException(err)` may call the exception proc
    `gFileXceptProc` (0x107362, installed by `fileSetXceptProc`). It is a Pascal
    `proc(VAR class): Boolean`, called with a message from 'STR#' 0x7d00 in
    `gFileXceptMsg` (-> 0x107366). If the proc says "retry", the operation runs again.
  * `gFileXceptOff` (0x107358, `fileSetXceptFail`) disables the proc. `fileInit` sets it
    to 1.
* **Shared parameter block** `gFilePB` (0x107466, 0x6c bytes): most calls use it. Its
  ioNamePtr often points to a buffer in the caller's stack frame, so a stack address ends
  up in a global (A5-0xb88 = gFilePB+0x12).
* **Current directory**:
  * Fields: `gFileCurVRefNum`/`gFileCurDirID` (0x107350/0x107352), `gFileCurPath`
    (0x1074d2, a Pascal full path) and `gFileAtRoot` (0x1075d2).
  * `gFileLaunchDir` (0x10724a, a fileSpec) is the directory at `fileInit` time.
    `fileQuit` restores it with `fileSetDir`.
  * `fileSetDir`/`fileGetDir`. The directory ID is found by FSMakeFSSpec on the path
    plus ":a" (`gFileStr5fe`/`602`), which gives the parID of a dummy file.
* **API**:
  * `fileOpen(mode, spec)` returns a refNum, or 0 on error. Mode bits:
    * 1 read, 2 write, 0x10/0x20 shared
    * 0x100 create, 0x200 create new only
    * 0x4000 truncate, 0x1000 append
  * Other calls: `fileClose`, `fileRead`/`fileWrite(&count, buf, ref)`,
    `fileSeek(whence, off, ref)`, `fileGetEOF`/`fileSetEOF`, `fileCreate`,
    `fileDelete`, `fileRename`, `fileExists`, `fileGetSize`.
  * `fileGetSize` sums a directory recursively with `getDirectorySize`.
  * Attributes and times: `fileGetAttr`/`fileSetAttr` (1 file, 2 dir, 4 invisible,
    8 locked), `fileGetOSAttr`/`fileSetOSAttr` (raw CInfoPB) and `fileGetTime`
    (6 bytes, sec..year-1980).
  * Volumes and drives: `fileGetSpec`/`fileGetInfo` (open file), `fileGetVolInfo`,
    `fileGetVolFree`, `fileMountVol`/`fileUnmountVol`/`fileEject`.
  * `fileGetDriveInfo` classifies a drive by its driver name: ".Sony", ".AppleCD" or
    ".AFPTranslator".
  * Enumerators `fileEnumDirs`/`Files`/`Drives`/`Vols(data, proc)` call a Pascal proc
    that returns "stop".
* **Memory layer** (`getMaster__FUs`, `findEmptySlot__FRs`): handle numbers are 1-based.
  The page is (n-1)>>9 and the record is (n-1)&0x1ff, in pages of 0x200 records of
  14 bytes (`gMemPages` 0x107626, count 0x107624). Bit 2 of the word at +4 means "in
  use". The errors go to 0x10761e, and the previous error to 0x107620.

## Porting notes

* **Stack garbage copied into fileSpecs (the main difficulty).** `qualify__8fileSpec` and
  `fileGetDir` copy a whole FSSpec (0x46 bytes) from their frame into a fileSpec.
  FSMakeFSSpec writes only the name's length+1 bytes, so the rest of the name holds stale
  stack bytes left by earlier calls. In the first check runs, `fileInit` and the
  by-value fileSpec constructor mismatched on exactly these bytes. The stale bytes were:
  * saved A6 values (`fileInit`'s frame pointer, saved by `tc_memset`'s link)
  * register images (`fileInit`'s D3 = 0x100; the constructor's A3 = name pointer)

  The port therefore reproduces these stack writes:
  * `ENTER(fn, link, mask)` = `ENTRY_LINK` plus what `link A6; movem.l <mask>,-(SP)`
    writes: the caller's A6 at (A6), and the CPU register images below the locals (the
    mask is the movem's: bit n = Dn, bit 8+n = An). CPU A6 is set to the frame while the
    function runs. The saved registers and A6 are restored on return (cleanup
    attribute), because the function changes CPU registers.
  * `RL/RW/RB(reg, value)` mirror every load of D3–D7/A2–A4 in the original (long, word
    or byte), so callees (68k or ported) save the same register values.
  * `JSR(ret, FN_callee, call)`: after each call, the original return address (runtime
    address of the instruction after the `jsr`, given as a Ghidra address) is written
    into the slot below the arguments. Trampolines and `cpu_call` push THUNK_BASE there
    instead. The Pascal callback helpers (`pcall_l/ll`) do the same.
  * `ASYNC`: the PB glue's `clr.b -(SP)` async flag writes only the high byte of its
    word. The port passes the byte already in memory as the low byte, so the trampoline
    writes it back unchanged. `fileOpen` does the same for `fileSeek`'s whence.
  * Every frame local is kept at its `a6 - k` offset and written like in the original.
    This covers parameter blocks on the stack, name buffers whose address goes into
    `gFilePB`, and locals the original reads uninitialised (`fileOpen`'s permission
    byte -0xe, `getPath`'s -4).
  * `tc_lmul_d0d1` in `fileGetVolFree` is replaced by a C multiplication. Its own stack
    writes are not reproduced (that function is never called by the game).
* **`fileCheckException`**: the Boolean result is written with `move.b` into the high
  byte of the Pascal slot, and D0's low byte is the Boolean. It is `"conv": "manual"`
  with `glue_fileCheckException`, which writes only that byte. The C function returns
  the slot word (Boolean << 8), which is also what the check-build trampoline returns.
  Callers test `(r >> 8) & 0xff` (the `XCEPT()` macro). The exception proc's word
  result is reduced to its **low** byte, as in the original.
* **Results in D0 for Pascal procedures** (rtd, no slot): `fileDerivePathFromIDs` and
  `fileGetVolName` return their error in D0. `fileSetDir`, `getOSSpec`, `getPath` and
  the fileSpec(short, char*) constructor test it. `getMaster`/`findEmptySlot` return the
  record pointer. Overrides: `"ret": 2/4, "retslot": false`.
* **By-value fileSpec**: `__ct__8fileSpecDC8fileSpecPCc` gets the params override
  `[[4,4],[8,2],[10,4],[270,4]]` (this, dir vRefNum, dir dirID, name). The body works
  on the argument block (`a6 + 0xc`), which the original modifies (PtoCstr/CtoPstr on
  the copy's name). When it is called from C, only the four declared values are written
  into the reserved block; the copy's name bytes are not. Its callers (`midiMapInit`,
  `midiMapGetDevice`, seg20b) should keep calling it with a real by-value copy, through
  its 68k entry.
* **Deeper calls**:
  * `fileOpen`'s append mode: `fileGetEOF` is called with `fileSeek`'s result slot and
    refNum already pushed (6 bytes).
  * The fileSpec constructors call `tc_strlen` with `fileXlatSpecToMac`'s 4-byte slot
    already reserved.
  * Both are reproduced with `DEEPER()`.
* **Uninitialised register**: `fileEnumDirs`/`fileEnumFiles` test D5 before setting
  it, so the first loop runs only if the caller's D5.w is 0. The port reads
  `cpu_get(R_D5)`. Both functions have no callers in the game.
* `fileSeek`'s whence is a `char`: it is the high byte of its word.
* Original quirks kept:
  * `fileSetAttr` with bit 8 locks then unlocks.
  * `fileGetSize` passes the C-string name as ioNamePtr.
  * `fileDelete` copies the name with strcpy, not as a Pascal string.
  * `_fileGetVolInfo`'s "locked" test against 0xffff8000 can never be true.
  * `getPath` on an unqualified spec writes through an uninitialised pointer when the
    name has no ':'.
  * `getMaster` accepts page == count.
  * A failing call retries forever while the exception proc keeps saying "retry".
    With `fileEnumDirs`/`Files`, the retry does not even clear the error.
* **Initialised data**: `fileInit` dumped the strings at 0x1075d6..0x10761c (see the
  `gFileStr*` defines).

## Verification

* `tools/check.sh` (22 scenarios, JOBS=4) passes with no mismatches, no crash and no
  timeout. The same holds with `CALLS=300`.
* Most of these functions are only called from other ported functions (seg11b config
  files, the constructors), so the default run checks them only as part of their
  callers. Three more runs checked them directly, with no mismatches:
  * `POP2_NATIVE_ONLY=<the 68 functions>`
  * `POP2_NATIVE_ONLY=qualify,fileDerivePathFromIDs,fileGetDriveVolume,fileCheckVol,fileCheckException`
  * `POP2_NATIVE_ONLY=validName,fileXlatSpecToMac,fileGetWDPath,fileEnumVols,fileMapOSError`
* **Temporary test hook** (not committed): it ran once from the checker's runner, right
  after `fileInit`, and called every function of the unit through its 68k entry. It
  used a scratch file and directory on the overlay disk. It covered:
  * create/open (all mode bits)/write/seek/read/EOF/close, error refNums
  * attributes, rename, delete
  * unqualified and invalid specs
  * the Unix-style path forms through `fileXlatSpecToMac`, `validName` and the
    constructors
  * directories: create, set/get dir, enum files/dirs, size via `getDirectorySize`, the
    parent of the root
  * volume calls
  * a fabricated drive queue, unit table and VCB queue for `fileGetDriveInfo` (all
    driver-name branches), `fileEnumDrives`, `fileEnumVols`, `driveVolumeEnumProc` and
    `_fileGetVolInfo`
  * the exception proc (68k stubs returning 0 and 1)
  * every `fileMapOSError` code
  * `fileInit`/`fileQuit` down to 0, and the static constructor/destructor

  There were no mismatches. **All 68 functions are verified** by the checker.
* Not reached even by the test hook: the retry branch of the exception loops (a proc
  that says "retry" once and then "stop"). The error paths of the volume calls that
  the emulator's File Manager never fails were reviewed against the disassembly.
