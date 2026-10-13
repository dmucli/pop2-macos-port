/* Minimal 68k "core" for the release build (make CORE=mini): no emulator.
 *
 * Every function of the game is ported to C, so in a normal run the 68k side only ever
 * executes dispatch: the line-F words of native hooks and thunks, line-A traps, the jump
 * table's "jmp abs.l", returns, and the small system-heap stub the sound library builds for
 * its VBL task. This file implements exactly that subset behind the Musashi interface used
 * by cpu.c, and stops with a clear message on anything else. (The differential checker
 * and POP2_NATIVE=none need the real emulator: build without CORE=mini for those.) */
#include "m68k.h"
#include "../mac/mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int (*m68k_linea_hook)(unsigned int opcode) = 0;
int (*m68k_linef_hook)(unsigned int opcode) = 0;

typedef struct { uint32_t d[8], a[8], pc, sr, ppc, ir; } Ctx;
static Ctx C;
static int stop_slice;

void cpu_instr_hook(unsigned int pc);

void m68k_init(void) { memset(&C, 0, sizeof C); C.sr = 0x2700; }
void m68k_set_cpu_type(unsigned int t) { (void)t; }
void m68k_pulse_reset(void) { C.sr = 0x2700; C.a[7] = rd32(0); C.pc = rd32(4); }
void m68k_end_timeslice(void) { stop_slice = 1; }
unsigned int m68k_context_size(void) { return sizeof(Ctx); }
unsigned int m68k_get_context(void *dst) { if (dst) memcpy(dst, &C, sizeof C); return sizeof C; }
void m68k_set_context(void *src) { if (src) memcpy(&C, src, sizeof C); }

unsigned int m68k_get_reg(void *context, m68k_register_t r) {
    const Ctx *c = context ? (const Ctx *)context : &C;
    if (r <= M68K_REG_D7) return c->d[r - M68K_REG_D0];
    if (r <= M68K_REG_A7) return c->a[r - M68K_REG_A0];
    switch (r) {
        case M68K_REG_PC: return c->pc;
        case M68K_REG_SR: return c->sr;
        case M68K_REG_SP: return c->a[7];
        case M68K_REG_PPC: return c->ppc;
        case M68K_REG_IR: return c->ir;
        default: return 0;
    }
}
void m68k_set_reg(m68k_register_t r, unsigned int v) {
    if (r <= M68K_REG_D7) { C.d[r - M68K_REG_D0] = v; return; }
    if (r <= M68K_REG_A7) { C.a[r - M68K_REG_A0] = v; return; }
    switch (r) {
        case M68K_REG_PC: C.pc = v; break;
        case M68K_REG_SR: C.sr = v & 0xFFFF; break;
        case M68K_REG_SP: C.a[7] = v; break;
        default: break;
    }
}
unsigned int m68k_disassemble(char *buf, unsigned int pc, unsigned int t) {
    (void)t; sprintf(buf, "dc.w $%04x", rd16(pc)); return 2;
}

static void push32(uint32_t v) { C.a[7] -= 4; wr32(C.a[7], v); }
static uint32_t pop32(void) { uint32_t v = rd32(C.a[7]); C.a[7] += 4; return v; }

static void unsupported(uint32_t pc, uint16_t w) {
    fprintf(stderr, "*** release core: 68k instruction %04x at %06x is not supported (a function that is "
                    "not ported was reached; run the emulator build to investigate)\n", w, pc);
    fflush(stderr);
    abort();
}

int m68k_execute(int cycles) {
    int done = 0;
    stop_slice = 0;
    while (done < cycles && !stop_slice) {
        uint32_t pc = C.pc & ADDR_MASK;
        uint16_t w = rd16(pc);
#ifdef POP2_PC_HISTORY
        cpu_instr_hook(pc);
#endif
        C.ppc = pc; C.ir = w;
        done += 4;
        if ((w & 0xF000) == 0xF000) {                      /* native hooks and thunks */
            C.pc = pc + 2;
            if (!m68k_linef_hook || !m68k_linef_hook(w)) unsupported(pc, w);
        } else if ((w & 0xF000) == 0xA000) {               /* Toolbox / OS trap */
            C.pc = pc + 2;
            if (!m68k_linea_hook || !m68k_linea_hook(w)) unsupported(pc, w);
        } else if (w == 0x4EF9) {                          /* jmp abs.l (jump table) */
            C.pc = rd32(pc + 2);
        } else if (w == 0x3F3C) {                          /* move.w #imm,-(sp) (unloaded jump-table entry) */
            C.a[7] -= 2; wr16(C.a[7], rd16(pc + 2)); C.pc = pc + 4;
        } else if (w == 0x4EB9) {                          /* jsr abs.l (THINK C patch stubs) */
            push32(pc + 6); C.pc = rd32(pc + 2);
        } else if (w == 0x4E75) {                          /* rts */
            C.pc = pop32();
        } else if (w == 0x6104) {                          /* bsr.s *+6 (VBL stub) */
            push32(pc + 2); C.pc = pc + 6;
        } else if (w == 0x225F) {                          /* movea.l (sp)+,a1 */
            C.a[1] = pop32(); C.pc = pc + 2;
        } else if (w == 0x2251) {                          /* movea.l (a1),a1 */
            C.a[1] = rd32(C.a[1]); C.pc = pc + 2;
        } else if (w == 0x4ED1) {                          /* jmp (a1) */
            C.pc = C.a[1];
        } else if (w == 0x4ED0) {                          /* jmp (a0) */
            C.pc = C.a[0];
        } else unsupported(pc, w);
    }
    return done;
}
