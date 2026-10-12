/* Prince of Persia 2 for modern macOS — entry point. */
#include "mac/mac.h"
#include "fs/vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

void traps_loader_init(void);
void runtime_init_lowmem(void);
void runtime_install_exception_vectors(void);
void runtime_idle(void);
void runtime_system_resources(void);
int  platform_init(int scale);
void platform_shutdown(void);

static const char *default_hfv(void) {
    static char p[1024];
    const char *home = getenv("HOME");
    snprintf(p, sizeof p, "%s/Documents/GitHub/PoP2/PoP2.hfv", home ? home : ".");
    return p;
}

static void on_term(int sig) {
    (void)sig;
    extern void cpu_print_native_stats(void);
    if (getenv("POP2_NATIVE_STATS")) cpu_print_native_stats();
    _exit(0);
}

int main(int argc, char **argv) {
    const char *hfv = NULL, *app = ":Prince2:Prince of Persia 2";
    int headless = 0, scale = 2;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--app") && i + 1 < argc) app = argv[++i];
        else if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else hfv = argv[i];
    }
    if (!hfv) hfv = getenv("POP2_HFV") ? getenv("POP2_HFV") : default_hfv();

    char overlay[1024];
    const char *home = getenv("HOME");
    if (getenv("POP2_OVERLAY")) snprintf(overlay, sizeof overlay, "%s", getenv("POP2_OVERLAY"));
    else snprintf(overlay, sizeof overlay, "%s/Library/Application Support/PoP2-port/disk", home ? home : ".");

    if (vfs_mount_hfs(hfv, overlay)) {
        fprintf(stderr, "usage: pop2 [path/to/PoP2.hfv]\n  The game data is read from your own copy of the original disk image.\n");
        return 1;
    }
    if (getenv("POP2_LS")) {
        for (VNode *n = g_vol.nodes; n; n = n->next)
            fprintf(stderr, "%5u in %5u %s %-32s d=%u r=%u\n", n->cnid, n->parid, n->isdir ? "DIR " : "file", n->name, n->forklen[0], n->forklen[1]);
        uint32_t par; char leaf[64];
        VNode *t = vfs_resolve_path(atoi(getenv("POP2_LS")), ":Data:Prince2.opt", &par, leaf);
        fprintf(stderr, "resolve -> %p par=%u leaf=%s\n", (void *)t, par, leaf);
        return 0;
    }
    mm_init();
    cpu_init();
    runtime_init_lowmem();
    runtime_install_exception_vectors();
    traps_mem_init();
    traps_rsrc_init();
    traps_file_init();
    traps_loader_init();
    traps_misc_init();
    traps_qd_init();
    traps_wm_init();
    traps_event_init();
    traps_sound_init();
    traps_time_init();
    runtime_system_resources();
    { extern void native_init(void); native_init(); }
    if (headless) signal(SIGTERM, on_term);
    if (!headless && platform_init(scale)) return 1;
    if (loader_launch(app)) return 1;
    cpu_set_idle_hook(runtime_idle);
    cpu_run_forever();
    fm_close_all();
    vfs_flush_all();
    if (!headless) platform_shutdown();
    fprintf(stderr, "exited after %llu instructions\n", (unsigned long long)cpu_instr_count);
    return 0;
}
