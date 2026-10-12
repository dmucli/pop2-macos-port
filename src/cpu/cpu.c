#include "cpu.h"
#include "trapinfo.h"
#include "m68k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile int cpu_running = 1;
uint64_t cpu_instr_count;
maddr trap_ret;
uint16_t cur_trap_word;

/* ---------------------------------------------------- PC history (debug) -- */
#define PCH 4096
static uint32_t br_from[PCH], br_to[PCH]; static unsigned br_pos; static uint32_t last_pc;
static int brk_seg = -1; static uint32_t brk_off; static int brk_count;
static void brk_check(unsigned pc) {
    extern int loader_locate(maddr a, int *seg, uint32_t *off);
    int sg; uint32_t of;
    if (!loader_locate(pc, &sg, &of) || sg != brk_seg || of != brk_off) return;
    { extern uint64_t runtime_us(void); static long after = -1; if (after < 0) after = getenv("POP2_BREAK_AFTER_MS") ? atol(getenv("POP2_BREAK_AFTER_MS")) : 0;
      if ((long)(runtime_us() / 1000) < after) return; }
    if (brk_count++ > 40) return;
    uint32_t sp = cpu_get(R_SP);
    fprintf(stderr, "[break] seg%d+%04x SP=%06x A6=%06x D0=%08x A0=%08x args:", sg, of, sp, cpu_get(R_A6), cpu_get(R_D0), cpu_get(R_A0));
    for (int i = 0; i < 12; i++) fprintf(stderr, " %04x", rd16(sp + 4 + 2 * i));
    const char *dump = getenv("POP2_BREAK_DUMP");      /* "A5off:len" */
    if (dump) {
        long off = strtol(dump, NULL, 0); const char *c = strchr(dump, ':'); int len = c ? atoi(c + 1) : 16;
        maddr a = cpu_get(R_A5) + (int32_t)off;
        fprintf(stderr, "\n        A5%+ld:", off);
        for (int i = 0; i < len; i++) fprintf(stderr, " %02x", rd8(a + i));
        fprintf(stderr, " (ticks=%u)", rd32(0x16A));
    }
    {   /* return address and A6 frame chain */
        int s2; uint32_t o2; maddr ra = rd32(sp), a6 = cpu_get(R_A6);
        fprintf(stderr, "\n        ret:");
        if (loader_locate(ra, &s2, &o2)) fprintf(stderr, " seg%d+%04x", s2, o2); else fprintf(stderr, " %06x", ra);
        for (int i = 0; i < 10 && a6 && a6 < 0x1000000; i++) {
            ra = rd32(a6 + 4);
            if (loader_locate(ra, &s2, &o2)) fprintf(stderr, " <- seg%d+%04x", s2, o2); else fprintf(stderr, " <- %06x", ra);
            maddr n6 = rd32(a6); if (n6 <= a6) break; a6 = n6;
        }
    }
    maddr p1 = rd32(sp + 4);
    fprintf(stderr, "\n        *arg0:");
    for (int i = 0; i < 16; i++) fprintf(stderr, " %04x", rd16(p1 + 2 * i));
    fprintf(stderr, "\n");
}
void cpu_instr_hook(unsigned int pc) {
    if (brk_seg >= 0) brk_check(pc);
    if (pc - last_pc > 10) { br_from[br_pos & (PCH - 1)] = last_pc; br_to[br_pos & (PCH - 1)] = pc; br_pos++; }
    last_pc = pc;
}
void cpu_dump_history(int n) {
#ifdef POP2_PC_HISTORY
    extern int loader_locate(maddr a, int *seg, uint32_t *off);
    if (n > PCH) n = PCH;
    fprintf(stderr, "--- last %d control transfers (from -> to) ---\n", n);
    for (int i = n; i > 0; i--) {
        unsigned k = (br_pos - i) & (PCH - 1);
        char a[32] = "", b[32] = ""; int sg; uint32_t of;
        if (loader_locate(br_from[k], &sg, &of)) snprintf(a, sizeof a, "seg%d+%04x", sg, of);
        if (loader_locate(br_to[k], &sg, &of)) snprintf(b, sizeof b, "seg%d+%04x", sg, of);
        fprintf(stderr, "  %06x %-12s -> %06x %s\n", br_from[k], a, br_to[k], b);
    }
#else
    (void)n;
#endif
}

/* ------------------------------------------------------------ memory -- */
unsigned int m68k_read_memory_8(unsigned int a)  { return rd8(a); }
void runtime_ticks_peek(void);
#define TICKS_PEEK(a) do { if (((a) & ADDR_MASK) - 0x168u < 6u) runtime_ticks_peek(); } while (0)   /* reads of Ticks ($16A) */
unsigned int m68k_read_memory_16(unsigned int a) { TICKS_PEEK(a); return rd16(a); }
unsigned int m68k_read_memory_32(unsigned int a) { TICKS_PEEK(a); return rd32(a); }
static uint32_t watch_lo = 1, watch_hi = 0;
static void watch_hit(unsigned a, unsigned v, int size) {
    extern int loader_locate(maddr a, int *seg, uint32_t *off);
    unsigned pc = m68k_get_reg(NULL, M68K_REG_PPC);
    int sg; uint32_t of; char loc[32] = "";
    if (loader_locate(pc, &sg, &of)) snprintf(loc, sizeof loc, "seg%d+%04x", sg, of);
    fprintf(stderr, "[watch] write%d %06x = %x at pc %06x %s\n", size, a, v, pc, loc);
}
#define WATCH(a, n, v, sz) do { if ((a) + (n) > watch_lo && (a) <= watch_hi) watch_hit(a, v, sz); } while (0)
void m68k_write_memory_8(unsigned int a, unsigned int v)  { WATCH(a, 1, v, 8); wr8(a, (uint8_t)v); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { WATCH(a, 2, v, 16); wr16(a, (uint16_t)v); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { WATCH(a, 4, v, 32); wr32(a, v); }
void cpu_watch_from_env(void) {
    const char *w = getenv("POP2_WATCH");
    if (w) { watch_lo = (uint32_t)strtoul(w, NULL, 16); watch_hi = watch_lo + 3; }
    const char *b = getenv("POP2_BREAK");
    if (b) { brk_seg = atoi(b); const char *c = strchr(b, ':'); brk_off = c ? (uint32_t)strtoul(c + 1, NULL, 16) : 0; }
}
unsigned int m68k_read_disassembler_8(unsigned int a)  { return rd8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return rd16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return rd32(a); }

/* ---------------------------------------------------------- registers -- */
static const m68k_register_t regmap[] = {
    M68K_REG_D0, M68K_REG_D1, M68K_REG_D2, M68K_REG_D3, M68K_REG_D4, M68K_REG_D5, M68K_REG_D6, M68K_REG_D7,
    M68K_REG_A0, M68K_REG_A1, M68K_REG_A2, M68K_REG_A3, M68K_REG_A4, M68K_REG_A5, M68K_REG_A6, M68K_REG_A7,
    M68K_REG_PC, M68K_REG_SR };
uint32_t cpu_get(int r) { return m68k_get_reg(NULL, regmap[r]); }
void cpu_set(int r, uint32_t v) { m68k_set_reg(regmap[r], v); }

uint32_t pop32(void) { uint32_t sp = cpu_get(R_SP); uint32_t v = rd32(sp); cpu_set(R_SP, sp + 4); return v; }
uint16_t pop16(void) { uint32_t sp = cpu_get(R_SP); uint16_t v = rd16(sp); cpu_set(R_SP, sp + 2); return v; }
uint8_t  pop8(void)  { return (uint8_t)(pop16() >> 8); }
void push32(uint32_t v) { uint32_t sp = cpu_get(R_SP) - 4; wr32(sp, v); cpu_set(R_SP, sp); }
void push16(uint16_t v) { uint32_t sp = cpu_get(R_SP) - 2; wr16(sp, v); cpu_set(R_SP, sp); }
void ret16(uint16_t v) { wr16(cpu_get(R_SP), v); }
void ret32(uint32_t v) { wr32(cpu_get(R_SP), v); }
void retbool(int v) { wr16(cpu_get(R_SP), v ? 0x0100 : 0); }

/* ------------------------------------------------------------ thunks -- */
/* Thunk i lives at THUNK_BASE + 2*i and holds the word 0xF000 | i. Thunk 0 is
 * the "return to native caller" marker used by cpu_call. */
static NativeFn thunk_fn[THUNK_COUNT];
static const char *thunk_name[THUNK_COUNT];
static uint8_t thunk_is_hook[THUNK_COUNT];
static uint64_t thunk_calls[THUNK_COUNT];
/* Optional wrapper around ported-function hooks (differential checking). */
void (*cpu_hook_runner)(NativeFn fn, const char *name, maddr entry);
void cpu_print_native_stats(void) {
    for (unsigned i = 0; i < THUNK_COUNT; i++)
        if (thunk_is_hook[i]) fprintf(stderr, "[native] %-36s %llu calls\n", thunk_name[i], (unsigned long long)thunk_calls[i]);
}   /* may appear anywhere (patched function entries) */
static unsigned thunk_next = 1;

maddr cpu_thunk(NativeFn fn, const char *name) {
    for (unsigned i = 1; i < thunk_next; i++) if (thunk_fn[i] == fn) return THUNK_BASE + 2 * i;
    if (thunk_next >= THUNK_COUNT) { fprintf(stderr, "out of thunks\n"); abort(); }
    unsigned i = thunk_next++;
    thunk_fn[i] = fn; thunk_name[i] = name;
    wr16(THUNK_BASE + 2 * i, (uint16_t)(0xF000 | i));
    return THUNK_BASE + 2 * i;
}

/* A native replacement for an original function: its entry word is patched with
 * the returned F-line opcode. */
static unsigned hook_next = THUNK_COUNT - 1;   /* hooks grow down so trap thunk addresses never shift */
uint16_t cpu_register_hook(NativeFn fn, const char *name) {
    if (hook_next <= thunk_next) { fprintf(stderr, "out of thunks\n"); abort(); }
    unsigned i = hook_next--;
    thunk_fn[i] = fn; thunk_name[i] = name; thunk_is_hook[i] = 1;
    wr16(THUNK_BASE + 2 * i, (uint16_t)(0xF000 | i));
    return (uint16_t)(0xF000 | i);
}

static int call_depth;
static int call_returned[64];

static int linef_hook(unsigned int op) {
    unsigned i = op & 0x0FFF;
    uint32_t pc = cpu_get(R_PC) - 2;
    if (pc != THUNK_BASE + 2 * i && !thunk_is_hook[i]) {
        fprintf(stderr, "unexpected line-F %04x at %06x\n", op, pc);
        return 0;
    }
    if (i == 0) {                     /* nested call returned */
        call_returned[call_depth] = 1;
        m68k_end_timeslice();
        return 1;
    }
    trap_ret = pop32();
    cur_trap_word = 0;
    if (thunk_is_hook[i]) {
        thunk_calls[i]++;
        if (cpu_hook_runner) { cpu_hook_runner(thunk_fn[i], thunk_name[i], pc); cpu_set(R_PC, trap_ret); return 1; }
    }
    thunk_fn[i]();
    cpu_set(R_PC, trap_ret);
    return 1;
}

/* --------------------------------------------------------- nested call -- */
uint32_t cpu_call_regs(maddr pc, const uint32_t *regs16, uint32_t *a0) {
    static unsigned char ctx[64][4096];
    if (call_depth >= 63) { fprintf(stderr, "cpu_call: too deep\n"); abort(); }
    unsigned sz = m68k_context_size();
    if (sz > sizeof ctx[0]) abort();
    int d = ++call_depth;
    m68k_get_context(ctx[d]);
    maddr saved_ret = trap_ret;
    uint16_t saved_word = cur_trap_word;
    if (regs16) for (int r = 0; r < 15; r++) cpu_set(r, regs16[r]);   /* D0-D7, A0-A6 */
    push32(THUNK_BASE);               /* return lands on thunk 0 */
    cpu_set(R_PC, pc);
    call_returned[d] = 0;
    while (!call_returned[d] && cpu_running) {
        cpu_instr_count += m68k_execute(20000);
    }
    uint32_t rd0 = cpu_get(R_D0), ra0 = cpu_get(R_A0);
    m68k_set_context(ctx[d]);
    call_depth--;
    trap_ret = saved_ret;
    cur_trap_word = saved_word;
    m68k_end_timeslice();
    if (a0) *a0 = ra0;
    return rd0;
}
/* What interrupt-level code sees of the interrupted program (registers, stack contents
 * below SP) is undefined on a real Mac. Making it fixed keeps runs independent of
 * whether the interrupted code is original 68k or ported C, which keeps the
 * differential checker exact and deterministic runs reproducible. */
static int irq_depth;
uint32_t cpu_call_irq(maddr pc, uint32_t d0, maddr a0, maddr a1, int nargs, const uint32_t *args) {
    uint32_t regs[15] = {0};
    regs[R_D0] = d0; regs[R_A0] = a0; regs[R_A1] = a1;
    regs[R_A5] = rd32(0x904);                       /* CurrentA5 */
    maddr sp0 = cpu_get(R_SP);
    if (irq_depth++ == 0) {
        mset(IRQ_STACK_TOP - IRQ_STACK_SIZE, 0, IRQ_STACK_SIZE);
        cpu_set(R_SP, IRQ_STACK_TOP);
    }
    for (int i = 0; i < nargs; i++) push32(args[i]);
    uint32_t r = cpu_call_regs(pc, regs, NULL);
    irq_depth--;
    cpu_set(R_SP, sp0);
    return r;
}
uint32_t cpu_call(maddr pc, uint32_t *a0) {
    uint32_t regs[15];
    for (int r = 0; r < 15; r++) regs[r] = cpu_get(r);
    return cpu_call_regs(pc, regs, a0);
}

/* --------------------------------------------------------------- traps -- */
static TrapFn tb_native[0x400], os_native[0x100];
static maddr tb_patch[0x400], os_patch[0x100];
static maddr tb_thunk[0x400], os_thunk[0x100];

const TrapInfo *trap_lookup(uint16_t w) {
    int tool = (w & 0x0800) != 0;
    for (const TrapInfo *t = trap_info; t->name; t++) {
        if (tool) { if ((t->word & 0x0800) && (t->word & 0x3FF) == (w & 0x3FF)) return t; }
        else if (!(t->word & 0x0800) && (t->word & 0xFF) == (w & 0xFF)) return t;
    }
    return NULL;
}

void trap_unimplemented(uint16_t w) {
    static uint8_t warned[0x1000];
    const TrapInfo *t = trap_lookup(w);
    if (!warned[w & 0xFFF]) {
        warned[w & 0xFFF] = 1;
        fprintf(stderr, "[trap] unimplemented %04X %s (pc=%06x)\n", w, t ? t->name : "?", cpu_get(R_PC) - 2);
    }
    if ((w & 0x0800) && t && t->pop > 0 && !t->regbased) {
        cpu_set(R_SP, cpu_get(R_SP) + t->pop);
        if (t->retsize == 2) ret16(0); else if (t->retsize == 4) ret32(0);
    } else if (!(w & 0x0800)) {
        cpu_set(R_D0, (uint32_t)-1);     /* generic OS error */
    }
}

void trap_register(uint16_t w, TrapFn fn) {
    if (w & 0x0800) tb_native[w & 0x3FF] = fn; else os_native[w & 0xFF] = fn;
}

/* Thunk entry for a trap address obtained with GetTrapAddress: behaves like
 * the trap itself when JSR'd / JMP'd to (return address already on stack). */
static uint16_t thunk_trap_word[THUNK_COUNT];
static void run_native_trap(uint16_t w);
static void trap_thunk_entry(void);
static maddr trap_thunk(uint16_t w) {
    maddr *slot = (w & 0x0800) ? &tb_thunk[w & 0x3FF] : &os_thunk[w & 0xFF];
    if (!*slot) {
        /* each trap gets a distinct thunk so we know which trap it is */
        if (thunk_next >= hook_next) abort();
        unsigned i = thunk_next++;
        thunk_fn[i] = trap_thunk_entry; thunk_name[i] = "trap";
        thunk_trap_word[i] = w;
        wr16(THUNK_BASE + 2 * i, (uint16_t)(0xF000 | i));
        *slot = THUNK_BASE + 2 * i;
    }
    return *slot;
}
static void trap_thunk_entry(void) {
    unsigned i = (cpu_get(R_PC) - 2 - THUNK_BASE) / 2;   /* PC still at thunk+2 */
    run_native_trap(thunk_trap_word[i]);
}

maddr trap_get_address(uint16_t w) {
    maddr p = (w & 0x0800) ? tb_patch[w & 0x3FF] : os_patch[w & 0xFF];
    return p ? p : trap_thunk(w);
}
void trap_set_address(uint16_t w, maddr addr) {
    maddr *p = (w & 0x0800) ? &tb_patch[w & 0x3FF] : &os_patch[w & 0xFF];
    *p = (addr == trap_thunk(w)) ? 0 : addr;
}

int cpu_trace = -1;
static void run_native_trap(uint16_t w) {
    TrapFn fn = (w & 0x0800) ? tb_native[w & 0x3FF] : os_native[w & 0xFF];
    cur_trap_word = w;
    if (cpu_trace < 0) cpu_trace = getenv("POP2_TRACE") ? atoi(getenv("POP2_TRACE")) : 0;
    if (cpu_trace) {
        const TrapInfo *t = trap_lookup(w);
        uint16_t ow = w;
        if (!(cpu_trace & 2) && (ow == 0xA975 || ow == 0xA029 || ow == 0xA02A || ow == 0xA02E)) { /* noisy */ }
        else {
            extern int loader_locate(maddr a, int *seg, uint32_t *off);
            int sg; uint32_t of; char loc[32] = "";
            if (loader_locate(trap_ret, &sg, &of)) snprintf(loc, sizeof loc, " seg%d+%04x", sg, of);
            fprintf(stderr, "[T] %06x%s %04X %s A0=%06x D0=%08x SP=%06x\n", trap_ret, loc, w, t ? t->name : "?", cpu_get(R_A0), cpu_get(R_D0), cpu_get(R_SP));
        }
    }
    static FILE *tlog; static int tlog_init; static uint64_t tlog_n;
    if (!tlog_init) { tlog_init = 1; if (getenv("POP2_TRAPLOG")) tlog = fopen(getenv("POP2_TRAPLOG"), "w"); }
    if (tlog) {
        extern int loader_locate(maddr a, int *seg, uint32_t *off);
        int sg = 0; uint32_t of = trap_ret; loader_locate(trap_ret, &sg, &of);
        fprintf(tlog, "%llu %04X %d+%04x D0=%08x A0=%06x A1=%06x SP=%06x\n", (unsigned long long)tlog_n++, w, sg, of, cpu_get(R_D0), cpu_get(R_A0), cpu_get(R_A1), cpu_get(R_SP));
        if ((tlog_n & 1023) == 0) fflush(tlog);
    }
    if (fn) fn(w); else trap_unimplemented(w);
    { extern void runtime_trap_hook(void); runtime_trap_hook(); }
    if (cpu_trace && !(w & 0x0800)) fprintf(stderr, "    -> D0=%08x A0=%06x\n", cpu_get(R_D0), cpu_get(R_A0));
}

/* Invoke a trap from C (ported code): same dispatch as a line-A instruction. */
void trap_invoke(uint16_t w) {
    maddr save_ret = trap_ret;
    maddr patch = (w & 0x0800) ? tb_patch[w & 0x3FF] : os_patch[w & 0xFF];
    if (patch) {
        if (w & 0x0800) cpu_call(patch, NULL);
        else { uint32_t regs[15]; for (int r = 0; r < 15; r++) regs[r] = cpu_get(r); regs[R_D1] = w;
               uint32_t a0, d0 = cpu_call_regs(patch, regs, &a0); cpu_set(R_D0, d0); if (!(w & 0x0100)) cpu_set(R_A0, a0); }
    } else {
        trap_ret = 0;
        run_native_trap(w);
    }
    trap_ret = save_ret;
}

static int linea_hook(unsigned int op) {
    uint16_t w = (uint16_t)op;
    trap_ret = cpu_get(R_PC);
    if (w & 0x0800) {
        maddr patch = tb_patch[w & 0x3FF];
        if (patch) {                     /* Toolbox dispatcher: push return, jump */
            push32(trap_ret);
            cpu_set(R_PC, patch);
            return 1;
        }
        if (w & 0x0400) {                /* auto-pop: discard the return address of a JSR to the trap */
            trap_ret = pop32();
        }
        run_native_trap(w);
        cpu_set(R_PC, trap_ret);
        return 1;
    }
    maddr patch = os_patch[w & 0xFF];
    if (patch) {                         /* OS dispatcher: D1 = trap word, preserve A1/D1/D2 */
        uint32_t regs[15]; for (int r = 0; r < 15; r++) regs[r] = cpu_get(r);
        regs[R_D1] = w;
        uint32_t ra0, rd0 = cpu_call_regs(patch, regs, &ra0);
        /* cpu_call restored the caller's registers (so A1/D1/D2 are preserved); pass results */
        cpu_set(R_D0, rd0);
        if (!(w & 0x0100)) cpu_set(R_A0, ra0);
        cpu_set(R_PC, trap_ret);
        return 1;
    }
    run_native_trap(w);
    /* OS traps set the condition codes from D0.W (TST.W D0) */
    uint32_t sr = cpu_get(R_SR) & ~0x0F;
    int16_t d0 = (int16_t)cpu_get(R_D0);
    if (d0 == 0) sr |= 0x04;
    if (d0 < 0) sr |= 0x08;
    cpu_set(R_SR, sr);
    cpu_set(R_PC, trap_ret);
    return 1;
}

/* ---------------------------------------------------------------- loop -- */
static void (*idle_hook)(void);
void cpu_set_idle_hook(void (*fn)(void)) { idle_hook = fn; }
void cpu_stop(void) { cpu_running = 0; m68k_end_timeslice(); }

void cpu_init(void) {
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68020);
    m68k_linea_hook = linea_hook;
    m68k_linef_hook = linef_hook;
    m68k_pulse_reset();
    wr16(THUNK_BASE, 0xF000);
    cpu_watch_from_env();           /* thunk 0 */
}

void cpu_run_forever(void) {
    while (cpu_running) {
        cpu_instr_count += m68k_execute(20000);
        if (idle_hook) idle_hook();
    }
}

/* ------------------------------------------------- dispatched traps -- */
const DispatchInfo *dispatch_lookup(uint16_t trap, uint32_t sel) {
    trap = (trap & 0x0800) ? (uint16_t)(trap & ~0x0400) : trap;
    for (const DispatchInfo *d = dispatch_info; d->name; d++) {
        if (d->trap != trap) continue;
        if (!strcmp(d->loc, "D0<0xFF>")) { if ((d->selector & 0xFF) == (sel & 0xFF)) return d; }
        else if (!strcmp(d->loc, "D0W") || !strcmp(d->loc, "StackW")) { if ((d->selector & 0xFFFF) == (sel & 0xFFFF)) return d; }
        else if (d->selector == sel) return d;
    }
    return NULL;
}
int dispatch_default(uint16_t trap, uint32_t sel) {
    const DispatchInfo *d = dispatch_lookup(trap, sel);
    static uint32_t warned[256]; static int nw;
    int seen = 0;
    for (int i = 0; i < nw; i++) if (warned[i] == ((uint32_t)trap << 16 ^ sel)) seen = 1;
    if (!seen && nw < 256) {
        warned[nw++] = (uint32_t)trap << 16 ^ sel;
        fprintf(stderr, "[trap] unimplemented %04X selector %08x %s (pc=%06x)\n", trap, sel, d ? d->name : "?", trap_ret);
    }
    if (!d) return 0;
    int pop = d->pop;
    if (!strcmp(d->loc, "StackW")) pop -= 2;
    if (!strcmp(d->loc, "StackL")) pop -= 4;
    cpu_set(R_SP, cpu_get(R_SP) + pop);
    if (d->retsize == 2) ret16(0); else if (d->retsize == 4) ret32(0);
    return 1;
}
