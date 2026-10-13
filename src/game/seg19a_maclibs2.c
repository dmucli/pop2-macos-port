/* CODE 19 "Mac Libs 2", first part (0x563c4-0x58a8a): the THINK C / Symantec C runtime.
 *   - malloc/calloc/realloc/free (15008-byte pools of 2-byte-header blocks + NewPtr)
 *   - atexit/_atexit/exit and the exit-handler machinery (ExitToShell patch)
 *   - atoi/strtol/strtoul (through sscanf)
 *   - the stdio buffer layer (_filbuf/_flsbuf) and the console package (a window with
 *     TextEdit attached to stdin/stdout/stderr; the game never uses it)
 *   - ctype, remove/rename/tmpfile/tmpnam, fopen/freopen
 * Most of these routines are hand-written assembly in the library: the translation follows
 * the disassembly instruction by instruction (word vs long arithmetic, flag tests).
 * Addresses in comments are Ghidra addresses (see docs/PORTING.md). */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* ---- allocator globals (A5 offsets) ---- */
#define gPoolLast       (-0x187a)   /* long: most recently created pool */
#define gPoolFirst      (-0x1876)   /* long: first pool (pools form a ring through +0x3a9c) */
#define gPoolCur        (-0x1872)   /* long: pool being searched */
#define gPoolRover      (-0x186e)   /* long: next block to look at in gPoolCur (0 = none yet) */
#define POOL_SIZE       0x3aa0      /* NewPtr size of a pool: 0x3a9a bytes of blocks, end marker, link */
#define SMALL_LIMIT     15000       /* requests below this come from the pools */

/* ---- exit machinery ---- */
#define gAtexitCount    (-0x186a)   /* long: atexit entries used */
#define gAtexitTable    (-0x1862)   /* 34 x {long fn, word run-on-ExitToShell} */
#define gAtexitAll      (-0x1866)   /* long: set to 1 by run_exit_handlers(0) */
#define gSavedExitHook  (-0x1796)   /* long: previous 0x6c(A5) exit hook */
#define gSavedExitTrap  (-0x1792)   /* long: original ExitToShell address */
#define gExitStub       (-0x178e)   /* long: {jmp patch} block (64K ROM only) */
#define gExitHook       (0x6c)      /* long at A5+0x6c: exit hook called by exit() */
#define gTrapBlock      (0x68)      /* long at A5+0x68: CODE 1 trap patch block */

/* ---- stdio ---- */
#define gStdin          (-0x151c)   /* FILE[15], 0x28 bytes each: stdin, stdout, stderr, ... */
#define gStdout         (-0x14f4)
#define gStderr         (-0x14cc)
#define gErrno          (-0x155e)   /* long */
/* FILE fields */
#define FP_FLAGS        0x00        /* b7 std stream not attached, b6 binary, b5 EOF, b4 error, b3 writing,
                                     * b2 malloc'd buffer, b1 append, b0 temporary */
#define FP_FLAGS2       0x01        /* b7 ungetc pushback active */
#define FP_REFNUM       0x06        /* word */
#define FP_BUF          0x08
#define FP_BUFSIZE      0x0c
#define FP_PTR          0x10
#define FP_CNT          0x14
#define FP_POS          0x18
#define FP_EOF          0x1c
#define FP_WIN          0x20        /* console window, or line-buffered flag */
#define FP_PROC         0x24

/* ---- console package (A5 offsets) ---- */
#define cWinState       (-0x1720)   /* 0x2e-byte state of the current console window */
#define cWindow         (-0x1720)   /*   long: WindowPtr */
#define cLineHeight     (-0x171c)   /*   word */
#define cCharWidth      (-0x171a)   /*   word */
#define cRows           (-0x1718)   /*   word */
#define cCols           (-0x1716)   /*   word */
#define cRow            (-0x1714)   /*   word: cursor row */
#define cCol            (-0x1712)   /*   word: cursor column */
#define cTabs           (-0x1710)   /*   word: tab width */
#define cTE             (-0x170e)   /*   long: TEHandle */
#define cMaxSize        (-0x170a)   /*   Point: largest window size (v, h) */
#define cPaste          (-0x1706)   /*   long: handle of pasted text fed as keys */
#define cPastePos       (-0x1702)   /*   long */
#define cPasteLen       (-0x16fe)   /*   long */
#define cEchoFile       (-0x16fa)   /*   long: FILE* echo */
#define cMode           (-0x16f6)   /*   byte: b7 RAW, b6 CBREAK, b5 ECHO, b4, b3 7-bit keys, b2 echo to printer */
#define cInited         (-0x16f2)   /* byte: Toolbox initialised by the console */
#define cDrvrRef        (-0x16ee)   /* word: refnum of the fake desk accessory */
#define cAppleMenu      (-0x16ec)   /* long: MenuHandle */
#define cWaitKey        (-0x1762)   /* word: wait for a key before closing at exit */

#define ROM85           0x28e       /* low memory: ROM version flags (< 0: 64K ROM) */

static inline void bset8(int32_t off, int bit) { SG8(off, G8(off) | (1u << bit)); }
static inline void bclr8(int32_t off, int bit) { SG8(off, G8(off) & ~(1u << bit)); }

/* Register-based OS trap: registers already set; returns D0. */
static uint32_t os_trap(uint16_t w) { tb_trap(w); return cpu_get(R_D0); }

/* ======================================================================== */

/* 000563c4: malloc */
uint32_t tc_malloc(uint32_t size) {
    ENTRY_LINK(FN_tc_malloc, 0, 0);
    if (size < SMALL_LIMIT) return tc__malloc_small((int16_t)size);
    if (size > 0x800000) return 0;
    cpu_set(R_D0, size + 2);
    int16_t err = (int16_t)os_trap(0xA11E);                /* NewPtr */
    maddr p = cpu_get(R_A0);
    if (err < 0) return p;
    W16(p, 0xffff);
    return p + 2;
}

/* 00056400: calloc. It rounds `size` and stores the total in its own argument slot. */
static uint32_t calloc_slot;                               /* final value of the size slot */
uint32_t tc_calloc(uint32_t nmemb, uint32_t size) {
    ENTRY_LINK(FN_tc_calloc, 4, 0);
    calloc_slot = size;
    if (size > 0x800000) return 0;
    if (size & 1) calloc_slot = ++size;
    uint32_t n = nmemb * size;                             /* tc_lmul */
    calloc_slot = n;
    if (n > 0x800000) return 0;
    if (n < SMALL_LIMIT) {
        uint32_t p = tc__malloc_small((int16_t)n);
        if (p != 0) tc_memset(p, 0, n);
        return p;
    }
    cpu_set(R_D0, n + 2);
    int16_t err = (int16_t)os_trap(0xA31E);                /* NewPtrClear */
    maddr q = cpu_get(R_A0);
    if (err < 0) return q;
    W16(q, 0xffff);
    return q + 2;
}
void glue_tc_calloc(void) {
    maddr sp = emu_sp();
    uint32_t r = tc_calloc(R32(sp), R32(sp + 4));
    W32(sp + 4, calloc_slot);
    emu_set_d0(r);
}

/* 00056480: realloc: shrink/grow pool blocks in place when possible, SetPtrSize for big
 * blocks, else malloc + BlockMove + free */
uint32_t tc_realloc(uint32_t p, uint32_t size) {
    ENTRY_LINK(FN_tc_realloc, 0, 20);
    uint32_t d7 = size, d6, d5;
    maddr a4 = p;
    if (d7 > 0x800000) return 0;
    if (a4 == 0) return tc_malloc(d7);
    if (d7 == 0) { tc_free(a4); return 0; }
    if (d7 & 1) d7++;
    uint32_t d0 = (uint16_t)~RU16(a4 - 2);
    if ((uint16_t)d0 == 0) {
        cpu_set(R_A0, a4 - 2);
        d0 = os_trap(0xA021);                              /* GetPtrSize */
    }
    d6 = d0 - 2;                                           /* current data size */
    if (d6 > d7) {
        d5 = d7;
        if (d6 < SMALL_LIMIT) {                            /* shrink a pool block in place */
            maddr a0 = a4 - 2;
            uint16_t d1 = RU16(a0);
            uint32_t diff = d6 - d7;
            d1 = (uint16_t)(d1 + (uint16_t)diff);
            W16(a0, d1);
            d1 = (uint16_t)~d1;
            a0 += (int16_t)d1;
            W16(a0, (uint16_t)diff);
            return a4;
        }
        if (d7 < SMALL_LIMIT) goto copy;
        goto setsize;
    }
    if (d6 >= d7) return a4;
    d5 = d6;
    if (d6 >= SMALL_LIMIT) goto setsize;
    if (d7 >= SMALL_LIMIT) goto copy;
    {                                                      /* grow a pool block into free followers */
        maddr a1 = a4 - 2;
        uint16_t w = (uint16_t)~RU16(a1);
        maddr a0 = a1 + (int16_t)w;
        w = RU16(a0);
        if ((int16_t)w < 0) goto copy;
        SG32(gPoolRover, G32(gPoolCur));
        uint16_t d1 = 0;
        do {
            w = (uint16_t)(w + d1);
            d1 = RU16(a0 + (int16_t)w);
        } while ((int16_t)d1 >= 0);
        W16(a0, w);
        uint32_t need = d7 - d6;
        a0 += (int16_t)(uint16_t)need;
        if (w < (uint16_t)need) goto copy;
        w = (uint16_t)(w - (uint16_t)need);
        if (w != 0) W16(a0, w);
        W16(a1, RU16(a1) - (uint16_t)need);
        return a4;
    }
setsize:
    cpu_set(R_A0, a4 - 2);
    cpu_set(R_D0, d7 + 2);
    if ((int16_t)os_trap(0xA020) >= 0) return a4;          /* SetPtrSize */
copy: {
        maddr a3 = tc_malloc(d7);
        if (a3 != 0) {
            cpu_set(R_D0, d5); cpu_set(R_A1, a3); cpu_set(R_A0, a4);
            tb_trap(0xA02E);                               /* BlockMove */
            tc_free(a4);
        }
        return a3;
    }
}

/* 00056586: free: a block header 0xffff (NewPtr block) becomes 0 -> DisposePtr; a pool
 * block's ~size header becomes +size (free) */
uint32_t tc_free(uint32_t p) {
    ENTRY_LINK(FN_tc_free, 0, 0);
    if (p == 0) return 0;
    maddr a0 = p - 2;
    uint16_t h = (uint16_t)~RU16(a0);
    W16(a0, h);
    if (h == 0) { cpu_set(R_A0, a0); return os_trap(0xA01F); }   /* DisposePtr */
    return p;
}

/* 0005659c: pool allocator: first fit from the rover, coalescing free neighbours; a new
 * pool when no pool has room */
uint32_t tc__malloc_small(int16_t size) {
    ENTRY_LINK(FN_tc__malloc_small, 8, 4);
    uint16_t d7 = (uint16_t)((uint16_t)size + 3) & 0xfffe;
    uint32_t start = G32(gPoolCur);                        /* -4(A6) */
    uint32_t next = 0;                                     /* -8(A6): link of the pool's end marker */
    uint32_t d0 = G32(gPoolRover);
    maddr a0;
    uint16_t w;
    if (d0 == 0) goto newpool;
    for (;;) {
        a0 = d0;
        uint32_t limit = 0xffffffff;
        for (;;) {
            w = RU16(a0);
            if ((int16_t)w < 0) {                          /* allocated, or the end marker */
                w = (uint16_t)~w;
                if (w == 0) {                              /* end: wrap to the pool start */
                    next = R32(a0 + 2);
                    a0 = G32(gPoolCur);
                    limit = G32(gPoolRover);
                }
            } else {                                       /* free: merge the free blocks after it */
                uint16_t d1 = 0;
                do {
                    w = (uint16_t)(w + d1);
                    d1 = RU16(a0 + (int16_t)w);
                } while ((int16_t)d1 >= 0);
                W16(a0, w);
                if (w >= d7) goto found;
            }
            a0 += (int16_t)w;
            if (a0 >= limit) break;
        }
        d0 = next;
        if (d0 == start) goto newpool;
        SG32(gPoolCur, d0);
        SG32(gPoolRover, d0);
    }
newpool:
    cpu_set(R_D0, POOL_SIZE);
    if ((int16_t)os_trap(0xA11E) < 0) return cpu_get(R_A0);     /* NewPtr */
    a0 = cpu_get(R_A0);
    W16(a0 + 0x3a9a, 0xffff);
    W32(a0 + 0x3a9c, G32(gPoolLast));
    SG32(gPoolLast, a0);
    SG32(gPoolCur, a0);
    d0 = G32(gPoolFirst);
    if (d0 == 0) { d0 = a0; SG32(gPoolFirst, d0); }
    W32(d0 + 0x3a9c, a0);
    w = 0x3a9a;
    W16(a0, w);
found: {
        maddr a1 = a0 + (int16_t)d7;
        SG32(gPoolRover, a1);
        w = (uint16_t)(w - d7);
        if (w != 0) W16(a1, w);
        W16(a0, (uint16_t)~d7);
        return a0 + 2;
    }
}

/* 00056650: atexit(fn) */
uint32_t tc_atexit(uint32_t fn) {
    ENTRY_LINK(FN_tc_atexit, 0, 4);
    if (0x21 < (int32_t)G32(gAtexitCount)) return 0xffffffff;
    uint32_t n = G32(gAtexitCount);
    SG32(gAtexitCount, n + 1);
    maddr e = GA(gAtexitTable) + n * 6;                    /* tc_lmul(n, 6) */
    W32(e, fn);
    tc__save_A5();
    if (G32(gSavedExitHook) == 0) {
        SG32(gSavedExitHook, G32(gExitHook));
        SG32(gExitHook, JT_ADDR(FN_tc__exit_hook_all));
    }
    return 0;
}

/* 000566a2: THINK C _atexit(fn): like atexit, but fn also runs when the application quits
 * through ExitToShell (patched the first time) */
uint32_t tc__atexit(uint32_t fn) {
    ENTRY_LINK(FN_tc__atexit, 0, 8);
    if (0x21 < (int32_t)G32(gAtexitCount)) return 0xffffffff;
    uint32_t n = G32(gAtexitCount);
    SG32(gAtexitCount, n + 1);
    maddr e = GA(gAtexitTable) + n * 6;                    /* tc_lmul(n, 6) */
    W32(e, fn);
    W16(e + 4, 1);
    tc__save_A5();
    if (G32(gSavedExitTrap) == 0) {
        cpu_set(R_D0, (e & 0xffff0000) | 0xa9f4);          /* move.w #$A9F4,D0 (D0 held the entry) */
        tb_trap(0xA146);                                   /* GetTrapAddress(ExitToShell) */
        SG32(gSavedExitTrap, cpu_get(R_A0));
        if (R16(ROM85) >= 0) {
            cpu_set(R_A0, JT_ADDR(FN_tc__ExitToShell_patch));
            cpu_set(R_D0, (cpu_get(R_D0) & 0xffff0000) | 0xa9f4);
            tb_trap(0xA047);                               /* SetTrapAddress */
        } else {
            cpu_set(R_D0, 6);
            tb_trap(0xA51E);                               /* NewPtrSys */
            maddr a3 = cpu_get(R_A0);
            W16(a3, 0x4ef9);                               /* jmp abs.l */
            W32(a3 + 2, JT_ADDR(FN_tc__ExitToShell_patch));
            SG32(gExitStub, a3);
            cpu_set(R_D0, (cpu_get(R_D0) & 0xffff0000) | 0xa9f4);
            cpu_set(R_A0, a3);
            tb_trap(0xA047);                               /* SetTrapAddress */
        }
    }
    return 0;
}

/* 00056728: atexit into fixed slot 1 */
uint32_t tc__atexit_slot1(uint32_t fn) {
    ENTRY_LINK(FN_tc__atexit_slot1, 0, 4);
    uint32_t saved = G32(gAtexitCount);
    SG32(gAtexitCount, 1);
    uint32_t r = tc_atexit(fn);
    SG32(gAtexitCount, saved);
    return r;
}

/* 0005674c: _atexit into fixed slot 0 */
uint32_t tc__atexit_slot0(uint32_t fn) {
    ENTRY_LINK(FN_tc__atexit_slot0, 0, 4);
    uint32_t saved = G32(gAtexitCount);
    SG32(gAtexitCount, 0);
    uint32_t r = tc__atexit(fn);
    SG32(gAtexitCount, saved);
    return r;
}

/* 0005676e: exit(status): call the exit hook, then ExitToShell */
uint32_t tc_exit(uint32_t status) {
    ENTRY_LINK(FN_tc_exit, 0, 0);
    call68_addr(G32(gExitHook));
    tb_trap(0xA9F4);                                       /* ExitToShell */
    return cpu_get(R_D0);
}

/* 0005677e: _exit(status): ExitToShell (the patched one still runs the _atexit handlers) */
uint32_t tc__exit(uint32_t status) {
    ENTRY_LINK(FN_tc__exit, 0, 0);
    tb_trap(0xA9F4);                                       /* ExitToShell */
    return cpu_get(R_D0);
}

/* 00056788: exit hook installed by atexit: run every handler */
uint32_t tc__exit_hook_all(void) {
    ENTRY_NOLINK(FN_tc__exit_hook_all, 0);
    return tc__run_exit_handlers(1);
}

/* 00056794: ExitToShell patch installed by _atexit */
uint32_t tc__ExitToShell_patch(void) {
    ENTRY_NOLINK(FN_tc__ExitToShell_patch, 0);
    tc__run_exit_handlers(0);
    tb_trap(0xA9F4);                                       /* ExitToShell */
    return cpu_get(R_D0);
}

/* 000567a0: run the exit handlers 33..0 with the application's A5 (all = 0: only the
 * _atexit ones), restore ExitToShell and call the previous exit hook */
uint32_t tc__run_exit_handlers(uint32_t all) {
    ENTRY_LINK(FN_tc__run_exit_handlers, 0, 16);
    uint32_t a5 = tc__get_saved_A5();
    uint32_t d6 = cpu_get(R_A5);                           /* exg D0,A5: globals now off the saved A5 */
    cpu_set(R_A5, a5);
#define XA(off) ((maddr)(a5 + (int32_t)(off)))
    if (all == 0) W32(XA(gAtexitAll), 1);
    maddr a4 = XA(gSavedExitHook);                         /* just past the table */
    uint32_t d7 = 0x22;
    for (;;) {
        uint32_t c = d7--;
        if (c == 0) break;
        a4 -= 6;
        maddr fn = R32(a4);
        if (fn == 0) continue;
        W32(a4, 0);
        if (all != 0 || R16(a4 + 4) != 0) call68_addr(fn);
    }
    if (R32(XA(gExitStub)) != 0) {
        cpu_set(R_A0, R32(XA(gExitStub)));
        tb_trap(0xA01F);                                   /* DisposePtr */
        W32(XA(gExitStub), 0);
    }
    if (R32(XA(gSavedExitTrap)) != 0) {
        cpu_set(R_D0, 0xa9f4);                             /* D0 was 0 (loop counter / noErr) */
        cpu_set(R_A0, R32(XA(gSavedExitTrap)));
        tb_trap(0xA047);                                   /* SetTrapAddress(ExitToShell) */
        W32(XA(gSavedExitTrap), 0);
    }
    if (R32(XA(gSavedExitHook)) != 0) call68_addr(R32(XA(gSavedExitHook)));
    else call68_addr(R32(XA(gExitHook)));
    W32(XA(gSavedExitHook), a5 + fn_info[FN_tc__exit_nop].jt_a5off);   /* lea $1402(A5) */
#undef XA
    cpu_set(R_A5, d6);                                     /* exg D0,A5 */
    return a5;
}

/* 00056834: empty exit hook */
uint32_t tc__exit_nop(void) {
    return 0;
}

/* 00056836: A1 = address of the A5 save slot that follows the bsr (code segment +2).
 * tc__pop_A1 (0005683c, "movea.l (SP)+,A1; rts") is the tail of this bsr trick: entered
 * with a data address as return address, it cannot run as a hooked native function and
 * stays on the 68k core (it is reached only from the original of this routine). */
uint32_t tc__A5_storage_addr(void) {
    cpu_set(R_A1, fn_address(FN_tc__A5_storage_addr) + 2);
    return 0;
}
void glue_tc__A5_storage_addr(void) { tc__A5_storage_addr(); }

/* 00056840: save A5 in the code-segment slot */
uint32_t tc__save_A5(void) {
    ENTRY_NOLINK(FN_tc__save_A5, 0);
    tc__A5_storage_addr();
    W32(cpu_get(R_A1), cpu_get(R_A5));
    return 0;
}
void glue_tc__save_A5(void) { tc__save_A5(); }

/* 00056848: D0 = saved A5 */
uint32_t tc__get_saved_A5(void) {
    ENTRY_NOLINK(FN_tc__get_saved_A5, 0);
    tc__A5_storage_addr();
    return R32(cpu_get(R_A1));
}
void glue_tc__get_saved_A5(void) { emu_set_d0(tc__get_saved_A5()); }

/* 00056850: atoi(s) = sscanf(s, "%d"-like format in DATA, &v) > 0 ? v : 0 */
uint32_t tc_atoi(uint32_t s) {
    ENTRY_LINK(FN_tc_atoi, 4, 0);
    maddr v = a6 - 4;
    if ((int32_t)call_ptr(fn_address(FN_tc_sscanf), "lll", s, GA(-0x178a), v) <= 0) W32(v, 0);
    return R32(v);
}

/* 00056878: atol(s) = strtol(s, NULL, 10) */
uint32_t tc_atol(uint32_t s) {
    ENTRY_LINK(FN_tc_atol, 0, 0);
    return tc_strtol(s, 0, 10);
}

/* 0005688e: strtol via sscanf(s, fmt, base, &value, &consumed) */
uint32_t tc_strtol(uint32_t s, uint32_t endp, uint32_t base) {
    ENTRY_LINK(FN_tc_strtol, 8, 0);
    if ((int32_t)call_ptr(fn_address(FN_tc_sscanf), "lllll", s, GA(-0x1786), base, a6 - 4, a6 - 8) <= 0) {
        W32(a6 - 8, 0);
        W32(a6 - 4, 0);
    }
    if (endp != 0) W32(endp, s + R32(a6 - 8));
    return R32(a6 - 4);
}

/* 000568d6: strtoul, same scheme with the unsigned format */
uint32_t tc_strtoul(uint32_t s, uint32_t endp, uint32_t base) {
    ENTRY_LINK(FN_tc_strtoul, 8, 0);
    if ((int32_t)call_ptr(fn_address(FN_tc_sscanf), "lllll", s, GA(-0x177e), base, a6 - 4, a6 - 8) <= 0) {
        W32(a6 - 8, 0);
        W32(a6 - 4, 0);
    }
    if (endp != 0) W32(endp, s + R32(a6 - 8));
    return R32(a6 - 4);
}

/* 0005691e: getc slow path: refill the buffer (or end an ungetc pushback) */
uint32_t tc__filbuf(uint32_t fp) {
    ENTRY_LINK(FN_tc__filbuf, 0, 4);
    W32(fp + FP_CNT, 0);
    tc__checkfp(fp);
    if ((int8_t)R8(fp + FP_FLAGS2) < 0) {                  /* pushback: back to the saved buffer */
        uint32_t n = R32(fp + FP_BUFSIZE);
        W32(fp + FP_CNT, n);
        W32(fp + FP_POS, R32(fp + FP_POS) + n);
        W32(fp + FP_PTR, R32(fp + FP_BUF));
        W8(fp + FP_FLAGS2, R8(fp + FP_FLAGS2) & 0x7f);
    } else {
        tc__stdio_prep_buffer(fp);
        if (tc__fp_proc_fill(fp) != 0) return 0xffffffff;
    }
    W32(fp + FP_CNT, R32(fp + FP_CNT) - 1);
    maddr p = R32(fp + FP_PTR);
    W32(fp + FP_PTR, p + 1);
    return R8(p);
}

/* 00056980: putc slow path */
uint32_t tc__flsbuf(uint32_t c, uint32_t fp) {
    ENTRY_LINK(FN_tc__flsbuf, 0, 8);
    uint32_t d7 = c;
    if (!(R8(fp + FP_FLAGS) & 0x08)) {                     /* start writing */
        tc__checkfp(fp);
        if (R8(fp + FP_FLAGS) & 0x02) W32(fp + FP_POS, R32(fp + FP_EOF));   /* append */
        tc__stdio_prep_buffer(fp);
        if (R32(fp + FP_WIN) != 0) W32(fp + FP_CNT, 1);
        W8(fp + FP_FLAGS, R8(fp + FP_FLAGS) | 0x08);
        uint32_t n = R32(fp + FP_CNT) - 1;
        W32(fp + FP_CNT, n);
        if (n != 0) {
            maddr p = R32(fp + FP_PTR);
            W32(fp + FP_PTR, p + 1);
            W8(p, d7);
            return (uint8_t)d7;
        }
    }
    d7 = (uint8_t)d7;
    maddr p = R32(fp + FP_PTR);
    W32(fp + FP_PTR, p + 1);
    W8(p, d7);
    if (R32(fp + FP_WIN) != 0 && !(0x20 > (int32_t)d7) &&
        R32(fp + FP_PTR) - R32(fp + FP_BUF) < R32(fp + FP_BUFSIZE)) {
        W32(fp + FP_CNT, 1);                               /* console: flush only at control chars or full */
        return d7;
    }
    if (tc__stdio_flush_buffer(fp) != 0) return 0xffffffff;
    return d7;
}

/* 00056a24: start a buffer window: ptr = buf, cnt = bufsize (aligned on the file position
 * for disk files), pos += cnt */
uint32_t tc__stdio_prep_buffer(uint32_t fp) {
    ENTRY_LINK(FN_tc__stdio_prep_buffer, 0, 4);
    W32(fp + FP_PTR, R32(fp + FP_BUF));
    W32(fp + FP_CNT, R32(fp + FP_BUFSIZE));
    if (R16(fp + FP_REFNUM) > 0 && R32(fp + FP_BUFSIZE) > 1) {
        uint32_t m = R32(fp + FP_POS) % R32(fp + FP_BUFSIZE);   /* tc_ulmod(pos, bufsize) */
        W32(fp + FP_CNT, R32(fp + FP_CNT) - m);
    }
    uint32_t n = R32(fp + FP_CNT);
    W32(fp + FP_POS, R32(fp + FP_POS) + n);
    return n;
}

/* 00056a66: end the buffer window; write it out if dirty */
uint32_t tc__stdio_flush_buffer(uint32_t fp) {
    ENTRY_LINK(FN_tc__stdio_flush_buffer, 0, 8);
    uint32_t r = 0;
    W32(fp + FP_POS, R32(fp + FP_POS) - R32(fp + FP_CNT));
    if (R8(fp + FP_FLAGS) & 0x08) {
        W32(fp + FP_CNT, R32(fp + FP_PTR) - R32(fp + FP_BUF));
        W32(fp + FP_PTR, R32(fp + FP_BUF));
        W8(fp + FP_FLAGS, R8(fp + FP_FLAGS) & ~0x08);
        r = tc__fp_proc_flush(fp);
    }
    tc__stdio_reset_buffer(fp);
    return r;
}

/* 00056ab4: empty buffer: ptr = buf + bufsize, cnt = 0, no pushback */
uint32_t tc__stdio_reset_buffer(uint32_t fp) {
    maddr p = R32(fp + FP_BUF) + R32(fp + FP_BUFSIZE);
    W32(fp + FP_PTR, p);
    W32(fp + FP_CNT, 0);
    W8(fp + FP_FLAGS2, R8(fp + FP_FLAGS2) & 0x7f);
    return p;
}

/* 00056aea: make `to` a console stream, sharing the window of `from` (or a new one) */
uint32_t tc_freopenc(uint32_t from, uint32_t to) {
    ENTRY_LINK(FN_tc_freopenc, 0, 4);
    if (to == 0) return 0;
    if (R8(0x8f2) != 0) tc__console_InitToolbox();         /* QDExist: QuickDraw not initialised */
    tc_fclose(to);
    W16(to + FP_REFNUM, 0xffff);
    uint32_t win = from != 0 ? R32(from + FP_WIN) : tc__console_new_window();
    W32(to + FP_WIN, win);
    tc_setvbuf(to, 0, 1, 0x200);
    W32(to + FP_PROC, JT_ADDR(FN_tc__console_proc));
    tc__atexit_slot1(JT_ADDR(FN_tc__stdio_exit_cleanup));
    return to;
}

/* ---- console API: each call selects the stream's window, saves the port state in a
 * local record (tc__console_enter_port/leave_port) and works on the console globals ---- */

/* 00056b56: cgotoxy(x, y, fp) */
uint32_t tc_cgotoxy(uint32_t x, uint32_t y, uint32_t fp) {
    ENTRY_LINK(FN_tc_cgotoxy, 8, 0);
    tc__console_enter_port(tc__console_select(fp), a6 - 8);
    SG16(cCol, (uint16_t)x - 1);
    SG16(cRow, (uint16_t)y - 1);
    return tc__console_leave_port(a6 - 8);
}

/* 00056b8c: cgetxy(&x, &y, fp) */
uint32_t tc_cgetxy(uint32_t px, uint32_t py, uint32_t fp) {
    ENTRY_LINK(FN_tc_cgetxy, 8, 0);
    tc__console_enter_port(tc__console_select(fp), a6 - 8);
    W32(px, (int32_t)G16(cCol) + 1);
    W32(py, (int32_t)G16(cRow) + 1);
    return tc__console_leave_port(a6 - 8);
}

/* 00056bc6: ccleos(fp): clear to end of screen */
uint32_t tc_ccleos(uint32_t fp) {
    ENTRY_LINK(FN_tc_ccleos, 8, 0);
    tc__console_enter_port(tc__console_select(fp), a6 - 8);
    tc__console_clear_from_line((uint32_t)(int32_t)G16(cRow));
    return tc__console_leave_port(a6 - 8);
}

/* 00056bf2: ccleol(fp): clear to end of line */
uint32_t tc_ccleol(uint32_t fp) {
    ENTRY_LINK(FN_tc_ccleol, 8, 0);
    tc__console_enter_port(tc__console_select(fp), a6 - 8);
    tc__console_clear_eol_impl();
    return tc__console_leave_port(a6 - 8);
}

/* 00056c18: csettabs(n, fp): tab width, 1..ncols */
uint32_t tc_csettabs(uint32_t n, uint32_t fp) {
    ENTRY_LINK(FN_tc_csettabs, 8, 4);
    int32_t d7 = (int32_t)n;
    tc__console_enter_port(tc__console_select(fp), a6 - 8);
    if (1 > d7 || (int32_t)G16(cCols) < d7) d7 = 1;
    SG16(cTabs, d7);
    return tc__console_leave_port(a6 - 8);
}

/* 00056c5a: csetmode(mode, fp): 0 RAW, 1 CBREAK, 2 NOECHO, 3 ECHO */
uint32_t tc_csetmode(uint32_t mode, uint32_t fp) {
    ENTRY_LINK(FN_tc_csetmode, 8, 0);
    tc__console_enter_port(tc__console_select(fp), a6 - 8);
    bclr8(cMode, 5);
    bclr8(cMode, 6);
    bclr8(cMode, 7);
    int32_t m = (int32_t)mode;
    if (m == 0) bset8(cMode, 7);
    else if (m == 1) bset8(cMode, 6);
    else if (m == 3) bset8(cMode, 5);
    return tc__console_leave_port(a6 - 8);
}

/* 00056cba: cinverse(on, fp): QuickDraw text proc drawing chars >= 0x80 inverted */
uint32_t tc_cinverse(uint32_t on, uint32_t fp) {
    ENTRY_LINK(FN_tc_cinverse, 8, 4);
    maddr win = tc__console_select(fp);
    tc__console_enter_port(win, a6 - 8);
    if (on != 0) {
        if (R32(win + 0x68) == 0) {                        /* grafProcs */
            W32(win + 0x68, tc_malloc(0x34));
            _SetStdProcs(R32(win + 0x68));
            W32(R32(win + 0x68), JT_ADDR(FN_tc__console_inverse_textProc));   /* textProc */
        }
    } else if (R32(win + 0x68) != 0) {
        tc_free(R32(win + 0x68));
        W32(win + 0x68, 0);
    }
    tc__console_clear_from_line(0);
    return tc__console_leave_port(a6 - 8);
}

/* 00056d2c: cshow(fp) */
uint32_t tc_cshow(uint32_t fp) {
    ENTRY_LINK(FN_tc_cshow, 0, 4);
    maddr win = tc__console_select(fp);
    if (_FrontWindow() != win) _SelectWindow(win);
    return _ShowWindow(win);
}

/* 00056d52: chide(fp) */
uint32_t tc_chide(uint32_t fp) {
    ENTRY_LINK(FN_tc_chide, 0, 0);
    return _HideWindow(tc__console_select(fp));
}

/* 00056d66: cecho2file(name, append, fp) */
uint32_t tc_cecho2file(uint32_t name, uint32_t append, uint32_t fp) {
    ENTRY_LINK(FN_tc_cecho2file, 8, 0);
    tc__console_enter_port(tc__console_select(fp), a6 - 8);
    tc__console_end_echo();
    SG32(cEchoFile, tc_fopen(name, append != 0 ? GA(-0x16cc) : GA(-0x16ca)));
    bclr8(cMode, 2);
    return tc__console_leave_port(a6 - 8);
}

/* 00056db6: cecho2printer(fp) */
uint32_t tc_cecho2printer(uint32_t fp) {
    ENTRY_LINK(FN_tc_cecho2printer, 8, 0);
    tc__console_enter_port(tc__console_select(fp), a6 - 8);
    tc__console_end_echo();
    SG32(cEchoFile, tc_tmpfile());
    bset8(cMode, 2);
    return tc__console_leave_port(a6 - 8);
}

/* 00056dea: attach stdin/stdout/stderr to one console window on first use */
uint32_t tc__console_init_std(void) {
    ENTRY_LINK(FN_tc__console_init_std, 0x28, 4);
    maddr a4 = 0;
    if ((int8_t)G8(gStdin) < 0) a4 = tc_freopenc(a4, GA(gStdin));
    if ((int8_t)G8(gStdout) < 0) a4 = tc_freopenc(a4, GA(gStdout));
    if ((int8_t)G8(gStderr) < 0) a4 = tc_freopenc(a4, GA(gStderr));
    if (G8(-0x1722) != 0) {                                /* echo to "<app name>..." file */
        call_ptr(fn_address(FN_tc_sprintf), "lll", a6 - 0x28, GA(-0x16c8), 0x910);
        tc_cecho2file(a6 - 0x28, 1, GA(gStdout));
        SG16(cWaitKey, 0);
    }
    return a4;
}

/* 00056e62: Toolbox initialisation for a console-only program */
uint32_t tc__console_InitToolbox(void) {
    ENTRY_NOLINK(FN_tc__console_InitToolbox, 8);
    if (R16(ROM85) >= 0) _MaxApplZone();
    for (int32_t i = 0; 10 > i; i++) _MoreMasters();
    maddr g = _NewPtr(0xce);
    _InitGraf(g + 0xca);
    _InitFonts();
    _InitWindows();
    _TEInit();
    _InitDialogs(0);
    _InitMenus();
    maddr m = _NewMenu(1, GA(-0x16c0));
    SG32(cAppleMenu, m);
    _InsertMenu(m, 0);
    _AppendResMenu(G32(cAppleMenu), 0x44525652);          /* 'DRVR' */
    m = _NewMenu(2, GA(-0x16be));
    _InsertMenu(m, 0);
    _AppendMenu(m, GA(-0x16b8));
    m = _NewMenu(3, GA(-0x16b0));
    _InsertMenu(m, 0);
    _AppendMenu(m, GA(-0x16aa));
    _DrawMenuBar();
    _FlushEvents((int16_t)0xffff, 0);
    _InitCursor();
    SG8(cInited, 1);
    return 0;
}

/* 00056f0c: console event pump: pasted text first, else one event (menus, DAs) */
uint32_t tc__console_poll_event(void) {
    ENTRY_LINK(FN_tc__console_poll_event, 0x118, 4);
    maddr ev = a6 - 0x10;                                  /* EventRecord */
    if (G32(cPaste) != 0) {
        uint32_t pos = G32(cPastePos);
        SG32(cPastePos, pos + 1);
        uint32_t c = R8(R32(G32(cPaste)) + pos);
        if (G32(cPastePos) == G32(cPasteLen)) {
            cpu_set(R_A0, G32(cPaste));
            tb_trap(0xA023);                               /* DisposeHandle */
            SG32(cPaste, 0);
        }
        if (G8(cMode) & 0x08) c &= 0x7f;
        if (c == 9) c = 0x20;
        tc__console_key(c);
        return 0;
    }
    _SystemTask();
    W8(0x15c, 0);                                          /* SEvtEnb */
    if (_GetNextEvent((int16_t)0xffff, ev) == 0) {
        if (R16(ev) != 0) return 0;
        if (_FrontWindow() != 0) return 0;
        _InitCursor();
        return 0;
    }
    if (_SystemEvent(ev) != 0) return 0;
    if (R16(ev) != 1) return 0;                            /* mouseDown */
    int16_t part = (int16_t)_FindWindow(R32(a6 - 6), a6 - 0x14);
    if (part <= 0) return 0;
    if (part == 2) { _SystemClick(ev, R32(a6 - 0x14)); return 0; }   /* inSysWindow */
    if (part > 2) return 0;
    _InitCursor();                                         /* inMenuBar */
    W32(a6 - 0x18, _MenuSelect(R32(a6 - 6)));
    int16_t menu = R16(a6 - 0x18);
    if (menu > 0) {
        if (menu == 1) {                                   /* Apple: open the desk accessory */
            _GetMenuItemText(G32(cAppleMenu), R16(a6 - 0x16), a6 - 0x118);
            _OpenDeskAcc(a6 - 0x118);
        } else if (menu <= 3) {
            if (menu == 2) {                               /* File: Quit */
                SG16(cWaitKey, 0);
                tc_exit(0);
            }
            _SystemEdit((int16_t)(R16(a6 - 0x16) - 1));    /* Edit */
        }
    }
    _HiliteMenu(0);
    return 0;
}

/* 0005702c: create the console window (font, size, TextEdit record, zoom state, fake
 * desk accessory for event routing); returns the WindowPtr */
uint32_t tc__console_new_window(void) {
    ENTRY_LINK(FN_tc__console_new_window, 0x18, 8);
    _GetPort(a6 - 4);
    tc__console_set_window(0);
    maddr win = _NewWindow(0, GA(-0x1736), G32(-0x1772), 0, G16(-0x176e), 0xffffffff, 0, 0);
    _MoveWindow(win, G16(-0x1774), G16(-0x1776), 0);
    SG32(cWindow, win);
    _SetPort(win);
    _TextFont(G16(-0x176c));
    _TextSize(G16(-0x176a));
    _TextFace(G16(-0x1768));
    _GetFontInfo(a6 - 0x18);                               /* ascent, descent, widMax, leading */
    SG16(cLineHeight, (uint16_t)(RU16(a6 - 0x18) + RU16(a6 - 0x16) + RU16(a6 - 0x12)));
    SG16(cCharWidth, RU16(a6 - 0x14));
    SG16(cTabs, 8);
    bclr8(cMode, 3);
    bclr8(cMode, 4);
    bclr8(cMode, 6);
    bclr8(cMode, 7);
    bset8(cMode, 5);
    SG32(cPaste, 0);
    SG32(cEchoFile, 0);
    maddr r = a6 - 0x10;                                   /* Rect */
    W16(r + 2, 0);
    W16(r, 0);
    uint16_t n = GU16(-0x1766);
    SG16(cRows, n);
    W16(r + 4, (uint16_t)((uint32_t)n * GU16(cLineHeight) + 8));
    n = GU16(-0x1764);
    SG16(cCols, n);
    W16(r + 6, (uint16_t)((uint32_t)n * GU16(cCharWidth) + 8));
    _SizeWindow(win, R16(r + 6), R16(r + 4), 0);
    SG32(cTE, _TENew(r, r));
    W16(R32(G32(cTE)) + 0x48, 0xffff);                     /* crOnly */
    SG16(cRow, G16(cRows) - 1);
    tc__console_clear_from_line(0);
    SG32(cMaxSize, R32(r + 4));
    SG16(cMaxSize, GU16(cMaxSize) + 1);
    SG16(cMaxSize + 2, GU16(cMaxSize + 2) + 1);
    _LocalToGlobal(r);
    _LocalToGlobal(r + 4);
    maddr zs = R32(R32(win + 0x82));                       /* WStateData */
    W32(zs + 8, R32(r));
    W32(zs + 0xc, R32(r + 4));
    W32(zs, R32(zs + 8));
    W32(zs + 4, R32(zs + 0xc));
    _PtrToHand(GA(cWinState), win + 0x98, 0x2e);           /* refCon = copy of the state */
    if (G16(cDrvrRef) == 0) SG16(cDrvrRef, tc__console_install_driver());
    W16(win + 0x6c, G16(cDrvrRef));                        /* windowKind */
    tc__console_resize_te();
    _ShowWindow(win);
    _SetPort(R32(a6 - 4));
    return win;
}

/* 000571a6: dispose of the console window unless another stream still uses it */
uint32_t tc__console_close_window(void) {
    ENTRY_NOLINK(FN_tc__console_close_window, 12);
    uint32_t users = 0;
    maddr fp = GA(gStdin);
    for (uint32_t n = 15; n-- != 0; fp += 0x28) {
        if (R32(fp + FP_WIN) == G32(cWindow) && users++ != 0) return 0;
    }
    tc__console_end_echo();
    if (G32(cPaste) != 0) { cpu_set(R_A0, G32(cPaste)); tb_trap(0xA023); }      /* DisposeHandle */
    cpu_set(R_A0, R32(G32(cWindow) + 0x98));
    tb_trap(0xA023);                                       /* DisposeHandle(refCon state copy) */
    _TEDispose(G32(cTE));
    _DisposeWindow(G32(cWindow));
    SG32(cWindow, 0);
    return 0;
}

/* 00057202: close the echo file (print it first when echoing to the printer) */
uint32_t tc__console_end_echo(void) {
    ENTRY_NOLINK(FN_tc__console_end_echo, 0);
    if (G32(cEchoFile) == 0) return 0;
    if (G8(cMode) & 0x04) tc__console_print_echo();
    return tc_fclose(G32(cEchoFile));
}

/* 00057220: flush the other dirty streams sharing fp's window; returns the window */
uint32_t tc__console_select(uint32_t fp) {
    ENTRY_LINK(FN_tc__console_select, 0, 12);
    maddr win = R32(tc__checkfp(fp) + FP_WIN);
    maddr f = GA(gStdin);
    for (uint32_t n = 15; n-- != 0; f += 0x28)
        if ((R8(f + FP_FLAGS) & 0x08) && R32(f + FP_WIN) == win) tc_fflush(f);
    return win;
}

/* 0005726c: atexit slot 1: flush console streams, optionally wait for a key, close streams */
uint32_t tc__stdio_exit_cleanup(void) {
    ENTRY_NOLINK(FN_tc__stdio_exit_cleanup, 8);
    maddr f = GA(gStdin);
    for (uint32_t n = 15; n-- != 0; f += 0x28)
        if ((R8(f + FP_FLAGS) & 0x08) && R32(f + FP_WIN) != 0) tc_fflush(f);
    if (G8(cInited) != 0 && G16(cWaitKey) != 0) {
        f = GA(gStdin);
        for (uint32_t n = 15; n-- != 0; f += 0x28) {
            if (R32(f + FP_WIN) == 0) continue;
            _SetWTitle(R32(f + FP_WIN), GA(-0x1684));     /* "press return" style title */
            bclr8(cMode, 5);
            bclr8(cMode, 6);
            bclr8(cMode, 7);
            tc_setbuf(f, 0);
            tc_fgetc(f);
            break;
        }
    }
    f = GA(gStdin);
    for (uint32_t n = 15; n-- != 0; f += 0x28)
        if (R32(f + FP_WIN) != 0) tc_fclose(f);
    return 0;
}

/* 00057314: console stream proc: 0 = read a line, 1 = write the buffer, 2 = close */
uint32_t tc__console_proc(uint32_t fp, uint32_t cmd) {
    ENTRY_LINK(FN_tc__console_proc, 8, 8);
    uint32_t r = 0;
    if (G32(gAtexitAll) != 0) return 0;
    tc__console_enter_port(R32(fp + FP_WIN), a6 - 8);
    int32_t c = (int32_t)cmd;
    if (c == 0) {
        uint32_t p = R32(fp + FP_PTR);
        SG32(-0x16e0, p);                                  /* input end */
        SG32(-0x16e4, p);                                  /* input start */
        if (G8(cInited) != 0) {
            tc_cshow(fp);
            bset8(cMode, 4);                               /* reading */
            SG32(-0x16dc, R32(fp + FP_CNT));               /* room left */
            if ((G8(cMode) & 0x20) &&
                (uint32_t)(int32_t)G16(cCols) < (uint32_t)((int32_t)G16(cCol) + G32(-0x16dc)))
                SG32(-0x16dc, (int32_t)G16(cCols) - (int32_t)G16(cCol) + 1);
            uint32_t pos = (int8_t)G8(cMode) < 0 ? 0 : tc__console_te_activate(0);
            SG32(-0x16d4, pos);
            SG32(-0x16d8, pos);
            W8(fp + FP_FLAGS, R8(fp + FP_FLAGS) & ~0x20);
            do tc__console_poll_event();
            while (G32(-0x16dc) != 0 && (int8_t)G8(cMode) >= 0);
            bclr8(cMode, 4);
        }
        uint32_t n = G32(-0x16e0) - G32(-0x16e4);
        W32(fp + FP_CNT, n);
        if (n == 0) { W8(fp + FP_FLAGS, R8(fp + FP_FLAGS) | 0x20); r = 0xffffffff; }
    } else if (c == 1) {
        tc__console_write(R32(fp + FP_PTR), R16(fp + FP_CNT + 2));
    } else if (c == 2) {
        tc__console_close_window();
        if (R32(fp + FP_WIN) == R32(a6 - 8)) W32(a6 - 8, 0);
    }
    if (G8(-0x16f0) != 0) {                                /* Cmd-. typed */
        SG8(-0x16f0, 0);
        _FlushEvents(8, 0);
        W32(fp + FP_CNT, 0);
        tc_raise(4);
        SG32(gErrno, 4);
        r = 0xffffffff;
    }
    tc__console_leave_port(a6 - 8);
    return r;
}

/* 0005744a: QuickDraw textProc (Pascal): chars with bit 7 set are drawn (c & 0x7f) in
 * TextMode 4 (inverted) */
uint32_t tc__console_inverse_textProc(uint32_t denom, uint32_t numer, uint32_t buf, int16_t count) {
    ENTRY_LINK(FN_tc__console_inverse_textProc, 2, 12);
    int16_t d7 = count;
    maddr a4 = buf;
    for (;;) {
        int16_t d0 = d7--;
        if (d0 == 0) break;
        maddr a3 = a4;
        int neg;
        for (;;) {                                         /* tst.b (A4)+ / dbmi D7 */
            neg = (R8(a4++) & 0x80) != 0;
            if (neg) break;
            if (--d7 == -1) break;
        }
        if (neg) a4--;
        if (a3 < a4) _StdText((int16_t)(a4 - a3), a3, numer, denom);
        if (d7 < 0) break;
        W8(a6 - 1, R8(a4++) & 0x7f);
        _TextMode(4);
        _StdText(1, a6 - 1, numer, denom);
        _TextMode(0);
    }
    return 0;
}

/* TextEdit record fields used by the console */
#define TE_SELSTART     0x20
#define TE_SELEND       0x22
#define TE_ACTIVE       0x24
#define TE_LENGTH       0x3c
#define TE_HTEXT        0x3e
#define TE_LINESTARTS   0x60
#define TE_SCRAP_LEN    0xab0       /* low memory: TEScrpLength (word) */
#define TE_SCRAP_HANDLE 0xab4       /* low memory: TEScrpHandle */
#define cInStart        (-0x16e4)   /* long: start of the line being read (stream buffer) */
#define cInEnd          (-0x16e0)   /* long: end of the line being read */
#define cInRoom         (-0x16dc)   /* long: bytes still accepted */
#define cInTEStart      (-0x16d8)   /* long: TE offset where the input line starts */
#define cInTEPos        (-0x16d4)   /* long: TE offset of the input cursor */

/* 000574bc: blank the lines from `line` to the bottom; column 0 */
uint32_t tc__console_clear_from_line(uint32_t line) {
    ENTRY_LINK(FN_tc__console_clear_from_line, 0, 0);
    tc__console_fill_scratch(0x0d00, 0, (uint32_t)((int32_t)G16(cRows) - (int32_t)line));
    maddr te = R32(G32(cTE));
    uint32_t len = (uint32_t)(int32_t)R16(te + TE_LENGTH);
    te = R32(G32(cTE));
    tc__console_te_insert_at((uint32_t)(int32_t)R16(te + TE_LINESTARTS + line * 2), len);
    SG16(cCol, 0);
    return 0;
}

/* 000574fe: delete from the cursor to the end of the line */
uint32_t tc__console_clear_eol_impl(void) {
    ENTRY_NOLINK(FN_tc__console_clear_eol_impl, 16);
    maddr te = tc__console_te_deactivate();
    maddr ls = te + TE_LINESTARTS + (int16_t)G16(cRow) * 2;
    int32_t from = (int32_t)R16(ls) + G16(cCol);
    int32_t to = (int32_t)R16(ls + 2) - 1;
    if (to > from) {
        W16(te + TE_SELSTART, from);
        W16(te + TE_SELEND, to);
        _TEDelete(G32(cTE));
    }
    return 0;
}

/* 0005753a: output n chars: printable runs go to put_text; BEL BS TAB LF VT FF CR are
 * interpreted; a pending Cmd-. in the event queue stops the output */
uint32_t tc__console_write(uint32_t buf, int16_t n) {
    ENTRY_LINK(FN_tc__console_write, 4, 12);
    int16_t d7 = n;
    maddr a4 = buf;
    for (;;) {
        int16_t d0 = d7--;
        if (d0 == 0) break;
        maddr start = a4;
        int ctl;
        for (;;) {                                         /* cmp.b (A4)+ with ' ' / dbhi D7 */
            ctl = 0x20 > R8(a4++);
            if (ctl) break;
            if (--d7 == -1) break;
        }
        if (ctl) a4--;
        if (a4 > start) tc__console_put_text(start, a4 - start);
        if (d7 < 0) break;
        if ((int8_t)G8(cMode) >= 0) {                      /* not RAW: look for Cmd-. */
            for (maddr q = R32(0x14c); q != 0; q = R32(q)) {           /* EventQueue.qHead */
                if (R16(q + 6) == 3 && R8(q + 0xb) == '.' && (R8(q + 0x14) & 1)) {
                    SG8(-0x16f0, 1);
                    return 0;
                }
            }
        }
        switch (R8(a4++)) {
        case 7:                                            /* BEL */
            _SysBeep(4);
            break;
        case 8:                                            /* BS */
            tc__console_te_deactivate();
            if (G16(cCol) != 0) SG16(cCol, G16(cCol) - 1);
            break;
        case 9:                                            /* TAB */
            do {
                SG16(cCol, G16(cCol) + 1);
                if (G16(cTabs) == 0) port_divide_by_zero(__func__);
            } while ((int32_t)G16(cCol) % (int32_t)G16(cTabs) != 0);      /* tc_lmod */
            if (G16(cCol) > G16(cCols)) SG16(cCol, G16(cCols));
            break;
        case 10:                                           /* LF */
            tc__console_newline();
            break;
        case 11:                                           /* VT */
            SG16(cRow, G16(cRow) + 1);
            if (G16(cRow) == G16(cRows)) SG16(cRow, G16(cRow) - 1);
            break;
        case 12:                                           /* FF */
            SG16(cRow, 0);
            tc__console_clear_from_line(0);
            break;
        case 13:                                           /* CR */
            SG16(cCol, 0);
            break;
        default:
            break;
        }
    }
    return 0;
}

/* 00057656: put a run of printable chars at the cursor (overwriting), wrapping lines.
 * It advances its own `buf` argument slot. */
static uint32_t put_text_slot;
uint32_t tc__console_put_text(uint32_t buf, uint32_t n) {
    ENTRY_LINK(FN_tc__console_put_text, 0, 20);
    int32_t d7 = (int32_t)n, d6;
    for (;;) {
        d6 = d7;
        if ((int32_t)G16(cCols) < (int32_t)G16(cCol) + d7) d6 = (int32_t)G16(cCols) - (int32_t)G16(cCol);
        int16_t row = G16(cRow);
        maddr ls = (maddr)((int32_t)row + row + TE_LINESTARTS) + R32(G32(cTE));
        int16_t d5 = (int16_t)(G16(cCol) + R16(ls));
        int16_t d4 = (int16_t)(R16(ls + 2) - 1);
        if (d4 < d5) {                                     /* line too short: pad with spaces */
            tc__console_fill_scratch(0x2000, 0, (uint32_t)((int32_t)d5 - (int32_t)d4));
            tc__console_te_insert_at((uint32_t)(int32_t)d4, (uint32_t)(int32_t)d4);
            d4 = d5;
        } else if ((int32_t)d5 + d6 < (int32_t)d4) {
            d4 = (int16_t)(d6 + d5);
        }
        cpu_set(R_D0, (uint32_t)d6);
        cpu_set(R_A1, R32(TE_SCRAP_HANDLE));
        cpu_set(R_A0, buf);
        tb_trap(0xA9E2);                                   /* PtrToXHand(buf, TEScrpHandle, d6) */
        W16(TE_SCRAP_LEN, d6);
        tc__console_te_insert_at((uint32_t)(int32_t)d5, (uint32_t)(int32_t)d4);
        if (d7 <= d6) break;
        tc__console_newline();
        buf += d6;
        d7 -= d6;
    }
    SG16(cCol, G16(cCol) + d6);
    put_text_slot = buf;
    return 0;
}
void glue_tc__console_put_text(void) {
    maddr sp = emu_sp();
    uint32_t r = tc__console_put_text(R32(sp), R32(sp + 4));
    W32(sp, put_text_slot);
    emu_set_d0(r);
}

/* 00057710: delete [from, to) of the line being typed */
uint32_t tc__console_input_delete(uint32_t from, uint32_t to) {
    ENTRY_LINK(FN_tc__console_input_delete, 0, 12);
    int32_t d7 = (int32_t)from, d6 = (int32_t)to;
    maddr te = tc__console_te_deactivate();
    if ((int32_t)R16(te + TE_LINESTARTS + 2 + (int32_t)G16(cRow) * 2) == (int32_t)G32(cInTEPos) + 1) {
        W16(te + TE_SELSTART, d7);                         /* at the end of the line: just delete */
        W16(te + TE_SELEND, d6);
        _TEDelete(G32(cTE));
    } else {                                               /* shift the rest left, pad with spaces */
        W16(te + TE_SELSTART, d6);
        W16(te + TE_SELEND, G16(cInTEPos + 2));
        _TECopy(G32(cTE));
        tc__console_fill_scratch(0x2000, G32(cInTEPos) - d6, G32(cInTEPos) - d7);
        tc__console_te_insert_at(d7, G32(cInTEPos));
    }
    SG32(cInTEPos, G32(cInTEPos) - (d6 - d7));
    return 0;
}

/* 00057792: insert a typed char at TE offset pos of the line being typed */
uint32_t tc__console_input_insert(int16_t c, uint32_t pos) {
    ENTRY_LINK(FN_tc__console_input_insert, 0, 4);
    W16(a6 + 8, c);                                        /* the char's slot is passed to Munger */
    maddr te = tc__console_te_deactivate();
    W16(te + TE_SELSTART, pos);
    if ((int32_t)R16(te + TE_LINESTARTS + 2 + (int32_t)G16(cRow) * 2) == (int32_t)G32(cInTEPos) + 1) {
        W16(te + TE_SELEND, pos);
        _TEKey((int16_t)(int8_t)((uint16_t)c >> 8), G32(cTE));
    } else {
        W16(te + TE_SELEND, G16(cInTEPos + 2));
        _TECopy(G32(cTE));
        _Munger(R32(TE_SCRAP_HANDLE), 0, 0, 0, a6 + 8, 1);
        W16(TE_SCRAP_LEN, RU16(TE_SCRAP_LEN) + 1);
        tc__console_te_insert_at(pos, G32(cInTEPos) + 1);
    }
    SG32(cInTEPos, G32(cInTEPos) + 1);
    return 0;
}

/* 00057814: TE scrap = `to` bytes; bytes [from, to) set to c */
uint32_t tc__console_fill_scratch(int16_t c, uint32_t from, uint32_t to) {
    ENTRY_LINK(FN_tc__console_fill_scratch, 0, 12);
    uint8_t ch = (uint8_t)((uint16_t)c >> 8);
    maddr h = R32(TE_SCRAP_HANDLE);
    W16(TE_SCRAP_LEN, to);
    _SetHandleSize(h, to);
    maddr p = R32(h) + from;
    for (uint16_t k = (uint16_t)(to - from); k != 0; k--) W8(p++, ch);
    return 0;
}

/* 0005784a: replace TE selection [from, to) with the scrap */
uint32_t tc__console_te_insert_at(uint32_t from, uint32_t to) {
    ENTRY_LINK(FN_tc__console_te_insert_at, 0, 4);
    maddr te = tc__console_te_deactivate();
    W16(te + TE_SELSTART, from);
    W16(te + TE_SELEND, to);
    return _TEPaste(G32(cTE));
}

/* 0005786e: put the caret at column+x of the cursor line (padding with spaces) and
 * activate TextEdit; returns the TE offset */
uint32_t tc__console_te_activate(uint32_t x) {
    ENTRY_LINK(FN_tc__console_te_activate, 0, 16);
    int32_t d7 = (int32_t)x;
    maddr te = tc__console_te_deactivate();
    maddr ls = te + TE_LINESTARTS + (int16_t)G16(cRow) * 2;
    int32_t d6 = (int32_t)R16(ls + 2) - 1;
    d7 += (int32_t)G16(cCol) + (int32_t)R16(ls);
    if (d6 < d7) {
        tc__console_fill_scratch(0x2000, 0, (uint32_t)(d7 - d6));
        tc__console_te_insert_at(d6, d6);
        te = R32(G32(cTE));
    }
    W16(te + TE_SELEND, d7);
    W16(te + TE_SELSTART, d7);
    W16(te + 0x46, 0xff);                                  /* caretState */
    _TEActivate(G32(cTE));
    return (uint32_t)d7;
}

/* 000578de: TEDeactivate if active; returns *TEHandle */
uint32_t tc__console_te_deactivate(void) {
    ENTRY_NOLINK(FN_tc__console_te_deactivate, 0);
    if (R16(R32(G32(cTE)) + TE_ACTIVE) != 0) _TEDeactivate(G32(cTE));
    return R32(G32(cTE));
}

/* 000578f8: strip trailing spaces from every line (keeping the line being typed) */
uint32_t tc__console_trim_text(void) {
    ENTRY_NOLINK(FN_tc__console_trim_text, 20);
    int32_t d7 = G16(cRows);
    maddr te = R32(G32(cTE));
    while (d7 != 0) {
        int32_t d0 = d7--;
        int32_t d5 = (int32_t)R16(te + TE_LINESTARTS + d0 * 2) - 1;
        int32_t d6 = d5;
        maddr a3 = R32(R32(te + TE_HTEXT)) + d6;
        while (d6 != 0 && R8(--a3) == ' ') d6--;
        if ((G8(cMode) & 0x10) && (int8_t)G8(cMode) >= 0 && (int32_t)G16(cRow) == d7 &&
            d6 < (int32_t)G32(cInTEPos))
            d6 = G32(cInTEPos);
        d5 -= d6;
        if (d5 == 0) continue;
        _Munger(R32(te + TE_HTEXT), d6, 0, d5, GA(-0x166c), 0);
        te = R32(G32(cTE));
        if (G8(cMode) & 0x10) {
            if (d6 < (int32_t)G32(cInTEStart)) SG32(cInTEStart, G32(cInTEStart) - d5);
            if (d6 < (int32_t)G32(cInTEPos)) SG32(cInTEPos, G32(cInTEPos) - d5);
        }
        if (d6 < (int32_t)R16(te + TE_SELSTART)) W16(te + TE_SELSTART, R16(te + TE_SELSTART) - d5);
        if (d6 < (int32_t)R16(te + TE_SELEND)) W16(te + TE_SELEND, R16(te + TE_SELEND) - d5);
    }
    return _TECalText(G32(cTE));
}

/* 000579bc: TE rects from the window's portRect (inset 4); destRect holds exactly the rows */
uint32_t tc__console_resize_te(void) {
    ENTRY_LINK(FN_tc__console_resize_te, 8, 0);
    maddr r = a6 - 8;
    W32(r, R32(G32(cWindow) + 0x10));
    W32(r + 4, R32(G32(cWindow) + 0x14));
    _InvalRect(r);
    _InsetRect(r, 4, 4);
    maddr te = R32(G32(cTE));
    W32(te + 8, R32(r));                                   /* viewRect */
    W32(te + 0xc, R32(r + 4));
    uint32_t h = (uint32_t)GU16(cLineHeight) * GU16(cRows);
    W16(r, (uint16_t)(RU16(r + 4) - (uint16_t)h));
    te = R32(G32(cTE));
    W32(te, R32(r));                                       /* destRect */
    W32(te + 4, R32(r + 4));
    return 0;
}

/* 00057a1a: save the port (and the current console window) in save[0..8]; select the
 * console window win and process its pending update */
#define cCurWindow      (-0x16e8)   /* long: console window whose state is in cWinState */
uint32_t tc__console_enter_port(uint32_t win, uint32_t save) {
    ENTRY_LINK(FN_tc__console_enter_port, 8, 4);
    _GetPort(save + 4);
    W32(save, G32(cCurWindow));
    if (win == 0 || R16(win + 0x6c) != G16(cDrvrRef)) return 0;    /* windowKind */
    tc__console_set_window(win);
    _SetPort(win);
    if (_EmptyRgn(R32(win + 0x7a)) == 0) {                 /* updateRgn */
        W32(a6 - 8, R32(win + 0x10));                      /* portRect */
        W32(a6 - 4, R32(win + 0x14));
        _BeginUpdate(win);
        _EraseRect(a6 - 8);
        _TEUpdate(a6 - 8, G32(cTE));
        _EndUpdate(win);
    }
    SG32(cCurWindow, win);
    return win;
}

/* 00057a88: restore what enter_port saved */
uint32_t tc__console_leave_port(uint32_t save) {
    ENTRY_LINK(FN_tc__console_leave_port, 0, 0);
    uint32_t w = R32(save);
    SG32(cCurWindow, w);
    if (w != 0) tc__console_set_window(R32(save));
    return _SetPort(R32(save + 4));
}

/* 00057ab0: swap the 0x2e-byte console state: save the current window's into its refCon
 * handle, load w's */
uint32_t tc__console_set_window(uint32_t w) {
    if (w == G32(cWindow)) return w;
    if (G32(cWindow) != 0) mcopy(R32(R32(G32(cWindow) + 0x98)), GA(cWinState), 0x2e);
    if (w != 0) mcopy(GA(cWinState), R32(R32(w + 0x98)), 0x2e);
    return w;
}

/* 00057afe: HLock the TE scrap; strip bit 7 of its bytes in 7-bit mode; returns *scrap */
uint32_t tc__console_lock_text(void) {
    ENTRY_NOLINK(FN_tc__console_lock_text, 0);
    maddr h = R32(TE_SCRAP_HANDLE);
    cpu_set(R_A0, h);
    tb_trap(0xA029);                                       /* HLock */
    uint32_t p = R32(h);
    if (G8(cMode) & 0x08) {
        maddr a1 = p;
        for (uint16_t n = RU16(TE_SCRAP_LEN); n-- != 0; ) {
            if (R8(a1++) & 0x80) W8(a1 - 1, R8(a1 - 1) & 0x7f);
        }
    }
    return p;
}

/* 00057b26: HUnlock the TE scrap */
uint32_t tc__console_unlock_text(void) {
    ENTRY_NOLINK(FN_tc__console_unlock_text, 0);
    cpu_set(R_A0, R32(TE_SCRAP_HANDLE));
    return os_trap(0xA02A);                                /* HUnlock */
}

/* 00057b2e: end of line: pause while the mouse is down, echo the line to the echo file,
 * scroll when on the last row */
uint32_t tc__console_newline(void) {
    ENTRY_LINK(FN_tc__console_newline, 0x18, 24);
    maddr te = tc__console_te_deactivate();
    if ((G8(cMode) & 0x10) && !(G8(cMode) & 0x20) && !(G8(cMode) & 0x40)) return 0;
    if ((uint8_t)(_GetOSEvent(2, a6 - 0x18) + 1) != 0) {   /* mouseDown: wait for mouseUp */
        while ((uint8_t)(_GetOSEvent(4, a6 - 0x18) + 1) == 0) { }
    }
    if (G32(cEchoFile) != 0) {
        maddr ls = te + TE_LINESTARTS + (int16_t)G16(cRow) * 2;
        W16(te + TE_SELSTART, R16(ls));
        W16(te + TE_SELEND, R16(ls + 2));
        _TECopy(G32(cTE));
        uint32_t f = G32(cEchoFile);
        uint32_t n = (uint32_t)(int32_t)R16(TE_SCRAP_LEN);
        tc_fwrite(tc__console_lock_text(), 1, n, f);
        tc__console_unlock_text();
    }
    SG16(cRow, G16(cRow) + 1);
    if (G16(cRow) == G16(cRows)) {                         /* scroll up one line */
        te = R32(G32(cTE));
        maddr htext = R32(te + TE_HTEXT);
        int16_t d7 = R16(te + TE_LINESTARTS + 2);
        W16(te + TE_LENGTH, R16(te + TE_LENGTH) - d7);
        int16_t d6 = R16(te + TE_LENGTH);
        W16(te + TE_LENGTH, R16(te + TE_LENGTH) + 1);
        W32(a6 - 8, R32(te));
        W32(a6 - 4, R32(te + 4));
        maddr rgn = _NewRgn();
        _ScrollRect(a6 - 8, 0, (int16_t)-G16(cLineHeight), rgn);
        _DisposeRgn(rgn);
        _Munger(htext, 0, 0, (uint32_t)(int32_t)d7, GA(-0x166a), 0);
        _Munger(htext, (uint32_t)(int32_t)d6, 0, 0, GA(-0x1668), 1);
        _TECalText(G32(cTE));
        SG16(cRow, G16(cRow) - 1);
    }
    SG16(cCol, 0);
    return 0;
}

/* 00057c40: install a fake desk accessory driver (open/close = noop, control =
 * tc__console_driver_control) so the console window gets DA-style events; returns its
 * refnum */
uint32_t tc__console_install_driver(void) {
    ENTRY_LINK(FN_tc__console_install_driver, 6, 4);
    if (G32(-0x16d0) == 0) {
        SG32(-0x1740, JT_ADDR(FN_tc__console_driver_control));
        SG32(-0x173a, JT_ADDR(FN_tc__console_driver_noop));
        cpu_set(R_A0, GA(-0x1760));
        cpu_set(R_D0, 0x2a);
        tb_trap(0xA9E3);                                   /* PtrToHand(driver header) */
        SG32(-0x16d0, cpu_get(R_A0));
    }
    W16(a6 - 2, 0x1b);
    maddr a0, d0;
    for (;;) {                                             /* first free (or closed) unit >= 27 */
        a0 = d0 = (maddr)((int32_t)R16(a6 - 2) * 4) + R32(0x11c);      /* UTableBase */
        W32(a6 - 6, R32(a0));
        if (R32(a6 - 6) == 0) break;
        a0 = R32(R32(a6 - 6));
        if (!(R8(a0 + 5) & 0x20)) break;                   /* dCtlFlags: not open */
        W16(a6 - 2, R16(a6 - 2) + 1);
    }
    W16(a6 - 2, ~RU16(a6 - 2));
    cpu_set(R_A0, a0);
    cpu_set(R_D0, (d0 & 0xffff0000) | RU16(a6 - 2));
    tb_trap(0xA13D);                                       /* DrvrInstall */
    maddr dce = R32(cpu_get(R_A0));
    W32(dce, G32(-0x16d0));                                /* dCtlDriver */
    W16(dce + 4, G16(-0x1760));                            /* dCtlFlags = drvrFlags */
    W16(dce + 0x24, G16(-0x175c));                         /* dCtlEMask = drvrEMask */
    return (uint32_t)(int32_t)R16(a6 - 2);
}

/* 00057cc4: driver open/close entry: D0 = -24 */
uint32_t tc__console_driver_noop(void) {
    return 0xffffffe8;
}
void glue_tc__console_driver_noop(void) { emu_set_d0(tc__console_driver_noop()); }

/* 00057cc8: the fake desk accessory's control routine (A0 = param block, A1 = DCE):
 * accEvent (mouse, keys, update), accCursor, accCut/Copy/Paste/Clear */
uint32_t tc__console_driver_control(void) {
    maddr pb = cpu_get(R_A0), dce = cpu_get(R_A1);
    ENTRY_LINK(FN_tc__console_driver_control, 0x1c, 8);
    uint32_t saved_a5 = cpu_get(R_A5);
    cpu_set(R_A5, R32(0x904));                             /* CurrentA5 */
    W32(a6 - 0x1c, saved_a5);
    W32(a6 - 4, dce);
    tc__console_enter_port(_FrontWindow(), a6 - 0xc);
    switch ((uint16_t)(R16(pb + 0x1a) - 0x40)) {          /* csCode */
    case 0: {                                              /* accEvent: csParam = EventRecord* */
        maddr ev = R32(pb + 0x1c);
        switch (RU16(ev)) {
        case 1:                                            /* mouseDown */
            tc__console_mousedown(ev);
            break;
        case 3: case 5:                                    /* keyDown, autoKey */
            W32(a6 - 0x18, R8(ev + 5));
            if (R8(ev + 0xe) & 1) {                        /* cmdKey */
                if (R16(ev) == 5) break;
                W32(a6 - 0x18, tc__console_edit_command(R32(a6 - 0x18)));
            }
            if (R32(a6 - 0x18) != 0) tc__console_key(R32(a6 - 0x18));
            break;
        case 6:                                            /* updateEvt */
            tc__console_enter_port(R32(ev + 2), a6 - 0x14);
            break;
        default:
            break;
        }
        break;
    }
    case 2: tc__console_idle(); break;                     /* accCursor */
    case 6: tc__console_cut(); break;                      /* accCut */
    case 7: tc__console_copy(); break;                     /* accCopy */
    case 8: tc__console_paste(); break;                    /* accPaste */
    case 9: tc__console_key(0x1b); break;                  /* accClear */
    default: break;
    }
    cpu_set(R_A0, G32(-0x16d0));
    tb_trap(0xA02A);                                       /* HUnlock(driver header) */
    cpu_set(R_A0, R32(a6 - 4));
    tb_trap(0xA528);                                       /* RecoverHandleSys(DCE) */
    tb_trap(0xA02A);                                       /* HUnlock */
    tc__console_leave_port(a6 - 0xc);
    cpu_set(R_A5, R32(a6 - 0x1c));
    return 0;
}
void glue_tc__console_driver_control(void) { emu_set_d0(tc__console_driver_control()); }

/* 00057de4: TEIdle; I-beam over the window, arrow elsewhere */
uint32_t tc__console_idle(void) {
    ENTRY_LINK(FN_tc__console_idle, 4, 0);
    _TEIdle(G32(cTE));
    _GetMouse(a6 - 4);
    if (_PtInRect(R32(a6 - 4), G32(cWindow) + 0x10) != 0) return _SetCursor(R32(_GetCursor(1)));   /* iBeamCursor */
    return _SetCursor(R32(g_a5) - 0x6c);                   /* qd.arrow */
}

/* 00057e24: mouse down in the console window */
uint32_t tc__console_mousedown(uint32_t ev) {
    ENTRY_LINK(FN_tc__console_mousedown, 0, 8);
    W16(G32(cWindow) + 0x6c, 8);                           /* windowKind = userKind for FindWindow */
    int32_t part = (int16_t)_FindWindow(R32(ev + 0xa), GA(cWindow));
    W16(G32(cWindow) + 0x6c, G16(cDrvrRef));
    if (part < 0) return 0;
    if (part == 5) {                                       /* inGrow */
        if (RU16(ev + 0xe) & 0x900) tc__console_content_click(ev);   /* cmd/option: click */
        else tc__console_grow(R32(ev + 0xa));
    } else if (part == 3) {                                /* inContent */
        tc__console_content_click(ev);
    } else if (part == 7 || part == 8) {                   /* inZoomIn/Out */
        tc__console_zoom(R32(ev + 0xa), (uint32_t)part);
    }
    return 0;
}

/* 00057ea0: zoom box */
uint32_t tc__console_zoom(uint32_t where, uint32_t part) {
    ENTRY_LINK(FN_tc__console_zoom, 0, 4);
    maddr w = G32(cWindow);
    _InitCursor();
    if (_TrackBox(w, where, (int16_t)part) == 0) return 0;
    _EraseRect(w + 0x10);
    _ZoomWindow(w, (int16_t)part, 0);
    return tc__console_resize_te();
}

/* 00057ed8: grow box, limited to the full console size */
uint32_t tc__console_grow(uint32_t where) {
    ENTRY_LINK(FN_tc__console_grow, 4, 4);
    maddr w = G32(cWindow);
    _InitCursor();
    SG32(-0x172a, G32(cMaxSize));                          /* sizeRect bottom-right */
    W32(a6 - 4, _GrowWindow(w, where, GA(-0x172e)));
    if (R32(a6 - 4) == 0) return 0;
    _EraseRect(w + 0x10);
    _SizeWindow(w, R16(a6 - 2), R16(a6 - 4), 0);
    return tc__console_resize_te();
}

/* 00057f1c: click in the text: TEClick (shift extends), caret kept inside the input line */
uint32_t tc__console_content_click(uint32_t ev) {
    ENTRY_LINK(FN_tc__console_content_click, 0, 12);
    int d7 = 0;
    if (R16(R32(G32(cTE)) + TE_ACTIVE) == 0) tc__console_te_activate(0);
    else if (R8(ev + 0xe) & 2) d7 = 1;                     /* shiftKey */
    tc__console_trim_text();
    _GlobalToLocal(ev + 0xa);
    _TEClick(R32(ev + 0xa), (int16_t)d7, G32(cTE));
    maddr te = R32(G32(cTE));
    if (R16(te + TE_SELSTART) != R16(te + TE_SELEND)) return 0;
    W16(te + 0x46, 0xff);                                  /* caretState */
    if (!(G8(cMode) & 0x10) || (int8_t)G8(cMode) < 0) return _TEDeactivate(G32(cTE));
    if ((int32_t)R16(te + TE_SELSTART) < (int32_t)G32(cInTEStart))
        _TESetSelect(G32(cInTEStart), G32(cInTEStart), G32(cTE));
    else if ((int32_t)R16(te + TE_SELEND) > (int32_t)G32(cInTEPos))
        _TESetSelect(G32(cInTEPos), G32(cInTEPos), G32(cTE));
    return 0;
}

/* 00057fc6: Cmd-key: X/C/V cut/copy/paste, . interrupt, D end of file (4), U/Z kill
 * line (0x15), Q quit; in RAW mode returns the control char */
uint32_t tc__console_edit_command(uint32_t c) {
    ENTRY_LINK(FN_tc__console_edit_command, 0, 0);
    if ((int8_t)G8(cMode) < 0) return c & 0x1f;
    switch (c) {
    case '.':
        if (G8(cInited) != 0) SG8(-0x16f0, 1);
        return 4;
    case 'C': case 'c': tc__console_copy(); return 0;
    case 'D': case 'd': return 4;
    case 'Q': case 'q':
        if (G8(cInited) != 0) { SG16(cWaitKey, 0); tc_exit(0); }
        return 0;
    case 'U': case 'Z': case 'u': case 'z': return 0x15;
    case 'V': case 'v': tc__console_paste(); return 0;
    case 'X': case 'x': tc__console_cut(); return 0;
    default: return 0;
    }
}

/* 000580ca: a typed key while reading: line editing (BS, ^U/ESC kill, arrows 0x1c-0x1f,
 * CR/^C/^D end the line), RAW/CBREAK/ECHO modes */
uint32_t tc__console_key(uint32_t c) {
    ENTRY_LINK(FN_tc__console_key, 0, 20);
    int32_t ch = (int32_t)c;
    maddr te = R32(G32(cTE));
    int32_t d7 = R16(te + TE_SELSTART);
    int32_t d6 = R16(te + TE_SELEND);
    if (!(G8(cMode) & 0x10)) goto beep;
    if ((G8(cMode) & 0x08) && 0x7f < ch) goto beep;
    if ((int8_t)G8(cMode) < 0) {                           /* RAW: take it as is */
        maddr p = G32(cInEnd);
        SG32(cInEnd, p + 1);
        W8(p, (uint8_t)c);
        SG32(cInRoom, 0);
        return 0;
    }
    if (0x20 > ch) {
        switch ((uint32_t)(ch - 3)) {
        case 0: case 1: case 10: {                         /* ^C ^D CR: line done */
            uint16_t n = (uint16_t)(GU16(cInTEPos + 2) - GU16(cInTEStart + 2));
            if (n != 0) {
                maddr src = G32(cInTEStart) + R32(R32(te + TE_HTEXT));
                maddr dst = G32(cInEnd);
                while (n-- != 0) W8(dst++, R8(src++));
                SG32(cInEnd, dst);
            }
            if (ch != 4) {
                maddr p = G32(cInEnd);
                SG32(cInEnd, p + 1);
                W8(p, '\n');
            }
            tc__console_newline();
            SG32(cInRoom, 0);
            return 0;
        }
        case 0x12: case 0x18:                              /* ^U ESC: kill the line */
            SG32(cInRoom, G32(cInRoom) + (G32(cInEnd) - G32(cInStart)));
            SG32(cInEnd, G32(cInStart));
            d7 = G32(cInTEStart);
            d6 = G32(cInTEPos);
            goto printable;
        case 5:                                            /* BS */
            if (G8(cMode) & 0x20) goto printable;
            if (G8(cMode) & 0x40) goto store;
            if (G32(cInEnd) == G32(cInStart)) goto beep;
            SG32(cInEnd, G32(cInEnd) - 1);
            SG32(cInRoom, G32(cInRoom) + 1);
            goto caret;
        case 0x19:                                         /* left */
            if (d6 == d7) d7--;
            goto caret;
        case 0x1a:                                         /* right */
            if (d6 == d7) d6++;
            d7 = d6;
            goto caret;
        case 0x1b:                                         /* up: start of input */
            d7 = G32(cInTEStart);
            goto caret;
        case 6: case 0x1c:                                 /* TAB, down: end of input */
            d7 = G32(cInTEPos);
            goto caret;
        default:
            return 0;
        }
    }
printable:
    if (G8(cMode) & 0x20) {                                /* ECHO: replace the selection */
        if (d6 == d7) {
            if (ch != 8) goto insert;
            d7--;
        }
        if (d7 < (int32_t)G32(cInTEStart)) goto beep;
        if (d6 > (int32_t)G32(cInTEPos)) goto beep;
        tc__console_input_delete(d7, d6);
    }
insert:
    if (0x20 > ch) goto caret;
    if ((int32_t)(G32(cInTEPos) - G32(cInTEStart)) == (int32_t)(G32(cInRoom) - 1)) {
        _SysBeep(2);
        goto caret;
    }
    if (G8(cMode) & 0x20) {
        int32_t at = d7++;
        tc__console_input_insert((int16_t)((uint16_t)(uint8_t)c << 8), at);
        goto caret;
    }
store: {
        maddr p = G32(cInEnd);
        SG32(cInEnd, p + 1);
        W8(p, (uint8_t)c);
        if (G8(cMode) & 0x40) {                            /* CBREAK: echo it and finish */
            tc__console_write(G32(cInEnd) - 1, 1);
            SG32(cInRoom, 0);
            return 0;
        }
        SG32(cInRoom, G32(cInRoom) - 1);
    }
caret:
    if (d7 > (int32_t)G32(cInTEPos)) d7 = G32(cInTEPos);
    else if (d7 < (int32_t)G32(cInTEStart)) d7 = G32(cInTEStart);
    tc__console_te_activate((uint32_t)(d7 - (int32_t)G32(cInTEStart)));
    return 0;
beep:
    _SysBeep(2);
    return 0;
}

/* 000582fe: cut (only inside the line being typed) = copy + backspace */
uint32_t tc__console_cut(void) {
    ENTRY_NOLINK(FN_tc__console_cut, 4);
    maddr te = R32(G32(cTE));
    if (R16(te + TE_ACTIVE) == 0) return 0;
    if (R16(te + TE_SELSTART) >= R16(te + TE_SELEND)) return 0;
    if (!(G8(cMode) & 0x10) || (int32_t)R16(te + TE_SELSTART) < (int32_t)G32(cInTEStart) ||
        (int32_t)R16(te + TE_SELEND) > (int32_t)G32(cInTEPos)) {
        _SysBeep(2);
        return 0;
    }
    tc__console_copy();
    return tc__console_key(8);
}

/* 0005834c: copy the selection to the desk scrap */
uint32_t tc__console_copy(void) {
    ENTRY_NOLINK(FN_tc__console_copy, 4);
    maddr te = R32(G32(cTE));
    if (R16(te + TE_ACTIVE) == 0) return 0;
    if (R16(te + TE_SELSTART) >= R16(te + TE_SELEND)) return 0;
    _TECopy(G32(cTE));
    _ZeroScrap();
    uint32_t len = (uint32_t)(int32_t)R16(TE_SCRAP_LEN);
    _PutScrap(len, 0x54455854, tc__console_lock_text());   /* 'TEXT' */
    return tc__console_unlock_text();
}

/* 0005838e: paste: the desk scrap's text becomes pending keystrokes */
uint32_t tc__console_paste(void) {
    ENTRY_NOLINK(FN_tc__console_paste, 0);
    maddr te = R32(G32(cTE));
    if (!(G8(cMode) & 0x10) || (int32_t)R16(te + TE_SELSTART) < (int32_t)G32(cInTEStart) ||
        (int32_t)R16(R32(G32(cTE)) + TE_SELEND) > (int32_t)G32(cInTEPos)) {
        _SysBeep(2);
        return 0;
    }
    int32_t n = (int32_t)_GetScrap(R32(TE_SCRAP_HANDLE), 0x54455854, GA(cPastePos));
    SG32(cPasteLen, n);
    if (n > 0) {
        SG32(cPaste, R32(TE_SCRAP_HANDLE));
        cpu_set(R_D0, 0);
        tb_trap(0xA122);                                   /* NewHandle(0) */
        W32(TE_SCRAP_HANDLE, cpu_get(R_A0));
        SG32(cPastePos, 0);
    }
    W16(TE_SCRAP_LEN, 0);
    return 0;
}

/* Printing Manager call through _PrGlue: n long arguments, then the selector; `slot` =
 * bytes of the function result (0, 2, 4; 1 = Boolean) */
static uint32_t prglue(uint32_t sel, int slot, int n, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    uint32_t v[5] = { a, b, c, d, e };
    tb_begin();
    if (slot == 4) push32(0); else if (slot) push16(0);
    for (int i = 0; i < n; i++) push32(v[i]);
    push32(sel);
    tb_trap(0xA8FD);
    uint32_t r = slot == 4 ? pop32() : slot == 2 ? pop16() : slot == 1 ? (uint32_t)(pop16() >> 8) : 0;
    tb_end();
    return r;
}
#define PR_ERROR()  prglue(0xba000000, 2, 0, 0, 0, 0, 0, 0)

/* 000583f2: print the echo file */
uint32_t tc__console_print_echo(void) {
    ENTRY_NOLINK(FN_tc__console_print_echo, 0);
    if (G8(-0x16f1) != 0) return 0;                        /* printing cancelled once */
    prglue(0xc8000000, 0, 0, 0, 0, 0, 0, 0);               /* PrOpen */
    if (PR_ERROR() != 0) return 0;
    tc__console_print_file();
    prglue(0xd0000000, 0, 0, 0, 0, 0, 0, 0);               /* PrClose */
    return 0;
}

/* 0005841c: print the echo file, one text line per printer line */
uint32_t tc__console_print_file(void) {
    ENTRY_LINK(FN_tc__console_print_file, 0x22a, 24);
    cpu_set(R_D0, 0x78);
    tb_trap(0xA122);                                       /* NewHandle(sizeof TPrint) */
    maddr hp = cpu_get(R_A0);
    prglue(0x20040480, 0, 1, hp, 0, 0, 0, 0);              /* PrintDefault */
    if (G32(-0x1726) != 0) {
        prglue(0x5804089c, 0, 2, G32(-0x1726), hp, 0, 0, 0);   /* PrJobMerge(old, new) */
        cpu_set(R_A0, G32(-0x1726));
        tb_trap(0xA023);                                   /* DisposeHandle */
    } else {
        _InitCursor();
        if (prglue(0x32040488, 1, 1, hp, 0, 0, 0, 0) == 0) {   /* PrJobDialog */
            SG8(-0x16f1, 1);
            return 0;
        }
    }
    SG32(-0x1726, hp);
    _GetPort(a6 - 4);
    maddr port = prglue(0x04000c00, 4, 3, hp, 0, 0, 0, 0); /* PrOpenDoc */
    maddr te = R32(G32(cTE));
    _TextFont(R16(te + 0x4a));
    _TextSize(R16(te + 0x50));
    _TextFace(R8(te + 0x4c));
    int32_t ascent = R16(te + 0x1a);
    W32(a6 - 0xc, R32(R32(hp) + 8));                       /* prInfo.rPage */
    W32(a6 - 8, R32(R32(hp) + 0xc));
    int32_t lh = G16(cLineHeight);
    if (lh == 0) port_divide_by_zero(__func__);
    int32_t lines = ((int32_t)R16(a6 - 8) - (int32_t)R16(a6 - 0xc)) / lh;   /* tc_ldiv */
    W16(a6 - 0xe, RU16(a6 - 0xa) + 0x24);                  /* left margin */
    tc_rewind(G32(cEchoFile));
    W8(G32(cEchoFile), R8(G32(cEchoFile)) & ~0x40);        /* text mode */
    do {
        prglue(0x10000808, 0, 2, port, 0, 0, 0, 0);        /* PrOpenPage */
        W16(a6 - 0x10, (uint16_t)(RU16(a6 - 0xc) + (uint16_t)ascent));
        for (int32_t n = 0; n < lines; n++) {
            if (tc_fgets(a6 - 0x210, 0x200, G32(cEchoFile)) == 0) break;
            _MoveTo(R16(a6 - 0xe), R16(a6 - 0x10));
            _DrawText(a6 - 0x210, 0, (int16_t)(tc_strlen(a6 - 0x210) - 1));
            W16(a6 - 0x10, RU16(a6 - 0x10) + GU16(cLineHeight));
        }
        prglue(0x1800040c, 0, 1, port, 0, 0, 0, 0);        /* PrClosePage */
    } while (PR_ERROR() == 0 && !(R8(G32(cEchoFile)) & 0x20));
    prglue(0x08000484, 0, 1, port, 0, 0, 0, 0);            /* PrCloseDoc */
    _SetPort(R32(a6 - 4));
    if (R8(R32(hp) + 0x44) == 1 && PR_ERROR() == 0)       /* bJDocLoop = bSpoolLoop */
        prglue(0x60051480, 0, 5, hp, 0, 0, 0, a6 - 0x22a); /* PrPicFile */
    return 0;
}

/* ---- ctype: the class table __ctype[256] is at A5-0x165e (filled from DATA) ---- */
#define gCtype          (-0x165e)
static inline uint32_t ctype_of(uint32_t c) { return (uint32_t)(int32_t)(int8_t)G8(gCtype + (int32_t)(uint8_t)c); }

/* 000585b4 */
uint32_t tc_isalnum(uint32_t c) { return ctype_of(c) & 0xd0; }
/* 000585d4 */
uint32_t tc_isalpha(uint32_t c) { return ctype_of(c) & 0xc0; }
/* 000585f4 */
uint32_t tc_iscntrl(uint32_t c) { return ctype_of(c) & 0x03; }
/* 00058612 */
uint32_t tc_isdigit(uint32_t c) { return ctype_of(c) & 0x10; }
/* 00058630 */
uint32_t tc_isgraph(uint32_t c) { return ctype_of(c) & 0xd8; }
/* 00058650 */
uint32_t tc_isupper(uint32_t c) { return ctype_of(c) & 0x80; }
/* 00058670 */
uint32_t tc_isprint(uint32_t c) { return ctype_of(c) & 0xdc; }
/* 00058690 */
uint32_t tc_ispunct(uint32_t c) { return ctype_of(c) & 0x08; }
/* 000586ae */
uint32_t tc_isspace(uint32_t c) { return ctype_of(c) & 0x06; }
/* 000586cc */
uint32_t tc_islower(uint32_t c) { return ctype_of(c) & 0x40; }
/* 000586ea */
uint32_t tc_isxdigit(uint32_t c) { return ctype_of(c) & 0x30; }

/* 00058708: tolower: c ^ 0x20 when class bit 0x80 */
uint32_t tc_tolower(uint32_t c) {
    return (G8(gCtype + (int32_t)(uint8_t)c) & 0x80) ? c ^ 0x20 : c;
}

/* 0005872e: toupper: c ^ 0x20 when class bit 0x40 */
uint32_t tc_toupper(uint32_t c) {
    return (G8(gCtype + (int32_t)(uint8_t)c) & 0x40) ? c ^ 0x20 : c;
}

/* File Manager parameter block (ioParam) on the stack at pb */
#define PB_RESULT       0x10
#define PB_NAMEPTR      0x12
#define PB_VREFNUM      0x16
#define PB_REFNUM       0x18
#define PB_VERSNUM      0x1a
#define PB_PERMSSN      0x1b
#define PB_MISC         0x1c

/* 00058756: remove(name) */
uint32_t tc_remove(uint32_t name) {
    ENTRY_LINK(FN_tc_remove, 0x132, 0);
    maddr pb = a6 - 0x32;
    W32(pb + PB_NAMEPTR, tc__c2pstr_copy(name, a6 - 0x132));
    W16(pb + PB_VREFNUM, 0);
    W8(pb + PB_VERSNUM, 0);
    cpu_set(R_A0, pb);
    if ((int32_t)(int16_t)os_trap(0xA009) != 0) {          /* PBDelete */
        SG32(gErrno, (int32_t)R16(pb + PB_RESULT));
        return 0xffffffff;
    }
    return 0;
}

/* 00058790: rename(old, new) */
uint32_t tc_rename(uint32_t from, uint32_t to) {
    ENTRY_LINK(FN_tc_rename, 0x232, 0);
    maddr pb = a6 - 0x32;
    W32(pb + PB_NAMEPTR, tc__c2pstr_copy(from, a6 - 0x132));
    W16(pb + PB_VREFNUM, 0);
    W8(pb + PB_VERSNUM, 0);
    W32(pb + PB_MISC, tc__c2pstr_copy(to, a6 - 0x232));   /* ioNewName */
    cpu_set(R_A0, pb);
    if ((int32_t)(int16_t)os_trap(0xA00B) != 0) {          /* PBRename */
        SG32(gErrno, (int32_t)R16(pb + PB_RESULT));
        return 0xffffffff;
    }
    return 0;
}

/* 000587dc: tmpfile(): a unique name, opened "w+b"-like, deleted on close */
uint32_t tc_tmpfile(void) {
    ENTRY_LINK(FN_tc_tmpfile, 0x14, 4);
    if (tc__tmpnam_impl(a6 - 0x14, GA(-0x153e), GA(-0x155a)) == 0) return 0;
    maddr fp = tc_fopen(a6 - 0x14, GA(-0x1536));
    if (fp == 0) return 0;
    W8(fp + FP_FLAGS, R8(fp + FP_FLAGS) | 0x01);           /* temporary */
    return fp;
}

/* 0005881e: tmpnam(buf) (NULL: static buffer; stored in its own argument slot) */
static uint32_t tmpnam_slot;
uint32_t tc_tmpnam(uint32_t buf) {
    ENTRY_LINK(FN_tc_tmpnam, 0, 0);
    if (buf == 0) buf = GA(-0x1556);
    tmpnam_slot = buf;
    return tc__tmpnam_impl(buf, GA(-0x1532), GA(-0x1542));
}
void glue_tc_tmpnam(void) {
    maddr sp = emu_sp();
    uint32_t r = tc_tmpnam(R32(sp));
    W32(sp, tmpnam_slot);
    emu_set_d0(r);
}

/* 00058844: build "<prefix><counter>" names (counter 1..9999, wrapping) until one does
 * not exist (GetFileInfo -> fnfErr); the Pascal name is turned into a C string in place */
uint32_t tc__tmpnam_impl(uint32_t buf, uint32_t prefix, uint32_t counter) {
    ENTRY_LINK(FN_tc__tmpnam_impl, 0x50, 12);
    maddr pb = a6 - 0x50;
    uint32_t start = R32(counter);
    W8(buf, (uint8_t)(tc_strlen(prefix) + 4));
    W32(pb + PB_NAMEPTR, buf);
    W16(pb + PB_VREFNUM, 0);
    W8(pb + PB_VERSNUM, 0);
    W16(pb + PB_MISC, 0);                                  /* ioFDirIndex */
    do {
        W32(counter, R32(counter) + 1);
        if ((int32_t)R32(counter) > 9999) W32(counter, 1);
        if (R32(counter) == start) return 0;
        call_ptr(fn_address(FN_tc_sprintf), "llll", buf + 1, GA(-0x152c), prefix, R32(counter));
        cpu_set(R_A0, pb);
    } while ((int16_t)os_trap(0xA00C) != -43);             /* PBGetFInfo until fnfErr */
    return tc_memmove(buf, buf + 1, (uint32_t)R8(buf) + 1);
}

/* 000588ca: fopen(name, mode) */
uint32_t tc_fopen(uint32_t name, uint32_t mode) {
    ENTRY_LINK(FN_tc_fopen, 0, 0);
    uint32_t fp = tc__find_free_file();
    return tc_freopen(name, mode, fp);
}

/* 000588e4: freopen: "r" (fsRdPerm), "w" (create+truncate), "a" (create+append), with
 * 'b' (binary) and '+' (read/write) */
uint32_t tc_freopen(uint32_t name, uint32_t mode, uint32_t fp) {
    ENTRY_LINK(FN_tc_freopen, 0, 12);
    uint32_t perm, oflags;
    uint8_t m = R8(mode);
    if (m == 'r') { perm = 1; oflags = 0; }
    else if (m == 'w') { perm = 2; oflags = 3; }
    else if (m == 'a') { perm = 2; oflags = 9; }
    else { SG32(gErrno, 0x16); return 0; }
    if (R8(mode + 1) == 'b') {
        oflags |= 0x10;
        if (R8(mode + 2) == '+') perm = 3;
    } else if (R8(mode + 1) == '+') {
        perm = 3;
        if (R8(mode + 2) == 'b') oflags |= 0x10;
    }
    return tc__fopen_impl(name, perm, oflags, fp);
}

/* 00058966: open a disk file stream. oflags: 1 create, 2 truncate, 4 must not exist,
 * 8 append, 0x10 binary */
uint32_t tc__fopen_impl(uint32_t name, uint32_t perm, uint32_t oflags, uint32_t fp) {
    ENTRY_LINK(FN_tc__fopen_impl, 0x132, 8);
    maddr pb = a6 - 0x32;
    uint32_t d7 = oflags;
    if (fp == 0) return 0;
    tc_fclose(fp);
    W32(pb + PB_NAMEPTR, tc__c2pstr_copy(name, a6 - 0x132));
    W16(pb + PB_VREFNUM, 0);
    W8(pb + PB_VERSNUM, 0);
    W8(pb + PB_PERMSSN, (uint8_t)perm);
    W32(pb + PB_MISC, 0);
    if (d7 & 1) {
        cpu_set(R_A0, pb);
        tb_trap(0xA008);                                   /* PBCreate */
        if (R16(pb + PB_RESULT) == 0) d7 &= ~2u;           /* new file: nothing to truncate */
        else if (R16(pb + PB_RESULT) == -48 && !(d7 & 4)) d7 &= ~1u;    /* dupFNErr: exists */
        else { SG32(gErrno, (int32_t)R16(pb + PB_RESULT)); return 0; }
    }
    cpu_set(R_A0, pb);
    tb_trap(0xA000);                                       /* PBOpen */
    if (R16(pb + PB_RESULT) != 0) {
        SG32(gErrno, (int32_t)R16(pb + PB_RESULT));
        if (d7 & 1) { cpu_set(R_A0, pb); tb_trap(0xA009); }   /* PBDelete the new file */
        return 0;
    }
    W16(fp + FP_REFNUM, R16(pb + PB_REFNUM));
    if (d7 & 2) { cpu_set(R_A0, pb); tb_trap(0xA012); }    /* PBSetEOF(0) */
    else if (!(d7 & 1)) { cpu_set(R_A0, pb); tb_trap(0xA011); }   /* PBGetEOF */
    W32(fp + FP_EOF, R32(pb + PB_MISC));
    if (d7 & 8) {
        W8(fp + FP_FLAGS, R8(fp + FP_FLAGS) | 0x02);
        W32(fp + FP_POS, R32(fp + FP_EOF));
    }
    if (d7 & 0x10) W8(fp + FP_FLAGS, R8(fp + FP_FLAGS) | 0x40);
    tc_setvbuf(fp, 0, 0, 0x200);
    W32(fp + FP_PROC, JT_ADDR(FN_tc__file_proc));
    if (d7 & 3) tc__set_file_type(R32(pb + PB_NAMEPTR), d7);
    tc__atexit_slot0(JT_ADDR(FN_tc__close_all_files));
    return fp;
}
