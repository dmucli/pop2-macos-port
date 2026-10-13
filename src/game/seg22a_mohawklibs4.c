/* CODE 22 "Mohawk Libs 4", first half (0x66854-0x6a6fe): the Mohawk resource manager
 * (res*), which reads the game's 'MHWK'/'RSRC' archive files.
 * Ported from the Ghidra decompilation of the original 68k code; addresses in
 * comments are Ghidra addresses (see docs/PORTING.md).
 *
 * The code was compiled with a Pascal-style compiler: every routine except resQuit and
 * resQsortList is a Pascal function (callee pops, result in the slot above the
 * arguments). The generated glue takes care of that; here they are plain C functions.
 * In the prototypes the parameters are named by stack offset, so the first one (p4) is
 * the LAST argument of the Pascal declaration.
 *
 * Data structures (all big-endian, in the emulated heap):
 *
 *  Per-client record (0x14 bytes, at osGetClientBytes(client) + gResClientOff):
 *    +0 word  initialised        +2 word  memory owner id (memRegisterOwner)
 *    +4 long  previous purge proc (chained by resPurgeProc)
 *    +8 long  current file       +c long  head of the list of open files
 *
 *  An open file ("map", a locked memory handle; the file id is the handle in the low
 *  word of a long):
 *    +0 long next file   +4 long previous file   +10 long client
 *    +14 word open count +16 word file ref        +18 word read-only
 *    +1a word directory handle                    +1e long queued-load list (resQ*)
 *    +22 long file end   +26 long directory offset in the file
 *    +2a long directory + entry list size in the file
 *    +2e word "has holes" (compact on close)      +30 word modified
 *    +34 word free entry list head   +36 word entry count
 *    +38 entries of 10 bytes: +0 long file offset, +4 word size low, +6 byte size high,
 *        +7 byte flags (1 dirty, 2 loaded, 4 purged handle kept, 8 queued, 0x10 free,
 *        0x20 preload, 0x40 protected, 0x80 purgeable), +8 word memory handle (or the
 *        next free entry when free).
 *
 *  A resource is a long: open file id << 16 | entry index (1-based).
 *
 *  The directory (memory handle at map+1a), offsets relative to its start:
 *    +0 word offset of the name pool   +2 word type count
 *    +4 type records of 8 bytes, sorted by type: +0 long type, +4 word id list offset,
 *       +6 word name list offset
 *    id list:   +0 word count, then 4-byte records sorted by id: +0 word id, +2 word index
 *    name list: +0 word count, then 4-byte records sorted by name: +0 word name offset
 *               (in the pool), +2 word index
 *
 *  File header (0x1c bytes at offset 0): 'MHWK', file size - 8, 'RSRC', word version
 *  0x100, word "has holes", long file end, long directory offset, word directory size,
 *  word entry list size (the list is written from map+0x34).
 */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* Globals (Ghidra addresses) */
#define gResError       GH(0x107a5e)   /* word: last error (resGetError) */
#define gResInitCount   GH(0x107a60)   /* word: number of clients that called resInit */
#define gResClientOff   GH(0x107a62)   /* word: offset of our record in the client bytes */
#define gResCopyBuf     GH(0x107a64)   /* word: memory handle used as resCopy's buffer */

/* map fields */
#define MAP_NEXT     0x00
#define MAP_PREV     0x04
#define MAP_CLIENT   0x10
#define MAP_OPEN     0x14
#define MAP_FILE     0x16
#define MAP_RDONLY   0x18
#define MAP_DIR      0x1a
#define MAP_QUEUE    0x1e
#define MAP_EOF      0x22
#define MAP_DIROFF   0x26
#define MAP_DIRSIZE  0x2a
#define MAP_HOLES    0x2e
#define MAP_DIRTY    0x30
#define MAP_FREE     0x34
#define MAP_COUNT    0x36
/* entry i (1-based) of a map: 32-bit index arithmetic like the original's mulu pair */
#define ENTRY(map, i)  ((maddr)(map) + 0x38 + ((uint32_t)(i) - 1) * 10)
/* type record t (0-based) of a directory */
#define TYPEREC(dir, t) ((maddr)(dir) + 4 + (uint32_t)(t) * 8)

#define ERR_NOTFOUND   0x28a2
#define ERR_DUPLICATE  0x28a1
#define ERR_BADFILE    0x28a3
#define ERR_PROTECTED  0x28a4
#define ERR_READONLY   0x28a5
#define ERR_NOTINIT    0x28d2
#define ERR_BADRES     0x28d3
#define ERR_VERSION    0x28d4

/* the per-client record of the current client (osGetClientBytes(-1)) */
static maddr client_rec(void) {
    maddr b = osGetClientBytes(0xffffffff);
    return b + RU16(gResClientOff);
}

/* The original sometimes reserves stack (a result slot, arguments of an outer call)
 * before evaluating a nested call; reproduce the SP the inner callee runs at. */
#define NESTED(n, stmt) do { maddr nsp_ = emu_sp(); emu_set_sp(nsp_ - (n)); stmt; emu_set_sp(nsp_); } while (0)

/* Pascal calls through function pointers. pcall_w_ll: word result, two longs pushed in
 * the order given; pcall_w_w: word result, one word argument. */
static uint16_t pcall_w_ll(maddr fn, uint32_t first, uint32_t second) {
    maddr sp0 = emu_sp(), sp = sp0 - 2 - 8;
    W32(sp + 4, first);
    W32(sp, second);
    emu_set_sp(sp);
    call68_addr(fn);
    uint16_t r = RU16(sp + 8);
    emu_set_sp(sp0);
    return r;
}
static uint16_t pcall_w_w(maddr fn, uint16_t a) {
    maddr sp0 = emu_sp(), sp = sp0 - 2 - 2;
    W16(sp, a);
    emu_set_sp(sp);
    call68_addr(fn);
    uint16_t r = RU16(sp + 2);
    emu_set_sp(sp0);
    return r;
}

/* Free a resource's memory handle: take it from its owner first, give it back if the
 * free fails. Returns the error (the original inlines this sequence everywhere). */
static int16_t free_owned_handle(int16_t mh) {
    int16_t e;
    int16_t own = S16(memGetOwner(mh));
    if (own == -1) {
        e = S16(memGetError());
    } else {
        e = S16(memSetOwner(0, mh));
        if (e == 0) {
            e = S16(memFreeHandle(mh));
            if (e != 0) memSetOwner(own, mh);
        }
    }
    return e;
}

/* 00066854: give memory handle h to entry pe: mark it loaded, attach it to the client
 * and our owner id, purgeable if the entry says so. */
uint32_t resAttachHandle(int16_t h, uint32_t pe) {
    ENTRY_LINK(FN_resAttachHandle, 8, 20);
    W8(pe + 7, R8(pe + 7) | 2);
    W8(pe + 7, R8(pe + 7) & 0xfb);
    W16(pe + 8, h);
    uint32_t client = osGetClient();
    maddr cb = osGetClientBytes(client);
    cb += RU16(gResClientOff);
    memAttachHandle(client, h);
    memSetOwner(R16(cb + 2), h);
    memSetPurgeState((R8(pe + 7) & 0x80) ? 1 : 0, h);
    return 0;
}

/* 000668fc: copy count bytes inside a file from offset src to offset dst (Pascal
 * resCopy(file, dst, src, count)). Uses a 1K stack buffer or the gResCopyBuf handle. */
uint32_t resCopy(uint32_t count, uint32_t src, uint32_t dst, int16_t file) {
    ENTRY_LINK(FN_resCopy, 0x418, 28);
    const maddr stackBuf = a6 - 0x418;
    const maddr chunk = a6 - 8;                /* long: bytes per fileRead/fileWrite */
    uint16_t err = 0;
    if (count != 0) {
        uint32_t bufSize = memGetHandleSize(R16(gResCopyBuf));
        int16_t purge = S16(memPurgeEnable(0));
        uint32_t failProc = memSetFailProc(0);
        maddr buf;
        if (bufSize < count && S16(memSetHandleSize(count, R16(gResCopyBuf))) == 0 && bufSize <= 0x400) {
            buf = stackBuf;
            bufSize = 0x400;
        } else {
            buf = memLock(R16(gResCopyBuf));
            bufSize = memGetHandleSize(R16(gResCopyBuf));
        }
        memSetFailProc(failProc);
        memPurgeEnable(purge);
        uint32_t pos = 0;
        while (count != 0) {
            uint32_t n = count < bufSize ? count : bufSize;
            W32(chunk, n);
            if (fileSeek(0, src + pos, file) == 0xffffffff
                || S16(fileRead(chunk, buf, file)) != 0
                || fileSeek(0, dst + pos, file) == 0xffffffff
                || S16(fileWrite(chunk, buf, file)) != 0) {
                err = (uint16_t)fileGetError();
                break;
            }
            n = R32(chunk);
            pos += n;
            count -= n;
        }
        if (buf != stackBuf) memUnlock(R16(gResCopyBuf));
    }
    W16(gResError, err);
    return err;
}

/* 00066a88: remove resource index from the directory (its id record, its name record
 * and the name, and the type record when its id list becomes empty), then shrink the
 * directory handle. */
uint32_t resDirDelete(int16_t index, int16_t dirH) {
    ENTRY_LINK(FN_resDirDelete, 0x2c, 32);
    maddr dir = memXlat(dirH);
    uint32_t t, i, j, k;
    maddr idl, nl;
    for (t = 0; t < RU16(dir + 2); t++) {
        idl = dir + RU16(TYPEREC(dir, t) + 4);
        for (i = 0; i < RU16(idl); i++)
            if (R16(idl + 2 + i * 4 + 2) == index) goto found;
    }
    W16(gResError, ERR_NOTINIT);
    return 0;
found:;
    uint16_t size = (uint16_t)memGetHandleSize(dirH);       /* -0x18: used size */
    uint16_t off = (uint16_t)((uint16_t)(((uint16_t)i + 1) << 2) + (uint16_t)(idl + 2) - (uint16_t)dir);
    tc_memcpy(idl + 2 + i * 4, idl + 2 + (i + 1) * 4, (uint32_t)size - off);
    size -= 4;
    W16(dir, RU16(dir) - 4);
    W16(idl, RU16(idl) - 1);
    for (j = 0; j < RU16(dir + 2); j++) {
        maddr ej = TYPEREC(dir, j), et = TYPEREC(dir, t);
        if (RU16(ej + 4) > RU16(et + 4)) {
            W16(ej + 4, RU16(ej + 4) - 4);
            W16(ej + 6, RU16(ej + 6) - 4);
        } else if (RU16(ej + 4) == RU16(et + 4)) {
            W16(ej + 6, RU16(ej + 6) - 4);
        }
    }
    /* the name record; the inner loops reuse k and nl like the original */
    nl = dir + RU16(TYPEREC(dir, t) + 6);
    for (k = 0; k < RU16(nl); k++) {
        if (R16(nl + 2 + k * 4 + 2) != index) continue;
        uint16_t noff = RU16(nl + 2 + k * 4);                         /* -0x16 */
        uint16_t len = (uint16_t)(tc_strlen(dir + RU16(dir) + noff) + 1);  /* -0x14 */
        uint32_t pool = RU16(dir);
        maddr p = dir + pool + noff;
        tc_memcpy(p, p + len, (uint32_t)size - (pool + noff + len));
        size -= len;
        uint16_t off2 = (uint16_t)((uint16_t)(((uint16_t)k + 1) << 2) + (uint16_t)(nl + 2) - (uint16_t)dir);
        tc_memcpy(nl + 2 + k * 4, nl + 2 + (k + 1) * 4, (uint32_t)size - off2);
        size -= 4;
        W16(dir, RU16(dir) - 4);
        W16(nl, RU16(nl) - 1);
        for (j = 0; j < RU16(dir + 2); j++) {
            maddr ej = TYPEREC(dir, j), et = TYPEREC(dir, t);
            if (RU16(ej + 4) > RU16(et + 6)) {
                W16(ej + 4, RU16(ej + 4) - 4);
                W16(ej + 6, RU16(ej + 6) - 4);
            }
            nl = dir + RU16(ej + 6);
            for (k = 0; k < RU16(nl); k++)
                if (RU16(nl + 2 + k * 4) > noff) W16(nl + 2 + k * 4, RU16(nl + 2 + k * 4) - len);
        }
    }
    if (R16(dir + RU16(TYPEREC(dir, t) + 4)) == 0) {
        W16(dir, RU16(dir) - 0xc);
        uint16_t idOff = RU16(TYPEREC(dir, t) + 4);                    /* -0x12 */
        maddr p = dir + idOff;
        tc_memcpy(p, p + 4, (uint32_t)size - ((uint32_t)idOff + 4));
        size -= 4;
        W16(dir + 2, RU16(dir + 2) - 1);
        uint16_t off3 = (uint16_t)((uint16_t)(((uint16_t)t + 1) << 3) + 4);
        tc_memcpy(TYPEREC(dir, t), TYPEREC(dir, t + 1), (uint32_t)size - off3);
        size -= 8;
        for (j = 0; j < RU16(dir + 2); j++) {
            maddr ej = TYPEREC(dir, j);
            if (RU16(ej + 4) > idOff) {
                W16(ej + 4, RU16(ej + 4) - 4);
                W16(ej + 6, RU16(ej + 6) - 4);
            }
            W16(ej + 4, RU16(ej + 4) - 8);
            W16(ej + 6, RU16(ej + 6) - 8);
        }
    }
    memSetHandleSize((uint32_t)size, dirH);
    return 0;
}

/* 00066fbe: insert (type, id, name) -> index into the directory, keeping the type, id
 * and name records sorted (Pascal resDirInsert(dirH, index, type, id, name)). Names are
 * cut to 31 characters and compared without case. */
uint32_t resDirInsert(uint32_t name, int16_t id, uint32_t type, int16_t index, int16_t dirH) {
    ENTRY_LINK(FN_resDirInsert, 0x48, 32);
    uint16_t nameLen;                                   /* -0x1c */
    uint16_t err;
    if (name == 0) {
        nameLen = 0;
    } else if (tc_strlen(name) < 0x1f) {
        nameLen = (uint16_t)tc_strlen(name);
    } else {
        nameLen = 0x1f;
    }
    maddr dir = memXlat(dirH);                          /* -0x18 */
    uint32_t lo = 0, hi = RU16(dir + 2) + 1, mid;       /* type search: -0x24 -0x20 -0x28 */
    uint32_t mid2 = 0, mid3 = 0;                        /* -0x40, -0x34 */
    maddr idl = 0, nl = 0;                              /* A4, -0x48 */
    for (;;) {
        mid = (lo + hi) >> 1;
        if (mid == lo) goto insert;
        if (R32(TYPEREC(dir, mid - 1)) < type) { lo = mid; continue; }
        if (R32(TYPEREC(dir, mid - 1)) > type) { hi = mid; continue; }
        break;
    }
    idl = dir + RU16(TYPEREC(dir, mid - 1) + 4);
    {
        uint32_t lo2 = 0, hi2 = RU16(idl) + 1;
        for (;;) {
            mid2 = (lo2 + hi2) >> 1;
            if (mid2 == lo2) break;
            if (RU16(idl + 2 + (mid2 - 1) * 4) < (uint16_t)id) { lo2 = mid2; continue; }
            if (RU16(idl + 2 + (mid2 - 1) * 4) > (uint16_t)id) { hi2 = mid2; continue; }
            err = ERR_DUPLICATE;
            goto out;
        }
    }
    if (nameLen == 0) goto insert;
    nl = dir + RU16(TYPEREC(dir, mid - 1) + 6);
    {
        uint32_t lo3 = 0, hi3 = RU16(nl) + 1;
        for (;;) {
            mid3 = (lo3 + hi3) >> 1;
            if (mid3 == lo3) break;
            maddr s = dir + RU16(dir);
            s += RU16(nl + 2 + (mid3 - 1) * 4);
            int32_t r = S16(memicmp_(s, name, (uint32_t)nameLen + 1));
            if (r < 0) { lo3 = mid3; continue; }
            if (r > 0) { hi3 = mid3; continue; }
            err = ERR_DUPLICATE;
            goto out;
        }
    }
insert:;
    uint16_t used = (uint16_t)memGetHandleSize(dirH);  /* -0x10 */
    {
        uint32_t newSize = (mid == lo ? 0xc : 0) + (uint32_t)used + 4 + (nameLen ? (uint32_t)nameLen + 5 : 0);
        if (S16(memSetHandleSize(newSize, dirH)) != 0) {
            err = (uint16_t)memGetError();
            goto out;
        }
    }
    dir = memXlat(dirH);
    if (mid == lo) {
        /* new type record at position mid */
        uint16_t off = (uint16_t)((uint16_t)((uint16_t)mid << 3) + 4);
        tc_memmove(TYPEREC(dir, mid + 1), TYPEREC(dir, mid), (uint32_t)used - off);
        used += 8;
        W16(dir, RU16(dir) + 8);
        W16(dir + 2, RU16(dir + 2) + 1);
        W32(TYPEREC(dir, mid), type);
        for (uint32_t j = 0; j < RU16(dir + 2); j++) {
            W16(TYPEREC(dir, j) + 4, RU16(TYPEREC(dir, j) + 4) + 8);
            W16(TYPEREC(dir, j) + 6, RU16(TYPEREC(dir, j) + 6) + 8);
        }
        uint32_t pool = RU16(dir);
        tc_memmove(dir + pool + 4, dir + pool, (uint32_t)used - pool);
        used += 4;
        W16(TYPEREC(dir, mid) + 4, RU16(dir));
        W16(TYPEREC(dir, mid) + 6, RU16(dir) + 2);
        W16(dir, RU16(dir) + 4);
        idl = dir + RU16(TYPEREC(dir, mid) + 4);
        W16(idl, 0);
        mid2 = 0;
        nl = dir + RU16(TYPEREC(dir, mid) + 6);
        W16(nl, 0);
        mid3 = 0;
    } else {
        mid -= 1;
        idl = dir + RU16(TYPEREC(dir, mid) + 4);
    }
    /* the id record at position mid2 */
    {
        uint16_t off = (uint16_t)((uint16_t)((uint16_t)mid2 << 2) + (uint16_t)(idl + 2) - (uint16_t)dir);
        tc_memmove(idl + 2 + (mid2 + 1) * 4, idl + 2 + mid2 * 4, (uint32_t)used - off);
    }
    used += 4;
    W16(dir, RU16(dir) + 4);
    W16(idl, RU16(idl) + 1);
    W16(idl + 2 + mid2 * 4, id);
    W16(idl + 2 + mid2 * 4 + 2, index);
    for (uint32_t j = 0; j < RU16(dir + 2); j++) {
        maddr ej = TYPEREC(dir, j), et = TYPEREC(dir, mid);
        if (RU16(ej + 4) > RU16(et + 4)) {
            W16(ej + 4, RU16(ej + 4) + 4);
            W16(ej + 6, RU16(ej + 6) + 4);
        } else if (RU16(ej + 4) == RU16(et + 4)) {
            W16(ej + 6, RU16(ej + 6) + 4);
        }
    }
    if (nameLen != 0) {
        /* the name record at position mid3, the name at the end of the pool */
        nl = dir + RU16(TYPEREC(dir, mid) + 6);
        uint16_t off = (uint16_t)((uint16_t)((uint16_t)mid3 << 2) + (uint16_t)(nl + 2) - (uint16_t)dir);
        tc_memmove(nl + 2 + (mid3 + 1) * 4, nl + 2 + mid3 * 4, (uint32_t)used - off);
        used += 4;
        W16(dir, RU16(dir) + 4);
        W16(nl, RU16(nl) + 1);
        W16(nl + 2 + mid3 * 4 + 2, index);
        W16(nl + 2 + mid3 * 4, (uint16_t)(used - RU16(dir)));
        for (uint32_t j = 0; j < RU16(dir + 2); j++) {
            maddr ej = TYPEREC(dir, j);
            if (RU16(ej + 4) > RU16(TYPEREC(dir, mid) + 6)) {
                W16(ej + 4, RU16(ej + 4) + 4);
                W16(ej + 6, RU16(ej + 6) + 4);
            }
        }
        maddr p = dir + used;
        tc_memcpy(p, name, nameLen);
        W8(p + nameLen, 0);
    }
    err = 0;
out:
    W16(gResError, err);
    return err;
}

/* 00067676: call an enumeration callback (Pascal: cb(ref, arg): word) as the client
 * that registered it (Pascal resEnumCallback(client, cb, ref, arg)). */
uint32_t resEnumCallback(uint32_t arg, uint32_t ref, uint32_t cb, uint32_t client) {
    ENTRY_LINK(FN_resEnumCallback, 0, 24);
    int16_t inst;
    NESTED(2, inst = S16(osGetClientInstance(client)));
    int16_t saved = S16(osSetInstance(inst));
    uint16_t r = pcall_w_ll(cb, ref, arg);
    osSetInstance(saved);
    return r;
}

/* 000676e4: initialise the resource manager for the current client (the first call
 * also sets up the globals and the copy buffer). */
uint32_t resInit(void) {
    ENTRY_LINK(FN_resInit, 4, 28);
    uint16_t err;
    if ((uint16_t)osQueryInit() < 0x100 || (uint16_t)memQueryInit() < 0x100
        || (uint16_t)timerQueryInit() < 0x100 || (uint16_t)fileQueryInit() < 0x100) {
        W16(gResError, ERR_VERSION);
        return ERR_VERSION;
    }
    if (R16(gResInitCount) == 0) {
        tc_memset(gResError, 0, 8);
        uint16_t off = (uint16_t)osReserveClientBytes(0x14);
        W16(gResClientOff, off);
        if (off == 0) {
            err = (uint16_t)osGetError();
            W16(gResError, err);
            return err;
        }
        uint16_t h = (uint16_t)memAllocHandle(1, 0);
        W16(gResCopyBuf, h);
        if (h == 0) {
            err = (uint16_t)memGetError();
            W16(gResError, err);
            return err;
        }
        memSetPurgeState(1, R16(gResCopyBuf));
    }
    maddr rec = client_rec();
    tc_memset(rec, 0, 0x14);
    W32(rec + 4, memSetPurgeProc(JT_ADDR(FN_resPurgeProc)));
    W16(rec + 2, memRegisterOwner());
    if (R16(rec + 2) == 0) {
        err = (uint16_t)memGetError();
        if (R16(gResInitCount) == 0) memFreeHandle(R16(gResCopyBuf));
    } else {
        W16(rec, 1);
        W16(gResInitCount, R16(gResInitCount) + 1);
        err = 0;
    }
    W16(gResError, err);
    return err;
}

/* 00067854: write the directory and the entry list of an open file back (Pascal
 * resMapUpdate(map), map = locked map pointer), at their old place when they fit,
 * else at the end of the file, and rewrite the header. */
uint32_t resMapUpdate(uint32_t map) {
    ENTRY_LINK(FN_resMapUpdate, 0x34, 32);
    const maddr hdr = a6 - 0x2c;               /* 0x1c-byte file header */
    const maddr cnt = a6 - 0xc;                /* long: byte count for fileWrite */
    uint16_t err;
    if (R16(map + MAP_RDONLY) != 0) {
        W16(gResError, ERR_READONLY);
        return ERR_READONLY;
    }
    uint16_t dirSize = (uint16_t)memGetHandleSize(R16(map + MAP_DIR));       /* D4 */
    uint16_t listSize = (uint16_t)(S16(R16(map + MAP_COUNT)) * 10 + 4);     /* D5 */
    uint16_t holes = RU16(map + MAP_HOLES);                                  /* D6 */
    uint32_t end = R32(map + MAP_EOF);                                       /* D7 */
    uint32_t dirOff = R32(map + MAP_DIROFF);                                 /* -0x34 */
    uint32_t total = (uint32_t)dirSize + listSize;                           /* -0x30 */
    if (total > R32(map + MAP_DIRSIZE)) {
        if (R32(map + MAP_DIROFF) + R32(map + MAP_DIRSIZE) != R32(map + MAP_EOF)) {
            dirOff = end;
            holes = 1;
        }
        end = dirOff + total;
    }
    W32(hdr + 0x00, 0x4d48574b);               /* 'MHWK' */
    W32(hdr + 0x04, end - 8);
    W32(hdr + 0x08, 0x52535243);               /* 'RSRC' */
    W16(hdr + 0x0c, 0x100);
    W16(hdr + 0x0e, holes);
    W32(hdr + 0x10, end);
    W32(hdr + 0x14, dirOff);
    W16(hdr + 0x18, dirSize);
    W16(hdr + 0x1a, listSize);
    resXlatChunk(hdr);
    W32(cnt, 0x1c);
    if (S16(fileSetEOF(end, R16(map + MAP_FILE))) != 0
        || fileSeek(0, 0, R16(map + MAP_FILE)) == 0xffffffff
        || S16(fileWrite(cnt, hdr, R16(map + MAP_FILE))) != 0) {
        return (uint16_t)fileGetError();       /* gResError is not set on this path */
    }
    uint32_t oldProc = fileSetXceptProc(0);
    maddr dirPtr = memLock(R16(map + MAP_DIR));                             /* -8 */
    resXlatDir(0, dirPtr);
    resXlatList(0, map + MAP_FREE);
    if (fileSeek(0, dirOff, R16(map + MAP_FILE)) == 0xffffffff) goto ferr;
    W32(cnt, dirSize);
    if (S16(fileWrite(cnt, dirPtr, R16(map + MAP_FILE))) != 0) goto ferr;
    W32(cnt, listSize);
    if (S16(fileWrite(cnt, map + MAP_FREE, R16(map + MAP_FILE))) != 0) goto ferr;
    W16(map + MAP_HOLES, holes);
    W32(map + MAP_EOF, end);
    W32(map + MAP_DIROFF, dirOff);
    W32(map + MAP_DIRSIZE, total);
    err = 0;
    goto done;
ferr:
    err = (uint16_t)fileGetError();
done:
    resXlatList(1, map + MAP_FREE);
    resXlatDir(1, dirPtr);
    memUnlock(R16(map + MAP_DIR));
    fileSetXceptProc(oldProc);
    W16(gResError, err);
    return err;
}

/* 00067aa8: memory manager purge procedure. For a handle of ours, write the resource
 * back if it is dirty and allow the purge (1) unless that failed (0); other handles go
 * to the previous purge procedure. */
uint32_t resPurgeProc(int16_t h) {
    ENTRY_LINK(FN_resPurgeProc, 0x60, 32);
    const maddr pPe = a6 - 0x58;               /* entry pointer */
    const maddr pMap = a6 - 0x5c;              /* locked map */
    uint32_t client = osGetClient();
    maddr cb = osGetClientBytes(client);
    cb += RU16(gResClientOff);
    if (S16(memGetOwner(h)) == R16(cb + 2)) {
        uint32_t rh = resFindHandle(h);
        if (rh != 0) {
            uint16_t maph = (uint16_t)(rh >> 16);
            uint32_t idx = rh & 0xffff;
            uint16_t err;
            if (maph != 0) {
                W32(pMap, memLock(maph));
                err = 0;
                W16(gResError, 0);
            } else {
                err = ERR_BADRES;
                W16(gResError, ERR_BADRES);
            }
            if (err == 0) {
                int bad = 1;
                if (idx != 0) {
                    maddr pe = ENTRY(R32(pMap), idx);
                    W32(pPe, pe);
                    bad = (R8(pe + 7) & 0x10) != 0;
                }
                if (bad) {
                    memUnlock(maph);
                    W16(gResError, ERR_BADRES);
                }
            }
            /* the original goes on even after the errors above */
            if (R8(R32(pPe) + 7) & 1) {
                if (S16(resUpdate(rh)) != 0) {
                    memUnlock((uint16_t)(rh >> 16));
                    return 0;
                }
            }
            W8(R32(pPe) + 7, R8(R32(pPe) + 7) & 0xfd);
            W8(R32(pPe) + 7, R8(R32(pPe) + 7) | 4);
            memUnlock((uint16_t)(rh >> 16));
            return 1;
        }
    }
    if (R32(cb + 4) == 0) return 1;
    int16_t inst;
    NESTED(2, inst = S16(osGetClientInstance(client)));
    int16_t saved = S16(osSetInstance(inst));
    uint16_t r = pcall_w_w(R32(cb + 4), (uint16_t)h);
    osSetInstance(saved);
    return r;
}

/* 00067cda: 0x100 when the current client called resInit, else 0. */
uint32_t resQueryInit(void) {
    ENTRY_LINK(FN_resQueryInit, 0, 4);
    if (R16(gResInitCount) != 0) {
        maddr rec = client_rec();
        if (R16(rec) != 0) return 0x100;
    }
    return 0;
}

/* 00067d26: close the client's files and undo resInit (registered with atexit). */
uint32_t resQuit(void) {
    ENTRY_LINK(FN_resQuit, 0, 20);
    if (S16(resQueryInit()) == 0) {
        W16(gResError, ERR_NOTINIT);
        return 0;
    }
    maddr rec = client_rec();
    while (R32(rec + 0xc) != 0) {
        W16(gResError, ERR_NOTINIT);
        resCloseFile(0, R32(rec + 0xc));
    }
    memSetPurgeProc(R32(rec + 4));
    W16(rec, 0);
    W16(gResInitCount, R16(gResInitCount) - 1);
    if (R16(gResInitCount) == 0) memFreeHandle(R16(gResCopyBuf));
    return 0;
}

/* 00067db6: byte-order translation of the file header: nothing to do on the Mac. */
uint32_t resXlatChunk(uint32_t hdr) {
    (void)hdr;
    return 0;
}

/* 00067dda: byte-order translation of a directory (toFile 0 / fromFile 1). On the Mac
 * every field is rewritten with its own value. */
uint32_t resXlatDir(int16_t dir_, uint32_t dir) {
    (void)dir_;
    uint16_t types = RU16(dir + 2);
    W16(dir, RU16(dir));
    W16(dir + 2, RU16(dir + 2));
    for (uint16_t t = 0; t < types; t++) {
        maddr rec = TYPEREC(dir, t);
        maddr idl = dir + RU16(rec + 4);
        maddr nl = dir + RU16(rec + 6);
        W32(rec, R32(rec));
        W16(rec + 4, RU16(rec + 4));
        W16(rec + 6, RU16(rec + 6));
        uint16_t n = RU16(idl);
        W16(idl, RU16(idl));
        for (uint16_t i = 0; i < n; i++) {
            maddr r = idl + 2 + (uint32_t)i * 4;
            W16(r, RU16(r));
            W16(r + 2, RU16(r + 2));
        }
        n = RU16(nl);
        W16(nl, RU16(nl));
        for (uint16_t i = 0; i < n; i++) {
            maddr r = nl + 2 + (uint32_t)i * 4;
            W16(r, RU16(r));
            W16(r + 2, RU16(r + 2));
        }
    }
    return 0;
}

/* 00067f7a: byte-order translation of an entry list (map + 0x34): identity on the Mac. */
uint32_t resXlatList(int16_t dir_, uint32_t list) {
    (void)dir_;
    uint16_t n = RU16(list + 2);
    W16(list, RU16(list));
    W16(list + 2, RU16(list + 2));
    for (uint16_t i = 0; i < n; i++) {
        maddr e = list + 4 + (uint32_t)i * 10;
        W32(e, R32(e));
        W16(e + 4, RU16(e + 4));
        W16(e + 8, RU16(e + 8));
    }
    return 0;
}

/* 00068020: detach the memory handle of resource rh (returned, 0 if none) and attach
 * newH instead when it is not 0 (Pascal resAttach(rh, newH)). -1 on error. */
uint32_t resAttach(int16_t newH, uint32_t rh) {
    ENTRY_LINK(FN_resAttach, 0x54, 32);
    uint16_t maph = (uint16_t)(rh >> 16);
    uint32_t idx = rh & 0xffff;
    maddr map = 0, pe = 0;
    if (maph == 0) {
        W16(gResError, ERR_BADRES);
        return 0xffffffff;
    }
    map = memLock(maph);
    W16(gResError, 0);
    if (idx == 0 || (pe = ENTRY(map, idx), R8(pe + 7) & 0x10)) {
        memUnlock(maph);
        W16(gResError, ERR_BADRES);
        return 0xffffffff;
    }
    if (R8(pe + 7) & 0x40) {
        memUnlock((uint16_t)(rh >> 16));
        W16(gResError, ERR_PROTECTED);
        return 0xffffffff;
    }
    if (R8(pe + 7) & 8) resQCancel(rh);
    uint16_t old = (R8(pe + 7) & 6) ? RU16(pe + 8) : 0;
    if (old != 0) {
        W16(pe + 8, 0);
        W8(pe + 7, R8(pe + 7) & 0xf9);
        memSetOwner(0, old);
    }
    if (newH != 0) resAttachHandle(newH, pe);
    memUnlock((uint16_t)(rh >> 16));
    return old;
}

/* 00068220: close an open file (Pascal resCloseFile(file, compact)). The last close
 * writes the dirty resources and the map back, frees the loaded resources, compacts
 * the file when asked and it has holes, closes it and unlinks the map. */
uint32_t resCloseFile(int16_t compact, uint32_t file) {
    ENTRY_LINK(FN_resCloseFile, 0x50, 32);
    uint16_t fh = (uint16_t)file;
    int16_t err;                                               /* D5 */
    if (file == 0) {
        W16(gResError, ERR_BADRES);
        return ERR_BADRES;
    }
    maddr map = memLock(fh);                                   /* -0x50 */
    W16(gResError, 0);
    W16(map + MAP_OPEN, R16(map + MAP_OPEN) - 1);
    if (R16(map + MAP_OPEN) != 0) {
        memUnlock(fh);
        W16(gResError, 0);
        return 0;
    }
    err = S16(resUpdateFile(file));
    if (err != 0) goto fail;
    for (uint16_t i = 1; i <= RU16(map + MAP_COUNT); i++) {
        maddr pe = ENTRY(map, i);
        if ((R8(pe + 7) & 0x10) || !(R8(pe + 7) & 6)) continue;
        for (;;) {
            err = free_owned_handle(R16(pe + 8));
            if (err != 0x27a8) break;
            memSetLockCount(0, R16(pe + 8));               /* locked: unlock and retry */
        }
        if (err != 0) {
            W16(gResError, err);
            goto fail;
        }
        W8(pe + 7, R8(pe + 7) & 0xf9);
    }
    while (R32(map + MAP_QUEUE) != 0) resQFreeRef(R32(map + MAP_QUEUE));
    if (compact != 0 && R16(map + MAP_HOLES) != 0) {
        uint16_t nfree = 0;
        for (uint16_t f = RU16(map + MAP_FREE); f != 0; f = RU16(ENTRY(map, f) + 8)) nfree++;
        tc_qsort(map + 0x38, RU16(map + MAP_COUNT), 10, JT_ADDR(FN_resQsortList));
        W16(map + MAP_COUNT, R16(map + MAP_COUNT) - nfree);
        W16(map + MAP_FREE, 0);
        uint32_t pos = 0x1c;                                    /* -0x44 */
        for (uint16_t i = 1; i <= RU16(map + MAP_COUNT); i++) {
            maddr pe = ENTRY(map, i);                           /* -0x40 */
            if (pos < R32(pe)) {
                uint32_t size = (uint32_t)R8(pe + 6) << 16 | RU16(pe + 4);
                err = S16(resCopy(size, R32(pe), pos, R16(map + MAP_FILE)));
                if (err != 0) break;
                W32(pe, pos);
                pos += size;
            }
        }
        if (err == 0) {
            W32(map + MAP_EOF, pos);
            W16(map + MAP_HOLES, 0);
        }
        W16(map + MAP_DIRTY, 1);
        W32(map + MAP_DIRSIZE, 0);
        resMapUpdate(map);
        if (err != 0) goto fail;
        err = S16(resGetError());
        if (err != 0) goto fail;
    }
    err = S16(fileClose(R16(map + MAP_FILE)));
    if (err != 0) {
        W16(gResError, err);
        goto fail;
    }
    maddr rec = client_rec();
    if (R32(map + MAP_PREV) != 0) {
        uint32_t next = R32(map + MAP_NEXT);
        maddr prev = memXlat((uint16_t)R32(map + MAP_PREV));
        W32(prev + MAP_NEXT, next);
    }
    if (R32(map + MAP_NEXT) != 0) {
        uint32_t prevh = R32(map + MAP_PREV);
        maddr next = memXlat((uint16_t)R32(map + MAP_NEXT));
        W32(next + MAP_PREV, prevh);
    }
    if (R32(rec + 0xc) == file) W32(rec + 0xc, R32(map + MAP_NEXT));
    if (R32(rec + 8) == file) W32(rec + 8, R32(map + MAP_NEXT));
    memFreeHandle(R16(map + MAP_DIR));
    memUnlock(fh);
    memFreeHandle(fh);
    W16(gResError, 0);
    return 0;
fail:
    memUnlock(fh);
    return (uint16_t)err;
}

/* 0006868a: qsort comparator for the entry list: used entries by file offset, free
 * entries last. */
uint32_t resQsortList(uint32_t a, uint32_t b) {
    if (!(R8(a + 7) & 0x10) && !(R8(b + 7) & 0x10)) {
        uint32_t oa = R32(a), ob = R32(b);
        if (oa < ob) return 0xffffffff;
        if (oa > ob) return 1;
        return 0;
    }
    if (!(R8(a + 7) & 0x10)) return 0xffffffff;
    return (R8(b + 7) & 0x10) ? 0 : 1;
}

/* 0006871e: create an empty resource (type, id, name) in the current file (Pascal
 * resCreate(type, id, name)); returns the resource or 0. */
uint32_t resCreate(uint32_t name, int16_t id, uint32_t type) {
    ENTRY_LINK(FN_resCreate, 0x30, 32);
    maddr rec = client_rec();                                   /* A4 */
    uint32_t result;                                            /* -0x30 */
    if (R32(rec + 8) == 0) {
        W16(gResError, ERR_NOTFOUND);
        return 0;
    }
    maddr map = 0;                                              /* -0x2c */
    if (R32(rec + 8) != 0) {
        map = memLock((uint16_t)R32(rec + 8));
        W16(gResError, 0);
    } else {
        W16(gResError, ERR_BADRES);
    }
    if (R16(map + MAP_RDONLY) != 0) {
        W16(gResError, ERR_READONLY);
        result = 0;
        goto unlock;
    }
    uint32_t idx = RU16(map + MAP_FREE);                        /* D6 */
    if (idx != 0) {
        W16(map + MAP_FREE, RU16(ENTRY(map, idx) + 8));
        W8(ENTRY(map, idx) + 7, R8(ENTRY(map, idx) + 7) & 0xef);
    } else {
        idx = RU16(map + MAP_COUNT) + 1;
        memUnlock((uint16_t)R32(rec + 8));
        if (S16(memSetHandleSize(idx * 10 + 0x38, (uint16_t)R32(rec + 8))) != 0) {
            W16(gResError, memGetError());
            return 0;
        }
        map = memLock((uint16_t)R32(rec + 8));
        W16(map + MAP_COUNT, R16(map + MAP_COUNT) + 1);
    }
    W16(map + MAP_DIRTY, 1);
    maddr pe = ENTRY(map, idx);                                 /* -0x28 */
    if (S16(resDirInsert(name, id, type, (int16_t)idx, R16(map + MAP_DIR))) != 0) {
        result = 0;
        W8(pe + 7, R8(pe + 7) | 0x10);
        W16(pe + 8, R16(map + MAP_FREE));
        W16(map + MAP_FREE, (uint16_t)idx);
    } else {
        result = (uint32_t)(uint16_t)R32(rec + 8) << 16 | (uint16_t)idx;
        tc_memset(pe, 0, 10);
        W32(pe, R32(map + MAP_EOF));
        W16(gResError, 0);
    }
unlock:
    memUnlock((uint16_t)R32(rec + 8));
    return result;
}

/* 000689b4: create an empty resource file (header, empty directory and list). */
uint32_t resCreateFile(uint32_t spec) {
    ENTRY_LINK(FN_resCreateFile, 0x70, 32);
    const maddr buf = a6 - 0x70;               /* header (0x1c) + directory (4) + list (4) */
    const maddr cnt = a6 - 4;
    uint16_t err;
    int16_t f = S16(fileOpen(0x202, spec));
    if (f == 0) {
        err = (uint16_t)fileGetError();
        W16(gResError, err);
        return err;
    }
    W32(buf + 0x00, 0x4d48574b);               /* 'MHWK' */
    W32(buf + 0x04, 0x62);
    W32(buf + 0x08, 0x52535243);               /* 'RSRC' */
    W16(buf + 0x0c, 0x100);
    W16(buf + 0x0e, 0);
    W32(buf + 0x10, 0x24);
    W32(buf + 0x14, 0x1c);
    W16(buf + 0x18, 4);
    W16(buf + 0x1a, 4);
    W16(buf + 0x1e, 0);                        /* directory: no types */
    W16(buf + 0x1c, 4);                        /*            pool at 4 */
    W16(buf + 0x20, 0);                        /* list: no free entry */
    W16(buf + 0x22, 0);                        /*       no entry */
    W32(cnt, R32(buf + 0x10));
    resXlatChunk(buf);
    resXlatDir(0, buf + 0x1c);
    resXlatList(0, buf + 0x20);
    err = (uint16_t)fileWrite(cnt, buf, f);
    fileClose(f);
    if (err == 0) err = (uint16_t)fileGetError();
    W16(gResError, err);
    return err;
}

/* 00068ab6: delete a resource: free its memory, mark its entry free and remove it
 * from the directory. */
uint32_t resDelete(uint32_t rh) {
    ENTRY_LINK(FN_resDelete, 0x64, 32);
    uint16_t maph = (uint16_t)(rh >> 16);
    uint32_t idx = rh & 0xffff;
    int16_t err;
    if (maph == 0) {
        W16(gResError, ERR_BADRES);
        return ERR_BADRES;
    }
    maddr map = memLock(maph);                                 /* -0x64 */
    W16(gResError, 0);
    maddr pe = 0;                                              /* -0x60 */
    if (idx == 0 || (pe = ENTRY(map, idx), R8(pe + 7) & 0x10)) {
        memUnlock(maph);
        W16(gResError, ERR_BADRES);
        return ERR_BADRES;
    }
    if (R16(map + MAP_RDONLY) != 0) {
        W16(gResError, ERR_READONLY);
        err = ERR_READONLY;
    } else if (R8(pe + 7) & 0x40) {
        W16(gResError, ERR_PROTECTED);
        err = ERR_PROTECTED;
    } else {
        if (R8(pe + 7) & 8) resQCancel(rh);
        err = 0;
        if (R8(pe + 7) & 6) {
            err = free_owned_handle(R16(pe + 8));
            if (err != 0) {
                W16(gResError, err);
                goto unlock;
            }
        }
        W16(map + MAP_DIRTY, 1);
        if (((uint32_t)R8(pe + 6) << 16 | RU16(pe + 4)) != 0) W16(map + MAP_HOLES, 1);
        W8(pe + 7, R8(pe + 7) | 0x10);
        W16(pe + 8, R16(map + MAP_FREE));
        W16(map + MAP_FREE, (uint16_t)rh);
        resDirDelete((int16_t)rh, R16(map + MAP_DIR));
    }
unlock:
    memUnlock((uint16_t)(rh >> 16));
    return (uint16_t)err;
}

/* 00068d74: call cb(&file, arg) for each open file of the client until it returns
 * non-zero (Pascal resEnumFiles(cb, arg)). */
uint32_t resEnumFiles(uint32_t arg, uint32_t cb) {
    ENTRY_LINK(FN_resEnumFiles, 0x30, 32);
    const maddr pFile = a6 - 0x30;             /* long: the file passed to the callback */
    uint32_t client = osGetClient();
    maddr rec = osGetClientBytes(client);
    rec += RU16(gResClientOff);
    W32(pFile, R32(rec + 0xc));
    maddr map = 0;                             /* -0x10 */
    while (R32(pFile) != 0) {
        if (S16(resEnumCallback(arg, pFile, cb, client)) != 0) break;
        uint32_t f = R32(pFile);
        if (f != 0) {
            map = memXlat((uint16_t)f);
            W16(gResError, 0);
        } else {
            W16(gResError, ERR_BADRES);
        }
        W32(pFile, R32(map + MAP_NEXT));
    }
    W16(gResError, 0);
    return 0;
}

/* 00068e38: call cb(&idrecord, arg) for each id of a type in a file (Pascal
 * resEnumIds(file, type, cb, arg)). */
uint32_t resEnumIds(uint32_t arg, uint32_t cb, uint32_t type, uint32_t file) {
    ENTRY_LINK(FN_resEnumIds, 0x2c, 32);
    if (file == 0) {
        W16(gResError, ERR_BADRES);
        return ERR_BADRES;
    }
    maddr map = memLock((uint16_t)file);
    W16(gResError, 0);
    maddr dir = memLock(R16(map + MAP_DIR));
    uint32_t lo = 0, hi = RU16(dir + 2) + 1, mid;
    for (;;) {
        mid = (lo + hi) >> 1;
        if (mid == lo) goto done;
        if (R32(TYPEREC(dir, mid - 1)) < type) { lo = mid; continue; }
        if (R32(TYPEREC(dir, mid - 1)) > type) { hi = mid; continue; }
        break;
    }
    maddr idl = dir + RU16(TYPEREC(dir, mid - 1) + 4);
    for (uint32_t i = 0; i < RU16(idl); i++)
        if (S16(resEnumCallback(arg, idl + 2 + i * 4, cb, R32(map + MAP_CLIENT))) != 0) break;
done:
    memUnlock(R16(map + MAP_DIR));
    memUnlock((uint16_t)file);
    W16(gResError, 0);
    return 0;
}

/* 00068fbc: call cb(name, arg) for each named resource of a type in a file (Pascal
 * resEnumNames(file, type, cb, arg)). */
uint32_t resEnumNames(uint32_t arg, uint32_t cb, uint32_t type, uint32_t file) {
    ENTRY_LINK(FN_resEnumNames, 0x2c, 32);
    if (file == 0) {
        W16(gResError, ERR_BADRES);
        return ERR_BADRES;
    }
    maddr map = memLock((uint16_t)file);
    W16(gResError, 0);
    maddr dir = memLock(R16(map + MAP_DIR));
    uint32_t lo = 0, hi = RU16(dir + 2) + 1, mid;
    for (;;) {
        mid = (lo + hi) >> 1;
        if (mid == lo) goto done;
        if (R32(TYPEREC(dir, mid - 1)) < type) { lo = mid; continue; }
        if (R32(TYPEREC(dir, mid - 1)) > type) { hi = mid; continue; }
        break;
    }
    maddr nl = dir + RU16(TYPEREC(dir, mid - 1) + 6);
    for (uint32_t i = 0; i < RU16(nl); i++) {
        maddr s = dir + RU16(dir);
        s += RU16(nl + 2 + i * 4);
        if (S16(resEnumCallback(arg, s, cb, R32(map + MAP_CLIENT))) != 0) break;
    }
done:
    memUnlock(R16(map + MAP_DIR));
    memUnlock((uint16_t)file);
    W16(gResError, 0);
    return 0;
}

/* 00069158: call cb(&typerecord, arg) for each type in a file (Pascal
 * resEnumTypes(file, cb, arg)). */
uint32_t resEnumTypes(uint32_t arg, uint32_t cb, uint32_t file) {
    ENTRY_LINK(FN_resEnumTypes, 0x1c, 32);
    if (file == 0) {
        W16(gResError, ERR_BADRES);
        return ERR_BADRES;
    }
    maddr map = memLock((uint16_t)file);
    W16(gResError, 0);
    maddr dir = memLock(R16(map + MAP_DIR));
    for (uint32_t t = 0; t < RU16(dir + 2); t++)
        if (S16(resEnumCallback(arg, TYPEREC(dir, t), cb, R32(map + MAP_CLIENT))) != 0) break;
    memUnlock(R16(map + MAP_DIR));
    memUnlock((uint16_t)file);
    W16(gResError, 0);
    return 0;
}

/* 0006924e: find resource (type, id) in a file, or in the current file and the files
 * opened before it when file is 0 (Pascal resFind(type, id, file)). */
uint32_t resFind(uint32_t file, int16_t id, uint32_t type) {
    ENTRY_LINK(FN_resFind, 0x3c, 32);
    int one = 0;                                               /* D6 */
    if (file != 0) {
        one = 1;
    } else {
        maddr rec = client_rec();
        file = R32(rec + 8);
    }
    while (file != 0) {
        maddr map = 0;                                         /* -0x3c */
        if (file != 0) {
            map = memXlat((uint16_t)file);
            W16(gResError, 0);
        } else {
            W16(gResError, ERR_BADRES);
        }
        maddr dir = memXlat(R16(map + MAP_DIR));
        uint32_t lo = 0, hi = RU16(dir + 2) + 1, mid;
        for (;;) {
            mid = (lo + hi) >> 1;
            if (mid == lo) goto next;
            if (R32(TYPEREC(dir, mid - 1)) < type) { lo = mid; continue; }
            if (R32(TYPEREC(dir, mid - 1)) > type) { hi = mid; continue; }
            break;
        }
        maddr idl = dir + RU16(TYPEREC(dir, mid - 1) + 4);     /* -0x30 */
        lo = 0;
        hi = RU16(idl) + 1;
        for (;;) {
            mid = (lo + hi) >> 1;
            if (mid == lo) goto next;
            if (RU16(idl + 2 + (mid - 1) * 4) < (uint16_t)id) { lo = mid; continue; }
            if (RU16(idl + 2 + (mid - 1) * 4) > (uint16_t)id) { hi = mid; continue; }
            W16(gResError, 0);
            return (uint32_t)(uint16_t)file << 16 | RU16(idl + 2 + (mid - 1) * 4 + 2);
        }
    next:
        file = one ? 0 : R32(map + MAP_NEXT);
    }
    W16(gResError, ERR_NOTFOUND);
    return 0;
}

/* 0006945c: the resource whose loaded memory handle is h, in any open file. */
uint32_t resFindHandle(int16_t h) {
    ENTRY_LINK(FN_resFindHandle, 0x1c, 32);
    maddr rec = client_rec();
    uint32_t file = R32(rec + 0xc);
    while (file != 0) {
        maddr map = 0;                                         /* -0x1c */
        if (file != 0) {
            map = memXlat((uint16_t)file);
            W16(gResError, 0);
        } else {
            W16(gResError, ERR_BADRES);
        }
        for (uint32_t i = 1; i <= RU16(map + MAP_COUNT); i++) {
            maddr pe = ENTRY(map, i);
            if (!(R8(pe + 7) & 0x10) && (R8(pe + 7) & 6) && R16(pe + 8) == h)
                return (uint32_t)(uint16_t)file << 16 | (uint16_t)i;
        }
        file = R32(map + MAP_NEXT);
    }
    W16(gResError, ERR_NOTFOUND);
    return 0;
}

/* 00069590: find resource (type, name) like resFind (Pascal resFindName(type, name,
 * file)); names are compared without case. */
uint32_t resFindName(uint32_t file, uint32_t name, uint32_t type) {
    ENTRY_LINK(FN_resFindName, 0x44, 32);
    int one = 0;                                               /* D5 */
    if (file != 0) {
        one = 1;
    } else {
        maddr rec = client_rec();
        file = R32(rec + 8);
    }
    uint16_t len = (uint16_t)tc_strlen(name);                 /* D6 */
    while (file != 0) {
        maddr map = 0;                                         /* -0x44 */
        if (file != 0) {
            map = memXlat((uint16_t)file);
            W16(gResError, 0);
        } else {
            W16(gResError, ERR_BADRES);
        }
        maddr dir = memXlat(R16(map + MAP_DIR));               /* -0x40 */
        uint32_t lo = 0, hi = RU16(dir + 2) + 1, mid;
        for (;;) {
            mid = (lo + hi) >> 1;
            if (mid == lo) goto next;
            if (R32(TYPEREC(dir, mid - 1)) < type) { lo = mid; continue; }
            if (R32(TYPEREC(dir, mid - 1)) > type) { hi = mid; continue; }
            break;
        }
        maddr nl = dir + RU16(TYPEREC(dir, mid - 1) + 6);      /* -0x34 */
        lo = 0;
        hi = RU16(nl) + 1;
        for (;;) {
            mid = (lo + hi) >> 1;
            if (mid == lo) goto next;
            maddr s = dir + RU16(dir);
            s += RU16(nl + 2 + (mid - 1) * 4);
            int32_t r = S16(memicmp_(s, name, (uint32_t)len + 1));
            if (r < 0) { lo = mid; continue; }
            if (r > 0) { hi = mid; continue; }
            W16(gResError, 0);
            return (uint32_t)(uint16_t)file << 16 | RU16(nl + 2 + (mid - 1) * 4 + 2);
        }
    next:
        file = one ? 0 : R32(map + MAP_NEXT);
    }
    W16(gResError, ERR_NOTFOUND);
    return 0;
}

/* 000697d4: free the memory of a loaded resource (it stays in the file). */
uint32_t resFree(uint32_t rh) {
    ENTRY_LINK(FN_resFree, 0x48, 32);
    uint16_t maph = (uint16_t)(rh >> 16);
    uint32_t idx = rh & 0xffff;
    int16_t err;
    if (maph == 0) {
        W16(gResError, ERR_BADRES);
        return ERR_BADRES;
    }
    maddr map = memLock(maph);
    W16(gResError, 0);
    maddr pe = 0;
    if (idx == 0 || (pe = ENTRY(map, idx), R8(pe + 7) & 0x10)) {
        memUnlock(maph);
        W16(gResError, ERR_BADRES);
        return ERR_BADRES;
    }
    err = 0;
    if (R8(pe + 7) & 6) {
        err = free_owned_handle(R16(pe + 8));
        if (err != 0) {
            W16(gResError, err);
            goto unlock;
        }
    }
    W8(pe + 7, R8(pe + 7) & 0xf9);
unlock:
    memUnlock((uint16_t)(rh >> 16));
    return (uint16_t)err;
}

/* 000699a2 */
uint32_t resGetError(void) {
    return RU16(gResError);
}

/* 000699c2: the current file of the client. */
uint32_t resGetFile(void) {
    ENTRY_LINK(FN_resGetFile, 0, 4);
    maddr rec = client_rec();
    return R32(rec + 8);
}

/* 000699fc: type, id and name (empty when none) of a resource (Pascal
 * resGetInfo(rh, &type, &id, name)). */
uint32_t resGetInfo(uint32_t name, uint32_t pId, uint32_t pType, uint32_t rh) {
    ENTRY_LINK(FN_resGetInfo, 0x5c, 32);
    uint16_t maph = (uint16_t)(rh >> 16);
    uint32_t idx = rh & 0xffff;
    if (maph == 0) {
        W16(gResError, ERR_BADRES);
        return 0xffffffff;
    }
    maddr map = memXlat(maph);
    W16(gResError, 0);
    if (idx == 0 || (R8(ENTRY(map, idx) + 7) & 0x10)) {
        W16(gResError, ERR_BADRES);
        return 0xffffffff;
    }
    maddr dir = memXlat(R16(map + MAP_DIR));
    uint32_t want = rh & 0xffff;
    for (uint32_t t = 0; t < RU16(dir + 2); t++) {
        maddr idl = dir + RU16(TYPEREC(dir, t) + 4);
        for (uint32_t i = 0; i < RU16(idl); i++) {
            if (RU16(idl + 2 + i * 4 + 2) != want) continue;
            W32(pType, R32(TYPEREC(dir, t)));
            W16(pId, RU16(idl + 2 + i * 4));
            W8(name, 0);
            maddr nl = dir + RU16(TYPEREC(dir, t) + 6);
            for (uint32_t k = 0; k < RU16(nl); k++) {
                if (RU16(nl + 2 + k * 4 + 2) == want) {
                    maddr s = dir + RU16(dir);
                    s += RU16(nl + 2 + k * 4);
                    tc_strcpy(name, s);
                    break;
                }
            }
            W16(gResError, 0);
            return 0;
        }
    }
    W16(gResError, ERR_NOTINIT);
    return ERR_NOTINIT;
}

/* 00069c84: size of a resource (its handle's size when loaded); -1 on error. */
uint32_t resGetSize(uint32_t rh) {
    ENTRY_LINK(FN_resGetSize, 0x34, 32);
    uint16_t maph = (uint16_t)(rh >> 16);
    uint32_t idx = rh & 0xffff;
    if (maph == 0) {
        W16(gResError, ERR_BADRES);
        return 0xffffffff;
    }
    maddr map = memXlat(maph);
    W16(gResError, 0);
    maddr pe;
    if (idx == 0 || (pe = ENTRY(map, idx), R8(pe + 7) & 0x10)) {
        W16(gResError, ERR_BADRES);
        return 0xffffffff;
    }
    if (R8(pe + 7) & 2) return memGetHandleSize(R16(pe + 8));
    return (uint32_t)R8(pe + 6) << 16 | RU16(pe + 4);
}

/* 00069dd4: the open file a resource belongs to (0 on error). */
uint32_t resHomeFile(uint32_t rh) {
    ENTRY_LINK(FN_resHomeFile, 0x30, 32);
    uint16_t maph = (uint16_t)(rh >> 16);
    uint32_t idx = rh & 0xffff;
    if (maph == 0) {
        W16(gResError, ERR_BADRES);
        return 0;
    }
    maddr map = memXlat(maph);
    W16(gResError, 0);
    if (idx == 0 || (R8(ENTRY(map, idx) + 7) & 0x10)) {
        W16(gResError, ERR_BADRES);
        return 0;
    }
    return rh >> 16;
}

/* 00069ee8: load a resource; returns its memory handle (0 on error). */
uint32_t resLoad(uint32_t rh) {
    ENTRY_LINK(FN_resLoad, 0x54, 32);
    const maddr pSize = a6 - 0x4c;             /* long: size, passed to resRead */
    uint16_t maph = (uint16_t)(rh >> 16);
    uint32_t idx = rh & 0xffff;
    int16_t err;                                               /* D4 */
    uint16_t mh;                                               /* D5 */
    if (maph == 0) {
        W16(gResError, ERR_BADRES);
        return 0;
    }
    maddr map = memLock(maph);
    W16(gResError, 0);
    maddr pe = 0;                                              /* -0x50 */
    if (idx == 0 || (pe = ENTRY(map, idx), R8(pe + 7) & 0x10)) {
        memUnlock(maph);
        W16(gResError, ERR_BADRES);
        return 0;
    }
    if (R8(pe + 7) & 2) goto loaded;
    W32(pSize, (uint32_t)R8(pe + 6) << 16 | RU16(pe + 4));
    if (R8(pe + 7) & 4) {
        mh = RU16(pe + 8);                     /* reuse the purged handle */
        err = S16(memSetHandleSize(R32(pSize), mh));
        if (err != 0) {
            mh = 0;
            goto out;
        }
    } else {
        mh = (uint16_t)memAllocHandle(0, R32(pSize));
        if (mh == 0) {
            err = S16(memGetError());
            goto out;
        }
    }
    if (R32(pSize) != 0) {
        maddr p;
        NESTED(6, (W32(emu_sp(), rh), p = memLock(mh)));     /* resRead's slot and rh pushed */
        err = S16(resRead(0, pSize, p, rh));
        memUnlock(mh);
        if (err != 0) {
            if (R8(pe + 7) & 4) memSetHandleSize(0, mh);
            else free_owned_handle(mh);
            mh = 0;
            goto out;
        }
    }
    resAttachHandle(mh, pe);
    resQFlush(rh);
loaded:
    err = 0;
    mh = RU16(pe + 8);
out:
    memUnlock((uint16_t)(rh >> 16));
    W16(gResError, err);
    return mh;
}

/* 0006a1a6: open a resource file (or count one more open of an already open one) and
 * make it the current file; returns the file or 0. */
uint32_t resOpenFile(uint32_t spec) {
    ENTRY_LINK(FN_resOpenFile, 0x1c4, 32);
    const maddr hdr = a6 - 0x1c4;              /* 0x1c-byte file header */
    const maddr cnt = a6 - 0x1a4;              /* long: byte count for fileRead */
    const maddr ospec = a6 - 0x19c;            /* fileSpec of an open file */
    const maddr finfo = a6 - 0x96;             /* fileGetInfo: +0 volume */
    const maddr vinfo = a6 - 0x94;             /* fileGetVolInfo: +0 drive */
    const maddr dinfo = a6 - 0x70;             /* fileGetDriveInfo: +0 flags */
    int16_t err;                                               /* D6 */
    maddr rec = client_rec();                                  /* A4 */
    uint32_t file = R32(rec + 0xc);                            /* -0x1a0 */
    maddr map = 0;                                             /* -0x1a8 */
    while (file != 0) {
        __ct__8fileSpecDv(ospec);
        if (file != 0) {
            map = memLock((uint16_t)file);
            W16(gResError, 0);
        } else {
            W16(gResError, ERR_BADRES);
        }
        fileGetSpec(ospec, R16(map + MAP_FILE));
        if (S16(equal__8fileSpec(ospec, spec)) == 0) {
            W16(map + MAP_OPEN, R16(map + MAP_OPEN) + 1);
            uint32_t f = file;
            memUnlock((uint16_t)f);
            W16(gResError, 0);
            __dt__8fileSpecDv(ospec);
            return f;
        }
        memUnlock((uint16_t)file);
        file = R32(map + MAP_NEXT);
        __dt__8fileSpecDv(ospec);
    }
    int16_t fref = S16(fileOpen(0x43, spec));                  /* D4 */
    uint16_t rdonly;                                           /* D7 */
    if (fref != 0) {
        rdonly = 0;
    } else {
        err = S16(fileGetError());
        if (err != 0x283c || (fref = S16(fileOpen(0x41, spec))) == 0) {
            W16(gResError, err);
            return 0;
        }
        rdonly = 1;
    }
    uint16_t dirH;                                             /* D5 */
    if ((err = S16(fileGetInfo(finfo, fref))) != 0
        || (err = S16(fileGetVolInfo(vinfo, R16(finfo)))) != 0
        || (err = S16(fileGetDriveInfo(dinfo, R16(vinfo)))) != 0) {
        W16(gResError, err);
        goto close;
    }
    rdonly = (rdonly != 0 || ((RU16(dinfo) >> 6) & 1)) ? 1 : 0;
    W32(cnt, 0x1c);
    err = S16(fileRead(cnt, hdr, fref));
    if (err != 0) {
        W16(gResError, err == 0x283f ? ERR_BADFILE : err);
        goto close;
    }
    resXlatChunk(hdr);
    if (R32(hdr) != 0x4d48574b || R32(hdr + 8) != 0x52535243 || R16(hdr + 0xc) != 0x100
        || fileGetEOF(fref) != R32(hdr + 0x10)) {
        W16(gResError, ERR_BADFILE);
        goto close;
    }
    dirH = (uint16_t)memAllocHandle(0, RU16(hdr + 0x18));
    if (dirH == 0) {
        W16(gResError, memGetError());
        goto close;
    }
    fileSeek(0, R32(hdr + 0x14), fref);
    W32(cnt, RU16(hdr + 0x18));
    {
        maddr p;
        NESTED(4, (W16(emu_sp(), fref), p = memLock(dirH)));  /* fileRead's slot and file pushed */
        err = S16(fileRead(cnt, p, fref));
    }
    memUnlock(dirH);
    if (err != 0) {
        W16(gResError, err);
        goto freedir;
    }
    file = (uint16_t)memAllocHandle(0, (uint32_t)RU16(hdr + 0x1a) + 0x34);
    if (file == 0) {
        W16(gResError, memGetError());
        goto freedir;
    }
    map = memLock((uint16_t)file);
    W32(cnt, RU16(hdr + 0x1a));
    err = S16(fileRead(cnt, map + MAP_FREE, fref));
    if (err != 0) {
        W16(gResError, err);
        memUnlock((uint16_t)file);
        memFreeHandle((uint16_t)file);
        goto freedir;
    }
    tc_memset(map, 0, 0x34);
    W32(map + MAP_CLIENT, osGetClient());
    W16(map + MAP_OPEN, 1);
    W16(map + MAP_FILE, fref);
    W16(map + MAP_RDONLY, rdonly);
    W16(map + MAP_DIR, dirH);
    W16(map + MAP_HOLES, R16(hdr + 0xe));
    W32(map + MAP_EOF, R32(hdr + 0x10));
    W32(map + MAP_DIROFF, R32(hdr + 0x14));
    W32(map + MAP_DIRSIZE, (uint32_t)RU16(hdr + 0x18) + RU16(hdr + 0x1a));
    W32(map + MAP_PREV, 0);
    W32(map + MAP_NEXT, R32(rec + 0xc));
    if (R32(map + MAP_NEXT) != 0) {
        maddr next = memXlat((uint16_t)R32(map + MAP_NEXT));
        W32(next + MAP_PREV, file);
    }
    W32(rec + 0xc, file);
    W32(rec + 8, file);
    resXlatDir(1, memXlat(R16(map + MAP_DIR)));
    resXlatList(1, map + MAP_FREE);
    for (uint16_t i = 1; i <= RU16(map + MAP_COUNT); i++) {
        maddr pe = ENTRY(map, i);
        W8(pe + 7, R8(pe + 7) & 0xf0);
        if (R8(pe + 7) & 0x20) resQLoad(0, 0, (uint32_t)(uint16_t)file << 16 | i);   /* preload */
    }
    while (R32(map + MAP_QUEUE) != 0) {
        uint32_t q = R32(map + MAP_QUEUE);
        resQExecRef(0xffffffff, q);
        resQFreeRef(q);
    }
    memUnlock((uint16_t)file);
    W16(gResError, 0);
    return file;
freedir:
    memFreeHandle(dirH);
close:
    fileClose(fref);
    return 0;
}
