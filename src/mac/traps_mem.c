/* Trap glue: Memory Manager and miscellaneous OS traps. */
#include "mac.h"
#include <stdio.h>

#define CLRBIT 0x0200
#define SYSBIT 0x0400

static void set_d0_err(void) { cpu_set(R_D0, (uint32_t)(int32_t)(int16_t)rd16(LM_MemErr)); }

static void T_NewPtr(uint16_t w) {
    maddr p = (w & SYSBIT) ? mm_new_ptr_in(0, D0, w & CLRBIT) : ((w & CLRBIT) ? NewPtrClear(D0) : NewPtr(D0));
    cpu_set(R_A0, p); set_d0_err();
}
static void T_NewHandle(uint16_t w) {
    maddr h = (w & SYSBIT) ? mm_new_handle_in(0, D0, w & CLRBIT) : ((w & CLRBIT) ? NewHandleClear(D0) : NewHandle(D0));
    cpu_set(R_A0, h); set_d0_err();
}
static void T_NewEmptyHandle(uint16_t w) { (void)w; cpu_set(R_A0, NewEmptyHandle()); set_d0_err(); }
static void T_DisposePtr(uint16_t w) { (void)w; DisposePtr(A0); set_d0_err(); }
static void T_DisposeHandle(uint16_t w) { (void)w; DisposeHandle(A0); set_d0_err(); }
static void T_GetPtrSize(uint16_t w) { (void)w; uint32_t s = GetPtrSize(A0); cpu_set(R_D0, rd16(LM_MemErr) ? (uint32_t)(int32_t)(int16_t)rd16(LM_MemErr) : s); }
static void T_SetPtrSize(uint16_t w) { (void)w; SetPtrSize(A0, D0); set_d0_err(); }
static void T_GetHandleSize(uint16_t w) { (void)w; uint32_t s = GetHandleSize(A0); cpu_set(R_D0, rd16(LM_MemErr) ? (uint32_t)(int32_t)(int16_t)rd16(LM_MemErr) : s); }
static void T_SetHandleSize(uint16_t w) { (void)w; SetHandleSize(A0, D0); set_d0_err(); }
static void T_HLock(uint16_t w) { (void)w; HLock(A0); set_d0_err(); }
static void T_HUnlock(uint16_t w) { (void)w; HUnlock(A0); set_d0_err(); }
static void T_HPurge(uint16_t w) { (void)w; HPurge(A0); set_d0_err(); }
static void T_HNoPurge(uint16_t w) { (void)w; HNoPurge(A0); set_d0_err(); }
static void T_HGetState(uint16_t w) { (void)w; uint8_t s = HGetState(A0); cpu_set(R_D0, rd16(LM_MemErr) ? (uint32_t)(int32_t)(int16_t)rd16(LM_MemErr) : s); }
static void T_HSetState(uint16_t w) { (void)w; HSetState(A0, (uint8_t)D0); set_d0_err(); }
static void T_HSetRBit(uint16_t w) { (void)w; HSetRBit(A0); set_d0_err(); }
static void T_HClrRBit(uint16_t w) { (void)w; HClrRBit(A0); set_d0_err(); }
static void T_RecoverHandle(uint16_t w) { (void)w; cpu_set(R_A0, RecoverHandle(A0)); }
static void T_EmptyHandle(uint16_t w) { (void)w; EmptyHandle(A0); set_d0_err(); }
static void T_ReallocHandle(uint16_t w) { (void)w; maddr h = A0; EmptyHandle(h); SetHandleSize(h, D0); set_d0_err(); }
static void T_BlockMove(uint16_t w) { (void)w; mcopy(cpu_get(R_A1), A0, D0); cpu_set(R_D0, 0); }
static void T_FreeMem(uint16_t w) { (void)w; cpu_set(R_D0, (w & SYSBIT) ? mm_free_bytes(0, 0) : FreeMem()); }
static void T_MaxMem(uint16_t w) { uint32_t big; mm_free_bytes((w & SYSBIT) ? 0 : 1, &big); cpu_set(R_D0, big); cpu_set(R_A0, 0); }
static void T_CompactMem(uint16_t w) { uint32_t big; mm_free_bytes((w & SYSBIT) ? 0 : 1, &big); cpu_set(R_D0, big); }
static void T_PurgeMem(uint16_t w) { (void)w; PurgeMem(D0); mm_set_memerr(0); set_d0_err(); }
static void T_PurgeSpace(uint16_t w) { uint32_t big; uint32_t tot = mm_free_bytes((w & SYSBIT) ? 0 : 1, &big); cpu_set(R_D0, tot); cpu_set(R_A0, big); }
static void T_MaxBlock(uint16_t w) { uint32_t big; mm_free_bytes((w & SYSBIT) ? 0 : 1, &big); cpu_set(R_D0, big); }
static void T_ReserveMem(uint16_t w) { (void)w; uint32_t big; mm_free_bytes(1, &big); mm_set_memerr(big >= D0 ? 0 : -108); set_d0_err(); }
static void T_ok(uint16_t w) { (void)w; mm_set_memerr(0); cpu_set(R_D0, 0); }
static void T_SetZone(uint16_t w) { (void)w; wr32(LM_TheZone, A0); cpu_set(R_D0, 0); }
static void T_GetZone(uint16_t w) { (void)w; cpu_set(R_A0, rd32(LM_TheZone)); cpu_set(R_D0, 0); }
static void T_HandleZone(uint16_t w) { (void)w; cpu_set(R_A0, rd32(LM_ApplZone)); cpu_set(R_D0, 0); }
static void T_StripAddress(uint16_t w) { (void)w; cpu_set(R_D0, D0 & ADDR_MASK); }
static void T_SwapMMUMode(uint16_t w) { (void)w; cpu_set(R_D0, 1); }   /* previous mode, zero-extended: always 32-bit */
static void T_StackSpace(uint16_t w) { (void)w; cpu_set(R_D0, cpu_get(R_SP) - rd32(LM_ApplLimit)); }
static void T_SetApplLimit(uint16_t w) { (void)w; cpu_set(R_D0, 0); }
static void T_MaxApplZone(uint16_t w) { (void)w; cpu_set(R_D0, 0); }

static void T_HandToHand(uint16_t w) {
    (void)w; maddr h = A0; uint32_t sz = GetHandleSize(h);
    maddr n = NewHandle(sz);
    if (n) mcopy(rd32(n), rd32(h), sz);
    cpu_set(R_A0, n); set_d0_err();
}
static void T_PtrToHand(uint16_t w) {
    (void)w; maddr src = A0; uint32_t sz = D0;
    maddr n = NewHandle(sz);
    if (n) mcopy(rd32(n), src, sz);
    cpu_set(R_A0, n); set_d0_err();
}
static void T_PtrToXHand(uint16_t w) {
    (void)w; maddr src = A0, h = cpu_get(R_A1); uint32_t sz = D0;
    SetHandleSize(h, sz);
    if (!rd16(LM_MemErr)) mcopy(rd32(h), src, sz);
    cpu_set(R_A0, h); set_d0_err();
}
static void T_HandAndHand(uint16_t w) {
    (void)w; maddr a = A0, b = cpu_get(R_A1);
    uint32_t sa = GetHandleSize(a), sb = GetHandleSize(b);
    SetHandleSize(b, sa + sb);
    if (!rd16(LM_MemErr)) mcopy(rd32(b) + sb, rd32(a), sa);
    cpu_set(R_A0, b); set_d0_err();
}
static void T_PtrAndHand(uint16_t w) {
    (void)w; maddr p = A0, b = cpu_get(R_A1); uint32_t n = D0;
    uint32_t sb = GetHandleSize(b);
    SetHandleSize(b, sb + n);
    if (!rd16(LM_MemErr)) mcopy(rd32(b) + sb, p, n);
    cpu_set(R_A0, b); set_d0_err();
}

/* Munger(h, offset, ptr1, len1, ptr2, len2): long — Pascal */
static void T_Munger(uint16_t w) {
    (void)w;
    int32_t len2 = (int32_t)pop32(); maddr p2 = pop32();
    int32_t len1 = (int32_t)pop32(); maddr p1 = pop32();
    int32_t off = (int32_t)pop32(); maddr h = pop32();
    uint32_t hs = GetHandleSize(h);
    int32_t found = -1;
    if (p1) {
        for (int32_t i = off; i + len1 <= (int32_t)hs; i++)
            if (!memcmp(mptr(rd32(h) + i), mptr(p1), (size_t)len1)) { found = i; break; }
        if (found < 0) { ret32((uint32_t)-1); return; }
    } else {
        found = off;
        if (len1 < 0) len1 = (int32_t)hs - off;
    }
    if (!p2) { ret32((uint32_t)found); return; }
    if (len2 != len1) {
        uint32_t newsz = hs - len1 + len2;
        if (len2 > len1) SetHandleSize(h, newsz);
        maddr base = rd32(h);
        memmove(mptr(base + found + len2), mptr(base + found + len1), hs - found - len1);
        if (len2 < len1) SetHandleSize(h, newsz);
    }
    mcopy(rd32(h) + found, p2, (uint32_t)len2);
    ret32((uint32_t)(found + len2));
}

/* ---- trap table access ---- */
static uint16_t trapword_for(uint16_t trapword_of_call, uint32_t d0) {
    uint16_t n = (uint16_t)d0;
    if (trapword_of_call & 0x0200) {       /* new style: bit 10 of the call selects toolbox */
        return (trapword_of_call & 0x0400) ? (uint16_t)(0xA800 | (n & 0x3FF)) : (uint16_t)(0xA000 | (n & 0xFF));
    }
    if (n & 0x0800) return (uint16_t)(0xA800 | (n & 0x3FF));
    if ((n & 0x1FF) > 0x4F) return (uint16_t)(0xA800 | (n & 0x1FF));   /* old combined table */
    return (uint16_t)(0xA000 | (n & 0xFF));
}
static void T_GetTrapAddress(uint16_t w) { cpu_set(R_A0, trap_get_address(trapword_for(w, D0))); cpu_set(R_D0, 0); }
static void T_SetTrapAddress(uint16_t w) { trap_set_address(trapword_for(w, D0), A0); cpu_set(R_D0, 0); }

/* ---- system info ---- */
static void T_SysEnvirons(uint16_t w) {
    (void)w; maddr r = A0;
    wr16(r + 0, 2);          /* environsVersion */
    wr16(r + 2, 11);         /* machineType: envMacIIci */
    wr16(r + 4, 0x0710);     /* System 7.1 */
    wr16(r + 6, 3);          /* env68020 */
    wr8(r + 8, 0);           /* hasFPU */
    wr8(r + 9, 1);           /* hasColorQD */
    wr16(r + 10, 2);         /* keyboard: Apple Extended */
    wr16(r + 12, 0);
    wr16(r + 14, (uint16_t)-1);
    cpu_set(R_D0, 0);
}

static int gestalt(uint32_t sel, uint32_t *resp) {
    switch (sel) {
        case FOURCC('s','y','s','v'): *resp = 0x0710; return 1;
        case FOURCC('q','d',' ',' '): *resp = 0x0230; return 1;   /* 32-bit Color QD 1.3 */
        case FOURCC('q','d','r','w'): *resp = 0x1F; return 1;
        case FOURCC('p','r','o','c'): *resp = 4; return 1;        /* 68030 */
        case FOURCC('c','p','u',' '): *resp = 3; return 1;
        case FOURCC('f','p','u',' '): *resp = 0; return 1;
        case FOURCC('m','m','u',' '): *resp = 2; return 1;
        case FOURCC('a','d','d','r'): *resp = 7; return 1;        /* 32-bit capable/clean */
        case FOURCC('m','a','c','h'): *resp = 32; return 1;       /* Mac IIci */
        case FOURCC('s','n','d',' '): *resp = 0x1F07; return 1;   /* stereo, 16-bit, ... */
        case FOURCC('t','m','g','r'): *resp = 3; return 1;        /* extended Time Manager */
        case FOURCC('s','y','s','a'): *resp = 1; return 1;        /* 68k */
        case FOURCC('h','d','w','r'): *resp = 0; return 1;
        case FOURCC('r','a','m',' '): *resp = 16 << 20; return 1;
        case FOURCC('l','r','a','m'): *resp = 16 << 20; return 1;
        case FOURCC('v','m',' ',' '): *resp = 0; return 1;
        case FOURCC('f','s',' ',' '): *resp = 0x02; return 1;     /* FSSpec calls */
        case FOURCC('a','l','i','s'): *resp = 1; return 1;
        case FOURCC('k','b','d',' '): *resp = 2; return 1;
        case FOURCC('o','s',' ',' '): *resp = 0x3FB; return 1;
        case FOURCC('t','e',' ',' '): *resp = 4; return 1;
        case FOURCC('d','i','t','l'): *resp = 1; return 1;
        case FOURCC('m','e','n','u'): *resp = 0; return 1;
        case FOURCC('w','i','n','d'): *resp = 0; return 1;
        case FOURCC('q','t','i','m'): return 0;
    }
    return 0;
}
static void T_Gestalt(uint16_t w) {
    uint32_t resp = 0, sel = D0;
    if ((w & 0x0600) != 0) { cpu_set(R_D0, (uint32_t)-5550); return; }  /* New/Replace: unsupported */
    if (gestalt(sel, &resp)) { cpu_set(R_A0, resp); cpu_set(R_D0, 0); }
    else {
        cpu_set(R_D0, (uint32_t)-5551);
        fprintf(stderr, "[gestalt] unknown selector '%c%c%c%c'\n", sel >> 24, sel >> 16 & 0xFF, sel >> 8 & 0xFF, sel & 0xFF);
    }
}

static void T_Debugger(uint16_t w) { (void)w; fprintf(stderr, "[trap] Debugger at %06x\n", trap_ret); }
static void T_DebugStr(uint16_t w) {
    (void)w; char s[256]; pstr_to_c(pop32(), s, sizeof s);
    fprintf(stderr, "[DebugStr] %s\n", s);
}
static void T_SysError(uint16_t w) {
    (void)w;
    fprintf(stderr, "[SysError] %d at %06x\n", (int16_t)D0, trap_ret);
    cpu_stop();
}

void traps_mem_init(void) {
    trap_register(0xA11E, T_NewPtr);
    trap_register(0xA122, T_NewHandle);
    trap_register(0xA166, T_NewEmptyHandle);
    trap_register(0xA01F, T_DisposePtr);
    trap_register(0xA023, T_DisposeHandle);
    trap_register(0xA021, T_GetPtrSize);
    trap_register(0xA020, T_SetPtrSize);
    trap_register(0xA025, T_GetHandleSize);
    trap_register(0xA024, T_SetHandleSize);
    trap_register(0xA029, T_HLock);
    trap_register(0xA02A, T_HUnlock);
    trap_register(0xA049, T_HPurge);
    trap_register(0xA04A, T_HNoPurge);
    trap_register(0xA069, T_HGetState);
    trap_register(0xA06A, T_HSetState);
    trap_register(0xA067, T_HSetRBit);
    trap_register(0xA068, T_HClrRBit);
    trap_register(0xA128, T_RecoverHandle);
    trap_register(0xA02B, T_EmptyHandle);
    trap_register(0xA027, T_ReallocHandle);
    trap_register(0xA02E, T_BlockMove);
    trap_register(0xA01C, T_FreeMem);
    trap_register(0xA11D, T_MaxMem);
    trap_register(0xA04C, T_CompactMem);
    trap_register(0xA04D, T_PurgeMem);
    trap_register(0xA162, T_PurgeSpace);
    trap_register(0xA061, T_MaxBlock);
    trap_register(0xA040, T_ReserveMem);
    trap_register(0xA036, T_ok);             /* MoreMasters */
    trap_register(0xA064, T_ok);             /* MoveHHi */
    trap_register(0xA05C, T_ok);             /* MoveHHi/HLockHi variant (A05C) */
    trap_register(0xA063, T_MaxApplZone);
    trap_register(0xA02D, T_SetApplLimit);
    trap_register(0xA01B, T_SetZone);
    trap_register(0xA11A, T_GetZone);
    trap_register(0xA126, T_HandleZone);
    trap_register(0xA055, T_StripAddress);
    trap_register(0xA05D, T_SwapMMUMode);
    trap_register(0xA065, T_StackSpace);
    trap_register(0xA9E1, T_HandToHand);
    trap_register(0xA9E3, T_PtrToHand);
    trap_register(0xA9E2, T_PtrToXHand);
    trap_register(0xA9E4, T_HandAndHand);
    trap_register(0xA9EF, T_PtrAndHand);
    trap_register(0xA9E0, T_Munger);
    trap_register(0xA146, T_GetTrapAddress);
    trap_register(0xA047, T_SetTrapAddress);
    trap_register(0xA090, T_SysEnvirons);
    trap_register(0xA1AD, T_Gestalt);
    trap_register(0xA9FF, T_Debugger);
    trap_register(0xABFF, T_DebugStr);
    trap_register(0xA9C9, T_SysError);
}
