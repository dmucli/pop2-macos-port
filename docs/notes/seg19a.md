# seg19a — CODE 19 "Mac Libs 2", first part (0x563c4–0x58a8a), and CODE 1

120 of the 124 functions are ported. The CODE 19 functions are in `src/game/seg19a_maclibs2.c`. The 20 CODE 1 functions are in `src/game/seg01_runtime.c`. Signature fixes are in `tools/sig_overrides/seg19a.json`.

This is the THINK C / Symantec C runtime library (see `docs/THINKC_LIB.md`). Most of these routines are hand-written assembly, so they were translated from the disassembly instruction by instruction, keeping the word/long arithmetic and the condition-code tests.

## Not ported (4 functions)

These four stay on the 68k core. Each one changes its own return path, which a native hook cannot do: a hooked routine always resumes at the address the hook popped.
- `tc__switch_word_list`, `tc__switch_long_list`, `tc__switch_range` (0x10148/0x1015c/0x10170). These are switch helpers. Each pops its return address as a pointer to an inline case table and then jumps into the calling function. The game never references them: there is no `jsr` through their jump-table slots 0x2a/0x32/0x3a(A5). Ported code uses a C `switch`.
- `tc__pop_A1` (0x5683c, `movea.l (SP)+,A1; rts`). This is the tail of the bsr trick in `tc__A5_storage_addr`. It is entered with a data address as its return address. `tc__A5_storage_addr` is ported, and its glue sets A1 directly, so `tc__pop_A1` runs only inside checker children that run the original `tc__A5_storage_addr`.

## CODE 1 (startup and long arithmetic)

The CODE 1 routines work on the emulated registers themselves (`cpu_get`/`cpu_set`). Every register they leave behind is therefore the original's, and Toolbox traps see the same registers. All of them except the Pascal-convention `tc_lmul`/`tc_uldiv`/`tc_ulmod`/`tc_ldiv`/`tc_lmod` have `"conv": "manual"` and a `glue_` function.

- **`tc__start`** is entered by a `jmp` from jump-table entry 0, so no return address was pushed. Its glue undoes the 4 bytes the hook popped. In order it:
  1. clears $A4A and A6
  2. loads the string literals ('STRS', base in A4, stored at CODE1+4)
  3. initialises the globals ('DATA'/'ZERO', then 'DREL')
  4. installs the _LoadSeg/_ExitToShell patches
  5. runs the C++ constructors (`tc__call_STI`)
  6. pushes argc/argv/envp and calls `main` through the JT offset at CODE1+8
  7. calls the exit hook 0x6c(A5), then ExitToShell

  It is marked `nocheck`. The whole game runs inside it, just as it already ran inside the native `main_`.
- **`tc__call_STI`** keeps the callee's D0/A0 after the nested call, like a `jsr` would. Without this, `main` was entered with a different D0 than the original.
- **`tc__LoadSeg_reloc_patch`** runs on every segment load: the _LoadSeg patch is the stub {jsr patch; jmp old}. It saves and restores D0–D7/A0–A4 and reads the segment number at 0x3c(SP). A segment whose first byte has bit 7 set is relocated once with its 'CREL' resource. No segment of this game has that bit set.
- **`tc__init_globals`** falls through into `tc__relocate`. In C, both call a shared `relocate_body()`, so the stack depth is the original's.
- **Long arithmetic.** `tc__uldivmod_regs` is emulated exactly, including the estimate-and-correct path for 32-bit divisors and the scratch registers D2/D3/D7. `tc__ldivmod_regs` adds the sign handling. The C-callable helpers use the same core and preserve D1–D7. A zero divisor calls `port_divide_by_zero`. These helpers are reached only from the library itself; ported code uses the C operators.

## CODE 19 part 1

### Allocator (A5 offsets)

The allocator uses these globals:
- `-0x187a` last pool
- `-0x1876` first pool (the pools form a ring through +0x3a9c)
- `-0x1872` pool being searched
- `-0x186e` rover

Requests under 15000 bytes come from 0x3aa0-byte NewPtr pools. Each block has a 2-byte header: `~size` while allocated, `+size` while free. A pool ends with a -1 marker followed by the link to the next pool. Free neighbours are merged lazily during the search. Larger requests use `NewPtr(n+2)` with a 0xffff header.

`realloc` resizes in place when it can: it shrinks pool blocks, grows into free followers, and uses `SetPtrSize` for large blocks.

`calloc` writes the rounded size and then the total into its own argument slot. It therefore has manual glue, as does `tmpnam`, which stores the default buffer in its own slot.

### Exit machinery (A5 offsets)

The globals are:
- `-0x186a` count
- `-0x1862` table of 34 entries of 6 bytes: {fn, word run-on-ExitToShell}
- `-0x1866` set to 1 by `run_exit_handlers(0)`
- `-0x1796` saved exit hook
- `-0x1792` original ExitToShell address
- `-0x178e` jmp stub, used on 64K ROMs only

`_atexit` (`tc__atexit`, called by MohawkInit) patches ExitToShell to go to `tc__ExitToShell_patch`. `tc__run_exit_handlers` swaps in the A5 that was saved in the code segment (slot at `tc__A5_storage_addr`+2). Its global accesses are therefore made off that A5.

### stdio buffers

A FILE is 0x28 bytes, with 15 of them at A5-0x151c. The field layout is in THINKC_LIB.md.

With `--cnt > 0` semantics, `putc` calls `_flsbuf` when the count runs out. `_flsbuf` stores the character at `ptr` and flushes. `getc` uses `--cnt >= 0`.

The game itself uses only `atoi`, `atexit`/`_atexit` and, through `sprintf`, the long helpers.

### Console package

The console package makes a window with TextEdit attached to stdin/stdout/stderr. Its state lives at A5-0x1720..-0x16f2:
- the current window's 0x2e-byte state record, which is swapped with the window's refCon handle
- cursor row/col, rows/cols, tab width, the TEHandle
- the input line pointers -0x16e4..-0x16d4
- the mode byte -0x16f6: b7 RAW, b6 CBREAK, b5 ECHO, b4 reading, b3 7-bit, b2 echo to printer

A fake desk accessory driver is installed so that the window gets events: its open/close entry is `tc__console_driver_noop` and its control entry is `tc__console_driver_control`. The game never uses the console.

## Signature overrides

- All CODE 1 register routines, plus `tc__A5_storage_addr`, `tc__save_A5`, `tc__get_saved_A5`, `tc__console_driver_noop` and `tc__console_driver_control`: `"conv": "manual"`.
- `tc__start`: also `"nocheck"`.
- `tc_calloc`, `tc_tmpnam`, `tc__console_put_text`: manual glue, because they write their own argument slots.
- `tc__console_input_insert`: params `[[4,2],[6,4]]`. Its arguments are a char word and a long position; the index had a long and a word.

## Verification

`tools/check.sh` (all 22 scenarios) reports no mismatch. Checked in the scenarios:
- `tc__LoadSeg_reloc_patch`
- `tc__atexit`, `tc_atexit`
- `tc_atoi`
- `tc_lmul`, `tc_uldiv`, `tc_ulmod`

A temporary test hook, not committed, called the other functions through their 68k entries under `POP2_CHECK`. All of these were ok:
- **Allocator:** every path of malloc/calloc/realloc/free/malloc_small, including new pools, in-place shrink/grow, SetPtrSize, and the copy fallback.
- **ctype, tolower/toupper.**
- **Conversions:** atoi/atol/strtol/strtoul.
- **Long math:** all 169 operand pairs for the five C-callable helpers and the two register cores.
- **Files:** tmpnam/tmpfile/fopen/freopen/fopen_impl (all flag combinations)/remove/rename/tmpnam_impl, and `_filbuf`/`_flsbuf`/prep/flush/reset through a real file.
- **Exit machinery:** atexit slots, save/get A5, run_exit_handlers, exit_hook_all.
- **CODE 1:** save/restore handle state, relocate, install/remove trap patches, load_STRS, call_STI, init_globals.
- **Console:** init_std, which covers InitToolbox, freopenc, new_window, install_driver, set_window and clear_from_line as nested calls. Also checked individually: cgotoxy, cgetxy, csettabs, csetmode, cinverse, cshow, chide, ccleol, ccleos, clear_from_line, clear_eol_impl, te_deactivate, fill_scratch, lock/unlock_text, trim_text, resize_te, input_delete, copy, cut, paste, edit_command, key (RAW/CBREAK paths), idle, zoom, grow, enter/leave_port, set_window, select, driver_control (every csCode), driver_noop, poll_event, console_proc, cecho2file, cecho2printer, end_echo, print_echo, install_driver, inverse_textProc, stdio_exit_cleanup, close_window.

The startup routines were also compared with a trap log against the original (`POP2_TRAPLOG` with the CODE 1 functions excluded). D0 and SP are identical at all 2.5M traps of a 20 s run. Only scratch A0/A1 left by other C functions differ.

Not dynamically verified, so reviewed against the disassembly:
- the console routines that edit the TextEdit text (write, put_text, newline, te_activate, te_insert_at, input_insert, key with ECHO, mousedown, content_click) and print_file. The emulator's TextEdit crashes on the console's selections (TEPaste in `te_replace_sel`) with the original 68k code too.
- `tc_exit`, `tc__exit`, `tc__ExitToShell_patch`, which end the process.
