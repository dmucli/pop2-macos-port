/* CODE 20 "Mohawk Libs 1", first part (0x5b134–0x5d8ae).
 *
 * 1. THINK C Toolbox glue (0x5b134–0x5ba1e): C<->Pascal strings, SysEnvirons/Gestalt with
 *    their fallbacks for old systems, and the register-based File Manager / Memory Manager
 *    traps wrapped as Pascal-callable functions (FSOpen, PBOpen(pb, async), ...).
 * 2. Mohawk working-directory helpers HGETVOL / OPENWD / CLOSEWD.
 * 3. The Mohawk "audio" API (audioInit/Create/Start/...): handle-based audio objects
 *    (Symantec C++ classes audioObj -> midiObj / waveObj, Pascal methods called through
 *    their vtable), and the midiObj methods (a Standard MIDI File player driven by a
 *    Mohawk timer), plus midiMapCachePatches / midiMapClose.
 *
 * Ported from the Ghidra disassembly; addresses in comments are Ghidra addresses (see
 * docs/PORTING.md). Notes: docs/notes/seg20a.md. */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"
#include <stdio.h>

/* ---- globals (A5 world) ---- */
#define gAudioError        GH(0x107e8e)   /* word: last audio error (A5-0x172) */
#define gAudioInitCount    GH(0x107e90)   /* word: audioInit nesting count (A5-0x170) */
#define gAudioClientOff    GH(0x107e92)   /* word: offset of the audio client bytes (A5-0x16e) */
#define gMidiMapClientOff  GH(0x107ec6)   /* word: offset of the midiMap client bytes (A5-0x13a) */
#define gMidiMapResFile    GH(0x107ebc)   /* long: resource file of the midiMap (A5-0x144) */
#define VT_audioObj        GH(0x107e4a)   /* vtable of class audioObj (A5-0x1b6) */
#define VT_midiObj         GH(0x106e56)   /* vtable of class midiObj (A5-0x11aa) */

/* ---- low memory ---- */
#define LM_MemTop_        0x108
#define LM_CPUFlag_       0x12f
#define LM_SysVersion_    0x15a
#define LM_SPConfig_      0x1fb
#define LM_BootDrive_     0x210
#define LM_KbdType_       0x21e
#define LM_ROM85_         0x28e
#define LM_PortBUse_      0x291
#define LM_ROMBase_       0x2ae
#define LM_ABusDCE_       0x2dc
#define LM_Lo3Bytes_      0x31a
#define LM_FSFCBLen_      0x3f6
#define LM_SysMap_        0xa58
#define LM_AppParmHandle_ 0xaec
#define LM_HWCfgFlags_    0xb22
#define LM_MMUType_       0xcb1
#define LM_0cb3_          0xcb3            /* byte used by the Gestalt 'mach' fallback */

/* audio errors */
#define AUDERR_NODEVICE   0x29cc
#define AUDERR_NOTENABLED 0x29cd
#define AUDERR_BADDEVICE  0x29ce
#define AUDERR_SEEKRANGE  0x29cf
#define AUDERR_BADFORMAT  0x29d0
#define AUDERR_BADMIDI    0x29d1
#define AUDERR_NOTINIT    0x29fe
#define AUDERR_BADHANDLE  0x29ff
#define AUDERR_INACTIVE   0x2a00
#define AUDERR_NOTPAUSED  0x2a01
#define AUDERR_NOTPLAYING 0x2a02
#define AUDERR_NOTPREP    0x2a03
#define AUDERR_PAUSED     0x2a04
#define AUDERR_PLAYING    0x2a05
#define AUDERR_PREPARED   0x2a06
#define AUDERR_NOLIBS     0x2a07

/* A Boolean/char argument pushed with move.b lives in the high byte of its word. */
#define B_HI(w) ((uint8_t)((uint16_t)(w) >> 8))

/* The trap glue pops the return address and the arguments before trapping: the trap runs
 * with SP on the caller's result slot (args_ + purge). */
#define ENTRY_POPPED(fn, purge) ENTRY_NOLINK(fn, 0); emu_set_sp(args_ + (uint32_t)(purge))

/* Register-based OS trap: A0/D0 in, D0 out (and A0 if asked). */
static uint32_t os_trap(uint16_t w, maddr a0, uint32_t d0, maddr *a0_out) {
    tb_begin();
    cpu_set(R_A0, a0);
    cpu_set(R_D0, d0);
    tb_trap(w);
    uint32_t r = cpu_get(R_D0);
    if (a0_out) *a0_out = cpu_get(R_A0);
    tb_end();
    return r;
}
/* BlockMove(A0 src, A1 dst, D0 count) */
static uint32_t block_move(maddr src, maddr dst, uint32_t n) {
    return _BlockMove(src, dst, n);
}

/* D0 at exit of the Pascal functions whose D0 is not their result (manual glue). */
static uint32_t s_d0;
/* Glue of a Pascal function with a result slot whose D0 differs from the result: the C
 * function returns the slot value and leaves the exact D0 in s_d0. */
#define RESULT_SLOT_GLUE(Name, argb, slotb, call) void glue_##Name(void); void glue_##Name(void) { \
        maddr sp = emu_sp(); uint32_t r = (call); \
        if ((slotb) == 2) W16(sp + (argb), r); else if ((slotb) == 4) W32(sp + (argb), r); \
        emu_set_sp(sp + (argb)); emu_set_d0(s_d0); }

/* ---- Pascal methods through the vtable (Symantec C++: `this` pushed last) ---- */
static maddr vfn(uint32_t obj, int off) { return R32(R32(obj) + (uint32_t)off); }
/* The caller reserved `slot` result bytes at s0 - slot (not written, like subq) and pushed
 * the arguments; the method pops them. Returns the slot, SP back to s0. */
static uint32_t pmeth(maddr s0, int slot, maddr fn) {
    call68_addr(fn);
    uint32_t r = slot == 2 ? RU16(s0 - 2) : slot == 4 ? R32(s0 - 4) : 0;
    emu_set_sp(s0);
    return r;
}
/* Method without result: this + `extra` bytes of arguments already pushed. */
static void pmeth_void(maddr s0, maddr fn) {
    call68_addr(fn);
    emu_set_sp(s0);
}
/* The client bytes of the audio library: osGetClientBytes(client) + offset. */
static maddr audio_bytes(uint32_t client) {
    uint32_t p = osGetClientBytes(client);
    return p + RU16(gAudioClientOff);
}
static int16_t audio_err(int16_t e) { W16(gAudioError, e); return e; }

/* Callee-saved registers the original holds while it calls 68k code (deeper callees save
 * them on the stack, where later code may pick them up): KEEP_REGS() saves D3-D7/A2-A4
 * at entry and restores them on every return; set_reg() sets them as the original does. */
typedef struct { uint32_t r[8]; } regsave_t;
static regsave_t regs_save(void) {
    regsave_t s;
    for (int i = 0; i < 5; i++) s.r[i] = cpu_get(R_D3 + i);
    for (int i = 0; i < 3; i++) s.r[5 + i] = cpu_get(R_A2 + i);
    return s;
}
static void regs_restore(regsave_t *s) {
    for (int i = 0; i < 5; i++) cpu_set(R_D3 + i, s->r[i]);
    for (int i = 0; i < 3; i++) cpu_set(R_A2 + i, s->r[5 + i]);
}
#define KEEP_REGS() regsave_t saved_regs_ __attribute__((cleanup(regs_restore))) = regs_save()
static void set_reg_w(int r, uint16_t v) { cpu_set(r, (cpu_get(r) & 0xffff0000u) | v); }

/* ===================================================================================== */
/* THINK C Toolbox glue                                                                  */
/* ===================================================================================== */

/* 0005b134: C string -> Pascal string in place; returns s */
uint32_t tc_CtoPstr(uint32_t s) {
    maddr a0 = s;
    uint8_t d0 = 0, d1;
    do {
        d1 = R8(a0);
        W8(a0, d0);
        a0++;
        d0 = d1;
    } while (d0 != 0);
    W8(s, (uint8_t)(a0 - (s + 1)));
    return s;
}

/* 0005b150: Pascal string -> C string in place; returns s */
uint32_t tc_PtoCstr(uint32_t s) {
    maddr a0 = s;
    uint16_t n = R8(a0);
    while (n-- != 0) { W8(a0, R8(a0 + 1)); a0++; }
    W8(a0, 0);
    return s;
}

/* 0005b16a: SysEnvirons(versReq, VAR theWorld): OSErr. Uses the trap when it is
 * implemented, else fills the record by hand and returns envNotPresent (-5500). */
uint32_t tc_SysEnvirons(uint32_t world, int16_t vers) {
    ENTRY_NOLINK(FN_tc_SysEnvirons, 0);
    uint32_t d0 = cpu_get(R_D0);                 /* entry D0 (high word survives) */
    if (R16(LM_ROM85_) >= 0) {
        maddr a1, a0;
        d0 = os_trap(0xA346, 0, (d0 & 0xffff0000u) | 0x90, &a1);   /* GetTrapAddress(SysEnvirons) */
        d0 = os_trap(0xA746, a1, (d0 & 0xffff0000u) | 0x9f, &a0);  /* GetTrapAddress(Unimplemented) */
        if (a0 != a1) {
            emu_set_sp(args_ + 2);                /* args popped, return address pushed back */
            d0 = os_trap(0xA090, world, (d0 & 0xffff0000u) | (uint16_t)vers, NULL);
            s_d0 = d0;
            return (uint16_t)d0;
        }
    }
    /* by hand */
    for (int i = 0; i < 8; i++) W16(world + 2 * i, 0);
    d0 = (d0 & 0xffff0000u) | 0xffff;            /* dbra counter */
    W16(world, 2);
    maddr rom = R32(LM_ROMBase_);
    W16(world + 2, 0xfffe);
    if (R8(rom + 9) != 0xff) {
        if (RS8(rom + 8) > 0) {
            W16(world + 2, 0);
            if (RS8(rom + 8) <= 2) W16(world + 2, RS8(rom + 8) == 2 ? 3 : 4);
        } else {
            W16(world + 2, 0xffff);
            if (R16(LM_ROM85_) >= 0) {
                W16(world + 2, 1);
                if (R16(LM_HWCfgFlags_) < 0) W16(world + 2, 2);
            }
        }
    }
    W16(world + 4, 0);
    if (RS8(LM_CPUFlag_) <= 2) {
        d0 = (d0 & 0xffffff00u) | R8(LM_CPUFlag_);
        d0 = (d0 & 0xffff0000u) | (uint16_t)(d0 + 1);
        W8(world + 7, d0);
    }
    if (R16(LM_ROM85_) >= 0 && (R8(LM_HWCfgFlags_) & 0x10)) W8(world + 8, 1);
    if (RU16(LM_ROM85_) <= 0x3fff) W8(world + 9, 1);
    uint8_t kbd = R8(LM_KbdType_);
    d0 = (d0 & 0xffffff00u) | kbd;
    maddr t = fn_address(FN_tc_SysEnvirons) + (0x5b1a3 - 0x5b16a);   /* 5 keyboard types */
    int16_t d1 = 4;
    for (;;) {
        t--;
        if (R8(t) == kbd) break;
        if (--d1 == -1) break;
    }
    W16(world + 0xa, d1 + 1);
    if (RS8(LM_PortBUse_) >= 0 && (R8(LM_SPConfig_) & 0xf) == 1)
        W8(world + 0xd, R8(R32(LM_ABusDCE_) + 7));
    W16(world + 0xe, R16(LM_BootDrive_));
    if (R16(LM_FSFCBLen_) >= 0) {
        W16(world + 0xe, 0);
        maddr pb = args_ - 4 - 0x7a;              /* 0x7a-byte cleared block on the stack */
        mset(pb, 0, 0x7a);
        emu_set_sp(pb);
        W16(pb + 0x18, R16(LM_SysMap_));
        d0 = os_trap(0xA260, pb, 8, NULL);        /* GetFCBInfo */
        if ((int16_t)d0 == 0) {
            W16(pb + 0x16, R16(pb + 0x34));
            d0 = os_trap(0xA207, pb, d0, NULL);   /* PBHGetVInfo */
            if ((int16_t)d0 == 0) {
                W32(pb + 0x30, R32(pb + 0x5a));
                W32(pb + 0x1c, 0x4552494b);       /* 'ERIK' */
                d0 = os_trap(0xA260, pb, 1, NULL);  /* OpenWD */
                if ((int16_t)d0 == 0) W16(world + 0xe, R16(pb + 0x16));
            }
        }
    }
    s_d0 = d0;
    return 0xea84;                                /* envNotPresent */
}
RESULT_SLOT_GLUE(tc_SysEnvirons, 6, 2, tc_SysEnvirons(R32(sp), R16(sp + 4)))

/* 0005b2d4: Gestalt(selector, VAR response): OSErr, with a built-in table (0x5b334) when
 * the trap is not implemented. */
uint32_t tc_Gestalt(uint32_t response, uint32_t selector) {
    ENTRY_LINK(FN_tc_Gestalt, 0, 0);
    maddr unimp, g, a0;
    uint32_t d0 = os_trap(0xA746, 0, 0xa89f, &unimp);
    push32(unimp);
    d0 = os_trap(0xA346, unimp, 0xa0ad, &g);
    emu_set_sp(emu_sp() + 4);
    if (g != unimp) {
        d0 = os_trap(0xA1AD, g, selector, &a0);   /* Gestalt */
        W32(response, a0);
        return d0;
    }
    maddr tbl = fn_address(FN_tc_Gestalt) + (0x5b334 - 0x5b2d4);
    maddr t = tbl;
    d0 = (d0 & 0xffff0000u) | 0xea51;             /* gestaltUndefSelectorErr */
    uint32_t off = 0;
    for (;;) {
        uint32_t sel = R32(t); t += 4;
        if (selector == sel) { off = R32(t); break; }
        uint32_t o = R32(t); t += 4;
        if (o == 0) return d0;
    }
    switch (off) {
    case 0x60:                                    /* 'vers' */
        d0 = 1;
        break;
    case 0x64: {                                  /* 'mach' */
        maddr rom = R32(LM_ROMBase_);
        d0 = 4;
        if (RU16(rom + 8) == 0x75) break;
        if (RU16(rom + 8) == 0x276) { d0 = 5; break; }
        d0 = (d0 & 0xffffff00u) | R8(LM_0cb3_);
        d0 += 6;
        break;
    }
    case 0x88:                                    /* 'sysv' */
        d0 = RU16(LM_SysVersion_);
        break;
    case 0x92:                                    /* 'proc' */
        d0 = R8(LM_CPUFlag_);
        d0 = (d0 & 0xffff0000u) | (uint16_t)(d0 + 1);
        break;
    case 0x9e:                                    /* 'fpu ' */
        if (R8(LM_CPUFlag_) == 4) { d0 = 3; break; }
        if (!(R8(LM_HWCfgFlags_) & 0x10)) { d0 = 0; break; }
        /* FNOP + FSAVE probe of the coprocessor's idle frame (1f18/3f18 -> 68881 = 1,
         * 3f38/1f38 -> 68882 = 2): coprocessor instructions cannot run here (the emulated
         * machine reports no FPU in HWCfgFlags, so this is not reached). */
        fprintf(stderr, "tc_Gestalt: FPU probe not supported, reporting no FPU\n");
        d0 = 0;
        break;
    case 0xe8:                                    /* 'qd  ' */
        if (R16(LM_ROM85_) > 0x3fff) { d0 = 0; break; }
        {
            maddr u, q;
            os_trap(0xA746, 0, (d0 & 0xffff0000u) | 0xa89f, &u);
            os_trap(0xA746, u, 0xab03, &q);
            d0 = q != u ? 0x200 : 0x100;
        }
        break;
    case 0x11a: {                                 /* 'kbd ' */
        uint8_t k = R8(LM_KbdType_);
        d0 = (d0 & 0xffffff00u) | k;
        maddr kt = fn_address(FN_tc_Gestalt) + (0x5b46a - 0x5b2d4), p = kt;
        for (;;) {
            uint8_t b = R8(p); p++;
            if (b == 0) return (d0 & 0xffff0000u) | 0xea52;   /* gestaltUnknownErr */
            if (b == k) break;
        }
        d0 = p - kt;
        break;
    }
    case 0x142:                                   /* 'atlk' */
        d0 = 0;
        if (RS8(LM_PortBUse_) >= 0 && (R8(LM_SPConfig_) & 0xf) == 1) d0 = R8(R32(LM_ABusDCE_) + 7);
        break;
    case 0x164:                                   /* 'mmu ' */
        if (RS8(LM_CPUFlag_) < 2) { d0 = 0; break; }
        d0 = R8(LM_MMUType_);
        if (d0 == 1) break;
        if ((int8_t)d0 < 3) { d0 = 0; break; }
        d0 = (uint16_t)(d0 - 1);
        break;
    case 0x188: {                                 /* 'ram ', 'lram' */
        maddr u, od;
        os_trap(0xA746, 0, (d0 & 0xffff0000u) | 0xa89f, &u);
        os_trap(0xA746, u, 0xa88f, &od);
        d0 = R32(LM_MemTop_);
        if (od != u) {
            tb_begin();
            emu_set_sp(emu_sp() - 4);
            push16(0x16);
            tb_trap(0xA88F);                      /* OSDispatch(0x16) */
            d0 = pop32();
            tb_end();
        }
        break;
    }
    default:
        /* a selector equal to 0 matches the terminating entry and jumps into the table */
        fprintf(stderr, "tc_Gestalt: selector %08x jumps to table offset %x\n", selector, off);
        return d0;
    }
    W32(response, d0);
    return d0 & 0xffff0000u;                      /* clr.w D0: noErr */
}

/* 0005b4e2: GetPtrSize(p): 0 on error (D0 keeps the error) */
uint32_t tc_GetPtrSize(uint32_t p) {
    ENTRY_POPPED(FN_tc_GetPtrSize, 4);
    uint32_t d0 = os_trap(0xA021, p, cpu_get(R_D0), NULL);
    s_d0 = d0;
    return (int32_t)d0 < 0 ? 0 : d0;
}
RESULT_SLOT_GLUE(tc_GetPtrSize, 4, 4, tc_GetPtrSize(R32(sp)))

/* 0005b4f0: GetHandleSize(h): 0 on error (D0 keeps the error) */
uint32_t tc_GetHandleSize(uint32_t h) {
    ENTRY_POPPED(FN_tc_GetHandleSize, 4);
    uint32_t d0 = os_trap(0xA025, h, cpu_get(R_D0), NULL);
    s_d0 = d0;
    return (int32_t)d0 < 0 ? 0 : d0;
}
RESULT_SLOT_GLUE(tc_GetHandleSize, 4, 4, tc_GetHandleSize(R32(sp)))

/* 0005b4fe: NGetTrapAddress(trapNum, tTyp): address */
uint32_t tc_NGetTrapAddress(int16_t ttyp, int16_t trapnum) {
    ENTRY_POPPED(FN_tc_NGetTrapAddress, 4);
    maddr a0;
    uint32_t d0 = (cpu_get(R_D0) & 0xffff0000u) | (uint16_t)trapnum;
    s_d0 = os_trap(B_HI(ttyp) ? 0xA746 : 0xA346, cpu_get(R_A0), d0, &a0);
    return a0;
}
RESULT_SLOT_GLUE(tc_NGetTrapAddress, 4, 4, tc_NGetTrapAddress(R16(sp), R16(sp + 2)))

/* Pascal glue of the parameter-block calls: PBxxx(pb, async): OSErr. */
static uint32_t pb_call(int16_t async, uint32_t pb, uint16_t sync_trap, uint16_t async_trap) {
    uint32_t d0 = (cpu_get(R_D0) & 0xffffff00u) | B_HI(async);
    return os_trap(B_HI(async) ? async_trap : sync_trap, pb, d0, NULL);
}
/* HFSDispatch calls: D0 = selector */
static uint32_t pb_hfs(int16_t async, uint32_t pb, uint32_t sel) {
    return os_trap(B_HI(async) ? 0xA660 : 0xA260, pb, sel, NULL);
}

/* 0005b512 */
uint32_t tc_PBOpen(int16_t async, uint32_t pb) {
    ENTRY_POPPED(FN_tc_PBOpen, 6);
    return pb_call(async, pb, 0xA000, 0xA400);
}
/* 0005b524 */
uint32_t tc_PBClose(int16_t async, uint32_t pb) {
    ENTRY_POPPED(FN_tc_PBClose, 6);
    return pb_call(async, pb, 0xA001, 0xA401);
}
/* 0005b536 */
uint32_t tc_PBControl(int16_t async, uint32_t pb) {
    ENTRY_POPPED(FN_tc_PBControl, 6);
    return pb_call(async, pb, 0xA004, 0xA404);
}
/* 0005b548 */
uint32_t tc_PBGetVInfo(int16_t async, uint32_t pb) {
    ENTRY_POPPED(FN_tc_PBGetVInfo, 6);
    return pb_call(async, pb, 0xA007, 0xA407);
}
/* 0005b55a */
uint32_t tc_PBGetEOF(int16_t async, uint32_t pb) {
    ENTRY_POPPED(FN_tc_PBGetEOF, 6);
    return pb_call(async, pb, 0xA011, 0xA411);
}
/* 0005b56c */
uint32_t tc_PBSetEOF(int16_t async, uint32_t pb) {
    ENTRY_POPPED(FN_tc_PBSetEOF, 6);
    return pb_call(async, pb, 0xA012, 0xA412);
}
/* 0005b57e */
uint32_t tc_PBSetFPos(int16_t async, uint32_t pb) {
    ENTRY_POPPED(FN_tc_PBSetFPos, 6);
    return pb_call(async, pb, 0xA044, 0xA444);
}

/* 0005b590: FSOpen(fileName, vRefNum, VAR refNum): OpenDF, PBOpen if that gives paramErr */
uint32_t tc_FSOpen(uint32_t refnum, int16_t vref, uint32_t name) {
    ENTRY_LINK(FN_tc_FSOpen, 0x32, 0);
    maddr pb = a6 - 0x32;
    W32(pb + 0x12, name);
    W16(pb + 0x16, vref);
    W8(pb + 0x1a, 0);
    W8(pb + 0x1b, 0);
    W32(pb + 0x1c, 0);
    uint32_t d0 = os_trap(0xA060, pb, 0x1a, NULL);
    if ((int16_t)d0 == -50) d0 = os_trap(0xA000, pb, d0, NULL);
    W16(refnum, R16(pb + 0x18));
    return d0;
}

/* 0005b5d0: FSClose(refNum) */
uint32_t tc_FSClose(int16_t refnum) {
    ENTRY_LINK(FN_tc_FSClose, 0x32, 0);
    maddr pb = a6 - 0x32;
    W16(pb + 0x18, refnum);
    return os_trap(0xA001, pb, cpu_get(R_D0), NULL);
}

/* FSRead/FSWrite body (0x5b5f0): read/write at the mark */
static uint32_t fs_rw(maddr a6, uint32_t buf, uint32_t count, int16_t refnum, uint16_t trap) {
    maddr pb = a6 - 0x32;
    W32(pb + 0x20, buf);
    W16(pb + 0x18, refnum);
    W32(pb + 0x24, R32(count));
    W16(pb + 0x2c, 0);
    W32(pb + 0x2e, 0);
    uint32_t d0 = os_trap(trap, pb, cpu_get(R_D0), NULL);
    W32(count, R32(pb + 0x28));
    return d0;
}
/* 0005b5ea: FSRead(refNum, VAR count, buffPtr) */
uint32_t tc_FSRead(uint32_t buf, uint32_t count, int16_t refnum) {
    ENTRY_LINK(FN_tc_FSRead, 0x32, 0);
    return fs_rw(a6, buf, count, refnum, 0xA002);
}
/* 0005b5ee: FSWrite(refNum, VAR count, buffPtr) */
uint32_t tc_FSWrite(uint32_t buf, uint32_t count, int16_t refnum) {
    ENTRY_LINK(FN_tc_FSWrite, 0x32, 0);
    return fs_rw(a6, buf, count, refnum, 0xA003);
}

/* 0005b632: GetVInfo(drvNum, volName, VAR vRefNum, VAR freeBytes) */
uint32_t tc_GetVInfo(uint32_t freebytes, uint32_t vrefp, uint32_t volname, int16_t drv) {
    ENTRY_LINK(FN_tc_GetVInfo, 0x7a, 0);
    maddr pb = a6 - 0x7a;
    W32(pb + 0x12, volname);
    W16(pb + 0x16, drv);
    W16(pb + 0x1c, 0);
    uint32_t err = os_trap(0xA207, pb, cpu_get(R_D0), NULL);
    W16(vrefp, R16(pb + 0x16));
    uint32_t d0 = (uint32_t)((int32_t)R32(pb + 0x30) >> 9);      /* ioVAlBlkSiz / 512 */
    d0 = (uint32_t)(uint16_t)d0 * RU16(pb + 0x3e);                /* mulu.w ioVFrBlk */
    d0 <<= 9;
    W32(freebytes, d0);
    s_d0 = d0;
    return err;
}
RESULT_SLOT_GLUE(tc_GetVInfo, 14, 2, tc_GetVInfo(R32(sp), R32(sp + 4), R32(sp + 8), R16(sp + 12)))

/* 0005b674: GetFInfo(fileName, vRefNum, VAR fndrInfo) */
uint32_t tc_GetFInfo(uint32_t finfo, int16_t vref, uint32_t name) {
    ENTRY_LINK(FN_tc_GetFInfo, 0x50, 0);
    maddr pb = a6 - 0x50;
    W32(pb + 0x12, name);
    W16(pb + 0x16, vref);
    W8(pb + 0x1a, 0);
    W16(pb + 0x1c, 0);
    uint32_t err = os_trap(0xA00C, pb, cpu_get(R_D0), NULL);
    s_d0 = block_move(pb + 0x20, finfo, 0x10);
    return err;
}
RESULT_SLOT_GLUE(tc_GetFInfo, 10, 2, tc_GetFInfo(R32(sp), R16(sp + 4), R32(sp + 6)))

/* 0005b6aa: GetVol(volName, VAR vRefNum) */
uint32_t tc_GetVol(uint32_t vrefp, uint32_t volname) {
    ENTRY_LINK(FN_tc_GetVol, 0x40, 0);
    maddr pb = a6 - 0x40;
    W32(pb + 0x12, volname);
    uint32_t d0 = os_trap(0xA014, pb, cpu_get(R_D0), NULL);
    W16(vrefp, R16(pb + 0x16));
    return d0;
}

/* 0005b6cc: SetVol(volName, vRefNum) */
uint32_t tc_SetVol(int16_t vref, uint32_t volname) {
    ENTRY_LINK(FN_tc_SetVol, 0x40, 0);
    maddr pb = a6 - 0x40;
    W32(pb + 0x12, volname);
    W16(pb + 0x16, vref);
    return os_trap(0xA015, pb, cpu_get(R_D0), NULL);
}

/* 0005b6ec: Create(fileName, vRefNum, creator, fileType) */
uint32_t tc_Create(uint32_t ftype, uint32_t creator, int16_t vref, uint32_t name) {
    ENTRY_LINK(FN_tc_Create, 0x50, 0);
    maddr pb = a6 - 0x50;
    W32(pb + 0x12, name);
    W16(pb + 0x16, vref);
    W8(pb + 0x1a, 0);
    uint32_t d0 = os_trap(0xA008, pb, cpu_get(R_D0), NULL);   /* PBCreate */
    if ((int16_t)d0 == 0) {
        W16(pb + 0x1c, 0);
        d0 = os_trap(0xA00C, pb, d0, NULL);                    /* PBGetFInfo */
        if ((int16_t)d0 == 0) {
            W32(pb + 0x20, ftype);
            W32(pb + 0x24, creator);
            d0 = os_trap(0xA00D, pb, d0, NULL);                /* PBSetFInfo */
        }
    }
    return d0;
}

/* 0005b72a: SetFInfo(fileName, vRefNum, fndrInfo) */
uint32_t tc_SetFInfo(uint32_t finfo, int16_t vref, uint32_t name) {
    ENTRY_LINK(FN_tc_SetFInfo, 0x50, 0);
    maddr pb = a6 - 0x50;
    W32(pb + 0x12, name);
    W16(pb + 0x16, vref);
    W8(pb + 0x1a, 0);
    W16(pb + 0x1c, 0);
    os_trap(0xA00C, pb, cpu_get(R_D0), NULL);                 /* PBGetFInfo (result ignored) */
    uint32_t d0 = block_move(finfo, pb + 0x20, 0x10);
    return os_trap(0xA00D, pb, d0, NULL);                      /* PBSetFInfo */
}

/* 0005b764: GetEOF(refNum, VAR logEOF) */
uint32_t tc_GetEOF(uint32_t eofp, int16_t refnum) {
    ENTRY_LINK(FN_tc_GetEOF, 0x32, 0);
    maddr pb = a6 - 0x32;
    W16(pb + 0x18, refnum);
    uint32_t d0 = os_trap(0xA011, pb, cpu_get(R_D0), NULL);
    W32(eofp, R32(pb + 0x1c));
    return d0;
}

/* 0005b786: SetFPos(refNum, posMode, posOff) */
uint32_t tc_SetFPos(uint32_t off, int16_t mode, int16_t refnum) {
    ENTRY_LINK(FN_tc_SetFPos, 0x32, 0);
    maddr pb = a6 - 0x32;
    W16(pb + 0x18, refnum);
    W16(pb + 0x2c, mode);
    W32(pb + 0x2e, off);
    return os_trap(0xA044, pb, cpu_get(R_D0), NULL);
}

/* 0005b7ac: PBOpenWD (HFSDispatch 1) */
uint32_t tc_PBOpenWD(int16_t async, uint32_t pb) {
    ENTRY_POPPED(FN_tc_PBOpenWD, 6);
    return pb_hfs(async, pb, 1);
}
/* 0005b7c2: PBCloseWD (HFSDispatch 2) */
uint32_t tc_PBCloseWD(int16_t async, uint32_t pb) {
    ENTRY_POPPED(FN_tc_PBCloseWD, 6);
    return pb_hfs(async, pb, 2);
}
/* 0005b7d8: PBHGetVol */
uint32_t tc_PBHGetVol(int16_t async, uint32_t pb) {
    ENTRY_POPPED(FN_tc_PBHGetVol, 6);
    return pb_call(async, pb, 0xA214, 0xA614);
}
/* 0005b7ea: PBGetFCBInfo (HFSDispatch 8) */
uint32_t tc_PBGetFCBInfo(int16_t async, uint32_t pb) {
    ENTRY_POPPED(FN_tc_PBGetFCBInfo, 6);
    return pb_hfs(async, pb, 8);
}
/* 0005b800: PBGetCatInfo (HFSDispatch 9) */
uint32_t tc_PBGetCatInfo(int16_t async, uint32_t pb) {
    ENTRY_POPPED(FN_tc_PBGetCatInfo, 6);
    return pb_hfs(async, pb, 9);
}
/* 0005b816: PBSetCatInfo (HFSDispatch 10) */
uint32_t tc_PBSetCatInfo(int16_t async, uint32_t pb) {
    ENTRY_POPPED(FN_tc_PBSetCatInfo, 6);
    return pb_hfs(async, pb, 10);
}
/* 0005b82c: PBHGetVInfo */
uint32_t tc_PBHGetVInfo(int16_t async, uint32_t pb) {
    ENTRY_POPPED(FN_tc_PBHGetVInfo, 6);
    return pb_call(async, pb, 0xA207, 0xA607);
}
/* 0005b83e: PBHOpen */
uint32_t tc_PBHOpen(int16_t async, uint32_t pb) {
    ENTRY_POPPED(FN_tc_PBHOpen, 6);
    return pb_call(async, pb, 0xA200, 0xA600);
}
/* 0005b850: PBHDelete */
uint32_t tc_PBHDelete(int16_t async, uint32_t pb) {
    ENTRY_POPPED(FN_tc_PBHDelete, 6);
    return pb_call(async, pb, 0xA209, 0xA609);
}
/* 0005b862: PBHRename */
uint32_t tc_PBHRename(int16_t async, uint32_t pb) {
    ENTRY_POPPED(FN_tc_PBHRename, 6);
    return pb_call(async, pb, 0xA20B, 0xA60B);
}
/* 0005b874: PBHRstFLock */
uint32_t tc_PBHRstFLock(int16_t async, uint32_t pb) {
    ENTRY_POPPED(FN_tc_PBHRstFLock, 6);
    return pb_call(async, pb, 0xA242, 0xA642);
}
/* 0005b886: PBHSetFLock */
uint32_t tc_PBHSetFLock(int16_t async, uint32_t pb) {
    ENTRY_POPPED(FN_tc_PBHSetFLock, 6);
    return pb_call(async, pb, 0xA241, 0xA641);
}

/* 0005b898: SetSoundVol(level): immediate Control call to the sound driver (-4, csCode 2) */
uint32_t tc_SetSoundVol(int16_t level) {
    ENTRY_NOLINK(FN_tc_SetSoundVol, 0);
    /* the level is popped and the return address pushed back: link A6 sits 2 bytes higher */
    maddr a6 = args_ - 6, pb = a6 - 0x20;
    emu_set_sp(pb);
    W16(pb + 0x18, 0xfffc);
    W16(pb + 0x1a, 2);
    W16(pb + 0x1c, level);
    os_trap(0xA204, pb, (cpu_get(R_D0) & 0xffff0000u) | (uint16_t)level, NULL);
    return 0;
}

/* 0005b8ba: CountAppFiles(VAR message, VAR count) */
uint32_t tc_CountAppFiles(uint32_t countp, uint32_t messagep) {
    ENTRY_NOLINK(FN_tc_CountAppFiles, 0);
    W16(countp, 0);
    maddr h = R32(LM_AppParmHandle_);
    int32_t n = (int32_t)os_trap(0xA025, h, cpu_get(R_D0), NULL);
    if (n > 0) {
        maddr p = R32(h);
        W16(messagep, R16(p));
        W16(countp, R16(p + 2));
    }
    return 0;
}

/* 0005b8de: GetAppFiles(index, VAR theFile): copies entries 1..index, the last one stays */
uint32_t tc_GetAppFiles(uint32_t thefile, int16_t index) {
    ENTRY_NOLINK(FN_tc_GetAppFiles, 8);              /* movem A2-A3 */
    W32(thefile + 2, 0);
    maddr h = R32(LM_AppParmHandle_);
    int32_t n = (int32_t)os_trap(0xA025, h, cpu_get(R_D0), NULL);
    if (n > 0) {
        maddr a2 = R32(h) + 2;
        int16_t d1 = index;
        int16_t cnt = R16(a2); a2 += 2;
        if (d1 > cnt) return 0;
        for (d1--; d1 >= 0; d1--) {
            maddr a0 = a2;
            uint16_t sz = (uint8_t)(2 + R8(a0 + 8));
            sz = (uint16_t)((sz & 0xfffe) + 8);
            a2 += (uint32_t)(int32_t)(int16_t)sz;
            block_move(a0, thefile, sz);
        }
    }
    return 0;
}

/* 0005b924: PurgeSpace(VAR total, VAR contig) */
uint32_t tc_PurgeSpace(uint32_t contigp, uint32_t totalp) {
    ENTRY_NOLINK(FN_tc_PurgeSpace, 0);
    maddr a0;
    uint32_t d0 = os_trap(0xA162, cpu_get(R_A0), cpu_get(R_D0), &a0);
    W32(contigp, a0);
    W32(totalp, d0);
    return 0;
}

/* 0005b938: FlushInstructionCache (HWPriv 1) */
uint32_t tc_FlushInstructionCache(void) {
    ENTRY_POPPED(FN_tc_FlushInstructionCache, 0);
    os_trap(0xA198, cpu_get(R_A0), 1, NULL);
    return 0;
}
/* 0005b940: FlushDataCache (HWPriv 3) */
uint32_t tc_FlushDataCache(void) {
    ENTRY_POPPED(FN_tc_FlushDataCache, 0);
    os_trap(0xA198, cpu_get(R_A0), 3, NULL);
    return 0;
}

/* 0005b94a: Pack2 (Disk Initialization) dispatcher: D2 = selector. Pushes the selector under
 * the arguments and traps (auto-pop: the package returns straight to the caller). Register
 * convention: only entered through its glue, with the return address already popped. */
uint32_t tc__Pack2_dispatch(void) {
    push16((uint16_t)cpu_get(R_D2));
    tb_trap(0xA9E9);                              /* the package pops the selector and args */
    return cpu_get(R_D0);
}
void glue_tc__Pack2_dispatch(void);
void glue_tc__Pack2_dispatch(void) { tc__Pack2_dispatch(); }

/* 0005b952: DIBadMount(where, evtMessage): INTEGER (Pack2 selector 0) */
uint32_t tc_DIBadMount(uint32_t evtmsg, uint32_t where) {
    ENTRY_NOLINK(FN_tc_DIBadMount, 0);
    W32(args_, evtmsg);                           /* the arguments as the caller pushed them */
    W32(args_ + 4, where);
    cpu_set(R_D2, 0);
    emu_set_sp(args_);
    tc__Pack2_dispatch();
    s_d0 = cpu_get(R_D0);
    return RU16(args_ + 8);
}
RESULT_SLOT_GLUE(tc_DIBadMount, 8, 2, tc_DIBadMount(R32(sp), R32(sp + 4)))

/* 0005b958: NumToString(theNum, VAR theString) (Pack7 selector 0) */
uint32_t tc_NumToString(uint32_t str, uint32_t num) {
    ENTRY_NOLINK(FN_tc_NumToString, 0);
    tb_begin();
    cpu_set(R_A0, str);
    cpu_set(R_D0, num);
    push16(0);
    tb_trap(0xA9EE);
    tb_end();
    return 0;
}

/* 0005b96a: StripAddress(addr): masks with Lo3Bytes on 24-bit-only ROMs */
uint32_t tc_StripAddress(uint32_t addr) {
    ENTRY_POPPED(FN_tc_StripAddress, 4);
    if (R8(LM_ROM85_) & 0x40) return addr & R32(LM_Lo3Bytes_);
    return os_trap(0xA055, cpu_get(R_A0), addr, NULL);
}

/* 0005b984: HandToHand(VAR theHndl): OSErr */
uint32_t tc_HandToHand(uint32_t hp) {
    ENTRY_NOLINK(FN_tc_HandToHand, 0);
    tb_begin();
    cpu_set(R_A0, R32(hp));
    tb_trap(0xA9E1);
    uint32_t d0 = cpu_get(R_D0);
    maddr a0 = cpu_get(R_A0);
    tb_end();
    W32(hp, a0);
    return d0;
}

/* 0005b99a: GetIndString(VAR theString, strListID, index) */
uint32_t tc_GetIndString(int16_t index, int16_t id, uint32_t str) {
    ENTRY_LINK(FN_tc_GetIndString, 0, 0);
    uint32_t h = _GetResource(0x53545223, id);      /* 'STR#' */
    W8(str, 0);
    if (h == 0) return 0;
    maddr p = R32(h);
    uint16_t cnt = RU16(p); p += 2;
    uint16_t d1 = (uint16_t)index;
    if (d1 == 0 || d1 > cnt) return 0;
    while (--d1 != 0) { uint8_t n = R8(p); p++; p += n; }
    block_move(p, str, (uint16_t)(R8(p) + 1));
    return 0;
}

/* 0005b9e0: GetIndPattern(VAR thePattern, patListID, index) ('PAT#'); Pascal, pops 8 */
uint32_t seg20_05b9e0(int16_t index, int16_t id, uint32_t pat) {
    ENTRY_LINK(FN_seg20_05b9e0, 0, 0);
    uint32_t h = _GetResource(0x50415423, id);      /* 'PAT#' */
    if (h == 0) return 0;
    maddr p = R32(h);
    uint16_t cnt = RU16(p); p += 2;
    uint16_t d1 = (uint16_t)index;
    if (d1 == 0 || d1 > cnt) return 0;
    while (--d1 != 0) p += 8;
    W32(pat, R32(p));
    W32(pat + 4, R32(p + 4));
    return 0;
}

/* ===================================================================================== */
/* Mohawk working directories                                                            */
/* ===================================================================================== */

/* 0005ba1e: HGETVOL(volName, VAR vRefNum, VAR dirID): OSErr */
uint32_t HGETVOL(uint32_t dirid, uint32_t vrefp, uint32_t volname) {
    ENTRY_LINK(FN_HGETVOL, 0x34, 4);                /* link -0x34; move.l D7 */
    maddr pb = a6 - 0x34;
    W32(pb + 0x12, volname);
    uint32_t err = tc_PBHGetVol(0, pb);
    W16(vrefp, R16(pb + 0x16));
    W32(dirid, R32(pb + 0x30));
    return err;
}

/* 0005ba66: OPENWD(vRefNum, dirID, procID, VAR wdRefNum): OSErr */
uint32_t OPENWD(uint32_t wdp, uint32_t procid, uint32_t dirid, int16_t vref) {
    ENTRY_LINK(FN_OPENWD, 0x34, 4);                 /* link -0x34; move.l D7 */
    maddr pb = a6 - 0x34;
    W16(pb + 0x16, vref);
    W32(pb + 0x30, dirid);
    W32(pb + 0x1c, procid);
    W32(pb + 0x12, 0);
    uint32_t err = tc_PBOpenWD(0, pb);
    W16(wdp, R16(pb + 0x16));
    return err;
}

/* 0005bab8: CLOSEWD(wdRefNum): OSErr */
uint32_t CLOSEWD(int16_t wd) {
    ENTRY_LINK(FN_CLOSEWD, 0x34, 0);
    maddr pb = a6 - 0x34;
    W16(pb + 0x16, wd);
    return tc_PBCloseWD(0, pb);
}

/* ===================================================================================== */
/* Mohawk audio API                                                                      */
/*                                                                                       */
/* Client bytes (12, at osGetClientBytes(client) + gAudioClientOff): +0 initialised,     */
/* +2 active, +4 output device enabled, +6 MIDI out device, +8 wave out device,           */
/* +a first audio object (memory handle). Audio objects (in a locked memory handle):      */
/* +0 vtable, +4 next / +6 previous object handle, +8 handle (long), +c data handle,      */
/* +e client, +12 device enabled, +14 paused, +16 playing, +18 prepared, +1a device,     */
/* +1c notify proc, +20 notify instance.                                                  */
/* vtable: +0 destroy, +4 deviceEnable, +8 devicePrepare, +c deviceStart, +10 deviceStop, */
/* +14 deviceUnprepare, +18 getPosition, +1c notify, +20 pause, +24 prepare,              */
/* +28 releaseLoop, +2c resetLoop, +30 resume, +34 seek, +38 start, +3c stop,             */
/* +40 unprepare.                                                                         */
/* ===================================================================================== */

/* 0005bae6: audioActivate(on): enable/disable the output device of every object */
uint32_t audioActivate(int16_t on) {
    ENTRY_LINK(FN_audioActivate, 4, 32);            /* link -4; movem D3-D7/A2-A4 */
    W32(a6 - 4, 0xffffffff);
    maddr cb = audio_bytes(0xffffffff);
    if (R16(cb + 2) != 0 && on == 0) {
        int16_t h = R16(cb + 0xa);
        while (h != 0) {
            uint32_t obj = memXlat(h);
            maddr s0 = emu_sp();
            emu_set_sp(s0 - 2);
            push16(0);
            push32(obj);
            pmeth(s0, 2, vfn(obj, 4));              /* deviceEnable(obj, false) */
            h = R16(obj + 4);
        }
        W16(cb + 4, S16(mmaudioEnable(0)) != 0 ? 1 : 0);
    } else if (R16(cb + 2) == 0 && on != 0) {
        if (R16(cb + 4) == 0) {
            if (S16(mmaudioEnable(1)) != 0) return audio_err(AUDERR_NOTENABLED);
            W16(cb + 4, 1);
        }
        int16_t h = R16(cb + 0xa);
        while (h != 0) {
            uint32_t obj = memXlat(h);
            maddr s0 = emu_sp();
            emu_set_sp(s0 - 2);
            push16(1);
            push32(obj);
            int16_t err = (int16_t)pmeth(s0, 2, vfn(obj, 4));   /* deviceEnable(obj, true) */
            if (err != 0) {
                /* disable again: the original walks the failing object's "previous" link,
                 * re-reading it from the failing object every time */
                for (;;) {
                    int16_t ph = R16(obj + 6);
                    if (ph == 0) break;
                    maddr s1 = emu_sp();
                    emu_set_sp(s1 - 2);
                    push16(0);
                    uint32_t pobj = memXlat(ph);
                    push32(pobj);
                    pmeth(s1, 2, vfn(pobj, 4));
                }
                W16(cb + 4, S16(mmaudioEnable(0)) != 0 ? 1 : 0);
                return audio_err(err);
            }
            h = R16(obj + 4);
        }
    }
    W16(cb + 2, on);
    return audio_err(0);
}

/* 0005bc68: audioCreate(dataHandle): new audio object for a 'MHWK' MIDI or WAVE resource */
uint32_t audioCreate(int16_t data) {
    ENTRY_LINK(FN_audioCreate, 0x14, 32);           /* link -0x14; movem D3-D7/A2-A4 */
    uint32_t p = memXlat(data);
    if (p == 0) {
        audio_err(S16(memGetError()));
        return 0;
    }
    if (R32(p) != 0x4d48574b || R32(p + 4) < 4) {   /* 'MHWK' */
        audio_err(AUDERR_BADFORMAT);
        return 0;
    }
    uint32_t type = R32(p + 8);
    W32(a6 - 0xc, type);
    uint32_t obj;
    if (type == 0x4d494449) obj = create__7midiObj(data);        /* 'MIDI' */
    else if (type == 0x57415645) obj = create__7waveObj(data);   /* 'WAVE' */
    else {
        W16(a6 - 8, AUDERR_BADFORMAT);
        audio_err(AUDERR_BADFORMAT);
        obj = 0;
    }
    if (obj != 0) {
        W32(a6 - 4, 0xffffffff);
        maddr cb = audio_bytes(0xffffffff);
        W32(a6 - 0x14, cb);
        uint32_t o = memXlat((int16_t)obj);
        W32(a6 - 0x10, o);
        int16_t first = R16(cb + 0xa);
        W16(o + 4, first);                          /* insert at the head of the list */
        if (first != 0) {
            uint32_t f = memXlat(R16(o + 4));
            W16(f + 6, obj);
        }
        W16(o + 6, 0);
        W16(cb + 0xa, obj);
    }
    return obj;
}

/* 0005bd96: audioDestroy(obj) */
uint32_t audioDestroy(uint32_t h) {
    ENTRY_LINK(FN_audioDestroy, 0x10, 16);          /* link -0x10; movem D3/A2-A4 */
    W32(a6 - 8, h);
    if (h == 0) return audio_err(AUDERR_BADHANDLE);
    maddr s0 = emu_sp();
    emu_set_sp(s0 - 2);
    uint32_t obj = memXlat((int16_t)h);
    W32(a6 - 0x10, obj);
    push32(obj);
    if ((int16_t)pmeth(s0, 2, vfn(obj, 0)) == 0) {  /* destroy */
        uint32_t o = memXlat((int16_t)h);
        W32(a6 - 0xc, o);
        if (R16(o + 4) != 0) {
            int16_t prev = R16(o + 6);
            uint32_t n = memXlat(R16(o + 4));
            W16(n + 6, prev);
        }
        if (R16(o + 6) != 0) {
            int16_t next = R16(o + 4);
            uint32_t pv = memXlat(R16(o + 6));
            W16(pv + 4, next);
        } else {
            int16_t next = R16(o + 4);
            W32(a6 - 4, 0xffffffff);
            maddr cb = audio_bytes(0xffffffff);
            W16(cb + 0xa, next);
        }
        memFreeHandle((int16_t)h);
    }
    return RU16(gAudioError);
}

/* 0005be88: audioGetError() */
uint32_t audioGetError(void) {
    return RU16(gAudioError);
}

/* 0005beaa: audioGetPosition(obj): long */
uint32_t audioGetPosition(uint32_t h) {
    ENTRY_LINK(FN_audioGetPosition, 0, 20);         /* movem D3-D4/A2-A4 */
    if (h == 0) {
        W16(gAudioError, AUDERR_BADHANDLE);
        return 0xffffffff;
    }
    W16(gAudioError, 0);
    maddr s0 = emu_sp();
    emu_set_sp(s0 - 4);
    uint32_t obj = memXlat((int16_t)h);
    push32(obj);
    return pmeth(s0, 4, vfn(obj, 0x18));
}

/* 0005bf16: audioGetState(obj): 1 prepared | 2 paused | 4 playing */
uint32_t audioGetState(uint32_t h) {
    ENTRY_LINK(FN_audioGetState, 0, 20);            /* movem D3-D4/A2-A4 */
    if (h == 0) {
        W16(gAudioError, AUDERR_BADHANDLE);
        return 0xffffffff;
    }
    W16(gAudioError, 0);
    uint32_t obj = memXlat((int16_t)h);
    uint16_t st = R16(obj + 0x18) != 0 ? 1 : 0;
    st |= R16(obj + 0x14) != 0 ? 2 : 0;
    st |= R16(obj + 0x16) != 0 ? 4 : 0;
    return (obj & 0xffff0000u) | st;
}

/* 0005bf9c: audioInit() */
uint32_t audioInit(void) {
    ENTRY_LINK(FN_audioInit, 8, 32);                /* link -8; movem D3-D7/A2-A4 */
    KEEP_REGS();
    /* the libraries it needs must be initialised (version 1.0 or later), at least one of them */
    uint16_t q = (uint16_t)osQueryInit();
    set_reg_w(R_D3, 0x100);                         /* D3 = 0x100 from here on */
    if (q < 0x100 && (uint16_t)memQueryInit() < 0x100 && (uint16_t)timerQueryInit() < 0x100 &&
        (uint16_t)fileQueryInit() < 0x100 && (uint16_t)configQueryInit() < 0x100 && (uint16_t)resQueryInit() < 0x100)
        return audio_err(AUDERR_NOLIBS);
    uint16_t a2 = RU16(gAudioInitCount);
    if (a2 == 0) {
        tc_memset(gAudioError, 0, 6);
        int16_t off = S16(osReserveClientBytes(0xc));
        W16(gAudioClientOff, off);
        if (off == 0) return audio_err(S16(osGetError()));
        a2 = (uint16_t)off;
    }
    /* movea.l D0,A2: D0 is the count or the offset word over the high word of the last
     * call's D0 (the slot pop leaves it) */
    set_reg_w(R_A2, a2);
    uint16_t r = (uint16_t)mmaudioInit();
    if (r == 4) return audio_err(AUDERR_NOTENABLED);
    if (r != 0) {
        W16(a6 - 8, AUDERR_NOLIBS);
        return audio_err(AUDERR_NOLIBS);
    }
    if (S16(midiMapInit()) != 0) {
        mmaudioQuit();
        W16(a6 - 6, AUDERR_NODEVICE);
        return audio_err(AUDERR_NODEVICE);
    }
    cpu_set(R_A4, 0xffffffff);
    maddr cb = audio_bytes(0xffffffff);
    cpu_set(R_A3, cb);
    tc_memset(cb, 0, 0xc);
    W16(cb, 1);
    W16(cb + 2, 1);
    W16(cb + 4, 1);
    W16(gAudioInitCount, R16(gAudioInitCount) + 1);
    W16(a6 - 4, 0);
    return audio_err(0);
}

/* 0005c10c: audioPause(obj) */
uint32_t audioPause(uint32_t h) {
    ENTRY_LINK(FN_audioPause, 4, 20);               /* link -4; movem D3-D4/A2-A4 */
    maddr cb = audio_bytes(0xffffffff);
    if (R16(cb + 2) == 0) return audio_err(AUDERR_INACTIVE);
    W32(a6 - 4, h);
    if (h == 0) return audio_err(AUDERR_BADHANDLE);
    maddr s0 = emu_sp();
    emu_set_sp(s0 - 2);
    uint32_t obj = memXlat((int16_t)h);
    push32(obj);
    return pmeth(s0, 2, vfn(obj, 0x20));
}

/* 0005c19a: audioQueryInit(): version (0x100) when initialised, else 0 */
uint32_t audioQueryInit(void) {
    ENTRY_LINK(FN_audioQueryInit, 0, 4);            /* movem A2 */
    if (R16(gAudioInitCount) == 0) return 0;
    maddr cb = audio_bytes(0xffffffff);
    return R16(cb) != 0 ? 0x100 : 0;
}

/* 0005c1e8: audioQuit() (C convention) */
uint32_t audioQuit(void) {
    ENTRY_LINK(FN_audioQuit, 0, 12);                /* movem D3/A2-A3 */
    uint32_t q = audioQueryInit();
    if ((uint16_t)q == 0) {
        W16(gAudioError, AUDERR_NOTINIT);
        return q & 0xffff0000u;                     /* D0 = the cleared query result */
    }
    midiMapQuit();
    mmaudioQuit();
    uint32_t p = osGetClientBytes(0xffffffff);
    uint16_t off = RU16(gAudioClientOff);
    W16(p + off, 0);
    W16(gAudioInitCount, R16(gAudioInitCount) - 1);
    return off;                                     /* D0: the offset, zero-extended */
}

/* 0005c246: audioPrepare(device, obj) */
uint32_t audioPrepare(int16_t dev, uint32_t h) {
    ENTRY_LINK(FN_audioPrepare, 4, 24);             /* link -4; movem D3-D5/A2-A4 */
    maddr cb = audio_bytes(0xffffffff);
    if (R16(cb + 2) == 0) return audio_err(AUDERR_INACTIVE);
    W32(a6 - 4, h);
    if (h == 0) return audio_err(AUDERR_BADHANDLE);
    maddr s0 = emu_sp();
    emu_set_sp(s0 - 2);
    push16(dev);
    uint32_t obj = memXlat((int16_t)h);
    push32(obj);
    return pmeth(s0, 2, vfn(obj, 0x24));
}

/* 0005c2dc: audioReleaseLoop(obj) */
uint32_t audioReleaseLoop(uint32_t h) {
    ENTRY_LINK(FN_audioReleaseLoop, 4, 20);         /* link -4; movem D3-D4/A2-A4 */
    maddr cb = audio_bytes(0xffffffff);
    if (R16(cb + 2) == 0) return audio_err(AUDERR_INACTIVE);
    W32(a6 - 4, h);
    if (h == 0) return audio_err(AUDERR_BADHANDLE);
    maddr s0 = emu_sp();
    emu_set_sp(s0 - 2);
    uint32_t obj = memXlat((int16_t)h);
    push32(obj);
    return pmeth(s0, 2, vfn(obj, 0x28));
}

/* 0005c370: audioResume(obj) */
uint32_t audioResume(uint32_t h) {
    ENTRY_LINK(FN_audioResume, 4, 20);              /* link -4; movem D3-D4/A2-A4 */
    maddr cb = audio_bytes(0xffffffff);
    if (R16(cb + 2) == 0) return audio_err(AUDERR_INACTIVE);
    W32(a6 - 4, h);
    if (h == 0) return audio_err(AUDERR_BADHANDLE);
    maddr s0 = emu_sp();
    emu_set_sp(s0 - 2);
    uint32_t obj = memXlat((int16_t)h);
    push32(obj);
    return pmeth(s0, 2, vfn(obj, 0x30));
}

/* 0005c400: audioSeek(obj, position); seeking to 0 also resets the loop */
uint32_t audioSeek(uint32_t pos, uint32_t h) {
    ENTRY_LINK(FN_audioSeek, 0, 24);                /* movem D3-D5/A2-A4 */
    if (h == 0) return audio_err(AUDERR_BADHANDLE);
    uint32_t obj = memXlat((int16_t)h);
    maddr s0 = emu_sp();
    emu_set_sp(s0 - 2);
    push32(pos);
    push32(obj);
    int16_t r = (int16_t)pmeth(s0, 2, vfn(obj, 0x34));
    if (r == 0 && pos == 0) {
        push32(obj);
        pmeth_void(s0, vfn(obj, 0x2c));             /* resetLoop */
    }
    return (uint16_t)r;
}

/* 0005c47c: audioStart(obj, notifyProc, instance): three longs pushed as six words */
uint32_t audioStart(int16_t p4, int16_t p6, int16_t p8, int16_t p10, int16_t p12, int16_t p14) {
    ENTRY_LINK(FN_audioStart, 8, 24);               /* link -8; movem D3-D5/A2-A4 */
    uint32_t inst = (uint32_t)(uint16_t)p4 << 16 | (uint16_t)p6;
    uint32_t proc = (uint32_t)(uint16_t)p8 << 16 | (uint16_t)p10;
    uint32_t h = (uint32_t)(uint16_t)p12 << 16 | (uint16_t)p14;
    W32(a6 - 8, 0xffffffff);
    maddr cb = audio_bytes(0xffffffff);
    if (R16(cb + 2) == 0) return audio_err(AUDERR_INACTIVE);
    W32(a6 - 4, h);
    if (h == 0) return audio_err(AUDERR_BADHANDLE);
    maddr s0 = emu_sp();
    emu_set_sp(s0 - 2);
    push32(proc);
    push32(inst);
    uint32_t obj = memXlat((int16_t)h);
    push32(obj);
    return pmeth(s0, 2, vfn(obj, 0x38));
}

/* 0005c51a: audioStop(obj) */
uint32_t audioStop(uint32_t h) {
    ENTRY_LINK(FN_audioStop, 0, 16);                /* movem D3/A2-A4 */
    if (h == 0) return audio_err(AUDERR_BADHANDLE);
    maddr s0 = emu_sp();
    emu_set_sp(s0 - 2);
    uint32_t obj = memXlat((int16_t)h);
    push32(obj);
    return pmeth(s0, 2, vfn(obj, 0x3c));
}

/* 0005c578: audioUnprepare(obj) */
uint32_t audioUnprepare(uint32_t h) {
    ENTRY_LINK(FN_audioUnprepare, 0, 16);           /* movem D3/A2-A4 */
    if (h == 0) return audio_err(AUDERR_BADHANDLE);
    maddr s0 = emu_sp();
    emu_set_sp(s0 - 2);
    uint32_t obj = memXlat((int16_t)h);
    push32(obj);
    return pmeth(s0, 2, vfn(obj, 0x40));
}

/* Device counts: C functions that store their result in the caller's slot */
static uint16_t num_devs(int fn) {
    maddr s0 = emu_sp();
    emu_set_sp(s0 - 2);
    call68_addr(JT_ADDR(fn));
    uint16_t n = RU16(s0 - 2);
    emu_set_sp(s0);
    return n;
}

/* 0005c5da: audioUseMidiOutDevice(dev): previous device, -1 if dev is out of range */
uint32_t audioUseMidiOutDevice(int16_t dev) {
    ENTRY_LINK(FN_audioUseMidiOutDevice, 4, 28);    /* link -4; movem D3-D6/A2-A4 */
    int32_t want = (int32_t)(uint16_t)dev + 1;
    uint16_t n = num_devs(FN_midiOutGetNumDevs);
    if (want > (int32_t)n) {
        W16(gAudioError, AUDERR_BADDEVICE);
        return 0xffffffff;
    }
    W16(gAudioError, 0);
    W32(a6 - 4, 0xffffffff);
    maddr cb = audio_bytes(0xffffffff);
    uint16_t old = RU16(cb + 6);
    W16(cb + 6, dev);
    return old;
}

/* 0005c66e: audioUseWaveOutDevice(dev): previous device, -1 if dev is out of range */
uint32_t audioUseWaveOutDevice(int16_t dev) {
    ENTRY_LINK(FN_audioUseWaveOutDevice, 4, 28);    /* link -4; movem D3-D6/A2-A4 */
    int32_t want = (int32_t)(uint16_t)dev + 1;
    uint16_t n = num_devs(FN_waveOutGetNumDevs);
    if (want > (int32_t)n) {
        W16(gAudioError, AUDERR_BADDEVICE);
        return 0xffffffff;
    }
    W16(gAudioError, 0);
    W32(a6 - 4, 0xffffffff);
    maddr cb = audio_bytes(0xffffffff);
    uint16_t old = RU16(cb + 8);
    W16(cb + 8, dev);
    return old;
}

/* 0005c702: audioObj::destroy(): refused while paused, playing or prepared */
uint32_t destroy__8audioObj(uint32_t self) {
    ENTRY_LINK(FN_destroy__8audioObj, 0, 24);       /* movem D3-D6/A2-A3 */
    if (R16(self + 0x14) != 0) return audio_err(AUDERR_PAUSED);
    if (R16(self + 0x16) != 0) return audio_err(AUDERR_PLAYING);
    if (R16(self + 0x18) != 0) return audio_err(AUDERR_PREPARED);
    memUnlock(R16(self + 0xc));
    memUnlock((int16_t)R32(self + 8));
    return audio_err(0);
}

/* 0005c7a0: audioObj::deviceEnable(on) */
uint32_t deviceEnable__8audioObj(uint32_t self, int16_t on) {
    ENTRY_LINK(FN_deviceEnable__8audioObj, 0, 20);  /* movem D3-D5/A2-A3 */
    if (R16(self + 0x12) != 0 && on == 0) {
        maddr s0 = emu_sp();
        if (R16(self + 0x14) != 0 || R16(self + 0x16) != 0) {
            push32(self);
            pmeth_void(s0, vfn(self, 0x10));        /* deviceStop */
        }
        if (R16(self + 0x18) != 0) {
            push32(self);
            pmeth_void(s0, vfn(self, 0x14));        /* deviceUnprepare */
        }
    } else if (R16(self + 0x12) == 0 && on != 0 && R16(self + 0x18) != 0) {
        maddr s0 = emu_sp();
        emu_set_sp(s0 - 2);
        push32(self);
        int16_t r = (int16_t)pmeth(s0, 2, vfn(self, 8));   /* devicePrepare */
        if (r != 0) return (uint16_t)r;
        if (R16(self + 0x14) != 0 || R16(self + 0x16) != 0) {
            emu_set_sp(s0 - 2);
            push16(R16(self + 0x14));
            push32(self);
            r = (int16_t)pmeth(s0, 2, vfn(self, 0xc));      /* deviceStart(paused) */
            if (r != 0) {
                push32(self);
                pmeth_void(s0, vfn(self, 0x14));            /* deviceUnprepare */
                return (uint16_t)r;
            }
        }
    }
    W16(self + 0x12, on);
    return audio_err(0);
}

/* 0005c88a: audioObj::notify(msg): calls the client's notify proc (Pascal
 * proc(instance, msg, obj)) under the client's instance */
uint32_t notify__8audioObj(uint32_t self, uint32_t msg) {
    ENTRY_LINK(FN_notify__8audioObj, 0, 16);        /* movem D3/A2-A4 */
    if (R32(self + 0x1c) != 0) {
        maddr s0 = emu_sp();
        emu_set_sp(s0 - 2);                         /* osSetInstance's result slot */
        uint32_t inst = osGetClientInstance(R32(self + 0xe));
        emu_set_sp(s0);
        int16_t old = S16(osSetInstance((int16_t)inst));
        push32(R32(self + 8));
        push32(msg);
        push32(R32(self + 0x20));
        pmeth_void(s0, R32(self + 0x1c));
        osSetInstance(old);
    }
    return 0;
}

/* ===================================================================================== */
/* midiObj (Standard MIDI File player). Object (0xda + 16 * tracks bytes): audioObj      */
/* fields, +24 data, +28 data end offset, +2c 'Prg#' chunk, +30 midiMap, +34 device caps, */
/* +62 caps flags, +66 MIDIHDR (0x1c), +9e timer, +a2 timer period, +a4 start time,       */
/* +a8 elapsed, +b8 position, +bc/+c0 tempo state, +c2 first event time, +c6..+cc loop,   */
/* +d2 tracks done, +d4 format, +d6 division, +d8 track count, +da tracks {+0 start,     */
/* +4 pointer, +8 done, +a delta, +e running status}.                                     */
/* ===================================================================================== */

/* 0005c8f8: midiObj::cachePatches(op): the 'Prg#' chunk lists {key, patch} pairs */
uint32_t cachePatches__7midiObj(uint32_t self, int16_t op) {
    ENTRY_LINK(FN_cachePatches__7midiObj, 0x108, 28);   /* link -0x108; movem D3-D7/A2-A3 */
    if (R32(self + 0x2c) == 0 || (R32(self + 0x62) & 4) == 0) return 0;
    maddr patches = a6 - 0x108;
    tc_memset(patches, 0, 0x100);
    for (uint16_t i = 0; ; i++) {
        uint16_t n = RU16(R32(self + 0x2c) + 8);
        if (n <= i) break;
        W32(a6 - 4, i);
        maddr e = R32(self + 0x2c) + 0xa + (uint32_t)i * 4;
        uint16_t patch = RU16(e + 2);
        W16(a6 - 6, patch);
        uint16_t key = RU16(R32(self + 0x2c) + 0xa + (uint32_t)i * 4);
        W16(a6 - 8, key);
        W16(patches + (key & 0x7f) * 2, patch);
    }
    return midiMapCachePatches(op, patches, 0, R32(self + 0x30));
}

/* 0005c9da: midiObj::create(dataHandle): parses the 'MHWK' 'MIDI' resource (MThd, MTrk
 * and Prg# chunks); returns the object's memory handle */
uint32_t create__7midiObj(int16_t data) {
    ENTRY_LINK(FN_create__7midiObj, 0x60, 32);      /* link -0x60; movem D3-D7/A2-A4 */
    uint32_t p = memLockXtd(data);
    W32(a6 - 0x48, R32(p + 4));
    uint32_t end = R32(p + 4) + 8;
    W32(a6 - 0x44, R32(p));
    if (R32(p) != 0x4d48574b || (W32(a6 - 0x40, R32(p + 8)), R32(p + 8) != 0x4d494449)) {  /* 'MHWK' 'MIDI' */
        memUnlock(data);
        W16(a6 - 0x3c, AUDERR_BADFORMAT);
        W16(gAudioError, AUDERR_BADFORMAT);
        return 0;
    }
    maddr hdr = p + 0xc;
    uint16_t fmt = RU16(hdr + 8);
    W16(a6 - 0x3a, fmt);
    uint16_t ntrk = RU16(hdr + 0xa);
    W16(a6 - 0x38, ntrk);
    uint16_t div = RU16(hdr + 0xc);
    W16(a6 - 0x36, div);
    W16(a6 - 0x60, div);
    W32(a6 - 0x34, R32(hdr));
    if (R32(hdr) != 0x4d546864 || (div & 0x8000) || fmt >= 2 || ntrk == 0) {   /* 'MThd' */
        memUnlock(data);
        W16(a6 - 0x30, AUDERR_BADMIDI);
        W16(gAudioError, AUDERR_BADMIDI);
        return 0;
    }
    uint16_t size = (uint16_t)((ntrk << 4) + 0xda);
    W16(a6 - 0x5e, size);
    int16_t mh = S16(memAllocHandle(1, size));
    W16(a6 - 0x5c, mh);
    if (mh == 0) {
        memUnlock(data);
        int16_t e = S16(memGetError());
        W16(a6 - 0x2e, e);
        W16(gAudioError, e);
        return 0;
    }
    uint32_t m = memLockXtd(mh);
    W32(a6 - 0x2c, m);
    W32(a6 - 0x28, 0xea);
    uint32_t obj = tc_memset(m, 0, 0xea);
    W32(a6 - 0x58, obj);
    if (obj != 0) {
        W32(a6 - 0x24, obj);
        W32(a6 - 0x20, obj);
        W32(obj, VT_audioObj);                      /* audioObj constructor, then midiObj */
        W32(obj, VT_midiObj);
    }
    W32(a6 - 0x54, obj);
    W32(obj + 8, (uint16_t)mh);
    W16(obj + 0xc, data);
    uint32_t client = osGetClient();
    W32(obj + 0xe, client);
    W32(a6 - 0x1c, client);
    maddr cb = audio_bytes(client);
    W16(obj + 0x12, R16(cb + 2));
    W32(obj + 0x24, p);
    W32(obj + 0x28, end);
    W16(obj + 0xd4, fmt);
    W16(obj + 0xd6, R16(a6 - 0x60));
    W32(a6 - 0x18, R32(hdr + 4));
    maddr ck = hdr + 8 + R32(hdr + 4);
    W32(a6 - 0x50, ck);
    for (;;) {
        if (!(p + end + 1 > ck)) {
            W16(a6 - 0x14, AUDERR_BADFORMAT);
            W16(gAudioError, AUDERR_BADFORMAT);
            goto fail;
        }
        W32(a6 - 0x10, R32(ck));
        if (R32(ck) == 0x4d54726b) {                /* 'MTrk' */
            uint16_t t = RU16(obj + 0xd8);
            W16(obj + 0xd8, RU16(obj + 0xd8) + 1);
            maddr trk = obj + 0xda + (uint32_t)t * 16;
            W32(a6 - 0x4c, trk);
            W32(trk, ck + 8);
        } else {
            W32(a6 - 0xc, R32(ck));
            if (R32(ck) == 0x50726723) {            /* 'Prg#' */
                if (R32(obj + 0x2c) != 0) {
                    W16(a6 - 8, AUDERR_BADFORMAT);
                    W16(gAudioError, AUDERR_BADFORMAT);
                    goto fail;
                }
                W32(obj + 0x2c, ck);
            }
        }
        W32(a6 - 4, R32(ck + 4));
        ck = ck + 8 + ((R32(ck + 4) + 1) & ~1u);
        W32(a6 - 0x50, ck);
        if (p + end <= ck) break;
    }
    {
        maddr s0 = emu_sp();
        emu_set_sp(s0 - 2);
        push32(0);
        push32(obj);
        pmeth(s0, 2, vfn(obj, 0x34));               /* seek(0) */
        push32(obj);
        pmeth_void(s0, vfn(obj, 0x2c));             /* resetLoop */
    }
    return R32(obj + 8);
fail:
    memFreeHandle(R16(a6 - 0x5c));
    memUnlock(data);
    return 0;
}

/* 0005ccd8: midiObj::devicePrepare(): opens the midiMap, caches the patches and prepares
 * the header for the whole data */
uint32_t devicePrepare__7midiObj(uint32_t self) {
    ENTRY_LINK(FN_devicePrepare__7midiObj, 0, 28);  /* movem D3-D7/A2-A3 */
    maddr mapp = self + 0x30;
    int16_t r = S16(midiMapOpen(0, 0, 0, 0, 0, R16(self + 0x1a), (int16_t)(mapp >> 16), (int16_t)mapp));
    if (r != 0) {
        int16_t e = r == 4 ? AUDERR_NOTENABLED : r == 2 ? AUDERR_BADDEVICE : AUDERR_NODEVICE;
        return audio_err(e);
    }
    midiOutGetDevCaps(0x32, self + 0x34, R16(self + 0x1a));
    if (S16(cachePatches__7midiObj(self, 1)) != 0) {
        midiMapClose(R32(self + 0x30));
        return audio_err(AUDERR_NODEVICE);
    }
    tc_memset(self + 0x66, 0, 0x1c);
    W32(self + 0x66, R32(self + 0x24));
    W32(self + 0x6a, R32(self + 0x28));
    if (S16(midiMapPrepareHeader(0x1c, self + 0x66, R32(self + 0x30))) != 0) {
        cachePatches__7midiObj(self, 4);
        midiMapClose(R32(self + 0x30));
        return audio_err(AUDERR_NODEVICE);
    }
    return audio_err(0);
}

/* Start the playback timer (deviceStart/resume): returns the timer */
static uint32_t midi_start_timer(uint32_t self) {
    advance__7midiObj(self, 1, 0);
    W16(self + 0xa2, calcTime2Next__7midiObj(self));
    W32(self + 0xa4, timerGetTime());
    W32(self + 0xa8, 0);
    uint32_t client = R32(self + 0xe);
    uint32_t t = timerCreate(self, JT_ADDR(FN_timerProc__7midiObj), 0, R16(self + 0xa2),
                             (int16_t)(client >> 16), (int16_t)client);
    W32(self + 0x9e, t);
    return t;
}
/* Stop the timer and advance to the current time (deviceStop/pause) */
static void midi_stop_timer(uint32_t self) {
    timerDestroy(R32(self + 0x9e));
    W32(self + 0x9e, 0);
    uint32_t now = timerGetTime();
    uint16_t base = (uint16_t)(R32(self + 0xa4) + (uint16_t)R32(self + 0xa8));
    advance__7midiObj(self, 1, (int16_t)(now - base));
}
/* Notify the client: msg byte in the caller's frame, notify(this, &msg) */
static void midi_notify(uint32_t self, maddr msgp, uint8_t msg) {
    W8(msgp, msg);
    maddr s0 = emu_sp();
    push32(msgp);
    push32(self);
    pmeth_void(s0, vfn(self, 0x1c));
}

/* 0005ce18: midiObj::deviceStart(paused) */
uint32_t deviceStart__7midiObj(uint32_t self, int16_t paused) {
    ENTRY_LINK(FN_deviceStart__7midiObj, 0, 20);    /* movem D3-D5/A2-A3 */
    if (paused == 0 && midi_start_timer(self) == 0) return audio_err(S16(timerGetError()));
    return audio_err(0);
}

/* 0005cec4: midiObj::deviceStop() */
uint32_t deviceStop__7midiObj(uint32_t self) {
    ENTRY_LINK(FN_deviceStop__7midiObj, 0, 8);      /* movem A2-A3 */
    if (R16(self + 0x16) != 0) midi_stop_timer(self);
    midiMapReset(R32(self + 0x30));
    return 0;
}

/* 0005cf40: midiObj::deviceUnprepare() */
uint32_t deviceUnprepare__7midiObj(uint32_t self) {
    ENTRY_LINK(FN_deviceUnprepare__7midiObj, 0, 8); /* movem A2-A3 */
    midiMapUnprepareHeader(0x1c, self + 0x66, R32(self + 0x30));
    cachePatches__7midiObj(self, 4);
    midiMapClose(R32(self + 0x30));
    return 0;
}

/* 0005cfaa: midiObj::getPosition(): position read with interrupts off (this pushed as a long,
 * the index splits it into two words) */
uint32_t getPosition__7midiObj(int16_t p4, int16_t p6) {
    ENTRY_LINK(FN_getPosition__7midiObj, 0, 12);    /* movem D3-D4/A2 */
    uint32_t self = (uint32_t)(uint16_t)p4 << 16 | (uint16_t)p6;
    int16_t s = S16(osIntrEnable(0));
    uint32_t pos = R32(self + 0xb8);
    osIntrEnable(s);
    return pos;
}

/* 0005d000: midiObj::pause() */
uint32_t pause__7midiObj(uint32_t self) {
    ENTRY_LINK(FN_pause__7midiObj, 8, 16);          /* link -8; movem D3-D4/A2-A3 */
    if (R16(self + 0x16) == 0) return audio_err(AUDERR_NOTPLAYING);
    midi_stop_timer(self);
    W16(self + 0x16, 0);
    W16(self + 0x14, 1);
    midi_notify(self, a6 - 8, 2);
    return audio_err(0);
}

/* 0005d0a8: midiObj::prepare(device): -1 = the client's MIDI out device */
uint32_t prepare__7midiObj(uint32_t self, int16_t dev) {
    ENTRY_LINK(FN_prepare__7midiObj, 0, 28);        /* movem D3-D6/A2-A4 */
    if (R16(self + 0x18) != 0) return audio_err(AUDERR_PREPARED);
    int16_t d = dev;
    if (dev == -1) {
        maddr cb = audio_bytes(R32(self + 0xe));
        d = R16(cb + 6);
    }
    W16(self + 0x1a, d);
    maddr s0 = emu_sp();
    emu_set_sp(s0 - 2);
    push32(self);
    int16_t r = (int16_t)pmeth(s0, 2, vfn(self, 8));   /* devicePrepare */
    if (r == 0) {
        memPageLock(self);
        W16(self + 0x18, 1);
    }
    return (uint16_t)r;
}

/* 0005d150: midiObj::releaseLoop() */
uint32_t releaseLoop__7midiObj(uint32_t self) {
    if (R16(self + 0x18) == 0) return audio_err(AUDERR_NOTPREP);
    W16(self + 0xca, 1);
    return audio_err(0);
}

/* 0005d1ae: midiObj::resetLoop() */
uint32_t resetLoop__7midiObj(uint32_t self) {
    W16(self + 0xc6, 0);
    W16(self + 0xc8, 0);
    W16(self + 0xca, 0);
    W16(self + 0xcc, 0);
    return 0;
}

/* 0005d1f8: midiObj::resume() */
uint32_t resume__7midiObj(uint32_t self) {
    ENTRY_LINK(FN_resume__7midiObj, 8, 20);         /* link -8; movem D3-D5/A2-A3 */
    if (R16(self + 0x14) == 0) return audio_err(AUDERR_NOTPAUSED);
    if (midi_start_timer(self) == 0) return audio_err(S16(timerGetError()));
    W16(self + 0x14, 0);
    W16(self + 0x16, 1);
    midi_notify(self, a6 - 8, 3);
    return audio_err(0);
}

/* 0005d2d2: midiObj::seek(position): rewinds when seeking backwards, then advances */
uint32_t seek__7midiObj(uint32_t self, uint32_t pos) {
    ENTRY_LINK(FN_seek__7midiObj, 4, 32);           /* link -4; movem D3-D7/A2-A4 */
    if (R16(self + 0x14) != 0) return audio_err(AUDERR_PAUSED);
    if (R16(self + 0x16) != 0) return audio_err(AUDERR_PLAYING);
    if (pos == 0 || pos < R32(self + 0xb8)) {
        W32(self + 0xc2, 0xffffffff);
        for (uint16_t i = 0; i < RU16(self + 0xd8); i++) {
            maddr trk = self + 0xda + (uint32_t)i * 16;
            W8(trk + 0xe, 0);
            W16(trk + 8, 0);
            W32(trk + 4, R32(trk));
            uint32_t d = getVLQ__7midiObj(trk + 4);
            W32(trk + 0xa, d);
            if (d < R32(self + 0xc2)) W32(self + 0xc2, R32(trk + 0xa));
        }
        calcTempo__7midiObj(self, 0x7a120);
        W32(self + 0xb8, 0);
        W32(self + 0xbc, 0);
        W16(self + 0xc0, 0);
        W16(self + 0xd2, 0);
    }
    while (R32(self + 0xb8) < pos && RU16(self + 0xd2) < RU16(self + 0xd8)) {
        uint16_t t = (uint16_t)calcTime2Next__7midiObj(self);
        if (R32(self + 0xb8) + t < pos) advance__7midiObj(self, 0, (int16_t)t);
        else advance__7midiObj(self, 0, (int16_t)((uint16_t)pos - (uint16_t)R32(self + 0xb8)));
    }
    if (RU16(self + 0xd2) == RU16(self + 0xd8) && R32(self + 0xb8) < pos && pos != 0xffffffff) {
        W16(a6 - 4, AUDERR_SEEKRANGE);
        return audio_err(AUDERR_SEEKRANGE);
    }
    W16(a6 - 2, 0);
    return audio_err(0);
}

/* 0005d464: midiObj::start(instance, notifyProc) */
uint32_t start__7midiObj(uint32_t self, uint32_t inst, uint32_t proc) {
    ENTRY_LINK(FN_start__7midiObj, 8, 32);          /* link -8; movem D3-D7/A2-A4 */
    if (R16(self + 0x18) == 0) return audio_err(AUDERR_NOTPREP);
    if (R16(self + 0x14) != 0) return audio_err(AUDERR_PAUSED);
    if (R16(self + 0x16) != 0) return audio_err(AUDERR_PLAYING);
    W32(self + 0x1c, proc);
    W32(self + 0x20, inst);
    maddr s0 = emu_sp();
    emu_set_sp(s0 - 2);
    push16(0);
    push32(self);
    int16_t r = (int16_t)pmeth(s0, 2, vfn(self, 0xc));   /* deviceStart(false) */
    if (r == 0) {
        W16(self + 0x16, 1);
        midi_notify(self, a6 - 8, 4);
    }
    return (uint16_t)r;
}

/* 0005d524: midiObj::stop() */
uint32_t stop__7midiObj(uint32_t self) {
    ENTRY_LINK(FN_stop__7midiObj, 8, 20);           /* link -8; movem D3-D5/A2-A3 */
    if (R16(self + 0x18) == 0) return audio_err(AUDERR_NOTPREP);
    if (R16(self + 0x16) == 0 && R16(self + 0x14) == 0) return audio_err(0);
    if (R16(self + 0x12) != 0) {
        maddr s0 = emu_sp();
        push32(self);
        pmeth_void(s0, vfn(self, 0x10));            /* deviceStop */
    }
    W16(self + 0x14, 0);
    W16(self + 0x16, 0);
    midi_notify(self, a6 - 8, 5);
    return audio_err(0);
}

/* 0005d5c2: midiObj::unprepare() */
uint32_t unprepare__7midiObj(uint32_t self) {
    ENTRY_LINK(FN_unprepare__7midiObj, 0, 20);      /* movem D3-D6/A2 */
    if (R16(self + 0x14) != 0) return audio_err(AUDERR_PAUSED);
    if (R16(self + 0x16) != 0) return audio_err(AUDERR_PLAYING);
    if (R16(self + 0x18) == 0) return audio_err(AUDERR_NOTPREP);
    memPageUnlock(self);
    if (R16(self + 0x12) != 0) {
        maddr s0 = emu_sp();
        push32(self);
        pmeth_void(s0, vfn(self, 0x14));            /* deviceUnprepare */
    }
    W16(self + 0x18, 0);
    return audio_err(0);
}

/* ===================================================================================== */
/* midiMap (MIDI output mapping): map = {+0 device record, +4 next, +8 prev}; device      */
/* record: +34 id, +46 open count, +48 midiOut device, +4a current map.                   */
/* ===================================================================================== */

/* 0005d66a: midiMapCachePatches(op, patches, bank, map): 5 = no map */
uint32_t midiMapCachePatches(int16_t op, uint32_t patches, int16_t bank, uint32_t map) {
    ENTRY_LINK(FN_midiMapCachePatches, 4, 24);      /* link -4; movem D3-D5/A2-A4 */
    W32(a6 - 4, map);
    if (map == 0) return 5;
    return midiOutCachePatches(op, patches, bank, R16(R32(map) + 0x48));
}

/* 0005d6de: midiMapClose(map): on the last close, sends the device's reset SysEx resource
 * (waiting until it is done) and closes the midiOut device; unlinks and frees the map */
uint32_t midiMapClose(uint32_t map) {
    ENTRY_LINK(FN_midiMapClose, 0x34, 20);          /* link -0x34; movem D3-D4/A2-A4 */
    W32(a6 - 8, map);
    W32(a6 - 0x34, map);
    if (map == 0) return 5;
    maddr dev = R32(map);
    W32(a6 - 0x30, dev);
    if (R16(dev + 0x46) == 1) {
        W32(a6 - 4, 0xffffffff);
        maddr cb = osGetClientBytes(0xffffffff) + RU16(gMidiMapClientOff);
        W32(a6 - 0x2c, cb);
        if (R32(cb + 2) != 0) {
            uint32_t res = resFind(0, (int16_t)-R16(dev + 0x34), R32(gMidiMapResFile));
            W32(a6 - 0x28, res);
            if (res != 0) {
                int16_t h = S16(resLoad(res));
                if (h != 0) {
                    maddr hdr = a6 - 0x24;          /* MIDIHDR */
                    tc_memset(hdr, 0, 0x1c);
                    W32(hdr, memLock(h));
                    W32(hdr + 4, memGetHandleSize(h));
                    if (S16(midiOutPrepareHeader(0x1c, hdr, R16(dev + 0x48))) == 0) {
                        if (S16(midiOutLongMsg(0x1c, hdr, R16(dev + 0x48))) == 0) {
                            while ((R32(hdr + 0x10) & 1) == 0) { }   /* MHDR_DONE */
                        }
                        midiOutUnprepareHeader(0x1c, hdr, R16(dev + 0x48));
                    }
                    memUnlock(h);
                    resSetPurgeable(1, R32(a6 - 0x28));
                }
            }
        }
        midiOutClose(R16(dev + 0x48));
        memPageUnlock(dev);
    }
    W16(dev + 0x46, R16(dev + 0x46) - 1);
    W32(R32(map + 8) + 4, R32(map + 4));
    W32(R32(map + 4) + 8, R32(map + 8));
    if (R16(dev + 0x46) == 0) W32(dev + 0x4a, 0);
    else if (R32(dev + 0x4a) == map) W32(dev + 0x4a, R32(map + 4));
    memPageUnlock(map);
    memFreePtr(map);
    return 0;
}
