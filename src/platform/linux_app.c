/* Host glue outside macOS (Linux, e.g. Recalbox): what macos_app.m and platform_menu.m
 * provide on the Mac, without native windows or menus.
 *
 * - settings: "key=value" lines in <support dir>/settings.txt
 * - support dir: $POP2_HOME, else $XDG_DATA_HOME/pop2-port, else ~/.local/share/pop2-port
 * - the game's alerts are drawn by the emulated Dialog Manager (macos_game_alert returns
 *   -1); Open / Save dialogs choose by themselves (newest game, default name). */
#ifndef __APPLE__
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <sys/stat.h>

static void mkdirs(const char *path) {
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}

const char *macos_support_dir(void) {
    static char path[1024];
    if (!*path) {
        const char *h = getenv("POP2_HOME"), *x = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
        if (h && *h) snprintf(path, sizeof path, "%s", h);
        else if (x && *x) snprintf(path, sizeof path, "%s/pop2-port", x);
        else snprintf(path, sizeof path, "%s/.local/share/pop2-port", home ? home : ".");
        mkdirs(path);
    }
    return path;
}

const char *macos_locate_disk_image(int force_ask) { (void)force_ask; return NULL; }
void macos_alert(const char *title, const char *text) { fprintf(stderr, "%s: %s\n", title, text); }
int  macos_in_app_bundle(void) { return 0; }
int  macos_choose_file(const char *prompt, const char *const *names, int n) { (void)prompt; (void)names; return n > 0 ? 0 : -1; }
int  macos_ask_save_name(const char *prompt, const char *def, char *out, int outsz, int (*exists)(const char *)) {
    (void)prompt; (void)exists;
    snprintf(out, (size_t)outsz, "%s", def);
    return 1;
}
void macos_set_app_icon(const uint8_t *rgba, int w, int h) { (void)rgba; (void)w; (void)h; }
void macos_beep(void) {}
int  macos_game_alert(int kind, const char *text, const char *const *buttons, int n) {
    (void)kind; (void)text; (void)buttons; (void)n;
    return -1;
}
void platform_menu_early_init(void) {}

/* ---- settings ---- */
#define MAX_PREFS 128
static struct { char key[48]; int val; } prefs[MAX_PREFS];
static int nprefs = -1;

static const char *prefs_path(void) {
    static char p[1100];
    if (!*p) snprintf(p, sizeof p, "%s/settings.txt", macos_support_dir());
    return p;
}
static void prefs_load(void) {
    if (nprefs >= 0) return;
    nprefs = 0;
    FILE *f = fopen(prefs_path(), "r");
    if (!f) return;
    char line[128];
    while (nprefs < MAX_PREFS && fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        snprintf(prefs[nprefs].key, sizeof prefs[nprefs].key, "%s", line);
        prefs[nprefs].val = atoi(eq + 1);
        nprefs++;
    }
    fclose(f);
}
int macos_pref_int(const char *key, int def) {
    prefs_load();
    for (int i = 0; i < nprefs; i++) if (!strcmp(prefs[i].key, key)) return prefs[i].val;
    return def;
}
void macos_pref_set_int(const char *key, int v) {
    prefs_load();
    int i = 0;
    while (i < nprefs && strcmp(prefs[i].key, key)) i++;
    if (i == nprefs) {
        if (nprefs == MAX_PREFS) return;
        snprintf(prefs[nprefs++].key, sizeof prefs[0].key, "%s", key);
    }
    prefs[i].val = v;
    FILE *f = fopen(prefs_path(), "w");
    if (!f) { fprintf(stderr, "settings: %s: %s\n", prefs_path(), strerror(errno)); return; }
    for (int k = 0; k < nprefs; k++) fprintf(f, "%s=%d\n", prefs[k].key, prefs[k].val);
    fclose(f);
}
#endif
