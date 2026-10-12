#include "vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

Volume g_vol;

static uint16_t b16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t b32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

int mac_strcasecmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca < 128) ca = tolower(ca);
        if (cb < 128) cb = tolower(cb);
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
}

uint32_t vfs_mac_time_now(void) {
    /* seconds since 1904-01-01 local time; virtual in deterministic mode */
    extern int runtime_deterministic;
    extern uint64_t runtime_us(void);
    if (runtime_deterministic) return 3000000000u + (uint32_t)(runtime_us() / 1000000u);
    time_t t = time(NULL);
    struct tm lt; localtime_r(&t, &lt);
    return (uint32_t)(t + lt.tm_gmtoff + 2082844800u);
}

/* ------------------------------------------------------------------ HFS -- */
typedef struct {
    const uint8_t *img; size_t len;
    uint32_t alblksz, alblst;           /* allocation block size, first block (in 512-byte sectors) */
    uint8_t xt_ext[12]; uint32_t xt_size;
    uint8_t ct_ext[12]; uint32_t ct_size;
    uint8_t *xt; uint8_t *ct;           /* B-tree files read into memory */
} Hfs;

static const uint8_t *ablk(Hfs *h, uint32_t blk) {
    size_t off = (size_t)h->alblst * 512 + (size_t)blk * h->alblksz;
    return off < h->len ? h->img + off : NULL;
}

typedef struct { uint16_t start, count; } Ext;

static int xt_lookup(Hfs *h, uint8_t fork, uint32_t fileid, uint16_t startblk, Ext out[3]);

/* Read a fork given its first extent record; follows the extents-overflow tree. */
static uint8_t *read_fork(Hfs *h, const uint8_t *extrec, uint32_t lglen, uint8_t forktype, uint32_t fileid) {
    uint8_t *buf = malloc(lglen ? lglen : 1);
    uint32_t done = 0, fblk = 0;
    Ext ex[3];
    for (int i = 0; i < 3; i++) { ex[i].start = b16(extrec + i * 4); ex[i].count = b16(extrec + i * 4 + 2); }
    while (done < lglen) {
        int progressed = 0;
        for (int i = 0; i < 3 && done < lglen; i++) {
            for (uint32_t k = 0; k < ex[i].count && done < lglen; k++) {
                const uint8_t *src = ablk(h, ex[i].start + k);
                uint32_t n = lglen - done < h->alblksz ? lglen - done : h->alblksz;
                if (src) memcpy(buf + done, src, n); else memset(buf + done, 0, n);
                done += n; fblk++; progressed = 1;
            }
        }
        if (done >= lglen) break;
        if (!progressed || !h->xt || !xt_lookup(h, forktype, fileid, (uint16_t)fblk, ex)) {
            fprintf(stderr, "hfs: missing extents for file %u\n", fileid);
            memset(buf + done, 0, lglen - done);
            break;
        }
    }
    return buf;
}

static int xt_lookup(Hfs *h, uint8_t fork, uint32_t fileid, uint16_t startblk, Ext out[3]) {
    uint16_t nodesz = b16(h->xt + 14 + 18);
    uint32_t node = b32(h->xt + 14 + 10);     /* first leaf */
    while (node) {
        const uint8_t *nd = h->xt + (size_t)node * nodesz;
        uint16_t nrec = b16(nd + 10);
        for (int r = 0; r < nrec; r++) {
            uint16_t off = b16(nd + nodesz - 2 - r * 2);
            const uint8_t *k = nd + off;
            if (k[1] == fork && b32(k + 2) == fileid && b16(k + 6) == startblk) {
                const uint8_t *d = k + 1 + k[0]; if ((d - nd) & 1) d++;
                for (int i = 0; i < 3; i++) { out[i].start = b16(d + i * 4); out[i].count = b16(d + i * 4 + 2); }
                return 1;
            }
        }
        node = b32(nd);
    }
    return 0;
}

static VNode *new_node(void) {
    VNode *n = calloc(1, sizeof *n);
    n->next = g_vol.nodes; g_vol.nodes = n;
    return n;
}

static int mount_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno)); return -1; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *img = malloc((size_t)len);
    if (fread(img, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(img); return -1; }
    fclose(f);
    Hfs h = { img, (size_t)len };
    const uint8_t *mdb = img + 1024;
    if (len < 2048 || b16(mdb) != 0x4244) { fprintf(stderr, "%s: not an HFS volume\n", path); free(img); return -1; }
    g_vol.crdate = b32(mdb + 2);
    h.alblksz = b32(mdb + 0x14);
    h.alblst = b16(mdb + 0x1C);
    memcpy(g_vol.volname, mdb + 0x25, mdb[0x24] < 27 ? mdb[0x24] : 27);
    g_vol.next_cnid = b32(mdb + 0x1E);
    h.xt_size = b32(mdb + 0x82); memcpy(h.xt_ext, mdb + 0x86, 12);
    h.ct_size = b32(mdb + 0x92); memcpy(h.ct_ext, mdb + 0x96, 12);
    h.xt = read_fork(&h, h.xt_ext, h.xt_size, 0, 3);
    h.ct = read_fork(&h, h.ct_ext, h.ct_size, 0, 4);
    uint16_t nodesz = b16(h.ct + 14 + 18);
    uint32_t node = b32(h.ct + 14 + 10);
    int files = 0;
    while (node) {
        const uint8_t *nd = h.ct + (size_t)node * nodesz;
        uint16_t nrec = b16(nd + 10);
        for (int r = 0; r < nrec; r++) {
            uint16_t off = b16(nd + nodesz - 2 - r * 2);
            const uint8_t *k = nd + off;
            uint8_t klen = k[0];
            if (!klen) continue;
            uint32_t parid = b32(k + 2);
            uint8_t nlen = k[6];
            const uint8_t *d = k + 1 + klen; if ((d - nd) & 1) d++;
            uint8_t type = d[0];
            if (type != 1 && type != 2) continue;
            VNode *n = new_node();
            n->parid = parid;
            memcpy(n->name, k + 7, nlen < 31 ? nlen : 31);
            if (type == 1) {
                n->isdir = 1;
                n->cnid = b32(d + 6);
                n->crdate = b32(d + 10); n->mddate = b32(d + 14);
                memcpy(n->finfo, d + 22, 16); memcpy(n->fxinfo, d + 38, 16);
                if (n->cnid == 2) strcpy(n->name, g_vol.volname);
            } else {
                n->locked = d[2] & 1;
                memcpy(n->finfo, d + 4, 16);
                n->cnid = b32(d + 20);
                uint32_t dlen = b32(d + 26), rlen = b32(d + 36);
                n->crdate = b32(d + 44); n->mddate = b32(d + 48);
                memcpy(n->fxinfo, d + 56, 16);
                n->fork[0] = read_fork(&h, d + 74, dlen, 0x00, n->cnid); n->forklen[0] = n->forkcap[0] = dlen;
                n->fork[1] = read_fork(&h, d + 86, rlen, 0xFF, n->cnid); n->forklen[1] = n->forkcap[1] = rlen;
                files++;
            }
        }
        node = b32(nd);
    }
    free(h.xt); free(h.ct); free(img);
    fprintf(stderr, "vfs: mounted \"%s\" (%d files)\n", g_vol.volname, files);
    return 0;
}

/* ---------------------------------------------------------------- lookup -- */
VNode *vfs_by_id(uint32_t cnid) {
    for (VNode *n = g_vol.nodes; n; n = n->next) if (n->cnid == cnid && !n->deleted) return n;
    return NULL;
}
VNode *vfs_lookup(uint32_t parid, const char *name) {
    for (VNode *n = g_vol.nodes; n; n = n->next)
        if (!n->deleted && n->parid == parid && n->cnid != 2 && !mac_strcasecmp(n->name, name)) return n;
    return NULL;
}

/* Sorted children (HFS catalog order is by name). */
static int cmp_nodes(const void *a, const void *b) {
    return mac_strcasecmp((*(VNode *const *)a)->name, (*(VNode *const *)b)->name);
}
static int children(uint32_t parid, VNode ***out) {
    int cnt = 0;
    for (VNode *n = g_vol.nodes; n; n = n->next) if (!n->deleted && n->parid == parid && n->cnid != 2) cnt++;
    VNode **arr = malloc(sizeof(VNode *) * (cnt ? cnt : 1));
    int i = 0;
    for (VNode *n = g_vol.nodes; n; n = n->next) if (!n->deleted && n->parid == parid && n->cnid != 2) arr[i++] = n;
    qsort(arr, cnt, sizeof(VNode *), cmp_nodes);
    *out = arr;
    return cnt;
}
int vfs_child_count(uint32_t parid) { VNode **a; int c = children(parid, &a); free(a); return c; }
VNode *vfs_child_index(uint32_t parid, int index) {
    VNode **a; int c = children(parid, &a);
    VNode *r = (index >= 1 && index <= c) ? a[index - 1] : NULL;
    free(a);
    return r;
}

/* Resolve a Mac path relative to dirid. Handles "Vol:dir:file", ":dir:file", "file", "::up".
 * Returns the node (or NULL); out_parid/out_leaf describe where it would be. */
VNode *vfs_resolve_path(uint32_t dirid, const char *path, uint32_t *out_parid, char *out_leaf) {
    char buf[512]; strncpy(buf, path, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    uint32_t cur = dirid ? dirid : 2;
    char *p = buf;
    if (*p && *p != ':' && strchr(p, ':')) {          /* absolute: volume name first */
        char *c = strchr(p, ':'); *c = 0;
        cur = 2; p = c + 1;
    } else if (*p == ':') p++;
    VNode *node = vfs_by_id(cur);
    char leaf[64] = "";
    uint32_t parid = cur;
    while (*p) {
        char *c = strchr(p, ':');
        size_t l = c ? (size_t)(c - p) : strlen(p);
        if (l == 0) {                 /* "::" = parent */
            VNode *d = vfs_by_id(cur); cur = d && d->cnid != 2 ? d->parid : 2;
            node = vfs_by_id(cur); parid = node ? node->parid : 1;
            p = c ? c + 1 : p + l; continue;
        }
        if (l > 31) l = 31;
        memcpy(leaf, p, l); leaf[l] = 0;
        parid = cur;
        node = vfs_lookup(cur, leaf);
        if (!c) break;
        if (!node || !node->isdir) { node = NULL; break; }
        cur = node->cnid;
        p = c + 1;
        if (!*p) { leaf[0] = 0; break; }
    }
    if (out_parid) *out_parid = node && !*leaf ? node->parid : parid;
    if (out_leaf) strcpy(out_leaf, leaf);
    return node;
}

/* --------------------------------------------------------------- overlay -- */
static void host_path(VNode *n, char *out, size_t sz) {
    char parts[16][32]; int np = 0;
    for (VNode *c = n; c && c->cnid != 2 && np < 16; c = vfs_by_id(c->parid)) strcpy(parts[np++], c->name);
    snprintf(out, sz, "%s", g_vol.overlay_dir);
    for (int i = np - 1; i >= 0; i--) {
        size_t l = strlen(out);
        snprintf(out + l, sz - l, "/%s", parts[i]);
        for (char *q = out + l + 1; *q; q++) if (*q == '/') *q = ':';
    }
}
static void mkdirs(const char *path) {
    char tmp[1024]; strncpy(tmp, path, sizeof tmp - 1); tmp[sizeof tmp - 1] = 0;
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}
static void write_file(const char *path, const uint8_t *d, size_t n) {
    FILE *f = fopen(path, "wb"); if (!f) { fprintf(stderr, "vfs: cannot write %s\n", path); return; }
    fwrite(d, 1, n, f); fclose(f);
}
void vfs_flush(VNode *n) {
    if (!n->dirty || !g_vol.overlay_dir[0]) return;
    char p[1024], q[1100];
    host_path(n, p, sizeof p);
    if (n->deleted) {
        snprintf(q, sizeof q, "%s.deleted", p); write_file(q, (const uint8_t *)"", 0);
        remove(p); snprintf(q, sizeof q, "%s.rsrc", p); remove(q);
        n->dirty = 0; return;
    }
    char dir[1024]; strcpy(dir, p); char *s = strrchr(dir, '/'); if (s) *s = 0; mkdirs(dir);
    snprintf(q, sizeof q, "%s.deleted", p); remove(q);
    if (n->isdir) { mkdirs(p); n->dirty = 0; return; }
    write_file(p, n->fork[0], n->forklen[0]);
    snprintf(q, sizeof q, "%s.rsrc", p); write_file(q, n->fork[1], n->forklen[1]);
    uint8_t fi[32]; memcpy(fi, n->finfo, 16); memcpy(fi + 16, n->fxinfo, 16);
    snprintf(q, sizeof q, "%s.finf", p); write_file(q, fi, 32);
    n->dirty = 0;
}
void vfs_flush_all(void) { for (VNode *n = g_vol.nodes; n; n = n->next) vfs_flush(n); }

static uint8_t *slurp(const char *path, uint32_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(l ? (size_t)l : 1);
    *len = (uint32_t)fread(b, 1, (size_t)l, f); fclose(f);
    return b;
}
static int has_suffix(const char *s, const char *suf) { size_t a = strlen(s), b = strlen(suf); return a > b && !strcmp(s + a - b, suf); }

static void load_overlay_dir(const char *hostdir, uint32_t parid) {
    DIR *d = opendir(hostdir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (has_suffix(e->d_name, ".rsrc") || has_suffix(e->d_name, ".finf")) continue;
        char hp[1100]; snprintf(hp, sizeof hp, "%s/%s", hostdir, e->d_name);
        char mname[64]; strncpy(mname, e->d_name, 63); mname[63] = 0;
        for (char *q = mname; *q; q++) if (*q == ':') *q = '/';
        int del = has_suffix(mname, ".deleted");
        if (del) mname[strlen(mname) - 8] = 0;
        VNode *n = vfs_lookup(parid, mname);
        if (del) { if (n) n->deleted = 1; continue; }
        struct stat st; stat(hp, &st);
        if (S_ISDIR(st.st_mode)) {
            if (!n) { n = new_node(); n->isdir = 1; n->parid = parid; n->cnid = g_vol.next_cnid++; strncpy(n->name, mname, 31); }
            load_overlay_dir(hp, n->cnid);
            continue;
        }
        if (!n) { n = new_node(); n->parid = parid; n->cnid = g_vol.next_cnid++; strncpy(n->name, mname, 31); n->crdate = vfs_mac_time_now(); }
        free(n->fork[0]); free(n->fork[1]);
        n->fork[0] = slurp(hp, &n->forklen[0]); n->forkcap[0] = n->forklen[0];
        char q[1200]; snprintf(q, sizeof q, "%s.rsrc", hp);
        n->fork[1] = slurp(q, &n->forklen[1]); n->forkcap[1] = n->forklen[1];
        if (!n->fork[1]) { n->fork[1] = malloc(1); n->forklen[1] = n->forkcap[1] = 0; }
        uint32_t fl; snprintf(q, sizeof q, "%s.finf", hp);
        uint8_t *fi = slurp(q, &fl);
        if (fi && fl >= 32) { memcpy(n->finfo, fi, 16); memcpy(n->fxinfo, fi + 16, 16); }
        free(fi);
        n->mddate = (uint32_t)(st.st_mtime + 2082844800u);
    }
    closedir(d);
}

int vfs_mount_hfs(const char *image, const char *overlay) {
    memset(&g_vol, 0, sizeof g_vol);
    if (mount_image(image)) return -1;
    if (overlay) {
        strncpy(g_vol.overlay_dir, overlay, sizeof g_vol.overlay_dir - 1);
        mkdirs(overlay);
        load_overlay_dir(overlay, 2);
    }
    return 0;
}

/* --------------------------------------------------------------- changes -- */
VNode *vfs_create(uint32_t parid, const char *name, int isdir) {
    VNode *n = vfs_lookup(parid, name);
    if (n) return NULL;
    n = new_node();
    n->parid = parid; n->cnid = g_vol.next_cnid++; n->isdir = isdir;
    strncpy(n->name, name, 31);
    n->crdate = n->mddate = vfs_mac_time_now();
    if (!isdir) { n->fork[0] = malloc(1); n->fork[1] = malloc(1); }
    n->dirty = 1;
    vfs_flush(n);
    return n;
}
int vfs_delete(VNode *n) {
    if (n->isdir && vfs_child_count(n->cnid)) return -47;   /* fBsyErr */
    n->deleted = 1; n->dirty = 1; vfs_flush(n);
    return 0;
}
int vfs_rename(VNode *n, const char *newname) {
    if (vfs_lookup(n->parid, newname)) return -48;          /* dupFNErr */
    /* mark the old path deleted in the overlay, then write under the new name */
    VNode tomb = *n; tomb.deleted = 1; tomb.dirty = 1; vfs_flush(&tomb);
    strncpy(n->name, newname, 31); n->name[31] = 0;
    n->dirty = 1; vfs_flush(n);
    return 0;
}
void vfs_set_fork_len(VNode *n, int fork, uint32_t len) {
    if (len > n->forkcap[fork]) {
        uint32_t cap = len + len / 2 + 256;
        n->fork[fork] = realloc(n->fork[fork], cap);
        memset(n->fork[fork] + n->forkcap[fork], 0, cap - n->forkcap[fork]);
        n->forkcap[fork] = cap;
    }
    if (len > n->forklen[fork]) memset(n->fork[fork] + n->forklen[fork], 0, len - n->forklen[fork]);
    n->forklen[fork] = len;
    n->dirty = 1;
}
