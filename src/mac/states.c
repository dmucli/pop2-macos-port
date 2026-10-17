/* Save state history: each save state is a file in "Save States" next to the overlay
 * directory (normally ~/Library/Application Support/PoP2-port/Save States), holding a
 * header (name, date, level, time left), a thumbnail of the screen, the emulated memory
 * (zlib), the runtime's host-side state, the open resource files and the open files (by
 * file, so that they can be reopened in another run). The newest STATES_MAX are kept.
 *
 * A state is taken and restored at the start of a game frame (see mods.c), in any level
 * and in a later run of the same build with the same disk image. Writing the file
 * (compressing 16 MB) happens on a thread so that saving does not stall the game. */
#include "mac.h"
#include "qd.h"
#include "mods.h"
#include "states.h"
#include "../fs/vfs.h"
#include <zlib.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t *mods_core_blob(size_t *len);
int  mods_core_restorable(const uint8_t *core, size_t len);
void mods_restore_flat(const uint8_t *mem, const uint8_t *core);
size_t rsrc_portable_size(void);
void rsrc_portable_save(uint8_t *o);
int  rsrc_portable_load(const uint8_t *in, size_t len, int apply);
size_t file_fcbs_size(void);
void file_fcbs_save(uint8_t *o);
int  file_fcbs_load(const uint8_t *in, int apply);
void event_menu_command_key(uint8_t ch);
void macos_beep(void);

/* ---- file format: a 256-byte header (big-endian), then thumbnail, memory, host state,
 * resource files, open files ---- */
#define HDR 256
enum { H_MAGIC = 0, H_VERSION = 8, H_TIME = 12, H_LEVEL = 20, H_MINUTES = 24, H_TW = 28, H_TH = 32,
       H_MEMLEN = 36, H_MEMZ = 40, H_HOSTLEN = 44, H_FCBLEN = 48, H_VOLDATE = 52, H_VOLNAME = 56,
       H_NAME = 88, H_RSRCLEN = 152 };
#define MAGIC   "PoP2SAVE"
#define VERSION 1
#define THUMB_BYTES (STATE_THUMB_W * STATE_THUMB_H * 4)
#define EXT     ".pop2state"

typedef struct { StateInfo info; char path[1200]; uint8_t *thumb; } Entry;
static Entry list[64];
static int count = -1;                    /* -1: the directory is not read yet */
static int version;

static const char *dir(void) {
    static char d[1100];
    if (!*d) {
        snprintf(d, sizeof d, "%s", g_vol.overlay_dir);
        size_t n = strlen(d);
        while (n > 1 && d[n - 1] == '/') d[--n] = 0;
        char *s = strrchr(d, '/');
        if (s) *s = 0; else strcpy(d, ".");
        strncat(d, "/Save States", sizeof d - strlen(d) - 1);
        mkdir(d, 0755);
    }
    return d;
}

static int same_disk(const uint8_t *h) {
    return be32(h + H_VOLDATE) == g_vol.crdate && !strncmp((const char *)h + H_VOLNAME, g_vol.volname, 32);
}

static int read_header(const char *path, uint8_t *h) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int ok = fread(h, 1, HDR, f) == HDR;
    fclose(f);
    return ok && !memcmp(h, MAGIC, 8) && be32(h + H_VERSION) == VERSION && same_disk(h);
}

static void info_from_header(const uint8_t *h, StateInfo *s) {
    memcpy(s->name, h + H_NAME, 63); s->name[63] = 0;
    s->time = (int64_t)be32(h + H_TIME) << 32 | be32(h + H_TIME + 4);
    s->level = (int32_t)be32(h + H_LEVEL);
    s->minutes = (int32_t)be32(h + H_MINUTES);
}

static int newest_first(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    if (x->info.time != y->info.time) return x->info.time < y->info.time ? 1 : -1;
    return strcmp(y->path, x->path);
}

static void scan(void) {
    count = 0;
    DIR *d = opendir(dir());
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && count < 64) {
        size_t n = strlen(e->d_name), x = strlen(EXT);
        if (n <= x || strcmp(e->d_name + n - x, EXT)) continue;
        Entry *en = &list[count];
        memset(en, 0, sizeof *en);
        snprintf(en->path, sizeof en->path, "%s/%s", dir(), e->d_name);
        uint8_t h[HDR];
        if (!read_header(en->path, h)) continue;
        info_from_header(h, &en->info);
        count++;
    }
    closedir(d);
    qsort(list, count, sizeof *list, newest_first);
}
static void ensure(void) { if (count < 0) scan(); }

/* ---- writing, on a thread ---- */
typedef struct { char path[1200]; uint8_t hdr[HDR]; uint8_t *thumb, *mem, *host, *rsrc, *fcb; size_t host_len, rsrc_len, fcb_len; } Job;
static pthread_t writer;
static int writer_on;
static volatile int writer_failed;
static char writer_path[1200];

static void *write_job(void *arg) {
    Job *j = arg;
    uLongf zlen = compressBound(MEM_SIZE);
    uint8_t *z = malloc(zlen);
    int ok = z && compress2(z, &zlen, j->mem, MEM_SIZE, 1) == Z_OK;
    put32(j->hdr + H_MEMZ, (uint32_t)zlen);
    char tmp[1300];
    snprintf(tmp, sizeof tmp, "%s.tmp", j->path);
    FILE *f = ok ? fopen(tmp, "wb") : NULL;
    if (f) {
        ok = fwrite(j->hdr, 1, HDR, f) == HDR && fwrite(j->thumb, 1, THUMB_BYTES, f) == THUMB_BYTES &&
             fwrite(z, 1, zlen, f) == zlen && fwrite(j->host, 1, j->host_len, f) == j->host_len &&
             fwrite(j->rsrc, 1, j->rsrc_len, f) == j->rsrc_len && fwrite(j->fcb, 1, j->fcb_len, f) == j->fcb_len;
        ok = (fclose(f) == 0) && ok;
        if (ok) ok = rename(tmp, j->path) == 0;
        if (!ok) remove(tmp);
    } else ok = 0;
    if (!ok) { fprintf(stderr, "[states] could not write %s\n", j->path); writer_failed = 1; }
    free(z); free(j->thumb); free(j->mem); free(j->host); free(j->rsrc); free(j->fcb); free(j);
    return NULL;
}

static void writer_wait(void) {
    if (!writer_on) return;
    pthread_join(writer, NULL);
    writer_on = 0;
    if (writer_failed) {                  /* drop the entry of the state that was not written */
        writer_failed = 0;
        for (int i = 0; i < count; i++) if (!strcmp(list[i].path, writer_path)) { states_delete(i); break; }
    }
}

static void make_thumb(uint8_t *out) {
    uint32_t rgb[256];
    for (int i = 0; i < 256; i++) rgb[i] = qd_device_rgb(i);
    int w = qd_screen_w, h = qd_screen_h;
    const uint8_t *src = M + SCREEN_BASE;
    for (int ty = 0; ty < STATE_THUMB_H; ty++)
        for (int tx = 0; tx < STATE_THUMB_W; tx++) {
            int x0 = tx * w / STATE_THUMB_W, x1 = (tx + 1) * w / STATE_THUMB_W;
            int y0 = ty * h / STATE_THUMB_H, y1 = (ty + 1) * h / STATE_THUMB_H;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    uint32_t c = rgb[src[y * w + x]];
                    r += c >> 16 & 0xFF; g += c >> 8 & 0xFF; b += c & 0xFF; n++;
                }
            uint8_t *o = out + (ty * STATE_THUMB_W + tx) * 4;
            if (!n) n = 1;
            o[0] = (uint8_t)(r / n); o[1] = (uint8_t)(g / n); o[2] = (uint8_t)(b / n); o[3] = 0xFF;
        }
}

void states_save_now(int level, int minutes) {
    ensure();
    writer_wait();
    Job *j = calloc(1, sizeof *j);
    time_t now = time(NULL);
    for (int k = 0; k < 1000; k++) {
        snprintf(j->path, sizeof j->path, "%s/%lld-%d" EXT, dir(), (long long)now, k);
        if (access(j->path, F_OK) != 0) break;
    }
    uint8_t *h = j->hdr;
    memcpy(h + H_MAGIC, MAGIC, 8);
    put32(h + H_VERSION, VERSION);
    put32(h + H_TIME, (uint32_t)((uint64_t)now >> 32)); put32(h + H_TIME + 4, (uint32_t)now);
    put32(h + H_LEVEL, (uint32_t)level); put32(h + H_MINUTES, (uint32_t)minutes);
    put32(h + H_TW, STATE_THUMB_W); put32(h + H_TH, STATE_THUMB_H);
    put32(h + H_MEMLEN, MEM_SIZE);
    j->mem = malloc(MEM_SIZE);
    memcpy(j->mem, M, MEM_SIZE);
    j->host = mods_core_blob(&j->host_len);
    j->rsrc_len = rsrc_portable_size();
    j->rsrc = malloc(j->rsrc_len);
    rsrc_portable_save(j->rsrc);
    j->fcb_len = file_fcbs_size();
    j->fcb = malloc(j->fcb_len);
    file_fcbs_save(j->fcb);
    put32(h + H_HOSTLEN, (uint32_t)j->host_len); put32(h + H_FCBLEN, (uint32_t)j->fcb_len);
    put32(h + H_RSRCLEN, (uint32_t)j->rsrc_len);
    put32(h + H_VOLDATE, g_vol.crdate);
    strncpy((char *)h + H_VOLNAME, g_vol.volname, 32);
    j->thumb = malloc(THUMB_BYTES);
    make_thumb(j->thumb);

    /* the new entry, newest first; the oldest beyond STATES_MAX go */
    if (count == 64) { free(list[63].thumb); count--; }
    memmove(list + 1, list, count * sizeof *list);
    memset(&list[0], 0, sizeof list[0]);
    info_from_header(h, &list[0].info);
    snprintf(list[0].path, sizeof list[0].path, "%s", j->path);
    list[0].thumb = malloc(THUMB_BYTES);
    memcpy(list[0].thumb, j->thumb, THUMB_BYTES);
    count++;
    while (count > STATES_MAX) { count--; remove(list[count].path); free(list[count].thumb); }
    version++;

    snprintf(writer_path, sizeof writer_path, "%s", j->path);
    if (pthread_create(&writer, NULL, write_job, j) == 0) writer_on = 1;
    else { write_job(j); writer_on = 1; writer_wait(); writer_on = 0; }
}

/* ---- the list ---- */
int states_count(void) { ensure(); return count; }
int states_version(void) { return version; }
int states_info(int i, StateInfo *out) {
    ensure();
    if (i < 0 || i >= count) return 0;
    *out = list[i].info;
    return 1;
}
int states_thumb(int i, uint8_t *rgba) {
    ensure();
    if (i < 0 || i >= count) return 0;
    Entry *e = &list[i];
    if (!e->thumb) {
        writer_wait();
        FILE *f = fopen(e->path, "rb");
        uint8_t h[HDR];
        if (!f) return 0;
        uint8_t *t = malloc(THUMB_BYTES);
        int ok = fread(h, 1, HDR, f) == HDR && be32(h + H_TW) == STATE_THUMB_W && be32(h + H_TH) == STATE_THUMB_H &&
                 fread(t, 1, THUMB_BYTES, f) == THUMB_BYTES;
        fclose(f);
        if (!ok) { free(t); return 0; }
        e->thumb = t;
    }
    memcpy(rgba, e->thumb, THUMB_BYTES);
    return 1;
}
void states_rename(int i, const char *name) {
    ensure();
    if (i < 0 || i >= count) return;
    writer_wait();
    char buf[64] = {0};
    strncpy(buf, name, 63);
    FILE *f = fopen(list[i].path, "r+b");
    if (!f) return;
    int ok = fseek(f, H_NAME, SEEK_SET) == 0 && fwrite(buf, 1, 64, f) == 64;
    ok = (fclose(f) == 0) && ok;
    if (ok) { memcpy(list[i].info.name, buf, 64); version++; }
}
void states_delete(int i) {
    ensure();
    if (i < 0 || i >= count) return;
    if (writer_on) writer_wait();
    if (i >= count) return;
    remove(list[i].path);
    free(list[i].thumb);
    memmove(list + i, list + i + 1, (count - i - 1) * sizeof *list);
    count--;
    version++;
}

/* ---- loading, at the start of a frame ---- */
static char load_path[1200];
static int load_req;

void states_request_load(int i) {
    ensure();
    if (i < 0 || i >= count) { macos_beep(); return; }
    snprintf(load_path, sizeof load_path, "%s", list[i].path);
    load_req = 1;
    if (!mods_in_level()) event_menu_command_key('n');   /* New Game: the state is loaded at its first frame */
}
int states_load_pending(void) { return load_req; }
int states_load_waiting(void) { return load_req; }

int states_load_now(void) {
    load_req = 0;
    writer_wait();
    uint8_t h[HDR];
    uint8_t *z = NULL, *host = NULL, *rsrc = NULL, *fcb = NULL, *mem = NULL;
    const char *why = "unreadable";
    int ok = 0;
    FILE *f = fopen(load_path, "rb");
    if (!f || fread(h, 1, HDR, f) != HDR || memcmp(h, MAGIC, 8) || be32(h + H_VERSION) != VERSION) goto done;
    why = "made with another disk image";
    if (!same_disk(h)) goto done;
    why = "unreadable";
    uint32_t zlen = be32(h + H_MEMZ), hlen = be32(h + H_HOSTLEN), rlen = be32(h + H_RSRCLEN), flen = be32(h + H_FCBLEN);
    if (be32(h + H_MEMLEN) != MEM_SIZE || flen != file_fcbs_size() || zlen > compressBound(MEM_SIZE) ||
        hlen > (64u << 20) || rlen > (64u << 20)) goto done;
    z = malloc(zlen); host = malloc(hlen); rsrc = malloc(rlen); fcb = malloc(flen);
    if (fseek(f, HDR + be32(h + H_TW) * be32(h + H_TH) * 4, SEEK_SET) || fread(z, 1, zlen, f) != zlen ||
        fread(host, 1, hlen, f) != hlen || fread(rsrc, 1, rlen, f) != rlen || fread(fcb, 1, flen, f) != flen) goto done;
    why = "made by another version of the port";
    if (!mods_core_restorable(host, hlen)) goto done;
    why = "for a file that no longer exists";
    if (!rsrc_portable_load(rsrc, rlen, 0) || !file_fcbs_load(fcb, 0)) goto done;
    why = "damaged";
    mem = malloc(MEM_SIZE);
    uLongf mlen = MEM_SIZE;
    if (uncompress(mem, &mlen, z, zlen) != Z_OK || mlen != MEM_SIZE) goto done;
    mods_restore_flat(mem, host);
    rsrc_portable_load(rsrc, rlen, 1);
    file_fcbs_load(fcb, 1);
    ok = 1;
done:
    if (f) fclose(f);
    if (!ok) fprintf(stderr, "[states] %s: not loaded, %s\n", load_path, why);
    free(z); free(host); free(rsrc); free(fcb); free(mem);
    return ok;
}
