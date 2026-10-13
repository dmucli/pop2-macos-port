# seg19b — CODE 19 "Mac Libs 2", second part (0x58a8a–0x5b12e)

All 88 functions are ported in `src/game/seg19b_maclibs2.c`. This is the second half of the
THINK C / Symantec runtime library (see `docs/THINKC_LIB.md` for the whole map). In this
library `int` is 32 bits: every `int` argument takes a 4-byte slot and mem*/str*/printf
results are 32-bit.

## What the unit does

* **stdio** (FILE = 0x28 bytes, 15 slots at A5-0x151C: stdin, stdout (A5-0x14F4), stderr,
  then 12 free slots from A5-0x14A4; errno at A5-0x155E). The FILE fields are listed at the
  top of the source (`F_FLAGS` ... `F_PROC`).
  * Disk streams: `tc__file_proc(fp, cmd)` is the stream proc (0 fill with PBRead at the
    absolute position pos-cnt, eofErr with data is ok; 1 write with PBWrite, extending the
    EOF with PBSetEOF first; 2 close). Text mode converts CR->LF after reading and LF->CR
    in place before writing (`tc__replace_char`, through memchr). `tc__file_close` closes
    and deletes temporary files (bit 0): on HFS by the name/dirID in the FCB (PBHDelete),
    on MFS by finding the file's index with PBGetFInfo. `tc__set_file_type` sets the Finder
    type ('TEXT' or A5-0x1524) and creator (A5-0x1520) of new files.
  * `tc_fseek`/`tc_ftell`/`tc_fgetpos`/`tc_fsetpos`/`tc_rewind` (disk files only, else
    errno 19), `tc_fwrite` (direct proc writes for big aligned binary writes, else memcpy
    into the buffer or putc), `tc_fgets`/`tc_gets`/`tc_fgetc`/`tc_getc`, `tc_ungetc`
    (moves the buffered data up or uses the pushback byte at fp+5), `tc_setbuf`/
    `tc_setvbuf`, `tc_fflush` (NULL = all writing streams), `tc_fclose`,
    `tc__close_all_files` (atexit), `tc__find_free_file`, `tc__checkfp` (std streams are
    attached to the console lazily; a closed FILE becomes a dummy one-byte stream),
    `tc__fp_proc_fill/flush/close` (call fp->proc).
  * String FILEs: `tc__string_fp_init` (refnum -1, null proc `tc__stdio_nullproc`),
    `tc__sprintf_fp_init` (cnt = -1: unbounded), `tc__sscanf_fp_init` (cnt = strlen).
* **printf engine** `tc_vfprintf`: spec copied from A5-0x12C4 (flags - + space # 0, width,
  precision, h/l/L). Conversions d i u o x X p c s (%#s = Pascal string) n %% and the
  floating e E f g G (10-byte SANE extended arguments, converted by
  `tc__printf_num2dec` with Pack4 FX2DEC, INF/NAN texts). Output through the inline putc
  (`tc__flsbuf` when cnt <= 1) and `tc_fwrite`. An unknown conversion ends formatting.
  Wrappers: `tc_sprintf`/`tc_vsprintf` (FILE on the stack, NUL-terminated), `tc_fprintf`,
  `tc_printf`, `tc_vprintf`.
* **scanf engine** `tc_vfscanf`: spec copied from A5-0x1272 ('*', width, '?' = base from
  the argument list, h/l/L). Conversions d i o u x X p (with overflow detection: errno 34
  and LONG_MIN/MAX-like values), e f g (Decimal record converted by `tc__scanf_dec2num`,
  Pack4 FDEC2X/FCLASSX), s, [...] (256-bit scan set at A5-0x126A: `tc__scanset_*`), c, n.
  Wrappers `tc_sscanf`/`tc_vsscanf`, `tc_fscanf`, `tc_scanf`, `tc_vscanf`.
* **mem/str**: `tc_memcpy` (forward byte copy), `tc_memmove`, `tc_memcmp`/`tc_strcmp`
  (exactly -1/0/1), `tc_memchr`, `tc_memset`, `tc_strcpy`, `tc_strcat`, `tc_strchr`,
  `tc_strrchr`, `tc_strcspn`, `tc_strpbrk`, `tc_strspn`, `tc_strstr`, `tc_strtok` (saved
  pointer A5-0x1206), `tc_strlen`, `tc__c2pstr_copy`.
* **qsort**: `tc_qsort` keeps base/size/cmp at A5-0x1286/-0x1282/-0x127E and sorts
  indices with `tc__isort(n, cmp_idx, swap_idx)` (callbacks at A5-0x127A/-0x1276) and the
  recursive `tc__quicksort` (pivot = first element, scan index in the global A5-0x128A).
* **signal/raise**: handlers 1..6 at A5-0x124A (0 = SIG_DFL -> `_exit`, 1 = SIG_IGN).
* **operator new/delete**: `tc__new_alloc`/`tc__new_free`/`tc__new_alloc_small` are a copy
  of the malloc allocator with their own pools (list A5-0x1964, first A5-0x1960, current
  A5-0x195C, rover A5-0x1958): 15008-byte NewPtr pools, 2-byte headers (~size when
  allocated, +size when free, 0xFFFF pool trailer followed by the link to the next pool),
  first fit with lazy merging of free neighbours; big blocks NewPtr(n+2) with header
  0xFFFF. `tc_operator_new` (Pascal-style, pops 4) turns size 0 into 2 and retries through
  the new_handler at A5-0x1954.
* **long math**: `tc_lmul_d0d1`, `tc_ldiv_d0d1`, `tc_uldiv_d0d1`, `tc_lmod_d0d1`,
  `tc_ulmod_d0d1` take D0 (dividend) and D1 and return D0; lmod/ulmod leave the quotient
  in D1, ldiv/uldiv leave it in D0 and D1, lmul leaves D1 = hi(D0)*lo(D1) and A0 = the
  product. The `_stk` shims (0x5AF40...) load D1 = 4(SP) (divisor), D0 = 8(SP) and fall
  into them. Ported with hand-written glue (`"conv": "manual"`); they set D1/A0 like the
  originals. Division by zero calls `port_divide_by_zero`.
* **C++ statics**: `tc__run_STI` keeps '_STD' 0 (A5-0x194C, registered with atexit) and
  calls the constructors listed in '_STI' 0 (A5 offsets); `tc__run_STD` calls the
  destructors.

## Porting notes

* Functions that write their own argument slots do it in the ENTRY frame (`a6 + 8...`),
  after rewriting the slot with the argument: `tc_fwrite` (ptr), `tc_fgets` (n),
  `tc_vfprintf` (va), `tc_vfscanf` (fmt, va), `tc_operator_new` (size).
* The variadic functions find their arguments above the fixed ones (`a6 + 0x10`): they are
  only valid when entered from 68k code (`call_ptr(JT_ADDR(FN_tc_sprintf), ...)`).
* `tc_vsprintf`/`tc_vsscanf` push vfprintf's last two arguments before calling the FILE
  init in the original; the port reproduces those 8 bytes so the callee frames sit at the
  original SP.
* `tc__scanset_invert` is a register routine that leaves D0 alone: the port returns the
  entry D0.
* `tc__c2pstr_copy` first stores the low byte of the entry D0 at dst[0] (overwritten by the
  length afterwards); reproduced.
* The CODE 1 helpers `tc_lmul`/`tc_ulmod`/`tc_uldiv` used by the library are C operators.

## Signature overrides (`tools/sig_overrides/seg19b.json`)

* `tc__printf_num2dec`: the 10-byte extended argument is passed by value after the three
  longs: params `[4,4] [8,4] [12,4] [16,4] [20,4] [24,2]`.
* `tc_lmul_d0d1`, `tc_ldiv_d0d1`, `tc_uldiv_d0d1`, `tc_lmod_d0d1`, `tc_ulmod_d0d1`:
  `"conv": "manual"` (register arguments; `glue_*` in the source).
* `tc__run_STI`, `tc__run_STD`: `"ret": 0` (register routines, D0 is whatever the last
  trap left; their callers ignore it).

## Verification

* `tools/check.sh` (all scenarios): no mismatch. The game itself reaches 30 of the
  functions (mem*/str*, sprintf/sscanf and their engines, operator new/delete and the
  allocator, the long-math register helpers, `tc__run_STI`); 16 of them are entered at top
  level and checked directly, the others only nested inside those.
* A temporary self-test (not committed) called every function of the unit through its
  68k entry at top level with the checker on (CALLS=1000): printf/scanf formats (flags,
  widths, precisions, h/l/L, %n, %#s, %p, floats through SANE, overflow cases, scan sets),
  qsort and its internals, every mem*/str* function, the long-math shims and register
  helpers (signed/unsigned, large divisors), signal/raise, operator new/delete (small,
  pool-sized, > 8 MB), string FILEs (fgetc/getc/ungetc/fgets/fseek/ftell/fwrite/
  setvbuf/fflush/fclose), disk files (fopen w/r/ab/w+, fprintf, fgets, fseek modes,
  direct fwrite, tmpfile deleted on close, remove, `tc__file_proc` with every command,
  `tc__file_close`), gets/scanf/vscanf with stdin replaced by a string FILE, printf/vprintf
  on the console, `tc__run_STD` with a fake '_STD' list: all 88 functions verified, no
  mismatch.
