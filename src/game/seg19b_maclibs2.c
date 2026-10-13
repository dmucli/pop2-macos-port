/* CODE 19 "Mac Libs 2", second part (Ghidra 0x58a8a-0x5b12e): the THINK C / Symantec
 * runtime library (see docs/THINKC_LIB.md for the whole library map).
 *
 *  - stdio on disk files and strings: the 0x28-byte FILE records (15 slots at A5-0x151C:
 *    stdin, stdout, stderr, then 12 free), the disk stream proc (PBRead/PBWrite at absolute
 *    positions, CR<->LF conversion in text mode), fseek/ftell/fwrite/fgets/getc/ungetc,
 *    setvbuf/fflush/fclose;
 *  - the printf engine (tc_vfprintf) and the scanf engine (tc_vfscanf) with their SANE
 *    helpers, sprintf/sscanf on string FILEs (cnt = -1: unbounded);
 *  - mem* / str* (C library; `int` arguments are 32-bit longs, results are 32-bit), qsort
 *    (index quicksort through two callbacks), signal/raise;
 *  - operator new/delete with their own pool allocator, the Symantec long multiply/divide
 *    register helpers (D0/D1) and their stack shims, and the C++ static constructor and
 *    destructor runners (_STI/_STD resources).
 *
 * In this library `int` is 32 bits. Ported from the disassembly (the decompiler output was
 * used as a guide only); frame locals that the original keeps in memory live at their
 * original A6 offsets (ENTRY_LINK's a6), registers become C locals.
 *
 * Functions that write their own argument slots (tc_fwrite: ptr, tc_fgets: n, tc_vfprintf:
 * va, tc_vfscanf: fmt and va, tc_operator_new: size) do it through the ENTRY frame
 * (a6 + 8...): when entered from 68k code these are the caller's real slots; when called
 * from C they are the argument bytes ENTRY reserves. The slot is first (re)written with
 * the argument value so both cases behave like the original.
 *
 * Variadic functions (tc_sprintf, tc_fprintf, ...) find their variable arguments above the
 * fixed ones in the same frame: they must be called through their 68k entry
 * (call_ptr(JT_ADDR(FN_tc_sprintf), ...)), as the other units do. */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* ---- globals (A5 offsets) ---- */
#define gErrno          GA(-0x155E)    /* long: errno */
#define gFileTypeBin    GA(-0x1524)    /* long: file type of binary files created by fopen */
#define gFileCreator    GA(-0x1520)    /* long: creator of files created by fopen */
#define gFiles          GA(-0x151C)    /* FILE[15]: stdin, stdout, stderr, 12 free slots */
#define gStdout         GA(-0x14F4)    /* &gFiles[1] */
#define gFreeFiles      GA(-0x14A4)    /* &gFiles[3] */
#define gCtype          GA(-0x165E)    /* byte[256]: ctype flags (0x10 digit, 0x06 space) */
#define gPrintfSpec     GA(-0x12C4)    /* 14 bytes: initial printf conversion spec */
#define gHexUpper       GA(-0x12B6)    /* "0123456789ABCDEF" */
#define gHexLower       GA(-0x12A4)    /* "0123456789abcdef" */
#define gInfText        GA(-0x1292)    /* 7 bytes: replacement digits for huge %f */
#define gQsortMid       GA(-0x128A)    /* long: quicksort scan index */
#define gQsortBase      GA(-0x1286)    /* long: qsort base */
#define gQsortSize      GA(-0x1282)    /* long: qsort element size */
#define gQsortCmp       GA(-0x127E)    /* long: qsort user compare function */
#define gSortCmp        GA(-0x127A)    /* long: index compare callback of tc__isort */
#define gSortSwap       GA(-0x1276)    /* long: index swap callback of tc__isort */
#define gScanfSpec      GA(-0x1272)    /* 8 bytes: initial scanf conversion spec */
#define gScanSet        GA(-0x126A)    /* byte[32]: 256-bit scan set */
#define gSignals        GA(-0x124A)    /* long[7]: signal handlers (1..6) */
#define gStrtokNext     GA(-0x1206)    /* long: strtok saved pointer */
#define gNewPool        GA(-0x1964)    /* long: operator new pool list */
#define gNewFirstPool   GA(-0x1960)    /* long: first pool (its link is patched) */
#define gNewCurPool     GA(-0x195C)    /* long: current pool */
#define gNewRover       GA(-0x1958)    /* long: next block to look at */
#define gNewHandler     GA(-0x1954)    /* long: new_handler */
#define gSTIHandle      GA(-0x1950)    /* long: '_STI' 0 resource */
#define gSTDHandle      GA(-0x194C)    /* long: '_STD' 0 resource */

#define FCBSPtr         0x34E          /* low memory: file control blocks */

/* FILE record (0x28 bytes) */
#define F_FLAGS   0x00   /* bit7 std stream not attached, bit6 binary, bit5 EOF, bit4 error,
                            bit3 writing, bit2 malloc'd buffer, bit1 append, bit0 temp file */
#define F_FLAGS2  0x01   /* bit7 ungetc pushback active */
#define F_REFNUM  0x06
#define F_BUF     0x08
#define F_BUFSIZE 0x0C
#define F_PTR     0x10
#define F_CNT     0x14
#define F_POS     0x18
#define F_EOF     0x1C
#define F_CONSOLE 0x20
#define F_PROC    0x24

#define CTYPE(c)  R8(gCtype + (uint8_t)(c))

static void bset8(maddr a, int bit) { W8(a, R8(a) | (1u << bit)); }
static void bclr8(maddr a, int bit) { W8(a, R8(a) & ~(1u << bit)); }
static int  btst8(maddr a, int bit) { return (R8(a) >> bit) & 1; }
static void add32(maddr a, uint32_t v) { W32(a, R32(a) + v); }

/* SANE (_Pack4 = FP68K): operand addresses are pushed in order, then the opword. */
static void sane(uint16_t op, int n, uint32_t a, uint32_t b, uint32_t c) {
    tb_begin();
    if (n > 0) push32(a);
    if (n > 1) push32(b);
    if (n > 2) push32(c);
    push16(op);
    tb_trap(0xA9EB);
    tb_end();
}

/* _NewPtr (A11E): pointer in A0, result code in D0 */
static maddr new_ptr(uint32_t size, int16_t *err) {
    tb_begin();
    cpu_set(R_D0, size);
    tb_trap(0xA11E);
    maddr a0 = cpu_get(R_A0);
    *err = (int16_t)cpu_get(R_D0);
    tb_end();
    return a0;
}

/* 00058a8a: give a file created by fopen its Finder type ('TEXT' or the binary type)
 * and creator. (name: C/Pascal name pointer, oflags: bit 4 = binary) */
uint32_t tc__set_file_type(uint32_t name, uint32_t oflags) {
    ENTRY_LINK(FN_tc__set_file_type, 0x50, 0);
    maddr pb = a6 - 0x50;                  /* HFileParam */
    W32(pb + 0x12, name);                  /* ioNamePtr */
    W16(pb + 0x16, 0);                     /* ioVRefNum */
    W8(pb + 0x1A, 0);                      /* ioFVersNum */
    W16(pb + 0x1C, 0);                     /* ioFDirIndex */
    uint32_t d0 = _PBGetFInfo(pb);
    if ((int16_t)d0 != 0) return (uint32_t)(int32_t)(int16_t)d0;
    if (R8(a6 + 0xF) & 0x10) W32(pb + 0x20, R32(gFileTypeBin));
    else W32(pb + 0x20, 0x54455854);      /* 'TEXT' */
    W32(pb + 0x24, R32(gFileCreator));
    return _PBSetFInfo(pb);
}

/* 00058ad2: fclose all 15 FILE slots (atexit handler) */
uint32_t tc__close_all_files(void) {
    ENTRY_NOLINK(FN_tc__close_all_files, 8);
    maddr fp = gFiles;
    for (uint32_t n = 15; n != 0; n--) {
        tc_fclose(fp);
        fp += 0x28;
    }
    return 0;
}

/* 00058af8: stream proc of disk files. cmd 0 = fill the buffer (PBRead at pos - cnt),
 * 1 = write the buffer (PBWrite, extending the EOF first), 2 = close. 0 / -1 (errno) */
uint32_t tc__file_proc(uint32_t fp, uint32_t cmd) {
    ENTRY_LINK(FN_tc__file_proc, 0x32, 4);
    maddr pb = a6 - 0x32;                  /* ParamBlockRec (ioParam) */
#define PB_RESULT  (pb + 0x10)
#define PB_REFNUM  (pb + 0x18)
#define PB_BUFFER  (pb + 0x20)
#define PB_REQCNT  (pb + 0x24)
#define PB_ACTCNT  (pb + 0x28)
#define PB_POSMODE (pb + 0x2C)
#define PB_POSOFF  (pb + 0x2E)
#define PB_MISC    (pb + 0x1C)
    W16(PB_REFNUM, R16(fp + F_REFNUM));
    int32_t c = (int32_t)cmd;
    if (c == 0) {
        W32(PB_BUFFER, R32(fp + F_PTR));
        W32(PB_REQCNT, R32(fp + F_CNT));
        W16(PB_POSMODE, R16(fp + F_REFNUM) > 0 ? 1 : 0);
        W32(PB_POSOFF, R32(fp + F_POS) - R32(fp + F_CNT));
        _PBRead(pb);
        if (R16(PB_RESULT) == -39) {       /* eofErr */
            W32(fp + F_POS, R32(PB_POSOFF));
            uint32_t got = R32(PB_ACTCNT);
            W32(fp + F_CNT, got);
            if (got == 0) {
                bset8(fp + F_FLAGS, 5);
                return 0xFFFFFFFF;
            }
            W16(PB_RESULT, 0);
        }
        if (R16(PB_RESULT) == 0 && !btst8(fp + F_FLAGS, 6))
            tc__replace_char(R32(fp + F_PTR), R32(fp + F_CNT), 0xD, 0xA);
    } else if (c < 0) {
        /* nothing: the result field is tested as it is */
    } else if (c == 2) {
        W16(PB_RESULT, tc__file_close(fp));
    } else if (c > 2) {
    } else {                               /* 1: write */
        W32(PB_BUFFER, R32(fp + F_PTR));
        W32(PB_REQCNT, R32(fp + F_CNT));
        W16(PB_POSMODE, R16(fp + F_REFNUM) > 0 ? 1 : 0);
        uint32_t pos = R32(fp + F_POS) - R32(fp + F_CNT);
        W32(PB_POSOFF, pos);
        int ok = 1;
        if (pos > R32(fp + F_EOF)) {
            W32(PB_MISC, R32(PB_POSOFF));
            if ((int16_t)_PBSetEOF(pb) != 0) ok = 0;
        }
        if (ok) {
            if (!btst8(fp + F_FLAGS, 6))
                tc__replace_char(R32(fp + F_PTR), R32(fp + F_CNT), 0xA, 0xD);
            _PBWrite(pb);
            if (R16(PB_RESULT) == 0 && R32(PB_POSOFF) > R32(fp + F_EOF))
                W32(fp + F_EOF, R32(PB_POSOFF));
        }
    }
    if (R16(PB_RESULT) == 0) return 0;
    if (2 > c) {
        uint32_t cnt = R32(fp + F_CNT);
        W32(fp + F_POS, R32(fp + F_POS) - cnt);
        W32(fp + F_CNT, 0);
    }
    bset8(fp + F_FLAGS, 4);
    W32(gErrno, (uint32_t)(int32_t)R16(PB_RESULT));
    return 0xFFFFFFFF;
#undef PB_RESULT
#undef PB_REFNUM
#undef PB_BUFFER
#undef PB_REQCNT
#undef PB_ACTCNT
#undef PB_POSMODE
#undef PB_POSOFF
#undef PB_MISC
}

/* 00058c58: close a disk file: PBClose, delete it if it is a temporary file (found by
 * name in its FCB on HFS, by refnum on MFS), PBFlushVol. Returns the OSErr. */
uint32_t tc__file_close(uint32_t fp) {
    ENTRY_LINK(FN_tc__file_close, 0x150, 20);
    maddr pb = a6 - 0x50;
    maddr name = a6 - 0x150;
    maddr fcb = R32(FCBSPtr) + (uint32_t)(int32_t)R16(fp + F_REFNUM);
    maddr vcb = R32(fcb + 0x14);
    uint8_t kind = 0;                      /* 1 = delete (MFS), 2 = HDelete (HFS) */
    W16(pb + 0x16, R16(vcb + 0x4E));       /* ioVRefNum = vcbVRefNum */
    if (btst8(fp + F_FLAGS, 0)) {
        W32(pb + 0x12, name);              /* ioNamePtr */
        W8(pb + 0x1A, 0);
        if (R16(vcb + 8) == 0x4244) {      /* HFS */
            W32(pb + 0x30, R32(fcb + 0x3A));   /* ioDirID = fcbDirID */
            maddr cname = fcb + 0x3E;
            tc_memcpy(name, cname, (uint32_t)R8(cname) + 1);
            kind = 2;
        } else if (R16(vcb + 8) == (int16_t)0xD2D7) {   /* MFS: find the file by index */
            W16(pb + 0x1C, 1);
            for (;;) {
                if ((int16_t)_PBGetFInfo(pb) != 0) break;
                if (R16(pb + 0x18) == R16(fp + F_REFNUM)) { kind = 1; break; }
                W16(pb + 0x1C, R16(pb + 0x1C) + 1);
            }
        }
    }
    W16(pb + 0x18, R16(fp + F_REFNUM));
    if ((int16_t)_PBClose(pb) == 0) {
        if (kind == 1) _PBDelete(pb);
        else if (kind == 2) _PBHDelete(pb);
        W32(pb + 0x12, 0);
        _PBFlushVol(pb);
    }
    return (uint32_t)(int32_t)R16(pb + 0x10);
}

/* 00058d2c: replace every byte `from` by `to` in buf[0..n) */
uint32_t tc__replace_char(uint32_t buf, uint32_t n, uint32_t from, uint32_t to) {
    ENTRY_LINK(FN_tc__replace_char, 0, 20);
    maddr a4 = buf;
    uint32_t d7 = n;
    while (d7 != 0) {
        maddr a3 = tc_memchr(a4, from, d7);
        if (a3 == 0) break;
        W8(a3, to);
        a3++;
        d7 -= a3 - a4;
        a4 = a3;
    }
    return 0;
}

/* 00058d70: fseek (disk files only, else errno 19) */
uint32_t tc_fseek(uint32_t fp, uint32_t off, uint32_t whence) {
    ENTRY_LINK(FN_tc_fseek, 0, 16);
    int32_t d7 = (int32_t)off;
    uint32_t d5 = R32(fp + F_POS) - R32(fp + F_CNT);
    if (R16(fp + F_REFNUM) <= 0) {
        W32(gErrno, 0x13);
        return 0xFFFFFFFF;
    }
    tc_fflush(fp);
    if (whence == 1) d7 += (int32_t)d5;
    else if (whence == 2) d7 += (int32_t)R32(fp + F_EOF);
    else if (whence != 0) d7 = -1;
    if (d7 < 0) {
        W32(gErrno, 0x16);
        return 0xFFFFFFFF;
    }
    W32(fp + F_POS, d7);
    bclr8(fp + F_FLAGS, 5);
    return 0;
}

/* 00058de0: fsetpos(fp, &pos) = fseek(fp, pos, SEEK_SET) */
uint32_t tc_fsetpos(uint32_t fp, uint32_t pos) {
    ENTRY_LINK(FN_tc_fsetpos, 0, 0);
    return tc_fseek(fp, R32(pos), 0);
}

/* 00058df8: rewind: fseek(fp, 0, 0) and clear the error bit */
uint32_t tc_rewind(uint32_t fp) {
    ENTRY_LINK(FN_tc_rewind, 0, 0);
    uint32_t r = tc_fseek(fp, 0, 0);
    bclr8(fp + F_FLAGS, 4);
    return r;
}

/* 00058e14: ftell */
uint32_t tc_ftell(uint32_t fp) {
    if (R16(fp + F_REFNUM) <= 0) {
        W32(gErrno, 0x13);
        return 0xFFFFFFFF;
    }
    return R32(fp + F_POS) - R32(fp + F_CNT);
}

/* 00058e3c: fgetpos */
uint32_t tc_fgetpos(uint32_t fp, uint32_t pos) {
    if (R16(fp + F_REFNUM) <= 0) {
        W32(gErrno, 0x13);
        return 0xFFFFFFFF;
    }
    W32(pos, R32(fp + F_POS) - R32(fp + F_CNT));
    return 0;
}

/* 00058e6c: fwrite. Large aligned writes to binary disk files go straight to the stream
 * proc; otherwise through the buffer (memcpy) or putc. Returns the items written. */
uint32_t tc_fwrite(uint32_t ptr, uint32_t size, uint32_t n, uint32_t fp) {
    ENTRY_LINK(FN_tc_fwrite, 0, 24);
    W32(a6 + 8, ptr);                      /* own slot: advanced below */
    uint32_t d7 = size, d6 = n, d4 = 0;
    uint32_t d5 = d6 * d7;
    if (d5 == 0) return 0;
    tc__checkfp(fp);
    if (btst8(fp + F_FLAGS, 1) && !btst8(fp + F_FLAGS, 3))
        W32(fp + F_POS, R32(fp + F_EOF));
    for (;;) {
        if (R16(fp + F_REFNUM) > 0 && btst8(fp + F_FLAGS, 6) && R32(fp + F_CNT) == 0 &&
            d5 >= R32(fp + F_BUFSIZE)) {
            int direct = R32(fp + F_BUFSIZE) == 1;
            if (!direct) {
                uint32_t bs = R32(fp + F_BUFSIZE);
                if (bs == 0) port_divide_by_zero(__func__);
                direct = R32(fp + F_POS) % bs == 0;
            }
            if (direct) {
                d4 = d5;
                if (R32(fp + F_BUFSIZE) > 1) d4 -= d4 % R32(fp + F_BUFSIZE);
                W32(fp + F_PTR, R32(a6 + 8));
                W32(fp + F_CNT, d4);
                add32(fp + F_POS, d4);
                uint32_t d3 = tc__fp_proc_flush(fp);
                tc__stdio_reset_buffer(fp);
                if (d3 != 0) break;
                goto next;
            }
        }
        if (!btst8(fp + F_FLAGS, 3) || R32(fp + F_CONSOLE) != 0) {
            /* one character, putc */
            uint32_t old = R32(fp + F_CNT);
            W32(fp + F_CNT, old - 1);
            uint32_t d1;
            if (old - 1 != 0 && old != 0) {
                maddr p = R32(fp + F_PTR);
                W32(fp + F_PTR, p + 1);
                uint8_t ch = R8(R32(a6 + 8));
                W8(p, ch);
                d1 = ch;
            } else {
                d1 = tc__flsbuf((uint32_t)(int32_t)(int8_t)R8(R32(a6 + 8)), fp);
            }
            if (d1 + 1 == 0) break;
            d4 = 1;
        } else {
            /* copy into the buffer */
            d4 = R32(fp + F_CNT);
            if (d5 < d4) d4 = d5;
            tc_memcpy(R32(fp + F_PTR), R32(a6 + 8), d4);
            add32(fp + F_PTR, d4);
            W32(fp + F_CNT, R32(fp + F_CNT) - d4);
            if (R32(fp + F_CNT) == 0 && tc__stdio_flush_buffer(fp) != 0) break;
        }
    next:
        add32(a6 + 8, d4);
        d5 -= d4;
        if (d5 == 0) break;
    }
    if (d5 != 0) d6 -= (d5 + d7 - 1) / d7;
    return d6;
}

/* inline getc of the library: cnt-- ? *ptr++ : _filbuf(fp) */
static uint32_t lib_getc(maddr fp) {
    uint32_t old = R32(fp + F_CNT);
    W32(fp + F_CNT, old - 1);
    if (old != 0) {
        maddr p = R32(fp + F_PTR);
        W32(fp + F_PTR, p + 1);
        return R8(p);
    }
    return tc__filbuf(fp);
}

/* inline putc of the library (the out-of-buffer test is cnt <= 1, unsigned) */
static uint32_t lib_putc(uint32_t c, maddr fp) {
    uint32_t old = R32(fp + F_CNT);
    W32(fp + F_CNT, old - 1);
    if (old - 1 != 0 && old != 0) {
        maddr p = R32(fp + F_PTR);
        W32(fp + F_PTR, p + 1);
        W8(p, c);
        return c & 0xFF;
    }
    return tc__flsbuf(c, fp);
}

/* 00058fc2: fgets: up to n-1 chars, the newline is kept; NULL on EOF with nothing read */
uint32_t tc_fgets(uint32_t s, uint32_t n, uint32_t fp) {
    ENTRY_LINK(FN_tc_fgets, 0, 16);
    W32(a6 + 0xC, n);                      /* own slot: counted down */
    maddr a2 = s;
    if (1 > (int32_t)n) return 0;
    for (;;) {
        W32(a6 + 0xC, R32(a6 + 0xC) - 1);
        if (R32(a6 + 0xC) == 0) break;
        int32_t d7 = (int32_t)lib_getc(fp);
        if (d7 < 0) {
            if (btst8(fp + F_FLAGS, 5) && a2 != s) break;
            return 0;
        }
        W8(a2++, d7);
        if (d7 == 10) break;
    }
    W8(a2, 0);
    return s;
}

/* 00059030: gets (stdin), the newline is not stored */
uint32_t tc_gets(uint32_t s) {
    ENTRY_LINK(FN_tc_gets, 0, 16);
    maddr fp = gFiles;
    maddr a2 = s;
    for (;;) {
        int32_t d7 = (int32_t)lib_getc(fp);
        if (d7 < 0) {
            if (btst8(fp + F_FLAGS, 5) && a2 != s) break;
            return 0;
        }
        if (d7 == 10) break;
        W8(a2++, d7);
    }
    W8(a2, 0);
    return s;
}

/* 0005908e: fgetc */
uint32_t tc_fgetc(uint32_t fp) {
    ENTRY_LINK(FN_tc_fgetc, 0, 0);
    return lib_getc(fp);
}

/* 000590c2: getc (function version) */
uint32_t tc_getc(uint32_t fp) {
    ENTRY_LINK(FN_tc_getc, 0, 0);
    return lib_getc(fp);
}

/* 0005911c: first free (refnum 0) FILE among the 12 non-std slots, else errno 24, NULL */
uint32_t tc__find_free_file(void) {
    maddr fp = gFreeFiles;
    for (uint32_t n = 12; n != 0; n--) {
        if (R16(fp + F_REFNUM) == 0) return fp;
        fp += 0x28;
    }
    W32(gErrno, 0x18);
    return 0;
}

/* 0005914c: fprintf(fp, fmt, ...) */
uint32_t tc_fprintf(uint32_t fp, uint32_t fmt) {
    ENTRY_LINK(FN_tc_fprintf, 0, 0);
    return tc_vfprintf(fp, fmt, a6 + 0x10);
}

/* 00059164: printf(fmt, ...) */
uint32_t tc_printf(uint32_t fmt) {
    ENTRY_LINK(FN_tc_printf, 0, 0);
    return tc_vprintf(fmt, a6 + 0xC);
}

/* 00059178: vprintf = vfprintf(stdout, ...) */
uint32_t tc_vprintf(uint32_t fmt, uint32_t va) {
    ENTRY_LINK(FN_tc_vprintf, 0, 0);
    return tc_vfprintf(gStdout, fmt, va);
}

/* 00059190: fscanf(fp, fmt, ...) */
uint32_t tc_fscanf(uint32_t fp, uint32_t fmt) {
    ENTRY_LINK(FN_tc_fscanf, 0, 0);
    return tc_vfscanf(fp, fmt, a6 + 0x10);
}

/* 000591a8: scanf(fmt, ...) */
uint32_t tc_scanf(uint32_t fmt) {
    ENTRY_LINK(FN_tc_scanf, 0, 0);
    return tc_vscanf(fmt, a6 + 0xC);
}

/* 000591bc: vscanf = vfscanf(stdin, ...) */
uint32_t tc_vscanf(uint32_t fmt, uint32_t va) {
    ENTRY_LINK(FN_tc_vscanf, 0, 0);
    return tc_vfscanf(gFiles, fmt, va);
}

/* 000591d4: setbuf = setvbuf(fp, buf, buf ? _IOFBF : _IONBF, 512) */
uint32_t tc_setbuf(uint32_t fp, uint32_t buf) {
    ENTRY_LINK(FN_tc_setbuf, 0, 0);
    return tc_setvbuf(fp, buf, buf != 0 ? 0 : 2, 0x200);
}

/* 000591fa: setvbuf (mode 0 full, 1 line, 2 none = the 1-byte buffer at fp+4) */
uint32_t tc_setvbuf(uint32_t fp, uint32_t buf, uint32_t mode, uint32_t size) {
    ENTRY_LINK(FN_tc_setvbuf, 0, 16);
    uint32_t d7 = size;
    maddr a3 = buf;
    int d6 = 0;
    tc__checkfp(fp);
    int32_t m = (int32_t)mode;
    if (m < 0 || m > 2) return 0xFFFFFFFF;
    if (m == 2) {
        a3 = fp + 4;
        d7 = 1;
    } else {
        if (d7 == 0) return 0xFFFFFFFF;
        if (a3 == 0) {
            d6 = 1;
            a3 = tc_malloc(d7);
            if (a3 == 0) return 0xFFFFFFFF;
        }
    }
    if (btst8(fp + F_FLAGS, 2)) {
        tc_free(R32(fp + F_BUF));
        bclr8(fp + F_FLAGS, 2);
    }
    if (d6) bset8(fp + F_FLAGS, 2);
    W32(fp + F_BUF, a3);
    W32(fp + F_BUFSIZE, d7);
    tc__stdio_reset_buffer(fp);
    return 0;
}

/* 00059280: fflush(fp); fflush(NULL) flushes every writing stream (-1 if one failed) */
uint32_t tc_fflush(uint32_t fp) {
    ENTRY_LINK(FN_tc_fflush, 0, 12);
    if (fp != 0) {
        tc__checkfp(fp);
        return tc__stdio_flush_buffer(fp);
    }
    uint32_t d7 = 0;
    maddr a4 = gFiles;
    for (uint32_t n = 15; n != 0; n--) {
        if (btst8(a4 + F_FLAGS, 3) && tc__stdio_flush_buffer(a4) != 0) d7 = 0xFFFFFFFF;
        a4 += 0x28;
    }
    return d7;
}

/* 000592d4: fclose: flush, close through the proc, free a malloc'd buffer, clear the FILE */
uint32_t tc_fclose(uint32_t fp) {
    ENTRY_LINK(FN_tc_fclose, 0, 8);
    uint32_t d7 = 0;
    bclr8(fp + F_FLAGS, 7);
    if (R16(fp + F_REFNUM) != 0) {
        if (tc__stdio_flush_buffer(fp) != 0) d7 = 0xFFFFFFFF;
        if (tc__fp_proc_close(fp) != 0) d7 = 0xFFFFFFFF;
        if (btst8(fp + F_FLAGS, 2)) tc_free(R32(fp + F_BUF));
        tc_memset(fp, 0, 0x28);
    }
    return d7;
}

/* the whole range [a, a+n) maps linearly into emulated memory */
static int lin(maddr a, uint32_t n) { return (a & ADDR_MASK) + (uint64_t)n <= MEM_SIZE; }

/* 00059332: memcpy: forward byte copy (overlap copies forward too), returns dst */
uint32_t tc_memcpy(uint32_t dst, uint32_t src, uint32_t n) {
    if (n == 0) return dst;
    uint32_t d = dst & ADDR_MASK, s = src & ADDR_MASK;
    if (lin(dst, n) && lin(src, n) && (d <= s || d >= s + n)) {
        mcopy(d, s, n);
    } else {
        maddr a0 = dst, a1 = src;
        do { W8(a0++, R8(a1++)); } while (--n != 0);
    }
    return dst;
}

/* 0005934a: memmove: forward if src > dst, else backward; returns dst */
uint32_t tc_memmove(uint32_t dst, uint32_t src, uint32_t n) {
    if (n == 0) return dst;
    if (lin(dst, n) && lin(src, n)) {
        mcopy(dst & ADDR_MASK, src & ADDR_MASK, n);
    } else if (src > dst) {
        maddr a0 = dst, a1 = src;
        do { W8(a0++, R8(a1++)); } while (--n != 0);
    } else {
        maddr a0 = dst + n, a1 = src + n;
        do { W8(--a0, R8(--a1)); } while (--n != 0);
    }
    return dst;
}

/* 00059372: memcmp: unsigned bytes, returns exactly -1, 0 or 1 */
uint32_t tc_memcmp(uint32_t a, uint32_t b, uint32_t n) {
    for (; n != 0; n--, a++, b++) {
        uint8_t x = R8(a), y = R8(b);
        if (x != y) return y < x ? 1 : 0xFFFFFFFF;
    }
    return 0;
}

/* 00059392: memchr (c: low byte of the 32-bit argument) */
uint32_t tc_memchr(uint32_t s, uint32_t c, uint32_t n) {
    uint8_t ch = (uint8_t)c;
    for (; n != 0; n--) {
        if (R8(s++) == ch) return s - 1;
    }
    return 0;
}

/* 000593b0: memset (c: low byte of the 32-bit argument), returns s */
uint32_t tc_memset(uint32_t s, uint32_t c, uint32_t n) {
    if (n == 0) return s;
    if (lin(s, n)) mset(s & ADDR_MASK, (uint8_t)c, n);
    else { maddr a0 = s; do { W8(a0++, c); } while (--n != 0); }
    return s;
}

/* ---- printf engine ----
 * Conversion spec at A6-0x220 (14 bytes copied from gPrintfSpec):
 *   -0x220 flags: bit7 '-', bit6 '+', bit5 '#', bit4 '0', bit3 precision given,
 *                 bit2 'h', bit1 'l', bit0 'L'
 *   -0x21c sign/prefix char, -0x21b exponent char (0 = fixed notation),
 *   -0x21a width (long), -0x216 precision (long, low word at -0x214)
 * -0x212 memchr result, -0x20e hex digit table; floats: -0x240 Decimal record
 * {sgn, exp word, sig Str20} from num2dec, -0x226/-0x224/-0x222 words (lowest digit index,
 * position of the point, highest digit index); the value itself is copied to -0xa.
 * Digits are built downwards from A6-0xa (A2). */
#define VF_FLAGS  (a6 - 0x220)
#define VF_SIGN   (a6 - 0x21C)
#define VF_EXPCH  (a6 - 0x21B)
#define VF_WIDTH  (a6 - 0x21A)
#define VF_PREC   (a6 - 0x216)
#define VF_PRECW  (a6 - 0x214)
#define VF_CHR    (a6 - 0x212)
#define VF_HEX    (a6 - 0x20E)
#define VF_LOW    (a6 - 0x226)
#define VF_POINT  (a6 - 0x224)
#define VF_HIGH   (a6 - 0x222)
#define VF_DEC    (a6 - 0x240)
#define VF_EXT    (a6 - 0xA)

/* 000593c8: vfprintf(fp, fmt, va). Returns the characters written, -1 on output error;
 * an unknown conversion stops and returns the count so far. */
uint32_t tc_vfprintf(uint32_t fp, uint32_t fmt, uint32_t va) {
    ENTRY_LINK(FN_tc_vfprintf, 0x240, 32);
    W32(a6 + 0x10, va);                    /* own slot: advanced as arguments are used */
    maddr a3 = fmt, a2 = 0;
    uint32_t d4 = 0, d3 = 0;
    int32_t d5 = 0, d6 = 0;
    int32_t d7 = (int8_t)R8(a3);
    maddr a0;
    goto test;

next:
    a3++;
    d7 = (int8_t)R8(a3);
test:
    if (d7 == 0) return d4;
    if (d7 != '%') goto outchar;
    mcopy(VF_FLAGS, gPrintfSpec, 14);
    /* flags */
    for (;;) {
        a3++;
        d7 = (int8_t)R8(a3);
        if (d7 == '-') bset8(VF_FLAGS, 7);
        else if (d7 == '+') bset8(VF_FLAGS, 6);
        else if (d7 == ' ') W8(VF_SIGN, 0x20);
        else if (d7 == '#') bset8(VF_FLAGS, 5);
        else if (d7 == '0') bset8(VF_FLAGS, 4);
        else break;
    }
    /* width */
    if (d7 == '*') {
        a0 = R32(a6 + 0x10); add32(a6 + 0x10, 4);
        int32_t w = (int32_t)R32(a0);
        W32(VF_WIDTH, w);
        if (w < 0) {
            bset8(VF_FLAGS, 7);
            W32(VF_WIDTH, -(int32_t)R32(VF_WIDTH));
        }
        a3++;
        d7 = (int8_t)R8(a3);
    } else {
        while (d7 >= 0x30 && d7 <= 0x39) {
            W32(VF_WIDTH, R32(VF_WIDTH) * 10 - 0x30 + (uint32_t)d7);
            a3++;
            d7 = (int8_t)R8(a3);
        }
    }
    /* precision */
    if (d7 == '.') {
        a3++;
        d7 = (int8_t)R8(a3);
        if (d7 == '*') {
            a0 = R32(a6 + 0x10); add32(a6 + 0x10, 4);
            W32(VF_PREC, R32(a0));
            a3++;
            d7 = (int8_t)R8(a3);
        } else {
            while (d7 >= 0x30 && d7 <= 0x39) {
                W32(VF_PREC, R32(VF_PREC) * 10 - 0x30 + (uint32_t)d7);
                a3++;
                d7 = (int8_t)R8(a3);
            }
        }
        if ((int32_t)R32(VF_PREC) >= 0) bset8(VF_FLAGS, 3);
    }
    a2 = a6 - 0xA;
    if (btst8(VF_FLAGS, 7)) bclr8(VF_FLAGS, 4);

conv:
    switch (d7) {
    case 'h': bset8(VF_FLAGS, 2); a3++; d7 = (int8_t)R8(a3); goto conv;
    case 'l': bset8(VF_FLAGS, 1); a3++; d7 = (int8_t)R8(a3); goto conv;
    case 'L': bset8(VF_FLAGS, 0); a3++; d7 = (int8_t)R8(a3); goto conv;

    case 'd': case 'i':
        a0 = R32(a6 + 0x10); add32(a6 + 0x10, 4);
        d3 = R32(a0);
        if (btst8(VF_FLAGS, 2)) d3 = (uint32_t)(int32_t)(int16_t)d3;
        if ((int32_t)d3 < 0) {
            d3 = -d3;
            W8(VF_SIGN, '-');
        } else if (btst8(VF_FLAGS, 6)) {
            W8(VF_SIGN, '+');
        }
        goto decimal;
    case 'u':
        a0 = R32(a6 + 0x10); add32(a6 + 0x10, 4);
        d3 = R32(a0);
        if (btst8(VF_FLAGS, 2)) d3 &= 0xFFFF;
        W8(VF_SIGN, 0);
    decimal:
        if (!btst8(VF_FLAGS, 3)) {
            if (btst8(VF_FLAGS, 4)) {
                W32(VF_PREC, R32(VF_WIDTH));
                if (R8(VF_SIGN) != 0) W32(VF_PREC, R32(VF_PREC) - 1);
            }
            if (1 > (int32_t)R32(VF_PREC)) W32(VF_PREC, 1);
        }
        d6 = 0;
        while (d3 != 0) {
            W8(--a2, (uint8_t)(d3 % 10) + 0x30);
            d3 = d3 / 10;
            d6++;
        }
        while (d6 < (int32_t)R32(VF_PREC)) { W8(--a2, '0'); d6++; }
        if (R8(VF_SIGN) != 0) { W8(--a2, R8(VF_SIGN)); d6++; }
        goto field;

    case 'o':
        a0 = R32(a6 + 0x10); add32(a6 + 0x10, 4);
        d3 = R32(a0);
        if (btst8(VF_FLAGS, 2)) d3 &= 0xFFFF;
        if (!btst8(VF_FLAGS, 3)) {
            if (btst8(VF_FLAGS, 4)) W32(VF_PREC, R32(VF_WIDTH));
            if (1 > (int32_t)R32(VF_PREC)) W32(VF_PREC, 1);
        }
        d6 = 0;
        while (d3 != 0) {
            W8(--a2, (uint8_t)(d3 & 7) + 0x30);
            d3 >>= 3;
            d6++;
        }
        if (btst8(VF_FLAGS, 5) && d6 != 0 && R8(a2) != '0') { W8(--a2, '0'); d6++; }
        while (d6 < (int32_t)R32(VF_PREC)) { W8(--a2, '0'); d6++; }
        goto field;

    case 'p':
        bset8(VF_FLAGS, 1);
        bset8(VF_FLAGS, 3);
        W32(VF_PREC, 8);
        W32(VF_HEX, gHexUpper);
        goto hex;
    case 'X':
        W32(VF_HEX, gHexUpper);
        goto hex;
    case 'x':
        W32(VF_HEX, gHexLower);
    hex:
        a0 = R32(a6 + 0x10); add32(a6 + 0x10, 4);
        d3 = R32(a0);
        if (btst8(VF_FLAGS, 2)) d3 &= 0xFFFF;
        if (!btst8(VF_FLAGS, 3)) {
            if (btst8(VF_FLAGS, 4)) {
                W32(VF_PREC, R32(VF_WIDTH));
                if (btst8(VF_FLAGS, 5)) W32(VF_PREC, R32(VF_PREC) - 2);
            }
            if (1 > (int32_t)R32(VF_PREC)) W32(VF_PREC, 1);
        }
        d6 = 0;
        while (d3 != 0) {
            W8(--a2, R8((d3 & 0xF) + R32(VF_HEX)));
            d3 >>= 4;
            d6++;
        }
        while (d6 < (int32_t)R32(VF_PREC)) { W8(--a2, '0'); d6++; }
        if (btst8(VF_FLAGS, 5)) {
            W8(--a2, d7);
            W8(--a2, '0');
            d6 += 2;
        }
        goto field;

    case 'f': {
        a0 = R32(a6 + 0x10); add32(a6 + 0x10, 10);
        mcopy(VF_EXT, a0, 10);
        if (!btst8(VF_FLAGS, 3)) W32(VF_PREC, 6);
        tc__printf_num2dec(1, R32(VF_PREC), VF_DEC, R32(VF_EXT), R32(VF_EXT + 4), R16(VF_EXT + 8));
        int16_t d0 = (int16_t)((int8_t)R8(VF_DEC + 4) + R16(VF_DEC + 2));
        W16(VF_POINT, d0);
        W16(VF_LOW, d0);
        if (d0 > 1) W16(VF_LOW, 1);
        W16(VF_HIGH, R16(VF_POINT) + R16(VF_PRECW));
        if ((int32_t)R16(VF_HIGH) - (int32_t)R16(VF_LOW) > 0x1FC)
            tc_memcpy(VF_DEC + 4, gInfText, 7);
        goto floatout;
    }
    case 'e': case 'E': {
        a0 = R32(a6 + 0x10); add32(a6 + 0x10, 10);
        mcopy(VF_EXT, a0, 10);
        if (!btst8(VF_FLAGS, 3)) W32(VF_PREC, 6);
        W8(VF_EXPCH, d7);
        int16_t d0 = (int16_t)(1 + R16(VF_PRECW));
        W16(VF_HIGH, d0);
        tc__printf_num2dec(0, (uint32_t)(int32_t)d0, VF_DEC, R32(VF_EXT), R32(VF_EXT + 4), R16(VF_EXT + 8));
        W16(VF_POINT, 1);
        W16(VF_LOW, 1);
        W16(VF_DEC + 2, R16(VF_DEC + 2) + (int16_t)((int8_t)R8(VF_DEC + 4) - 1));
        goto floatout;
    }
    case 'g': case 'G': {
        a0 = R32(a6 + 0x10); add32(a6 + 0x10, 10);
        mcopy(VF_EXT, a0, 10);
        if (!btst8(VF_FLAGS, 3)) W32(VF_PREC, 6);
        else if (R32(VF_PREC) == 0) W32(VF_PREC, 1);
        W8(VF_EXPCH, d7 - 2);
        int16_t d0 = R16(VF_PRECW);
        W16(VF_HIGH, d0);
        tc__printf_num2dec(0, (uint32_t)(int32_t)d0, VF_DEC, R32(VF_EXT), R32(VF_EXT + 4), R16(VF_EXT + 8));
        W16(VF_POINT, 1);
        W16(VF_LOW, 1);
        W16(VF_DEC + 2, R16(VF_DEC + 2) + (int16_t)((int8_t)R8(VF_DEC + 4) - 1));
        if (R16(VF_DEC + 2) >= -4 && (int32_t)R16(VF_DEC + 2) < (int32_t)R32(VF_PREC)) {
            W8(VF_EXPCH, 0);
            W16(VF_POINT, R16(VF_POINT) + R16(VF_DEC + 2));
            if (R16(VF_POINT) < 1) W16(VF_LOW, R16(VF_POINT));
        }
        if (!btst8(VF_FLAGS, 5)) {
            int16_t n = (int8_t)R8(VF_DEC + 4);
            if (n < R16(VF_HIGH)) {
                W16(VF_HIGH, n);
                if (n < R16(VF_POINT)) W16(VF_HIGH, R16(VF_POINT));
            }
        }
        goto floatout;
    }

    case 'c':
        a0 = R32(a6 + 0x10); add32(a6 + 0x10, 4);
        W8(--a2, R8(a0 + 3));
        d6 = 1;
        goto field;

    case 's':
        a0 = R32(a6 + 0x10); add32(a6 + 0x10, 4);
        a2 = R32(a0);
        if (btst8(VF_FLAGS, 5)) {          /* %#s: Pascal string */
            d6 = R8(a2);
            a2++;
            if (btst8(VF_FLAGS, 3) && d6 > (int32_t)R32(VF_PREC)) d6 = (int32_t)R32(VF_PREC);
        } else if (!btst8(VF_FLAGS, 3)) {
            d6 = (int32_t)tc_strlen(a2);
        } else {
            uint32_t r = tc_memchr(a2, 0, R32(VF_PREC));
            W32(VF_CHR, r);
            if (r != 0) d6 = (int32_t)(R32(VF_CHR) - a2);
            else d6 = (int32_t)R32(VF_PREC);
        }
        goto field;

    case 'n':
        a0 = R32(a6 + 0x10); add32(a6 + 0x10, 4);
        a2 = R32(a0);
        if (btst8(VF_FLAGS, 2)) W16(a2, d4);
        else W32(a2, d4);
        goto next;

    default:
        if (d7 != '%') return d4;
        goto outchar;
    }

floatout:
    if ((int8_t)R8(VF_DEC + 5) > '9') {    /* INF / NAN: print the text */
        W8(VF_EXPCH, 0);
        W16(VF_POINT, 0);
        W16(VF_LOW, 1);
        W16(VF_HIGH, (int8_t)R8(VF_DEC + 4));
    }
    d6 = 0;
    if (R8(VF_EXPCH) != 0) {
        d3 = R16(VF_DEC + 2) < 0 ? (uint32_t)(-(int32_t)R16(VF_DEC + 2)) : (uint32_t)(int32_t)R16(VF_DEC + 2);
        while (d3 != 0) {
            W8(--a2, (uint8_t)(d3 % 10) + 0x30);
            d3 = d3 / 10;
            d6++;
        }
        while (2 > d6) { W8(--a2, '0'); d6++; }
        W8(--a2, R16(VF_DEC + 2) < 0 ? '-' : '+');
        W8(--a2, R8(VF_EXPCH));
        d6 += 2;
    }
    d5 = R16(VF_HIGH);
    if ((int32_t)R16(VF_POINT) == d5 && !btst8(VF_FLAGS, 5)) W16(VF_POINT, R16(VF_POINT) + 1);
    do {
        if ((int32_t)R16(VF_POINT) == d5) { W8(--a2, '.'); d6++; }
        int32_t ch;
        if (d5 > 0 && (int32_t)(int8_t)R8(VF_DEC + 4) >= d5) ch = (int8_t)R8(a6 + (uint32_t)d5 - 0x23C);
        else ch = '0';
        W8(--a2, ch);
        d5--;
    } while ((int32_t)R16(VF_LOW) <= d5);
    d6 += (int32_t)R16(VF_HIGH) - d5;
    if (R8(VF_DEC) != 0) W8(VF_SIGN, '-');
    else if (btst8(VF_FLAGS, 6)) W8(VF_SIGN, '+');
    if (btst8(VF_FLAGS, 4)) {
        d5 = (int32_t)R32(VF_WIDTH);
        if (R8(VF_SIGN) != 0) d5--;
        while (d5 > d6) { W8(--a2, '0'); d6++; }
    }
    if (R8(VF_SIGN) != 0) { W8(--a2, R8(VF_SIGN)); d6++; }
    goto field;

outchar:
    if ((int32_t)lib_putc((uint32_t)d7, fp) < 0) return 0xFFFFFFFF;
    d4++;
    goto next;

field:
    /* right-justify: leading spaces */
    if (d6 < (int32_t)R32(VF_WIDTH) && !btst8(VF_FLAGS, 7)) {
        do {
            if ((int32_t)lib_putc(0x20, fp) < 0) return 0xFFFFFFFF;
            d4++;
            W32(VF_WIDTH, R32(VF_WIDTH) - 1);
        } while (d6 < (int32_t)R32(VF_WIDTH));
    }
    if ((uint32_t)d6 != tc_fwrite(a2, 1, (uint32_t)d6, fp)) return 0xFFFFFFFF;
    d4 += (uint32_t)d6;
    /* left-justify: trailing spaces */
    while (d6 < (int32_t)R32(VF_WIDTH)) {
        if ((int32_t)lib_putc(0x20, fp) < 0) return 0xFFFFFFFF;
        d4++;
        d6++;
    }
    goto next;
}
#undef VF_FLAGS
#undef VF_SIGN
#undef VF_EXPCH
#undef VF_WIDTH
#undef VF_PREC
#undef VF_PRECW
#undef VF_CHR
#undef VF_HEX
#undef VF_LOW
#undef VF_POINT
#undef VF_HIGH
#undef VF_DEC
#undef VF_EXT

/* 00059d1e: SANE num2dec for printf: style (0 float, 1 fixed), digits (max 19), Decimal
 * record, then the 10-byte extended value by value. Retries in float style when the
 * result is '?', strips trailing zeros (adjusting the exponent), INF/NAN texts. */
uint32_t tc__printf_num2dec(uint32_t style, uint32_t digits, uint32_t dec, uint32_t x0, uint32_t x4, int16_t x8) {
    ENTRY_LINK(FN_tc__printf_num2dec, 4, 16);
    W32(a6 + 0x14, x0);                    /* the value lives in the argument area */
    W32(a6 + 0x18, x4);
    W16(a6 + 0x1C, x8);
    maddr a4 = dec, a3 = a6 + 0x14;
    uint32_t d7 = digits;
    if (!(0x14 > d7)) d7 = 0x13;
    W8(a6 - 4, style);                     /* decform.style (low byte) */
    W16(a6 - 2, d7);                       /* decform.digits */
    sane(0x000B, 3, a6 - 4, a3, a4);       /* FX2DEC */
    if (R8(a4 + 5) == '?') {
        W8(a6 - 4, 0);
        W16(a6 - 2, 0x13);
        sane(0x000B, 3, a6 - 4, a3, a4);
    }
    int32_t d6 = (int8_t)R8(a4 + 4);
    while (1 < d6 && R8(a4 + 4 + (uint32_t)d6) == '0') {
        W16(a4 + 2, R16(a4 + 2) + 1);
        d6--;
    }
    W8(a4 + 4, d6);
    if (R8(a4 + 5) == '0') {
        W8(a4, 0);
        W16(a4 + 2, 0);
    } else if (R8(a4 + 5) == 'I') {
        W8(a4 + 4, 3);
        W8(a4 + 6, 'N');
        W8(a4 + 7, 'F');
    } else if (R8(a4 + 5) == 'N') {
        W8(a4 + 4, 5);
        W8(a4 + 6, 'A');
        W8(a4 + 7, 'N');
    }
    return 0;
}

/* 00059de2: qsort(base, n, size, cmp): index quicksort through cmp_idx/swap_idx */
uint32_t tc_qsort(uint32_t base, uint32_t n, uint32_t size, uint32_t cmp) {
    ENTRY_LINK(FN_tc_qsort, 0, 0);
    W32(gQsortBase, base);
    W32(gQsortSize, size);
    W32(gQsortCmp, cmp);
    tc__isort(n, JT_ADDR(FN_tc__qsort_cmp_idx), JT_ADDR(FN_tc__qsort_swap_idx));
    return 0;
}

/* 00059e0c: compare elements i and j with the user function */
uint32_t tc__qsort_cmp_idx(uint32_t i, uint32_t j) {
    ENTRY_LINK(FN_tc__qsort_cmp_idx, 0, 0);
    uint32_t pj = j * R32(gQsortSize) + R32(gQsortBase);
    uint32_t pi = i * R32(gQsortSize) + R32(gQsortBase);
    return call_ptr(R32(gQsortCmp), "ll", pi, pj);
}

/* 00059e3e: swap elements i and j byte by byte */
uint32_t tc__qsort_swap_idx(uint32_t i, uint32_t j) {
    uint32_t d7 = R32(gQsortSize);
    maddr a4 = i * d7 + R32(gQsortBase);
    maddr a3 = j * d7 + R32(gQsortBase);
    do {
        uint8_t t = R8(a4);
        W8(a4++, R8(a3));
        W8(a3++, t);
    } while (--d7 != 0);
    return 0;
}

/* 00059e7c: sort indices [0, n) with the given compare/swap callbacks */
uint32_t tc__isort(uint32_t n, uint32_t cmp, uint32_t swap) {
    ENTRY_LINK(FN_tc__isort, 0, 0);
    W32(gSortCmp, cmp);
    W32(gSortSwap, swap);
    tc__quicksort(0, n);
    return 0;
}

/* 00059e9a: quicksort of indices [lo, hi), pivot = lo, recursion on the smaller part */
uint32_t tc__quicksort(uint32_t lo, uint32_t hi) {
    ENTRY_LINK(FN_tc__quicksort, 0, 12);
    uint32_t d7 = lo, d6 = hi, d5;
    while (d6 - d7 > 1) {
        W32(gQsortMid, d7);
        d5 = d6;
        for (;;) {
            do {
                add32(gQsortMid, 1);
                if (d6 <= R32(gQsortMid)) break;
            } while ((int32_t)call_ptr(R32(gSortCmp), "ll", R32(gQsortMid), d7) < 0);
            do {
                d5--;
                if (d7 >= d5) break;
            } while ((int32_t)call_ptr(R32(gSortCmp), "ll", d5, d7) > 0);
            if (d5 <= R32(gQsortMid)) break;
            call_ptr(R32(gSortSwap), "ll", R32(gQsortMid), d5);
        }
        if (d7 == d5) {
            d7++;
            continue;
        }
        call_ptr(R32(gSortSwap), "ll", d7, d5);
        if (d6 - 1 - d5 > d5 - d7) {
            tc__quicksort(d7, d5);
            d7 = d5 + 1;
        } else {
            tc__quicksort(d5 + 1, d6);
            d6 = d5;
        }
    }
    return 0;
}

/* ---- scanf engine ----
 * Locals: -4 assigned count, -8 conversions done, -0xc characters read, -0x10 overflow,
 * -0x14 number parser state, -0x15 scan-set range character, -0x36 Decimal record
 * {sgn, exp word, sig Str19} for floating conversions.
 * Spec (8 bytes copied from gScanfSpec):
 *   -0x1e flags: bit7 '*' (no assignment), bit6 width given, bit5 '?' (base from the
 *         argument list), bit4 negative / inverted scan set, bit3 something matched,
 *         bit2 unsigned, bit1 floating, bit0 decimal point seen
 *   -0x1d size: bit7 'h', bit6 'l', bit5 'L'
 *   -0x1a width (long) */
#define SF_FLAGS  (a6 - 0x1E)
#define SF_SIZE   (a6 - 0x1D)
#define SF_WIDTH  (a6 - 0x1A)
#define SF_STATE  (a6 - 0x14)
#define SF_OVF    (a6 - 0x10)
#define SF_NREAD  (a6 - 0xC)
#define SF_NCONV  (a6 - 8)
#define SF_NASSN  (a6 - 4)
#define SF_RANGE  (a6 - 0x15)
#define SF_DEC    (a6 - 0x36)
#define SF_FMT    (a6 + 0xC)
#define SF_VA     (a6 + 0x10)

/* 00059f50: vfscanf(fp, fmt, va): returns the number of assignments, or -1 on input
 * failure before any conversion. */
uint32_t tc_vfscanf(uint32_t fp, uint32_t fmt, uint32_t va) {
    ENTRY_LINK(FN_tc_vfscanf, 0x36, 20);
    W32(SF_FMT, fmt);                      /* own slots: advanced as the format and the */
    W32(SF_VA, va);                        /* arguments are used */
    int16_t d7, d6 = 0, d5;
    uint32_t d4 = 0;
    maddr a4 = 0, a0;
    W32(SF_NASSN, 0);
    W32(SF_NCONV, 0);
    W32(SF_NREAD, 0);
    d7 = (int8_t)R8(R32(SF_FMT));
    goto test;

nextfmt:
    add32(SF_FMT, 1);
    d7 = (int8_t)R8(R32(SF_FMT));
test:
    if (d7 == 0) goto done;
    if (d7 != '%') goto literal;
    mcopy(SF_FLAGS, gScanfSpec, 8);
    add32(SF_FMT, 1);
    d7 = (int8_t)R8(R32(SF_FMT));
    if (d7 == '*') {
        bset8(SF_FLAGS, 7);
        add32(SF_FMT, 1);
        d7 = (int8_t)R8(R32(SF_FMT));
    }
    if (CTYPE(d7) & 0x10) {
        bset8(SF_FLAGS, 6);
        do {
            W32(SF_WIDTH, R32(SF_WIDTH) * 10 - 0x30 + (uint32_t)(int32_t)d7);
            add32(SF_FMT, 1);
            d7 = (int8_t)R8(R32(SF_FMT));
        } while (CTYPE(d7) & 0x10);
        if ((int32_t)R32(SF_WIDTH) <= 0) goto done;
    }
conv:
    switch (d7) {
    case 'h': bset8(SF_SIZE, 7); add32(SF_FMT, 1); d7 = (int8_t)R8(R32(SF_FMT)); goto conv;
    case 'l': bset8(SF_SIZE, 6); add32(SF_FMT, 1); d7 = (int8_t)R8(R32(SF_FMT)); goto conv;
    case 'L': bset8(SF_SIZE, 5); add32(SF_FMT, 1); d7 = (int8_t)R8(R32(SF_FMT)); goto conv;
    case '?': bset8(SF_FLAGS, 5); add32(SF_FMT, 1); d7 = (int8_t)R8(R32(SF_FMT)); goto conv;
    case 'd': d6 = 10; goto integer;
    case 'i': d6 = 0; goto integer;
    case 'o': d6 = 8; goto uinteger;
    case 'u': d6 = 10; goto uinteger;
    case 'p': bset8(SF_SIZE, 6); d6 = 16; goto uinteger;
    case 'x': case 'X': d6 = 16; goto uinteger;
    case 'e': case 'E': case 'f': case 'g': case 'G':
        bset8(SF_FLAGS, 1);
        W32(SF_STATE, 0xFFFFFFFF);
        goto number;

    case 's':
        do {
            d7 = (int16_t)lib_getc(fp);
            add32(SF_NREAD, 1);
        } while (CTYPE(d7) & 6);
        tc__scanset_clear(1);
        goto scanset;

    case '[':
        add32(SF_FMT, 1);
        d7 = (int8_t)R8(R32(SF_FMT));
        if (d7 == '^') {
            bset8(SF_FLAGS, 4);
            add32(SF_FMT, 1);
            d7 = (int8_t)R8(R32(SF_FMT));
        }
        tc__scanset_clear(0);
        for (;;) {
            if (d7 == 0) goto done;
            tc__scanset_add((uint8_t)d7);
            add32(SF_FMT, 1);
            d7 = (int8_t)R8(R32(SF_FMT));
            if (d7 == ']') break;
            if (d7 != '-') continue;
            a0 = R32(SF_FMT);
            if (R8(a0 + 1) == ']') continue;
            int16_t hi = (int8_t)R8(a0 + 1);
            uint8_t lo = R8(a0 - 1);
            W8(SF_RANGE, lo);
            if ((int16_t)lo > hi) continue;
            add32(SF_FMT, 1);
            d7 = (int8_t)R8(R32(SF_FMT));
            while ((int16_t)R8(SF_RANGE) != d7) {
                uint8_t c = R8(SF_RANGE);
                W8(SF_RANGE, c + 1);
                tc__scanset_add(c);
            }
        }
        if (btst8(SF_FLAGS, 4)) tc__scanset_invert();
        d7 = (int16_t)lib_getc(fp);
        add32(SF_NREAD, 1);
        goto scanset;

    case 'c':
        if (!btst8(SF_FLAGS, 6)) W32(SF_WIDTH, 1);
        if (!btst8(SF_FLAGS, 7)) {
            a0 = R32(SF_VA); add32(SF_VA, 4);
            a4 = R32(a0);
        }
        for (;;) {
            uint32_t w = R32(SF_WIDTH);
            W32(SF_WIDTH, w - 1);
            if ((int32_t)w <= 0) break;
            d7 = (int16_t)lib_getc(fp);
            if ((int16_t)(d7 + 1) == 0) goto done;
            if (!btst8(SF_FLAGS, 7)) W8(a4++, d7);
            add32(SF_NREAD, 1);
        }
        if (!btst8(SF_FLAGS, 7)) add32(SF_NASSN, 1);
        add32(SF_NCONV, 1);
        goto nextfmt;

    case 'n':
        d4 = R32(SF_NREAD);
        if (!btst8(SF_FLAGS, 7)) W32(SF_NASSN, R32(SF_NASSN) - 1);
        goto store;

    default:
        if (d7 != '%') goto done;
        goto literal;
    }

literal:
    if (CTYPE(d7) & 6) {
        /* white space in the format: skip white space in the input */
        do {
            d7 = (int16_t)lib_getc(fp);
            add32(SF_NREAD, 1);
        } while (CTYPE(d7) & 6);
        tc_ungetc((uint32_t)(int32_t)d7, fp);
        W32(SF_NREAD, R32(SF_NREAD) - 1);
        goto nextfmt;
    } else {
        uint32_t c = lib_getc(fp);
        d7 = (int16_t)c;
        if ((uint16_t)R8(R32(SF_FMT)) == (uint16_t)c) {
            add32(SF_NREAD, 1);
            goto nextfmt;
        }
        tc_ungetc((uint32_t)(int32_t)d7, fp);
        goto done;
    }

scanset:
    /* %s and %[: characters in the scan set */
    if (!btst8(SF_FLAGS, 6)) W32(SF_WIDTH, 0x7FFFFFFF);
    if (!btst8(SF_FLAGS, 7)) {
        a0 = R32(SF_VA); add32(SF_VA, 4);
        a4 = R32(a0);
    }
    while (d7 != -1) {
        W32(SF_WIDTH, R32(SF_WIDTH) - 1);
        if (tc__scanset_test((uint32_t)(int32_t)d7) == 0) break;
        bset8(SF_FLAGS, 3);
        if (!btst8(SF_FLAGS, 7)) W8(a4++, d7);
        if (R32(SF_WIDTH) == 0) goto setend;
        d7 = (int16_t)lib_getc(fp);
        add32(SF_NREAD, 1);
    }
    tc_ungetc((uint32_t)(int32_t)d7, fp);
    W32(SF_NREAD, R32(SF_NREAD) - 1);
setend:
    if (!btst8(SF_FLAGS, 3)) goto done;
    if (!btst8(SF_FLAGS, 7)) {
        W8(a4, 0);
        add32(SF_NASSN, 1);
    }
    add32(SF_NCONV, 1);
    goto nextfmt;

uinteger:
    bset8(SF_FLAGS, 2);
integer:
    if (btst8(SF_FLAGS, 5)) {
        a0 = R32(SF_VA); add32(SF_VA, 4);
        d6 = R16(a0 + 2);
    }
    W32(SF_STATE, 0);
number:
    d4 = 0;
    do {
        d7 = (int16_t)lib_getc(fp);
        add32(SF_NREAD, 1);
    } while (CTYPE(d7) & 6);
    if (!btst8(SF_FLAGS, 6)) W32(SF_WIDTH, 0x7FFFFFFF);
    W32(SF_OVF, 0);
    while (d7 != -1) {
        W32(SF_WIDTH, R32(SF_WIDTH) - 1);
        switch ((int32_t)R32(SF_STATE)) {
        case 0:                            /* integer: sign */
            W32(SF_STATE, 1);
            if (d7 == '-') { bset8(SF_FLAGS, 4); goto nextch; }
            if (d7 == '+') goto nextch;
            /* fall through */
        case 1:                            /* integer: leading 0 (octal / 0x) */
            W32(SF_STATE, 3);
            if (d7 != '0') goto digit;
            bset8(SF_FLAGS, 3);
            if (R32(SF_WIDTH) == 0) goto nextch;
            if (d6 == 0) {
                d6 = 8;
                W32(SF_STATE, 2);
                goto nextch;
            }
            if (d6 == 16) W32(SF_STATE, 2);
            goto nextch;
        case 2:                            /* integer: after the leading 0 */
            W32(SF_STATE, 3);
            if (d7 == 'x' || d7 == 'X') {
                d6 = 16;
                bclr8(SF_FLAGS, 3);
                goto nextch;
            }
            /* fall through */
        case 3:                            /* integer: digits */
        digit:
            d5 = d7;
            if (d5 >= '0' && d5 <= '9') d5 = (int16_t)(d5 - 0x30);
            else if (d5 >= 'A' && d5 <= 'Z') d5 = (int16_t)(d5 - 0x37);
            else if (d5 >= 'a' && d5 <= 'z') d5 = (int16_t)(d5 - 0x57);
            else goto unget;
            if (d6 == 0) d6 = 10;
            if (d6 <= d5) goto unget;
            {
                uint32_t hi = (d4 >> 16) * (uint16_t)d6;
                if ((hi >> 16) != 0) {
                    W8(SF_OVF, 0xFF);      /* st */
                } else {
                    uint32_t d0 = (hi << 16) | (uint16_t)d5;
                    uint32_t lo = (d4 & 0xFFFF) * (uint16_t)d6;
                    d4 = lo + d0;
                    if (d4 < lo) W8(SF_OVF, 0xFF);
                }
            }
            bset8(SF_FLAGS, 3);
            goto nextch;
        case -1:                           /* floating: sign */
            W32(SF_STATE, 0xFFFFFFFE);
            W16(SF_DEC + 2, 0);
            W8(SF_DEC + 4, 0);
            if (d7 == '-') { W8(SF_DEC, 1); goto nextch; }
            W8(SF_DEC, 0);
            if (d7 == '+') goto nextch;
            /* fall through */
        case -2:                           /* floating: mantissa */
            if (d7 >= '0' && d7 <= '9') {
                bset8(SF_FLAGS, 3);
                if (d7 != '0' || R8(SF_DEC + 4) != 0) {
                    if (R8(SF_DEC + 4) >= 0x13) {
                        if (btst8(SF_FLAGS, 0)) goto nextch;
                        W16(SF_DEC + 2, R16(SF_DEC + 2) + 1);
                        goto nextch;
                    }
                    W8(SF_DEC + 4, R8(SF_DEC + 4) + 1);
                    W8(SF_DEC + 4 + (uint32_t)(int32_t)(int8_t)R8(SF_DEC + 4), d7);
                }
                if (btst8(SF_FLAGS, 0)) W16(SF_DEC + 2, R16(SF_DEC + 2) - 1);
                goto nextch;
            }
            if (d7 == '.' && !btst8(SF_FLAGS, 0)) {
                bset8(SF_FLAGS, 0);
                goto nextch;
            }
            if ((d7 == 'e' || d7 == 'E') && btst8(SF_FLAGS, 3)) {
                d6 = 10;                   /* the exponent is read as an integer */
                bclr8(SF_FLAGS, 3);
                W32(SF_STATE, 0);
                goto nextch;
            }
            goto unget;
        default:
            break;
        }
    nextch:
        if (R32(SF_WIDTH) == 0) goto numend;
        d7 = (int16_t)lib_getc(fp);
        add32(SF_NREAD, 1);
    }
unget:
    tc_ungetc((uint32_t)(int32_t)d7, fp);
    W32(SF_NREAD, R32(SF_NREAD) - 1);
numend:
    if (!btst8(SF_FLAGS, 3)) goto done;
    if (btst8(SF_FLAGS, 4) && d4 != 0) {
        d4 = -d4;
        if (btst8(SF_FLAGS, 2) || (int32_t)d4 > 0) W32(SF_OVF, 1);
    } else if (!btst8(SF_FLAGS, 2) && (int32_t)d4 < 0) {
        W32(SF_OVF, 1);
    }
    if (btst8(SF_FLAGS, 1)) {
        /* floating: d4 is the exponent, added to the decimal exponent */
        int32_t e = (int32_t)R16(SF_DEC + 2);
        int64_t s = (int64_t)(int32_t)d4 + e;
        d4 = (uint32_t)(int32_t)d4 + (uint32_t)e;
        if (s != (int32_t)d4 || (int32_t)d4 != (int16_t)d4) W8(SF_OVF, 0xFF);
    } else if ((int8_t)R8(SF_SIZE) < 0) {  /* 'h' */
        if (btst8(SF_FLAGS, 2)) {
            if (d4 != (uint16_t)d4) W32(SF_OVF, 1);
        } else {
            if ((int32_t)d4 != (int16_t)d4) W32(SF_OVF, 1);
        }
    }
    if (R32(SF_OVF) != 0) {
        if (btst8(SF_FLAGS, 2)) d4 = 0;
        else if ((int8_t)R8(SF_SIZE) < 0 || btst8(SF_FLAGS, 1)) d4 = 0xFFFF8000;
        else d4 = 0x80000000;
        if (!btst8(SF_FLAGS, 4)) d4 = ~d4;
        if (!btst8(SF_FLAGS, 1)) W32(gErrno, 0x22);
    }
store:
    if ((int8_t)R8(SF_FLAGS) >= 0) {
        a0 = R32(SF_VA); add32(SF_VA, 4);
        a4 = R32(a0);
        if (btst8(SF_FLAGS, 1)) {
            W16(SF_DEC + 2, d4);
            if (btst8(SF_SIZE, 6)) bset8(SF_SIZE, 5);
            if (btst8(SF_SIZE, 5)) tc__scanf_dec2num(0, SF_DEC, a4);           /* extended */
            else if ((int8_t)R8(SF_SIZE) < 0) tc__scanf_dec2num(0x800, SF_DEC, a4);   /* double */
            else tc__scanf_dec2num(0x1000, SF_DEC, a4);                      /* float */
        } else if (btst8(SF_SIZE, 6)) {
            W32(a4, d4);
        } else if ((int8_t)R8(SF_SIZE) < 0) {
            W16(a4, d4);
        } else {
            W32(a4, d4);
        }
        add32(SF_NASSN, 1);
    }
    add32(SF_NCONV, 1);
    goto nextfmt;

done:
    if (R32(SF_NCONV) == 0 && d7 == -1) return 0xFFFFFFFF;
    return R32(SF_NASSN);
}
#undef SF_FLAGS
#undef SF_SIZE
#undef SF_WIDTH
#undef SF_STATE
#undef SF_OVF
#undef SF_NREAD
#undef SF_NCONV
#undef SF_NASSN
#undef SF_RANGE
#undef SF_DEC
#undef SF_FMT
#undef SF_VA

/* 0005a89c: clear the scan set; with `ws` set it becomes "everything but white space" */
uint32_t tc__scanset_clear(uint32_t ws) {
    ENTRY_LINK(FN_tc__scanset_clear, 0, 0);
    mset(gScanSet, 0, 32);
    if (ws != 0) {
        W8(gScanSet + 1, 0x3E);            /* 9..13 */
        W8(gScanSet + 4, 0x01);            /* ' ' */
        tc__scanset_invert();
    }
    return 0;
}

/* 0005a8d0: add character c to the scan set */
uint32_t tc__scanset_add(uint32_t c) {
    uint8_t bit = (uint8_t)(1u << (c & 7));
    maddr a = gScanSet + (uint32_t)((int32_t)c >> 3);
    W8(a, R8(a) | bit);
    return 0;
}

/* 0005a8f0: invert the scan set (register routine: D0 is left alone) */
uint32_t tc__scanset_invert(void) {
    for (int i = 0; i < 32; i += 4) W32(gScanSet + i, ~R32(gScanSet + i));
    return cpu_get(R_D0);
}

/* 0005a906: test character c (EOF = -1 reads the byte before the set) */
uint32_t tc__scanset_test(uint32_t c) {
    uint32_t d1 = 1u << (c & 7);
    uint32_t d0 = (uint32_t)(int32_t)(int8_t)R8(gScanSet + (uint32_t)((int32_t)c >> 3));
    return d0 & d1;
}

/* 0005a92c: SANE dec2num for scanf: Decimal -> dst in format fmt (0 extended, 0x800
 * double, 0x1000 single); overflow/underflow set errno 34 and store INF / 0 */
uint32_t tc__scanf_dec2num(int16_t fmt, uint32_t dec, uint32_t dst) {
    ENTRY_LINK(FN_tc__scanf_dec2num, 4, 16);
    maddr a4 = dec, a3 = dst;
    int16_t d7 = fmt;
    int16_t d6 = '0';
    sane(0x0017, 1, a6 - 2, 0, 0);                       /* FPROCENTRY */
    if (R8(a4 + 4) != 0) {
        sane((uint16_t)(d7 + 9), 2, a4, a3, 0);         /* FDEC2X */
        sane(0x0003, 1, a6 - 2, 0, 0);                   /* FGETENV */
        sane((uint16_t)(d7 + 0x1C), 2, a3, a6 - 4, 0);  /* FCLASSX */
        if (R16(a6 - 4) < 0) W16(a6 - 4, -R16(a6 - 4));
        if (R16(a6 - 4) == 3 || btst8(a6 - 2, 2)) {
            d6 = 'I';
        } else if (R16(a6 - 4) == 5 && !btst8(a6 - 2, 1)) {
            return 0;
        }
        W32(gErrno, 0x22);
    }
    if (d6 == '0') W8(a4, 0);
    W16(a4 + 2, 0);
    W8(a4 + 4, 1);
    W8(a4 + 5, d6);
    sane((uint16_t)(d7 + 9), 2, a4, a3, 0);             /* FDEC2X */
    return 0;
}

/* 0005a9da: signal(sig, handler): sig 1..6, returns the previous handler */
uint32_t tc_signal(uint32_t sig, uint32_t handler) {
    int32_t d7 = (int32_t)sig;
    if (d7 <= 0 || d7 > 6) {
        W32(gErrno, 0x16);
        return 0xFFFFFFFF;
    }
    uint32_t old = R32(gSignals + (uint32_t)d7 * 4);
    W32(gSignals + (uint32_t)d7 * 4, handler);
    return old;
}

/* 0005aa1e: raise(sig): SIG_IGN (1) ignored, SIG_DFL (0) -> _exit, else handler(sig)
 * after resetting it */
uint32_t tc_raise(uint32_t sig) {
    ENTRY_LINK(FN_tc_raise, 0, 8);
    int32_t d7 = (int32_t)sig;
    if (d7 <= 0 || d7 > 6) {
        W32(gErrno, 0x16);
        return 0xFFFFFFFF;
    }
    maddr a4 = R32(gSignals + (uint32_t)d7 * 4);
    if (a4 - 1 != 0) {
        W32(gSignals + (uint32_t)d7 * 4, 0);
        if (a4 == 0) tc__exit(0);
        call_ptr(a4, "l", (uint32_t)d7);
    }
    return 0;
}

/* 0005aa8a: make a FILE usable: attach the std streams to the console on first use; a
 * closed FILE becomes a dummy stream (1-byte buffer, null proc). Returns fp. */
uint32_t tc__checkfp(uint32_t fp) {
    ENTRY_LINK(FN_tc__checkfp, 0, 4);
    if ((int8_t)R8(fp + F_FLAGS) < 0) tc__console_init_std();
    if (R16(fp + F_REFNUM) == 0) {
        W32(fp + F_BUF, fp + 4);
        W32(fp + F_PTR, fp + 4);
        W32(fp + F_BUFSIZE, 1);
        W32(fp + F_PROC, JT_ADDR(FN_tc__stdio_nullproc));
    }
    return fp;
}

/* 0005aac4: copy C string src to Pascal string dst (length byte = strlen & 0xFF) */
uint32_t tc__c2pstr_copy(uint32_t src, uint32_t dst) {
    maddr a4 = src, a3 = dst + 1;
    uint8_t n = 0;
    uint8_t c;
    W8(dst, cpu_get(R_D0));                /* the original first stores D0's low byte */
    while ((c = R8(a4++)) != 0) {
        W8(a3++, c);
        n++;
    }
    W8(dst, n);
    return dst;
}

/* 0005aaec: string FILE for sscanf (cnt = strlen) */
uint32_t tc__sscanf_fp_init(uint32_t fp, uint32_t str) {
    ENTRY_LINK(FN_tc__sscanf_fp_init, 0, 0);
    uint32_t n = tc_strlen(str);
    tc__string_fp_init(fp, str, n);
    return fp;
}

/* 0005ab0e: string FILE for sprintf (unbounded: cnt = -1), writing */
uint32_t tc__sprintf_fp_init(uint32_t fp, uint32_t buf) {
    ENTRY_LINK(FN_tc__sprintf_fp_init, 0, 0);
    tc__string_fp_init(fp, buf, 0xFFFFFFFF);
    bset8(fp + F_FLAGS, 3);
    return fp;
}

/* 0005ab30: FILE on a string: refnum -1, ptr = p, cnt = n, null proc */
uint32_t tc__string_fp_init(uint32_t fp, uint32_t p, uint32_t n) {
    ENTRY_LINK(FN_tc__string_fp_init, 0, 4);
    tc_memset(fp, 0, 0x28);
    W16(fp + F_REFNUM, 0xFFFF);
    W32(fp + F_PTR, p);
    W32(fp + F_CNT, n);
    W32(fp + F_PROC, JT_ADDR(FN_tc__stdio_nullproc));
    return 0;
}

/* 0005ab68: stream proc of string FILEs: always -1 */
uint32_t tc__stdio_nullproc(void) {
    return 0xFFFFFFFF;
}

/* 0005ab72: sprintf(buf, fmt, ...) */
uint32_t tc_sprintf(uint32_t buf, uint32_t fmt) {
    ENTRY_LINK(FN_tc_sprintf, 0, 0);
    return tc_vsprintf(buf, fmt, a6 + 0x10);
}

/* 0005ab8a: vsprintf: printf into a string FILE on the stack, NUL-terminated */
uint32_t tc_vsprintf(uint32_t buf, uint32_t fmt, uint32_t va) {
    ENTRY_LINK(FN_tc_vsprintf, 0x28, 4);
    /* the original pushes vfprintf's last two arguments before calling the FILE init */
    maddr sp = emu_sp();
    W32(sp - 4, va);
    W32(sp - 8, fmt);
    emu_set_sp(sp - 8);
    uint32_t f = tc__sprintf_fp_init(a6 - 0x28, buf);
    emu_set_sp(sp);
    int32_t d7 = (int32_t)tc_vfprintf(f, fmt, va);
    if (d7 >= 0) W8(buf + (uint32_t)d7, 0);
    return (uint32_t)d7;
}

/* 0005abc6: sscanf(str, fmt, ...) */
uint32_t tc_sscanf(uint32_t str, uint32_t fmt) {
    ENTRY_LINK(FN_tc_sscanf, 0, 0);
    return tc_vsscanf(str, fmt, a6 + 0x10);
}

/* 0005abde: vsscanf: scanf from a string FILE on the stack */
uint32_t tc_vsscanf(uint32_t str, uint32_t fmt, uint32_t va) {
    ENTRY_LINK(FN_tc_vsscanf, 0x28, 0);
    maddr sp = emu_sp();
    W32(sp - 4, va);
    W32(sp - 8, fmt);
    emu_set_sp(sp - 8);
    uint32_t f = tc__sscanf_fp_init(a6 - 0x28, str);
    emu_set_sp(sp);
    return tc_vfscanf(f, fmt, va);
}

/* 0005ac02: fp->proc(fp, 0): fill */
uint32_t tc__fp_proc_fill(uint32_t fp) {
    ENTRY_LINK(FN_tc__fp_proc_fill, 0, 0);
    return call_ptr(R32(fp + F_PROC), "ll", fp, 0);
}

/* 0005ac1a: fp->proc(fp, 1): flush */
uint32_t tc__fp_proc_flush(uint32_t fp) {
    ENTRY_LINK(FN_tc__fp_proc_flush, 0, 0);
    return call_ptr(R32(fp + F_PROC), "ll", fp, 1);
}

/* 0005ac34: fp->proc(fp, 2): close */
uint32_t tc__fp_proc_close(uint32_t fp) {
    ENTRY_LINK(FN_tc__fp_proc_close, 0, 0);
    return call_ptr(R32(fp + F_PROC), "ll", fp, 2);
}

/* 0005ac4e: strcpy */
uint32_t tc_strcpy(uint32_t dst, uint32_t src) {
    maddr a0 = dst, a1 = src;
    uint8_t c;
    do { c = R8(a1++); W8(a0++, c); } while (c != 0);
    return dst;
}

/* 0005ac5e: strcat */
uint32_t tc_strcat(uint32_t dst, uint32_t src) {
    maddr a0 = dst, a1 = src;
    while (R8(a0++) != 0) {}
    a0--;
    uint8_t c;
    do { c = R8(a1++); W8(a0++, c); } while (c != 0);
    return dst;
}

/* 0005ac74: strcmp: unsigned chars, returns exactly -1, 0 or 1 */
uint32_t tc_strcmp(uint32_t a, uint32_t b) {
    for (;;) {
        uint8_t x = R8(a++), y = R8(b++);
        if (x != y) return x > y ? 1 : 0xFFFFFFFF;
        if (x == 0) return 0;
    }
}

/* 0005ac92: strchr ((char)c; c == 0 finds the terminator) */
uint32_t tc_strchr(uint32_t s, uint32_t c) {
    uint8_t ch = (uint8_t)c;
    for (;;) {
        if (R8(s) == ch) return s;
        if (R8(s++) == 0) return 0;
    }
}

/* 0005acaa: strcspn */
uint32_t tc_strcspn(uint32_t s, uint32_t set) {
    uint32_t n = 0xFFFFFFFF;
    for (;;) {
        n++;
        uint8_t c = R8(s++);
        if (c == 0) return n;
        maddr a1 = set;
        uint8_t d;
        while ((d = R8(a1++)) != 0)
            if (d == c) return n;
    }
}

/* 0005acc4: strpbrk */
uint32_t tc_strpbrk(uint32_t s, uint32_t set) {
    for (;;) {
        uint8_t c = R8(s++);
        if (c == 0) return 0;
        maddr a1 = set;
        uint8_t d;
        while ((d = R8(a1++)) != 0)
            if (d == c) return s - 1;
    }
}

/* 0005ace0: strrchr (the terminator can be found) */
uint32_t tc_strrchr(uint32_t s, uint32_t c) {
    uint8_t ch = (uint8_t)c;
    uint32_t r = 0;
    do {
        if (R8(s) == ch) r = s;
    } while (R8(s++) != 0);
    return r;
}

/* 0005acf6: strspn */
uint32_t tc_strspn(uint32_t s, uint32_t set) {
    uint32_t n = 0;
    for (;;) {
        uint8_t c = R8(s++);
        if (c == 0) return n;
        maddr a1 = set;
        uint8_t d;
        for (;;) {
            d = R8(a1++);
            if (d == 0) return n;
            if (d == c) break;
        }
        n++;
    }
}

/* 0005ad12: strstr (naive; empty sub -> s) */
uint32_t tc_strstr(uint32_t s, uint32_t sub) {
    uint32_t d0 = s;
    for (;;) {
        maddr a0 = d0, a1 = sub;
        for (;;) {
            uint8_t c = R8(a1++);
            if (c == 0) return d0;
            uint8_t d = R8(a0++);
            if (d == 0) return 0;
            if (d != c) break;
        }
        d0++;
    }
}

/* 0005ad30: strtok (saved pointer in gStrtokNext) */
uint32_t tc_strtok(uint32_t s, uint32_t delim) {
    uint32_t d0 = s;
    if (d0 == 0) {
        d0 = R32(gStrtokNext);
        if (d0 == 0) return 0;
    }
    W32(gStrtokNext, 0);
    maddr a0 = d0;
    /* skip leading delimiters */
    for (;;) {
        uint8_t c = R8(a0++);
        if (c == 0) return R32(gStrtokNext);
        maddr a1 = delim;
        uint8_t d;
        int isdelim = 0;
        while ((d = R8(a1++)) != 0)
            if (d == c) { isdelim = 1; break; }
        if (!isdelim) break;
        d0 = a0;
    }
    /* find the end of the token */
    for (;;) {
        uint8_t c = R8(a0++);
        if (c == 0) return d0;
        maddr a1 = delim;
        uint8_t d;
        while ((d = R8(a1++)) != 0) {
            if (d == c) {
                W32(gStrtokNext, a0);
                W8(a0 - 1, 0);
                return d0;
            }
        }
    }
}

/* 0005ad6e: strlen (32-bit) */
uint32_t tc_strlen(uint32_t s) {
    uint32_t n = 0xFFFFFFFF;
    do { n++; } while (R8(s++) != 0);
    return n;
}

/* 0005ad7c: ungetc: one pushed-back char in the buffer (moving the data up if needed)
 * or in the pushback byte at fp+5 */
uint32_t tc_ungetc(uint32_t c, uint32_t fp) {
    ENTRY_LINK(FN_tc_ungetc, 4, 4);
    if (c == 0xFFFFFFFF) return 0xFFFFFFFF;
    tc__checkfp(fp);
    if ((int8_t)R8(fp + F_FLAGS2) < 0) {
        if (R32(fp + F_CNT) != 0) return 0xFFFFFFFF;
    } else if (R32(fp + F_CNT) == R32(fp + F_BUFSIZE) && R32(fp + F_BUFSIZE) != 0) {
        uint32_t cnt = R32(fp + F_CNT);
        W32(fp + F_POS, R32(fp + F_POS) - cnt);
        W32(fp + F_CNT, 0);
        W32(fp + F_PTR, fp + 6);
        bset8(fp + F_FLAGS2, 7);
    } else if (R32(fp + F_PTR) == R32(fp + F_BUF)) {
        uint32_t cnt = R32(fp + F_CNT);
        uint32_t buf = R32(fp + F_BUF);
        add32(fp + F_PTR, R32(fp + F_BUFSIZE) - R32(fp + F_CNT));
        tc_memmove(R32(fp + F_PTR), buf, cnt);
    }
    bclr8(fp + F_FLAGS, 5);
    add32(fp + F_CNT, 1);
    W32(fp + F_PTR, R32(fp + F_PTR) - 1);
    W8(R32(fp + F_PTR), c);
    return (uint8_t)c;
}

/* 0005ae24: allocator of operator new (same scheme as malloc, own pools): small blocks
 * from 15008-byte pools, larger ones NewPtr(n+2) with header 0xFFFF, > 8 MB -> NULL */
uint32_t tc__new_alloc(uint32_t size) {
    ENTRY_LINK(FN_tc__new_alloc, 0, 0);
    if (size < 0x3A98) return tc__new_alloc_small((int16_t)size);
    if (size > 0x800000) return 0;
    int16_t err;
    maddr p = new_ptr(size + 2, &err);
    if (err < 0) return p;
    W16(p, 0xFFFF);
    return p + 2;
}

/* 0005ae60: free for operator delete: header ~= ; 0 means a NewPtr block */
uint32_t tc__new_free(uint32_t p) {
    ENTRY_LINK(FN_tc__new_free, 0, 0);
    if (p == 0) return 0;
    uint16_t h = (uint16_t)~RU16(p - 2);
    W16(p - 2, h);
    if (h == 0) _DisposePtr(p - 2);
    return 0;
}

/* 0005ae76: small-block allocator of operator new: first fit over the pools with lazy
 * coalescing of free blocks (header ~size when allocated, +size when free; each pool ends
 * with a 0xFFFF header and the link to the next pool). */
uint32_t tc__new_alloc_small(int16_t size) {
    ENTRY_LINK(FN_tc__new_alloc_small, 8, 4);
    uint16_t d7 = (uint16_t)((uint16_t)size + 3) & 0xFFFE;
    uint32_t d0, d2;
    uint16_t w;
    maddr a0;
    W32(a6 - 4, R32(gNewCurPool));
    d0 = R32(gNewRover);
    if (d0 != 0) goto scan;
newpool: {
        int16_t err;
        a0 = new_ptr(0x3AA0, &err);
        if (err < 0) return a0;
        W16(a0 + 0x3A9A, 0xFFFF);
        W32(a0 + 0x3A9C, R32(gNewPool));
        W32(gNewPool, a0);
        W32(gNewCurPool, a0);
        d0 = R32(gNewFirstPool);
        if (d0 == 0) {
            d0 = a0;
            W32(gNewFirstPool, d0);
        }
        W32(d0 + 0x3A9C, a0);
        w = 0x3A9A;
        W16(a0, w);
        goto take;
    }
nextpool:
    d0 = R32(a6 - 8);
    if (d0 == R32(a6 - 4)) goto newpool;
    W32(gNewCurPool, d0);
    W32(gNewRover, d0);
scan:
    a0 = d0;
    d2 = 0xFFFFFFFF;
    goto header;
used:
    w = (uint16_t)~w;
    if (w == 0) {                          /* end of a pool: continue with the next one */
        W32(a6 - 8, R32(a0 + 2));
        a0 = R32(gNewCurPool);
        d2 = R32(gNewRover);
    }
skip:
    a0 += (uint32_t)(int32_t)(int16_t)w;
    if (a0 >= d2) goto nextpool;
header:
    w = RU16(a0);
    if ((int16_t)w < 0) goto used;
    {                                      /* free block: merge the following free ones */
        uint16_t d1 = 0;
        do {
            w = (uint16_t)(w + d1);
            d1 = RU16(a0 + (uint32_t)(int32_t)(int16_t)w);
        } while ((int16_t)d1 >= 0);
    }
    W16(a0, w);
    if (w < d7) goto skip;
take: {
        maddr a1 = a0 + (uint32_t)(int32_t)(int16_t)d7;
        W32(gNewRover, a1);
        w = (uint16_t)(w - d7);
        if (w != 0) W16(a1, w);
        W16(a0, (uint16_t)~d7);
        return a0 + 2;
    }
}

/* ---- Symantec long multiply / divide: register helpers (D0 op D1) and stack shims ----
 * The C operators compute the same results; the registers the originals leave behind
 * (D1, A0) are reproduced. Division by zero is a 68k exception in the original. */
static uint32_t lmul_regs(uint32_t d0, uint32_t d1) {
    uint32_t r = d0 * d1;
    cpu_set(R_D1, (uint32_t)(uint16_t)(d0 >> 16) * (uint16_t)d1);
    cpu_set(R_A0, r);
    return r;
}
static uint32_t ulmod_regs(uint32_t d0, uint32_t d1) {    /* D0 = remainder, D1 = quotient */
    if (d1 == 0) port_divide_by_zero("tc_ulmod_d0d1");
    cpu_set(R_D1, d0 / d1);
    return d0 % d1;
}
static uint32_t lmod_regs(uint32_t d0, uint32_t d1) {     /* signed, remainder has the sign of D0 */
    int32_t d5 = (int32_t)d0, d6 = (int32_t)d1;
    uint32_t a = d5 < 0 ? -d0 : d0, b = d6 < 0 ? -d1 : d1;
    if (b == 0) port_divide_by_zero("tc_lmod_d0d1");
    uint32_t q = a / b, r = a % b;
    if (d5 < 0) r = -r;
    if ((d5 ^ d6) < 0) q = -q;
    cpu_set(R_D1, q);
    return r;
}
static uint32_t ldiv_regs(uint32_t d0, uint32_t d1) {
    lmod_regs(d0, d1);
    return cpu_get(R_D1);
}
static uint32_t uldiv_regs(uint32_t d0, uint32_t d1) {
    ulmod_regs(d0, d1);
    return cpu_get(R_D1);
}

/* 0005af40: D0 = 4(SP) * 8(SP) */
uint32_t tc__lmul_stk(uint32_t a, uint32_t b) {
    return lmul_regs(a, b);
}

/* 0005af48: D0 = D0 * D1 */
uint32_t tc_lmul_d0d1(void) {
    return lmul_regs(cpu_get(R_D0), cpu_get(R_D1));
}
void glue_tc_lmul_d0d1(void) { emu_set_d0(tc_lmul_d0d1()); }

/* 0005af68: 8(SP) / 4(SP) (signed) */
uint32_t tc__ldiv_stk(uint32_t divisor, uint32_t dividend) {
    return ldiv_regs(dividend, divisor);
}

/* 0005af70: D0 = D1 = D0 / D1 (signed, truncated) */
uint32_t tc_ldiv_d0d1(void) {
    return ldiv_regs(cpu_get(R_D0), cpu_get(R_D1));
}
void glue_tc_ldiv_d0d1(void) { emu_set_d0(tc_ldiv_d0d1()); }

/* 0005af78: 8(SP) / 4(SP) (unsigned) */
uint32_t tc__uldiv_stk(uint32_t divisor, uint32_t dividend) {
    return uldiv_regs(dividend, divisor);
}

/* 0005af80: D0 = D1 = D0 / D1 (unsigned) */
uint32_t tc_uldiv_d0d1(void) {
    return uldiv_regs(cpu_get(R_D0), cpu_get(R_D1));
}
void glue_tc_uldiv_d0d1(void) { emu_set_d0(tc_uldiv_d0d1()); }

/* 0005af88: 8(SP) % 4(SP) (signed) */
uint32_t tc__lmod_stk(uint32_t divisor, uint32_t dividend) {
    return lmod_regs(dividend, divisor);
}

/* 0005af90: D0 = D0 % D1 (sign of D0), D1 = D0 / D1 */
uint32_t tc_lmod_d0d1(void) {
    return lmod_regs(cpu_get(R_D0), cpu_get(R_D1));
}
void glue_tc_lmod_d0d1(void) { emu_set_d0(tc_lmod_d0d1()); }

/* 0005afb6: 8(SP) % 4(SP) (unsigned) */
uint32_t tc__ulmod_stk(uint32_t divisor, uint32_t dividend) {
    return ulmod_regs(dividend, divisor);
}

/* 0005afbe: D0 = D0 % D1, D1 = D0 / D1 (unsigned) */
uint32_t tc_ulmod_d0d1(void) {
    return ulmod_regs(cpu_get(R_D0), cpu_get(R_D1));
}
void glue_tc_ulmod_d0d1(void) { emu_set_d0(tc_ulmod_d0d1()); }

/* 0005b00e: C++ operator delete (Pascal-style: pops its argument) */
uint32_t tc_operator_delete(uint32_t p) {
    ENTRY_LINK(FN_tc_operator_delete, 0, 0);
    tc__new_free(p);
    return 0;
}

/* 0005b03e: C++ operator new: size 0 -> 2; retries through the new_handler */
uint32_t tc_operator_new(uint32_t size) {
    ENTRY_LINK(FN_tc_operator_new, 0, 4);
    W32(a6 + 8, size);
    if (size == 0) W32(a6 + 8, 2);
    maddr a2 = tc__new_alloc(R32(a6 + 8));
    if (a2 == 0 && R32(gNewHandler) != 0) {
        do {
            call_ptr(R32(gNewHandler), "");
            a2 = tc__new_alloc(R32(a6 + 8));
            if (a2 != 0) break;
        } while (R32(gNewHandler) != 0);
    }
    return a2;
}

/* 0005b092: C++ static constructors: '_STD' 0 is kept for tc__run_STD (registered with
 * atexit); each long of '_STI' 0 is an A5 offset of a constructor to call. */
uint32_t tc__run_STI(void) {
    ENTRY_NOLINK(FN_tc__run_STI, 8);
    W32(gSTDHandle, _GetResource(0x5F535444, 0));        /* '_STD' */
    if (R32(gSTDHandle) != 0) tc_atexit(JT_ADDR(FN_tc__run_STD));
    W32(gSTIHandle, _GetResource(0x5F535449, 0));        /* '_STI' */
    if (R32(gSTIHandle) != 0) {
        _MoveHHi(R32(gSTIHandle));
        _HLock(R32(gSTIHandle));
        maddr a4 = R32(R32(gSTIHandle));
        uint32_t d7 = _GetHandleSize(R32(gSTIHandle));
        while (d7 != 0) {
            maddr f = g_a5 + R32(a4);
            a4 += 4;
            cpu_set(R_A0, f);
            call68_addr(f);
            d7 -= 4;
        }
        _ReleaseResource(R32(gSTIHandle));
    }
    return 0;
}

/* 0005b0f4: C++ static destructors from '_STD' 0 */
uint32_t tc__run_STD(void) {
    ENTRY_NOLINK(FN_tc__run_STD, 8);
    _MoveHHi(R32(gSTDHandle));
    _HLock(R32(gSTDHandle));
    maddr a4 = R32(R32(gSTDHandle));
    uint32_t d7 = _GetHandleSize(R32(gSTDHandle));
    while (d7 != 0) {
        maddr f = g_a5 + R32(a4);
        a4 += 4;
        cpu_set(R_A0, f);
        call68_addr(f);
        d7 -= 4;
    }
    _HUnlock(R32(gSTDHandle));
    _ReleaseResource(R32(gSTDHandle));
    return 0;
}
