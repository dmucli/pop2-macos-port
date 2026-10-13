/* CODE 22 "Mohawk Libs 4", second half (0x6a70e-0x6ddf6): the rest of the Mohawk
 * resource manager (resRead/resWrite/resUpdate, the resSet* flags, the resQ* queued
 * loads) and the Mohawk timer library (timer*, the Time Manager tasks and the C++
 * TimerLinkList that holds the timers).
 * Ported from the Ghidra decompilation of the original 68k code; addresses in comments
 * are Ghidra addresses (see docs/PORTING.md). The resource manager structures (client
 * record, open file "map", entries) are described in seg22a_mohawklibs4.c.
 *
 * Every res* and timer* routine here is a Pascal function (callee pops, result in the
 * slot above the arguments): in the C prototypes p4 is the LAST Pascal argument.
 *
 * Queued load item (resQRead, a 0x32-byte memory handle):
 *   +0 long next item, +4 long previous item (a circular list per map, at map+0x1e,
 *   sorted by resource (resQCompare); map+0x1c is the word item count)
 *   +8 long resource, +c long proc, +10 long refcon (the proc gets its address),
 *   +14 word done, +16 long buffer (from the proc's message 0), +1a long offset in the
 *   resource, +1e long byte count, +22 long bytes read so far, +26 long file offset of
 *   the resource. resQLoad items also keep the user proc at +2a and its refcon at +2e.
 *   The proc is a Pascal function proc(refcon address, resource, char msg): long, called
 *   through resQCallback as the client that owns the file. Messages: 0 give a buffer,
 *   1 release it, 2 cancelled, 3 done, 4 error.
 * Maps with queued items form a circular list through map+8/map+c; the client record's
 * +10 holds the first one.
 *
 * Timer record (TTimerInfo, 0x2c bytes, an extended TMTask):
 *   +0 qLink, +4 qType, +6 tmAddr (periodicTask/periodicXTask), +a tmCount,
 *   +e tmWakeUp, +12 tmReserved, +16 long proc, +1a long A5, +1e long period
 *   (-microseconds, 0 = one shot), +22 long refcon, +26 long timer id, +2a byte in use,
 *   +2b byte installed. The proc is called as proc(refcon, id, 0) (Pascal, pops 12).
 * TimerLinkItem = TTimerInfo + 0x2c long next. TimerLinkList (gTimerList, 0x12 bytes):
 *   +0 head, +4 tail, +8 current, +c previous of current, +10 word count.
 */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* resource manager globals (seg22a) */
#define gResError       GH(0x107a5e)   /* word: last error (resGetError) */
#define gResClientOff   GH(0x107a62)   /* word: offset of our record in the client bytes */

/* timer globals */
#define gTimerStartTicks GH(0x107c32)  /* long: TickCount at timerInit */
#define gTimerError     GH(0x107c36)   /* word: timerGetError (never set) */
#define gTimerUnused4e  GH(0x107c4e)   /* long: cleared by timerInit */
#define gTimerCurTask   GH(0x107c56)   /* long: timer whose proc is running */
#define gTimerDelay     GH(0x107c5a)   /* byte: delay given to timerCreate inside a proc */
#define gTimerRearmed   GH(0x107c5b)   /* byte: periodic, or re-armed inside its proc */
#define gTimerInProc    GH(0x107c5c)   /* byte: a timer proc is running */
#define gTimerHasXTime  GH(0x107c5d)   /* byte: system 7 (extended Time Manager) */
#define gTimerUseXTime  GH(0x107c5e)   /* byte: last timerCreate used InsXTime */
#define gTimerCalib     GH(0x107c60)   /* long: Time Manager calibration (timerInit) */
#define gTimerInitCount GH(0x107c64)   /* word: timerInit calls */
#define gTimerList      GH(0x107c66)   /* TimerLinkList of all timers */
#define gOsSysVersion   GH(0x107846)   /* word: Gestalt 'sysv' */

/* map fields */
#define MAP_QPREV    0x08
#define MAP_QNEXT    0x0c
#define MAP_CLIENT   0x10
#define MAP_FILE     0x16
#define MAP_RDONLY   0x18
#define MAP_QCOUNT   0x1c
#define MAP_QUEUE    0x1e
#define MAP_EOF      0x22
#define MAP_HOLES    0x2e
#define MAP_DIRTY    0x30
#define MAP_NDIRTY   0x32
#define MAP_COUNT    0x36
/* entry i (1-based) of a map: 32-bit index arithmetic like the original's mulu pair */
#define RES_ENTRY(map, i)  ((maddr)(map) + 0x38 + ((uint32_t)(i) - 1) * 10)
#define ENTRY_SIZE(pe)     ((uint32_t)R8((pe) + 6) << 16 | RU16((pe) + 4))

/* queued item fields */
#define QI_RES     0x08
#define QI_PROC    0x0c
#define QI_REFCON  0x10
#define QI_DONE    0x14
#define QI_BUF     0x16
#define QI_OFFSET  0x1a
#define QI_COUNT   0x1e
#define QI_READ    0x22
#define QI_FILEPOS 0x26
#define QI_UPROC   0x2a
#define QI_UREFCON 0x2e

/* timer fields */
#define TM_ADDR    0x06
#define TM_COUNT   0x0a
#define TM_PROC    0x16
#define TM_A5      0x1a
#define TM_PERIOD  0x1e
#define TM_REFCON  0x22
#define TM_ID      0x26
#define TM_INUSE   0x2a
#define TM_INSTALLED 0x2b
#define TL_NEXT    0x2c

#define ERR_BADRES     0x28d3
#define ERR_RANGE      0x28a0
#define ERR_PROTECTED  0x28a4
#define ERR_READONLY   0x28a5

/* The original sometimes reserves stack (a result slot, arguments of an outer call)
 * before evaluating a nested call; reproduce the SP the inner callee runs at. */
#define NESTED(n, stmt) do { maddr nsp_ = emu_sp(); emu_set_sp(nsp_ - (n)); stmt; emu_set_sp(nsp_); } while (0)

/* A C-convention call whose argument the original leaves on the stack (no addq after the
 * jsr): lower SP like the original until the function returns (ENTRY restores it). */
static void leave_arg(uint32_t v) {
    maddr sp = emu_sp() - 4;
    W32(sp, v);
    emu_set_sp(sp);
}

/* The inlined "map of a resource" sequence at the start of most routines: rh's file
 * (high word) is locked (lock = 1) or translated (lock = 0), its entry (low word, 1-based)
 * found. Sets gResError; returns 0, or ERR_BADRES (and the map is unlocked again) when
 * the file is 0 or the entry 0 or free. */
static uint16_t res_map(uint32_t rh, int lock, maddr *map, maddr *pe) {
    uint16_t maph = (uint16_t)(rh >> 16);
    uint32_t idx = rh & 0xffff;
    if (maph == 0) {
        W16(gResError, ERR_BADRES);
        return ERR_BADRES;
    }
    *map = lock ? memLock((int16_t)maph) : memXlat((int16_t)maph);
    W16(gResError, 0);
    if (idx != 0) {
        *pe = RES_ENTRY(*map, idx);
        if (!(R8(*pe + 7) & 0x10)) return 0;
    }
    if (lock) memUnlock((int16_t)maph);
    W16(gResError, ERR_BADRES);
    return ERR_BADRES;
}

/* Free a memory handle: take it from its owner first, give it back if the free fails
 * (the same inlined sequence as in seg22a; the result is unused here). */
static void free_owned_handle(int16_t mh) {
    int16_t own = S16(memGetOwner(mh));
    if (own == -1) {
        memGetError();
    } else if (S16(memSetOwner(0, mh)) == 0) {
        if (S16(memFreeHandle(mh)) != 0) memSetOwner(own, mh);
    }
}

/* ===================================================================== resources == */

/* 0006a70e: read *countp bytes at offset of resource rh into buf (Pascal resRead(rh, buf,
 * countp, offset)), from its memory handle when loaded, else from the file. A read past
 * the end is cut (*countp is set) and gives 0x28a0. */
uint32_t resRead(uint32_t offset, uint32_t countp, uint32_t buf, uint32_t rh) {
    ENTRY_LINK(FN_resRead, 0x5c, 32);
    uint32_t want = R32(countp);
    W32(countp, 0);
    maddr map = 0, pe = 0;
    uint16_t err = res_map(rh, 1, &map, &pe);
    if (err != 0) return err;
    if (R8(pe + 7) & 2) {
        uint32_t size = memGetHandleSize(R16(pe + 8));
        if (offset + want > size) {
            err = ERR_RANGE;
            W32(countp, offset > size ? 0 : size - offset);
        } else {
            W32(countp, want);
        }
        uint32_t src;
        NESTED(8, src = memXlat(R16(pe + 8)));         /* memMove's slot and buf pushed */
        memMove(R32(countp), src + offset, buf);
    } else {
        uint32_t size = ENTRY_SIZE(pe);
        if (offset + want > size) {
            err = ERR_RANGE;
            W32(countp, offset > size ? 0 : size - offset);
        } else {
            W32(countp, want);
        }
        if (fileSeek(0, R32(pe) + offset, R16(map + MAP_FILE)) == 0xffffffff
            || S16(fileRead(countp, buf, R16(map + MAP_FILE))) != 0)
            err = (uint16_t)fileGetError();
    }
    memUnlock((int16_t)(rh >> 16));
    W16(gResError, err);
    return err;
}

/* 0006a97e: mark resource rh changed (flag != 0) or not; keeps the map's count of dirty
 * entries. Returns the previous state, or -1. */
uint32_t resSetChanged(int16_t flag, uint32_t rh) {
    ENTRY_LINK(FN_resSetChanged, 0x38, 32);
    maddr map = 0, pe = 0;
    if (res_map(rh, 0, &map, &pe) != 0) return 0xffffffff;
    if (R16(map + MAP_RDONLY) != 0) {
        W16(gResError, ERR_READONLY);
        return 0xffffffff;
    }
    if (R8(pe + 7) & 0x40) {
        W16(gResError, ERR_PROTECTED);
        return 0xffffffff;
    }
    uint8_t old = R8(pe + 7);
    if (flag != 0) W8(pe + 7, R8(pe + 7) | 1);
    else W8(pe + 7, R8(pe + 7) & 0xfe);
    if ((R8(pe + 7) ^ old) & 1)
        W16(map + MAP_NDIRTY, (uint16_t)((flag != 0 ? 1 : -1) + R16(map + MAP_NDIRTY)));
    return old & 1;
}

/* 0006ab36: make file the current file (0: the most recently opened); returns the
 * previous current file. */
uint32_t resSetFile(uint32_t file) {
    ENTRY_LINK(FN_resSetFile, 0x10, 28);
    if (file != 0) {
        memXlat((int16_t)file);
        W16(gResError, 0);
    }
    maddr cb = osGetClientBytes(0xffffffff);
    cb += RU16(gResClientOff);
    uint32_t old = R32(cb + 8);
    W32(cb + 8, file != 0 ? file : R32(cb + 0xc));
    W16(gResError, 0);
    return old;
}

/* 0006abe4: set or clear the preload flag (0x20) of rh; returns the previous state or -1 */
uint32_t resSetPreload(int16_t flag, uint32_t rh) {
    ENTRY_LINK(FN_resSetPreload, 0x34, 32);
    maddr map = 0, pe = 0;
    if (res_map(rh, 0, &map, &pe) != 0) return 0xffffffff;
    uint16_t old = (R8(pe + 7) & 0x20) ? 1 : 0;
    if (flag != 0) W8(pe + 7, R8(pe + 7) | 0x20);
    else W8(pe + 7, R8(pe + 7) & 0xdf);
    return old;
}

/* 0006ad24: set or clear the protected flag (0x40) of rh; returns the previous state or -1 */
uint32_t resSetProtected(int16_t flag, uint32_t rh) {
    ENTRY_LINK(FN_resSetProtected, 0x34, 32);
    maddr map = 0, pe = 0;
    if (res_map(rh, 0, &map, &pe) != 0) return 0xffffffff;
    uint16_t old = (R8(pe + 7) & 0x40) ? 1 : 0;
    if (flag != 0) W8(pe + 7, R8(pe + 7) | 0x40);
    else W8(pe + 7, R8(pe + 7) & 0xbf);
    return old;
}

/* 0006ae66: set or clear the purgeable flag (0x80) of rh, and the purge state of its
 * memory handle when it has one. Returns the previous state, or -1. */
uint32_t resSetPurgeable(int16_t flag, uint32_t rh) {
    ENTRY_LINK(FN_resSetPurgeable, 0x34, 32);
    maddr map = 0, pe = 0;
    if (res_map(rh, 0, &map, &pe) != 0) return 0xffffffff;
    if (R8(pe + 7) & 0x40) {
        W16(gResError, ERR_PROTECTED);
        return 0xffffffff;
    }
    uint16_t old = (R8(pe + 7) & 0x80) ? 1 : 0;
    if (flag != 0) W8(pe + 7, R8(pe + 7) | 0x80);
    else W8(pe + 7, R8(pe + 7) & 0x7f);
    if (R8(pe + 7) & 6) memSetPurgeState(flag, R16(pe + 8));   /* the flag as given */
    return old;
}

/* 0006aff0: write a dirty resource back: its loaded data (resWrite) and the map. */
uint32_t resUpdate(uint32_t rh) {
    ENTRY_LINK(FN_resUpdate, 0x48, 32);
    maddr map = 0, pe = 0;
    uint16_t err = res_map(rh, 1, &map, &pe);
    if (err != 0) return err;
    if (R8(pe + 7) & 1) {
        if (R8(pe + 7) & 2) {
            uint32_t size = memGetHandleSize(R16(pe + 8));
            uint32_t p;
            NESTED(6, p = memLock(R16(pe + 8)));       /* resWrite's slot and rh pushed */
            err = (uint16_t)resWrite(0, size, p, rh);
            memUnlock(R16(pe + 8));
            if (err != 0) goto out;
            W16(pe + 4, size);
            W8(pe + 6, size >> 16);
        }
        err = (uint16_t)resMapUpdate(map);
        if (err == 0) {
            W8(pe + 7, R8(pe + 7) & 0xfe);
            W16(map + MAP_NDIRTY, R16(map + MAP_NDIRTY) - 1);
            W16(map + MAP_DIRTY, 0);
        }
    }
out:
    memUnlock((int16_t)(rh >> 16));
    return err;
}

/* 0006b1e8: write all dirty loaded resources of a file and its map back. */
uint32_t resUpdateFile(uint32_t file) {
    ENTRY_LINK(FN_resUpdateFile, 0x24, 32);
    uint16_t err = 0;
    if (file == 0) {
        W16(gResError, ERR_BADRES);
        return ERR_BADRES;
    }
    maddr map = memLock((int16_t)file);
    W16(gResError, 0);
    if (R16(map + MAP_DIRTY) != 0 || R16(map + MAP_NDIRTY) != 0) {
        for (uint32_t i = 1; R16(map + MAP_NDIRTY) != 0 && i <= RU16(map + MAP_COUNT); i++) {
            maddr pe = RES_ENTRY(map, i);
            if (!(R8(pe + 7) & 1) || !(R8(pe + 7) & 2)) continue;
            uint32_t size = memGetHandleSize(R16(pe + 8));
            uint32_t rh = (uint32_t)(uint16_t)file << 16 | (uint16_t)i;
            uint32_t p;
            NESTED(6, p = memLock(R16(pe + 8)));       /* resWrite's slot and rh pushed */
            err = (uint16_t)resWrite(0, size, p, rh);
            memUnlock(R16(pe + 8));
            if (err != 0) break;
            W16(pe + 4, size);
            W8(pe + 6, size >> 16);
            W8(pe + 7, R8(pe + 7) & 0xfe);
            W16(map + MAP_NDIRTY, R16(map + MAP_NDIRTY) - 1);
        }
        if (S16(resMapUpdate(map)) != 0) {
            err = (uint16_t)resGetError();
        } else {
            W16(map + MAP_DIRTY, 0);
            W16(gResError, err);
        }
    }
    memUnlock((int16_t)file);
    return err;
}

/* 0006b3a4: write count bytes from buf at offset of resource rh (Pascal resWrite(rh, buf,
 * count, offset); offset -1 appends). A resource that grows or is written at its end is
 * first moved to the end of the file. The function writes its own argument slots: offset
 * -1 is replaced by the size, and fileWrite gets the address of count. */
uint32_t resWrite(uint32_t offset, uint32_t count, uint32_t buf, uint32_t rh) {
    ENTRY_LINK(FN_resWrite, 0x6c, 32);
    const maddr pOffset = args_ + 0, pCount = args_ + 4;
    W32(pOffset, offset);                      /* (already there when entered from 68k) */
    W32(pCount, count);
    maddr map = 0, pe = 0;
    uint16_t err = res_map(rh, 1, &map, &pe);
    if (err != 0) return err;
    uint32_t pos = R32(pe);                                     /* D6 */
    uint32_t size = ENTRY_SIZE(pe);                             /* D7 */
    if (R32(pOffset) == 0xffffffff) W32(pOffset, size);
    uint32_t end = R32(pOffset) + R32(pCount);
    uint32_t grow = end > size ? end - size : 0;                /* -0x64 */
    if (size + grow > 0xffffff) {
        err = ERR_RANGE;
        W16(gResError, err);
        goto out;
    }
    if (R16(map + MAP_RDONLY) != 0) {
        err = ERR_READONLY;
        W16(gResError, err);
        goto out;
    }
    if (R8(pe + 7) & 0x40) {
        err = ERR_PROTECTED;
        W16(gResError, err);
        goto out;
    }
    if (grow != 0 || R32(pOffset) >= size) {
        /* move the data to the end of the file unless it is already there */
        uint32_t old = R32(pe);
        uint32_t dst = old + size == R32(map + MAP_EOF) ? old : R32(map + MAP_EOF);
        pos = dst;
        uint32_t n = old == dst ? 0 : (size >= R32(pOffset) ? R32(pOffset) : size);
        err = (uint16_t)resCopy(n, old, dst, R16(map + MAP_FILE));
        if (err != 0) goto out;
    }
    if (fileSeek(0, pos + R32(pOffset), R16(map + MAP_FILE)) == 0xffffffff
        || S16(fileWrite(pCount, buf, R16(map + MAP_FILE))) != 0) {
        err = (uint16_t)fileGetError();
        W16(gResError, err);
        goto out;
    }
    if (grow != 0) {
        size += grow;
        W16(pe + 4, size);
        W8(pe + 6, size >> 16);
        W16(map + MAP_DIRTY, 1);
    }
    if (R32(pe) != pos) {
        W32(pe, pos);
        W16(map + MAP_HOLES, 1);
        W16(map + MAP_DIRTY, 1);
    }
    if (pos + size > R32(map + MAP_EOF)) {
        W32(map + MAP_EOF, pos + size);
        W16(map + MAP_DIRTY, 1);
    }
    err = 0;
    W16(gResError, 0);
out:
    memUnlock((int16_t)(rh >> 16));
    return err;
}

/* ============================================================ queued loads (resQ*) == */

/* 0006b6e0: call a queued-load proc as client `client`: proc(refp, rh, msg): long
 * (Pascal resQCallback(refp, rh, msg, proc, client) is reversed in C). msg is a char in
 * the high byte of its word. */
uint32_t resQCallback(uint32_t refp, uint32_t rh, int16_t msg, uint32_t proc, uint32_t client) {
    ENTRY_LINK(FN_resQCallback, 0, 20);
    int16_t inst;
    NESTED(2, inst = S16(osGetClientInstance(client)));        /* osSetInstance's slot */
    int16_t saved = S16(osSetInstance(inst));
    /* subq #4 (result); move.b msg,-(SP); A3; A2; jsr (A4). The move.b writes only the
     * high byte: the low byte is what the stack held there, the low byte of the return
     * address of the osSetInstance call above (0x6b708). */
    maddr sp0 = emu_sp();
    maddr sp = sp0 - 4 - 2 - 8;
    uint8_t lo = (uint8_t)(fn_address(FN_resQCallback) + (0x6b708 - 0x6b6e0));
    W16(sp + 8, (uint16_t)(((uint16_t)msg & 0xff00) | lo));
    W32(sp + 4, rh);
    W32(sp, refp);
    emu_set_sp(sp);
    call68_addr(proc);
    uint32_t r = R32(sp + 10);
    emu_set_sp(sp0);
    osSetInstance(saved);
    return r;
}

/* 0006b74c: cancel every queued load of resource rh. */
uint32_t resQCancel(uint32_t rh) {
    ENTRY_LINK(FN_resQCancel, 0x44, 32);
    maddr map = 0, pe = 0;
    uint16_t err = res_map(rh, 1, &map, &pe);
    if (err != 0) return err;
    if (R8(pe + 7) & 8) {
        uint32_t it;
        while ((it = resQFindItem(0, rh, map)) != 0) resQFreeRef(it);
    }
    W16(gResError, 0);
    memUnlock((int16_t)(rh >> 16));
    W16(gResError, 0);
    return 0;
}

/* 0006b8cc: order of two queued items (Pascal resQCompare(y, x)): -1 when y comes
 * first: smaller resource, then smaller offset, then larger count, then resQLoadProc
 * items; 1 when x comes first; else 0. */
uint32_t resQCompare(uint32_t x, uint32_t y) {
    if (R32(y + QI_RES) < R32(x + QI_RES)) return 0xffffffff;
    if (R32(y + QI_RES) > R32(x + QI_RES)) return 1;
    if (R32(y + QI_OFFSET) < R32(x + QI_OFFSET)) return 0xffffffff;
    if (R32(y + QI_OFFSET) > R32(x + QI_OFFSET)) return 1;
    if (R32(y + QI_COUNT) > R32(x + QI_COUNT)) return 0xffffffff;
    if (R32(y + QI_COUNT) < R32(x + QI_COUNT)) return 1;
    if (R32(y + QI_PROC) == JT_ADDR(FN_resQLoadProc)) return 0xffffffff;
    if (R32(x + QI_PROC) == JT_ADDR(FN_resQLoadProc)) return 1;
    return 0;
}

/* 0006b97c: run the queued loads of the client for at most `budget` ms (-1: all). For
 * each file, the item to run is the one nearest to the file's current position in the
 * sorted circular list (an elevator); it is prefetched, then read with resQExecRef.
 * Returns 1 when loads remain. */
uint32_t resQExec(uint32_t budget) {
    ENTRY_LINK(FN_resQExec, 0x3c, 32);
    maddr cb = osGetClientBytes(0xffffffff);
    cb += RU16(gResClientOff);
    if (budget != 0) {
        uint32_t start = timerGetTime();                     /* D5 */
        uint32_t elapsed = 0;                                /* D6 */
        uint32_t last = 0;                                   /* -0x3c */
        uint32_t file;                                       /* A4 */
        while ((file = R32(cb + 0x10)) != 0) {
            if (budget != 0xffffffff && elapsed >= budget) break;
            maddr map = memXlat((int16_t)file);              /* -0x38 */
            W16(gResError, 0);
            uint32_t h = R32(map + MAP_QUEUE);               /* -0x34 */
            if (file != last && RU16(map + MAP_QCOUNT) > 1) {
                maddr it = memXlat((int16_t)h);              /* -0x30 */
                uint32_t end = R32(it + QI_FILEPOS) + R32(it + QI_READ);   /* D7 */
                uint32_t pos = fileSeek(1 << 8, 0, R16(map + MAP_FILE));  /* -0x2c */
                if (end < pos) {
                    /* forwards while the items end before the position */
                    while (end < pos && R32(it) != R32(map + MAP_QUEUE)) {
                        uint32_t nh = R32(it);
                        maddr nit = memXlat((int16_t)nh);
                        uint32_t nend = R32(nit + QI_FILEPOS) + R32(nit + QI_READ);
                        int wrapped = nend < end;
                        h = nh;
                        it = nit;
                        end = nend;
                        if (wrapped) break;
                    }
                } else {
                    /* backwards while the previous item still ends after the position */
                    while (end > pos && R32(it + 4) != R32(map + MAP_QUEUE)) {
                        uint32_t ph = R32(it + 4);
                        maddr pit = memXlat((int16_t)ph);
                        uint32_t pend = R32(pit + QI_FILEPOS) + R32(pit + QI_READ);
                        if (pend < pos || pend > end) break;
                        h = ph;
                        it = pit;
                        end = pend;
                    }
                }
            }
            W32(map + MAP_QUEUE, h);
            if (S16(resQPrefetch(h)) != 1) break;
            if (S16(resQExecRef(budget == 0xffffffff ? 0xffffffff : budget - elapsed, h)) != 0)
                resQFreeRef(h);
            elapsed = timerGetTime() - start;
            last = file;
        }
    }
    return R32(cb + 0x10) != 0 ? 1 : 0;
}

/* 0006bbca: run queued item `item` for at most `budget` ms (-1: to the end): get a buffer
 * from the proc (message 0), then read in chunks of up to 100 bytes per remaining ms.
 * Returns the item's done flag, or -1. */
uint32_t resQExecRef(uint32_t budget, uint32_t item) {
    ENTRY_LINK(FN_resQExecRef, 0x38, 32);
    const maddr chunk = a6 - 0x30;             /* long: byte count for resRead */
    maddr ip = 0;                                                  /* -0x38 */
    uint16_t err = ERR_BADRES;
    if (item != 0) {
        ip = memLock((int16_t)item);
        if (ip != 0) err = 0;
    }
    W16(gResError, err);
    if (err != 0) return 0xffffffff;
    uint32_t rh = R32(ip + QI_RES);
    uint16_t maph = (uint16_t)(rh >> 16);                          /* A4 */
    maddr map;                                                     /* -0x34 */
    if (maph != 0) {
        map = memLock((int16_t)maph);
        W16(gResError, 0);
    } else {
        map = R32(a6 - 0x34);                  /* left unset by the original */
        W16(gResError, ERR_BADRES);
    }
    uint32_t start = timerGetTime();                               /* D5 */
    uint32_t elapsed = 0;                                          /* D6 */
    if (R16(ip + QI_DONE) != 0) goto done;
    if (R32(ip + QI_BUF) == 0) {
        uint32_t b = resQCallback(ip + QI_REFCON, R32(ip + QI_RES), 0 << 8, R32(ip + QI_PROC), R32(map + MAP_CLIENT));
        W32(ip + QI_BUF, b);
        if (b == 0) {
            W16(ip + QI_DONE, 1);
            goto done;
        }
    }
    for (;;) {
        uint32_t rem = R32(ip + QI_COUNT) - R32(ip + QI_READ);     /* D7 */
        uint32_t n;
        if (budget == 0xffffffff) {
            n = rem;
        } else {
            uint32_t t = (budget - elapsed) * 100;
            n = t >= rem ? rem : t;
        }
        W32(chunk, n);
        if (S16(resRead(R32(ip + QI_OFFSET) + R32(ip + QI_READ), chunk,
                        R32(ip + QI_BUF) + R32(ip + QI_READ), R32(ip + QI_RES))) != 0) {
            W16(ip + QI_DONE, 1);
            resQCallback(ip + QI_REFCON, R32(ip + QI_RES), 1 << 8, R32(ip + QI_PROC), R32(map + MAP_CLIENT));
            W32(ip + QI_BUF, 0);
            resQCallback(ip + QI_REFCON, R32(ip + QI_RES), 4 << 8, R32(ip + QI_PROC), R32(map + MAP_CLIENT));
            goto done;
        }
        W32(ip + QI_READ, R32(ip + QI_READ) + R32(chunk));
        uint16_t fin = R32(ip + QI_READ) == R32(ip + QI_COUNT) ? 1 : 0;
        W16(ip + QI_DONE, fin);
        if (fin != 0) {
            resQCallback(ip + QI_REFCON, R32(ip + QI_RES), 1 << 8, R32(ip + QI_PROC), R32(map + MAP_CLIENT));
            W32(ip + QI_BUF, 0);
            resQCallback(ip + QI_REFCON, R32(ip + QI_RES), 3 << 8, R32(ip + QI_PROC), R32(map + MAP_CLIENT));
        }
        elapsed = timerGetTime() - start;
        if (R16(ip + QI_DONE) != 0) break;
        if (budget != 0xffffffff && budget <= elapsed) break;
    }
done:;
    uint16_t r = RU16(ip + QI_DONE);
    memUnlock((int16_t)maph);
    memUnlock((int16_t)item);
    return r;
}

/* 0006beac: find the queued item of resource rh in map's list (Pascal resQFindItem(map,
 * rh, mode)): mode 0 the first one, else the first one not done. 0 if none. */
uint32_t resQFindItem(int16_t mode, uint32_t rh, uint32_t map) {
    ENTRY_LINK(FN_resQFindItem, 0x10, 20);
    if (R16(map + MAP_QCOUNT) == 0) return 0;
    uint32_t h = R32(map + MAP_QUEUE);                             /* -0x10 */
    maddr p = memXlat((int16_t)h);                                 /* -0xc */
    uint16_t passed = 0;                                           /* D4 */
    for (;;) {
        uint32_t nh;                                               /* -8 */
        maddr np;                                                  /* -4 */
        if (R32(p + QI_RES) < rh) {
            passed = 1;
            nh = R32(p);
            np = memXlat((int16_t)nh);
            if (!(R32(p + QI_RES) <= R32(np + QI_RES))) return 0;
        } else if (R32(p + QI_RES) > rh) {
            nh = R32(p + 4);
            np = memXlat((int16_t)nh);
            if (passed != 0 || R32(p + QI_RES) < R32(np + QI_RES)) return 0;
        } else {
            nh = R32(p + 4);
            np = memXlat((int16_t)nh);
            if (!(R32(np + QI_RES) == rh && S32(resQCompare(p, np)) <= 0 && nh != R32(map + MAP_QUEUE))) {
                /* p is the first item of rh */
                if (mode == 0) return h;
                for (;;) {
                    if (R32(p + QI_RES) != rh) return 0;
                    if (R16(p + QI_DONE) == 0) return h;
                    if (R16(map + MAP_QCOUNT) == 1) return 0;
                    h = R32(p);
                    p = memXlat((int16_t)h);
                }
            }
        }
        h = nh;
        p = np;
        if (h == R32(map + MAP_QUEUE)) return 0;
    }
}

/* 0006c044: run the queued loads of resource rh to the end now. Returns gResError. */
uint32_t resQFlush(uint32_t rh) {
    ENTRY_LINK(FN_resQFlush, 0x40, 32);
    maddr map = 0, pe = 0;
    uint16_t err = res_map(rh, 1, &map, &pe);
    if (err != 0) return err;
    if (R8(pe + 7) & 8) {
        uint32_t it;
        while ((it = resQFindItem(1, rh, map)) != 0) {
            if (S16(resQExecRef(0xffffffff, it)) == -1) goto unlock;
            resQFreeRef(it);
        }
    }
    W16(gResError, 0);
unlock:
    memUnlock((int16_t)(rh >> 16));
    return RU16(gResError);
}

/* 0006c1d8: remove queued item `item`: tell its proc (1 then 2) if it was not done,
 * unlink it (and its map from the client's list when it was the last one), update the
 * entry's queued flag and free it. An item still locked (being run) is left alone. */
uint32_t resQFreeRef(uint32_t item) {
    ENTRY_LINK(FN_resQFreeRef, 0xa0, 32);
    const maddr pPe = a6 - 0x98, pMap = a6 - 0x9c;     /* read even when not set */
    const maddr pPrevMap = a6 - 0x94, pNextMap = a6 - 0x90;
    maddr ip = 0;                                                  /* -0xa0 */
    uint16_t err = ERR_BADRES;
    if (item != 0) {
        ip = memLock((int16_t)item);
        if (ip != 0) err = 0;
    }
    W16(gResError, err);
    if (err != 0) return err;
    uint32_t rh = R32(ip + QI_RES);
    uint16_t maph = (uint16_t)(rh >> 16);
    uint32_t idx = rh & 0xffff;
    if (maph != 0) {
        W32(pMap, memLock((int16_t)maph));
        W16(gResError, 0);
        int bad = 1;
        if (idx != 0) {
            maddr pe = RES_ENTRY(R32(pMap), idx);
            W32(pPe, pe);
            bad = (R8(pe + 7) & 0x10) != 0;
        }
        if (bad) {
            memUnlock((int16_t)maph);
            W16(gResError, ERR_BADRES);
        }
    } else {
        W16(gResError, ERR_BADRES);
    }
    /* the original goes on after these errors */
    if (R16(ip + QI_DONE) == 0) {
        W16(ip + QI_DONE, 1);
        if (R32(ip + QI_BUF) != 0) {
            resQCallback(ip + QI_REFCON, R32(ip + QI_RES), 1 << 8, R32(ip + QI_PROC), R32(R32(pMap) + MAP_CLIENT));
            W32(ip + QI_BUF, 0);
        }
        resQCallback(ip + QI_REFCON, R32(ip + QI_RES), 2 << 8, R32(ip + QI_PROC), R32(R32(pMap) + MAP_CLIENT));
    }
    if ((uint16_t)memGetLockCount((int16_t)item) > 1) {
        memUnlock((int16_t)(R32(ip + QI_RES) >> 16));
        memUnlock((int16_t)item);
        W16(gResError, 0);
        return 0;
    }
    maddr map = R32(pMap);
    W16(map + MAP_QCOUNT, R16(map + MAP_QCOUNT) - 1);
    if (R16(map + MAP_QCOUNT) != 0) {
        uint32_t prev = R32(ip + 4);
        maddr n = memXlat((int16_t)R32(ip));
        W32(n + 4, prev);
        uint32_t next = R32(ip);
        maddr p = memXlat((int16_t)R32(ip + 4));
        W32(p, next);
        if (R32(map + MAP_QUEUE) == item) W32(map + MAP_QUEUE, R32(ip));
    } else {
        W32(map + MAP_QUEUE, 0);
        if (R32(map + MAP_QPREV) != 0) {
            W32(pPrevMap, memXlat((int16_t)R32(map + MAP_QPREV)));
            W16(gResError, 0);
        } else {
            W16(gResError, ERR_BADRES);
        }
        if (R32(map + MAP_QNEXT) != 0) {
            W32(pNextMap, memXlat((int16_t)R32(map + MAP_QNEXT)));
            W16(gResError, 0);
        } else {
            W16(gResError, ERR_BADRES);
        }
        W32(R32(pPrevMap) + MAP_QNEXT, R32(map + MAP_QNEXT));
        W32(R32(pNextMap) + MAP_QPREV, R32(map + MAP_QPREV));
        W32(map + MAP_QPREV, 0);
        W32(map + MAP_QNEXT, 0);
        maddr cb = osGetClientBytes(0xffffffff);
        cb += RU16(gResClientOff);
        if ((R32(ip + QI_RES) >> 16) == R32(cb + 0x10))
            W32(cb + 0x10, R32(R32(pNextMap) + MAP_QPREV));
    }
    if (resQFindItem(0, R32(ip + QI_RES), R32(pMap)) != 0)
        W8(R32(pPe) + 7, R8(R32(pPe) + 7) | 8);
    else
        W8(R32(pPe) + 7, R8(R32(pPe) + 7) & 0xf7);
    memUnlock((int16_t)(R32(ip + QI_RES) >> 16));
    memUnlock((int16_t)item);
    memFreeHandle((int16_t)item);
    W16(gResError, 0);
    return 0;
}

/* 0006c5ea: insert queued item `item` into map's sorted circular list (Pascal
 * resQLink(map, item)); the first item also links the map into the client's list of
 * maps with queued loads. */
uint32_t resQLink(uint32_t item, uint32_t map) {
    ENTRY_LINK(FN_resQLink, 0x3c, 32);
    maddr ip = memXlat((int16_t)item);                             /* -0x3c */
    if (R16(map + MAP_QCOUNT) != 0) {
        uint32_t h = R32(map + MAP_QUEUE);                         /* -0x38 */
        maddr p = memXlat((int16_t)h);                             /* -0x34 */
        maddr np = R32(a6 - 0x30);                                 /* -0x30 */
        if (RU16(map + MAP_QCOUNT) > 1) {
            for (;;) {
                int16_t c = S16(resQCompare(ip, p));
                if (c == -1) {
                    np = memXlat((int16_t)R32(p));
                    h = R32(p);
                    if (S32(resQCompare(p, np)) < 0) break;
                } else if (c == 0) {
                    break;
                } else if (c == 1) {
                    np = memXlat((int16_t)R32(p + 4));
                    if (S32(resQCompare(ip, np)) <= 0) break;
                    if (S32(resQCompare(p, np)) > 0) break;
                    h = R32(p + 4);
                }
                p = np;
                if (h == R32(map + MAP_QUEUE)) break;
            }
        }
        /* insert before h */
        p = memXlat((int16_t)h);
        np = memXlat((int16_t)R32(p + 4));
        W32(np, item);
        W32(ip + 4, R32(p + 4));
        W32(p + 4, item);
        W32(ip, h);
    } else {
        W32(ip, item);
        W32(ip + 4, item);
        W32(map + MAP_QUEUE, item);
        maddr cb = osGetClientBytes(0xffffffff);                   /* -0x2c */
        cb += RU16(gResClientOff);
        uint32_t maph = R32(ip + QI_RES) >> 16;                    /* -0x28 */
        if (R32(cb + 0x10) != 0) {
            maddr first = 0, last = 0;                             /* -0x24, -0x20 */
            first = memXlat((int16_t)R32(cb + 0x10));
            W16(gResError, 0);
            if (R32(first + MAP_QNEXT) != 0) {
                last = memXlat((int16_t)R32(first + MAP_QNEXT));
                W16(gResError, 0);
            } else {
                last = R32(a6 - 0x20);         /* left unset by the original */
                W16(gResError, ERR_BADRES);
            }
            W32(map + MAP_QPREV, R32(last + MAP_QPREV));
            W32(last + MAP_QPREV, maph);
            W32(map + MAP_QNEXT, R32(first + MAP_QNEXT));
            W32(first + MAP_QNEXT, maph);
        } else {
            W32(cb + 0x10, maph);
            W32(map + MAP_QPREV, maph);
            W32(map + MAP_QNEXT, maph);
        }
    }
    W16(map + MAP_QCOUNT, R16(map + MAP_QCOUNT) + 1);
    return 0;
}

/* 0006c85a: load resource rh in the background (Pascal resQLoad(rh, proc, refcon)): the
 * returned queued item reads it into a new handle through resQLoadProc, which calls
 * proc(&refcon, rh, msg) at the end. A loaded resource (or an empty one, loaded at once)
 * calls proc with message 3 (4 on error) and returns 0. */
uint32_t resQLoad(uint32_t refcon, uint32_t proc, uint32_t rh) {
    ENTRY_LINK(FN_resQLoad, 0x54, 32);
    const maddr pRefcon = args_ + 0;           /* its address goes to the proc */
    W32(pRefcon, refcon);
    maddr map = 0, pe = 0;
    if (res_map(rh, 1, &map, &pe) != 0) return 0;
    uint32_t q = 0;                                                /* A3 */
    if (R8(pe + 7) & 2) {
        if (proc != 0) resQCallback(pRefcon, rh, 3 << 8, proc, R32(map + MAP_CLIENT));
        q = 0;
        W16(gResError, 0);
    } else {
        uint32_t size = ENTRY_SIZE(pe);
        if (size == 0) {
            uint16_t ok = (uint16_t)resLoad(rh);
            uint16_t e = RU16(gResError);
            if (proc != 0) resQCallback(pRefcon, rh, (ok != 0 ? 3 : 4) << 8, proc, R32(map + MAP_CLIENT));
            q = 0;
            W16(gResError, e);
        } else {
            q = resQRead(0, size, 0, JT_ADDR(FN_resQLoadProc), rh);
            if (q != 0) {
                maddr p = memXlat((int16_t)q);
                W32(p + QI_UPROC, proc);
                W32(p + QI_UREFCON, R32(pRefcon));
            }
        }
    }
    memUnlock((int16_t)(rh >> 16));
    return q;
}

/* 0006cac4: the proc of resQLoad items (Pascal resQLoadProc(refp, rh, msg)); refp points
 * to the item's +10, where the new memory handle is kept. 0: allocate and lock the handle
 * (returns its address), 1: unlock it, 3: attach it to the resource (or swap it into the
 * purged handle kept by the entry), 2/4: free it. The user proc gets messages 3/4/2. */
uint32_t resQLoadProc(uint32_t refp, uint32_t rh, int16_t msgw) {
    ENTRY_LINK(FN_resQLoadProc, 0x64, 32);
    maddr map = 0, pe = 0;
    if (res_map(rh, 1, &map, &pe) != 0) return 0;
    maddr it = refp - 0x10;                                        /* -0x5c */
    uint32_t r = 0;                                                /* D4 */
    switch ((uint8_t)((uint16_t)msgw >> 8)) {
    case 0:
        if (R8(pe + 7) & 2) {
            if (R32(it + QI_UPROC) != 0)
                resQCallback(it + QI_UREFCON, rh, 3 << 8, R32(it + QI_UPROC), R32(map + MAP_CLIENT));
            r = 0;
        } else {
            uint16_t h = (uint16_t)memAllocHandle(0, ENTRY_SIZE(pe));
            W16(refp, h);
            if (h != 0) r = memLock((int16_t)h);
        }
        break;
    case 1:
        memUnlock(R16(refp));
        break;
    case 3:
        if (R8(pe + 7) & 2) {
            free_owned_handle(R16(refp));
        } else {
            if (R8(pe + 7) & 4) {
                int16_t e = S16(memSwapHandles(R16(pe + 8), R16(refp)));
                free_owned_handle(R16(refp));
                if (e != 0) {
                    if (R32(it + QI_UPROC) != 0)
                        resQCallback(it + QI_UREFCON, rh, 4 << 8, R32(it + QI_UPROC), R32(map + MAP_CLIENT));
                    break;
                }
                W16(refp, R16(pe + 8));
            }
            resAttachHandle(R16(refp), pe);
        }
        if (R32(it + QI_UPROC) != 0)
            resQCallback(it + QI_UREFCON, rh, 3 << 8, R32(it + QI_UPROC), R32(map + MAP_CLIENT));
        break;
    case 2:
    case 4: {
        int16_t h = R16(refp);
        if (h != 0) free_owned_handle(h);
        if (R32(it + QI_UPROC) != 0)
            resQCallback(it + QI_UREFCON, rh, msgw, R32(it + QI_UPROC), R32(map + MAP_CLIENT));
        break;
    }
    default:
        break;
    }
    memUnlock((int16_t)(rh >> 16));
    return r;
}

/* 0006cf08: seek to the unread part of queued item `item` and prefetch it. 1 when the
 * resource is already loaded, -1 on error, else filePrefetch's result. */
uint32_t resQPrefetch(uint32_t item) {
    ENTRY_LINK(FN_resQPrefetch, 0x40, 32);
    maddr ip = 0;                                                  /* -0x38 */
    uint16_t err = ERR_BADRES;
    if (item != 0) {
        ip = memXlat((int16_t)item);
        if (ip != 0) err = 0;
    }
    W16(gResError, err);
    if (err != 0) return 0xffffffff;
    maddr map = 0, pe = 0;
    if (res_map(R32(ip + QI_RES), 0, &map, &pe) != 0) return 0xffffffff;
    if (R8(pe + 7) & 2) return 1;
    uint32_t pos = R32(pe) + R32(ip + QI_READ);                    /* D4 */
    fileSeek(0, pos, R16(map + MAP_FILE));
    uint32_t n = R32(ip + QI_COUNT) - R32(ip + QI_READ);
    return (uint16_t)filePrefetch((int16_t)(n >> 16), (int16_t)n, (int16_t)(pos >> 16), (int16_t)pos,
                                  R16(map + MAP_FILE));
}

/* 0006d0b2: queue a read of count bytes at offset of resource rh through proc (Pascal
 * resQRead(rh, proc, refcon, count, offset)); returns the queued item (a memory handle)
 * or 0. A loaded resource is read at once. */
uint32_t resQRead(uint32_t offset, uint32_t count, uint32_t refcon, uint32_t proc, uint32_t rh) {
    ENTRY_LINK(FN_resQRead, 0x64, 32);
    maddr map = 0, pe = 0;
    if (res_map(rh, 1, &map, &pe) != 0) return 0;
    uint32_t q = 0;                                                /* -0x5c */
    uint32_t size = ENTRY_SIZE(pe);
    if (offset > size || offset + count > size) {
        W16(gResError, ERR_RANGE);
        goto out;
    }
    q = (uint16_t)memAllocHandle(0, 0x32);
    if (q == 0) {
        W16(gResError, memGetError());
        goto out;
    }
    maddr p = memLock((int16_t)q);
    tc_memset(p, 0, 0x32);
    W32(p + QI_RES, rh);
    W32(p + QI_PROC, proc);
    W32(p + QI_REFCON, refcon);
    W32(p + QI_OFFSET, offset);
    W32(p + QI_COUNT, count);
    W32(p + QI_FILEPOS, R32(pe));
    memUnlock((int16_t)q);
    resQLink(q, map);
    W8(pe + 7, R8(pe + 7) | 8);
    if (R8(pe + 7) & 2) {
        resQExecRef(0xffffffff, q);
        resQFreeRef(q);
        q = 0;
    }
out:
    memUnlock((int16_t)(rh >> 16));
    return q;
}

/* ======================================================================== timers == */

/* 0006d32a: create a timer calling proc(refcon, id, 0) after `delay` ms, then every
 * `period` ms (0: once). The long at p16 (p16/p18 in the prototype) is loaded but not
 * used. Inside a timer proc, it re-arms the running timer instead.
 * Returns the timer id, or 0. */
uint32_t timerCreate(uint32_t refcon, uint32_t proc, int16_t period, int16_t delay, int16_t p16, int16_t p18) {
    ENTRY_LINK(FN_timerCreate, 0x34, 20);
    (void)p16; (void)p18;
    const maddr rec = a6 - 0x34;               /* TTimerInfo when no timer is free */
    int isnew = 0;                                                 /* -4 */
    uint32_t us = (uint32_t)(uint16_t)period * 1000u;
    W8(gTimerUseXTime, R8(gTimerHasXTime) != 0 && period != 0 ? 1 : 0);
    if (R8(gTimerInProc) != 0) {
        maddr t = R32(gTimerCurTask);
        if (R32(t + TM_PERIOD) != 0) return 0;
        W32(R32(gTimerCurTask) + TM_PERIOD, (uint32_t)-us);
        W32(R32(gTimerCurTask) + TM_PROC, proc);
        W32(R32(gTimerCurTask) + TM_REFCON, refcon);
        W8(R32(gTimerCurTask) + TM_INUSE, 1);
        W32(R32(gTimerCurTask) + TM_ADDR, R8(gTimerUseXTime) ? JT_ADDR(FN_periodicXTask) : JT_ADDR(FN_periodicTask));
        _PrimeTime(R32(gTimerCurTask), (uint16_t)delay);
        W8(gTimerDelay, (uint8_t)delay);
        W8(gTimerRearmed, 1);
        return R32(R32(gTimerCurTask) + 0x26);
    }
    maddr t = FUN_0006d8e6();                                      /* -8 */
    if (t == 0) {
        t = rec;
        isnew = 1;
    }
    W32(t + 0xe, 0);
    W32(t + 0x12, 0);
    W32(t + TM_COUNT, 0);
    /* move.l A5,D0; movea.l CurrentA5,A5; move.l D0,($1a,A2): A5 stays CurrentA5 */
    W32(t + TM_A5, cpu_get(R_A5));
    cpu_set(R_A5, R32(LM_CurrentA5));
    W32(t + TM_PERIOD, (uint32_t)-us);
    W32(t + TM_PROC, proc);
    W32(t + TM_REFCON, refcon);
    W8(t + TM_INUSE, 1);
    W32(t + TM_ADDR, R8(gTimerUseXTime) ? JT_ADDR(FN_periodicXTask) : JT_ADDR(FN_periodicTask));
    if (isnew) {
        W32(t + TM_ID, FUN_0006d8aa());
        t = AddItem__13TimerLinkListFRC10TTimerInfo(gTimerList, t);
        if (t == 0) return 0;
    }
    if (R8(gTimerUseXTime)) _InsXTime(t);
    else _InsTime(t);
    W8(t + TM_INSTALLED, 1);
    _PrimeTime(t, (uint16_t)delay);
    return R32(t + TM_ID);
}

/* 0006d4fc: stop timer `id` (inside a timer proc it is only marked free). 0x27da when
 * there is no such timer. */
uint32_t timerDestroy(uint32_t id) {
    ENTRY_LINK(FN_timerDestroy, 0x2c, 8);
    const maddr key = a6 - 0x2c;               /* TTimerInfo: only the id is set */
    W32(key + TM_ID, id);
    maddr t = SelectItem__13TimerLinkListFRC10TTimerInfo(gTimerList, key);
    if (t == 0) return 0x27da;
    _RmvTime(t);
    W8(t + TM_INSTALLED, 0);
    if (R8(gTimerInProc) != 0) W8(t + TM_INUSE, 0);
    else RemoveItem__13TimerLinkListFv(gTimerList);
    return 0;
}

/* 0006d568 */
uint32_t timerGetError(void) {
    return RU16(gTimerError);
}

/* 0006d58a: milliseconds (osGetTime) */
uint32_t timerGetTime(void) {
    ENTRY_LINK(FN_timerGetTime, 0, 0);
    return osGetTime();
}

/* 0006d5ae: the first call measures the Time Manager; every call adds 10 free timers to
 * the list. 0x27d8 when one cannot be allocated. */
uint32_t timerInit(void) {
    ENTRY_LINK(FN_timerInit, 0x44, 12);
    const maddr info = a6 - 0x44;              /* TTimerInfo template (+0..+5 not set) */
    const maddr tm = a6 - 0x18;                /* TMTask for the measurement */
    const uint32_t big = 0x80100000;
    if (R16(gTimerInitCount) == 0) {
        W8(gTimerRearmed, 0);
        W8(gTimerInProc, 0);
        W8(gTimerHasXTime, 0);
        W8(gTimerUseXTime, 0);
        tc_memset(tm, 0, 0x16);
        _InsTime(tm);
        _PrimeTime(tm, big);
        _RmvTime(tm);
        W32(gTimerCalib, (uint32_t)-big + R32(tm + TM_COUNT));
    }
    W16(gTimerInitCount, R16(gTimerInitCount) + 1);
    W8(gTimerHasXTime, RU16(gOsSysVersion) >= 0x700 ? 1 : 0);
    W32(gTimerUnused4e, 0);
    W32(gTimerStartTicks, _TickCount());
    W32(info + 0x0e, 0);
    W32(info + 0x12, 0);
    W32(info + TM_COUNT, 0);
    W32(info + TM_A5, 0);
    W32(info + TM_PERIOD, 0);
    W32(info + TM_PROC, 0);
    W32(info + TM_REFCON, 0);
    W8(info + TM_INUSE, 0);
    W8(info + TM_INSTALLED, 0);
    W32(info + TM_ADDR, 0);
    for (int32_t i = 1; i <= 10; i++) {
        W32(info + TM_ID, FUN_0006d8aa());
        if (AddItem__13TimerLinkListFRC10TTimerInfo(gTimerList, info) == 0) return 0x27d8;
    }
    return 0;
}

/* 0006d6b4: 0x100 once timerInit was called */
uint32_t timerQueryInit(void) {
    return R16(gTimerInitCount) != 0 ? 0x100 : 0;
}

/* 0006d6e2: remove every installed timer from the Time Manager. */
uint32_t timerQuit(void) {
    ENTRY_LINK(FN_timerQuit, 0, 8);
    maddr t = SelectFirstItem__13TimerLinkListFv(gTimerList);
    while (t != 0) {
        if (R8(t + TM_INSTALLED)) _RmvTime(t);
        t = SelectNextItem__13TimerLinkListFv(gTimerList);
    }
    return 0;
}

/* The Time Manager tasks (A1 = the timer). They run the proc with the timer's A5, then
 * re-prime a periodic or re-armed timer and destroy a finished one-shot timer. D0 is the
 * timer's A5 (exg D0,A5 on the way out). */

/* 0006d726: extended Time Manager version (drift-free PrimeTime). */
static uint32_t periodicXTask_task(maddr t) {
    ENTRY_LINK(FN_periodicXTask, 4, 12);
    int oneShot = 0;                                               /* -4 */
    uint32_t oldA5 = cpu_get(R_A5);
    uint32_t a5 = R32(t + TM_A5);
    cpu_set(R_A5, a5);
    W8(gTimerInProc, 1);
    W32(gTimerCurTask, t);
    if (R32(t + TM_PERIOD) != 0) {
        W8(gTimerRearmed, 1);
    } else {
        W8(gTimerRearmed, 0);
        oneShot = 1;
    }
    call_ptr(R32(t + TM_PROC), "lll", R32(t + TM_REFCON), R32(t + TM_ID), 0u);
    if (R8(t + TM_INSTALLED)) {
        if (R8(gTimerRearmed)) {
            if (oneShot) _PrimeTime(t, R8(gTimerDelay));
            else _PrimeTime(t, R32(t + TM_PERIOD));
        } else {
            timerDestroy(R32(t + TM_ID));
        }
    }
    W8(gTimerInProc, 0);
    cpu_set(R_A5, oldA5);
    return a5;
}
uint32_t periodicXTask(void) { return periodicXTask_task(cpu_get(R_A1)); }
void glue_periodicXTask(void) { emu_set_d0(periodicXTask_task(cpu_get(R_A1))); }

/* 0006d7d0: classic Time Manager version: measures the proc's run time with its own task
 * and primes the timer for the rest of the period. */
static uint32_t periodicTask_task(maddr t) {
    ENTRY_LINK(FN_periodicTask, 0x18, 20);
    const maddr tm = a6 - 0x18;                /* TMTask: only tmCount is cleared */
    const uint32_t big = 0x83000000;
    int oneShot = 0;                                               /* -2 */
    W32(tm + TM_COUNT, 0);
    _InsTime(tm);
    _PrimeTime(tm, big);
    uint32_t oldA5 = cpu_get(R_A5);
    uint32_t a5 = R32(t + TM_A5);
    cpu_set(R_A5, a5);
    W8(gTimerInProc, 1);
    W32(gTimerCurTask, t);
    if (R32(t + TM_PERIOD) != 0) {
        W8(gTimerRearmed, 1);
    } else {
        W8(gTimerRearmed, 0);
        oneShot = 1;
    }
    call_ptr(R32(t + TM_PROC), "lll", R32(t + TM_REFCON), R32(t + TM_ID), 0u);
    _RmvTime(tm);
    if (R8(t + TM_INSTALLED)) {
        if (R8(gTimerRearmed)) {
            uint32_t used = big - R32(tm + TM_COUNT);
            uint32_t next = oneShot ? (uint32_t)R8(gTimerDelay) - used : R32(t + TM_PERIOD) - used;
            _PrimeTime(t, next);
        } else {
            timerDestroy(R32(t + TM_ID));
        }
    }
    W8(gTimerInProc, 0);
    cpu_set(R_A5, oldA5);
    return a5;
}
uint32_t periodicTask(void) { return periodicTask_task(cpu_get(R_A1)); }
void glue_periodicTask(void) { emu_set_d0(periodicTask_task(cpu_get(R_A1))); }

/* 0006d8aa: the smallest timer id not used by the (id-sorted) list */
uint32_t FUN_0006d8aa(void) {
    ENTRY_NOLINK(FN_FUN_0006d8aa, 8);
    uint32_t n = 0;
    maddr t = SelectFirstItem__13TimerLinkListFv(gTimerList);
    while (t != 0) {
        n++;
        if (R32(t + TM_ID) != n) return n;
        t = SelectNextItem__13TimerLinkListFv(gTimerList);
    }
    return n + 1;
}

/* 0006d8e6: the first free timer of the list, or 0 */
uint32_t FUN_0006d8e6(void) {
    ENTRY_NOLINK(FN_FUN_0006d8e6, 8);
    maddr t = SelectFirstItem__13TimerLinkListFv(gTimerList);
    while (t != 0) {
        if (R8(t + TM_INUSE) == 0) return t;
        t = SelectNextItem__13TimerLinkListFv(gTimerList);
    }
    return 0;
}

/* 0006d918: static constructor of gTimerList */
uint32_t seg22_06d918(void) {
    ENTRY_NOLINK(FN_seg22_06d918, 4);
    return __ct__13TimerLinkListFv(gTimerList);
}

/* 0006d92e: static destructor of gTimerList */
uint32_t seg22_06d92e(void) {
    ENTRY_NOLINK(FN_seg22_06d92e, 4);
    return __dt__13TimerLinkListFv(gTimerList);
}

/* ============================================================== TimerLinkList == */

/* 0006d944: TimerLinkItem(const TTimerInfo &): copy the 0x2c bytes, no next */
uint32_t __ct__13TimerLinkItemFRC10TTimerInfo(uint32_t self, uint32_t info) {
    for (uint32_t i = 0; i < 0x2c; i += 4) W32(self + i, R32(info + i));
    W32(self + TL_NEXT, 0);
    return self;
}

/* 0006d9a6: TimerLinkList(): empty */
uint32_t __ct__13TimerLinkListFv(uint32_t self) {
    W32(self + 0xc, 0);
    W32(self + 8, 0);
    W32(self + 4, 0);
    W32(self, 0);
    W16(self + 0x10, 0);
    return self;
}

/* 0006d9f2: ~TimerLinkList(): delete the items */
uint32_t __dt__13TimerLinkListFv(uint32_t self) {
    ENTRY_LINK(FN___dt__13TimerLinkListFv, 0, 16);
    uint32_t p = R32(self);
    while (p != 0) {
        uint32_t next = R32(p + TL_NEXT);
        tc_operator_delete(p);
        leave_arg(p);                          /* the argument is never popped */
        p = next;
    }
    return 0;
}

/* 0006da44: append a copy of info; it becomes the current item. Returns it, or 0. */
uint32_t AddItem__13TimerLinkListFRC10TTimerInfo(uint32_t self, uint32_t info) {
    ENTRY_LINK(FN_AddItem__13TimerLinkListFRC10TTimerInfo, 8, 16);
    uint32_t n = tc_operator_new(0x30);
    leave_arg(0x30);                           /* operator new's argument stays pushed */
    if (n != 0) __ct__13TimerLinkItemFRC10TTimerInfo(n, info);
    if (n == 0) return 0;
    if (R16(self + 0x10) == 0) {
        W32(self + 8, n);
        W32(self + 4, n);
        W32(self, n);
    } else {
        W32(R32(self + 4) + TL_NEXT, n);
        W32(self + 0xc, R32(self + 4));
        W32(self + 8, n);
        W32(self + 4, n);
    }
    W16(self + 0x10, R16(self + 0x10) + 1);
    return R32(self + 8);
}

/* 0006daf2: insert a copy of info after the current item (append when there is none or
 * it is the last); it becomes the current item. Returns it, or 0. */
uint32_t InsertItem__13TimerLinkListFRC10TTimerInfo(uint32_t self, uint32_t info) {
    ENTRY_LINK(FN_InsertItem__13TimerLinkListFRC10TTimerInfo, 8, 12);
    if (R16(self + 0x10) == 0 || R32(self + 8) == 0 || R32(R32(self + 8) + TL_NEXT) == 0)
        return AddItem__13TimerLinkListFRC10TTimerInfo(self, info);
    uint32_t n = tc_operator_new(0x30);
    leave_arg(0x30);
    if (n != 0) __ct__13TimerLinkItemFRC10TTimerInfo(n, info);
    if (n == 0) return 0;
    W32(n + TL_NEXT, R32(R32(self + 8) + TL_NEXT));
    W32(R32(self + 8) + TL_NEXT, n);
    W32(self + 0xc, R32(self + 8));
    W32(self + 8, n);
    W16(self + 0x10, R16(self + 0x10) + 1);
    return n;
}

/* 0006dbb0: delete the current item; its next becomes current. */
uint32_t RemoveItem__13TimerLinkListFv(uint32_t self) {
    ENTRY_LINK(FN_RemoveItem__13TimerLinkListFv, 0, 12);
    if (R16(self + 0x10) == 0 || R32(self + 8) == 0) return 0;
    uint32_t cur = R32(self + 8);
    if (R32(self + 8) == R32(self + 4)) W32(self + 4, R32(self + 0xc));
    if (R32(self + 8) == R32(self)) W32(self, R32(R32(self + 8) + TL_NEXT));
    W32(self + 8, R32(R32(self + 8) + TL_NEXT));
    if (R32(self + 0xc) != 0) W32(R32(self + 0xc) + TL_NEXT, R32(self + 8));
    tc_operator_delete(cur);
    leave_arg(cur);
    W16(self + 0x10, R16(self + 0x10) - 1);
    return 0;
}

/* 0006dc56: make the item with info's id current (it reads +26 of a null current item
 * before testing for the end, like the original). Returns it, or 0. */
uint32_t SelectItem__13TimerLinkListFRC10TTimerInfo(uint32_t self, uint32_t info) {
    W32(self + 8, R32(self));
    W32(self + 0xc, 0);
    for (;;) {
        uint32_t cur = R32(self + 8);
        int differ = R32(cur + TM_ID) != R32(info + TM_ID);
        if (!differ || R32(self + 8) == 0) break;
        W32(self + 0xc, R32(self + 8));
        W32(self + 8, R32(R32(self + 8) + TL_NEXT));
    }
    return R32(self + 8);
}

/* 0006dcfc: make the first item current */
uint32_t SelectFirstItem__13TimerLinkListFv(uint32_t self) {
    W32(self + 0xc, 0);
    uint32_t first = R32(self);
    W32(self + 8, first);
    return first;
}

/* 0006dd46: make the last item current */
uint32_t SelectLastItem__13TimerLinkListFv(uint32_t self) {
    W32(self + 8, R32(self));
    W32(self + 0xc, 0);
    while (R32(self + 8) != R32(self + 4)) {
        W32(self + 0xc, R32(self + 8));
        W32(self + 8, R32(R32(self + 8) + TL_NEXT));
    }
    return R32(self + 8);
}

/* 0006ddbc: advance the current item */
uint32_t SelectNextItem__13TimerLinkListFv(uint32_t self) {
    if (R32(self + 8) != 0) {
        W32(self + 0xc, R32(self + 8));
        W32(self + 8, R32(R32(self + 8) + TL_NEXT));
    }
    return R32(self + 8);
}
