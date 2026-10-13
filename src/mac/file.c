/* File Manager (PB calls, working directories, FSSpec) over the VFS. */
#include "mac.h"
#include "../fs/vfs.h"
#include <stdio.h>
#include <stdlib.h>

#define VREFNUM   (-1)
#define WD_BASE   (-32000)

typedef struct { VNode *node; int fork; uint32_t pos; int8_t perm; int used; } Fcb;
#define MAX_FCB 64
static Fcb fcbs[MAX_FCB];
#define FREF(i) ((int16_t)(1000 + 2 * (i)))

typedef struct { uint32_t dirid; int32_t procid; int used; } Wd;
static Wd wds[64];
static uint32_t default_dir = 2;

/* pb field offsets */
enum { ioCompletion = 12, ioResult = 16, ioNamePtr = 18, ioVRefNum = 22, ioRefNum = 24,
       ioPermssn = 27, ioMisc = 28, ioBuffer = 32, ioReqCount = 36, ioActCount = 40,
       ioPosMode = 44, ioPosOffset = 46,
       ioFDirIndex = 28, ioFlAttrib = 30, ioFlFndrInfo = 32, ioDirID = 48, ioFlLgLen = 54,
       ioFlPyLen = 58, ioFlRLgLen = 64, ioFlRPyLen = 68, ioFlCrDat = 72, ioFlMdDat = 76,
       ioFlBkDat = 80, ioFlXFndrInfo = 84, ioFlParID = 100, ioFlClpSiz = 104,
       ioDrNmFls = 52,
       ioVolIndex = 28, ioWDIndex = 26, ioWDProcID = 28, ioWDVRefNum = 32, ioWDDirID = 48 };

uint32_t fm_default_dir(void) { return default_dir; }
void fm_set_default_dir(uint32_t d) { default_dir = d; }

int16_t fm_open_wd(uint32_t dirid, int32_t procid) {
    for (int i = 0; i < 64; i++) if (wds[i].used && wds[i].dirid == dirid && wds[i].procid == procid) return (int16_t)(WD_BASE - i);
    for (int i = 0; i < 64; i++) if (!wds[i].used) { wds[i].used = 1; wds[i].dirid = dirid; wds[i].procid = procid; return (int16_t)(WD_BASE - i); }
    return 0;
}

/* Directory to use for (vRefNum, dirID). */
static uint32_t base_dir(int16_t vref, uint32_t dirid) {
    if (dirid) return dirid;
    if (vref <= WD_BASE && vref > WD_BASE - 64 && wds[WD_BASE - vref].used) return wds[WD_BASE - vref].dirid;
    if (vref == 0) return default_dir;
    return 2;
}

extern int cpu_trace;
static int16_t setres(maddr pb, int16_t err) {
    wr16(pb + ioResult, (uint16_t)err);
    if (cpu_trace) {
        char n[256] = ""; maddr np = rd32(pb + ioNamePtr); if (np && np < 0x01000000) pstr_to_c(np, n, sizeof n);
        fprintf(stderr, "    [file] vref=%d dir=%u name=\"%s\" -> %d\n", (int16_t)rd16(pb + ioVRefNum), rd32(pb + ioDirID), n, err);
    }
    return err;
}

static void name_from_pb(maddr pb, char *out) {
    maddr np = rd32(pb + ioNamePtr);
    if (np) pstr_to_c(np, out, 256); else out[0] = 0;
}

/* Find the node named by (vRefNum, dirID, ioNamePtr). Returns node or NULL; parid/leaf for creation. */
static VNode *pb_target(maddr pb, uint32_t dirid, uint32_t *parid, char *leaf) {
    char name[256]; name_from_pb(pb, name);
    uint32_t base = base_dir((int16_t)rd16(pb + ioVRefNum), dirid);
    if (!name[0]) { if (parid) *parid = base; if (leaf) leaf[0] = 0; return vfs_by_id(base); }
    return vfs_resolve_path(base, name, parid, leaf);
}

static void complete(maddr pb, int async) {
    maddr comp = rd32(pb + ioCompletion);
    if (async && comp) {
        cpu_call_irq(comp, (uint32_t)(int32_t)(int16_t)rd16(pb + ioResult), pb, 0, 0, NULL);
    }
}

static Fcb *fcb(int16_t ref) {
    int i = (ref - 1000) / 2;
    if (ref < 1000 || (ref & 1) || i >= MAX_FCB || !fcbs[i].used) return NULL;
    return &fcbs[i];
}

/* Device drivers are opened by names starting with '.'; they get negative refnums. */
static int16_t driver_refnum(const char *name) {
    static const struct { const char *n; int16_t r; } drv[] = {
        {".Print", -3}, {".Sound", -4}, {".Sony", -5}, {".AIn", -6}, {".AOut", -7}, {".BIn", -8}, {".BOut", -9},
        {".MPP", -10}, {".ATP", -11}, {".XPP", -41}, {".EDisk", -48} };
    for (unsigned i = 0; i < sizeof drv / sizeof drv[0]; i++) if (!mac_strcasecmp(name, drv[i].n)) return drv[i].r;
    return 0;
}

static int16_t open_fork(maddr pb, uint32_t dirid, int forkno) {
    {
        char nm[256]; name_from_pb(pb, nm);
        if (nm[0] == '.') {
            int16_t r = driver_refnum(nm);
            if (!r) return setres(pb, -43);
            wr16(pb + ioRefNum, (uint16_t)r);
            return setres(pb, 0);
        }
    }
    VNode *n = pb_target(pb, dirid, NULL, NULL);
    if (!n) return setres(pb, -43);     /* fnfErr */
    if (n->isdir) return setres(pb, -43);
    for (int i = 0; i < MAX_FCB; i++) if (!fcbs[i].used) {
        fcbs[i] = (Fcb){ n, forkno, 0, (int8_t)rd8(pb + ioPermssn), 1 };
        wr16(pb + ioRefNum, (uint16_t)FREF(i));
        return setres(pb, 0);
    }
    return setres(pb, -42);             /* tmfoErr */
}

int16_t PBOpen(maddr pb, int async)    { int16_t e = open_fork(pb, 0, 0); complete(pb, async); return e; }
int16_t PBHOpen(maddr pb, int async)   { int16_t e = open_fork(pb, rd32(pb + ioDirID), 0); complete(pb, async); return e; }
int16_t PBOpenRF(maddr pb, int async)  { int16_t e = open_fork(pb, 0, 1); complete(pb, async); return e; }
int16_t PBHOpenRF(maddr pb, int async) { int16_t e = open_fork(pb, rd32(pb + ioDirID), 1); complete(pb, async); return e; }

int16_t PBClose(maddr pb, int async) {
    Fcb *f = fcb((int16_t)rd16(pb + ioRefNum));
    int16_t e = 0;
    if ((int16_t)rd16(pb + ioRefNum) < 0) e = 0;      /* driver */
    else if (!f) e = -51;                    /* rfNumErr */
    else { vfs_flush(f->node); f->used = 0; }
    setres(pb, e); complete(pb, async); return e;
}

static uint32_t newpos(Fcb *f, maddr pb) {
    uint16_t mode = rd16(pb + ioPosMode) & 3;
    int32_t off = (int32_t)rd32(pb + ioPosOffset);
    switch (mode) {
        case 1: return (uint32_t)off;                                  /* fsFromStart */
        case 2: return (uint32_t)((int32_t)f->node->forklen[f->fork] + off);  /* fsFromLEOF */
        case 3: return (uint32_t)((int32_t)f->pos + off);              /* fsFromMark */
        default: return f->pos;                                        /* fsAtMark */
    }
}

int16_t PBRead(maddr pb, int async) {
    Fcb *f = fcb((int16_t)rd16(pb + ioRefNum));
    if (!f) { setres(pb, -51); complete(pb, async); return -51; }
    uint32_t pos = newpos(f, pb), len = f->node->forklen[f->fork];
    uint32_t req = rd32(pb + ioReqCount), n = 0;
    maddr buf = rd32(pb + ioBuffer);
    int16_t e = 0;
    uint16_t mode = rd16(pb + ioPosMode);
    if (pos > len) { pos = len; e = -39; }
    else {
        n = req; if (pos + n > len) { n = len - pos; e = -39; }  /* eofErr */
        if (mode & 0x80) {  /* newline mode */
            uint8_t nl = (uint8_t)(mode >> 8);
            for (uint32_t i = 0; i < n; i++) if (f->node->fork[f->fork][pos + i] == nl) { n = i + 1; e = 0; break; }
        }
        mput(buf, f->node->fork[f->fork] + pos, n);
    }
    f->pos = pos + n;
    wr32(pb + ioActCount, n); wr32(pb + ioPosOffset, f->pos);
    setres(pb, e); complete(pb, async); return e;
}

int16_t PBWrite(maddr pb, int async) {
    Fcb *f = fcb((int16_t)rd16(pb + ioRefNum));
    if (!f) { setres(pb, -51); complete(pb, async); return -51; }
    uint32_t pos = newpos(f, pb), req = rd32(pb + ioReqCount);
    if (pos + req > f->node->forklen[f->fork]) vfs_set_fork_len(f->node, f->fork, pos + req);
    mget(f->node->fork[f->fork] + pos, rd32(pb + ioBuffer), req);
    f->node->dirty = 1;
    f->node->mddate = vfs_mac_time_now();
    f->pos = pos + req;
    wr32(pb + ioActCount, req); wr32(pb + ioPosOffset, f->pos);
    setres(pb, 0); complete(pb, async); return 0;
}

int16_t PBGetEOF(maddr pb, int async) {
    Fcb *f = fcb((int16_t)rd16(pb + ioRefNum));
    int16_t e = f ? 0 : -51;
    if (f) wr32(pb + ioMisc, f->node->forklen[f->fork]);
    setres(pb, e); complete(pb, async); return e;
}
int16_t PBSetEOF(maddr pb, int async) {
    Fcb *f = fcb((int16_t)rd16(pb + ioRefNum));
    int16_t e = f ? 0 : -51;
    if (f) { vfs_set_fork_len(f->node, f->fork, rd32(pb + ioMisc)); if (f->pos > rd32(pb + ioMisc)) f->pos = rd32(pb + ioMisc); }
    setres(pb, e); complete(pb, async); return e;
}
int16_t PBGetFPos(maddr pb, int async) {
    Fcb *f = fcb((int16_t)rd16(pb + ioRefNum));
    int16_t e = f ? 0 : -51;
    if (f) { wr32(pb + ioPosOffset, f->pos); wr32(pb + ioReqCount, 0); wr32(pb + ioActCount, 0); wr16(pb + ioPosMode, 0); }
    setres(pb, e); complete(pb, async); return e;
}
int16_t PBSetFPos(maddr pb, int async) {
    Fcb *f = fcb((int16_t)rd16(pb + ioRefNum));
    int16_t e = f ? 0 : -51;
    if (f) {
        uint32_t p = newpos(f, pb);
        if (p > f->node->forklen[f->fork]) { p = f->node->forklen[f->fork]; e = -39; }
        f->pos = p; wr32(pb + ioPosOffset, p);
    }
    setres(pb, e); complete(pb, async); return e;
}

static int16_t do_create(maddr pb, uint32_t dirid, int isdir) {
    uint32_t parid; char leaf[64];
    VNode *n = pb_target(pb, dirid, &parid, leaf);
    if (n) return -48;                              /* dupFNErr */
    if (!leaf[0] || !vfs_by_id(parid)) return -120; /* dirNFErr */
    n = vfs_create(parid, leaf, isdir);
    if (!n) return -48;
    if (isdir) wr32(pb + ioDirID, n->cnid);
    return 0;
}
int16_t PBCreate(maddr pb, int async)  { int16_t e = setres(pb, do_create(pb, 0, 0)); complete(pb, async); return e; }
int16_t PBHCreate(maddr pb, int async) { int16_t e = setres(pb, do_create(pb, rd32(pb + ioDirID), 0)); complete(pb, async); return e; }
int16_t PBDirCreate(maddr pb, int async) { int16_t e = setres(pb, do_create(pb, rd32(pb + ioDirID), 1)); complete(pb, async); return e; }

static int16_t do_delete(maddr pb, uint32_t dirid) {
    VNode *n = pb_target(pb, dirid, NULL, NULL);
    if (!n) return -43;
    for (int i = 0; i < MAX_FCB; i++) if (fcbs[i].used && fcbs[i].node == n) return -47;
    if (n->locked) return -45;
    return vfs_delete(n);
}
int16_t PBDelete(maddr pb, int async)  { int16_t e = setres(pb, do_delete(pb, 0)); complete(pb, async); return e; }
int16_t PBHDelete(maddr pb, int async) { int16_t e = setres(pb, do_delete(pb, rd32(pb + ioDirID))); complete(pb, async); return e; }

static int16_t do_rename(maddr pb, uint32_t dirid) {
    VNode *n = pb_target(pb, dirid, NULL, NULL);
    if (!n) return -43;
    char nn[256]; pstr_to_c(rd32(pb + ioMisc), nn, sizeof nn);
    char *c = strrchr(nn, ':'); const char *leaf = c ? c + 1 : nn;
    return vfs_rename(n, leaf);
}
int16_t PBRename(maddr pb, int async)  { int16_t e = setres(pb, do_rename(pb, 0)); complete(pb, async); return e; }
int16_t PBHRename(maddr pb, int async) { int16_t e = setres(pb, do_rename(pb, rd32(pb + ioDirID))); complete(pb, async); return e; }

static void fill_file_info(maddr pb, VNode *n, int cat) {
    wr8(pb + ioFlAttrib, (uint8_t)((n->isdir ? 0x10 : 0) | (n->locked ? 0x01 : 0)));
    if (n->isdir) {
        mput(pb + ioFlFndrInfo, n->finfo, 16);
        wr32(pb + ioDirID, n->cnid);
        wr16(pb + ioDrNmFls, (uint16_t)vfs_child_count(n->cnid));
        wr32(pb + ioFlCrDat, n->crdate); wr32(pb + ioFlMdDat, n->mddate);
        if (cat) { wr32(pb + ioFlBkDat, 0); mput(pb + ioFlXFndrInfo, n->fxinfo, 16); wr32(pb + ioFlParID, n->parid); }
        return;
    }
    mput(pb + ioFlFndrInfo, n->finfo, 16);
    wr32(pb + ioDirID, n->cnid);
    wr16(pb + 52, 0);
    wr32(pb + ioFlLgLen, n->forklen[0]); wr32(pb + ioFlPyLen, (n->forklen[0] + 511) & ~511u);
    wr16(pb + 62, 0);
    wr32(pb + ioFlRLgLen, n->forklen[1]); wr32(pb + ioFlRPyLen, (n->forklen[1] + 511) & ~511u);
    wr32(pb + ioFlCrDat, n->crdate); wr32(pb + ioFlMdDat, n->mddate);
    if (cat) { wr32(pb + ioFlBkDat, 0); mput(pb + ioFlXFndrInfo, n->fxinfo, 16); wr32(pb + ioFlParID, n->parid); wr32(pb + ioFlClpSiz, 0); }
    wr16(pb + 24, 0);  /* ioFRefNum: not tracked */
    for (int i = 0; i < MAX_FCB; i++) if (fcbs[i].used && fcbs[i].node == n) { wr8(pb + ioFlAttrib, rd8(pb + ioFlAttrib) | 0x80); wr16(pb + 24, (uint16_t)FREF(i)); break; }
}

static void set_name(maddr pb, VNode *n) {
    maddr np = rd32(pb + ioNamePtr);
    if (np) c_to_pstr(n->name, np);
}

static int16_t do_getfinfo(maddr pb, uint32_t dirid, int cat) {
    int16_t idx = (int16_t)rd16(pb + ioFDirIndex);
    VNode *n;
    if (idx > 0) {
        uint32_t base = base_dir((int16_t)rd16(pb + ioVRefNum), dirid);
        if (cat) n = vfs_child_index(base, idx);
        else {   /* GetFInfo indexes files only */
            int k = 0; n = NULL;
            for (int i = 1; (n = vfs_child_index(base, i)); i++) if (!n->isdir && ++k == idx) break;
        }
        if (!n) return -43;
        set_name(pb, n);
    } else if (idx < 0 && cat) {
        n = vfs_by_id(base_dir((int16_t)rd16(pb + ioVRefNum), dirid));
        if (!n) return -120;
        set_name(pb, n);
    } else {
        n = pb_target(pb, dirid, NULL, NULL);
        if (!n) return -43;
        if (!cat && n->isdir) return -43;
    }
    fill_file_info(pb, n, cat);
    return 0;
}
int16_t PBGetFInfo(maddr pb, int async)  { int16_t e = setres(pb, do_getfinfo(pb, 0, 0)); complete(pb, async); return e; }
int16_t PBHGetFInfo(maddr pb, int async) { int16_t e = setres(pb, do_getfinfo(pb, rd32(pb + ioDirID), 0)); complete(pb, async); return e; }
int16_t PBGetCatInfo(maddr pb, int async){ int16_t e = setres(pb, do_getfinfo(pb, rd32(pb + ioDirID), 1)); complete(pb, async); return e; }

static int16_t do_setfinfo(maddr pb, uint32_t dirid, int cat) {
    VNode *n = pb_target(pb, dirid, NULL, NULL);
    if (!n) return -43;
    mget(n->finfo, pb + ioFlFndrInfo, 16);
    n->crdate = rd32(pb + ioFlCrDat); n->mddate = rd32(pb + ioFlMdDat);
    if (cat) mget(n->fxinfo, pb + ioFlXFndrInfo, 16);
    n->dirty = 1; vfs_flush(n);
    return 0;
}
int16_t PBSetFInfo(maddr pb, int async)  { int16_t e = setres(pb, do_setfinfo(pb, 0, 0)); complete(pb, async); return e; }
int16_t PBHSetFInfo(maddr pb, int async) { int16_t e = setres(pb, do_setfinfo(pb, rd32(pb + ioDirID), 0)); complete(pb, async); return e; }
int16_t PBSetCatInfo(maddr pb, int async){ int16_t e = setres(pb, do_setfinfo(pb, rd32(pb + ioDirID), 1)); complete(pb, async); return e; }

static int16_t do_lock(maddr pb, uint32_t dirid, int lock) {
    VNode *n = pb_target(pb, dirid, NULL, NULL);
    if (!n) return -43;
    n->locked = lock; return 0;
}
int16_t PBSetFLock(maddr pb, int async)  { int16_t e = setres(pb, do_lock(pb, 0, 1)); complete(pb, async); return e; }
int16_t PBRstFLock(maddr pb, int async)  { int16_t e = setres(pb, do_lock(pb, 0, 0)); complete(pb, async); return e; }
int16_t PBHSetFLock(maddr pb, int async) { int16_t e = setres(pb, do_lock(pb, rd32(pb + ioDirID), 1)); complete(pb, async); return e; }
int16_t PBHRstFLock(maddr pb, int async) { int16_t e = setres(pb, do_lock(pb, rd32(pb + ioDirID), 0)); complete(pb, async); return e; }

static void fill_vinfo(maddr pb, int h) {
    maddr np = rd32(pb + ioNamePtr);
    if (np) c_to_pstr(g_vol.volname, np);
    wr16(pb + ioVRefNum, (uint16_t)VREFNUM);
    wr32(pb + 30, g_vol.crdate); wr32(pb + 34, vfs_mac_time_now());
    wr16(pb + 38, 0); wr16(pb + 40, (uint16_t)vfs_child_count(2));
    wr16(pb + 42, 0); wr16(pb + 44, 0);
    wr16(pb + 46, 0xFFFF);            /* blocks */
    wr32(pb + 48, 0x8000);            /* alloc block size: 32K -> ~2GB volume */
    wr32(pb + 52, 0x8000);
    wr16(pb + 56, 0);
    wr32(pb + 58, g_vol.next_cnid);
    wr16(pb + 62, 0x8000);            /* free blocks: 1 GB */
    if (h) {
        wr16(pb + 64, 0x4244); wr16(pb + 66, 1); wr16(pb + 68, 0xFFDF);
        wr16(pb + 70, 0); wr32(pb + 72, 0); wr16(pb + 76, 0); wr32(pb + 78, 0);
        wr32(pb + 82, 100); wr32(pb + 86, 10); mset(pb + 90, 0, 32);
    }
}
static int16_t do_getvinfo(maddr pb, int h) {
    int16_t idx = (int16_t)rd16(pb + ioVolIndex);
    if (idx > 1) return -35;          /* nsvErr */
    fill_vinfo(pb, h);
    return 0;
}
int16_t PBGetVInfo(maddr pb, int async)  { int16_t e = setres(pb, do_getvinfo(pb, 0)); complete(pb, async); return e; }
int16_t PBHGetVInfo(maddr pb, int async) { int16_t e = setres(pb, do_getvinfo(pb, 1)); complete(pb, async); return e; }

int16_t PBGetVol(maddr pb, int async) {
    maddr np = rd32(pb + ioNamePtr);
    if (np) c_to_pstr(g_vol.volname, np);
    wr16(pb + ioVRefNum, (uint16_t)(default_dir == 2 ? VREFNUM : fm_open_wd(default_dir, 0)));
    setres(pb, 0); complete(pb, async); return 0;
}
int16_t PBHGetVol(maddr pb, int async) {
    maddr np = rd32(pb + ioNamePtr);
    if (np) c_to_pstr(g_vol.volname, np);
    wr16(pb + ioVRefNum, (uint16_t)(default_dir == 2 ? VREFNUM : fm_open_wd(default_dir, 0)));
    wr32(pb + ioWDProcID, 0);
    wr16(pb + ioWDVRefNum, (uint16_t)VREFNUM);
    wr32(pb + ioWDDirID, default_dir);
    setres(pb, 0); complete(pb, async); return 0;
}
int16_t PBSetVol(maddr pb, int async) {
    default_dir = base_dir((int16_t)rd16(pb + ioVRefNum), 0);
    if ((int16_t)rd16(pb + ioVRefNum) == VREFNUM) default_dir = 2;
    setres(pb, 0); complete(pb, async); return 0;
}
int16_t PBHSetVol(maddr pb, int async) {
    VNode *n = pb_target(pb, rd32(pb + ioWDDirID), NULL, NULL);
    if (!n || !n->isdir) { setres(pb, -120); complete(pb, async); return -120; }
    default_dir = n->cnid;
    setres(pb, 0); complete(pb, async); return 0;
}
int16_t PBOpenWD(maddr pb, int async) {
    VNode *n = pb_target(pb, rd32(pb + ioWDDirID), NULL, NULL);
    if (!n || !n->isdir) { setres(pb, -120); complete(pb, async); return -120; }
    wr16(pb + ioVRefNum, (uint16_t)fm_open_wd(n->cnid, (int32_t)rd32(pb + ioWDProcID)));
    setres(pb, 0); complete(pb, async); return 0;
}
int16_t PBCloseWD(maddr pb, int async) {
    int16_t v = (int16_t)rd16(pb + ioVRefNum);
    if (v <= WD_BASE && v > WD_BASE - 64) wds[WD_BASE - v].used = 0;
    setres(pb, 0); complete(pb, async); return 0;
}
int16_t PBGetWDInfo(maddr pb, int async) {
    int16_t v = (int16_t)rd16(pb + ioVRefNum);
    int16_t idx = (int16_t)rd16(pb + ioWDIndex);
    int i = -1;
    if (idx > 0) { int k = 0; for (int j = 0; j < 64; j++) if (wds[j].used && ++k == idx) { i = j; break; } }
    else if (v <= WD_BASE && v > WD_BASE - 64) i = WD_BASE - v;
    uint32_t dir = i >= 0 && wds[i].used ? wds[i].dirid : (v == 0 ? default_dir : 2);
    if (i >= 0) wr16(pb + ioVRefNum, (uint16_t)(WD_BASE - i));
    wr32(pb + ioWDProcID, i >= 0 ? (uint32_t)wds[i].procid : 0);
    wr16(pb + ioWDVRefNum, (uint16_t)VREFNUM);
    wr32(pb + ioWDDirID, dir);
    maddr np = rd32(pb + ioNamePtr);
    if (np) c_to_pstr(g_vol.volname, np);
    setres(pb, 0); complete(pb, async); return 0;
}
int16_t PBFlushVol(maddr pb, int async) { vfs_flush_all(); setres(pb, 0); complete(pb, async); return 0; }
int16_t PBFlushFile(maddr pb, int async) {
    Fcb *f = fcb((int16_t)rd16(pb + ioRefNum)); if (f) vfs_flush(f->node);
    setres(pb, f ? 0 : -51); complete(pb, async); return f ? 0 : -51;
}

/* ------------------------------------------------------------ FSSpec -- */
/* FSSpec: vRefNum(2) parID(4) name(Str63) */
int16_t FSMakeFSSpec(int16_t vref, uint32_t dirid, const char *name, maddr spec) {
    uint32_t base = base_dir(vref, dirid);
    uint32_t parid; char leaf[64];
    VNode *n;
    if (!name[0]) { n = vfs_by_id(base); parid = n ? n->parid : 1; strcpy(leaf, n ? n->name : ""); }
    else n = vfs_resolve_path(base, name, &parid, leaf);
    if (n && !leaf[0]) { parid = n->parid; strcpy(leaf, n->name); }
    wr16(spec, (uint16_t)VREFNUM); wr32(spec + 2, parid); c_to_pstr(leaf, spec + 6);
    if (!vfs_by_id(parid)) return -120;
    return n ? 0 : -43;
}
VNode *fsspec_node(maddr spec, uint32_t *parid, char *leaf) {
    char nm[64]; pstr_to_c(spec + 6, nm, sizeof nm);
    uint32_t p = rd32(spec + 2);
    if (parid) *parid = p;
    if (leaf) strcpy(leaf, nm);
    return vfs_lookup(p, nm);
}
int16_t FSpOpenFork(maddr spec, int fork, int8_t perm, int16_t *ref) {
    VNode *n = fsspec_node(spec, NULL, NULL);
    if (!n || n->isdir) return -43;
    for (int i = 0; i < MAX_FCB; i++) if (!fcbs[i].used) { fcbs[i] = (Fcb){ n, fork, 0, perm, 1 }; *ref = FREF(i); return 0; }
    return -42;
}
int16_t FSpCreate(maddr spec, uint32_t creator, uint32_t type, int16_t script) {
    (void)script;
    uint32_t parid; char leaf[64];
    if (fsspec_node(spec, &parid, leaf)) return -48;
    VNode *n = vfs_create(parid, leaf, 0);
    if (!n) return -120;
    put32(n->finfo, type); put32(n->finfo + 4, creator);
    n->dirty = 1; vfs_flush(n);
    return 0;
}
int16_t FSpDelete(maddr spec) { VNode *n = fsspec_node(spec, NULL, NULL); return n ? vfs_delete(n) : -43; }
int16_t FSpGetFInfo(maddr spec, maddr finfo) { VNode *n = fsspec_node(spec, NULL, NULL); if (!n) return -43; mput(finfo, n->finfo, 16); return 0; }
int16_t FSpSetFInfo(maddr spec, maddr finfo) { VNode *n = fsspec_node(spec, NULL, NULL); if (!n) return -43; mget(n->finfo, finfo, 16); n->dirty = 1; vfs_flush(n); return 0; }
int16_t FSpRename(maddr spec, const char *nn) { VNode *n = fsspec_node(spec, NULL, NULL); return n ? vfs_rename(n, nn) : -43; }
int16_t FSpOpenResFile(maddr spec, int8_t perm) {
    VNode *n = fsspec_node(spec, NULL, NULL);
    if (!n) { wr16(LM_ResErr, (uint16_t)-43); return -1; }
    return rsrc_open_node(n, perm);
}
void FSpCreateResFile(maddr spec, uint32_t creator, uint32_t type, int16_t script) {
    uint32_t parid; char leaf[64];
    VNode *n = fsspec_node(spec, &parid, leaf);
    if (!n) { FSpCreate(spec, creator, type, script); n = fsspec_node(spec, NULL, NULL); }
    if (!n) { wr16(LM_ResErr, (uint16_t)-120); return; }
    if (!n->forklen[1]) CreateResFile_node(n);
    wr16(LM_ResErr, 0);
}

/* Resource files by name (OpenResFile / HOpenResFile / CreateResFile) */
int16_t HOpenResFile(int16_t vref, uint32_t dirid, const char *name, int8_t perm) {
    VNode *n = vfs_resolve_path(base_dir(vref, dirid), name, NULL, NULL);
    if (!n || n->isdir) { wr16(LM_ResErr, (uint16_t)-43); return -1; }
    return rsrc_open_node(n, perm);
}
int16_t OpenResFile(const char *name) { return HOpenResFile(0, 0, name, 0); }
void HCreateResFile(int16_t vref, uint32_t dirid, const char *name) {
    uint32_t parid; char leaf[64];
    VNode *n = vfs_resolve_path(base_dir(vref, dirid), name, &parid, leaf);
    if (!n) n = vfs_create(parid, leaf, 0);
    if (!n) { wr16(LM_ResErr, (uint16_t)-120); return; }
    if (!n->forklen[1]) CreateResFile_node(n);
    wr16(LM_ResErr, 0);
}
void CreateResFile(const char *name) { HCreateResFile(0, 0, name); }

/* FCBs shared with the Resource Manager (resource files are open resource forks). */
int16_t fm_alloc_fcb(VNode *n, int fork) {
    for (int i = 0; i < MAX_FCB; i++) if (!fcbs[i].used) { fcbs[i] = (Fcb){ n, fork, 0, 3, 1 }; return FREF(i); }
    return -1;
}
void fm_free_fcb(int16_t ref) { Fcb *f = fcb(ref); if (f) f->used = 0; }

/* PBGetFCBInfo: ioRefNum@24 ioFCBIndx@28 ioFCBFlNm@32 ioFCBFlags@36 ioFCBStBlk@38 ioFCBEOF@40
 * ioFCBPLen@44 ioFCBCrPs@48 ioFCBVRefNum@52 ioFCBClpSiz@54 ioFCBParID@58 */
int16_t PBGetFCBInfo(maddr pb, int async) {
    int16_t idx = (int16_t)rd16(pb + 28);
    Fcb *f = NULL; int16_t ref = 0;
    if (idx > 0) {
        int k = 0;
        for (int i = 0; i < MAX_FCB; i++) if (fcbs[i].used && ++k == idx) { f = &fcbs[i]; ref = FREF(i); break; }
    } else { ref = (int16_t)rd16(pb + 24); f = fcb(ref); }
    if (!f) { setres(pb, -51); complete(pb, async); return -51; }
    wr16(pb + 24, (uint16_t)ref);
    maddr np = rd32(pb + ioNamePtr);
    if (np) c_to_pstr(f->node->name, np);
    wr32(pb + 32, f->node->cnid);
    wr16(pb + 36, (uint16_t)((f->fork ? 0x0200 : 0) | (f->perm > 1 ? 0x0100 : 0)));
    wr16(pb + 38, 0);
    wr32(pb + 40, f->node->forklen[f->fork]);
    wr32(pb + 44, (f->node->forklen[f->fork] + 511) & ~511u);
    wr32(pb + 48, f->pos);
    wr16(pb + 52, (uint16_t)VREFNUM);
    wr32(pb + 54, 0);
    wr32(pb + 58, f->node->parid);
    setres(pb, 0); complete(pb, async); return 0;
}

int fm_close_all(void) { for (int i = 0; i < MAX_FCB; i++) if (fcbs[i].used) { vfs_flush(fcbs[i].node); fcbs[i].used = 0; } return 0; }

/* save states (src/mac/mods.c): open files, working directories, default directory */
void *file_state(int i, size_t *n) {
    switch (i) {
        case 0: *n = sizeof fcbs; return fcbs;
        case 1: *n = sizeof wds; return wds;
        case 2: *n = sizeof default_dir; return &default_dir;
        default: *n = 0; return NULL;
    }
}
