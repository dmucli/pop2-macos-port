/* Segment Loader and application launch. */
#include "mac.h"
#include "../fs/vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static uint32_t jt_base;     /* A5 + CurJTOffset */
static maddr seg_handles[256];
void (*loader_segment_loaded)(int16_t seg, maddr code) = 0;

/* Translate a runtime address inside a loaded CODE segment into "segN+off" (offset from resource start). */
int loader_locate(maddr a, int *seg, uint32_t *off) {
    for (int i = 1; i < 256; i++) {
        maddr h = seg_handles[i];
        if (!h || !rd32(h)) continue;
        maddr p = rd32(h);
        uint32_t sz = rd32((p & ADDR_MASK) - 16);   /* block size, read directly: no MemErr side effect */
        if (a >= p && a < p + sz) { *seg = i; *off = a - p; return 1; }
    }
    return 0;
}
maddr loader_segment_handle(int16_t seg) { return (seg >= 0 && seg < 256) ? seg_handles[seg] : 0; }

void LoadSeg(int16_t seg) {
    if (seg < 0 || seg > 255) { fprintf(stderr, "LoadSeg: bad segment %d\n", seg); cpu_stop(); return; }
    maddr h = seg_handles[seg];
    if (!h || !rd32(h)) {
        int16_t cur = CurResFile();
        UseResFile((int16_t)rd16(LM_CurApRefNum));
        uint8_t rl = rd8(LM_ResLoad); wr8(LM_ResLoad, 1);
        h = GetResource(FOURCC('C','O','D','E'), seg);
        wr8(LM_ResLoad, rl);
        UseResFile(cur);
        if (!h) { fprintf(stderr, "LoadSeg: CODE %d missing\n", seg); cpu_stop(); return; }
        HLock(h); HNoPurge(h);
        seg_handles[seg] = h;
    }
    maddr code = rd32(h);
    uint16_t first = rd16(code), count = rd16(code + 2);
    for (unsigned i = 0; i < count; i++) {
        maddr e = jt_base + first + 8 * i;
        if (rd16(e + 2) == 0x3F3C && rd16(e + 6) == 0xA9F0) {
            uint16_t off = rd16(e);
            wr16(e, (uint16_t)seg);
            wr16(e + 2, 0x4EF9);
            wr32(e + 4, code + 4 + off);
        }
    }
    if (loader_segment_loaded) loader_segment_loaded(seg, code);
}

static void T_LoadSeg(uint16_t w) {
    (void)w;
    int16_t seg = pops16();
    LoadSeg(seg);
    trap_ret -= 6;          /* re-execute the (now patched) jump table entry */
}
static void T_UnloadSeg(uint16_t w) { (void)w; pop32(); }

/* ExitToShell never returns (ported C code may be inside an endless game loop): close
 * files, flush the writable overlay and end the process. A checker child process ends
 * silently without writing anything. */
int runtime_is_check_child;
void ExitToShell(void) {
    if (runtime_is_check_child) _exit(0);
    fprintf(stderr, "ExitToShell\n");
    { extern void cpu_print_native_stats(void); if (getenv("POP2_NATIVE_STATS")) cpu_print_native_stats(); }
    fm_close_all();
    vfs_flush_all();
    { extern uint64_t cpu_instr_count; fprintf(stderr, "exited after %llu instructions\n", (unsigned long long)cpu_instr_count); }
    { extern void cpu_print_profile(void); cpu_print_profile(); }
    { extern void platform_shutdown(void); platform_shutdown(); }
    exit(0);
}
void runtime_quit(void) { ExitToShell(); }   /* for code that sees Apple's ExitToShell declaration */
static void T_ExitToShell(uint16_t w) { (void)w; ExitToShell(); }

/* GetAppParms(apName, apRefNum, apParam) */
static void T_GetAppParms(uint16_t w) {
    (void)w;
    maddr param = pop32(), ref = pop32(), name = pop32();
    if (name) mcopy(name, LM_CurApName, 32);
    if (ref) wr16(ref, rd16(LM_CurApRefNum));
    if (param) wr32(param, rd32(0x0AEC));
}

int loader_launch(const char *app_path) {
    uint32_t parid; char leaf[64];
    VNode *app = vfs_resolve_path(2, app_path, &parid, leaf);
    if (!app || app->isdir) {
        /* another layout (a CD, a copied folder): the application by its name and type */
        app = NULL;
        for (VNode *n = g_vol.nodes; n && !app; n = n->next)
            if (!n->isdir && !n->deleted && !mac_strcasecmp(n->name, "Prince of Persia 2") && !memcmp(n->finfo, "APPL", 4)) app = n;
        if (!app) { fprintf(stderr, "Prince of Persia 2 not found on this disk (looked for \"%s\")\n", app_path); return -1; }
        fprintf(stderr, "application found in folder %u\n", app->parid);
    }
    int16_t ref = rsrc_open_node(app, 1);
    if (ref < 0) { fprintf(stderr, "cannot open application resources\n"); return -1; }
    wr16(LM_CurApRefNum, (uint16_t)ref);
    c_to_pstr(app->name, LM_CurApName);
    fm_set_default_dir(app->parid);
    wr16(LM_SFSaveDisk, (uint16_t)1);         /* -SFSaveDisk = vRefNum (-1) */
    wr32(LM_CurDirStore, app->parid);

    maddr c0 = GetResource(FOURCC('C','O','D','E'), 0);
    if (!c0) { fprintf(stderr, "CODE 0 missing\n"); return -1; }
    maddr p = rd32(c0);
    uint32_t above = rd32(p), below = rd32(p + 4), jtsize = rd32(p + 8), jtoff = rd32(p + 12);
    maddr world_top = SCREEN_BASE - 0x100;
    maddr a5 = (world_top - above) & ~3u;
    maddr stack_base = a5 - below;
    mset(stack_base, 0, below + above);
    mcopy(a5 + jtoff, p + 16, jtsize);
    jt_base = a5 + jtoff;
    ReleaseResource(c0);

    wr32(LM_CurrentA5, a5);
    wr32(LM_CurStackBase, stack_base);
    wr16(0x0934, (uint16_t)jtoff);            /* CurJTOffset */
    wr32(LM_ApplLimit, APPHEAP_END);
    wr32(LM_MemTop, MEM_SIZE);
    wr32(LM_BufPtr, SCREEN_BASE);

    /* Finder information: message 0 (open), no documents */
    maddr fh = NewHandleClear(4);
    wr32(0x0AEC, fh);                          /* AppParmHandle */
    wr32(a5 + 0x10, fh);

    cpu_set(R_A5, a5);
    { extern void native_set_a5(maddr a5); native_set_a5(a5); }
    cpu_set(R_SP, stack_base);
    cpu_set(R_SR, 0x2000);                     /* supervisor, interrupts enabled */
    cpu_set(R_PC, jt_base + 2);                /* first jump-table entry */
    fprintf(stderr, "launch: A5=%06x stack=%06x jt=%06x (%u entries)\n", a5, stack_base, jt_base, jtsize / 8);
    return 0;
}

void traps_loader_init(void) {
    trap_register(0xA9F0, T_LoadSeg);
    trap_register(0xA9F1, T_UnloadSeg);
    trap_register(0xA9F4, T_ExitToShell);
    trap_register(0xA9F5, T_GetAppParms);
}

/* save states (src/mac/mods.c): loaded CODE segments */
void *loader_state(size_t *n) { *n = sizeof seg_handles; return seg_handles; }
