/* Machine environment: low memory, exceptions, ticks, VBL / Time Manager tasks. */
#include "mac.h"
#include "../fs/vfs.h"
#include "m68k.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

void platform_poll(void);
void platform_present_if_due(void);
void time_run_tasks(uint64_t now_us);
void sound_service(void);

static uint64_t t0_us;
uint64_t host_us(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
/* Deterministic mode (POP2_DETERMINISTIC=1): time is virtual and only advances
 * per Toolbox trap (and per idle slice when no trap ran), so a run is a pure
 * function of the trap call sequence. Used to A/B-test ported code. */
int runtime_deterministic;
static uint64_t vclock_us;
static int traps_since_idle;
uint64_t runtime_us(void) { return runtime_deterministic ? vclock_us : host_us() - t0_us; }
void runtime_trap_boundary(void);
static int traps_since_peek = 1;
uint64_t runtime_trap_count;
void runtime_trap_hook(void) {
    runtime_trap_count++;
    if (!runtime_deterministic) return;
    vclock_us += 8;
    traps_since_idle++;
    traps_since_peek = 1;
    runtime_trap_boundary();
}
uint32_t runtime_ticks(void) { return (uint32_t)(runtime_us() * 60 / 1000000u); }
/* Deterministic mode: a read of the Ticks low-memory global that follows another
 * one with no trap in between is a polling loop; let virtual time pass. This keeps
 * time independent of how many instructions run (ported C vs original 68k code). */
void runtime_ticks_peek(void) {
    if (!runtime_deterministic) return;
    if (traps_since_peek) { traps_since_peek = 0; return; }
    vclock_us += 50;
    wr32(LM_Ticks, runtime_ticks());
}

void runtime_init_lowmem(void) {
    t0_us = host_us();
    runtime_deterministic = getenv("POP2_DETERMINISTIC") != NULL;
    /* low memory is zero from allocation; the Memory Manager already set its zone globals */
    wr16(LM_ROM85, 0x3FFF);           /* 128K+ ROMs with Color QuickDraw */
    wr16(LM_SysVersion, 0x0710);
    wr8(LM_MMU32Bit, 1);
    wr16(LM_MBarHeight, 20);
    wr8(LM_ResLoad, 1);
    wr32(LM_DoubleTime, 30);
    wr32(LM_CaretTime, 32);
    wr16(LM_SysFontFam, 0);
    wr16(LM_ApFontID, 1);
    wr8(LM_HiliteMode, 0xFF);
    wr16(LM_TimeDBRA, 2000);
    wr16(0x0B22, 0x8000 | 0x4000);    /* HWCfgFlags */
    wr8(LM_KbdType, 2);
    wr32(LM_Time, vfs_mac_time_now());
    wr16(LM_BootDrive, 1);
    wr32(LM_Lo3Bytes, 0x00FFFFFF);
    wr32(LM_MinStack, 0x400);
    wr32(LM_DefltStack, 0x10000);
    wr8(LM_SdVolume, 7);
    wr16(LM_DlgFont, 0);
    wr16(LM_SysEvtMask, 0xFFEF);
    c_to_pstr("System", LM_SysResName);
    c_to_pstr("Finder", LM_FinderName);
    wr16(LM_ScrapState, 0);
    /* Drive queue (DrvQHdr at 0x308): one fixed, writable hard disk as drive 1.
     * Four flag bytes precede each DrvQEl: [writeProt][diskInPlace][...][...] */
    maddr dq = NewPtrSys(32);
    wr8(dq + 0, 0x00); wr8(dq + 1, 0x08); wr8(dq + 2, 0x00); wr8(dq + 3, 0x00);
    maddr el = dq + 4;
    wr32(el, 0); wr16(el + 4, 1); wr16(el + 6, 1); wr16(el + 8, (uint16_t)-33); wr16(el + 10, 0);
    wr16(el + 12, 0xFFFF); wr16(el + 14, 0x000F);
    wr16(0x308, 0); wr32(0x30A, el); wr32(0x30E, el);

    /* Unit table: DCtlHandles for the drivers the game looks at (refnum r -> unit -1-r).
     * DCE: dCtlDriver(4) dCtlFlags(2) ...; driver header: name Str255 at +18. */
    maddr ut = NewPtrSys(64 * 4);
    wr32(LM_UTableBase, ut);
    wr16(0x01D2, 64);                      /* UnitNtryCnt */
    static const struct { int16_t ref; const char *name; } drv[] = {
        { -4, ".Sound" }, { -5, ".Sony" }, { -33, ".EDisk" } };
    for (unsigned i = 0; i < sizeof drv / sizeof drv[0]; i++) {
        maddr hdr = NewPtrSys(64);
        c_to_pstr(drv[i].name, hdr + 18);
        maddr dce = NewHandleSys(52);
        wr32(rd32(dce), hdr);              /* dCtlDriver: pointer (dRAMBased clear) */
        wr16(rd32(dce) + 4, 0x0020);       /* open */
        wr16(rd32(dce) + 24, (uint16_t)drv[i].ref);
        wr32(ut + 4 * (uint32_t)(-1 - drv[i].ref), dce);
    }
}

/* CPU exceptions (bus/address errors, illegal instructions, divide by zero...) */
static void report_exception(void) {
    uint32_t sp = cpu_get(R_SP);
    /* thunk glue already popped 4 bytes (SR + high PC word) as a "return address" */
    uint16_t sr = (uint16_t)(trap_ret >> 16);
    uint32_t pc = (trap_ret & 0xFFFF) << 16 | rd16(sp);
    uint16_t fmt = rd16(sp + 2);
    fprintf(stderr, "\n*** 68k exception vector %d at PC=%06x (SR=%04x)\n", (fmt & 0xFFF) / 4, pc, sr);
    char buf[128];
    for (uint32_t a = pc > 16 ? pc - 16 : pc, n = 0; n < 12; n++) {
        unsigned l = m68k_disassemble(buf, a, M68K_CPU_TYPE_68020);
        fprintf(stderr, "  %06x%s %s\n", a, a == pc ? ">" : " ", buf);
        a += l;
    }
    for (int r = 0; r < 8; r++) fprintf(stderr, "  D%d=%08x A%d=%08x\n", r, cpu_get(R_D0 + r), r, cpu_get(R_A0 + r));
    extern void cpu_dump_history(int n);
    cpu_dump_history(getenv("POP2_HIST") ? atoi(getenv("POP2_HIST")) : 60);
    cpu_stop();
}
void runtime_install_exception_vectors(void) {
    maddr t = cpu_thunk(report_exception, "exception");
    for (int v = 2; v < 64; v++) if (v != 10 && v != 11) wr32((maddr)v * 4, t);
}

/* ---- debug: periodic screen snapshots (POP2_SNAP=dir) as PPM ---- */
#include "qd.h"
static void snapshot(void) {
    static const char *dir; static int init, n; static uint64_t next;
    if (!init) { init = 1; dir = getenv("POP2_SNAP"); next = runtime_us() + 2000000; }
    if (!dir || runtime_us() < next) return;
    next = runtime_us() + (getenv("POP2_SNAP_MS") ? (uint64_t)atoi(getenv("POP2_SNAP_MS")) * 1000u : 3000000u);
    char path[1024]; snprintf(path, sizeof path, "%s/snap%05d.ppm", dir, n++);
    FILE *f = fopen(path, "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", qd_screen_w, qd_screen_h);
    for (int i = 0; i < qd_screen_w * qd_screen_h; i++) {
        uint32_t c = qd_device_rgb(rd8(SCREEN_BASE + (uint32_t)i));
        uint8_t px[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

/* Test-only: POP2_TEST_POKE="A5off:size:value,..." writes A5-relative words/longs
 * once the application globals exist (debugging aid for automated scenarios). */
static void test_pokes(void) {
    static int done;
    if (done || !rd32(LM_CurrentA5)) return;
    const char *s = getenv("POP2_TEST_POKE");
    if (!s) { done = 1; return; }
    if (runtime_us() < 2000000) return;          /* after the THINK C startup initialised globals */
    done = 1;
    char buf[512]; strncpy(buf, s, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
        long off; int size; unsigned long val;
        if (sscanf(t, "%li:%d:%li", &off, &size, (long *)&val) != 3) continue;
        maddr a = rd32(LM_CurrentA5) + (int32_t)off;
        if (size == 1) wr8(a, (uint8_t)val); else if (size == 2) wr16(a, (uint16_t)val); else wr32(a, (uint32_t)val);
        fprintf(stderr, "[test] poke A5%+ld = %lx\n", off, val);
    }
}

/* Work that must happen at deterministic points: timers, input script, snapshots, audio. */
void runtime_trap_boundary(void) {
    static int busy;
    if (busy) return;
    busy = 1;
    static uint32_t last_tick;
    uint32_t t = runtime_ticks();
    if (t != last_tick) { last_tick = t; wr32(LM_Ticks, t); wr32(LM_Time, vfs_mac_time_now()); }
    time_run_tasks(runtime_us());
    sound_service();
    { extern void event_script_tick(uint64_t now_ms); event_script_tick(runtime_us() / 1000); }
    test_pokes();
    snapshot();
    {   /* POP2_EXIT_AT_MS: end a deterministic run at a fixed virtual time */
        static long exit_at = -1;
        if (exit_at < 0) exit_at = getenv("POP2_EXIT_AT_MS") ? atol(getenv("POP2_EXIT_AT_MS")) : 0;
        if (exit_at && (long)(runtime_us() / 1000) >= exit_at) {
            extern void cpu_print_native_stats(void);
            if (getenv("POP2_NATIVE_STATS")) cpu_print_native_stats();
            fprintf(stderr, "exit at virtual %ld ms\n", exit_at);
            fflush(NULL);                       /* stderr and any debug logs (trap log) */
            _exit(0);
        }
    }
    busy = 0;
}

/* ---- idle: called between CPU slices ---- */
void runtime_idle(void) {
    if (runtime_deterministic) {
        /* Loops that neither call traps nor poll Ticks still progress, but only after a
         * long trap-free stretch, so ordinary code never depends on instruction counts. */
        static int quiet_slices;
        if (traps_since_idle) quiet_slices = 0;
        else if (++quiet_slices >= 32) { vclock_us += 1000; runtime_trap_boundary(); }
        traps_since_idle = 0;
        platform_poll();
        platform_present_if_due();
        return;
    }
    static uint32_t last_tick;
    uint32_t t = runtime_ticks();
    if (t != last_tick) {
        last_tick = t;
        wr32(LM_Ticks, t);
        wr32(LM_Time, vfs_mac_time_now());
    }
    time_run_tasks(runtime_us());
    sound_service();
    { extern void event_script_tick(uint64_t now_ms); event_script_tick(runtime_us() / 1000); }
    snapshot();
    platform_poll();
    platform_present_if_due();
}
