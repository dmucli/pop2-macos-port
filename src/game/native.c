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

static maddr entry_g;
static void report_diff(int fi, maddr sp_after, uint32_t d0_c, uint32_t d0_68k) {
    int shown = 0;
    uint32_t rmask = fn_info[fi].ret_bytes >= 4 ? 0xFFFFFFFFu : fn_info[fi].ret_bytes == 2 ? 0xFFFFu : fn_info[fi].ret_bytes == 1 ? 0xFFu : 0;
    if ((d0_c ^ d0_68k) & rmask) { fprintf(stderr, "[check] %s: D0 port=%08x original=%08x\n", fn_info[fi].name, d0_c, d0_68k); shown++; }
    for (uint32_t a = 0; a < MEM_SIZE && shown < 12; a++) {
        if (a >= THUNK_BASE && a < THUNK_BASE + 2 * THUNK_COUNT) continue;
        if (a < sp_after && a >= sp_after - 0x10000) continue;          /* dead stack below SP */
        if (M[a] != shared_mem[a] && a != entry_g && a != entry_g + 1) {
            fprintf(stderr, "[check] %s: mem %06x port=%02x original=%02x\n", fn_info[fi].name, a, M[a], shared_mem[a]);
            shown++;
        }
    }
}

static void checked_runner(NativeFn fn, const char *name, maddr entry) {
    int fi = fn_index_of(name);
    if (checking || fi < 0 || check_count[fi] >= check_budget) { fn(); return; }
    check_count[fi]++;
    checking = 1;
    entry_g = entry;
    fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        /* child: run the original code at its entry with the args still on the stack */
        wr16(entry, orig_word[fi]);
        uint32_t d0 = cpu_call(entry, NULL);
        memcpy(shared_mem, M, MEM_SIZE);
        shared_regs[0] = d0; shared_regs[1] = cpu_get(R_SP);
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
                    differs = 1; break;
                }
            }
            if (differs) break;
        }
    }
    if (differs) {
        check_fail[fi]++;
        fprintf(stderr, "[check] MISMATCH in %s (call %d)\n", name, check_count[fi]);
        report_diff(fi, sp, d0, shared_regs[0]);
    } else if (check_count[fi] == 1) {
        fprintf(stderr, "[check] %s ok\n", name);
    }
    checking = 0;
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
    if (n) fprintf(stderr, "native: %d ported functions active\n", n);
}

void native_set_a5(maddr a5) { g_a5 = a5; }

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

uint32_t call68_fn(int i) {
    if (native_enabled[i]) {           /* a ported function reached through a trampoline */
        fprintf(stderr, "call68_fn: %s is native\n", fn_info[i].name);
    }
    return cpu_call(fn_address(i), NULL);
}

/* ---- Toolbox calls from C ---- */
static maddr tb_sp[64]; static int tb_depth;
void tb_begin(void) { tb_sp[tb_depth++] = emu_sp(); }
void tb_end(void) { emu_set_sp(tb_sp[--tb_depth]); }
void tb_trap(uint16_t w) { trap_invoke(w); }
