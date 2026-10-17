/* Prince of Persia 2 native port — entry point. */
#include "mac/mac.h"
#include "fs/vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <execinfo.h>
#include <dirent.h>
#include <strings.h>

void traps_loader_init(void);
void runtime_init_lowmem(void);
void runtime_install_exception_vectors(void);
void runtime_idle(void);
void runtime_system_resources(void);
int  platform_init(int scale);
void platform_shutdown(void);

const char *macos_locate_disk_image(int force_ask);
const char *macos_support_dir(void);
void macos_alert(const char *title, const char *text);
int  macos_in_app_bundle(void);

/* A crash: say where (signal, the ported functions being run, the host stack) in the log. */
static void on_crash(int sig) {
    signal(sig, SIG_DFL);
    fprintf(stderr, "\n*** crashed: signal %d (%s)\n", sig, strsignal(sig));
    extern void native_print_stack(FILE *f);
    native_print_stack(stderr);
    void *bt[64];
    int n = backtrace(bt, 64);
    backtrace_symbols_fd(bt, n, 2);
    fflush(stderr);
    raise(sig);
}

/* No disk image given (Linux, handhelds): a disk image file in the current folder, next to
 * the program or in the data folder, the first one that holds an HFS volume. */
static const char *find_disk_image(void) {
    static const char *const exts[] = { ".hfv", ".dsk", ".img", ".image", ".toast", ".iso", ".cdr", ".bin", ".hda", ".dc42" };
    static char found[2048];
    char dirs[3][1024] = { ".", "", "" };
#ifdef __linux__
    ssize_t n = readlink("/proc/self/exe", dirs[1], sizeof dirs[1] - 1);
    if (n > 0) { dirs[1][n] = 0; char *s = strrchr(dirs[1], '/'); if (s) *s = 0; else dirs[1][0] = 0; }
#endif
    snprintf(dirs[2], sizeof dirs[2], "%s", macos_support_dir());
    for (int d = 0; d < 3; d++) {
        if (!dirs[d][0]) continue;
        DIR *dir = opendir(dirs[d]);
        if (!dir) continue;
        struct dirent *e;
        while ((e = readdir(dir))) {
            const char *dot = strrchr(e->d_name, '.');
            int ok = 0;
            for (size_t k = 0; dot && k < sizeof exts / sizeof exts[0]; k++) ok |= !strcasecmp(dot, exts[k]);
            if (!ok) continue;
            snprintf(found, sizeof found, "%s/%s", dirs[d], e->d_name);
            if (vfs_mount_hfs(found, NULL) == 0) { closedir(dir); fprintf(stderr, "disk image: %s\n", found); return found; }
        }
        closedir(dir);
    }
    return NULL;
}

static void on_term(int sig) {
    (void)sig;
    extern void cpu_print_native_stats(void);
    if (getenv("POP2_NATIVE_STATS")) cpu_print_native_stats();
    _exit(0);
}

int main(int argc, char **argv) {
    const char *hfv = NULL, *app = ":Prince2:Prince of Persia 2";
    /* the app bundle: messages go to ~/Library/Logs/PoP2 Port.log (last run only) */
    if (macos_in_app_bundle() && getenv("HOME")) {
        char log[1024]; snprintf(log, sizeof log, "%s/Library/Logs/PoP2 Port.log", getenv("HOME"));
        if (freopen(log, "w", stderr)) setvbuf(stderr, NULL, _IOLBF, 0);
    }
    int headless = 0, scale = 2;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--app") && i + 1 < argc) app = argv[++i];
        else if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strncmp(argv[i], "-psn_", 5)) continue;          /* Finder launch */
        else hfv = argv[i];
    }
    /* The game data comes from the user's own copy of the original disk image:
     * command line, POP2_HFV, or (app / interactive use) the remembered or chosen file. */
    int asked = 0;
    if (!hfv) hfv = getenv("POP2_HFV");
#ifdef __APPLE__
    if (!hfv && !headless) { hfv = macos_locate_disk_image(0); asked = 1; }
#endif
    if (!hfv) hfv = find_disk_image();
    if (!hfv) {
        fprintf(stderr, "usage: pop2 [path/to/disk image]\n"
                        "  Prince of Persia 2 runs from your own copy of the original Macintosh game: the disk\n"
                        "  image of its floppy, CD or hard disk (.hfv, .dsk, .img, .image, .toast, .iso, .bin).\n"
                        "  Without a path, a disk image in this folder, next to the program or in\n"
                        "  %s is used.\n", macos_support_dir());
        return 1;
    }

    char overlay[1024];
    if (getenv("POP2_OVERLAY")) snprintf(overlay, sizeof overlay, "%s", getenv("POP2_OVERLAY"));
    else snprintf(overlay, sizeof overlay, "%s/disk", macos_support_dir());

    while (vfs_mount_hfs(hfv, overlay)) {
        fprintf(stderr, "%s: not a Prince of Persia 2 HFS disk image\n", hfv);
        if (!asked) return 1;
        macos_alert("This file is not a usable disk image",
                    "Choose the HFS disk image of your original Prince of Persia 2 for Macintosh.");
        if (!(hfv = macos_locate_disk_image(1))) return 1;
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
    if (!getenv("POP2_CHECK")) {
        int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
        for (int i = 0; i < 5; i++) signal(sigs[i], on_crash);
    }
    if (!headless) { extern void platform_menu_early_init(void); platform_menu_early_init(); }
    if (!headless && platform_init(scale)) return 1;
    if (loader_launch(app)) return 1;
    if (!headless) { extern void runtime_set_app_icon(void); runtime_set_app_icon(); }
    cpu_set_idle_hook(runtime_idle);
    cpu_run_forever();
    fm_close_all();
    vfs_flush_all();
    if (!headless) platform_shutdown();
    fprintf(stderr, "exited after %llu instructions\n", (unsigned long long)cpu_instr_count);
    return 0;
}
