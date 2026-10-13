/* Runtime side of the port: install ported functions into loaded segments and
 * route calls between C and not-yet-ported 68k code.
 *
 * POP2_NATIVE=none          run everything on the 68k core (reference behaviour)
 * POP2_NATIVE_EXCLUDE=A,B   keep these functions on the 68k core
 * POP2_NATIVE_ONLY=A,B      only these functions run natively
 */
#include "port.h"
#include "gen/proto.h"
#include <stdio.h>
#include <stdlib.h>

maddr g_a5;
maddr loader_segment_handle(int16_t seg);
void  LoadSeg(int16_t seg);
extern void (*loader_segment_loaded)(int16_t seg, maddr code);
void trap_invoke(uint16_t w);
uint16_t cpu_register_hook(NativeFn fn, const char *name);

static uint16_t hook_word[FN_COUNT];
static int native_enabled[FN_COUNT];

/* ---- differential checker (POP2_CHECK=N: check the first N calls of each ported function) ----
 * fork(): the child runs the original 68k code from the identical state, the parent runs
 * the port; then the full memory images and D0 are compared. */
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
extern void (*cpu_hook_runner)(NativeFn fn, const char *name, maddr entry);
static uint8_t *shared_mem;            /* child's final memory */
static uint32_t *shared_regs;          /* child's D0, SP */
static int check_budget, checking;
static uint16_t orig_word[FN_COUNT];
static int check_count[FN_COUNT];
static int check_fail[FN_COUNT];

static int fn_index_of(const char *name) {
    for (int i = 0; i < FN_COUNT; i++) if (fn_info[i].name == name || !strcmp(fn_info[i].name, name)) return i;
    return -1;
}

/* Bytes the original leaves uninitialised and that only ever hold stack garbage: the
 * value differs between the original (its own frames left it) and the port, without
 * any effect on behaviour. Each entry: A5 offset of a pointer to a table, record
 * stride, byte within the record, record count.
 *   MIDI note queues (seg11 QNoteOff/QNoteOn): 100 records of 16 bytes; byte 15 is
 *   padding of the caller's stack record (ProcessMidiNoteOff/ProcessMidiEvents). */
static const struct { int32_t a5off; uint32_t stride, byte, count; } check_ignore[] = {
    { -0x11e0, 16, 15, 100 },
    { -0x11e4, 16, 15, 100 },
};
static int check_ignored(maddr a) {
    /* contents of a heap block that is free in both images (same header in both) */
    { extern maddr mm_free_block_header(maddr a);
      maddr b = mm_free_block_header(a);
      if (b && !memcmp(M + b, shared_mem + b, 16)) return 1; }
    /* the interrupt-level stack: dead scratch space, zeroed before each use */
    if (a >= IRQ_STACK_TOP - IRQ_STACK_SIZE && a < IRQ_STACK_TOP) return 1;
    for (size_t i = 0; i < sizeof check_ignore / sizeof check_ignore[0]; i++) {
        maddr t = rd32(g_a5 + check_ignore[i].a5off) & ADDR_MASK;
        if (!t || a < t || a >= t + check_ignore[i].stride * check_ignore[i].count) continue;
        if ((a - t) % check_ignore[i].stride == check_ignore[i].byte) return 1;
    }
    return 0;
}
static maddr entry_g;
extern uint64_t runtime_trap_count;
uint64_t runtime_us(void);
static void report_diff(int fi, maddr sp_after, uint32_t d0_c, uint32_t d0_68k) {
    int shown = 0;
    uint32_t rmask = fn_info[fi].ret_bytes >= 4 ? 0xFFFFFFFFu : fn_info[fi].ret_bytes == 2 ? 0xFFFFu : fn_info[fi].ret_bytes == 1 ? 0xFFu : 0;
    if ((d0_c ^ d0_68k) & rmask) { fprintf(stderr, "[check] %s: D0 port=%08x original=%08x\n", fn_info[fi].name, d0_c, d0_68k); shown++; }
    for (uint32_t a = 0; a < MEM_SIZE && shown < 12; a++) {
        if (a >= THUNK_BASE && a < THUNK_BASE + 2 * THUNK_COUNT) continue;
        if (a < sp_after && a >= sp_after - 0x10000) continue;          /* dead stack below SP */
        if (M[a] != shared_mem[a] && a != entry_g && a != entry_g + 1 && !check_ignored(a)) {
            { extern void mm_describe(maddr a, char *out, size_t n); char d[96]; mm_describe(a, d, sizeof d);
              fprintf(stderr, "[check] %s: mem %06x port=%02x original=%02x  %s\n", fn_info[fi].name, a, M[a], shared_mem[a], d); }
            shown++;
        }
    }
}

static void checked_runner(NativeFn fn, const char *name, maddr entry) {
    int fi = fn_index_of(name);
    /* "nocheck" (sig override): functions that never return in a run (main loops) */
    if (checking || fi < 0 || fn_info[fi].nocheck || check_count[fi] >= check_budget) { fn(); return; }
    check_count[fi]++;
    checking = 1;
    entry_g = entry;
    fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        /* child: run the original code at its entry with the args still on the stack */
        { extern int runtime_is_check_child; runtime_is_check_child = 1; }
        wr16(entry, orig_word[fi]);
        uint32_t d0 = cpu_call(entry, NULL);
        memcpy(shared_mem, M, MEM_SIZE);
        shared_regs[0] = d0; shared_regs[1] = cpu_get(R_SP);
        shared_regs[2] = (uint32_t)runtime_trap_count; shared_regs[3] = (uint32_t)runtime_us();
        _exit(0);
    }
    fn();
    int st; waitpid(pid, &st, 0);
    uint32_t d0 = cpu_get(R_D0);
    maddr sp = cpu_get(R_SP);
    /* the original saw its entry word patched back by the child only; compare */
    uint32_t rmask = fn_info[fi].ret_bytes >= 4 ? 0xFFFFFFFFu : fn_info[fi].ret_bytes == 2 ? 0xFFFFu : fn_info[fi].ret_bytes == 1 ? 0xFFu : 0;
    int differs = ((d0 ^ shared_regs[0]) & rmask) != 0;
    if (!differs) {
        for (uint32_t a = 0; a < MEM_SIZE; a += 4096) {
            uint32_t n = 4096;
            if (memcmp(M + a, shared_mem + a, n)) {
                for (uint32_t b = a; b < a + n; b++) {
                    if (M[b] == shared_mem[b]) continue;
                    if (b >= THUNK_BASE && b < THUNK_BASE + 2 * THUNK_COUNT) continue;
                    if (b < sp && b >= sp - 0x10000) continue;
                    if (b == entry || b == entry + 1) continue;
                    if (check_ignored(b)) continue;
                    differs = 1; break;
                }
            }
            if (differs) break;
        }
    }
    if (differs) {
        check_fail[fi]++;
        fprintf(stderr, "[check] MISMATCH in %s (call %d)\n", name, check_count[fi]);
        if (shared_regs[2] != (uint32_t)runtime_trap_count || shared_regs[3] != (uint32_t)runtime_us())
            fprintf(stderr, "[check] %s: traps/clock port=%u/%uus original=%u/%uus\n", name,
                    (uint32_t)runtime_trap_count, (uint32_t)runtime_us(), shared_regs[2], shared_regs[3]);
        report_diff(fi, sp, d0, shared_regs[0]);
    } else if (check_count[fi] == 1) {
        fprintf(stderr, "[check] %s ok\n", name);
    }
    checking = 0;
}

/* ---- POP2_CODEGUARD=<seg>: report the first ported function whose call changes that
 * code segment (catches stray native writes through stale pointers). Debug only. */
static int guard_seg = -1; static uint8_t *guard_copy; static maddr guard_base; static uint32_t guard_len;
static void guard_snapshot(void) {
    maddr h = loader_segment_handle((int16_t)guard_seg);
    if (!h || !rd32(h)) { guard_len = 0; return; }
    guard_base = rd32(h) & ADDR_MASK; guard_len = rd32(guard_base - 16);
    guard_copy = realloc(guard_copy, guard_len); memcpy(guard_copy, M + guard_base, guard_len);
}
static void guard_runner(NativeFn fn, const char *name, maddr entry) {
    fn();
    if (guard_len && memcmp(guard_copy, M + guard_base, guard_len)) {
        for (uint32_t i = 0; i < guard_len; i++) if (guard_copy[i] != M[guard_base + i]) {
            fprintf(stderr, "[codeguard] %s changed seg%d+%04x (%02x -> %02x)\n", name, guard_seg, i, guard_copy[i], M[guard_base + i]);
            break;
        }
        guard_snapshot();
    }
}

static int in_list(const char *list, const char *name) {
    if (!list) return 0;
    size_t n = strlen(name);
    for (const char *p = list; (p = strstr(p, name)); p += n)
        if ((p == list || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) return 1;
    return 0;
}

static void on_segment_loaded(int16_t seg, maddr code) {
    for (int i = 0; i < FN_COUNT; i++)
        if (native_enabled[i] && fn_info[i].seg == seg) {
            uint16_t w = rd16(code + fn_info[i].segoff);
            if (w != hook_word[i]) orig_word[i] = w;
            wr16(code + fn_info[i].segoff, hook_word[i]);
        }
    if (seg == guard_seg) guard_snapshot();
}

void native_init(void) {
    const char *mode = getenv("POP2_NATIVE");
    const char *excl = getenv("POP2_NATIVE_EXCLUDE");
    const char *only = getenv("POP2_NATIVE_ONLY");
    int n = 0;
    for (int i = 0; i < FN_COUNT; i++) {
        if (!fn_info[i].glue) continue;
        if (mode && !strcmp(mode, "none")) continue;
        if (in_list(excl, fn_info[i].name)) continue;
        if (only && !in_list(only, fn_info[i].name)) continue;
        native_enabled[i] = 1;
        hook_word[i] = cpu_register_hook(fn_info[i].glue, fn_info[i].name);
        n++;
    }
    loader_segment_loaded = on_segment_loaded;
    if (getenv("POP2_CHECK")) {
        check_budget = atoi(getenv("POP2_CHECK"));
        if (check_budget <= 0) check_budget = 20;
        shared_mem = mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
        shared_regs = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
        cpu_hook_runner = checked_runner;
        fprintf(stderr, "native: differential checking, %d calls per function\n", check_budget);
    }
    if (getenv("POP2_CODEGUARD")) { guard_seg = atoi(getenv("POP2_CODEGUARD")); cpu_hook_runner = guard_runner; }
    if (n) fprintf(stderr, "native: %d ported functions active\n", n);
}

void native_set_a5(maddr a5) { g_a5 = a5; }

void port_divide_by_zero(const char *where) {
    extern void runtime_quit(void);
    fprintf(stderr, "*** division by zero in %s (the original Mac game stops with a system error here)\n", where);
    runtime_quit();
    _exit(1);
}

maddr fn_address(int i) {
    const FnInfo *f = &fn_info[i];
    maddr h = loader_segment_handle((int16_t)f->seg);
    if (!h || !rd32(h)) { LoadSeg((int16_t)f->seg); h = loader_segment_handle((int16_t)f->seg); }
    return rd32(h) + f->segoff;
}

uint32_t call68_addr(maddr addr) { return cpu_call(addr, NULL); }

#include <stdarg.h>
uint32_t call_ptr(maddr fn, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = (int)strlen(fmt);
    uint32_t vals[16];
    for (int i = 0; i < n && i < 16; i++) vals[i] = va_arg(ap, uint32_t);
    va_end(ap);
    maddr sp0 = emu_sp();
    for (int i = n - 1; i >= 0; i--) { if (fmt[i] == 'w') push16((uint16_t)vals[i]); else push32(vals[i]); }
    uint32_t r = call68_addr(fn);
    emu_set_sp(sp0);
    return r;
}

/* Check build: a ported function called from C through its 68k entry (hook + checker). */
uint32_t call68_fn_chk(int i) {
    if (fn_info[i].jt_a5off) return cpu_call(g_a5 + fn_info[i].jt_a5off, NULL);
    return cpu_call(fn_address(i), NULL);
}
uint32_t call68_fn(int i) {
    if (native_enabled[i]) {           /* a ported function reached through a trampoline */
        fprintf(stderr, "call68_fn: %s is native\n", fn_info[i].name);
    }
    /* Like the original "jsr off(A5)": through the jump table when the function has a
     * slot, so an unloaded segment is brought in by the _LoadSeg trap (THINK C patches
     * it, and the trap advances the deterministic clock exactly like the original). */
    if (fn_info[i].jt_a5off) return cpu_call(g_a5 + fn_info[i].jt_a5off, NULL);
    return cpu_call(fn_address(i), NULL);
}

/* ---- Toolbox calls from C ---- */
static maddr tb_sp[64]; static int tb_depth;
void tb_begin(void) { tb_sp[tb_depth++] = emu_sp(); }
void tb_end(void) { emu_set_sp(tb_sp[--tb_depth]); }
void tb_trap(uint16_t w) { trap_invoke(w); }
