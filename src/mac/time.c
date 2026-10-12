/* Time Manager, Vertical Retrace Manager, clocks. */
#include "mac.h"
#include <stdio.h>
#include <time.h>
#include <unistd.h>

uint64_t runtime_us(void);
uint32_t runtime_ticks(void);
void runtime_idle(void);

/* ---- Time Manager tasks: TMTask { qLink, qType, tmAddr, tmCount, tmWakeUp, tmReserved } ---- */
typedef struct { maddr task; uint64_t due; int active; int extended; uint64_t wake; } TmEntry;
#define MAX_TM 32
static TmEntry tm[MAX_TM];

static TmEntry *tm_find(maddr task) { for (int i = 0; i < MAX_TM; i++) if (tm[i].task == task) return &tm[i]; return NULL; }

void InsTime(maddr task, int extended) {
    TmEntry *e = tm_find(task);
    if (!e) for (int i = 0; i < MAX_TM; i++) if (!tm[i].task) { e = &tm[i]; break; }
    if (!e) { fprintf(stderr, "Time Manager: too many tasks\n"); return; }
    e->task = task; e->active = 0; e->extended = extended; e->wake = 0;
    wr16(task + 4, (uint16_t)(extended ? 0x4000 : 0));    /* qType: task inactive */
    if (extended) wr32(task + 14, 0);
}
void RmvTime(maddr task) {
    TmEntry *e = tm_find(task);
    if (!e) return;
    if (e->active) {
        uint64_t now = runtime_us();
        int64_t left = (int64_t)e->due - (int64_t)now;
        if (left < 0) left = 0;
        wr32(task + 10, (uint32_t)(-(int32_t)left));      /* remaining time, negative microseconds */
    } else wr32(task + 10, 0);
    e->task = 0; e->active = 0;
}
void PrimeTime(maddr task, int32_t count) {
    TmEntry *e = tm_find(task);
    if (!e) { InsTime(task, 0); e = tm_find(task); if (!e) return; }
    uint64_t delay = count >= 0 ? (uint64_t)count * 1000u : (uint64_t)(-(int64_t)count);
    uint64_t now = runtime_us();
    uint64_t base = (e->extended && e->wake) ? e->wake : now;
    if (e->extended && e->wake && base + delay < now) base = now;   /* fell behind: don't burst */
    e->due = base + delay;
    e->wake = e->due;
    e->active = 1;
    wr16(task + 4, (uint16_t)(rd16(task + 4) | 0x8000)); /* task active */
}

void time_run_tasks(uint64_t now) {
    for (int i = 0; i < MAX_TM; i++) {
        TmEntry *e = &tm[i];
        if (!e->task || !e->active || now < e->due) continue;
        e->active = 0;
        maddr task = e->task;
        wr16(task + 4, (uint16_t)(rd16(task + 4) & ~0x8000));
        wr32(task + 10, 0);
        maddr fn = rd32(task + 6);
        if (fn) {
            cpu_call_irq(fn, 0, fn, task, 0, NULL);
        }
    }
    /* VBL tasks run once per tick */
    static uint32_t last;
    uint32_t t = runtime_ticks();
    if (t != last) {
        uint32_t n = t - last; if (n > 4) n = 1;
        last = t;
        extern void vbl_tick(void);
        while (n--) vbl_tick();
    }
}

/* ---- VBL queue: VBLTask { qLink, qType, vblAddr, vblCount, vblPhase } ---- */
#define MAX_VBL 32
static maddr vbl[MAX_VBL];
int16_t VInstall(maddr task) {
    for (int i = 0; i < MAX_VBL; i++) if (vbl[i] == task) return 0;
    for (int i = 0; i < MAX_VBL; i++) if (!vbl[i]) { vbl[i] = task; return 0; }
    return -2;
}
int16_t VRemove(maddr task) {
    for (int i = 0; i < MAX_VBL; i++) if (vbl[i] == task) { vbl[i] = 0; return 0; }
    return -1;   /* qErr */
}
void vbl_tick(void) {
    for (int i = 0; i < MAX_VBL; i++) {
        maddr task = vbl[i];
        if (!task) continue;
        int16_t c = (int16_t)rd16(task + 10);
        if (c <= 0) continue;
        wr16(task + 10, (uint16_t)(c - 1));
        if (c - 1 == 0) {
            maddr fn = rd32(task + 6);
            cpu_call_irq(fn, 0, task, 0, 0, NULL);
        }
    }
}

/* ---- clocks ---- */
uint32_t TickCount(void) { return runtime_ticks(); }

static void T_TickCount(uint16_t w) { (void)w; uint32_t t = runtime_ticks(); wr32(LM_Ticks, t); ret32(t); }
static void T_Delay(uint16_t w) {
    (void)w;
    uint32_t target = runtime_ticks() + A0;
    while (runtime_ticks() < target && cpu_running) { runtime_idle(); usleep(1000); }
    cpu_set(R_D0, runtime_ticks());
}
static void T_InsTime(uint16_t w) { InsTime(A0, (w & 0x0400) != 0); cpu_set(R_D0, 0); }
static void T_RmvTime(uint16_t w) { (void)w; RmvTime(A0); cpu_set(R_D0, 0); }
static void T_PrimeTime(uint16_t w) { (void)w; PrimeTime(A0, (int32_t)D0); cpu_set(R_D0, 0); }
static void T_VInstall(uint16_t w) { (void)w; cpu_set(R_D0, (uint32_t)(int32_t)VInstall(A0)); }
static void T_VRemove(uint16_t w) { (void)w; cpu_set(R_D0, (uint32_t)(int32_t)VRemove(A0)); }
static void T_SlotVInstall(uint16_t w) { (void)w; cpu_set(R_D0, (uint32_t)(int32_t)VInstall(A0)); }
static void T_SlotVRemove(uint16_t w) { (void)w; cpu_set(R_D0, (uint32_t)(int32_t)VRemove(A0)); }
static void T_ReadDateTime(uint16_t w) { (void)w; wr32(A0, rd32(LM_Time)); cpu_set(R_D0, 0); }
static void T_SetDateTime(uint16_t w) { (void)w; cpu_set(R_D0, 0); }
static void T_Microseconds(uint16_t w) {
    (void)w; uint64_t us = runtime_us();
    cpu_set(R_A0, (uint32_t)(us >> 32)); cpu_set(R_D0, (uint32_t)us);
}
/* SecondsToDate: D0 = seconds, A0 = DateTimeRec {year, month, day, hour, minute, second, dayOfWeek} */
static void T_SecondsToDate(uint16_t w) {
    (void)w;
    time_t t = (time_t)((int64_t)(uint32_t)D0 - 2082844800);
    struct tm g; gmtime_r(&t, &g);
    maddr r = A0;
    wr16(r, (uint16_t)(g.tm_year + 1900)); wr16(r + 2, (uint16_t)(g.tm_mon + 1)); wr16(r + 4, (uint16_t)g.tm_mday);
    wr16(r + 6, (uint16_t)g.tm_hour); wr16(r + 8, (uint16_t)g.tm_min); wr16(r + 10, (uint16_t)g.tm_sec);
    wr16(r + 12, (uint16_t)(g.tm_wday + 1));
}
/* DateToSeconds: A0 = DateTimeRec -> D0 */
static void T_DateToSeconds(uint16_t w) {
    (void)w; maddr r = A0;
    struct tm g = {0};
    g.tm_year = rd16(r) - 1900; g.tm_mon = rd16(r + 2) - 1; g.tm_mday = rd16(r + 4);
    g.tm_hour = rd16(r + 6); g.tm_min = rd16(r + 8); g.tm_sec = rd16(r + 10);
    cpu_set(R_D0, (uint32_t)(timegm(&g) + 2082844800));
}

void traps_time_init(void) {
    trap_register(0xA975, T_TickCount);
    trap_register(0xA03B, T_Delay);
    trap_register(0xA058, T_InsTime);
    trap_register(0xA059, T_RmvTime);
    trap_register(0xA05A, T_PrimeTime);
    trap_register(0xA033, T_VInstall);
    trap_register(0xA034, T_VRemove);
    trap_register(0xA06F, T_SlotVInstall);
    trap_register(0xA070, T_SlotVRemove);
    trap_register(0xA039, T_ReadDateTime);
    trap_register(0xA03A, T_SetDateTime);
    trap_register(0xA193, T_Microseconds);
    trap_register(0xA9C6, T_SecondsToDate);
    trap_register(0xA9C7, T_DateToSeconds);
}
