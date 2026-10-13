/* CODE 1: THINK C startup and runtime helpers (0x10010-0x10420).
 *   - tc__start: application entry (jump table entry 0): string literals ('STRS'), A5
 *     globals ('DATA'/'ZERO'/'DREL'), _LoadSeg/_ExitToShell patches, C++ static
 *     constructors, main(), exit hook, ExitToShell
 *   - the _LoadSeg patch that applies 'CREL' relocations to CODE segments
 *   - long multiply/divide helpers (C-callable, callee pops) and their register cores
 * These routines are hand-written assembly with register interfaces. The translation
 * works on the emulated registers themselves (cpu_get/cpu_set) so that every register a
 * routine leaves behind is the original's, and Toolbox calls see the same registers.
 *
 * Not ported (they stay on the 68k core): the three switch helpers tc__switch_word_list,
 * tc__switch_long_list and tc__switch_range (0x10148/0x1015c/0x10170). They pop their
 * return address as a pointer to an inline case table and jump into the middle of the
 * calling function; a hooked native routine always returns to the popped address, so they
 * cannot be expressed as one. They are not referenced by the game (no jsr through their
 * jump-table slots 0x2a/0x32/0x3a(A5)), and ported code uses a C switch instead.
 * Addresses in comments are Ghidra addresses (see docs/PORTING.md). */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

#define RG(r)           cpu_get(r)
#define SETL(r, v)      cpu_set((r), (uint32_t)(v))
#define SETW(r, v)      cpu_set((r), (cpu_get(r) & 0xffff0000u) | (uint16_t)(v))
#define SETB(r, v)      cpu_set((r), (cpu_get(r) & 0xffffff00u) | (uint8_t)(v))

#define ROM85           0x28e       /* low memory: < 0 on 64K ROMs */
#define RESLOAD         0xa5e       /* low memory: ResLoad (byte) */
#define CURSTACKBASE    0x908       /* low memory: CurStackBase (bottom of the A5 globals) */
#define CURRENTA5       0x904

/* CODE 1 holds a few longs before tc__start (Ghidra 0x10004..0x1000f) */
static maddr code1(uint32_t off) { return fn_address(FN_tc__start) - 0x10 + off; }
#define C1_STRS_BASE    0x04        /* long: base of the string literals (A4), set by tc__start */
#define C1_MAIN_JT      0x08        /* long: jump-table offset of main */
#define C1_STI_JT       0x0c        /* long: jump-table offset of tc__run_STI (0: none) */

/* Pascal Toolbox call whose result slot stays on the stack ("subq.l #4,SP" ... trap) */
static void getres_push(uint32_t type, uint16_t id) {
    push32(0);                                             /* result slot */
    push32(type);
    push16(id);
    tb_trap(0xA9A0);                                       /* GetResource */
}

/* 00010010: application entry, reached by a jmp from jump-table entry 0 (no return
 * address); never returns */
uint32_t tc__start(void) {
    W16(0xa4a, 0);                                         /* clr.w $A4A */
    SETL(R_A6, 0);
    SETL(R_D6, 0x53545253);                                /* 'STRS' */
    tc__load_STRS();
    SETL(R_A0, code1(C1_STRS_BASE));
    W32(RG(R_A0), RG(R_A4));
    tc__init_globals();
    tc__install_trap_patches();
    tc__call_STI();
    /* main(argc = 1, argv, envp): two pointers into the words pushed just before */
    push16(0);
    push32(0xffff);
    { maddr sp = emu_sp(); push32(sp + 4); }               /* pea (4,SP) */
    { maddr sp = emu_sp(); push32(sp); }                   /* pea (SP) */
    push32(1);
    SETL(R_D1, R32(code1(C1_MAIN_JT)));
    call68_addr(RG(R_A5) + (int16_t)RG(R_D1));
    SETL(R_A5, R32(CURRENTA5));
    SETL(R_A0, R32(RG(R_A5) + 0x6c));
    call68_addr(RG(R_A0));                                 /* exit hook */
    tb_trap(0xA9F4);                                       /* ExitToShell */
    return 0;
}
/* entered by jmp: the hook popped a "return address" that was never pushed */
void glue_tc__start(void) { emu_set_sp(emu_sp() - 4); tc__start(); }

/* 00010058: C++ static constructors through the jump-table offset at 0x1000c */
uint32_t tc__call_STI(void) {
    ENTRY_NOLINK(FN_tc__call_STI, 0);
    SETL(R_D1, R32(code1(C1_STI_JT)));
    if (RG(R_D1) != 0) {
        uint32_t a0, d0 = cpu_call(RG(R_A5) + (int16_t)RG(R_D1), &a0);
        SETL(R_D0, d0);                                    /* keep the callee's D0/A0 like a jsr */
        SETL(R_A0, a0);
    }
    return 0;
}
void glue_tc__call_STI(void) { tc__call_STI(); }

/* 00010064: head of the _LoadSeg patch (stub {jsr here; jmp old LoadSeg}): a CODE segment
 * whose first byte has bit 7 set is relocated once with its 'CREL' resource. Saves and
 * restores D0-D7/A0-A4. */
static void relocate_body(void);
uint32_t tc__LoadSeg_reloc_patch(void) {
    ENTRY_NOLINK(FN_tc__LoadSeg_reloc_patch, 52);
    static const int regs[13] = { R_D0, R_D1, R_D2, R_D3, R_D4, R_D5, R_D6, R_D7, R_A0, R_A1, R_A2, R_A3, R_A4 };
    uint32_t saved[13];
    maddr sp = emu_sp();
    for (int i = 0; i < 13; i++) { saved[i] = RG(regs[i]); W32(sp + 4 * i, saved[i]); }   /* movem.l -(SP) */
    SETW(R_D7, R16(sp + 0x3c));                            /* the LoadSeg segment number */
    W8(RESLOAD, 0xff);
    getres_push(0x434f4445, (uint16_t)RG(R_D7));           /* 'CODE' */
    SETL(R_D0, pop32());
    if (RG(R_D0) == 0) goto syserr;
    SETL(R_A3, RG(R_D0));
    SETL(R_A2, R32(RG(R_A3)));
    {
        uint8_t b = R8(RG(R_A2));
        W8(RG(R_A2), b & 0x7f);                            /* bclr #7,(A2) */
        if (b & 0x80) {
            tc__save_handle_state();
            tb_trap(0xA029);                               /* HLock(A0) */
            getres_push(0x4352454c, (uint16_t)RG(R_D7));   /* 'CREL' */
            tc__restore_handle_state();
            SETL(R_D0, pop32());
            if (RG(R_D0) == 0) goto syserr;
            SETL(R_A0, RG(R_D0));
            tc__relocate();
        }
    }
    for (int i = 0; i < 13; i++) SETL(regs[i], saved[i]);  /* movem.l (SP)+ */
    return RG(R_D0);
syserr:
    SETL(R_D0, 15);
    tb_trap(0xA9C9);                                       /* SysError(15): does not return */
    for (int i = 0; i < 13; i++) SETL(regs[i], saved[i]);
    return RG(R_D0);
}
void glue_tc__LoadSeg_reloc_patch(void) { tc__LoadSeg_reloc_patch(); }

/* 000100b4: initialise the A5 globals (CurStackBase..A5) from 'DATA' 0, each zero word
 * followed by a run of zero bytes counted in 'ZERO' 0; then relocate them with 'DREL' 0
 * (falls into tc__relocate with A2 = A5) */
uint32_t tc__init_globals(void) {
    ENTRY_NOLINK(FN_tc__init_globals, 0);
    getres_push(0x5a45524f, 0);                            /* 'ZERO': handle stays on the stack */
    SETL(R_A2, R32(emu_sp()));
    getres_push(0x44415441, 0);                            /* 'DATA' */
    SETL(R_A0, R32(emu_sp()));
    maddr a0 = R32(RG(R_A0));
    maddr a1 = R32(CURSTACKBASE);
    maddr a2 = R32(RG(R_A2));
    uint32_t d1 = RG(R_D1);
    maddr a5 = RG(R_A5);
    while (a1 != a5) {
        uint16_t w = RU16(a0);
        a0 += 2;
        W16(a1, w);
        a1 += 2;
        if (w == 0) {
            d1 = (d1 & 0xffff0000u) | RU16(a2);
            a2 += 2;
            for (;;) {                                     /* dbf D1 */
                d1 = (d1 & 0xffff0000u) | (uint16_t)(d1 - 1);
                if ((uint16_t)d1 == 0xffff) break;
                W8(a1, 0);
                a1++;
            }
        }
    }
    SETL(R_A0, a0); SETL(R_A1, a1); SETL(R_A2, a2); SETL(R_D1, d1);
    tb_trap(0xA9A3);                                       /* ReleaseResource(DATA) */
    tb_trap(0xA9A3);                                       /* ReleaseResource(ZERO) */
    getres_push(0x4452454c, 0);                            /* 'DREL' */
    SETL(R_A0, pop32());
    SETL(R_A2, RG(R_A5));
    relocate_body();
    return RG(R_D0);
}
void glue_tc__init_globals(void) { tc__init_globals(); }

/* 00010100: relocation: A0 = 'DREL'/'CREL' handle (words), A2 = base. Each word w adds A5
 * (or, when bit 0 is set, the string-literal base) to the long at A2 + (w & ~1). */
static void relocate_body(void) {
    tb_trap(0xA025);                                       /* GetHandleSize(A0) -> D0 */
    push32(RG(R_A0));
    maddr a0 = R32(RG(R_A0));
    uint32_t d0 = RG(R_D0), d1 = RG(R_D1), d2 = RG(R_D2);
    d0 = (d0 & 0xffff0000u) | (uint16_t)((int16_t)d0 >> 1);          /* asr.w #1 */
    for (;;) {                                             /* dbf D0 */
        d0 = (d0 & 0xffff0000u) | (uint16_t)(d0 - 1);
        if ((uint16_t)d0 == 0xffff) break;
        d2 = RG(R_A5);
        d1 = (d1 & 0xffff0000u) | RU16(a0);
        a0 += 2;
        int odd = d1 & 1;
        d1 &= ~1u;
        if (odd) d2 = R32(code1(C1_STRS_BASE));
        maddr p = RG(R_A2) + (int16_t)d1;
        W32(p, R32(p) + d2);
    }
    SETL(R_D0, d0); SETL(R_D1, d1); SETL(R_D2, d2); SETL(R_A0, a0);
    tb_trap(0xA9A3);                                       /* ReleaseResource(handle) */
}
uint32_t tc__relocate(void) {
    ENTRY_NOLINK(FN_tc__relocate, 0);
    relocate_body();
    return RG(R_D0);
}
void glue_tc__relocate(void) { tc__relocate(); }

/* 00010124: D3.b = HGetState(A3) (64K ROM: master pointer high byte); A0 = A3 */
uint32_t tc__save_handle_state(void) {
    ENTRY_NOLINK(FN_tc__save_handle_state, 0);
    SETL(R_A0, RG(R_A3));
    if (R16(ROM85) < 0) { SETB(R_D3, R8(RG(R_A3))); return RG(R_D0); }
    tb_trap(0xA069);                                       /* HGetState */
    SETB(R_D3, RG(R_D0));
    return RG(R_D0);
}
void glue_tc__save_handle_state(void) { tc__save_handle_state(); }

/* 00010136: HSetState(A3, D3.b) (64K ROM: *A3 = D3.b) */
uint32_t tc__restore_handle_state(void) {
    ENTRY_NOLINK(FN_tc__restore_handle_state, 0);
    if (R16(ROM85) < 0) { W8(RG(R_A3), RG(R_D3)); return RG(R_D0); }
    SETL(R_A0, RG(R_A3));
    SETB(R_D0, RG(R_D3));
    tb_trap(0xA06A);                                       /* HSetState */
    return RG(R_D0);
}
void glue_tc__restore_handle_state(void) { tc__restore_handle_state(); }

/* ---- long arithmetic ---- */

/* 00010274 core: unsigned D0 / D1 -> D0 quotient, D1 remainder; D2, D3, D7 scratch.
 * Exactly the assembly: 16-bit divu steps, or an estimate from shifted operands that is
 * corrected downwards. */
typedef struct { uint32_t d0, d1, d2, d3, d7; } DivRegs;
static void uldivmod(DivRegs *r) {
    r->d7 = 0xffff;
    if (r->d1 > r->d0) { r->d1 = r->d0; r->d0 = 0; return; }
    if (r->d1 == 0) port_divide_by_zero("tc__uldivmod_regs");      /* divu.w #0 */
    if (r->d0 <= r->d7) {                                  /* both fit in 16 bits */
        uint16_t q = (uint16_t)(r->d0 / (uint16_t)r->d1), m = (uint16_t)(r->d0 % (uint16_t)r->d1);
        r->d1 = (r->d1 & 0xffff0000u) | m;
        r->d0 = q;
        return;
    }
    if (r->d1 <= r->d7) {                                  /* 16-bit divisor: two divu steps */
        uint16_t dv = (uint16_t)r->d1;
        uint32_t a = r->d0;
        uint16_t qh = (uint16_t)((a >> 16) / dv), rh = (uint16_t)((a >> 16) % dv);
        uint32_t lo = (uint32_t)rh << 16 | (a & 0xffff);
        uint16_t ql = (uint16_t)(lo / dv), rl = (uint16_t)(lo % dv);
        r->d0 = (uint32_t)qh << 16 | ql;
        r->d7 = (uint32_t)ql << 16 | rl;
        r->d1 = (r->d1 & 0xffff0000u) | rl;
        return;
    }
    r->d2 = r->d0;                                         /* 32-bit divisor: estimate */
    r->d3 = r->d1;
    do { r->d0 >>= 1; r->d1 >>= 1; } while (r->d1 > r->d7);
    {
        uint16_t dv = (uint16_t)r->d1;
        uint32_t q = r->d0 / dv, m = r->d0 % dv;
        if (q <= 0xffff) r->d0 = (m << 16) | q;            /* divu.w (overflow: D0 unchanged) */
        r->d0 &= r->d7;                                    /* and.l D7,D0 */
    }
    for (;;) {
        r->d1 = (r->d1 & 0xffff0000u) | (uint16_t)r->d3;
        r->d1 = (uint32_t)(uint16_t)r->d0 * (uint16_t)r->d1;           /* mulu.w */
        r->d7 = r->d3;
        r->d7 = r->d7 >> 16 | r->d7 << 16;                 /* swap */
        r->d7 = (uint32_t)(uint16_t)r->d0 * (uint16_t)r->d7;
        r->d7 = r->d7 >> 16 | r->d7 << 16;
        uint32_t s = r->d1 + r->d7;
        int carry = s < r->d1;
        r->d1 = s;
        if (!carry) {
            uint32_t t = r->d1 - r->d2;                    /* sub.l D2,D1; bhi */
            int hi = r->d1 > r->d2;
            r->d1 = t;
            if (!hi) { r->d1 = (uint32_t)-(int32_t)r->d1; return; }
        }
        r->d0 = (r->d0 & 0xffff0000u) | (uint16_t)(r->d0 - 1);
    }
}

/* 00010246 core: signed D0 / D1 -> D0 quotient (toward zero), D1 remainder (sign of D0) */
static void ldivmod(DivRegs *r) {
    if ((int32_t)r->d0 >= 0) {
        if ((int32_t)r->d1 >= 0) { uldivmod(r); return; }
        r->d1 = (uint32_t)-(int32_t)r->d1;
        uldivmod(r);
        r->d0 = (uint32_t)-(int32_t)r->d0;
        return;
    }
    if ((int32_t)r->d1 >= 0) {
        r->d0 = (uint32_t)-(int32_t)r->d0;
        uldivmod(r);
        r->d0 = (uint32_t)-(int32_t)r->d0;
        r->d1 = (uint32_t)-(int32_t)r->d1;
        return;
    }
    r->d0 = (uint32_t)-(int32_t)r->d0;
    r->d1 = (uint32_t)-(int32_t)r->d1;
    uldivmod(r);
    r->d1 = (uint32_t)-(int32_t)r->d1;
}

static DivRegs divregs(uint32_t a, uint32_t b) {
    DivRegs r = { a, b, RG(R_D2), RG(R_D3), RG(R_D7) };
    return r;
}

/* 0001018c: a * b (low 32 bits); callee pops 8; preserves D1-D7 */
uint32_t tc_lmul(uint32_t a, uint32_t b) {
    return a * b;
}

/* 000101c2: unsigned a / b; preserves D1-D7 */
uint32_t tc_uldiv(uint32_t a, uint32_t b) {
    DivRegs r = divregs(a, b);
    uldivmod(&r);
    return r.d0;
}

/* 000101e2: unsigned a % b */
uint32_t tc_ulmod(uint32_t a, uint32_t b) {
    DivRegs r = divregs(a, b);
    uldivmod(&r);
    return r.d1;
}

/* 00010204: signed a / b */
uint32_t tc_ldiv(uint32_t a, uint32_t b) {
    DivRegs r = divregs(a, b);
    ldivmod(&r);
    return r.d0;
}

/* 00010224: signed a % b */
uint32_t tc_lmod(uint32_t a, uint32_t b) {
    DivRegs r = divregs(a, b);
    ldivmod(&r);
    return r.d1;
}

/* 00010246: register signed divmod (D0, D1 in; D0 quotient, D1 remainder; D2/D3/D7 scratch) */
uint32_t tc__ldivmod_regs(void) {
    DivRegs r = divregs(RG(R_D0), RG(R_D1));
    ldivmod(&r);
    SETL(R_D1, r.d1); SETL(R_D2, r.d2); SETL(R_D3, r.d3); SETL(R_D7, r.d7);
    return r.d0;
}
void glue_tc__ldivmod_regs(void) { emu_set_d0(tc__ldivmod_regs()); }

/* 00010274: register unsigned divmod */
uint32_t tc__uldivmod_regs(void) {
    DivRegs r = divregs(RG(R_D0), RG(R_D1));
    uldivmod(&r);
    SETL(R_D1, r.d1); SETL(R_D2, r.d2); SETL(R_D3, r.d3); SETL(R_D7, r.d7);
    return r.d0;
}
void glue_tc__uldivmod_regs(void) { emu_set_d0(tc__uldivmod_regs()); }

/* 000102de: load the string literals: resource type D6 ('STRS') id 0, continued in ids
 * 129.. (0x7ffe bytes each) while a part has attribute bit 5; one part is detached and
 * locked in place, several are copied into one NewPtr block. A4 = A3 = base, D0 = 1
 * (0 on failure). Uses D3/D4/D5/D7. */
uint32_t tc__load_STRS(void) {
    ENTRY_NOLINK(FN_tc__load_STRS, 0);
    SETL(R_D7, 0);
    W8(RESLOAD, 0);
    getres_push(RG(R_D6), (uint16_t)RG(R_D7));
    SETL(R_D0, pop32());
    if (RG(R_D0) == 0) return 0;
    SETL(R_A3, RG(R_D0));
    push32(0);
    push32(RG(R_A3));
    tb_trap(0xA9A5);                                       /* SizeResource */
    SETL(R_D3, pop32());
    SETL(R_D0, RG(R_A3));
    SETL(R_D4, RG(R_D3));
    SETW(R_D7, (uint16_t)((((uint16_t)RG(R_D7) + 1) << 7) + 1));       /* first continuation id 129 */
    SETW(R_D5, RG(R_D7));
    for (;;) {
        push16(0);
        push32(RG(R_D0));
        tb_trap(0xA9A6);                                   /* GetResAttrs */
        SETW(R_D0, pop16());
        if (!(RG(R_D0) & 0x20)) break;
        getres_push(RG(R_D6), (uint16_t)RG(R_D7));
        SETL(R_D0, pop32());
        if (RG(R_D0) == 0) return 0;
        SETW(R_D7, RG(R_D7) + 1);
        SETL(R_D4, RG(R_D4) + 0x7ffe);
    }
    W8(RESLOAD, 0xff);
    { uint32_t t = RG(R_D7); SETL(R_D7, RG(R_D5)); SETL(R_D5, t); }   /* exg D7,D5 */
    SETW(R_D5, (uint16_t)RG(R_D5) - (uint16_t)RG(R_D7));
    if ((uint16_t)RG(R_D5) == 0) {                         /* one part: use it in place */
        SETL(R_D0, RG(R_D3));
        tb_trap(0xA040);                                   /* ReserveMem */
        push32(RG(R_A3));
        tb_trap(0xA9A2);                                   /* LoadResource */
        SETL(R_D0, R32(RG(R_A3)));
        if (RG(R_D0) == 0) return 0;
        push32(RG(R_A3));
        tb_trap(0xA992);                                   /* DetachResource */
        SETL(R_A4, R32(RG(R_A3)));
        SETL(R_A0, RG(R_A3));
        tb_trap(0xA029);                                   /* HLock */
        SETL(R_D0, 1);
        return 1;
    }
    SETL(R_D0, RG(R_D4));
    tb_trap(0xA11E);                                       /* NewPtr(total) */
    SETL(R_D0, RG(R_A0));
    if (RG(R_D0) == 0) return 0;
    SETL(R_A4, RG(R_A0));
    push32(RG(R_A3));
    tb_trap(0xA9A2);                                       /* LoadResource */
    SETL(R_D0, R32(RG(R_A3)));
    if (RG(R_D0) == 0) return 0;
    SETL(R_A0, RG(R_A3));
    SETL(R_D0, RG(R_D3));
    for (;;) {
        push32(RG(R_A0));
        SETL(R_A0, R32(RG(R_A0)));
        SETL(R_A1, RG(R_A4));
        SETL(R_A4, RG(R_A4) + RG(R_D0));
        tb_trap(0xA02E);                                   /* BlockMove(part, A4, D0) */
        tb_trap(0xA9A3);                                   /* ReleaseResource(part) */
        SETW(R_D5, RG(R_D5) - 1);                          /* dbf D5 */
        if ((uint16_t)RG(R_D5) == 0xffff) break;
        getres_push(RG(R_D6), (uint16_t)RG(R_D7));
        SETL(R_D0, pop32());
        if (RG(R_D0) == 0) return 0;
        SETW(R_D7, RG(R_D7) + 1);
        SETL(R_A0, RG(R_D0));
        SETL(R_D0, 0x7ffe);
    }
    SETL(R_A4, RG(R_A4) - RG(R_D4));
    SETL(R_A3, RG(R_A4));
    SETL(R_D0, 1);
    return 1;
}
void glue_tc__load_STRS(void) { emu_set_d0(tc__load_STRS()); }

/* 000103ae: patch _LoadSeg with {jsr tc__LoadSeg_reloc_patch; jmp old} and _ExitToShell
 * with {jsr exit path of tc__start; jmp old} in a 0x18-byte block kept at 0x68(A5) */
uint32_t tc__install_trap_patches(void) {
    ENTRY_NOLINK(FN_tc__install_trap_patches, 0);
    SETL(R_D0, 0x18);
    tb_trap(R16(ROM85) < 0 ? 0xA51E : 0xA11E);             /* NewPtrSys / NewPtr */
    SETL(R_A1, RG(R_A0));
    W32(RG(R_A5) + 0x68, RG(R_A1));
    static const uint16_t traps[2] = { 0xa9f0, 0xa9f4 };
    for (int i = 0; i < 2; i++) {
        SETW(R_D0, traps[i]);
        tb_trap(0xA146);                                   /* GetTrapAddress */
        SETL(R_D1, RG(R_A0));
        SETW(R_D0, traps[i]);
        SETL(R_A0, RG(R_A1));
        tb_trap(0xA047);                                   /* SetTrapAddress(block) */
        maddr a1 = RG(R_A1);
        W16(a1, 0x4eb9);                                   /* jsr abs.l */
        SETL(R_A0, i == 0 ? fn_address(FN_tc__LoadSeg_reloc_patch) : code1(0x4c));
        W32(a1 + 2, RG(R_A0));
        W16(a1 + 6, 0x4ef9);                               /* jmp abs.l */
        W32(a1 + 8, RG(R_D1));
        SETL(R_A1, a1 + 12);
    }
    return RG(R_D0);
}
void glue_tc__install_trap_patches(void) { tc__install_trap_patches(); }

/* 00010404: default exit hook: restore _LoadSeg/_ExitToShell, free the patch block */
uint32_t tc__remove_trap_patches(void) {
    ENTRY_NOLINK(FN_tc__remove_trap_patches, 0);
    SETL(R_A1, R32(RG(R_A5) + 0x68));
    SETW(R_D0, 0xa9f0);
    SETL(R_A0, R32(RG(R_A1) + 8));
    tb_trap(0xA047);                                       /* SetTrapAddress */
    SETW(R_D0, 0xa9f4);
    SETL(R_A0, R32(RG(R_A1) + 0x14));
    tb_trap(0xA047);
    SETL(R_A0, RG(R_A1));
    tb_trap(0xA01F);                                       /* DisposePtr */
    return RG(R_D0);
}
void glue_tc__remove_trap_patches(void) { tc__remove_trap_patches(); }
