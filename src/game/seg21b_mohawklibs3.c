/* CODE 21 "Mohawk Libs 3", second part (0x6314a-0x66834): Mohawk support library.
 *   - the "mem" layer: numbered memory handles (1-based ids) over Mac handles, kept in
 *     a table of 14-byte master records with lock counts, purge levels and a purge proc
 *   - small C string helpers (stricmp, strrev, pstrcat, ...)
 *   - DEQBlock: a doubly linked list of 12-byte records kept in a handle
 *   - the "os" layer: clients (handles with reserved client bytes), interrupt tasks,
 *     the millisecond clock (Time Manager task), GetNextEvent/WaitNextEvent patches
 * Ported from the Ghidra disassembly of the original 68k code; addresses in comments are
 * Ghidra addresses (see docs/PORTING.md). */
#include <stdarg.h>
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* ---- mem globals (Ghidra addresses); memInit clears 0x10761c..0x107675 ---- */
#define gMemInitCount   GH(0x10761c)   /* word: memInit done */
#define gMemError       GH(0x10761e)   /* word: last mem error (memGetError) */
#define gMemPrevError   GH(0x107620)   /* word: error before the last one */
#define gMemPurgeOn     GH(0x107622)   /* word: purging allowed (memPurgeEnable) */
#define gMemBlockCount  GH(0x107624)   /* word: number of master blocks */
#define gMemBlocks      GH(0x107626)   /* long[16]: master blocks (0x200 records of 14 bytes) */
#define gMemOwnerCount  GH(0x107666)   /* word: memRegisterOwner */
#define gMemFailProc    GH(0x107668)   /* long: Pascal proc(err, size): Boolean "retry" */
#define gMemPurgeProc   GH(0x10766c)   /* long: Pascal proc(id): Boolean "may purge" */
#define gMemAttachPtr   GH(0x107670)   /* long: value attached by memAttachPtr */
#define gMemClientOff   GH(0x107674)   /* word: offset of the mem client bytes */

/* Master record (14 bytes), id = block * 0x200 + index + 1:
 *   +0 long  Mac handle (0 when purged)
 *   +4 word  flags: bit0 OS handle given out (memGetOSHandle), bit1 purged, bit2 in use,
 *            bit3 owner flag, bits4-7 purge level (0xf after a lock), bits8-15 lock count
 *   +6 word  bit15 purgeable
 *   +8 word  id
 *   +a long  attached value (memAttachHandle) */
#define MASTER_SIZE  0xe
#define MASTER_COUNT 0x200

/* ---- os globals ---- */
#define gOsCPU          GH(0x107842)   /* word: Gestalt 'proc' - 1 */
#define gOsMachine      GH(0x107844)   /* word: Gestalt 'mach' */
#define gOsSysVersion   GH(0x107846)   /* word: Gestalt 'sysv' */
#define gOsMMU          GH(0x107848)   /* word: Gestalt 'mmu ' */
#define gOsAddrMode     GH(0x10784a)   /* word: Gestalt 'addr' */
#define gOsTimer        GH(0x10784c)   /* TMTask (0x16 bytes, then A5 at +0x16) */
#define gOsTimerCount   GH(0x107856)   /*   tmCount */
#define gOsTimerWakeUp  GH(0x10785a)   /*   tmWakeUp */
#define gOsTimerRsvd    GH(0x10785e)   /*   tmReserved */
#define gOsTimeMs       GH(0x107866)   /* long: milliseconds (osGetTime) */
#define gOsTimeFrac     GH(0x10786a)   /* long: fraction of a millisecond, in 1/1000 */
#define gOsStartTicks   GH(0x10786e)   /* long: TickCount at osInit */
#define gOsOldGNE       GH(0x107872)   /* long: original GetNextEvent trap address */
#define gOsOldWNE       GH(0x107876)   /* long: original WaitNextEvent trap address */
#define gOsClients      GH(0x10787a)   /* long: client handle list (circular, doubly linked) */
#define gOsError        GH(0x10787e)   /* word: osGetError */
#define gOsInitCount    GH(0x107880)   /* word */
#define gOsIntrState    GH(0x107882)   /* word: osIntrEnable */
#define gOsActive       GH(0x107884)   /* long: active client handle */
#define gOsClientBytes  GH(0x107888)   /* word: client bytes reserved so far */
#define gInstancePtr    GH(0x1076c2)   /* long: pointer to the instance stored in new clients */
#define gTrapLimitPtr   GH(0x10779a)   /* long: pointer to the highest trap address accepted */
#define gThreadState    GH(0x107a58)   /* word: threadInit */

/* Client record (a locked handle): +0 next handle, +4 prev handle, +8 own handle,
 * +c instance, +10 activate hook (C proc(flag, client)), +14 interrupt task ring,
 * +24 client bytes (osReserveClientBytes). Interrupt task (NewPtr 0x14): +0 next,
 * +4 prev, +8 client, +c proc (C proc(refcon, task)), +10 refcon. */

#define MEM_SETERR(v) do { uint16_t e_ = (uint16_t)(v); W16(gMemPrevError, R16(gMemError)); W16(gMemError, e_); } while (0)

#define MASTER_FLAGS(m)   RU16((m) + 4)
#define M_OSHANDLE(m)     (MASTER_FLAGS(m) & 1)
#define M_PURGED(m)       ((MASTER_FLAGS(m) >> 1) & 1)
#define M_INUSE(m)        ((MASTER_FLAGS(m) >> 2) & 1)
#define M_OWNER(m)        ((MASTER_FLAGS(m) >> 3) & 1)
#define M_LEVEL(m)        ((MASTER_FLAGS(m) >> 4) & 0xf)
#define M_LOCKS(m)        (MASTER_FLAGS(m) >> 8)
#define M_PURGEABLE(m)    (RU16((m) + 6) >> 15)

/* A Boolean/char argument pushed with move.b lives in the high byte of its word. */
#define B_HI(w) ((uint8_t)((uint16_t)(w) >> 8))

/* The original "link A6" also saves A6 in the frame and makes A6 the frame pointer while
 * the function runs; 68k callees save that A6 in their own frames, and such stack bytes
 * can end up in the heap (timerCreate copies a stack record). ENTRY_LINK6 does the same
 * and restores A6 on return. */
static inline void port_unlink(maddr *old) { cpu_set(R_A6, *old); }
#define LINK_A6_() maddr old_a6_ __attribute__((cleanup(port_unlink))) = cpu_get(R_A6); \
    W32(a6, old_a6_); cpu_set(R_A6, a6)
#define ENTRY_LINK6(fn, link, regbytes) ENTRY_LINK(fn, link, regbytes); LINK_A6_()

/* D0 of a function with hand-written glue whose D0 is not its Pascal result. */
static uint32_t s_d0;

/* Pascal call through a pointer: `slot` result bytes reserved (not written, like subq),
 * then the arguments pushed in order ('w' word, 'l' long). Returns the slot contents. */
static uint32_t pas_call(maddr fn, int slot, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    maddr sp0 = emu_sp();
    maddr ss = sp0 - (uint32_t)slot;
    emu_set_sp(ss);
    for (const char *f = fmt; *f; f++) {
        uint32_t v = va_arg(ap, uint32_t);
        if (*f == 'w') push16((uint16_t)v); else push32(v);
    }
    va_end(ap);
    call68_addr(fn);
    uint32_t r = slot == 2 ? RU16(ss) : slot == 4 ? R32(ss) : 0;
    emu_set_sp(sp0);
    return r;
}

/* MemoryDispatch (A05C): LockMemory (2) / UnlockMemory (3), A0 = address, A1 = count */
static uint32_t trap_MemoryDispatch(uint32_t sel, uint32_t a0, uint32_t a1) {
    tb_begin();
    cpu_set(R_A0, a0);
    cpu_set(R_A1, a1);
    cpu_set(R_D0, sel);
    tb_trap(0xA05C);
    uint32_t r = cpu_get(R_D0);
    tb_end();
    return r;
}

/* Callee-saved registers: some originals hold values in D3-D7/A2-A4 while they call 68k
 * code, which saves them on the stack, and the stack bytes can end up in the heap. Those
 * functions set the emulated registers like the original (SETW) and restore them
 * on return (SAVE_REGS), as the original's movem does. */
typedef struct { uint32_t r[8]; } SavedRegs;
static const int saved_reg_ids[8] = { R_D3, R_D4, R_D5, R_D6, R_D7, R_A2, R_A3, R_A4 };
static inline void restore_regs(SavedRegs *s) { for (int i = 0; i < 8; i++) cpu_set(saved_reg_ids[i], s->r[i]); }
static inline SavedRegs save_regs(void) { SavedRegs s; for (int i = 0; i < 8; i++) s.r[i] = cpu_get(saved_reg_ids[i]); return s; }
#define SAVE_REGS SavedRegs saved_regs_ __attribute__((cleanup(restore_regs))) = save_regs(); (void)saved_regs_
#define SETW(reg, v)  cpu_set(reg, (cpu_get(reg) & 0xffff0000u) | (uint16_t)(v))

static inline maddr mem_block(int16_t i) { return R32(gMemBlocks + (int16_t)(i * 4)); }

/* ============================================================== master table == */

/* 0006314a: add a master block (0x200 free records) and 8 MoreMasters; 0 or 0x2776 */
uint32_t FUN_0006314a(void) {
    ENTRY_NOLINK(FN_FUN_0006314a, 28);
    if ((int32_t)R16(gMemBlockCount) + 1 < 0x10) {
        uint32_t p = tc_operator_new(0x1c00);
        W32(gMemBlocks + (int16_t)(R16(gMemBlockCount) * 4), p);
        if (R32(gMemBlocks + (int16_t)(R16(gMemBlockCount) * 4)) != 0) {
            maddr m = R32(gMemBlocks + (int16_t)(R16(gMemBlockCount) * 4));
            W16(gMemBlockCount, R16(gMemBlockCount) + 1);
            for (uint16_t n = 0x200; n != 0; n--) {
                W32(m, 0);
                W16(m + 4, RU16(m + 4) & 0xfffb);
                m += MASTER_SIZE;
            }
            for (uint16_t n = 8; n != 0; n--) _MoreMasters();
            MEM_SETERR(0);
            return 0;
        }
    }
    MEM_SETERR(0x2776);
    return 0x2776;
}

/* 000631f6: total size of the purgeable handles in use */
uint32_t FUN_000631f6(void) {
    ENTRY_NOLINK(FN_FUN_000631f6, 20);
    uint32_t total = 0;
    for (int16_t b = 0; b < R16(gMemBlockCount); b++) {
        maddr m = mem_block(b);
        for (uint16_t n = 0x200; n != 0; n--, m += MASTER_SIZE) {
            if (M_INUSE(m) && M_PURGEABLE(m))
                total += tc_GetHandleSize(R32(m));
        }
    }
    return total;
}

/* 00063260: purge every purgeable unlocked handle (the purge proc may veto); bytes freed */
uint32_t purgeAll__Fv(void) {
    ENTRY_LINK6(FN_purgeAll__Fv, 4, 32);
    uint32_t total = 0;
    for (int16_t b = 0; b < R16(gMemBlockCount); b++) {
        maddr m = mem_block(b);
        for (uint16_t n = 0x200; n != 0; n--, m += MASTER_SIZE) {
            if (!M_INUSE(m) || !M_PURGEABLE(m) || M_PURGED(m)) continue;
            uint32_t size = tc_GetHandleSize(R32(m));
            int16_t ok = 1;
            if (R32(gMemPurgeProc) != 0)
                ok = (int16_t)pas_call(R32(gMemPurgeProc), 2, "w", (uint32_t)(uint16_t)R16(m + 8));
            if (ok == 0) continue;
            _DisposeHandle(R32(m));
            if (R16(LM_MemErr) != 0) {
                MEM_SETERR(0x27a7);
                return total;
            }
            total += size;
            W16(m + 4, (RU16(m + 4) & 0xfffd) | 2);
            W32(m, 0);
        }
    }
    MEM_SETERR(0);
    return total;
}

/* 00063364: purge handles level by level (lowest purge level first, levels of the
 * others lowered) until CompactMem finds `need` bytes; bytes freed (Pascal, no result) */
uint32_t purgeThisMuch__FUl(uint32_t need) {
    ENTRY_LINK6(FN_purgeThisMuch__FUl, 0x14, 32);
    uint32_t total = 0;
    int16_t minLevel = 1, maxLevel = 0xf, level;
    MEM_SETERR(0);
    do {
        level = minLevel;
        minLevel = 0xf;
        maxLevel = 0;
        for (int16_t b = 0; b < R16(gMemBlockCount); b++) {
            maddr m = mem_block(b);
            for (uint16_t n = 0x200; n != 0; n--, m += MASTER_SIZE) {
                if (!M_INUSE(m) || !M_PURGEABLE(m) || M_PURGED(m)) continue;
                if (M_LEVEL(m) != 0) {
                    if (M_LEVEL(m) != 0) {
                        uint16_t v = (uint16_t)((M_LEVEL(m) - level) & 0xf);
                        W16(m + 4, (uint16_t)(v << 4) | (RU16(m + 4) & 0xff0f));
                    }
                    if ((int32_t)M_LEVEL(m) > (int32_t)maxLevel) maxLevel = (int16_t)M_LEVEL(m);
                    if ((int32_t)M_LEVEL(m) < (int32_t)minLevel && M_LEVEL(m) != 0)
                        minLevel = (int16_t)M_LEVEL(m);
                    continue;
                }
                uint32_t size = tc_GetHandleSize(R32(m));
                int16_t ok = 1;
                if (R32(gMemPurgeProc) != 0)
                    ok = (int16_t)pas_call(R32(gMemPurgeProc), 2, "w", (uint32_t)(uint16_t)R16(m + 8));
                if (ok == 0) continue;
                _DisposeHandle(R32(m));
                if (R16(LM_MemErr) != 0) {
                    MEM_SETERR(0x27a7);
                    return total;
                }
                total += size;
                W16(m + 4, (RU16(m + 4) & 0xfffd) | 2);
                W32(m, 0);
                if (_CompactMem(need) >= need) return total;
            }
        }
    } while (maxLevel != 0);
    return total;
}

/* 0006355a: NewPtr, retrying after purging and while the fail proc asks to */
uint32_t _memAllocPtr(uint32_t size) {
    ENTRY_LINK6(FN__memAllocPtr, 0, 12);
    uint32_t p = _NewPtr(size);
    if (p) return p;
    purgeThisMuch__FUl(size);
    p = _NewPtr(size);
    if (p) return p;
    if (R32(gMemFailProc) != 0) {
        int16_t retry;
        do {
            retry = (int16_t)pas_call(R32(gMemFailProc), 2, "lw", size, 0x2777u);
            p = _NewPtr(size);
            if (p) return p;
            purgeThisMuch__FUl(size);
            p = _NewPtr(size);
            if (p) return p;
        } while (retry != 0);
    }
    return 0;
}

/* 000635fa: NewHandle, retrying after purging and while the fail proc asks to */
uint32_t _memAllocHandle(uint32_t size) {
    ENTRY_LINK6(FN__memAllocHandle, 0, 12);
    uint32_t h = _NewHandle(size);
    if (h) return h;
    purgeThisMuch__FUl(size);
    h = _NewHandle(size);
    if (h) return h;
    if (R32(gMemFailProc) != 0) {
        int16_t retry;
        do {
            retry = (int16_t)pas_call(R32(gMemFailProc), 2, "lw", size, 0x2777u);
            h = _NewHandle(size);
            if (h) return h;
            purgeThisMuch__FUl(size);
            h = _NewHandle(size);
            if (h) return h;
        } while (retry != 0);
    }
    return 0;
}

/* 0006369e: SetHandleSize with the same retries; 0 or 0x2777 */
uint32_t _memSetHandleSize(uint32_t size, uint32_t h) {
    ENTRY_LINK6(FN__memSetHandleSize, 0, 12);
    _SetHandleSize(h, size);
    if (R16(LM_MemErr) == 0) return 0;
    purgeThisMuch__FUl(size);
    _SetHandleSize(h, size);
    if (R16(LM_MemErr) == 0) return 0;
    if (R32(gMemFailProc) != 0) {
        int16_t retry;
        do {
            retry = (int16_t)pas_call(R32(gMemFailProc), 2, "lw", size, 0x2777u);
            _SetHandleSize(h, size);
            if (R16(LM_MemErr) == 0) return 0;
            purgeThisMuch__FUl(size);
            _SetHandleSize(h, size);
            if (R16(LM_MemErr) == 0) return 0;
        } while (retry != 0);
    }
    return 0x2777;
}

/* 00063762 */
uint32_t memQueryInit(void) {
    return R16(gMemInitCount) != 0 ? 0x100 : 0;
}

/* 0006378e: allocate a numbered handle of `size` bytes; its id or 0 */
uint32_t memAllocHandle(int16_t owner, uint32_t size) {
    ENTRY_LINK6(FN_memAllocHandle, 0x28, 32);
    (void)owner;
    maddr id = a6 - 0x28;
    maddr m = findEmptySlot__FRs(id);
    if (m == 0) {
        if ((uint16_t)FUN_0006314a() != 0) {
            MEM_SETERR(0x2776);
            return 0;
        }
        m = findEmptySlot__FRs(id);
        if (m == 0) {
            MEM_SETERR(0x2776);
            return 0;
        }
    }
    uint32_t h = _memAllocHandle(size);
    W32(m, h);
    if (h == 0) {
        W16(m + 4, RU16(m + 4) & 0xfffb);
        W16(m + 4, (RU16(m + 4) & 0xfffd) | 2);
        MEM_SETERR(0x2777);
        return 0;
    }
    W16(m + 4, RU16(m + 4) & 0x00ff);
    W16(m + 4, RU16(m + 4) & 0xfffe);
    W16(m + 4, (RU16(m + 4) & 0xfffb) | 4);
    W16(m + 4, RU16(m + 4) & 0xfff7);
    W16(m + 6, RU16(m + 6) & 0x7fff);
    W16(m + 8, R16(id));
    W16(m + 4, RU16(m + 4) & 0xff0f);
    W16(m + 4, RU16(m + 4) & 0xfffd);
    MEM_SETERR(0);
    return RU16(m + 8);
}

/* 000638f6 */
uint32_t memAllocPtr(int16_t owner, uint32_t size) {
    ENTRY_LINK6(FN_memAllocPtr, 0, 24);
    (void)owner;
    if (size == 0) {
        MEM_SETERR(0);
        return 0;
    }
    uint32_t p = _memAllocPtr(size);
    if (p == 0) {
        MEM_SETERR(0x2777);
        return 0;
    }
    MEM_SETERR(0);
    return p;
}

/* 00063976: attach a value to a handle; the previous value (-1 if the id is bad) */
uint32_t memAttachHandle(uint32_t value, int16_t id) {
    ENTRY_LINK6(FN_memAttachHandle, 4, 24);
    maddr m = getMaster__FUs(id);
    if (m == 0) {
        MEM_SETERR(0x27a7);
        return 0xffffffff;
    }
    uint32_t old = R32(m + 0xa);
    W32(m + 0xa, value);
    MEM_SETERR(0);
    return old;
}

/* 000639e8 */
uint32_t memDummy(void) {
    return 0x19;
}

/* 00063a04: one global attached value for pointers */
uint32_t memAttachPtr(uint32_t value, uint32_t ptr) {
    ENTRY_LINK6(FN_memAttachPtr, 0, 20);
    if ((uint16_t)memQueryPtr(ptr) == 0) {
        MEM_SETERR(0x27af);
        return 0xffffffff;
    }
    uint32_t old = R32(gMemAttachPtr);
    W32(gMemAttachPtr, value);
    MEM_SETERR(0);
    return old;
}

/* 00063a6e */
uint32_t memCompact(uint32_t size) {
    ENTRY_LINK6(FN_memCompact, 0, 4);
    return _CompactMem(size);
}

/* 00063a98: call Pascal proc(info, refcon): Boolean for every handle in use (with the
 * client's instance set); a TRUE result skips the rest of that master block */
uint32_t memEnumHandles(uint32_t refcon, uint32_t proc) {
    ENTRY_LINK6(FN_memEnumHandles, 0x18, 32);
    maddr info = a6 - 0x14;
    W32(a6 - 0x18, osGetClient());
    for (int16_t b = 0; b < R16(gMemBlockCount); b++) {
        maddr m = mem_block(b);
        for (int16_t i = 0; i < 0x200; i++, m += MASTER_SIZE) {
            if (!M_INUSE(m)) continue;
            uint32_t h = R32(m);
            W32(info, h ? R32(h) : 0);
            W16(info + 4, R16(m + 8));
            W16(info + 6, M_OWNER(m));
            W16(info + 8, M_LOCKS(m));
            W16(info + 0xa, M_LEVEL(m));
            W16(info + 0xc, M_OSHANDLE(m));
            W16(info + 0xe, M_PURGEABLE(m));
            int16_t saved = (int16_t)osSetInstance((int16_t)osGetClientInstance(R32(a6 - 0x18)));
            uint16_t stop = (uint16_t)pas_call(proc, 2, "ll", (uint32_t)info, refcon);
            W16(a6 - 4, stop);
            osSetInstance(saved);
            if (R16(a6 - 4) != 0) break;
        }
    }
    return 0;
}

/* 00063bb0 */
uint32_t memFreeHandle(int16_t id) {
    ENTRY_LINK6(FN_memFreeHandle, 4, 32);
    maddr m = getMaster__FUs(id);
    if (m == 0) { MEM_SETERR(0x27a7); return 0x27a7; }
    if (M_LOCKS(m) != 0) { MEM_SETERR(0x27a8); return 0x27a8; }
    if (M_OWNER(m)) { MEM_SETERR(0x27a9); return 0x27a9; }
    if (M_OSHANDLE(m)) { MEM_SETERR(0x27a7); return 0x27a7; }
    _DisposeHandle(R32(m));
    if (R16(LM_MemErr) != 0) { MEM_SETERR(0x27a7); return 0x27a7; }
    W16(m + 4, RU16(m + 4) & 0xfffb);
    W32(m, 0);
    MEM_SETERR(0);
    return 0;
}

/* 00063cbc */
uint32_t memFreePtr(uint32_t p) {
    ENTRY_LINK6(FN_memFreePtr, 0, 12);
    _DisposePtr(p);
    if (R16(LM_MemErr) != 0) { MEM_SETERR(0x27af); return 0x27af; }
    MEM_SETERR(0);
    return 0;
}

/* 00063d1a */
uint32_t memGetAvail(void) {
    ENTRY_LINK6(FN_memGetAvail, 0, 4);
    MEM_SETERR(0);
    return _FreeMem();
}

/* 00063d4a */
uint32_t memGetError(void) {
    return RU16(gMemError);
}

/* 00063d6a: id of the handle whose master pointer is p (0 if none) */
uint32_t memGetHandle(uint32_t p) {
    for (int16_t b = 0; b < R16(gMemBlockCount); b++) {
        maddr m = mem_block(b);
        for (uint16_t n = 0x200; n != 0; n--, m += MASTER_SIZE) {
            if (M_INUSE(m) && R32(R32(m)) == p) return RU16(m + 8);
        }
    }
    MEM_SETERR(0x27ad);
    return 0;
}

/* 00063dfc */
uint32_t memGetHandleSize(int16_t id) {
    ENTRY_LINK6(FN_memGetHandleSize, 0, 20);
    maddr m = getMaster__FUs(id);
    if (m == 0) return 0xffffffff;
    uint32_t size = tc_GetHandleSize(R32(m));
    if (R16(LM_MemErr) != 0) { MEM_SETERR(0x27a7); return 0xffffffff; }
    MEM_SETERR(0);
    return size;
}

/* 00063e7e */
uint32_t memGetLockCount(int16_t id) {
    ENTRY_LINK6(FN_memGetLockCount, 0, 20);
    maddr m = getMaster__FUs(id);
    if (m == 0) { MEM_SETERR(0x27a7); return 0xffff; }
    if (!M_INUSE(m)) { MEM_SETERR(0x2775); return 0xffff; }
    MEM_SETERR(0);
    return M_LOCKS(m);
}

/* 00063f0a: the Mac handle of an unlocked handle (marked as given out) */
uint32_t memGetOSHandle(int16_t id) {
    ENTRY_LINK6(FN_memGetOSHandle, 0, 28);
    maddr m = getMaster__FUs(id);
    if (m == 0) return 0;
    if (M_OSHANDLE(m)) { MEM_SETERR(0x27a7); return 0; }
    if (M_LOCKS(m) != 0) { MEM_SETERR(0x27a8); return 0; }
    W16(m + 4, (RU16(m + 4) & 0xfffe) | 1);
    MEM_SETERR(0);
    return R32(m);
}

/* 00063fb2 */
uint32_t memGetOwner(int16_t id) {
    ENTRY_LINK6(FN_memGetOwner, 0, 12);
    maddr m = getMaster__FUs(id);
    if (m == 0) return 0xffff;
    MEM_SETERR(0);
    return M_OWNER(m);
}

/* 0006400c: size of a pointer (or of the handle it is the master pointer of) */
uint32_t memGetPtrSize(uint32_t p) {
    ENTRY_LINK6(FN_memGetPtrSize, 0, 16);
    uint32_t size = tc_GetPtrSize(p);
    if (size == 0) size = tc_GetHandleSize(_RecoverHandle(p));
    if (R16(LM_MemErr) != 0) { MEM_SETERR(0x27af); return 0; }
    MEM_SETERR(0);
    return size;
}

/* 0006408a: init (needs os); adds 1 + (n >> 9) master blocks (at most 16) */
uint32_t memInit(int16_t n) {
    ENTRY_LINK6(FN_memInit, 0, 20);
    SAVE_REGS;
    uint16_t r = 0;
    SETW(R_D3, n);
    SETW(R_D4, r);
    if ((uint16_t)osQueryInit() < 0x100) {
        MEM_SETERR(0x27b0);
        return 1;
    }
    if (R16(gMemInitCount) == 0) {
        tc_memset(gMemInitCount, 0, 0x5a);
        W16(gMemInitCount, R16(gMemInitCount) + 1);
        uint16_t off = (uint16_t)osReserveClientBytes(0xe);
        W16(gMemClientOff, off);
        if (off == 0) {
            r = (uint16_t)osGetError();
            SETW(R_D4, r);
            SETW(R_D7, r);
            MEM_SETERR(r);
        } else {
            _MaxApplZone();
            r = (uint16_t)FUN_0006314a();
            SETW(R_D4, r);
            int16_t k = (int16_t)((uint16_t)n >> 9);
            if (k >= 0x10) k = 0xf;
            SETW(R_D5, k);
            for (;;) {
                int16_t t = k--;
                SETW(R_D5, k);
                if (t <= 0) break;
                r = (uint16_t)FUN_0006314a();
                SETW(R_D4, r);
            }
        }
    }
    W16(gMemPurgeOn, 1);
    return r;
}

/* 0006415a: lock (count + 1, purge level 0xf); the master pointer */
uint32_t memLock(int16_t id) {
    ENTRY_LINK6(FN_memLock, 0xc, 32);
    maddr m = getMaster__FUs(id);
    if (m == 0) return 0;
    if (R32(m) == 0) { MEM_SETERR(0x2775); return 0; }
    if (M_OSHANDLE(m)) { MEM_SETERR(0x27a7); return 0; }
    if (M_LOCKS(m) == 0xff) { MEM_SETERR(0x27aa); return 0; }
    _HLock(R32(m));
    if (R16(LM_MemErr) != 0) { MEM_SETERR(0x27a7); return 0; }
    uint16_t c = (uint16_t)((((RU16(m + 4) >> 8) & 0xff) + 1) & 0xff);
    W16(m + 4, (uint16_t)(c << 8) | (RU16(m + 4) & 0xff));
    W16(m + 4, (RU16(m + 4) & 0xff0f) | 0xf0);
    MEM_SETERR(0);
    return R32(R32(m));
}

/* 00064274: MoveHHi then memLock */
uint32_t memLockXtd(int16_t id) {
    ENTRY_LINK6(FN_memLockXtd, 0, 8);
    maddr m = getMaster__FUs(id);
    if (m == 0) return 0;
    _MoveHHi(R32(m));
    return memLock(id);
}

/* 000642c2 */
uint32_t memMove(uint32_t size, uint32_t src, uint32_t dst) {
    ENTRY_LINK6(FN_memMove, 0, 16);
    _BlockMove(src, dst, size);
    MEM_SETERR(0);
    return dst;
}

/* VM present: system >= 7.0, an MMU (68030+ or a PMMU), not a Mac II or machine 0x31 */
static int vm_absent(void) {
    if (RU16(gOsSysVersion) < 0x700) return 1;
    if (RU16(gOsCPU) < 3 && RU16(gOsMMU) < 2) return 1;
    if (R16(gOsMachine) == 8 || R16(gOsMachine) == 0x31) return 1;
    return 0;
}

/* 0006430c: LockMemory a pointer's block (VM) */
uint32_t memPageLock(uint32_t p) {
    ENTRY_LINK6(FN_memPageLock, 0, 24);
    if (vm_absent()) { MEM_SETERR(0); return 0; }
    uint32_t size = memGetPtrSize(p);
    if (size == 0) { MEM_SETERR(0x27af); return 0x27af; }
    if ((uint16_t)trap_MemoryDispatch(2, p, size) != 0) { MEM_SETERR(0x27af); return 0x27af; }
    MEM_SETERR(0);
    return 0;
}

/* 000643ce: UnlockMemory a pointer's block (VM) */
uint32_t memPageUnlock(uint32_t p) {
    ENTRY_LINK6(FN_memPageUnlock, 0, 24);
    if (vm_absent()) { MEM_SETERR(0); return 0; }
    uint32_t size = memGetPtrSize(p);
    if (size == 0) { MEM_SETERR(0x27af); return 0x27af; }
    if ((uint16_t)trap_MemoryDispatch(3, p, size) != 0) { MEM_SETERR(0x27af); return 0x27af; }
    MEM_SETERR(0);
    return 0;
}

/* 00064492: size 0: purgeable total; else purge (everything if that is not more) */
uint32_t memPurge(uint32_t size) {
    ENTRY_LINK6(FN_memPurge, 0, 20);
    MEM_SETERR(0);
    uint32_t avail = FUN_000631f6();
    if (size == 0) return avail;
    if (R16(gMemPurgeOn) == 0) return 0;
    if (avail == 0) return 0;
    if (avail <= size) return purgeAll__Fv();
    return purgeThisMuch__FUl(size);
}

/* 00064516 */
uint32_t memPurgeEnable(int16_t on) {
    uint16_t old = RU16(gMemPurgeOn);
    W16(gMemPurgeOn, on);
    MEM_SETERR(0);
    return old;
}

/* 0006455c */
uint32_t memPurgeHandle(int16_t id) {
    ENTRY_LINK6(FN_memPurgeHandle, 4, 32);
    maddr m = getMaster__FUs(id);
    if (m == 0) { MEM_SETERR(0x27a7); return 0x27a7; }
    if (M_LOCKS(m) != 0) { MEM_SETERR(0x27a8); return 0x27a8; }
    if (!M_PURGEABLE(m)) { MEM_SETERR(0x27ac); return 0x27ac; }
    if (M_PURGED(m)) { MEM_SETERR(0x2775); return 0x2775; }
    if ((uint16_t)pas_call(R32(gMemPurgeProc), 2, "w", (uint32_t)(uint16_t)R16(m + 8)) != 0) {
        _DisposeHandle(R32(m));
        if (R16(LM_MemErr) != 0) { MEM_SETERR(0x27a7); return 0x27a7; }
        W16(m + 4, (RU16(m + 4) & 0xfffd) | 2);
        W32(m, 0);
    }
    MEM_SETERR(0);
    return 0;
}

/* 00064680: handle in use and not purged */
uint32_t memQueryHandle(int16_t id) {
    ENTRY_LINK6(FN_memQueryHandle, 0, 12);
    maddr m = getMaster__FUs(id);
    if (m == 0) return 0;
    if (!M_INUSE(m) || R32(m) == 0) return 0;
    return 1;
}

/* 000646de */
uint32_t memQueryPtr(uint32_t p) {
    ENTRY_LINK6(FN_memQueryPtr, 0, 4);
    return tc_GetPtrSize(p) != 0 ? 1 : 0;
}

/* 0006471c: free the master blocks */
uint32_t memQuit(void) {
    ENTRY_LINK6(FN_memQuit, 4, 20);
    for (int16_t b = 0; b < R16(gMemBlockCount); b++) {
        /* the original scans the block without effect */
        int32_t i = b;
        tc_operator_delete(R32(gMemBlocks + (int16_t)(i * 4)));
        W32(gMemBlocks + (int16_t)(i * 4), 0);
    }
    W16(gMemInitCount, 0);
    return 0;
}

/* 000647a0: realloc a pointer, or a handle's master pointer (see the glue: one path
 * leaves the result slot unwritten) */
uint32_t memReallocPtr(int16_t owner, uint32_t size, uint32_t p) {
    ENTRY_LINK6(FN_memReallocPtr, 4, 28);
    maddr slot = args_ + 10;
    uint32_t r;
    if (size == 0) {
        memFreePtr(p);
        r = 0;
        goto done;
    }
    if (p == 0) {
        r = memAllocPtr(owner, size);
        goto done;
    }
    uint16_t id = (uint16_t)memGetHandle(p);
    if (id != 0) {
        uint16_t locks = (uint16_t)memSetLockCount(0, (int16_t)id);
        uint16_t err = (uint16_t)memSetHandleSize(size, (int16_t)id);
        memSetLockCount((int16_t)locks, (int16_t)id);
        MEM_SETERR(err);
        if (err != 0) { r = 0; goto done; }
        r = memXlat((int16_t)id);
        goto done;
    }
    if ((uint16_t)memSetPtrSize(size, p) == 0) { r = p; goto done; }
    uint32_t np = memAllocPtr(owner, size);
    if (np == 0) { r = p; goto done; }
    uint32_t n = memGetPtrSize(p);
    if (n == 0) {           /* returns without writing its result */
        s_d0 = 0;
        return R32(slot);
    }
    memMove(n, p, np);
    memFreePtr(p);
    r = np;
done:
    W32(slot, r);
    s_d0 = r;
    return r;
}
void glue_memReallocPtr(void) {
    maddr sp = emu_sp();
    memReallocPtr(R16(sp), R32(sp + 2), R32(sp + 6));
    emu_set_sp(sp + 10);
    emu_set_d0(s_d0);
}

/* 000648d4: only once; 1 (the slot is not written on the error path) */
uint32_t memRegisterOwner(void) {
    maddr slot = emu_sp() == port_glue_sp ? port_glue_sp : emu_sp() - 2;
    uint32_t hi = cpu_get(R_D0) & 0xffff0000u;
    if (R16(gMemOwnerCount) == 0) {
        MEM_SETERR(0);
        W16(gMemOwnerCount, R16(gMemOwnerCount) + 1);
        uint16_t v = RU16(gMemOwnerCount);
        W16(slot, v);
        s_d0 = hi | v;
        return v;
    }
    s_d0 = hi | RU16(gMemOwnerCount);
    MEM_SETERR(0x27ab);
    return RU16(slot);
}
void glue_memRegisterOwner(void) {
    memRegisterOwner();
    emu_set_d0(s_d0);
}

/* 00064928 */
uint32_t memReleaseOSHandle(int16_t id) {
    ENTRY_LINK6(FN_memReleaseOSHandle, 0, 12);
    maddr m = getMaster__FUs(id);
    if (m == 0) return 0x27a7;
    W16(m + 4, RU16(m + 4) & 0xfffe);
    return 0;
}

/* 00064980: fill count bytes with a char (high byte of the word) */
uint32_t memSet(uint32_t count, int16_t c, uint32_t p) {
    MEM_SETERR(0);
    maddr q = p;
    while (count-- != 0) W8(q++, B_HI(c));
    return p;
}

/* 000649d0 */
uint32_t memSetFailProc(uint32_t proc) {
    uint32_t old = R32(gMemFailProc);
    W32(gMemFailProc, proc);
    return old;
}

/* 00064a08: resize (reallocating a purged handle) */
uint32_t memSetHandleSize(uint32_t size, int16_t id) {
    ENTRY_LINK6(FN_memSetHandleSize, 0xc, 32);
    maddr m = getMaster__FUs(id);
    if (m == 0) { MEM_SETERR(0x27a7); return 0x27a7; }
    if (M_OSHANDLE(m)) { MEM_SETERR(0x27a7); return 0x27a7; }
    if (M_LOCKS(m) != 0) { MEM_SETERR(0x27a8); return 0x27a8; }
    if (R32(m) == 0) {
        uint32_t h = _memAllocHandle(size);
        W32(m, h);
        if (h == 0) { MEM_SETERR(0x2777); return 0x2777; }
        W16(m + 4, RU16(m + 4) & 0xfffd);
        MEM_SETERR(0);
        return 0;
    }
    uint16_t e = (uint16_t)_memSetHandleSize(size, R32(m));
    if (e == 0) W16(m + 4, RU16(m + 4) & 0xfffd);
    MEM_SETERR(e);
    return e;
}

/* 00064b44: set the lock count (HLock/HUnlock); the previous count */
uint32_t memSetLockCount(int16_t count, int16_t id) {
    ENTRY_LINK6(FN_memSetLockCount, 8, 32);
    maddr m = getMaster__FUs(id);
    if (m == 0) { MEM_SETERR(0x27a7); return 0xffff; }
    if (M_OSHANDLE(m)) { MEM_SETERR(0x27a7); return 0xffff; }
    if ((uint16_t)count >= 0xff) { MEM_SETERR(0x27aa); return 0xffff; }
    uint16_t old = M_LOCKS(m);
    uint16_t c = (uint16_t)count & 0xff;
    W16(m + 4, (uint16_t)(c << 8) | (RU16(m + 4) & 0xff));
    if (c != 0) _HLock(R32(m)); else _HUnlock(R32(m));
    return old;
}

/* 00064c1c */
uint32_t memSetOwner(int16_t owner, int16_t id) {
    ENTRY_LINK6(FN_memSetOwner, 0, 24);
    maddr m = getMaster__FUs(id);
    if (m == 0) return 0x27a7;
    W16(m + 4, (uint16_t)(((uint16_t)owner & 1) << 3) | (RU16(m + 4) & 0xfff7));
    MEM_SETERR(0);
    return 0;
}

/* 00064c8a */
uint32_t memSetPtrSize(uint32_t size, uint32_t p) {
    ENTRY_LINK6(FN_memSetPtrSize, 4, 28);
    if (size == 0) return (uint16_t)memFreePtr(p);
    if ((uint16_t)memQueryPtr(p) == 0) _SetHandleSize(_RecoverHandle(p), size);
    else _SetPtrSize(p, size);
    int16_t e = R16(LM_MemErr);
    if (e != 0) {
        if (e == -0x6c) { MEM_SETERR(0x2774); return 0x2774; }
        MEM_SETERR(0x27af);
        return 0x27af;
    }
    MEM_SETERR(0);
    return 0;
}

/* 00064d46 */
uint32_t memSetPurgeProc(uint32_t proc) {
    uint32_t old = R32(gMemPurgeProc);
    W32(gMemPurgeProc, proc);
    return old;
}

/* 00064d80: set purgeable; the previous state */
uint32_t memSetPurgeState(int16_t on, int16_t id) {
    ENTRY_LINK6(FN_memSetPurgeState, 4, 32);
    maddr m = getMaster__FUs(id);
    if (m == 0) return 0xffff;
    if (M_OSHANDLE(m)) { MEM_SETERR(0x27a7); return 0xffff; }
    if (M_LOCKS(m) != 0) { MEM_SETERR(0x27a8); return 0xffff; }
    uint16_t old = M_PURGEABLE(m);
    W16(m + 6, (uint16_t)(((uint16_t)on & 1) << 15) | (RU16(m + 6) & 0x7fff));
    MEM_SETERR(0);
    return old;
}

/* 00064e4a: exchange the Mac handles of two ids */
uint32_t memSwapHandles(int16_t id1, int16_t id2) {
    ENTRY_LINK6(FN_memSwapHandles, 0xc, 32);
    maddr m2 = getMaster__FUs(id2);
    if (m2 == 0) return 0x27a7;
    maddr m1 = getMaster__FUs(id1);
    if (m1 == 0) return 0x27a7;
    if (M_OSHANDLE(m2) || M_OSHANDLE(m1)) { MEM_SETERR(0x27a7); return 0x27a7; }
    if (M_LOCKS(m2) != 0 || M_LOCKS(m1) != 0) { MEM_SETERR(0x27a8); return 0x27a8; }
    uint32_t h2 = R32(m2);
    W32(m2, R32(m1));
    W32(m1, h2);
    MEM_SETERR(0);
    return 0;
}

/* 00064f3a: lock count - 1 (HUnlock at 0) */
uint32_t memUnlock(int16_t id) {
    ENTRY_LINK6(FN_memUnlock, 4, 32);
    maddr m = getMaster__FUs(id);
    if (m == 0) return 0x27a7;
    if (M_OSHANDLE(m)) { MEM_SETERR(0x27a7); return 0x27a7; }
    if (M_LOCKS(m) != 0) {
        uint16_t c = (uint16_t)((((RU16(m + 4) >> 8) & 0xff) - 1) & 0xff);
        W16(m + 4, (uint16_t)(c << 8) | (RU16(m + 4) & 0xff));
    }
    if (M_LOCKS(m) == 0) _HUnlock(R32(m));
    if (R16(LM_MemErr) != 0) { MEM_SETERR(0x27a7); return 0x27a7; }
    MEM_SETERR(0);
    return 0;
}

/* 0006500c: the master pointer (purge level set to 0xf) */
uint32_t memXlat(int16_t id) {
    ENTRY_LINK6(FN_memXlat, 0, 20);
    maddr m = getMaster__FUs(id);
    if (m == 0) return 0;
    MEM_SETERR(0);
    W16(m + 4, (RU16(m + 4) & 0xff0f) | 0xf0);
    return R32(R32(m));
}

/* ==================================================================== strings == */

/* 0006506e: compare at most n chars ignoring bit 5 */
uint32_t strnicmp_(uint32_t s1, uint32_t s2, uint32_t n) {
    uint16_t hi = 0;                    /* high word D0 is left with */
    int16_t i = 1;
    for (;;) {
        if (R8(s1) == 0 || R8(s2) == 0) break;
        hi = 0;
        if ((((int32_t)RS8(s1) ^ (int32_t)RS8(s2)) & 0xdf) != 0) break;
        hi = i < 0 ? 0xffff : 0;
        if ((uint32_t)(int32_t)i >= n) break;
        s1++; s2++; i++;
    }
    return (uint32_t)hi << 16 | (uint16_t)(int16_t)(RS8(s1) - RS8(s2));
}

/* 000650dc */
uint32_t stricmp_(uint32_t s1, uint32_t s2) {
    while (R8(s1) != 0 && R8(s2) != 0) {
        if ((((int32_t)RS8(s1) ^ (int32_t)RS8(s2)) & 0xdf) != 0) break;
        s1++; s2++;
    }
    return (uint16_t)(int16_t)(RS8(s1) - RS8(s2));
}

/* 00065138: compare at most n bytes ignoring bit 5 (unsigned) */
uint32_t memicmp_(uint32_t s1, uint32_t s2, uint32_t n) {
    uint32_t i = 1;
    for (;;) {
        uint8_t c = R8(s1);
        if (c == 0 || R8(s2) == 0) break;
        if (((c ^ R8(s2)) & 0xdf) != 0) break;
        if (i >= n) break;
        s1++; s2++; i++;
    }
    return (uint16_t)(R8(s1) - R8(s2));
}

/* 000651b4 */
uint32_t strrev_(uint32_t s) {
    ENTRY_LINK6(FN_strrev_, 4, 12);
    maddr a = s;
    maddr b = tc_strlen(s) + a - 1;
    while (a < b) {
        uint8_t t = R8(a);
        W8(a++, R8(b));
        W8(b--, t);
    }
    return s;
}

/* 00065208 */
uint32_t strupr_(uint32_t s) {
    maddr p = s;
    int8_t c;
    while ((c = RS8(p)) != 0) {
        if (c >= 'a' && c <= 'z') c = (int8_t)(c - 0x20);
        W8(p++, c);
    }
    return s;
}

/* 00065254 */
uint32_t strlwr_(uint32_t s) {
    maddr p = s;
    int8_t c;
    while ((c = RS8(p)) != 0) {
        if (c >= 'A' && c <= 'Z') c = (int8_t)(c + 0x20);
        W8(p++, c);
    }
    return s;
}

/* 000652a0: the char is the high byte of the word */
uint32_t strset_(uint32_t s, int16_t c) {
    maddr p = s;
    while (R8(p) != 0) W8(p++, B_HI(c));
    return s;
}

/* 000652d2 */
uint32_t strnset_(uint32_t s, int16_t c, uint32_t n) {
    maddr p = s;
    while (R8(p) != 0) {
        if (n-- == 0) break;
        W8(p++, B_HI(c));
    }
    return s;
}

/* 00065312 */
uint32_t strdup_(uint32_t s) {
    ENTRY_LINK6(FN_strdup_, 0, 8);
    uint32_t p = tc_malloc(tc_strlen(s) + 1);
    if (p != 0) tc_strcpy(p, s);
    return p;
}

/* 00065354: dprintf(fmt, ...) -> DebugStr (variadic: the arguments follow fmt) */
uint32_t dprintf_(uint32_t fmt) {
    ENTRY_LINK6(FN_dprintf_, 0x100, 4);
    maddr buf = a6 - 0x100;
    tc_vsprintf(buf, fmt, args_ + 4);
    tc_CtoPstr(buf);
    return _DebugStr(buf);
}

/* 00065394: append Pascal string src to dst */
uint32_t pstrcat(uint32_t dst, uint32_t src) {
    ENTRY_LINK6(FN_pstrcat, 0, 12);
    _BlockMove(src + 1, dst + R8(dst) + 1, R8(src));
    W8(dst, R8(dst) + R8(src));
    return 0;
}

/* 000653d8: insert Pascal string src at the start of dst */
uint32_t pstrinsert(uint32_t dst, uint32_t src) {
    ENTRY_LINK6(FN_pstrinsert, 0, 12);
    _BlockMove(dst + 1, dst + R8(src) + 1, R8(dst));
    _BlockMove(src + 1, dst + 1, R8(src));
    W8(dst, R8(dst) + R8(src));
    return 0;
}

/* 0006542c: compare Pascal strings (at most n chars) ignoring bit 5 */
uint32_t pstrnicmp(uint32_t s1, uint32_t s2, uint32_t n) {
    uint32_t len;
    if (R8(s1) < R8(s2)) {
        len = R8(s1);
        if (len >= n) len = n;
    } else {
        len = R8(s2);
        if (len >= n) len = n;
    }
    uint8_t cnt = (uint8_t)len;
    maddr a = s1 + 1, b = s2 + 1;
    for (int16_t i = 1; (int32_t)i <= (int32_t)cnt; i++, a++, b++) {
        uint8_t x = R8(a), y = R8(b);
        if (((x ^ y) & 0xdf) != 0) return (uint16_t)(x - y);
    }
    return (uint16_t)(R8(s1) - R8(s2));
}

/* 000654f0: compare Pascal strings ignoring bit 5 */
uint32_t pstricmp(uint32_t s1, uint32_t s2) {
    uint8_t cnt = R8(s1) < R8(s2) ? R8(s1) : R8(s2);
    maddr a = s1 + 1, b = s2 + 1;
    for (int16_t i = 1; (int32_t)i <= (int32_t)cnt; i++, a++, b++) {
        uint8_t x = R8(a), y = R8(b);
        if (((x ^ y) & 0xdf) != 0) return (uint16_t)(x - y);
    }
    return (uint16_t)(R8(s1) - R8(s2));
}

/* 00065592: realloc a pointer with the Toolbox (one path leaves the slot unwritten) */
uint32_t ReallocPtr(uint32_t size, uint32_t p) {
    ENTRY_LINK6(FN_ReallocPtr, 0, 16);
    maddr slot = args_ + 8;
    uint32_t r;
    if (size == 0) {
        _DisposePtr(p);
        r = 0;
    } else if (p == 0) {
        r = _NewPtrClear(size);
    } else {
        _SetPtrSize(p, size);
        if (R16(LM_MemErr) == 0) {
            r = p;
        } else {
            uint32_t np = _NewPtrClear(size);
            if (np == 0) {
                r = p;
            } else {
                uint32_t n = tc_GetPtrSize(p);
                if (n == 0) {           /* returns without writing its result */
                    s_d0 = 0;
                    return R32(slot);
                }
                _BlockMove(p, np, n);
                _DisposePtr(p);
                r = np;
            }
        }
    }
    W32(slot, r);
    s_d0 = r;
    return r;
}
void glue_ReallocPtr(void) {
    maddr sp = emu_sp();
    ReallocPtr(R32(sp), R32(sp + 4));
    emu_set_sp(sp + 8);
    emu_set_d0(s_d0);
}

/* =================================================================== DEQBlock == */
/* DEQBlock: +0 word record count, +2 long used list head, +6 long free list head,
 * +a handle to the 12-byte records: +0 next, +4 prev, +8 data (-1 ends a list) */

static inline maddr deq_rec(maddr self, uint32_t i) { return R32(R32(self + 0xa)) + i * 12u; }

/* 0006562a: unlink the head of a list */
uint32_t PopRec__8DEQBlock(uint32_t self, uint32_t list) {
    maddr r = deq_rec(self, R32(list));
    if ((int32_t)R32(r) < 0) {
        W32(list, 0xffffffff);
        return 0;
    }
    uint32_t next = R32(r);
    W32(deq_rec(self, next) + 4, next);
    W32(list, R32(r));
    return 0;
}

/* 000656d0: push record i at the head of a list */
uint32_t PushRec__8DEQBlock(uint32_t self, uint32_t list, uint32_t i) {
    maddr r = deq_rec(self, i);
    if ((int32_t)R32(list) < 0) {
        W32(r, 0xffffffff);
    } else {
        W32(deq_rec(self, R32(list)) + 4, i);
        W32(r, R32(list));
    }
    W32(r + 4, i);
    W32(list, i);
    return 0;
}

/* 0006578a: new reference (1-based) holding data; 0 if the records cannot grow */
uint32_t NewRef__8DEQBlock(uint32_t self, uint32_t data) {
    ENTRY_LINK6(FN_NewRef__8DEQBlock, 8, 24);
    if ((int32_t)R32(self + 6) >= 0) {
        uint32_t i = R32(self + 6);
        PopRec__8DEQBlock(self, self + 6);
        PushRec__8DEQBlock(self, self + 2, i);
        W32(deq_rec(self, R32(self + 2)) + 8, data);
        return R32(self + 2) + 1;
    }
    _HUnlock(R32(self + 0xa));
    _SetHandleSize(R32(self + 0xa), ((uint32_t)(int32_t)R16(self) + 1) * 12u);
    if (R16(LM_MemErr) != 0) {
        _HLock(R32(self + 0xa));
        return 0;
    }
    _HLock(R32(self + 0xa));
    PushRec__8DEQBlock(self, self + 2, (uint32_t)(int32_t)R16(self));
    W32(deq_rec(self, R32(self + 2)) + 8, data);
    W16(self, R16(self) + 1);
    return (uint32_t)(int32_t)R16(self);
}

/* 000658ba: free a reference (back to the free list) */
uint32_t FreeRef__8DEQBlock(uint32_t self, uint32_t ref) {
    ENTRY_LINK6(FN_FreeRef__8DEQBlock, 0xc, 32);
    uint32_t i = ref - 1;
    maddr r = deq_rec(self, i);
    W32(r + 8, 0);
    if (i == R32(self + 2)) {
        PopRec__8DEQBlock(self, self + 2);
    } else {
        uint32_t prev = R32(r + 4);
        uint32_t next = R32(r);
        W32(deq_rec(self, prev), next);
        next = R32(r);
        if ((int32_t)next >= 0) W32(deq_rec(self, next) + 4, R32(r + 4));
    }
    PushRec__8DEQBlock(self, self + 6, i);
    return 0;
}

/* ========================================================================= os == */

/* 000659ac: the A4 register */
uint32_t FUN_000659ac(void) {
    return cpu_get(R_A4);
}

/* 000659b6: returns its argument and pops it */
uint32_t FUN_000659b6(uint32_t v) {
    return v;
}

/* 000659c4: in supervisor mode, mask (on != 0) or unmask the interrupts */
uint32_t FUN_000659c4(int16_t on) {
    uint16_t sr = (uint16_t)cpu_get(R_SR);
    if (sr & 0x2000) {
        if (on != 0) cpu_set(R_SR, sr | 0x700);
        else cpu_set(R_SR, sr & 0xf0ff);
    }
    return (cpu_get(R_D0) & 0xffff0000u) | (sr & 0x2000);
}

/* 000659e8: call fn with count words taken from the low words of the longs at argv:
 * count > 0 as a C function (result D0), count < 0 as a Pascal function (result
 * read above its arguments). Pascal, 4-byte result; it leaves 2*count in D3.
 * The index has no result slot for it (hand-written glue): a C caller reserves the
 * 4-byte slot above the arguments itself, like the original "subq.w #4,SP". */
uint32_t FUN_000659e8(uint32_t argv, int16_t count, uint32_t fn) {
    ENTRY_LINK6(FN_FUN_000659e8, 4, 0);
    cpu_set(R_D3, (cpu_get(R_D3) & 0xffff0000u) | (uint16_t)(count + count));
    maddr a0 = argv;
    uint32_t d0;
    if (count >= 0) {
        for (int16_t d1 = count; d1 > 0; d1--) { push16((uint16_t)R32(a0)); a0 += 4; }
        d0 = call68_addr(fn);
        W32(a6 - 4, d0);
    } else {
        for (int16_t d1 = count; d1 < 0; d1++) { push16((uint16_t)R32(a0)); a0 += 4; }
        d0 = call68_addr(fn);
        /* the callee popped its arguments: its result is at the old SP (a6-4) */
    }
    W32(args_ + 10, R32(a6 - 4));
    s_d0 = d0;
    return R32(args_ + 10);
}
void glue_FUN_000659e8(void) {
    maddr sp = emu_sp();
    FUN_000659e8(R32(sp), R16(sp + 4), R32(sp + 6));
    emu_set_sp(sp + 10);
    emu_set_d0(s_d0);
}

/* 00065a42: activate (flag != 0) or deactivate a client (-1: the current one) and call
 * its activate hook */
uint32_t osActivate(int16_t flag, uint32_t client) {
    ENTRY_LINK6(FN_osActivate, 0, 20);
    (void)flag; (void)client;
    maddr pc = args_ + 2;
    uint32_t c = R32(pc) == 0xffffffff ? osGetClient() : R32(pc);
    W32(pc, c);
    if (c == 0) {
        W16(gOsError, 0x2743);
        return 0x2743;
    }
    int doit;
    if (R16(args_) != 0 && R32(pc) != R32(gOsActive)) doit = 1;
    else doit = R16(args_) == 0 && R32(pc) == R32(gOsActive);
    if (doit) {
        if (R16(args_) != 0) {
            uint32_t v = R32(pc);
            W32(gOsActive, v);
            W32(gOsClients, v);
        } else {
            W32(gOsActive, 0);
        }
        maddr h = R32(pc);
        maddr rec = R32(h);
        if (R32(rec + 0x10) != 0)
            call_ptr(R32(rec + 0x10), "wl", (uint32_t)(uint16_t)R16(args_), h);
    }
    return 0;
}

/* 00065af2: osCallClient(client, &result, fn, count, args...) (dead code) */
uint32_t osCallClient(uint32_t client, uint32_t result, uint32_t fn, int16_t count) {
    ENTRY_LINK6(FN_osCallClient, 0, 12);
    (void)client; (void)result; (void)fn; (void)count;
    uint32_t d3 = cpu_get(R_D3);
    uint32_t c = R32(args_) == 0xffffffff ? osGetClient() : R32(args_);
    W32(args_, c);
    if (c == 0) {
        W16(gOsError, 0x2743);
        return 0x2743;
    }
    maddr sp0 = emu_sp();
    emu_set_sp(sp0 - 4);                      /* FUN_000659e8's result slot */
    FUN_000659e8(args_ + 0xe, R16(args_ + 0xc), R32(args_ + 8));
    emu_set_sp(sp0);
    uint32_t r = R32(sp0 - 4);
    cpu_set(R_D3, d3);
    W32(R32(args_ + 4), r);
    W16(gOsError, 0);
    return r & 0xffff0000u;
}

/* 00065b62: create an interrupt task proc(refcon, task) for a client (dead code) */
uint32_t osCreateITask(uint32_t refcon, uint32_t proc, uint32_t client) {
    ENTRY_LINK6(FN_osCreateITask, 8, 32);
    (void)refcon; (void)proc; (void)client;
    maddr pc = args_ + 8;
    uint32_t c = R32(pc) == 0xffffffff ? osGetClient() : R32(pc);
    W32(pc, c);
    if (c == 0) {
        W16(gOsError, 0x2743);
        return 0;
    }
    maddr rec = R32(R32(pc));
    maddr t = _NewPtr(0x14);
    W32(a6 - 8, t);
    if (t == 0) {
        W16(gOsError, R16(LM_MemErr));
        return 0;
    }
    tc_memset(R32(a6 - 8), 0, 0x14);
    t = R32(a6 - 8);
    W32(t + 8, R32(args_ + 8));
    W32(t + 0xc, R32(args_ + 4));
    W32(t + 0x10, R32(args_));
    if (R32(rec + 0x14) == 0) {
        t = R32(a6 - 8);
        W32(t, t);
        W32(t + 4, t);
        if ((uint16_t)FUN_00066788() != 0) {
            _DisposePtr(R32(a6 - 8));
            W16(gOsError, 1);
            return 0;
        }
    } else {
        t = R32(a6 - 8);
        W32(t + 4, R32(rec + 0x14));
        W32(t, R32(R32(rec + 0x14)));
        W32(R32(R32(rec + 0x14)) + 4, t);
        W32(R32(rec + 0x14), t);
    }
    t = R32(a6 - 8);
    W32(rec + 0x14, t);
    W16(gOsError, 0);
    return t;
}

/* 00065c88: unlink and free an interrupt task */
uint32_t osDestroyITask(uint32_t t) {
    ENTRY_LINK6(FN_osDestroyITask, 8, 20);
    if (t == 0) {
        W16(gOsError, 0x2744);
        return 0x2744;
    }
    maddr rec = R32(R32(t + 8));
    W32(a6 - 8, rec);
    if (R32(t + 4) == t) {
        W32(rec + 0x14, 0);
    } else {
        W32(R32(t + 4), R32(t));
        uint32_t prev = R32(t + 4);
        W32(R32(a6 - 8) + 0x14, prev);
        W32(R32(t) + 4, prev);
    }
    _DisposePtr(t);
    if (R32(R32(a6 - 8) + 0x14) == 0) FUN_000667d8();
    W16(gOsError, 0);
    return 0;
}

/* 00065d2a: the current client handle */
uint32_t osGetClient(void) {
    maddr h = R32(gOsClients);
    W32(gOsClients, h);
    return h;
}

/* 00065d5e: the client's record (dereferenced handle; -1: the current client) */
uint32_t osGetClientBytes(uint32_t client) {
    ENTRY_LINK6(FN_osGetClientBytes, 0, 12);
    (void)client;
    uint32_t c = R32(args_) == 0xffffffff ? osGetClient() : R32(args_);
    W32(args_, c);
    if (c == 0) {
        W16(gOsError, 0x2743);
        return 0;
    }
    W16(gOsError, 0);
    return R32(R32(args_));
}

/* 00065dca: the client's instance (low word of +c) */
uint32_t osGetClientInstance(uint32_t client) {
    ENTRY_LINK6(FN_osGetClientInstance, 0, 12);
    (void)client;
    uint32_t c = R32(args_) == 0xffffffff ? osGetClient() : R32(args_);
    W32(args_, c);
    if (c == 0) {
        W16(gOsError, 0x2743);
        return 0;
    }
    W16(gOsError, 0);
    return R32(R32(R32(args_)) + 0xc);
}

/* 00065e3e */
uint32_t osGetError(void) {
    return RU16(gOsError);
}

/* 00065e5c: create a client (first call: Gestalt info, timer task, MaxApplZone) and
 * activate it; its handle or 0 */
uint32_t osInit(int16_t unused) {
    ENTRY_LINK6(FN_osInit, 0x18, 32);
    (void)unused;
    maddr resp = a6 - 0x18;
    tc_Gestalt(resp, 0x70726f63);                 /* 'proc' */
    W16(gOsCPU, R16(resp + 2) - 1);
    tc_Gestalt(resp, 0x73797376);                 /* 'sysv' */
    W16(gOsSysVersion, R16(resp + 2));
    tc_Gestalt(resp, 0x6d616368);                 /* 'mach' */
    W16(gOsMachine, R16(resp + 2));
    tc_Gestalt(resp, 0x6d6d7520);                 /* 'mmu ' */
    W16(gOsMMU, R16(resp + 2));
    tc_Gestalt(resp, 0x61646472);                 /* 'addr' */
    W16(gOsAddrMode, R16(resp + 2));
    uint16_t err;
    if (R16(gOsInitCount) == 0) {
        W16(gOsClientBytes, 0xffff);
        tc_memset(gOsTimer, 0, 0x3e);
        if (R16(gOsClientBytes) != 0) { W16(gOsError, 0x2745); return 0; }
        if (RU16(gOsSysVersion) < 0x607) { W16(gOsError, 0x2745); return 0; }
        _MaxApplZone();
        _MoreMasters();
        W32(gOsStartTicks, _TickCount());
    }
    maddr h = _NewHandle((uint32_t)RU16(gOsClientBytes) + 0x24);
    W32(a6 - 0x10, h);
    if (h == 0) {
        err = RU16(LM_MemErr);
        goto fail;
    }
    _HLock(R32(a6 - 0x10));
    maddr p = R32(R32(a6 - 0x10));
    W32(a6 - 0xc, p);
    tc_memset(p, 0, (uint32_t)RU16(gOsClientBytes) + 0x24);
    p = R32(a6 - 0xc);
    W32(p + 8, R32(a6 - 0x10));
    W32(p + 0xc, R32(R32(gInstancePtr)));
    if (R32(gOsClients) != 0) {
        maddr first = R32(gOsClients);
        maddr frec = R32(first);
        maddr q = R32(a6 - 0xc);
        W32(q, first);
        W32(q + 4, R32(frec + 4));
        uint32_t nh = R32(a6 - 0x10);
        W32(R32(R32(frec + 4)), nh);
        W32(frec + 4, nh);
    } else {
        uint32_t nh = R32(a6 - 0x10);
        maddr q = R32(a6 - 0xc);
        W32(q, nh);
        W32(q + 4, nh);
    }
    W32(gOsClients, R32(a6 - 0x10));
    err = (uint16_t)threadInit(R32(R32(a6 - 0xc) + 8));
    if (err != 0) {
        maddr q = R32(a6 - 0xc);
        maddr next = R32(q);
        maddr self = R32(a6 - 0x10);
        W32(gOsClients, next == self ? 0 : next);
        W32(R32(R32(q + 4)), R32(q));
        W32(R32(R32(q)) + 4, R32(q + 4));
        _HUnlock(self);
        _DisposeHandle(self);
        goto fail;
    }
    W16(gOsInitCount, R16(gOsInitCount) + 1);
    err = (uint16_t)osActivate(1, R32(R32(a6 - 0xc) + 8));
    if (err != 0) {
        osQuit();
        W16(gOsError, err);
        return 0;
    }
    return R32(R32(a6 - 0xc) + 8);
fail:
    W16(a6 - 4, err);
    W16(gOsError, err);
    return 0;
}

/* 000660a2: interrupt state; the previous one */
uint32_t osIntrEnable(int16_t on) {
    ENTRY_LINK6(FN_osIntrEnable, 0, 12);
    uint16_t old = RU16(gOsIntrState);
    W16(gOsIntrState, on);
    FUN_000659c4(on);
    return old;
}

/* VM present for osPageLock/osPageUnlock: system >= 7.0, an MMU, not a Mac II */
static int os_vm_absent(void) {
    if (RU16(gOsSysVersion) < 0x700) return 1;
    if (RU16(gOsCPU) < 3 && RU16(gOsMMU) < 2) return 1;
    if (R16(gOsMachine) == 8) return 1;
    return 0;
}

/* 000660e2: LockMemory(addr, count) */
uint32_t osPageLock(uint32_t count, uint32_t addr) {
    ENTRY_LINK6(FN_osPageLock, 0, 20);
    if (os_vm_absent()) { W16(gOsError, 0); return 0; }
    if ((uint16_t)trap_MemoryDispatch(2, addr, count) != 0) { W16(gOsError, 0x2710); return 0x2710; }
    W16(gOsError, 0);
    return 0;
}

/* 00066162: UnlockMemory(addr, count) */
uint32_t osPageUnlock(uint32_t count, uint32_t addr) {
    ENTRY_LINK6(FN_osPageUnlock, 0, 20);
    if (os_vm_absent()) { W16(gOsError, 0); return 0; }
    if ((uint16_t)trap_MemoryDispatch(3, addr, count) != 0) { W16(gOsError, 0x2710); return 0x2710; }
    W16(gOsError, 0);
    return 0;
}

/* 000661e4: is the client the active one (-1 on error) */
uint32_t osQueryActive(uint32_t client) {
    ENTRY_LINK6(FN_osQueryActive, 0, 12);
    (void)client;
    uint32_t c = R32(args_) == 0xffffffff ? osGetClient() : R32(args_);
    W32(args_, c);
    if (c == 0) {
        W16(gOsError, 0x2743);
        return 0xffff;
    }
    W16(gOsError, 0);
    return R32(gOsActive) == R32(args_) ? 1 : 0;
}

/* 0006625a: 0x100 when os is initialised and there is a current client */
uint32_t osQueryInit(void) {
    ENTRY_LINK6(FN_osQueryInit, 4, 4);
    uint32_t c = osGetClient();
    if (R16(gOsInitCount) != 0) {
        if (c == 0xffffffff) c = osGetClient();
        if (c != 0) return 0x100;
    }
    return 0;
}

/* 000662be: release the current client (its tasks, the thread layer, its handle) */
uint32_t osQuit(void) {
    ENTRY_LINK6(FN_osQuit, 0, 16);
    uint32_t r = (uint16_t)osQueryInit();
    if (r == 0) {
        W16(gOsError, 0x2742);
        return r;
    }
    W16(gOsInitCount, R16(gOsInitCount) - 1);
    if (R16(gOsInitCount) != 0) return r;
    maddr h = osGetClient();
    osActivate(0, h);
    maddr rec = R32(h);
    while (R32(rec + 0x14) != 0) osDestroyITask(R32(rec + 0x14));
    threadQuit(h);
    if (R32(rec) == h) {
        W32(gOsClients, 0);
    } else {
        W32(R32(R32(rec)) + 4, R32(rec + 4));
        W32(R32(R32(rec + 4)), R32(rec));
        W32(gOsClients, R32(rec));
    }
    _HUnlock(h);
    _DisposeHandle(h);
    return _RmvTime(gOsTimer);
}

/* 00066366: reserve n client bytes (rounded to 4) in every client; their offset */
uint32_t osReserveClientBytes(int16_t n) {
    ENTRY_LINK6(FN_osReserveClientBytes, 0, 32);
    uint16_t size = (uint16_t)((n + 3) & 0xfffc);
    maddr h = R32(gOsClients);
    if (h != 0) {
        do {
            _HUnlock(h);
            _SetHandleSize(h, (uint32_t)RU16(gOsClientBytes) + 0x24 + size);
            int16_t e = R16(LM_MemErr);
            _HLock(h);
            maddr p = R32(h);
            if (e != 0) {
                W16(gOsError, e);
                return 0;
            }
            tc_memset(p + 0x24 + RU16(gOsClientBytes), 0, size);
            h = R32(p);
        } while (h != R32(gOsClients));
    }
    uint16_t off = (uint16_t)(RU16(gOsClientBytes) + 0x24);
    W16(gOsClientBytes, RU16(gOsClientBytes) + size);
    W16(gOsError, 0);
    return off;
}

/* 0006643e: set the client's activate hook; the previous one (dead code) */
uint32_t osSetActivateHook(uint32_t hook, uint32_t client) {
    ENTRY_LINK6(FN_osSetActivateHook, 0, 20);
    (void)hook; (void)client;
    maddr pc = args_ + 4;
    uint32_t c = R32(pc) == 0xffffffff ? osGetClient() : R32(pc);
    W32(pc, c);
    if (c == 0) {
        W16(gOsError, 0x2743);
        return 0xffffffff;
    }
    maddr rec = R32(R32(args_ + 4));
    uint32_t old = R32(rec + 0x10);
    W32(rec + 0x10, R32(args_));
    W16(gOsError, 0);
    return old;
}

/* 000664be */
uint32_t osSetInstance(int16_t inst) {
    return (uint16_t)inst;
}

/* 000664ea: EventAvail(0) */
uint32_t osYield(void) {
    ENTRY_LINK6(FN_osYield, 0x10, 0);
    _EventAvail(0, a6 - 0x10);
    return 0;
}

/* 0006650a: run the active client's next interrupt task proc(refcon, task) */
uint32_t osExecuteIdleTask(void) {
    ENTRY_LINK6(FN_osExecuteIdleTask, 0, 12);
    maddr rec = R32(R32(gOsActive));
    maddr t = R32(rec + 0x14);
    call_ptr(R32(t + 0xc), "ll", R32(t + 0x10), t);
    if (t == R32(rec + 0x14)) W32(rec + 0x14, R32(t + 4));
    return 0;
}

/* 00066562: GetNextEvent patch: the original trap, then the idle task. Its D0 is
 * CurrentA5 with the result in the low byte; the result is the high byte of the slot. */
uint32_t osGetNextEvent(uint32_t evt, int16_t mask) {
    ENTRY_LINK6(FN_osGetNextEvent, 4, 12);
    maddr a5 = R32(LM_CurrentA5);
    uint8_t b = (uint8_t)(pas_call(R32(a5 - 0x78e), 2, "wl", (uint32_t)(uint16_t)mask, evt) >> 8);
    W8(a6 - 4, b);
    osExecuteIdleTask();
    W8(args_ + 6, R8(a6 - 4));
    s_d0 = (a5 & 0xffffff00u) | R8(a6 - 4);
    return RU16(args_ + 6);
}
void glue_osGetNextEvent(void) {
    maddr sp = emu_sp();
    osGetNextEvent(R32(sp), R16(sp + 4));
    emu_set_sp(sp + 6);
    emu_set_d0(s_d0);
}

/* 000665ba: milliseconds since the timer started (the Time Manager task counts down
 * from 0x5200000 microseconds; a negative remaining time is in microseconds) */
uint32_t osGetTime(void) {
    ENTRY_LINK6(FN_osGetTime, 0, 16);
    _RmvTime(gOsTimer);
    int32_t left = (int32_t)R32(gOsTimerCount);
    W32(gOsTimerCount, 0);
    W32(gOsTimerWakeUp, 0);
    W32(gOsTimerRsvd, 0);
    _InsTime(gOsTimer);
    _PrimeTime(gOsTimer, 0x5200000);
    if (left >= 0) {
        W32(gOsTimeMs, R32(gOsTimeMs) + (uint32_t)(0x5200000 - left));
    } else {
        int32_t us = -left;
        int32_t rem = us % 1000;
        if (rem != 0) {
            W32(gOsTimeMs, R32(gOsTimeMs) + (uint32_t)(0x5200000 - us / 1000 - 1));
            W32(gOsTimeFrac, R32(gOsTimeFrac) + (uint32_t)(1000 - rem));
            if (R32(gOsTimeFrac) >= 1000) {
                W32(gOsTimeFrac, R32(gOsTimeFrac) - 1000);
                W32(gOsTimeMs, R32(gOsTimeMs) + 1);
            }
        } else {
            W32(gOsTimeMs, R32(gOsTimeMs) + (uint32_t)(0x5200000 - us / 1000));
        }
    }
    return R32(gOsTimeMs);
}

/* 0006669e: Time Manager task (A1 = the TMTask, the app's A5 stored after it): rearm
 * for 0x5200000 microseconds and add 0x5200000 microseconds to the clock. Returns
 * that A5 in D0. */
uint32_t osResetSystemTimer(void) {
    ENTRY_LINK6(FN_osResetSystemTimer, 0, 12);
    maddr a5 = R32(cpu_get(R_A1) + 0x16);
    W32(a5 - 0x7aa, 0);
    W32(a5 - 0x7a6, 0);
    W32(a5 - 0x7a2, 0);
    _PrimeTime(a5 - 0x7b4, 0x5200000);
    W32(a5 - 0x79a, R32(a5 - 0x79a) - 0x14fdf);
    W32(a5 - 0x796, R32(a5 - 0x796) - 0xe8);
    return a5;
}
void glue_osResetSystemTimer(void) {
    emu_set_d0(osResetSystemTimer());
}

/* 00066704: WaitNextEvent patch: WaitNextEvent(sleep 0) and the idle task until an
 * event comes or `sleep` ticks pass. D0 and the result as in osGetNextEvent. */
uint32_t osWaitNextEvent(uint32_t rgn, uint32_t sleep, uint32_t evt, int16_t mask) {
    ENTRY_LINK6(FN_osWaitNextEvent, 4, 24);
    maddr a5 = R32(LM_CurrentA5);
    uint32_t start = _TickCount();
    while (_TickCount() - start <= sleep) {
        uint8_t b = (uint8_t)(pas_call(R32(a5 - 0x78a), 2, "wlll", (uint32_t)(uint16_t)mask, evt, 0u, rgn) >> 8);
        W8(a6 - 4, b);
        if (b != 0) break;
        osExecuteIdleTask();
    }
    W8(args_ + 0xe, R8(a6 - 4));
    s_d0 = (a5 & 0xffffff00u) | R8(a6 - 4);
    return RU16(args_ + 0xe);
}
void glue_osWaitNextEvent(void) {
    maddr sp = emu_sp();
    osWaitNextEvent(R32(sp), R32(sp + 4), R32(sp + 8), R16(sp + 12));
    emu_set_sp(sp + 14);
    emu_set_d0(s_d0);
}

/* 00066788: patch GetNextEvent/WaitNextEvent with osGetNextEvent/osWaitNextEvent
 * (unless they are already patched above the limit); 0x2745 when they are */
uint32_t FUN_00066788(void) {
    ENTRY_NOLINK(FN_FUN_00066788, 0);
    maddr t = _GetTrapAddress(0xa970);
    W32(gOsOldGNE, t);
    if (t <= R32(R32(gTrapLimitPtr))) {
        t = _GetTrapAddress(0xa860);
        W32(gOsOldWNE, t);
        if (t <= R32(R32(gTrapLimitPtr))) {
            _SetTrapAddress(JT_ADDR(FN_osGetNextEvent), 0xa970);
            return _SetTrapAddress(JT_ADDR(FN_osWaitNextEvent), 0xa860);
        }
    }
    W32(gOsOldGNE, 0);
    W32(gOsOldWNE, 0);
    return 0x2745;
}

/* 000667d8: restore GetNextEvent/WaitNextEvent */
uint32_t FUN_000667d8(void) {
    ENTRY_NOLINK(FN_FUN_000667d8, 0);
    _SetTrapAddress(R32(gOsOldGNE), 0xa970);
    _SetTrapAddress(R32(gOsOldWNE), 0xa860);
    return 0;
}

/* 000667ee */
uint32_t threadInit(uint32_t client) {
    (void)client;
    W16(gThreadState, 0);
    return 0;
}

/* 00066820 */
uint32_t threadQuit(uint32_t client) {
    (void)client;
    return 0;
}
