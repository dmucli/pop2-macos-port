/* QuickDraw: globals, ports, devices, colors, GWorlds, Palette Manager. */
#include "qd.h"
#include "trapnums.h"
#include <stdio.h>
#include <stdlib.h>

int qd_screen_w = 640, qd_screen_h = 480;
uint32_t qd_clut_version = 1, qd_screen_dirty = 1;
static maddr main_gd, screen_pm, device_ctab;
static maddr qd_globals;              /* address of thePort inside the app's QD globals */
static maddr default_pixpat_white, default_pixpat_black;

/* ------------------------------------------------------------- colors -- */
uint32_t qd_device_rgb(int i) {
    maddr ct = rd32(device_ctab);
    maddr e = ct + CT_table + 8 * (i & 0xFF);
    return (uint32_t)(rd16(e + 2) >> 8) << 16 | (uint32_t)(rd16(e + 4) >> 8) << 8 | (rd16(e + 6) >> 8);
}
maddr qd_device_ctab(void) { return device_ctab; }
maddr qd_screen_device(void) { return main_gd; }

static void clut_changed(void) {
    maddr ct = rd32(device_ctab);
    wr32(ct + CT_seed, rd32(ct + CT_seed) + 1);
    qd_clut_version++;
}

int qd_rgb_to_index(uint16_t r, uint16_t g, uint16_t b) {
    maddr ct = rd32(device_ctab);
    int best = 0; uint32_t bd = 0xFFFFFFFF;
    for (int i = 0; i < 256; i++) {
        maddr e = ct + CT_table + 8 * i;
        int dr = (rd16(e + 2) >> 8) - (r >> 8), dg = (rd16(e + 4) >> 8) - (g >> 8), db = (rd16(e + 6) >> 8) - (b >> 8);
        uint32_t d = (uint32_t)(dr * dr * 3 + dg * dg * 4 + db * db * 2);
        if (d < bd) { bd = d; best = i; if (!d) break; }
    }
    return best;
}

static void classic_rgb(int32_t c, uint16_t *r, uint16_t *g, uint16_t *b) {
    switch (c) {
        case 33:  *r = *g = *b = 0; break;                       /* black */
        case 30:  *r = *g = *b = 0xFFFF; break;                  /* white */
        case 205: *r = 0xDD6B; *g = 0x08C2; *b = 0x06A2; break;  /* red */
        case 341: *r = 0x0000; *g = 0x8000; *b = 0x11B0; break;  /* green */
        case 409: *r = 0x0000; *g = 0x0000; *b = 0xD400; break;  /* blue */
        case 273: *r = 0x0241; *g = 0xAB54; *b = 0xEAFF; break;  /* cyan */
        case 137: *r = 0xF2D7; *g = 0x0856; *b = 0x84EC; break;  /* magenta */
        case 69:  *r = 0xFC00; *g = 0xF37D; *b = 0x052F; break;  /* yellow */
        default:  *r = *g = *b = 0; break;
    }
}

int qd_is_color_port(maddr port) { return (rd16(port + GP_portVersion) & 0xC000) == 0xC000; }

int qd_port_fg_index(maddr port) {
    if (qd_is_color_port(port)) return (int)(rd32(port + GP_fgColor) & 0xFF);
    uint16_t r, g, b; classic_rgb((int32_t)rd32(port + GP_fgColor), &r, &g, &b);
    return qd_rgb_to_index(r, g, b);
}
int qd_port_bk_index(maddr port) {
    if (qd_is_color_port(port)) return (int)(rd32(port + GP_bkColor) & 0xFF);
    uint16_t r, g, b; classic_rgb((int32_t)rd32(port + GP_bkColor), &r, &g, &b);
    return qd_rgb_to_index(r, g, b);
}

/* ------------------------------------------------------------- screen -- */
static maddr new_ctab_std8(void) {
    maddr h = NewHandleSys(8 + 256 * 8);
    maddr p = rd32(h);
    wr32(p + CT_seed, 8); wr16(p + CT_flags, 0x8000); wr16(p + CT_size, 255);
    static const uint8_t lv[6] = {0xFF, 0xCC, 0x99, 0x66, 0x33, 0x00};
    static const uint8_t ramp[10] = {0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11};
    int i = 0;
    for (int r = 0; r < 6; r++) for (int g = 0; g < 6; g++) for (int b = 0; b < 6; b++) {
        if (i == 215) break;
        maddr e = p + CT_table + 8 * i;
        wr16(e, (uint16_t)i); wr16(e + 2, lv[r] * 0x101); wr16(e + 4, lv[g] * 0x101); wr16(e + 6, lv[b] * 0x101);
        i++;
    }
    for (int c = 0; c < 4; c++) for (int k = 0; k < 10; k++, i++) {
        maddr e = p + CT_table + 8 * i;
        uint16_t v = ramp[k] * 0x101;
        wr16(e, (uint16_t)i);
        wr16(e + 2, c == 0 || c == 3 ? v : 0); wr16(e + 4, c == 1 || c == 3 ? v : 0); wr16(e + 6, c == 2 || c == 3 ? v : 0);
    }
    maddr e = p + CT_table + 8 * 255;
    wr16(e, 255); wr16(e + 2, 0); wr16(e + 4, 0); wr16(e + 6, 0);
    return h;
}

static void init_pixmap(maddr pm, maddr base, int rowbytes, QRect bounds, maddr ctab) {
    mset(pm, 0, PM_SIZE);
    wr32(pm + PM_baseAddr, base);
    wr16(pm + PM_rowBytes, (uint16_t)(0x8000 | rowbytes));
    wr_rect(pm + PM_bounds, bounds);
    wr32(pm + PM_hRes, 72 << 16); wr32(pm + PM_vRes, 72 << 16);
    wr16(pm + PM_pixelSize, 8); wr16(pm + PM_cmpCount, 1); wr16(pm + PM_cmpSize, 8);
    wr32(pm + PM_pmTable, ctab);
}

static maddr new_pixpat(int white) {
    maddr h = NewHandleSys(28);
    maddr p = rd32(h);
    mset(p, 0, 28);
    mset(p + 20, white ? 0x00 : 0xFF, 8);  /* pat1Data */
    return h;
}

void qd_init_screen(int w, int h) {
    qd_screen_w = w; qd_screen_h = h;
    device_ctab = new_ctab_std8();
    maddr pmh = NewHandleSys(PM_SIZE);
    screen_pm = pmh;
    init_pixmap(rd32(pmh), SCREEN_BASE, w, (QRect){0, 0, (int16_t)h, (int16_t)w}, device_ctab);
    main_gd = NewHandleSys(GD_SIZE);
    maddr gd = rd32(main_gd);
    mset(gd, 0, GD_SIZE);
    wr16(gd + GD_type, 0);                 /* clutType */
    wr16(gd + GD_flags, (1 << 0) | (1 << 11) | (1 << 13) | (1 << 14) | (1 << 15));  /* ... screenDevice, mainScreen, noDriver? ramInit, gdDevType */
    wr32(gd + GD_pMap, pmh);
    wr_rect(gd + GD_rect, (QRect){0, 0, (int16_t)h, (int16_t)w});
    wr32(gd + GD_mode, 0x83);              /* 8 bpp mode */
    wr32(gd + GD_iTable, NewHandleSys(16));
    wr32(LM_MainDevice, main_gd);
    wr32(LM_TheGDevice, main_gd);
    wr32(LM_DeviceList, main_gd);
    wr32(LM_ScrnBase, SCREEN_BASE);
    wr16(LM_ScreenRow, (uint16_t)w);
    mset(SCREEN_BASE, 0, (uint32_t)w * h);
    default_pixpat_white = new_pixpat(1);
    default_pixpat_black = new_pixpat(0);
}

/* ------------------------------------------------------------- ports -- */
maddr qd_globals_thePort_addr(void) { return qd_globals; }
maddr qd_the_port(void) { return qd_globals ? rd32(qd_globals) : 0; }
void qd_set_port(maddr port) { if (qd_globals) wr32(qd_globals, port); }

static void InitGraf(maddr gp) {
    qd_globals = gp;
    wr32(rd32(LM_CurrentA5), gp);          /* *(A5) = &thePort */
    wr32(gp, 0);
    mset(gp - 8, 0x00, 8);                 /* white */
    mset(gp - 16, 0xFF, 8);                /* black */
    for (int i = 0; i < 8; i++) wr8(gp - 24 + i, i & 1 ? 0x55 : 0xAA);   /* gray */
    for (int i = 0; i < 8; i++) wr8(gp - 32 + i, i & 1 ? 0x22 : 0x88);   /* ltGray */
    for (int i = 0; i < 8; i++) wr8(gp - 40 + i, i & 1 ? 0xDD : 0x77);   /* dkGray */
    /* arrow cursor (data, mask, hotspot) */
    static const uint16_t arrow_data[16] = {0x0000,0x4000,0x6000,0x7000,0x7800,0x7C00,0x7E00,0x7F00,0x7F80,0x7C00,0x6C00,0x4600,0x0600,0x0300,0x0300,0x0000};
    static const uint16_t arrow_mask[16] = {0xC000,0xE000,0xF000,0xF800,0xFC00,0xFE00,0xFF00,0xFF80,0xFFC0,0xFFE0,0xFE00,0xEF00,0xCF00,0x8780,0x0780,0x0380};
    for (int i = 0; i < 16; i++) { wr16(gp - 108 + 2 * i, arrow_data[i]); wr16(gp - 108 + 32 + 2 * i, arrow_mask[i]); }
    wr16(gp - 108 + 64, 1); wr16(gp - 108 + 66, 1);
    /* screenBits */
    wr32(gp - 122, SCREEN_BASE); wr16(gp - 118, (uint16_t)qd_screen_w);
    wr_rect(gp - 116, (QRect){0, 0, (int16_t)qd_screen_h, (int16_t)qd_screen_w});
    wr32(gp - 126, 1);                     /* randSeed */
}

static void init_port_common(maddr port) {
    QRect scr = {0, 0, (int16_t)qd_screen_h, (int16_t)qd_screen_w};
    wr_rect(port + GP_portRect, scr);
    maddr vis = qd_new_rgn(); qd_set_rect_rgn(vis, scr);
    maddr clip = qd_new_rgn(); qd_set_rect_rgn(clip, (QRect){-32767, -32767, 32767, 32767});
    wr32(port + GP_visRgn, vis); wr32(port + GP_clipRgn, clip);
    wr32(port + GP_pnLoc, 0); wr32(port + GP_pnSize, 0x00010001);
    wr16(port + GP_pnMode, 8);   /* patCopy */
    wr16(port + GP_pnVis, 0);
    wr16(port + GP_txFont, 0); wr8(port + GP_txFace, 0); wr8(port + GP_txFace + 1, 0);
    wr16(port + GP_txMode, 1);   /* srcOr */
    wr16(port + GP_txSize, 0); wr32(port + GP_spExtra, 0);
    wr16(port + GP_colrBit, 0); wr16(port + GP_patStretch, 0);
    wr32(port + GP_picSave, 0); wr32(port + GP_rgnSave, 0); wr32(port + GP_polySave, 0); wr32(port + GP_grafProcs, 0);
}

void InitPort(maddr port) {
    wr16(port + GP_device, 0);
    wr32(port + GP_portBits, SCREEN_BASE); wr16(port + GP_portBits + 4, (uint16_t)qd_screen_w);
    wr_rect(port + GP_portBits + 6, (QRect){0, 0, (int16_t)qd_screen_h, (int16_t)qd_screen_w});
    init_port_common(port);
    mset(port + GP_bkPat, 0x00, 8); mset(port + GP_fillPat, 0xFF, 8); mset(port + GP_pnPat, 0xFF, 8);
    wr32(port + GP_fgColor, 33); wr32(port + GP_bkColor, 30);
    qd_set_port(port);
}

void InitCPort(maddr port) {
    wr16(port + GP_device, 0);
    maddr pmh = NewHandleClear(PM_SIZE);
    mcopy(rd32(pmh), rd32(screen_pm), PM_SIZE);
    wr32(port + GP_portPixMap, pmh);
    wr16(port + GP_portVersion, 0xC000);
    wr32(port + GP_grafVars, NewHandleClear(32));
    wr16(port + GP_chExtra, 0); wr16(port + GP_pnLocHFrac, 0x8000);
    init_port_common(port);
    wr32(port + GP_bkPixPat, default_pixpat_white);
    wr32(port + GP_pnPixPat, default_pixpat_black);
    wr32(port + GP_fillPixPat, default_pixpat_black);
    for (int i = 0; i < 6; i++) wr8(port + GP_rgbFgColor + i, 0x00);
    for (int i = 0; i < 6; i++) wr8(port + GP_rgbBkColor + i, 0xFF);
    wr32(port + GP_fgColor, 255); wr32(port + GP_bkColor, 0);
    qd_set_port(port);
}

/* Pattern of a port field (old 8-byte pattern or PixPat handle's pat1Data). */
void qd_port_pattern(maddr port, int which, uint8_t out[8]) {
    /* which: 0 bk, 1 pen, 2 fill */
    if (qd_is_color_port(port)) {
        maddr h = rd32(port + (which == 0 ? GP_bkPixPat : which == 1 ? GP_pnPixPat : GP_fillPixPat));
        if (h && rd32(h)) { mget(out, rd32(h) + 20, 8); return; }
        memset(out, which == 0 ? 0 : 0xFF, 8);
        return;
    }
    mget(out, port + (which == 0 ? GP_bkPat : which == 1 ? GP_pnPat : GP_fillPat), 8);
}
void qd_port_set_pattern(maddr port, int which, const uint8_t pat[8]) {
    if (qd_is_color_port(port)) {
        maddr h = NewHandleClear(28);
        mput(rd32(h) + 20, pat, 8);
        wr32(port + (which == 0 ? GP_bkPixPat : which == 1 ? GP_pnPixPat : GP_fillPixPat), h);
        return;
    }
    mput(port + (which == 0 ? GP_bkPat : which == 1 ? GP_pnPat : GP_fillPat), pat, 8);
}

/* Surface for a BitMap or PixMap record address */
Surface qd_bits_surface(maddr bm) {
    Surface s;
    uint16_t rb = rd16(bm + 4);
    s.base = rd32(bm);
    s.rowbytes = rb & 0x3FFF;
    s.bounds = rd_rect(bm + 6);
    if (rb & 0x8000) {
        int ps = rd16(bm + PM_pixelSize);
        s.depth = ps ? ps : 8;
        s.ctab = rd32(bm + PM_pmTable);
    } else { s.depth = 1; s.ctab = 0; }
    return s;
}
/* The BitMap/PixMap record that a port draws into */
maddr qd_port_bits(maddr port) {
    if (qd_is_color_port(port)) return rd32(rd32(port + GP_portPixMap));
    return port + GP_portBits;
}
Surface qd_port_surface(maddr port) { return qd_bits_surface(qd_port_bits(port)); }

void qd_port_clip(maddr port, RgnList *out) {
    RgnList a, b, t;
    rgn_from_rect(&a, rd_rect(port + GP_portRect));
    rgn_get(rd32(port + GP_visRgn), &b);
    rgn_op(&a, &b, &t, 0);
    rgn_free(&a); rgn_free(&b);
    rgn_get(rd32(port + GP_clipRgn), &b);
    rgn_op(&t, &b, out, 0);
    rgn_free(&t); rgn_free(&b);
}

void qd_mark_dirty(Surface *s) {
    if (s->base >= SCREEN_BASE && s->base < SCREEN_BASE + 0x100000) qd_screen_dirty++;
}

/* ------------------------------------------------------------- GWorlds -- */
/* GWorld = CGrafPort (108) + private: [108] GDHandle of the device used */
#define GW_SIZE 128
static maddr cur_gworld_dev;

int16_t NewGWorld(maddr out, int16_t depth, QRect bounds, maddr ctab, maddr gdev, uint32_t flags) {
    (void)flags;
    if (depth == 0) depth = 8;
    if (depth != 8 && depth != 1) fprintf(stderr, "[qd] NewGWorld depth %d not supported, using 8\n", depth);
    int w = bounds.right - bounds.left, h = bounds.bottom - bounds.top;
    int rowbytes = (w + 3) & ~3;
    maddr pixels = NewPtrClear((uint32_t)rowbytes * h + 4);
    if (!pixels) return -108;
    maddr port = NewPtrClear(GW_SIZE);
    maddr save = qd_the_port();
    InitCPort(port);
    qd_set_port(save);
    maddr pm = rd32(rd32(port + GP_portPixMap));
    init_pixmap(pm, pixels, rowbytes, bounds, ctab ? ctab : device_ctab);
    wr_rect(port + GP_portRect, bounds);
    /* NewGWorld sets both visRgn and clipRgn to the boundary rectangle */
    qd_set_rect_rgn(rd32(port + GP_visRgn), bounds);
    qd_set_rect_rgn(rd32(port + GP_clipRgn), bounds);
    wr32(port + 108, gdev ? gdev : main_gd);
    wr32(out, port);
    if (getenv("POP2_QDLOG")) fprintf(stderr, "[qd] NewGWorld %d,%d,%d,%d depth %d -> port %06x pixels %06x..%06x rowbytes %d\n",
        bounds.top, bounds.left, bounds.bottom, bounds.right, depth, port, pixels, pixels + rowbytes * h, rowbytes);
    return 0;
}
void DisposeGWorld(maddr gw) {
    if (!gw) return;
    if (getenv("POP2_QDLOG")) fprintf(stderr, "[qd] DisposeGWorld %06x (pc %06x)\n", gw, trap_ret);
    maddr pmh = rd32(gw + GP_portPixMap);
    DisposePtr(rd32(rd32(pmh)));
    DisposeHandle(pmh);
    DisposeHandle(rd32(gw + GP_visRgn)); DisposeHandle(rd32(gw + GP_clipRgn));
    DisposePtr(gw);
}

/* -------------------------------------------------------------- traps -- */
static void T_InitGraf(uint16_t w) { (void)w; InitGraf(pop32()); }
static void T_OpenPort(uint16_t w) { (void)w; InitPort(pop32()); }
static void T_InitPort(uint16_t w) { (void)w; InitPort(pop32()); }
static void T_OpenCPort(uint16_t w) { (void)w; InitCPort(pop32()); }
static void T_InitCPort(uint16_t w) { (void)w; InitCPort(pop32()); }
static void T_ClosePort(uint16_t w) {
    (void)w; maddr p = pop32();
    DisposeHandle(rd32(p + GP_visRgn)); DisposeHandle(rd32(p + GP_clipRgn));
}
static void T_SetPort(uint16_t w) { (void)w; qd_set_port(pop32()); }
static void T_GetPort(uint16_t w) { (void)w; maddr p = pop32(); wr32(p, qd_the_port()); }
static void T_SetPortBits(uint16_t w) { (void)w; maddr bm = pop32(); mcopy(qd_the_port() + GP_portBits, bm, 14); }
static void T_PortSize(uint16_t w) {
    (void)w; int16_t h = pops16(), wd = pops16(); maddr p = qd_the_port();
    QRect r = rd_rect(p + GP_portRect); r.bottom = r.top + h; r.right = r.left + wd; wr_rect(p + GP_portRect, r);
}
static void T_MovePortTo(uint16_t w) {
    (void)w; int16_t top = pops16(), left = pops16(); maddr p = qd_the_port();
    maddr bm = qd_port_bits(p);
    QRect pr = rd_rect(p + GP_portRect), b = rd_rect(bm + 6);
    int bw = b.right - b.left, bh = b.bottom - b.top;
    b.left = pr.left - left; b.top = pr.top - top; b.right = b.left + bw; b.bottom = b.top + bh;
    wr_rect(bm + 6, b);
}
static void T_SetOrigin(uint16_t w) {
    (void)w; int16_t v = pops16(), h = pops16(); maddr p = qd_the_port();
    QRect pr = rd_rect(p + GP_portRect);
    int dh = h - pr.left, dv = v - pr.top;
    wr_rect(p + GP_portRect, rect_offset(pr, dv, dh));
    maddr bm = qd_port_bits(p);
    wr_rect(bm + 6, rect_offset(rd_rect(bm + 6), dv, dh));
    RgnList l; rgn_get(rd32(p + GP_visRgn), &l); rgn_offset(&l, dv, dh); rgn_set(rd32(p + GP_visRgn), &l); rgn_free(&l);
}
static void T_ClipRect(uint16_t w) { (void)w; maddr r = pop32(); qd_set_rect_rgn(rd32(qd_the_port() + GP_clipRgn), rd_rect(r)); }
static void copy_rgn(maddr src, maddr dst) {
    uint32_t sz = GetHandleSize(src); SetHandleSize(dst, sz); mcopy(rd32(dst), rd32(src), sz);
}
static void T_SetClip(uint16_t w) { (void)w; maddr r = pop32(); copy_rgn(r, rd32(qd_the_port() + GP_clipRgn)); }
static void T_GetClip(uint16_t w) { (void)w; maddr r = pop32(); copy_rgn(rd32(qd_the_port() + GP_clipRgn), r); }

static void T_LocalToGlobal(uint16_t w) {
    (void)w; maddr pt = pop32(); maddr p = qd_the_port();
    QRect b = rd_rect(qd_port_bits(p) + 6);
    QPoint q = rd_pt(pt); q.v -= b.top; q.h -= b.left; wr_pt(pt, q);
}
static void T_GlobalToLocal(uint16_t w) {
    (void)w; maddr pt = pop32(); maddr p = qd_the_port();
    QRect b = rd_rect(qd_port_bits(p) + 6);
    QPoint q = rd_pt(pt); q.v += b.top; q.h += b.left; wr_pt(pt, q);
}

/* colors */
static void set_rgb_field(maddr port, int bk, uint16_t r, uint16_t g, uint16_t b) {
    if (qd_is_color_port(port)) {
        maddr f = port + (bk ? GP_rgbBkColor : GP_rgbFgColor);
        wr16(f, r); wr16(f + 2, g); wr16(f + 4, b);
        wr32(port + (bk ? GP_bkColor : GP_fgColor), (uint32_t)qd_rgb_to_index(r, g, b));
    } else {
        /* old port: approximate with black/white */
        uint32_t lum = (r * 3u + g * 6u + b) / 10u;
        wr32(port + (bk ? GP_bkColor : GP_fgColor), lum > 0x7FFF ? 30 : 33);
    }
}
static void T_RGBForeColor(uint16_t w) { (void)w; maddr c = pop32(); set_rgb_field(qd_the_port(), 0, rd16(c), rd16(c + 2), rd16(c + 4)); }
static void T_RGBBackColor(uint16_t w) { (void)w; maddr c = pop32(); set_rgb_field(qd_the_port(), 1, rd16(c), rd16(c + 2), rd16(c + 4)); }
static void get_rgb(int bk, maddr c) {
    maddr p = qd_the_port();
    if (qd_is_color_port(p)) mcopy(c, p + (bk ? GP_rgbBkColor : GP_rgbFgColor), 6);
    else { uint16_t r, g, b; classic_rgb((int32_t)rd32(p + (bk ? GP_bkColor : GP_fgColor)), &r, &g, &b); wr16(c, r); wr16(c + 2, g); wr16(c + 4, b); }
}
static void T_GetForeColor(uint16_t w) { (void)w; get_rgb(0, pop32()); }
static void T_GetBackColor(uint16_t w) { (void)w; get_rgb(1, pop32()); }
static void T_ForeColor(uint16_t w) {
    (void)w; int32_t c = (int32_t)pop32(); maddr p = qd_the_port();
    if (qd_is_color_port(p)) { uint16_t r, g, b; classic_rgb(c, &r, &g, &b); set_rgb_field(p, 0, r, g, b); }
    else wr32(p + GP_fgColor, (uint32_t)c);
}
static void T_BackColor(uint16_t w) {
    (void)w; int32_t c = (int32_t)pop32(); maddr p = qd_the_port();
    if (qd_is_color_port(p)) { uint16_t r, g, b; classic_rgb(c, &r, &g, &b); set_rgb_field(p, 1, r, g, b); }
    else wr32(p + GP_bkColor, (uint32_t)c);
}
static void T_Color2Index(uint16_t w) { (void)w; maddr c = pop32(); ret32((uint32_t)qd_rgb_to_index(rd16(c), rd16(c + 2), rd16(c + 4))); }
static void T_Index2Color(uint16_t w) {
    (void)w; maddr c = pop32(); int32_t i = (int32_t)pop32();
    maddr e = rd32(device_ctab) + CT_table + 8 * (i & 0xFF);
    mcopy(c, e + 2, 6);
}
static void T_RealColor(uint16_t w) { (void)w; pop32(); retbool(1); }
static void T_InvertColor(uint16_t w) { (void)w; maddr c = pop32(); wr16(c, ~rd16(c)); wr16(c + 2, ~rd16(c + 2)); wr16(c + 4, ~rd16(c + 4)); }
static void T_GetMainDevice(uint16_t w) { (void)w; ret32(main_gd); }
static void T_GetGDevice(uint16_t w) { (void)w; ret32(rd32(LM_TheGDevice)); }
static void T_SetGDevice(uint16_t w) { (void)w; wr32(LM_TheGDevice, pop32()); }
static void T_GetDeviceList(uint16_t w) { (void)w; ret32(main_gd); }
static void T_GetNextDevice(uint16_t w) { (void)w; pop32(); ret32(0); }
static void T_TestDeviceAttribute(uint16_t w) { (void)w; int16_t a = pops16(); maddr gd = pop32(); retbool((rd16(rd32(gd) + GD_flags) >> a) & 1); }
static void T_GetMaxDevice(uint16_t w) { (void)w; pop32(); ret32(main_gd); }
static void T_GetCTSeed(uint16_t w) { (void)w; ret32(rd32(rd32(device_ctab) + CT_seed)); }
static void T_CTabChanged(uint16_t w) { (void)w; pop32(); clut_changed(); }
static void T_GDeviceChanged(uint16_t w) { (void)w; pop32(); clut_changed(); }
static void T_MakeITable(uint16_t w) { (void)w; pop16(); pop32(); pop32(); }
static void T_ProtectEntry(uint16_t w) { (void)w; pop16(); pop16(); }
static void T_ReserveEntry(uint16_t w) { (void)w; pop16(); pop16(); }
static void T_GetCTable(uint16_t w) {
    (void)w; int16_t id = pops16();
    maddr r = GetResource(FOURCC('c','l','u','t'), id);
    if (r) { maddr h = NewHandle(GetHandleSize(r)); mcopy(rd32(h), rd32(r), GetHandleSize(r)); ret32(h); return; }
    if (id == 8 || id == 72 || id == 0) {
        uint32_t sz = GetHandleSize(device_ctab);
        maddr h = NewHandle(sz); mcopy(rd32(h), rd32(device_ctab), sz);
        if (id == 72) {   /* standard, not the current palette: rebuild */
            maddr std = new_ctab_std8(); mcopy(rd32(h), rd32(std), sz); DisposeHandle(std);
        }
        ret32(h); return;
    }
    ret32(0);
}
static void T_DisposeCTable(uint16_t w) { (void)w; DisposeHandle(pop32()); }

/* SetEntries(start, count, aTable): writes directly into the device CLUT */
void qd_set_entries(int start, int count, maddr specs) {
    maddr ct = rd32(device_ctab);
    for (int i = 0; i <= count; i++) {
        maddr s = specs + 8 * i;
        int idx = start < 0 ? rd16(s) : start + i;
        if (idx < 0 || idx > 255) continue;
        mcopy(ct + CT_table + 8 * idx + 2, s + 2, 6);
    }
    clut_changed();
}
static void T_SetEntries(uint16_t w) { (void)w; maddr t = pop32(); int16_t cnt = pops16(), st = pops16(); qd_set_entries(st, cnt, t); }
static void T_SaveEntries(uint16_t w) {
    (void)w; maddr sel = pop32(), res = pop32(), src = pop32(); (void)sel;
    maddr r = rd32(res), s = src ? rd32(src) : rd32(device_ctab);
    int n = rd16(r + CT_size);
    for (int i = 0; i <= n; i++) { int idx = rd16(r + CT_table + 8 * i); mcopy(r + CT_table + 8 * i + 2, s + CT_table + 8 * (idx & 255) + 2, 6); }
}
static void T_RestoreEntries(uint16_t w) {
    (void)w; maddr sel = pop32(), dst = pop32(), src = pop32(); (void)sel; (void)dst;
    maddr s = rd32(src);
    qd_set_entries(-1, rd16(s + CT_size), s + CT_table);
}

/* GWorld traps (QDExtensions dispatcher, selector in D0) */
static void T_QDExtensions(uint16_t w) {
    (void)w;
    switch (D0 & 0xFFFF) {
        case 0x0000: {  /* NewGWorld(offscreenGWorld, pixelDepth, boundsRect, cTable, aGDevice, flags): QDErr */
            uint32_t flags = pop32(); maddr gd = pop32(), ct = pop32(), br = pop32(); int16_t d = pops16(); maddr out = pop32();
            ret16((uint16_t)NewGWorld(out, d, rd_rect(br), ct, gd, flags));
            break;
        }
        case 0x0001: pop32(); retbool(1); break;                         /* LockPixels */
        case 0x0002: pop32(); break;                                     /* UnlockPixels */
        case 0x0003: {  /* UpdateGWorld(offscreenGWorld*, depth, rect, ctab, gdev, flags): GWorldFlags */
            uint32_t flags = pop32(); maddr gd = pop32(), ct = pop32(), br = pop32(); int16_t d = pops16(); maddr pgw = pop32();
            maddr gw = rd32(pgw);
            QRect nr = rd_rect(br), orr = rd_rect(gw + GP_portRect);
            if (nr.bottom - nr.top != orr.bottom - orr.top || nr.right - nr.left != orr.right - orr.left) {
                DisposeGWorld(gw); NewGWorld(pgw, d, nr, ct, gd, flags);
                ret32(1 << 3);   /* reallocPix */
            } else {
                maddr pm = rd32(rd32(gw + GP_portPixMap));
                wr_rect(pm + PM_bounds, nr); wr_rect(gw + GP_portRect, nr);
                qd_set_rect_rgn(rd32(gw + GP_visRgn), nr); qd_set_rect_rgn(rd32(gw + GP_clipRgn), nr);
                ret32(0);
            }
            break;
        }
        case 0x0004: DisposeGWorld(pop32()); break;
        case 0x0005: {  /* GetGWorld(port*, gdh*) */
            maddr gdh = pop32(), port = pop32();
            wr32(port, qd_the_port()); wr32(gdh, cur_gworld_dev ? cur_gworld_dev : rd32(LM_TheGDevice));
            break;
        }
        case 0x0006: {  /* SetGWorld(port, gdh) */
            maddr gdh = pop32(), port = pop32();
            qd_set_port(port);
            cur_gworld_dev = gdh;
            if (gdh) wr32(LM_TheGDevice, gdh);
            break;
        }
        case 0x0007: pop32(); break;                                     /* CTabChanged */
        case 0x0008: pop32(); break;                                     /* PixPatChanged */
        case 0x0009: pop32(); break;                                     /* PortChanged */
        case 0x000A: pop32(); break;                                     /* GDeviceChanged */
        case 0x000B: pop32(); break;                                     /* AllowPurgePixels */
        case 0x000C: pop32(); break;
        case 0x000D: pop32(); ret32(0); break;                           /* GetPixelsState */
        case 0x000E: pop32(); pop32(); break;                            /* SetPixelsState */
        case 0x000F: { maddr pm = pop32(); ret32(rd32(rd32(pm) + PM_baseAddr));
            if (getenv("POP2_QDLOG")) fprintf(stderr, "[qd] GetPixBaseAddr(%06x) -> %06x rb=%04x bounds=%d,%d,%d,%d\n", pm, rd32(rd32(pm)), rd16(rd32(pm) + 4),
                rds16(rd32(pm) + 6), rds16(rd32(pm) + 8), rds16(rd32(pm) + 10), rds16(rd32(pm) + 12));
            break; }  /* GetPixBaseAddr */
        case 0x0010: case 0x0015: pop32(); pop32(); pop16(); pop32(); ret16((uint16_t)-108); break;  /* New[Temp]ScreenBuffer */
        case 0x0011: pop32(); break;
        case 0x0012: { maddr gw = pop32(); ret32(rd32(gw + 108)); break; }   /* GetGWorldDevice */
        case 0x0013: pop32(); retbool(0); break;                         /* QDDone */
        case 0x0014: ret32(0x130); break;                                /* OffscreenVersion */
        case 0x0016: pop32(); retbool(1); break;                         /* PixMap32Bit */
        case 0x0017: { maddr gw = pop32(); ret32(rd32(gw + GP_portPixMap)); break; }  /* GetGWorldPixMap */
        default:
            fprintf(stderr, "[qd] QDExtensions selector %08x unimplemented\n", D0);
            cpu_set(R_SP, cpu_get(R_SP) + ((D0 >> 16) & 0xFF) * 2);
            break;
    }
}

/* ---------------------------------------------------- Palette Manager -- */
/* Palette: pmEntries(2) pmDataFields(14) then ColorInfo[16 bytes]: rgb(6) usage(2) tolerance(2) private(6) */
#define PAL_HDR 16
typedef struct { maddr window, palette; } WinPal;
static WinPal winpal[64];
static maddr app_palette;

static maddr new_palette(int n, maddr ctab, int16_t usage, int16_t tol) {
    maddr h = NewHandleClear(PAL_HDR + 16u * n);
    maddr p = rd32(h);
    wr16(p, (uint16_t)n);
    for (int i = 0; i < n; i++) {
        maddr e = p + PAL_HDR + 16 * i;
        if (ctab && i <= rd16(rd32(ctab) + CT_size)) mcopy(e, rd32(ctab) + CT_table + 8 * i + 2, 6);
        wr16(e + 6, (uint16_t)usage); wr16(e + 8, (uint16_t)tol);
    }
    return h;
}
static maddr palette_for(maddr window) {
    for (int i = 0; i < 64; i++) if (winpal[i].window == window && window) return winpal[i].palette;
    return app_palette;
}
static void set_palette(maddr window, maddr pal) {
    if (!window || window == (maddr)-1) { app_palette = pal; return; }
    for (int i = 0; i < 64; i++) if (winpal[i].window == window) { winpal[i].palette = pal; return; }
    for (int i = 0; i < 64; i++) if (!winpal[i].window) { winpal[i] = (WinPal){ window, pal }; return; }
}
static void activate_palette(maddr window) {
    maddr pal = palette_for(window);
    if (!pal || !rd32(pal)) return;
    maddr p = rd32(pal), ct = rd32(device_ctab);
    int n = rd16(p);
    for (int i = 0; i < n && i < 256; i++) {
        maddr e = p + PAL_HDR + 16 * i;
        uint16_t usage = rd16(e + 6);
        if (!(usage & (0x0002 | 0x0004 | 0x0008))) continue;   /* tolerant / animated / explicit */
        mcopy(ct + CT_table + 8 * i + 2, e, 6);
    }
    clut_changed();
}

static void T_PaletteDispatch(uint16_t w) {
    (void)w;
    fprintf(stderr, "[qd] PaletteDispatch selector %04x unimplemented\n", D0 & 0xFFFF);
    cpu_set(R_SP, cpu_get(R_SP) + ((D0 >> 8) & 0xFF) * 2);
}
static void T_NewPalette(uint16_t w) { (void)w; int16_t tol = pops16(), usage = pops16(); maddr ct = pop32(); int16_t n = pops16(); ret32(new_palette(n, ct, usage, tol)); }
static void T_GetNewPalette(uint16_t w) {
    (void)w; int16_t id = pops16();
    maddr r = GetResource(FOURCC('p','l','t','t'), id);
    if (!r) { ret32(0); return; }
    maddr h = NewHandle(GetHandleSize(r)); mcopy(rd32(h), rd32(r), GetHandleSize(r)); ret32(h);
}
static void T_DisposePalette(uint16_t w) { (void)w; maddr p = pop32(); for (int i = 0; i < 64; i++) if (winpal[i].palette == p) winpal[i].window = 0; if (p == app_palette) app_palette = 0; DisposeHandle(p); }
static void T_ActivatePalette(uint16_t w) { (void)w; activate_palette(pop32()); }
static void T_SetPalette(uint16_t w) { (void)w; pop16(); maddr pal = pop32(); maddr win = pop32(); set_palette(win, pal); }
static void T_NSetPalette(uint16_t w) { (void)w; pop16(); maddr pal = pop32(); maddr win = pop32(); set_palette(win, pal); }
static void T_GetPalette(uint16_t w) { (void)w; ret32(palette_for(pop32())); }
static void T_CopyPalette(uint16_t w) {
    (void)w; int16_t dlen = pops16(), dst = pops16(), src = pops16(); maddr dp = pop32(), sp = pop32();
    (void)dlen;
    for (int i = 0; i < dlen; i++) mcopy(rd32(dp) + PAL_HDR + 16 * (dst + i), rd32(sp) + PAL_HDR + 16 * (src + i), 16);
}
static void T_PmForeColor(uint16_t w) { (void)w; int16_t e = pops16(); maddr p = qd_the_port(); wr32(p + GP_fgColor, (uint32_t)e); }
static void T_PmBackColor(uint16_t w) { (void)w; int16_t e = pops16(); maddr p = qd_the_port(); wr32(p + GP_bkColor, (uint32_t)e); }
static void T_AnimateEntry(uint16_t w) {
    (void)w; maddr rgb = pop32(); int16_t e = pops16(); maddr win = pop32();
    maddr pal = palette_for(win);
    if (pal && rd32(pal)) mcopy(rd32(pal) + PAL_HDR + 16 * e, rgb, 6);
    mcopy(rd32(device_ctab) + CT_table + 8 * (e & 255) + 2, rgb, 6);
    clut_changed();
}
static void T_AnimatePalette(uint16_t w) {
    (void)w; int16_t len = pops16(), dst = pops16(), src = pops16(); maddr ct = pop32(); maddr win = pop32();
    maddr pal = palette_for(win);
    maddr c = rd32(ct), dev = rd32(device_ctab);
    for (int i = 0; i < len; i++) {
        int di = dst + i; if (di < 0 || di > 255) continue;
        maddr s = c + CT_table + 8 * (src + i) + 2;
        if (pal && rd32(pal) && di < rd16(rd32(pal))) mcopy(rd32(pal) + PAL_HDR + 16 * di, s, 6);
        mcopy(dev + CT_table + 8 * di + 2, s, 6);
    }
    clut_changed();
}
static void T_GetEntryColor(uint16_t w) { (void)w; maddr rgb = pop32(); int16_t e = pops16(); maddr pal = pop32(); mcopy(rgb, rd32(pal) + PAL_HDR + 16 * e, 6); }
static void T_SetEntryColor(uint16_t w) { (void)w; maddr rgb = pop32(); int16_t e = pops16(); maddr pal = pop32(); mcopy(rd32(pal) + PAL_HDR + 16 * e, rgb, 6); }
static void T_GetEntryUsage(uint16_t w) { (void)w; maddr tol = pop32(), use = pop32(); int16_t e = pops16(); maddr pal = pop32(); maddr en = rd32(pal) + PAL_HDR + 16 * e; wr16(use, rd16(en + 6)); wr16(tol, rd16(en + 8)); }
static void T_SetEntryUsage(uint16_t w) {
    (void)w; int16_t tol = pops16(), use = pops16(), e = pops16(); maddr pal = pop32();
    maddr en = rd32(pal) + PAL_HDR + 16 * e;
    if (use != -1) wr16(en + 6, (uint16_t)use);
    if (tol != -1) wr16(en + 8, (uint16_t)tol);
}
static void T_CTab2Palette(uint16_t w) {
    (void)w; int16_t tol = pops16(), use = pops16(); maddr pal = pop32(), ct = pop32();
    int n = rd16(rd32(ct) + CT_size) + 1;
    SetHandleSize(pal, PAL_HDR + 16u * n);
    maddr p = rd32(pal);
    wr16(p, (uint16_t)n);
    for (int i = 0; i < n; i++) {
        maddr e = p + PAL_HDR + 16 * i;
        mcopy(e, rd32(ct) + CT_table + 8 * i + 2, 6);
        wr16(e + 6, (uint16_t)use); wr16(e + 8, (uint16_t)tol);
    }
}
static void T_Palette2CTab(uint16_t w) {
    (void)w; maddr ct = pop32(), pal = pop32();
    int n = rd16(rd32(pal));
    SetHandleSize(ct, 8 + 8u * n);
    maddr c = rd32(ct);
    wr32(c + CT_seed, rd32(c + CT_seed) + 1); wr16(c + CT_flags, 0); wr16(c + CT_size, (uint16_t)(n - 1));
    for (int i = 0; i < n; i++) { wr16(c + CT_table + 8 * i, (uint16_t)i); mcopy(c + CT_table + 8 * i + 2, rd32(pal) + PAL_HDR + 16 * i, 6); }
}
static void T_GetNewCWindow_palette_hook(void) {}

/* Random: Park-Miller on qd.randSeed */
static void T_Random(uint16_t w) {
    (void)w;
    maddr seedp = qd_globals - 126;
    uint32_t s = rd32(seedp);
    uint64_t v = (uint64_t)(s ? s : 1) * 16807u % 0x7FFFFFFFu;
    wr32(seedp, (uint32_t)v);
    int16_t r = (int16_t)(v & 0xFFFF);
    if ((uint16_t)r == 0x8000) r = 0;
    ret16((uint16_t)r);
}

void traps_qd_draw_init(void);
void traps_qd_init(void) {
    qd_init_screen(640, 480);
    trap_register(TRAP_InitGraf, T_InitGraf);
    trap_register(TRAP_OpenPort, T_OpenPort);
    trap_register(TRAP_InitPort, T_InitPort);
    trap_register(TRAP_OpenCPort, T_OpenCPort);
    trap_register(TRAP_InitCPort, T_InitCPort);
    trap_register(TRAP_ClosePort, T_ClosePort);
    trap_register(TRAP_CloseCPort, T_ClosePort);
    trap_register(TRAP_SetPort, T_SetPort);
    trap_register(TRAP_GetPort, T_GetPort);
    trap_register(TRAP_SetPortBits, T_SetPortBits);
    trap_register(TRAP_PortSize, T_PortSize);
    trap_register(TRAP_MovePortTo, T_MovePortTo);
    trap_register(TRAP_SetOrigin, T_SetOrigin);
    trap_register(TRAP_ClipRect, T_ClipRect);
    trap_register(TRAP_SetClip, T_SetClip);
    trap_register(TRAP_GetClip, T_GetClip);
    trap_register(TRAP_LocalToGlobal, T_LocalToGlobal);
    trap_register(TRAP_GlobalToLocal, T_GlobalToLocal);
    trap_register(TRAP_RGBForeColor, T_RGBForeColor);
    trap_register(TRAP_RGBBackColor, T_RGBBackColor);
    trap_register(TRAP_GetForeColor, T_GetForeColor);
    trap_register(TRAP_GetBackColor, T_GetBackColor);
    trap_register(TRAP_ForeColor, T_ForeColor);
    trap_register(TRAP_BackColor, T_BackColor);
    trap_register(TRAP_Color2Index, T_Color2Index);
    trap_register(TRAP_Index2Color, T_Index2Color);
    trap_register(TRAP_RealColor, T_RealColor);
    trap_register(TRAP_InvertColor, T_InvertColor);
    trap_register(TRAP_GetMainDevice, T_GetMainDevice);
    trap_register(TRAP_GetGDevice, T_GetGDevice);
    trap_register(TRAP_SetGDevice, T_SetGDevice);
    trap_register(TRAP_GetDeviceList, T_GetDeviceList);
    trap_register(TRAP_GetNextDevice, T_GetNextDevice);
    trap_register(TRAP_TestDeviceAttribute, T_TestDeviceAttribute);
    trap_register(TRAP_GetMaxDevice, T_GetMaxDevice);
    trap_register(TRAP_GetCTSeed, T_GetCTSeed);
    trap_register(TRAP_MakeITable, T_MakeITable);
    trap_register(TRAP_ProtectEntry, T_ProtectEntry);
    trap_register(TRAP_ReserveEntry, T_ReserveEntry);
    trap_register(TRAP_GetCTable, T_GetCTable);
    trap_register(TRAP_DisposeCTable, T_DisposeCTable);
    trap_register(TRAP_SetEntries, T_SetEntries);
    trap_register(TRAP_SaveEntries, T_SaveEntries);
    trap_register(TRAP_RestoreEntries, T_RestoreEntries);
    trap_register(TRAP_QDExtensions, T_QDExtensions);
    trap_register(TRAP_PaletteDispatch, T_PaletteDispatch);
    trap_register(TRAP_NewPalette, T_NewPalette);
    trap_register(TRAP_GetNewPalette, T_GetNewPalette);
    trap_register(TRAP_DisposePalette, T_DisposePalette);
    trap_register(TRAP_ActivatePalette, T_ActivatePalette);
    trap_register(TRAP_NSetPalette, T_NSetPalette);
    trap_register(TRAP_GetPalette, T_GetPalette);
    trap_register(TRAP_CopyPalette, T_CopyPalette);
    trap_register(TRAP_PmForeColor, T_PmForeColor);
    trap_register(TRAP_PmBackColor, T_PmBackColor);
    trap_register(TRAP_AnimateEntry, T_AnimateEntry);
    trap_register(TRAP_AnimatePalette, T_AnimatePalette);
    trap_register(TRAP_GetEntryColor, T_GetEntryColor);
    trap_register(TRAP_SetEntryColor, T_SetEntryColor);
    trap_register(TRAP_GetEntryUsage, T_GetEntryUsage);
    trap_register(TRAP_SetEntryUsage, T_SetEntryUsage);
    trap_register(TRAP_CTab2Palette, T_CTab2Palette);
    trap_register(TRAP_Palette2CTab, T_Palette2CTab);
    trap_register(TRAP_Random, T_Random);
    (void)T_GetNewCWindow_palette_hook;
    traps_qd_draw_init();
    font_init();
}

maddr qd_window_palette(maddr window) { return palette_for(window); }
void qd_activate_window_palette(maddr window) { activate_palette(window); }
