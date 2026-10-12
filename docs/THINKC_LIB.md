# THINK C / Symantec runtime map (pop2_image.bin)

All 262 functions in CODE 1 (0x10004-0x10430), CODE 19 "Mac Libs 2" (0x563C0-0x5B130) and CODE 20 Toolbox glue (0x5B130-0x5B9E0) were identified, renamed `tc_*` in Ghidra (`tc__*` = library-internal), given custom-storage prototypes, stack purges and a plate comment, and the program was saved. The full per-function detail (arg stack offsets, sizes, return location, exact semantics) is in `lib_map.json`. In that file `args` offsets are measured from SP at entry (return address at 0), and `purge` is the number of argument bytes the callee pops.

## Important findings (read before writing native replacements)

1. **`int` is 32 bits in this library, not 16.** Every `int` parameter of the ANSI library takes a 4-byte stack slot: `memset(s, c, n)` reads `c` as a long (its low byte is at 0xB(SP)), and the same holds for `memchr`/`strchr`/`strrchr`, `strtol`'s base and `atexit`'s count. `printf` `%d`/`%u`/`%x`/`%c`/`*` all consume 4 bytes, and only `h` narrows to 16 bits. Game code sign-extends its 16-bit `short`s before calling, e.g. `DialogDrawNumber` does `ext.l` and then `sprintf(buf, "%d", (long)v)`. `strlen`, `memcmp` and `strcmp` return 32-bit values in D0.
2. **Floating point:** the printf/scanf `double` arguments are 10-byte SANE `extended` values, and conversion goes through SANE Pack4 (`num2dec`/`dec2num`). In scanf, `%f` stores a 4-byte float, `%hf` an 8-byte double and `%lf`/`%Lf` a 10-byte extended. The game does not appear to use floating formats.
3. **Long-math helpers come in two families:**
   * CODE 1 C-callable helpers, called through JT 0x42/0x4A/0x52/0x5A/0x62(A5): `tc_lmul`, `tc_uldiv`, `tc_ulmod`, `tc_ldiv`, `tc_lmod`. They take C argument order (first operand at 4(SP), the divisor at 8(SP)), but **the callee pops the 8 bytes**. They return D0 and preserve all other registers. Only the library uses them.
   * Symantec C++ register helpers used by the game through JT 0x13CA-0x13EA: `tc_lmul_d0d1` (D0=D0*D1, clobbers D1/A0), `tc_ldiv_d0d1` (D0=D1=quotient), `tc_uldiv_d0d1`, `tc_lmod_d0d1` (D0=remainder, D1=quotient; used by `Rnd`, `osGetTime`, ...), `tc_ulmod_d0d1`. In each, D0 is the dividend and D1 the divisor. Remainders take the sign of the dividend and quotients truncate toward zero. A zero divisor raises a 68k divide-by-zero exception. Unused stack-entry shims exist at 0x5AF40/68/78/88/B6. They load D1=4(SP) and D0=8(SP), so the divisor comes first.
4. **There are two separate custom allocators,** both "pool + NewPtr":
   * `tc_malloc`/`tc_free`/`tc_realloc`/`tc_calloc` use globals A5-0x187A..-0x186E.
   * `operator new`/`operator delete` (`tc_operator_new`/`tc_operator_delete`, both callee-pop 4) use a byte-identical copy with its own pools (A5-0x1964..-0x1958) and retry with a new_handler at A5-0x1954.
   * The common scheme: requests under 15000 bytes come from 15008-byte NewPtr pools. Each block has a 2-byte header (`~size` when allocated, `+size` when free) and is found by first fit with lazy coalescing. Pools are never freed. Larger requests use `NewPtr(n+2)` with header 0xFFFF, and requests over 8 MB return NULL. **Memory is not zeroed.** Note that `operator new(0)` allocates 2 bytes.
5. **stdio layer:** a FILE is 0x28 bytes and there are 15 slots at A5-0x151C (stdin, stdout, stderr, then 12 free).
   * Fields: +0 flags (bit7 = std stream not yet attached, bit6 binary, bit5 EOF, bit4 error, bit3 dirty/writing, bit2 malloc'd buffer, bit1 append, bit0 temp file); +1 bit7 ungetc pushback; +4 one-byte unbuffered buffer; +5 pushback byte; +6 refnum; +8 buf; +0xC bufsize; +0x10 ptr; +0x14 cnt; +0x18 logical file position; +0x1C EOF; +0x20 console window / line-buffer flag; +0x24 proc(fp, cmd) with 0=fill, 1=flush, 2=close.
   * Disk files use absolute-position PBRead/PBWrite. Text mode converts CR to LF on read and LF to CR **in place in the buffer** on write. `errno` is at A5-0x155E.
   * `sprintf` uses a stack FILE with cnt=-1, so it is unbounded.
   * The game itself uses the Toolbox glue (FSOpen/FSRead...), not stdio, for file I/O.
6. **THINK C console package** (window + TextEdit, fake DA driver for event routing, Edit menu, echo to file/printer): about 50 functions at 0x56AEA-0x5841C. It is reachable only through the stdin/stdout streams, which the game never touches. Names there are best-effort ("console internal", low confidence).
7. **Exit machinery:**
   * `tc_exit` ignores its status argument. It calls the 0x6C(A5) hook, then ExitToShell.
   * `atexit` keeps 34 six-byte slots. Slot 0 is used for `tc__close_all_files` and slot 1 for `tc__stdio_exit_cleanup`.
   * `tc__atexit` (THINK `_atexit`, used by MohawkInit) also patches ExitToShell so its handlers run on any quit.
   * CODE 1 startup patches _LoadSeg, so that CODE segments carrying the "needs relocation" bit get their 'CREL' relocations applied, and patches ExitToShell.
   * Globals are initialised from 'DATA'/'ZERO'/'DREL' and string literals from 'STRS'. Initialised globals (format strings of atoi/strtol, the ctype table, FILE templates) are therefore **zero in this image**. Their content had to be inferred.
8. **Pascal glue results:** for Pascal glue the OSErr/function result is written to the caller-reserved slot above the arguments, so the decompiler shows it as `in_stack_xxx`.
   * The PB* glue (`tc_PBOpen` etc.) takes `(pb, async:Boolean)` and pops 6.
   * `tc_FSOpen` tries OpenDF and falls back to PBOpen on paramErr.
   * `tc_FSRead`/`tc_FSWrite` read and write at the mark.
   * `tc_GetHandleSize`/`tc_GetPtrSize` return 0 on error.
   * `tc_GetVInfo` computes freeBytes = ((alBlkSiz>>9)*freeBlocks)<<9.
   * `tc_Gestalt` contains a fallback table when the trap is missing.
   * `tc_SysEnvirons` fills the record by hand when the trap is missing.
9. **Uncertain:**
   * The ctype masks 0x80/0x40 are named upper/lower from the tolower/toupper code, but the source order suggests the pair might be swapped. They are unused.
   * The format strings for atoi/strtol/strtoul are in DATA and could not be read.
   * strtoul with a '-' sign is inferred from the decompiled scanf engine.
   * The CODE 1 switch helpers (`tc__switch_*`) are not referenced by the game.
   * There are a few undisassembled byte runs in CODE 19 (0x56ADA, 0x590F6-0x5911C, 0x5AF2A, 0x5B022) that may be unreferenced library code (e.g. fputc/putc); no functions were created there.

## Function table

| addr | name | conv | purge | meaning |
|---|---|---|---|---|
| 0x10010 | `tc__start` | regs | 0 | THINK C application entry (JT entry 0). |
| 0x10058 | `tc__call_STI` | regs | 0 | If the JT offset stored at 0x1000C is non-zero, jsr through it (A5+offset) to tc__run_STI (C++ static constructors). |
| 0x10064 | `tc__LoadSeg_reloc_patch` | regs | 0 | Head of the _LoadSeg (A9F0) patch: saves D0-D7/A0-A4, D7=segment number (word at 0x3C(SP), the LoadSeg argument), sets byte 0xA5E; GetResource('CODE',seg); if bit7 of first code... |
| 0x100B4 | `tc__init_globals` | regs | 0 | Initialises the A5 globals area: A1=(CurStackBase 0x908) .. |
| 0x10100 | `tc__relocate` | regs | 0 | Register args: A0=relocation resource handle (DREL/CREL), A2=base. |
| 0x10124 | `tc__save_handle_state` | regs | 0 | D3.b = HGetState(A3) (on 64K ROM, ROM85<0: D3.b = master-pointer high byte *A3). |
| 0x10136 | `tc__restore_handle_state` | regs | 0 | HSetState(A3, D3.b) (on 64K ROM: *A3 = D3.b). |
| 0x10148 | `tc__switch_word_list` | regs | 0 | Switch helper (jsr, never returns normally). |
| 0x1015C | `tc__switch_long_list` | regs | 0 | Like tc__switch_word_list but D0.l is compared against 32-bit values: table = word N-1, then N entries {word offset, long value}; target = entry address + offset; offset 0 -> in... |
| 0x10170 | `tc__switch_range` | regs | 0 | Dense switch helper. D0.w = selector. |
| 0x1018C | `tc_lmul` | pascal | 8 | 32x32->32 multiply (low 32 bits, sign-agnostic). |
| 0x101C2 | `tc_uldiv` | pascal | 8 | Unsigned 32/32 divide, D0 = dividend/divisor (4(SP)=dividend, 8(SP)=divisor), callee pops 8. |
| 0x101E2 | `tc_ulmod` | pascal | 8 | Unsigned 32-bit remainder, D0 = dividend % divisor, callee pops 8, preserves D1-D7. |
| 0x10204 | `tc_ldiv` | pascal | 8 | Signed 32/32 divide truncating toward zero, D0=quotient, callee pops 8, preserves D1-D7. |
| 0x10224 | `tc_lmod` | pascal | 8 | Signed remainder (sign of dividend, C99 semantics), callee pops 8, preserves D1-D7. |
| 0x10246 | `tc__ldivmod_regs` | regs | 0 | Register signed divmod: in D0=dividend, D1=divisor; out D0=quotient (trunc toward 0), D1=remainder (sign of dividend). |
| 0x10274 | `tc__uldivmod_regs` | regs | 0 | Register unsigned divmod: in D0=dividend, D1=divisor; out D0=quotient, D1=remainder. |
| 0x102DE | `tc__load_STRS` | regs | 0 | Loads resource type D6 ('STRS', the string-literal/A4 data) id 0 and, if larger than one resource, continuation ids (each 0x7FFE bytes) into one block: single part -> ReserveMem... |
| 0x103AE | `tc__install_trap_patches` | regs | 0 | NewPtr(0x18) (NewPtrSys on 64K ROM) stored at 0x68(A5); patches _LoadSeg (A9F0) with stub {jsr tc__LoadSeg_reloc_patch; jmp oldLoadSeg} and _ExitToShell (A9F4) with {jsr 0x1004C... |
| 0x10404 | `tc__remove_trap_patches` | regs | 0 | Default 0x6C(A5) exit hook: restores original LoadSeg/ExitToShell addresses saved in the 0x68(A5) block and DisposePtr's it. |
| 0x563C4 | `tc_malloc` | c | 0 | THINK C malloc. size<15000: tc__malloc_small((short)size) from 15008-byte pools (block = 2-byte header ~blocksize + data, blocksize=(size+3)&~1). |
| 0x56400 | `tc_calloc` | c | 0 | calloc: size>8MB -> NULL; size rounded up to even; total=nmemb*size (tc_lmul); total>8MB -> NULL; total<15000: tc__malloc_small + memset 0; else NewPtrClear(total+2) with 0xFFFF... |
| 0x56480 | `tc_realloc` | c | 0 | realloc: size>8MB -> NULL; p==NULL -> malloc(size); size==0 -> free(p), NULL. |
| 0x56586 | `tc_free` | c | 0 | free: NULL ignored. header=~header at p-2: if it becomes 0 (was 0xFFFF, NewPtr block) DisposePtr(p-2), else the small block is now marked free (positive size); pools are never r... |
| 0x5659C | `tc__malloc_small` | c | 0 | Small-block allocator: need=(size+3)&~1. |
| 0x56650 | `tc_atexit` | c | 0 | atexit: table of 34 entries x 6 bytes {fn, word flag} at A5-0x1862, count A5-0x186A; if count>33 return -1. |
| 0x566A2 | `tc__atexit` | c | 0 | THINK C _atexit: like atexit but sets entry flag=1 so fn also runs when the app quits via ExitToShell; first time patches ExitToShell (A9F4) -> tc__ExitToShell_patch (original s... |
| 0x56728 | `tc__atexit_slot1` | c | 0 | Temporarily sets the atexit count to 1, calls tc_atexit(fn) (stores into fixed slot 1), restores count. |
| 0x5674C | `tc__atexit_slot0` | c | 0 | Temporarily sets the atexit count to 0, calls tc__atexit(fn) (fixed slot 0, runs also on ExitToShell), restores count. |
| 0x5676E | `tc_exit` | c | 0 | exit(status): status ignored; jsr through 0x6C(A5) hook (runs atexit handlers, restores traps) then ExitToShell. |
| 0x5677E | `tc__exit` | c | 0 | _exit: ExitToShell directly (patched ExitToShell still runs _atexit handlers). |
| 0x56788 | `tc__exit_hook_all` | c | 0 | Exit hook installed in 0x6C(A5) by atexit: tc__run_exit_handlers(1). |
| 0x56794 | `tc__ExitToShell_patch` | c | 0 | ExitToShell patch installed by _atexit: tc__run_exit_handlers(0) (which restores the real ExitToShell) then ExitToShell. |
| 0x567A0 | `tc__run_exit_handlers` | c | 0 | Runs exit handlers slots 33..0 with A5 = saved A5 (tc__get_saved_A5): each non-NULL slot is cleared and called if all!=0 or its flag word!=0. |
| 0x56834 | `tc__exit_nop` | c | 0 | Empty function (rts); placeholder exit hook. |
| 0x56836 | `tc__A5_storage_addr` | regs | 0 | Returns in A1 the address (0x56838) of a 4-byte A5 save slot embedded in the code segment (bsr trick). |
| 0x5683C | `tc__pop_A1` | regs | 0 | Internal: movea.l (SP)+,A1; rts (helper of tc__A5_storage_addr). |
| 0x56840 | `tc__save_A5` | regs | 0 | Stores A5 into the code-segment slot at 0x56838 (clobbers A1). |
| 0x56848 | `tc__get_saved_A5` | regs | 0 | D0 = A5 value saved at 0x56838 (clobbers A1). |
| 0x56850 | `tc_atoi` | c | 0 | atoi: sscanf(s, fmt@A5-0x178A (initialised-data string, presumably "%d"), &v); returns 32-bit v, or 0 if sscanf<=0. |
| 0x56878 | `tc_atol` | c | 0 | atol(s) = strtol(s, NULL, 10). Unused by game. |
| 0x5688E | `tc_strtol` | c | 0 | strtol via sscanf(s, fmt@A5-0x1786, base, &val, &n) (THINK '?' flag takes the base from the arg list, '%n' gives chars consumed). |
| 0x568D6 | `tc_strtoul` | c | 0 | strtoul, same scheme with fmt@A5-0x177E (unsigned conversion). |
| 0x5691E | `tc__filbuf` | c | 0 | getc slow path: fp->cnt(0x14)=0; tc__checkfp; if ungetc pushback active (byte1 bit7): restore saved buffer (cnt=saved 0x0C, pos+=cnt, ptr=buf) else tc__stdio_prep_buffer + proc(... |
| 0x56980 | `tc__flsbuf` | c | 0 | putc slow path: if not yet writing (flag bit3): checkfp, append mode -> pos=EOF(0x1C), prep buffer, line-buffered(0x20!=0) -> cnt=1, set bit3. |
| 0x56A24 | `tc__stdio_prep_buffer` | c | 0 | ptr=buf, cnt=bufsize; if refnum(0x06)>0 and bufsize>1: cnt -= pos % bufsize (align window); pos(0x18) += cnt. |
| 0x56A66 | `tc__stdio_flush_buffer` | c | 0 | pos -= cnt; if dirty (bit3): cnt=ptr-buf, ptr=buf, clear bit3, result=proc(fp,1) (write). |
| 0x56AB4 | `tc__stdio_reset_buffer` | c | 0 | ptr=buf+bufsize, cnt=0, clear pushback flag (byte1 bit7). |
| 0x56AEA | `tc_freopenc` | c | 0 | THINK C console: makes stream `to` a console stream (sharing from's window if from!=NULL, else creates a console window via tc__console_new_window; on first use initialises the ... |
| 0x56B56 | `tc_cgotoxy` | c | 0 | Console: cursor col=x-1, row=y-1 (1-based). |
| 0x56B8C | `tc_cgetxy` | c | 0 | Console: *x=col+1, *y=row+1. Unused. |
| 0x56BC6 | `tc_ccleos` | c | 0 | Console: clear to end of screen. |
| 0x56BF2 | `tc_ccleol` | c | 0 | Console: clear to end of line. Unused. |
| 0x56C18 | `tc_csettabs` | c | 0 | Console: tab width (clamped to 1..ncols). |
| 0x56C5A | `tc_csetmode` | c | 0 | Console: 0=C_RAW,1=C_CBREAK,2=C_NOECHO,3=C_ECHO (mode bits 7/6/5 of A5-0x16F6). |
| 0x56CBA | `tc_cinverse` | c | 0 | Console: on -> installs QDProcs with textProc tc__console_inverse_textProc (chars with bit7 drawn inverted); off -> removes. |
| 0x56D2C | `tc_cshow` | c | 0 | Console: SelectWindow if not front, ShowWindow. |
| 0x56D52 | `tc_chide` | c | 0 | Console: HideWindow. Unused. |
| 0x56D66 | `tc_cecho2file` | c | 0 | Console: echo output to file (fopen mode A5-0x16CC if append else A5-0x16CA). |
| 0x56DB6 | `tc_cecho2printer` | c | 0 | Console: echo output to a tmpfile printed on close. |
| 0x56DEA | `tc__console_init_std` | c | 0 | Lazily attaches stdin/stdout/stderr (flag bit7 of their byte0) to one console window via tc_freopenc; optional startup echo file (A5-0x1722). |
| 0x56E62 | `tc__console_InitToolbox` | c | 0 | MaxApplZone, 10x MoreMasters, InitGraf(new 0xCE-byte QD globals), InitFonts/Windows/TEInit/Dialogs/Menus, Apple/File/Edit menus, DrawMenuBar, FlushEvents, InitCursor; sets A5-0x... |
| 0x56F0C | `tc__console_poll_event` | c | 0 | Console event pump: feeds pending pasted text as keystrokes, else SystemTask/GetNextEvent/SystemEvent, handles menu bar (DA open, Quit -> exit(0), Edit -> SystemEdit) (console i... |
| 0x5702C | `tc__console_new_window` | c | 0 | Creates the console window (NewWindow, font/size from globals, TENew, rows/cols), installs fake DA driver for event routing; returns WindowPtr (console internal). |
| 0x571A6 | `tc__console_close_window` | c | 0 | Disposes console window/TE if no other stream uses it (console internal). |
| 0x57202 | `tc__console_end_echo` | c | 0 | Closes the echo file (A5-0x16FA), printing it first if echo-to-printer (console internal). |
| 0x57220 | `tc__console_select` | c | 0 | checkfp; flushes all other dirty streams on the same window; returns fp's WindowPtr (0x20) (console internal). |
| 0x5726C | `tc__stdio_exit_cleanup` | c | 0 | atexit slot 1: fflush dirty console streams; if console active and A5-0x1762 set, retitles window and waits for a keypress ('press return'); then fclose all 15 streams. |
| 0x57314 | `tc__console_proc` | c | 0 | Console stream proc: cmd 0 = read line (event loop until input), 1 = write buffer (tc__console_write), 2 = close; Cmd-. |
| 0x5744A | `tc__console_inverse_textProc` | pascal | 14 | QuickDraw textProc (Pascal, pops 14): draws runs with StdText; chars with bit7 set are drawn (char&0x7F) in TextMode 4 (inverted), then TextMode 0. |
| 0x574BC | `tc__console_clear_from_line` | c | 0 | Console internal: blank lines from `line` to bottom, col=0. |
| 0x574FE | `tc__console_clear_eol_impl` | c | 0 | Console internal: TEDelete from cursor to end of line. |
| 0x5753A | `tc__console_write` | c | 0 | Console internal: output n chars handling BEL(7) BS(8) TAB(9) LF(10) VT(11) FF(12) CR(13); checks event queue for Cmd-. |
| 0x57656 | `tc__console_put_text` | c | 0 | Console internal: puts printable run into TE text, wrapping lines. |
| 0x57710 | `tc__console_input_delete` | c | 0 | Console internal: delete chars from pending input line (TEDelete/TECopy) (low confidence). |
| 0x57792 | `tc__console_input_insert` | c | 0 | Console internal: insert typed char into input line (TEKey/Munger) (low confidence). |
| 0x57814 | `tc__console_fill_scratch` | c | 0 | Console internal: SetHandleSize scratch handle and fill (low confidence). |
| 0x5784A | `tc__console_te_insert_at` | c | 0 | Console internal: TEPaste scratch at offset (low confidence). |
| 0x5786E | `tc__console_te_activate` | c | 0 | Console internal: set selection/caret and TEActivate. |
| 0x578DE | `tc__console_te_deactivate` | c | 0 | Console internal: TEDeactivate; returns *TEHandle. |
| 0x578F8 | `tc__console_trim_text` | c | 0 | Console internal: drops scrolled-off lines from TE text (Munger, TECalText). |
| 0x579BC | `tc__console_resize_te` | c | 0 | Console internal: recompute TE rects after resize/zoom. |
| 0x57A1A | `tc__console_enter_port` | c | 0 | Console internal: save port, select console window, process pending update (BeginUpdate/TEUpdate). |
| 0x57A88 | `tc__console_leave_port` | c | 0 | Console internal: restore port/current window. |
| 0x57AB0 | `tc__console_set_window` | c | 0 | Console internal: switch current console window state (copies 0x2E-byte state record). |
| 0x57AFE | `tc__console_lock_text` | c | 0 | Console internal: HLock TE text, returns pointer. |
| 0x57B26 | `tc__console_unlock_text` | c | 0 | Console internal: HUnlock TE text. |
| 0x57B2E | `tc__console_newline` | c | 0 | Console internal: newline: echo line to file, scroll (ScrollRect), handle pause (GetOSEvent) . |
| 0x57C40 | `tc__console_install_driver` | c | 0 | Console internal: builds DA driver header handle (open/close=tc__console_driver_noop, control=tc__console_driver_control) and DrvrInstall; returns refnum used as windowKind. |
| 0x57CC4 | `tc__console_driver_noop` | regs | 0 | Console fake-driver open/close entry (rts). |
| 0x57CC8 | `tc__console_driver_control` | regs | 0 | Console fake-driver control routine: accEvent/accRun/accCut/Copy/Paste/Clear/accMenu dispatch (low confidence). |
| 0x57DE4 | `tc__console_idle` | c | 0 | Console internal: TEIdle and I-beam cursor when over window. |
| 0x57E24 | `tc__console_mousedown` | c | 0 | Console internal: mouse-down dispatch (zoom/grow/content). |
| 0x57EA0 | `tc__console_zoom` | c | 0 | Console internal: TrackBox/ZoomWindow. |
| 0x57ED8 | `tc__console_grow` | c | 0 | Console internal: GrowWindow/SizeWindow. |
| 0x57F1C | `tc__console_content_click` | c | 0 | Console internal: TEClick selection handling. |
| 0x57FC6 | `tc__console_edit_command` | c | 0 | Console internal: cut/copy/paste/interrupt/quit commands (low confidence). |
| 0x580CA | `tc__console_key` | c | 0 | Console internal: processes a typed key (line editing, RAW/CBREAK/ECHO modes, EOF). |
| 0x582FE | `tc__console_cut` | c | 0 | Console internal: cut (copy + delete selection of input). |
| 0x5834C | `tc__console_copy` | c | 0 | Console internal: TECopy, ZeroScrap, PutScrap('TEXT'). |
| 0x5838E | `tc__console_paste` | c | 0 | Console internal: GetScrap('TEXT') into handle A5-0x1706 fed as keystrokes. |
| 0x583F2 | `tc__console_print_echo` | c | 0 | Console internal: PrOpen; tc__console_print_file; PrClose. |
| 0x5841C | `tc__console_print_file` | c | 0 | Console internal: prints echo file via Printing Manager (PrJobDialog, PrOpenDoc, DrawText per line). |
| 0x585B4 | `tc_isalnum` | c | 0 | isalnum: returns (long)(signed char)__ctype[(unsigned char)c] & 0xD0 (ctype table at A5-0x165E, filled from DATA). |
| 0x585D4 | `tc_isalpha` | c | 0 | isalpha: returns (long)(signed char)__ctype[(unsigned char)c] & 0xC0 (ctype table at A5-0x165E, filled from DATA). |
| 0x585F4 | `tc_iscntrl` | c | 0 | iscntrl: returns (long)(signed char)__ctype[(unsigned char)c] & 0x03 (ctype table at A5-0x165E, filled from DATA). |
| 0x58612 | `tc_isdigit` | c | 0 | isdigit: returns (long)(signed char)__ctype[(unsigned char)c] & 0x10 (ctype table at A5-0x165E, filled from DATA). |
| 0x58630 | `tc_isgraph` | c | 0 | isgraph: returns (long)(signed char)__ctype[(unsigned char)c] & 0xD8 (ctype table at A5-0x165E, filled from DATA). |
| 0x58650 | `tc_isupper` | c | 0 | isupper: returns (long)(signed char)__ctype[(unsigned char)c] & 0x80 (ctype table at A5-0x165E, filled from DATA). |
| 0x58670 | `tc_isprint` | c | 0 | isprint: returns (long)(signed char)__ctype[(unsigned char)c] & 0xDC (ctype table at A5-0x165E, filled from DATA). |
| 0x58690 | `tc_ispunct` | c | 0 | ispunct: returns (long)(signed char)__ctype[(unsigned char)c] & 0x08 (ctype table at A5-0x165E, filled from DATA). |
| 0x586AE | `tc_isspace` | c | 0 | isspace: returns (long)(signed char)__ctype[(unsigned char)c] & 0x06 (ctype table at A5-0x165E, filled from DATA). |
| 0x586CC | `tc_islower` | c | 0 | islower: returns (long)(signed char)__ctype[(unsigned char)c] & 0x40 (ctype table at A5-0x165E, filled from DATA). |
| 0x586EA | `tc_isxdigit` | c | 0 | isxdigit: returns (long)(signed char)__ctype[(unsigned char)c] & 0x30 (ctype table at A5-0x165E, filled from DATA). |
| 0x58708 | `tc_tolower` | c | 0 | Returns c^0x20 if __ctype[(uchar)c]&0x80 else c (whole 32-bit c returned). |
| 0x5872E | `tc_toupper` | c | 0 | Returns c^0x20 if __ctype[(uchar)c]&0x40 else c. |
| 0x58756 | `tc_remove` | c | 0 | PBDelete(name as Pascal str, vRefNum 0); error -> errno=OSErr, -1; else 0. |
| 0x58790 | `tc_rename` | c | 0 | PBRename; -1/errno on error. Unused. |
| 0x587DC | `tc_tmpfile` | c | 0 | tmpnam into local buffer, fopen(name, mode@A5-0x1536), sets temp flag (byte0 bit0, deleted on close). |
| 0x5881E | `tc_tmpnam` | c | 0 | tmpnam(buf or static A5-0x1556) via tc__tmpnam_impl. |
| 0x58844 | `tc__tmpnam_impl` | c | 0 | Builds Pascal name with sprintf(buf+1, fmt@A5-0x152C, prefix, ++*counter) (counter wraps 9999->1) until PBGetFInfo returns fnfErr; converts to C string in place; returns buf or ... |
| 0x588CA | `tc_fopen` | c | 0 | fopen: tc__find_free_file then freopen(name,mode,fp). |
| 0x588E4 | `tc_freopen` | c | 0 | Parses mode: 'r' (perm 1 fsRdPerm, flags 0), 'w' (perm 2, flags 3=create/trunc), 'a' (perm 2, flags 9=create/append); 'b' -> flags/0x10 (binary), '+' -> perm 3; other -> errno 2... |
| 0x58966 | `tc__fopen_impl` | c | 0 | fclose(fp); PBCreate if oflags&1 (success clears trunc bit; dupFNErr ok unless oflags&4); PBOpen (error -> errno, delete if just created, NULL); refnum->0x06; trunc -> PBSetEOF(... |
| 0x58A8A | `tc__set_file_type` | c | 0 | If file created: PBSetFInfo type = (binary? global A5-0x1524 : 'TEXT'), creator = A5-0x1520. |
| 0x58AD2 | `tc__close_all_files` | c | 0 | fclose all 15 FILE slots (A5-0x151C, 0x28 bytes each). |
| 0x58AF8 | `tc__file_proc` | c | 0 | Disk file stream proc. cmd 0: PBRead(cnt bytes at absolute pos-cnt) into buffer, eofErr with data -> ok, no data -> EOF bit5, -1; text mode converts CR->LF. |
| 0x58C58 | `tc__file_close` | c | 0 | PBClose; temp files (bit0) are deleted (PBDelete / PBHDelete for HFS); PBFlushVol. |
| 0x58D2C | `tc__replace_char` | c | 0 | Replaces every byte `from` with `to` in buf[0..n) (uses memchr). |
| 0x58D70 | `tc_fseek` | c | 0 | Only for refnum>0 else errno=19,-1. |
| 0x58DE0 | `tc_fsetpos` | c | 0 | fseek(fp,*pos,SEEK_SET). Unused. |
| 0x58DF8 | `tc_rewind` | c | 0 | fseek(fp,0,0); clears error bit4. |
| 0x58E14 | `tc_ftell` | c | 0 | refnum>0 ? pos-cnt : (errno=19,-1). |
| 0x58E3C | `tc_fgetpos` | c | 0 | *pos = pos-cnt; 0 (or errno 19,-1). |
| 0x58E6C | `tc_fwrite` | c | 0 | fwrite: total=size*n (0 -> 0); large aligned unbuffered-style writes go straight through proc(1); otherwise copies via buffer / putc. |
| 0x58FC2 | `tc_fgets` | c | 0 | fgets: n<1 -> NULL; reads up to n-1 chars, stops after '\n' (kept); EOF with nothing read -> NULL; NUL-terminates; returns s. |
| 0x59030 | `tc_gets` | c | 0 | gets from stdin (A5-0x151C), newline not stored. |
| 0x5908E | `tc_fgetc` | c | 0 | fgetc: cnt-- ? *ptr++ : tc__filbuf(fp). |
| 0x590C2 | `tc_getc` | c | 0 | Function version of getc (same as fgetc). |
| 0x5911C | `tc__find_free_file` | c | 0 | Returns first of the 12 non-std FILE slots (A5-0x14A4..) with refnum 0, else errno=24 and NULL. |
| 0x5914C | `tc_fprintf` | c | 0 | fprintf -> tc_vfprintf(fp, fmt, &args). |
| 0x59164 | `tc_printf` | c | 0 | printf -> tc_vprintf. Unused. |
| 0x59178 | `tc_vprintf` | c | 0 | tc_vfprintf(stdout=A5-0x14F4, fmt, va). |
| 0x59190 | `tc_fscanf` | c | 0 | fscanf -> tc_vfscanf. Unused. |
| 0x591A8 | `tc_scanf` | c | 0 | scanf -> tc_vscanf. Unused. |
| 0x591BC | `tc_vscanf` | c | 0 | tc_vfscanf(stdin=A5-0x151C, fmt, va). |
| 0x591D4 | `tc_setbuf` | c | 0 | setvbuf(fp, buf, buf?_IOFBF(0):_IONBF(2), 512). |
| 0x591FA | `tc_setvbuf` | c | 0 | mode 0=_IOFBF,1=_IOLBF,2=_IONBF (buffer = 1 byte at fp+4); size 0 with mode<2 -> -1; buf NULL -> malloc(size) (flag bit2); frees previous malloc'd buffer; sets buf/size, reset b... |
| 0x59280 | `tc_fflush` | c | 0 | fflush(fp) -> flush_buffer; fflush(NULL) flushes all writing streams, -1 if any failed. |
| 0x592D4 | `tc_fclose` | c | 0 | Clears bit7; if refnum!=0: flush, proc(fp,2), free malloc'd buffer, memset(fp,0,0x28). |
| 0x59332 | `tc_memcpy` | c | 0 | Forward byte-by-byte copy of n bytes (n==0 ok, overlapping regions copied forward), returns dst. |
| 0x5934A | `tc_memmove` | c | 0 | Overlap-safe byte copy: if src>dst forward else backward; returns dst. |
| 0x59372 | `tc_memcmp` | c | 0 | Unsigned byte compare; returns exactly 0, 1 (a>b) or -1 (a<b) as 32-bit long. |
| 0x59392 | `tc_memchr` | c | 0 | Finds first byte == (char)c (low byte of the 32-bit arg at 0xB(SP)) in n bytes; returns pointer or NULL. |
| 0x593B0 | `tc_memset` | c | 0 | Fills n bytes with (char)c (c is a 32-bit int, low byte used), returns s. |
| 0x593C8 | `tc_vfprintf` | c | 0 | THINK C printf engine. Flags - + space # 0; width/precision digits or '*' (32-bit int arg; negative '*' width -> left-justify); modifiers h (d/i: sign-extend low 16 bits; u/o/x/... |
| 0x59D1E | `tc__printf_num2dec` | c | 0 | SANE FX2DEC (Pack4 0x0B) of 10-byte extended x into Decimal record with decform{style,digits<=19}; retries with float style if result '?', strips trailing zeros (adjusting expon... |
| 0x59DE2 | `tc_qsort` | c | 0 | qsort: stores base/size/cmp in globals (A5-0x1286/-0x1282/-0x127E) and runs index quicksort tc__isort(n, cmp_idx, swap_idx). |
| 0x59E0C | `tc__qsort_cmp_idx` | c | 0 | Returns user_cmp(base+i*size, base+j*size). |
| 0x59E3E | `tc__qsort_swap_idx` | c | 0 | Swaps `size` bytes between element i and j (byte loop). |
| 0x59E7C | `tc__isort` | c | 0 | Stores index cmp/swap callbacks (A5-0x127A/-0x1276) and calls tc__quicksort(0, n). |
| 0x59E9A | `tc__quicksort` | c | 0 | Recursive index quicksort on [lo,hi), pivot = element lo; exact algorithm in tc_qsort semantics. |
| 0x59F50 | `tc_vfscanf` | c | 0 | THINK C scanf engine. Format whitespace skips input isspace (ctype&6); literal chars must match (mismatch -> ungetc & stop). |
| 0x5A89C | `tc__scanset_clear` | c | 0 | Clears 256-bit set at A5-0x126A; if arg!=0 sets {9..13,' '} and inverts (used by %s). |
| 0x5A8D0 | `tc__scanset_add` | c | 0 | Sets bit c in scan set. |
| 0x5A8F0 | `tc__scanset_invert` | regs | 0 | Inverts the 256-bit scan set (no args). |
| 0x5A906 | `tc__scanset_test` | c | 0 | Returns nonzero if bit c set (EOF -1 maps to byte -1/8...). |
| 0x5A92C | `tc__scanf_dec2num` | c | 0 | SANE FDEC2X (Pack4) Decimal -> dest in SANE format (0=extended,0x800=double,0x1000=single); overflow/underflow -> errno 34 and INF/0 result. |
| 0x5A9DA | `tc_signal` | c | 0 | sig must be 1..6 else errno 22,-1; swaps handler in table A5-0x124A, returns old. |
| 0x5AA1E | `tc_raise` | c | 0 | sig 1..6 else -1/errno 22; handler 1 (SIG_IGN) ignored; else reset to 0 and: 0 (SIG_DFL) -> _exit; else handler(sig). |
| 0x5AA8A | `tc__checkfp` | c | 0 | If byte0 bit7 (std stream not yet attached) -> tc__console_init_std; if refnum==0 makes it a dummy stream (1-byte buffer at fp+4, proc=nullproc). |
| 0x5AAC4 | `tc__c2pstr_copy` | c | 0 | Copies C string to Pascal string at dst (length = strlen & 0xFF); returns dst. |
| 0x5AAEC | `tc__sscanf_fp_init` | c | 0 | tc__string_fp_init(fp, str, strlen(str)); returns fp. |
| 0x5AB0E | `tc__sprintf_fp_init` | c | 0 | tc__string_fp_init(fp, buf, -1) (unbounded), sets writing bit3; returns fp. |
| 0x5AB30 | `tc__string_fp_init` | c | 0 | memset(fp,0,0x28); refnum=-1; ptr=p; cnt=cnt; proc=tc__stdio_nullproc. |
| 0x5AB68 | `tc__stdio_nullproc` | c | 0 | Stream proc for string FILEs: returns -1. |
| 0x5AB72 | `tc_sprintf` | c | 0 | sprintf -> tc_vsprintf(buf, fmt, &varargs). |
| 0x5AB8A | `tc_vsprintf` | c | 0 | Builds string FILE on stack (cnt=-1), n=tc_vfprintf; if n>=0 buf[n]=0; returns n. |
| 0x5ABC6 | `tc_sscanf` | c | 0 | sscanf -> tc_vsscanf(str, fmt, &varargs). |
| 0x5ABDE | `tc_vsscanf` | c | 0 | String FILE with cnt=strlen(str) then tc_vfscanf. |
| 0x5AC02 | `tc__fp_proc_fill` | c | 0 | return fp->proc(fp, 0). |
| 0x5AC1A | `tc__fp_proc_flush` | c | 0 | return fp->proc(fp, 1). |
| 0x5AC34 | `tc__fp_proc_close` | c | 0 | return fp->proc(fp, 2). |
| 0x5AC4E | `tc_strcpy` | c | 0 | Copies including NUL, returns dst. |
| 0x5AC5E | `tc_strcat` | c | 0 | Appends src to dst, returns dst. |
| 0x5AC74 | `tc_strcmp` | c | 0 | Unsigned char compare; returns exactly -1/0/1 (32-bit). |
| 0x5AC92 | `tc_strchr` | c | 0 | First occurrence of (char)c (32-bit arg, low byte), c==0 finds terminator; NULL if none. |
| 0x5ACAA | `tc_strcspn` | c | 0 | Standard strcspn. Unused. |
| 0x5ACC4 | `tc_strpbrk` | c | 0 | Standard strpbrk. Called by configFindFile. |
| 0x5ACE0 | `tc_strrchr` | c | 0 | Last occurrence of (char)c (terminator included). |
| 0x5ACF6 | `tc_strspn` | c | 0 | Standard strspn. Unused. |
| 0x5AD12 | `tc_strstr` | c | 0 | Naive strstr; empty sub -> s. Unused. |
| 0x5AD30 | `tc_strtok` | c | 0 | Standard strtok, saved pointer in A5-0x1206. |
| 0x5AD6E | `tc_strlen` | c | 0 | Standard strlen (32-bit result). |
| 0x5AD7C | `tc_ungetc` | c | 0 | ungetc: c==-1 -> -1. checkfp. If pushback mode active (byte1 bit7): cnt!=0 -> -1. |
| 0x5AE24 | `tc__new_alloc` | c | 0 | Allocator used by operator new: identical algorithm to tc_malloc but with its OWN pool list (globals A5-0x195C/-0x1958/-0x1960/-0x1964). |
| 0x5AE60 | `tc__new_free` | c | 0 | Same as tc_free (used by operator delete). |
| 0x5AE76 | `tc__new_alloc_small` | c | 0 | Small-block allocator for operator new pools (copy of tc__malloc_small with separate globals). |
| 0x5AF40 | `tc__lmul_stk` | c | 0 | D0=4(SP), D1=8(SP) then falls into tc_lmul_d0d1. |
| 0x5AF48 | `tc_lmul_d0d1` | regs | 0 | Register multiply: D0 = D0*D1 (low 32 bits). |
| 0x5AF68 | `tc__ldiv_stk` | c | 0 | D1=4(SP) (divisor), D0=8(SP) (dividend), falls into tc_ldiv_d0d1. |
| 0x5AF70 | `tc_ldiv_d0d1` | regs | 0 | Register signed divide: in D0=dividend, D1=divisor; out D0=quotient (trunc toward 0); D1 = quotient too (moved). |
| 0x5AF78 | `tc__uldiv_stk` | c | 0 | D1=4(SP), D0=8(SP), falls into tc_uldiv_d0d1. |
| 0x5AF80 | `tc_uldiv_d0d1` | regs | 0 | Register unsigned divide: in D0=dividend, D1=divisor; out D0=D1=quotient. |
| 0x5AF88 | `tc__lmod_stk` | c | 0 | D1=4(SP), D0=8(SP), falls into tc_lmod_d0d1. |
| 0x5AF90 | `tc_lmod_d0d1` | regs | 0 | Register signed divmod: in D0=dividend, D1=divisor; out D0=remainder (sign of dividend), D1=quotient (trunc toward 0). |
| 0x5AFB6 | `tc__ulmod_stk` | c | 0 | D1=4(SP), D0=8(SP), falls into tc_ulmod_d0d1. |
| 0x5AFBE | `tc_ulmod_d0d1` | regs | 0 | Register unsigned divmod: in D0=dividend, D1=divisor; out D0=remainder, D1=quotient. |
| 0x5B00E | `tc_operator_delete` | pascal | 4 | C++ operator delete(void*): tc__new_free(p). |
| 0x5B03E | `tc_operator_new` | pascal | 4 | C++ operator new(size_t): size 0 -> 2; p=tc__new_alloc(size); while p==NULL and new_handler (A5-0x1954) != NULL: call it and retry. |
| 0x5B092 | `tc__run_STI` | regs | 0 | C++ static init: if '_STD' 0 exists -> stored at A5-0x194C and tc__run_STD registered with atexit; '_STI' 0: MoveHHi/HLock, calls A5+offset for each long in it, ReleaseResource. |
| 0x5B0F4 | `tc__run_STD` | regs | 0 | Runs C++ static destructors: calls A5+offset for each long in the '_STD' resource, then releases it. |
| 0x5B134 | `tc_CtoPstr` | c | 0 | In-place C->Pascal string: shifts bytes right by one, s[0]=strlen&0xFF; returns s. |
| 0x5B150 | `tc_PtoCstr` | c | 0 | In-place Pascal->C string: shifts left by one, NUL-terminates; returns s. |
| 0x5B16A | `tc_SysEnvirons` | pascal | 6 | FUNCTION SysEnvirons(versReq; VAR theWorld): OSErr. |
| 0x5B2D4 | `tc_Gestalt` | pascal | 8 | FUNCTION Gestalt(selector; VAR response): OSErr. |
| 0x5B4E2 | `tc_GetPtrSize` | pascal | 4 | FUNCTION GetPtrSize(p): Size; negative (error) -> 0. |
| 0x5B4F0 | `tc_GetHandleSize` | pascal | 4 | FUNCTION GetHandleSize(h): Size; negative (error) -> 0. |
| 0x5B4FE | `tc_NGetTrapAddress` | pascal | 4 | FUNCTION NGetTrapAddress(trapNum; tTyp: TrapType): LongInt. |
| 0x5B512 | `tc_PBOpen` | pascal | 6 | FUNCTION PBOpen(pb; async: Boolean): OSErr. |
| 0x5B524 | `tc_PBClose` | pascal | 6 | FUNCTION PBClose(pb; async: Boolean): OSErr. |
| 0x5B536 | `tc_PBControl` | pascal | 6 | FUNCTION PBControl(pb; async: Boolean): OSErr. |
| 0x5B548 | `tc_PBGetVInfo` | pascal | 6 | FUNCTION PBGetVInfo(pb; async: Boolean): OSErr. |
| 0x5B55A | `tc_PBGetEOF` | pascal | 6 | FUNCTION PBGetEOF(pb; async: Boolean): OSErr. |
| 0x5B56C | `tc_PBSetEOF` | pascal | 6 | FUNCTION PBSetEOF(pb; async: Boolean): OSErr. |
| 0x5B57E | `tc_PBSetFPos` | pascal | 6 | FUNCTION PBSetFPos(pb; async: Boolean): OSErr. |
| 0x5B7AC | `tc_PBOpenWD` | pascal | 6 | FUNCTION PBOpenWD(pb; async: Boolean): OSErr. |
| 0x5B7C2 | `tc_PBCloseWD` | pascal | 6 | FUNCTION PBCloseWD(pb; async: Boolean): OSErr. |
| 0x5B7D8 | `tc_PBHGetVol` | pascal | 6 | FUNCTION PBHGetVol(pb; async: Boolean): OSErr. |
| 0x5B7EA | `tc_PBGetFCBInfo` | pascal | 6 | FUNCTION PBGetFCBInfo(pb; async: Boolean): OSErr. |
| 0x5B800 | `tc_PBGetCatInfo` | pascal | 6 | FUNCTION PBGetCatInfo(pb; async: Boolean): OSErr. |
| 0x5B816 | `tc_PBSetCatInfo` | pascal | 6 | FUNCTION PBSetCatInfo(pb; async: Boolean): OSErr. |
| 0x5B82C | `tc_PBHGetVInfo` | pascal | 6 | FUNCTION PBHGetVInfo(pb; async: Boolean): OSErr. |
| 0x5B83E | `tc_PBHOpen` | pascal | 6 | FUNCTION PBHOpen(pb; async: Boolean): OSErr. |
| 0x5B850 | `tc_PBHDelete` | pascal | 6 | FUNCTION PBHDelete(pb; async: Boolean): OSErr. |
| 0x5B862 | `tc_PBHRename` | pascal | 6 | FUNCTION PBHRename(pb; async: Boolean): OSErr. |
| 0x5B874 | `tc_PBHRstFLock` | pascal | 6 | FUNCTION PBHRstFLock(pb; async: Boolean): OSErr. |
| 0x5B886 | `tc_PBHSetFLock` | pascal | 6 | FUNCTION PBHSetFLock(pb; async: Boolean): OSErr. |
| 0x5B590 | `tc_FSOpen` | pascal | 10 | FUNCTION FSOpen(fileName: Str255; vRefNum; VAR refNum): OSErr. |
| 0x5B5D0 | `tc_FSClose` | pascal | 2 | FUNCTION FSClose(refNum): OSErr via PBClose. |
| 0x5B5EA | `tc_FSRead` | pascal | 10 | FUNCTION FSRead(refNum; VAR count; buffPtr): OSErr. |
| 0x5B5EE | `tc_FSWrite` | pascal | 10 | FUNCTION FSWrite(refNum; VAR count; buffPtr): OSErr. |
| 0x5B632 | `tc_GetVInfo` | pascal | 14 | FUNCTION GetVInfo(drvNum; volName; VAR vRefNum; VAR freeBytes): OSErr. |
| 0x5B674 | `tc_GetFInfo` | pascal | 10 | FUNCTION GetFInfo(fileName; vRefNum; VAR fndrInfo): OSErr. |
| 0x5B6AA | `tc_GetVol` | pascal | 8 | FUNCTION GetVol(volName; VAR vRefNum): OSErr via PBGetVol. |
| 0x5B6CC | `tc_SetVol` | pascal | 6 | FUNCTION SetVol(volName; vRefNum): OSErr via PBSetVol. |
| 0x5B6EC | `tc_Create` | pascal | 14 | FUNCTION Create(fileName; vRefNum; creator; fileType): OSErr. |
| 0x5B72A | `tc_SetFInfo` | pascal | 10 | FUNCTION SetFInfo(fileName; vRefNum; fndrInfo): OSErr. |
| 0x5B764 | `tc_GetEOF` | pascal | 6 | FUNCTION GetEOF(refNum; VAR logEOF): OSErr via PBGetEOF; *logEOF=ioMisc (written even on error). |
| 0x5B786 | `tc_SetFPos` | pascal | 8 | FUNCTION SetFPos(refNum; posMode; posOff): OSErr via PBSetFPos. |
| 0x5B898 | `tc_SetSoundVol` | pascal | 2 | PROCEDURE SetSoundVol(level): PBControl to Sound Driver refNum -4, csCode 2, csParam=level (PB on stack, other fields uninitialised). |
| 0x5B8BA | `tc_CountAppFiles` | pascal | 8 | PROCEDURE CountAppFiles(VAR message, VAR count): *count=0; if AppParmHandle (0xAEC) size>0: *message=word0, *count=word1. |
| 0x5B8DE | `tc_GetAppFiles` | pascal | 6 | PROCEDURE GetAppFiles(index; VAR theFile): clears fType; if 1<=index<=count copies AppParm entries 1..index into theFile (last = index-th) with entry size 8+((len+2)&~1). |
| 0x5B924 | `tc_PurgeSpace` | pascal | 8 | PROCEDURE PurgeSpace(VAR total, VAR contig): *total=D0, *contig=A0 from trap A162. |
| 0x5B938 | `tc_FlushInstructionCache` | pascal | 0 | HWPriv (A198) selector 1. |
| 0x5B940 | `tc_FlushDataCache` | pascal | 0 | HWPriv (A198) selector 3. |
| 0x5B94A | `tc__Pack2_dispatch` | regs | 0 | Inserts selector D2.w under the return address and executes _Pack2 (A9E9). |
| 0x5B952 | `tc_DIBadMount` | pascal | 8 | FUNCTION DIBadMount(where; evtMessage): INTEGER = Pack2 selector 0 (Disk Initialization dialog). |
| 0x5B958 | `tc_NumToString` | pascal | 8 | PROCEDURE NumToString(theNum; VAR theString: Str255): Pack7 selector 0 (A0=str, D0=num). |
| 0x5B96A | `tc_StripAddress` | pascal | 4 | FUNCTION StripAddress(addr): Ptr; trap A055 if available (ROM85 bit6 clear), else addr & Lo3Bytes (0x31A). |
| 0x5B984 | `tc_HandToHand` | pascal | 4 | FUNCTION HandToHand(VAR theHndl): OSErr; *theHndl = copy (A0 written even on error). |
| 0x5B99A | `tc_GetIndString` | pascal | 8 | PROCEDURE GetIndString(VAR theString; strListID; index): theString[0]=0; GetResource('STR#',id); if found and 1<=index<=count copies the index-th Pascal string (len+1 bytes). Re... |
