/* Resource Manager */
#include "mac.h"
#include "../fs/vfs.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int16_t id; uint8_t attrs; char name[256]; int hasname;
    uint8_t *data; uint32_t len;          /* host copy (from file or written) */
    maddr handle;                         /* master pointer when loaded / detached-not */
} Ref;
typedef struct { uint32_t type; Ref *refs; int nrefs, cap; } RType;
typedef struct ResFile {
    int16_t refnum;
    VNode *node;
    RType *types; int ntypes;
    uint16_t attrs;
    int changed;
    struct ResFile *next;                 /* chain: newer files first */
} ResFile;

static ResFile *chain;
static int16_t next_refnum = 2;           /* odd/even doesn't matter; positive */
static ResFile *sysfile;

static void set_reserr(int16_t e) { wr16(LM_ResErr, (uint16_t)e); }
int16_t ResError(void) { return (int16_t)rd16(LM_ResErr); }

static ResFile *file_by_ref(int16_t r) { for (ResFile *f = chain; f; f = f->next) if (f->refnum == r) return f; return NULL; }
int16_t CurResFile(void) { return (int16_t)rd16(LM_CurMap); }

static RType *get_type(ResFile *f, uint32_t type, int create) {
    for (int i = 0; i < f->ntypes; i++) if (f->types[i].type == type) return &f->types[i];
    if (!create) return NULL;
    f->types = realloc(f->types, sizeof(RType) * (f->ntypes + 1));
    RType *t = &f->types[f->ntypes++];
    memset(t, 0, sizeof *t); t->type = type;
    return t;
}
static Ref *add_ref(RType *t) {
    if (t->nrefs == t->cap) { t->cap = t->cap ? t->cap * 2 : 8; t->refs = realloc(t->refs, sizeof(Ref) * t->cap); }
    Ref *r = &t->refs[t->nrefs++]; memset(r, 0, sizeof *r);
    return r;
}

static int parse_map(ResFile *f, const uint8_t *d, uint32_t len) {
    if (len < 16) return 0;
    uint32_t doff = be32(d), moff = be32(d + 4), dlen = be32(d + 8), mlen = be32(d + 12);
    if (moff + mlen > len || doff + dlen > len || mlen < 30) return -1;
    const uint8_t *m = d + moff;
    f->attrs = be16(m + 22);
    uint16_t tlo = be16(m + 24), nlo = be16(m + 26);
    int ntypes = (int16_t)be16(m + 28) + 1;
    for (int i = 0; i < ntypes; i++) {
        const uint8_t *te = m + tlo + 2 + i * 8;
        RType *t = get_type(f, be32(te), 1);
        int cnt = be16(te + 4) + 1;
        uint16_t roff = be16(te + 6);
        for (int j = 0; j < cnt; j++) {
            const uint8_t *e = m + tlo + roff + j * 12;
            Ref *r = add_ref(t);
            r->id = (int16_t)be16(e);
            uint16_t noff = be16(e + 2);
            r->attrs = e[4];
            uint32_t off = (uint32_t)e[5] << 16 | (uint32_t)e[6] << 8 | e[7];
            uint32_t rl = be32(d + doff + off);
            r->len = rl;
            r->data = malloc(rl ? rl : 1);
            memcpy(r->data, d + doff + off + 4, rl);
            if (noff != 0xFFFF) {
                uint8_t nl = m[nlo + noff];
                memcpy(r->name, m + nlo + noff + 1, nl); r->name[nl] = 0; r->hasname = 1;
            }
        }
    }
    return 0;
}

int16_t rsrc_open_node(VNode *n, int8_t perm) {
    (void)perm;
    for (ResFile *f = chain; f; f = f->next) if (f->node == n) { wr16(LM_CurMap, (uint16_t)f->refnum); set_reserr(0); return f->refnum; }
    ResFile *f = calloc(1, sizeof *f);
    f->node = n;
    if (n->forklen[1] && parse_map(f, n->fork[1], n->forklen[1])) {
        free(f); set_reserr(-199); return -1;     /* mapReadErr */
    }
    if (!n->forklen[1]) { free(f); set_reserr(-193); return -1; }  /* resFNotFound */
    f->refnum = fm_alloc_fcb(n, 1);
    if (f->refnum < 0) { free(f); set_reserr(-42); return -1; }
    f->next = chain; chain = f;
    { extern void font_flush_cache(void); font_flush_cache(); }
    wr16(LM_CurMap, (uint16_t)f->refnum);
    wr32(LM_TopMapHndl, 0x10000 | (uint16_t)f->refnum);  /* opaque */
    set_reserr(0);
    return f->refnum;
}

/* A synthetic System file holding resources the game expects from the System. */
int16_t rsrc_open_system(const uint8_t *data, uint32_t len) {
    static VNode sysnode;
    strcpy(sysnode.name, "System");
    sysnode.fork[1] = (uint8_t *)data; sysnode.forklen[1] = len;
    ResFile *f = calloc(1, sizeof *f);
    f->node = &sysnode;
    if (len) parse_map(f, data, len);
    f->refnum = 0;     /* SysMap is 0 in practice when System file refnum is 0? use 2 */
    f->refnum = 2;
    next_refnum = 3;
    /* system file is always at the end of the chain */
    f->next = NULL;
    if (!chain) chain = f; else { ResFile *t = chain; while (t->next) t = t->next; t->next = f; }
    sysfile = f;
    wr16(LM_SysMap, 2);
    wr16(LM_CurMap, 2);
    return 2;
}

void rsrc_add_system(uint32_t type, int16_t id, const char *name, const void *data, uint32_t len) {
    RType *t = get_type(sysfile, type, 1);
    Ref *r = add_ref(t);
    r->id = id; r->len = len; r->data = malloc(len ? len : 1); memcpy(r->data, data, len);
    if (name) { strncpy(r->name, name, 255); r->hasname = 1; }
}

void UseResFile(int16_t refnum) {
    if (refnum == 0) refnum = 2;
    if (!file_by_ref(refnum)) { set_reserr(-193); return; }
    wr16(LM_CurMap, (uint16_t)refnum); set_reserr(0);
}

/* Search order: current file, then the files opened before it (further down the chain). */
static ResFile *search_start(void) {
    int16_t cur = CurResFile();
    ResFile *f = file_by_ref(cur);
    return f ? f : chain;
}

static int is_purged(Ref *r) { return r->handle && rd32(r->handle) == 0; }

static maddr load_ref(ResFile *f, Ref *r) {
    int sys = (f == sysfile);
    if (r->handle && !is_purged(r)) return r->handle;
    if (!rd8(LM_ResLoad) && !r->handle) {
        r->handle = sys ? NewHandleSys(0) : NewEmptyHandle();
        if (rd32(r->handle)) EmptyHandle(r->handle);
        HSetRBit(r->handle);
        return r->handle;
    }
    if (r->handle) {                       /* reload a purged resource into the same master pointer */
        SetHandleSize(r->handle, r->len);
        if (!rd32(r->handle)) { set_reserr(-108); return r->handle; }
    } else {
        maddr h = sys ? NewHandleSys(r->len) : NewHandle(r->len);
        if (!h) { set_reserr(-108); return 0; }
        r->handle = h;
    }
    mput(rd32(r->handle), r->data, r->len);
    HSetRBit(r->handle);
    if (r->attrs & 0x20) HPurge(r->handle);
    if (r->attrs & 0x10) HLock(r->handle);
    return r->handle;
}

static Ref *find_ref(ResFile *f, uint32_t type, int16_t id) {
    RType *t = get_type(f, type, 0);
    if (!t) return NULL;
    for (int i = 0; i < t->nrefs; i++) if (t->refs[i].id == id && t->refs[i].len != 0xFFFFFFFFu) return &t->refs[i];
    return NULL;
}

void (*rsrc_missing_hook)(uint32_t type, int16_t id);

static maddr get_resource(uint32_t type, int16_t id, int one) {
    {   /* debug: POP2_HIDE_RES=TYPE hides every resource of that type */
        static uint32_t hide = 1;
        if (hide == 1) { const char *e = getenv("POP2_HIDE_RES"); hide = (e && strlen(e) == 4) ? (uint32_t)e[0] << 24 | (uint32_t)e[1] << 16 | (uint32_t)e[2] << 8 | (uint8_t)e[3] : 0; }
        if (hide && type == hide) { set_reserr(-192); return 0; }
    }
    for (ResFile *f = search_start(); f; f = one ? NULL : f->next) {
        Ref *r = find_ref(f, type, id);
        if (r) { set_reserr(0); return load_ref(f, r); }
    }
    if (rsrc_missing_hook) rsrc_missing_hook(type, id);
    set_reserr(-192);   /* resNotFound */
    return 0;
}
maddr GetResource(uint32_t type, int16_t id) { return get_resource(type, id, 0); }
maddr Get1Resource(uint32_t type, int16_t id) { return get_resource(type, id, 1); }

static maddr get_named(uint32_t type, const char *name, int one) {
    for (ResFile *f = search_start(); f; f = one ? NULL : f->next) {
        RType *t = get_type(f, type, 0);
        if (!t) continue;
        for (int i = 0; i < t->nrefs; i++)
            if (t->refs[i].hasname && !mac_strcasecmp(t->refs[i].name, name)) { set_reserr(0); return load_ref(f, &t->refs[i]); }
    }
    set_reserr(-192);
    return 0;
}
maddr GetNamedResource(uint32_t type, const char *name) { return get_named(type, name, 0); }
maddr Get1NamedResource(uint32_t type, const char *name) { return get_named(type, name, 1); }

static int count_res(uint32_t type, int one, ResFile **fs, Ref **rs, int idx) {
    int n = 0;
    for (ResFile *f = search_start(); f; f = one ? NULL : f->next) {
        RType *t = get_type(f, type, 0);
        if (!t) continue;
        for (int i = 0; i < t->nrefs; i++) {
            if (t->refs[i].len == 0xFFFFFFFFu) continue;
            n++;
            if (idx == n && fs) { *fs = f; *rs = &t->refs[i]; return n; }
        }
    }
    return n;
}
int16_t CountResources(uint32_t type) { return (int16_t)count_res(type, 0, NULL, NULL, 0); }
int16_t Count1Resources(uint32_t type) { return (int16_t)count_res(type, 1, NULL, NULL, 0); }
static maddr get_ind(uint32_t type, int16_t idx, int one) {
    ResFile *f; Ref *r;
    if (idx < 1 || count_res(type, one, &f, &r, idx) < idx) { set_reserr(-192); return 0; }
    set_reserr(0);
    return load_ref(f, r);
}
maddr GetIndResource(uint32_t type, int16_t idx) { return get_ind(type, idx, 0); }
maddr Get1IndResource(uint32_t type, int16_t idx) { return get_ind(type, idx, 1); }

static int count_types(int one, uint32_t *out, int idx) {
    uint32_t seen[512]; int n = 0;
    for (ResFile *f = search_start(); f; f = one ? NULL : f->next)
        for (int i = 0; i < f->ntypes; i++) {
            int dup = 0;
            for (int k = 0; k < n; k++) if (seen[k] == f->types[i].type) dup = 1;
            if (!dup && n < 512) { seen[n++] = f->types[i].type; if (n == idx && out) { *out = seen[n - 1]; return n; } }
        }
    return n;
}
int16_t CountTypes(void) { return (int16_t)count_types(0, NULL, 0); }
int16_t Count1Types(void) { return (int16_t)count_types(1, NULL, 0); }
uint32_t GetIndType(int16_t idx) { uint32_t t = 0; count_types(0, &t, idx); return t; }
uint32_t Get1IndType(int16_t idx) { uint32_t t = 0; count_types(1, &t, idx); return t; }

/* handle -> (file, ref) */
static int locate(maddr h, ResFile **fo, RType **to, Ref **ro) {
    if (!h) return 0;
    for (ResFile *f = chain; f; f = f->next)
        for (int i = 0; i < f->ntypes; i++)
            for (int j = 0; j < f->types[i].nrefs; j++)
                if (f->types[i].refs[j].handle == (h & ADDR_MASK)) {
                    if (fo) *fo = f; if (to) *to = &f->types[i]; if (ro) *ro = &f->types[i].refs[j];
                    return 1;
                }
    return 0;
}

void LoadResource(maddr h) {
    ResFile *f; Ref *r;
    if (!locate(h, &f, NULL, &r)) { set_reserr(-192); return; }
    if (!rd32(h)) { maddr save = rd8(LM_ResLoad); wr8(LM_ResLoad, 1); r->handle = h; load_ref(f, r); wr8(LM_ResLoad, (uint8_t)save); }
    set_reserr(0);
}
void ReleaseResource(maddr h) {
    Ref *r;
    if (!locate(h, NULL, NULL, &r)) { set_reserr(-192); return; }
    DisposeHandle(h); r->handle = 0; set_reserr(0);
}
void DetachResource(maddr h) {
    Ref *r;
    if (!locate(h, NULL, NULL, &r)) { set_reserr(-192); return; }
    HClrRBit(h); r->handle = 0; set_reserr(0);
}
void GetResInfo(maddr h, int16_t *id, uint32_t *type, char *name) {
    RType *t; Ref *r;
    if (!locate(h, NULL, &t, &r)) { set_reserr(-192); return; }
    if (id) *id = r->id; if (type) *type = t->type; if (name) strcpy(name, r->hasname ? r->name : "");
    set_reserr(0);
}
/* debug: type/id of a resource handle, without touching ResErr */
int rsrc_describe(maddr h, uint32_t *type, int16_t *id) {
    RType *t; Ref *r;
    if (!locate(h, NULL, &t, &r)) return 0;
    *type = t->type; *id = r->id; return 1;
}
int16_t GetResAttrs(maddr h) { Ref *r; if (!locate(h, NULL, NULL, &r)) { set_reserr(-192); return 0; } set_reserr(0); return r->attrs; }
void SetResAttrs(maddr h, int16_t a) { Ref *r; if (!locate(h, NULL, NULL, &r)) { set_reserr(-192); return; } r->attrs = (uint8_t)a; set_reserr(0); }
int32_t GetResourceSizeOnDisk(maddr h) { Ref *r; if (!locate(h, NULL, NULL, &r)) { set_reserr(-192); return -1; } set_reserr(0); return (int32_t)r->len; }
int16_t HomeResFile(maddr h) { ResFile *f; if (!locate(h, &f, NULL, NULL)) { set_reserr(-192); return -1; } set_reserr(0); return f->refnum; }
void SetResInfo(maddr h, int16_t id, const char *name) {
    Ref *r; if (!locate(h, NULL, NULL, &r)) { set_reserr(-192); return; }
    r->id = id; if (name) { strncpy(r->name, name, 255); r->hasname = 1; }
    set_reserr(0);
}

void ChangedResource(maddr h) {
    ResFile *f; Ref *r;
    if (!locate(h, &f, NULL, &r)) { set_reserr(-192); return; }
    r->attrs |= 0x02; f->changed = 1; set_reserr(0);
}

void AddResource(maddr h, uint32_t type, int16_t id, const char *name) {
    ResFile *f = search_start();
    if (!f || f == sysfile) { set_reserr(-193); return; }
    Ref *r = add_ref(get_type(f, type, 1));
    r->id = id; r->handle = h & ADDR_MASK; r->attrs = 0x02;
    if (name && *name) { strncpy(r->name, name, 255); r->hasname = 1; }
    r->len = GetHandleSize(h); r->data = malloc(r->len ? r->len : 1); mget(r->data, rd32(h), r->len);
    HSetRBit(h);
    f->changed = 1; set_reserr(0);
}

void RemoveResource(maddr h) {
    ResFile *f; RType *t; Ref *r;
    if (!locate(h, &f, &t, &r)) { set_reserr(-192); return; }
    HClrRBit(h);
    free(r->data);
    *r = t->refs[--t->nrefs];
    f->changed = 1; set_reserr(0);
}

static void sync_data(ResFile *f) {
    for (int i = 0; i < f->ntypes; i++)
        for (int j = 0; j < f->types[i].nrefs; j++) {
            Ref *r = &f->types[i].refs[j];
            if ((r->attrs & 0x02) && r->handle && rd32(r->handle)) {
                uint32_t l = GetHandleSize(r->handle);
                free(r->data); r->data = malloc(l ? l : 1); r->len = l;
                mget(r->data, rd32(r->handle), l);
            }
        }
}

void WriteResource(maddr h) {
    ResFile *f; Ref *r;
    if (!locate(h, &f, NULL, &r)) { set_reserr(-192); return; }
    if (r->handle && rd32(r->handle)) {
        uint32_t l = GetHandleSize(r->handle);
        free(r->data); r->data = malloc(l ? l : 1); r->len = l; mget(r->data, rd32(r->handle), l);
    }
    f->changed = 1; set_reserr(0);
}

/* Serialize the map back into the node's resource fork. */
static void write_file(ResFile *f) {
    sync_data(f);
    uint32_t dlen = 0, nlen = 0; int nt = 0, nr = 0;
    for (int i = 0; i < f->ntypes; i++) {
        if (!f->types[i].nrefs) continue;
        nt++;
        for (int j = 0; j < f->types[i].nrefs; j++) {
            Ref *r = &f->types[i].refs[j];
            nr++; dlen += 4 + r->len;
            if (r->hasname) nlen += 1 + (uint32_t)strlen(r->name);
        }
    }
    uint32_t tl = 2 + nt * 8, rl = nr * 12;
    uint32_t mlen = 28 + 2 + tl - 2 + rl + nlen;
    mlen = 30 + nt * 8 + rl + nlen;
    uint32_t total = 256 + dlen + mlen;
    uint8_t *b = calloc(1, total);
    put32(b, 256); put32(b + 4, 256 + dlen); put32(b + 8, dlen); put32(b + 12, mlen);
    uint8_t *m = b + 256 + dlen;
    memcpy(m, b, 16);
    put16(m + 22, f->attrs & ~0x0020);
    put16(m + 24, 28); put16(m + 26, (uint16_t)(30 + nt * 8 + rl));
    put16(m + 28, (uint16_t)(nt - 1));
    uint32_t doff = 0, noff = 0, roff = 2 + nt * 8;
    int ti = 0;
    for (int i = 0; i < f->ntypes; i++) {
        RType *t = &f->types[i];
        if (!t->nrefs) continue;
        uint8_t *te = m + 30 + ti * 8;
        put32(te, t->type); put16(te + 4, (uint16_t)(t->nrefs - 1)); put16(te + 6, (uint16_t)roff);
        for (int j = 0; j < t->nrefs; j++) {
            Ref *r = &t->refs[j];
            uint8_t *e = m + 28 + roff + j * 12;
            put16(e, (uint16_t)r->id);
            if (r->hasname) {
                put16(e + 2, (uint16_t)noff);
                uint8_t *np = m + 30 + nt * 8 + rl + noff;
                np[0] = (uint8_t)strlen(r->name); memcpy(np + 1, r->name, np[0]);
                noff += 1 + np[0];
            } else put16(e + 2, 0xFFFF);
            r->attrs &= ~0x02;
            e[4] = r->attrs; e[5] = doff >> 16; e[6] = doff >> 8; e[7] = (uint8_t)doff;
            put32(b + 256 + doff, r->len); memcpy(b + 256 + doff + 4, r->data, r->len);
            doff += 4 + r->len;
        }
        roff += t->nrefs * 12;
        ti++;
    }
    VNode *n = f->node;
    vfs_set_fork_len(n, 1, total);
    memcpy(n->fork[1], b, total);
    n->mddate = vfs_mac_time_now();
    n->dirty = 1;
    vfs_flush(n);
    free(b);
    f->changed = 0;
}

void UpdateResFile(int16_t refnum) {
    ResFile *f = file_by_ref(refnum ? refnum : 2);
    if (!f) { set_reserr(-193); return; }
    if (f->changed && f != sysfile) write_file(f);
    set_reserr(0);
}

void CloseResFile(int16_t refnum) {
    ResFile *f = file_by_ref(refnum);
    if (!f || f == sysfile) { set_reserr(f ? 0 : -193); return; }
    if (f->changed) write_file(f);
    for (int i = 0; i < f->ntypes; i++) {
        for (int j = 0; j < f->types[i].nrefs; j++) {
            Ref *r = &f->types[i].refs[j];
            if (r->handle) DisposeHandle(r->handle);
            free(r->data);
        }
        free(f->types[i].refs);
    }
    free(f->types);
    ResFile **pp = &chain; while (*pp != f) pp = &(*pp)->next; *pp = f->next;
    fm_free_fcb(f->refnum);
    if (CurResFile() == refnum) wr16(LM_CurMap, (uint16_t)(chain ? chain->refnum : 2));
    free(f);
    set_reserr(0);
}

void CreateResFile_node(VNode *n) {
    static const uint8_t empty[] = {
        0,0,1,0, 0,0,1,0, 0,0,0,0, 0,0,0,0x1E };
    uint8_t buf[256 + 30]; memset(buf, 0, sizeof buf);
    memcpy(buf, empty, 16);
    memcpy(buf + 256, empty, 16);
    put16(buf + 256 + 24, 28); put16(buf + 256 + 26, 30); put16(buf + 256 + 28, 0xFFFF);
    vfs_set_fork_len(n, 1, sizeof buf);
    memcpy(n->fork[1], buf, sizeof buf);
    n->dirty = 1; vfs_flush(n);
}

void SetResLoad(int load) { wr8(LM_ResLoad, load ? 1 : 0); }
uint16_t GetResFileAttrs(int16_t refnum) { ResFile *f = file_by_ref(refnum); return f ? f->attrs : 0; }
void SetResFileAttrs(int16_t refnum, uint16_t a) { ResFile *f = file_by_ref(refnum); if (f) f->attrs = a; }
void SetResPurge(int install) { (void)install; }
int32_t rsrc_size(maddr h) { Ref *r; return locate(h, NULL, NULL, &r) ? (int32_t)r->len : -1; }

/* save states (src/mac/mods.c): the handle and attributes of every resource reference.
 * The file/type/reference structure itself must be unchanged for a restore. */
static void rsrc_walk(int (*fn)(Ref *r, void *ctx), void *ctx) {
    for (ResFile *f = chain; f; f = f->next)
        for (int t = 0; t < f->ntypes; t++)
            for (int k = 0; k < f->types[t].nrefs; k++) if (fn(&f->types[t].refs[k], ctx)) return;
}
static int count_ref(Ref *r, void *ctx) { (void)r; (*(size_t *)ctx)++; return 0; }
size_t rsrc_state_size(void) { size_t n = 0; rsrc_walk(count_ref, &n); return sizeof(uint32_t) + n * 5; }
typedef struct { uint8_t *p; } RsCur;
static int save_ref(Ref *r, void *ctx) { RsCur *c = ctx; put32(c->p, r->handle); c->p[4] = r->attrs; c->p += 5; return 0; }
static int restore_ref(Ref *r, void *ctx) { RsCur *c = ctx; r->handle = be32(c->p); r->attrs = c->p[4]; c->p += 5; return 0; }
void rsrc_state_save(uint8_t *out) {
    size_t n = 0; rsrc_walk(count_ref, &n);
    put32(out, (uint32_t)n);
    RsCur c = { out + 4 }; rsrc_walk(save_ref, &c);
}
int rsrc_state_load(const uint8_t *in) {
    size_t n = 0; rsrc_walk(count_ref, &n);
    if (be32(in) != n) return 0;                 /* a resource file was opened or closed */
    RsCur c = { (uint8_t *)in + 4 }; rsrc_walk(restore_ref, &c);
    return 1;
}
