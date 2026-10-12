/* Memory Manager: pointers and handles in the emulated address space.
 *
 * Every block has a 16-byte header just before its data:
 *   +0  u32 logical size
 *   +4  u32 physical size (header + data, 16-byte aligned)
 *   +8  u32 master pointer address (0 for nonrelocatable)
 *   +12 u8  flags (MF_*)
 *   +13 u8  heap index
 * Blocks never move except when SetHandleSize must grow an unlocked handle. */
#include "mac.h"
#include <stdio.h>
#include <stdlib.h>

uint8_t *M;

#define HDR 16
#define MF_USED   0x01
#define MF_RELOC  0x02
#define MF_RSRC   0x20   /* same bit positions as HGetState */
#define MF_PURGE  0x40
#define MF_LOCK   0x80

typedef struct { maddr start, end, zone; } Heap;
static Heap heaps[2];
static maddr mp_free = 0;      /* free list of master pointers (linked through the pointer value) */

static inline uint32_t align16(uint32_t v) { return (v + 15) & ~15u; }

void pstr_to_c(maddr p, char *out, size_t outsz) {
    unsigned n = rd8(p);
    if (n >= outsz) n = (unsigned)outsz - 1;
    for (unsigned i = 0; i < n; i++) out[i] = (char)rd8(p + 1 + i);
    out[n] = 0;
}
void c_to_pstr(const char *s, maddr p) {
    size_t n = strlen(s); if (n > 255) n = 255;
    wr8(p, (uint8_t)n);
    for (size_t i = 0; i < n; i++) wr8(p + 1 + (maddr)i, (uint8_t)s[i]);
}

static int heap_of(maddr a) {
    for (int i = 0; i < 2; i++) if (a >= heaps[i].start && a < heaps[i].end) return i;
    return -1;
}

static void heap_init(int h, maddr start, maddr end) {
    heaps[h].start = start; heaps[h].end = end;
    /* fake Zone header (52 bytes) at start; first block after it */
    heaps[h].zone = start;
    mset(start, 0, 64);
    wr32(start + 0, end);            /* bkLim */
    maddr b = start + 64;
    wr32(b + 0, 0); wr32(b + 4, end - b); wr32(b + 8, 0); wr8(b + 12, 0); wr8(b + 13, (uint8_t)h);
}

void mm_init(void) {
    M = calloc(1, MEM_SIZE);
    if (!M) { fprintf(stderr, "out of host memory\n"); exit(1); }
    heap_init(0, SYSHEAP_START, SYSHEAP_END);
    heap_init(1, APPHEAP_START, APPHEAP_END);
    wr32(LM_SysZone, heaps[0].zone);
    wr32(LM_ApplZone, heaps[1].zone);
    wr32(LM_TheZone, heaps[1].zone);
    wr32(LM_HeapEnd, APPHEAP_END);
    wr32(LM_ApplLimit, APPHEAP_END);
}

maddr mm_zone_for(int sys) { return heaps[sys ? 0 : 1].zone; }
static int cur_heap(void) { return rd32(LM_TheZone) == heaps[0].zone ? 0 : 1; }

static void coalesce(int h) {
    maddr b = heaps[h].start + 64;
    while (b < heaps[h].end) {
        uint32_t phys = rd32(b + 4);
        if (!(rd8(b + 12) & MF_USED)) {
            maddr n = b + phys;
            while (n < heaps[h].end && !(rd8(n + 12) & MF_USED)) { phys += rd32(n + 4); n = b + phys; }
            wr32(b + 4, phys);
        }
        b += phys;
    }
}

static int purge_some(int h, uint32_t need);

static maddr alloc_block_(int h, uint32_t size, uint8_t flags);
static maddr alloc_block(int h, uint32_t size, uint8_t flags) {
    maddr r = alloc_block_(h, size, flags);
    static int lo = -1; static uint32_t wlo, whi;
    if (lo < 0) { const char *e = getenv("POP2_ALLOCWATCH"); lo = e != NULL; if (e) { wlo = (uint32_t)strtoul(e, NULL, 16); whi = wlo + 0x10000; } }
    if (lo && r && r <= whi && r + size >= wlo) fprintf(stderr, "[mem] alloc %06x size %u (pc %06x)\n", r, size, trap_ret);
    return r;
}
static maddr alloc_block_(int h, uint32_t size, uint8_t flags) {
    uint32_t phys = align16(size + HDR);
    if (phys < 32) phys = 32;
    for (int pass = 0; pass < 3; pass++) {
        maddr b = heaps[h].start + 64;
        while (b < heaps[h].end) {
            uint32_t bp = rd32(b + 4);
            if (bp == 0) { fprintf(stderr, "heap %d corrupt at %06x\n", h, b); abort(); }
            if (!(rd8(b + 12) & MF_USED) && bp >= phys) {
                if (bp - phys >= 32) {   /* split */
                    maddr n = b + phys;
                    wr32(n + 0, 0); wr32(n + 4, bp - phys); wr32(n + 8, 0); wr8(n + 12, 0); wr8(n + 13, (uint8_t)h);
                    bp = phys;
                }
                wr32(b + 0, size); wr32(b + 4, bp); wr32(b + 8, 0); wr8(b + 12, flags | MF_USED); wr8(b + 13, (uint8_t)h);
                return b + HDR;
            }
            b += bp;
        }
        if (pass == 0) coalesce(h);
        else if (pass == 1 && !purge_some(h, phys)) break;
    }
    return 0;
}

static void free_block(maddr p) {
    maddr b = p - HDR;
    wr8(b + 12, 0); wr32(b + 8, 0);
}

static int valid_block(maddr p) {
    if (!p) return 0;
    p &= ADDR_MASK;
    if (heap_of(p) < 0) return 0;
    return (rd8(p - HDR + 12) & MF_USED) != 0;
}

/* POP2_MEMERR_LOG=1: report every Memory Manager error with the trap and its caller. */
void mm_set_memerr(int16_t e) {
    static int log = -1;
    if (log < 0) log = getenv("POP2_MEMERR_LOG") != NULL;
    if (e && log) {
        extern uint16_t cur_trap_word; extern uint32_t cpu_get(int r);
        extern int loader_locate(maddr a, int *seg, uint32_t *off);
        int sg = 0; uint32_t of = 0; maddr pc = cpu_get(16);
        loader_locate(pc, &sg, &of);
        fprintf(stderr, "[memerr] %d trap %04X at seg%d+%04x A0=%06x\n", e, cur_trap_word, sg, of, cpu_get(8));
    }
    wr16(LM_MemErr, (uint16_t)e);
}

/* ---------------- pointers ---------------- */
maddr mm_new_ptr_in(int h, uint32_t size, int clear) {
    maddr p = alloc_block(h, size, 0);
    if (!p) { mm_set_memerr(-108); return 0; }
    if (clear) mset(p, 0, size);
    mm_set_memerr(0);
    return p;
}
maddr NewPtr(uint32_t size) { return mm_new_ptr_in(cur_heap(), size, 0); }
maddr NewPtrClear(uint32_t size) { return mm_new_ptr_in(cur_heap(), size, 1); }
maddr NewPtrSys(uint32_t size) { return mm_new_ptr_in(0, size, 1); }
void DisposePtr(maddr p) { if (valid_block(p)) free_block(p & ADDR_MASK); mm_set_memerr(0); }
uint32_t GetPtrSize(maddr p) { if (!valid_block(p)) { mm_set_memerr(-111); return 0; } mm_set_memerr(0); return rd32((p & ADDR_MASK) - HDR); }

static int resize_in_place(maddr p, uint32_t size) {
    maddr b = p - HDR;
    int h = rd8(b + 13);
    uint32_t phys = rd32(b + 4);
    uint32_t need = align16(size + HDR);
    if (need <= phys) {
        if (phys - need >= 32) {
            maddr n = b + need;
            wr32(n + 0, 0); wr32(n + 4, phys - need); wr32(n + 8, 0); wr8(n + 12, 0); wr8(n + 13, (uint8_t)h);
            wr32(b + 4, need);
        }
        wr32(b + 0, size);
        return 1;
    }
    /* absorb following free blocks */
    maddr n = b + phys;
    uint32_t avail = phys;
    while (n < heaps[h].end && !(rd8(n + 12) & MF_USED) && avail < need) { avail += rd32(n + 4); n = b + avail; }
    if (avail >= need) {
        wr32(b + 4, avail);
        return resize_in_place(p, size);
    }
    return 0;
}

void SetPtrSize(maddr p, uint32_t size) {
    if (!valid_block(p)) { mm_set_memerr(-111); return; }
    mm_set_memerr(resize_in_place(p & ADDR_MASK, size) ? 0 : -108);
}

/* ---------------- handles ---------------- */
static maddr new_master(int h) {
    if (!mp_free) {
        maddr blk = alloc_block(h, 64 * 4, 0);
        if (!blk) return 0;
        for (int i = 0; i < 64; i++) wr32(blk + i * 4, i < 63 ? blk + (i + 1) * 4 : 0);
        mp_free = blk;
    }
    maddr m = mp_free;
    mp_free = rd32(m);
    wr32(m, 0);
    return m;
}
static void free_master(maddr m) { wr32(m, mp_free); mp_free = m; }

maddr mm_new_handle_in(int h, uint32_t size, int clear) {
    maddr m = new_master(h);
    if (!m) { mm_set_memerr(-108); return 0; }
    maddr p = alloc_block(h, size, MF_RELOC);
    if (!p) { free_master(m); mm_set_memerr(-108); return 0; }
    wr32(p - HDR + 8, m);
    wr32(m, p);
    if (clear) mset(p, 0, size);
    mm_set_memerr(0);
    return m;
}
maddr NewHandle(uint32_t size) { return mm_new_handle_in(cur_heap(), size, 0); }
maddr NewHandleClear(uint32_t size) { return mm_new_handle_in(cur_heap(), size, 1); }
maddr NewHandleSys(uint32_t size) { return mm_new_handle_in(0, size, 1); }

maddr NewEmptyHandle(void) {
    maddr m = new_master(cur_heap());
    mm_set_memerr(m ? 0 : -108);
    return m;
}

static int valid_handle(maddr hnd) { return hnd && heap_of(hnd & ADDR_MASK) >= 0; }

void DisposeHandle(maddr hnd) {
    /* DisposeHandle(NULL) is a harmless no-op with noErr on the real Memory Manager; the
     * Mohawk memory library relies on it when freeing an entry whose block was purged. */
    if (!(hnd & ADDR_MASK)) { mm_set_memerr(0); return; }
    if (!valid_handle(hnd)) { mm_set_memerr(-109); return; }
    hnd &= ADDR_MASK;
    maddr p = rd32(hnd) & ADDR_MASK;
    if (p && valid_block(p)) free_block(p);
    free_master(hnd);
    mm_set_memerr(0);
}

uint32_t GetHandleSize(maddr hnd) {
    if (!valid_handle(hnd)) { mm_set_memerr(-109); return 0; }
    maddr p = rd32(hnd) & ADDR_MASK;
    if (!p) { mm_set_memerr(-109); return 0; }
    mm_set_memerr(0);
    return rd32(p - HDR);
}

void SetHandleSize(maddr hnd, uint32_t size) {
    if (!valid_handle(hnd)) { mm_set_memerr(-109); return; }
    hnd &= ADDR_MASK;
    maddr p = rd32(hnd) & ADDR_MASK;
    if (!p) {   /* empty handle: reallocate */
        maddr np = alloc_block(heap_of(hnd), size, MF_RELOC);
        if (!np) { mm_set_memerr(-108); return; }
        wr32(np - HDR + 8, hnd); wr32(hnd, np); mm_set_memerr(0); return;
    }
    if (resize_in_place(p, size)) { mm_set_memerr(0); return; }
    uint8_t fl = rd8(p - HDR + 12);
    if (fl & MF_LOCK) { mm_set_memerr(-117); return; }    /* memLockedErr */
    maddr np = alloc_block(rd8(p - HDR + 13), size, fl & ~MF_USED);
    if (!np) { mm_set_memerr(-108); return; }
    uint32_t old = rd32(p - HDR);
    mcopy(np, p, old < size ? old : size);
    wr32(np - HDR + 8, hnd);
    free_block(p);
    wr32(hnd, np);
    mm_set_memerr(0);
}

static maddr hblock(maddr hnd) {
    if (!valid_handle(hnd)) return 0;
    maddr p = rd32(hnd & ADDR_MASK) & ADDR_MASK;
    return p ? p - HDR : 0;
}
uint8_t HGetState(maddr hnd) { maddr b = hblock(hnd); mm_set_memerr(b ? 0 : -109); return b ? (rd8(b + 12) & 0xE0) : 0; }
void HSetState(maddr hnd, uint8_t st) { maddr b = hblock(hnd); if (b) wr8(b + 12, (rd8(b + 12) & 0x1F) | (st & 0xE0)); mm_set_memerr(b ? 0 : -109); }
static void hflag(maddr hnd, uint8_t f, int on) { maddr b = hblock(hnd); if (b) wr8(b + 12, on ? (rd8(b + 12) | f) : (rd8(b + 12) & ~f)); mm_set_memerr(b ? 0 : -109); }
void HLock(maddr h)    { hflag(h, MF_LOCK, 1); }
void HUnlock(maddr h)  { hflag(h, MF_LOCK, 0); }
void HPurge(maddr h)   { hflag(h, MF_PURGE, 1); }
void HNoPurge(maddr h) { hflag(h, MF_PURGE, 0); }
void HSetRBit(maddr h) { hflag(h, MF_RSRC, 1); }
void HClrRBit(maddr h) { hflag(h, MF_RSRC, 0); }

maddr RecoverHandle(maddr p) {
    if (!valid_block(p)) { mm_set_memerr(-111); return 0; }
    mm_set_memerr(0);
    return rd32((p & ADDR_MASK) - HDR + 8);
}

void EmptyHandle(maddr hnd) {
    maddr b = hblock(hnd);
    if (b) { free_block(b + HDR); wr32(hnd & ADDR_MASK, 0); }
    mm_set_memerr(0);
}

/* Purge unlocked purgeable handles until a block of `need` bytes could fit. */
void (*mm_purge_notify)(maddr hnd) = 0;
static int purge_some(int h, uint32_t need) {
    int any = 0;
    maddr b = heaps[h].start + 64;
    while (b < heaps[h].end) {
        uint8_t fl = rd8(b + 12);
        if ((fl & MF_USED) && (fl & MF_RELOC) && (fl & MF_PURGE) && !(fl & MF_LOCK)) {
            maddr m = rd32(b + 8);
            if (mm_purge_notify) mm_purge_notify(m);
            free_block(b + HDR);
            wr32(m, 0);
            any = 1;
        }
        b += rd32(b + 4);
    }
    (void)need;
    if (any) coalesce(h);
    return any;
}

uint32_t mm_free_bytes(int h, uint32_t *largest) {
    coalesce(h);
    uint32_t tot = 0, big = 0;
    maddr b = heaps[h].start + 64;
    while (b < heaps[h].end) {
        uint32_t bp = rd32(b + 4);
        if (!(rd8(b + 12) & MF_USED)) { tot += bp - HDR; if (bp - HDR > big) big = bp - HDR; }
        b += bp;
    }
    if (largest) *largest = big;
    return tot;
}
uint32_t FreeMem(void) { return mm_free_bytes(cur_heap(), 0); }
uint32_t MaxMem(uint32_t *grow) { uint32_t big; mm_free_bytes(cur_heap(), &big); if (grow) *grow = 0; return big; }
uint32_t CompactMem(uint32_t need) { uint32_t big; mm_free_bytes(cur_heap(), &big); (void)need; return big; }
void PurgeMem(uint32_t need) { purge_some(cur_heap(), need); }
int mm_heap_index(maddr zone) { return zone == heaps[0].zone ? 0 : 1; }
int mm_is_block(maddr p) { return valid_block(p); }
int mm_is_handle(maddr h) { maddr b = hblock(h); return b && rd32(b + 8) == (h & ADDR_MASK); }
