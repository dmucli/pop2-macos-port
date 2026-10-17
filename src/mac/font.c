/* Font Manager / text drawing: Mac bitmap fonts (NFNT/FONT via FOND) with a host fallback
 * when the requested family isn't available: CoreText (Geneva) on macOS, elsewhere a
 * built-in bitmap font (Spleen 6x12, scaled by whole multiples). */
#include "qd.h"
#include "trapnums.h"
#include "../fs/vfs.h"
#include <stdio.h>
#include <stdlib.h>
#if defined(__APPLE__) && !defined(POP2_BUILTIN_FONT)
#include <CoreText/CoreText.h>
#include <CoreGraphics/CoreGraphics.h>
#else
#include <string.h>
#include "hostfont.h"
#endif

maddr qd_port_bits(maddr port);

typedef struct {
    int valid;
    uint8_t *rec; uint32_t len;       /* host copy of the FONT/NFNT record */
    int first, last, ascent, descent, leading, widmax, kernmax, fheight, rowwords;
    int size;                          /* nominal point size */
} BmFont;

typedef struct { int family, size, face; BmFont f; double scale; } CacheEnt;
static CacheEnt cache[32];
static int cache_n;

static int16_t b16s(const uint8_t *p) { return (int16_t)be16(p); }

static int load_font_rec(int id, BmFont *f) {
    maddr h = GetResource(FOURCC('N','F','N','T'), (int16_t)id);
    if (!h) h = GetResource(FOURCC('F','O','N','T'), (int16_t)id);
    if (!h || !rd32(h)) return 0;
    uint32_t len = GetHandleSize(h);
    f->rec = malloc(len); mget(f->rec, rd32(h), len); f->len = len;
    const uint8_t *r = f->rec;
    f->first = b16s(r + 2); f->last = b16s(r + 4); f->widmax = b16s(r + 6); f->kernmax = b16s(r + 8);
    f->fheight = b16s(r + 14); f->ascent = b16s(r + 18); f->descent = b16s(r + 20); f->leading = b16s(r + 22);
    f->rowwords = b16s(r + 24);
    f->valid = 1;
    return 1;
}

/* Choose (font id, nominal size) for family/size/face through FOND, or old FONT ids. */
static int find_font(int family, int size, int face, BmFont *out) {
    if (family == 1 && !GetResource(FOURCC('F','O','N','D'), 1)) family = 0;
    if (size == 0) size = 12;
    maddr fond = GetResource(FOURCC('F','O','N','D'), (int16_t)family);
    if (fond && rd32(fond)) {
        maddr p = rd32(fond);
        int n = rd16(p + 52) + 1;
        int best = -1, bestd = 1 << 30, bestsize = 0;
        for (int pass = 0; pass < 2 && best < 0; pass++)
            for (int i = 0; i < n; i++) {
                maddr e = p + 54 + 6 * i;
                int sz = rds16(e), st = rds16(e + 2), id = rds16(e + 4);
                if (sz == 0) continue;               /* outline font */
                if (pass == 0 && st != (face & 0x03)) continue;
                if (pass == 1 && st != 0) continue;
                int d = abs(sz - size) * 4 + (sz < size);
                if (d < bestd) { bestd = d; best = id; bestsize = sz; }
            }
        if (best >= 0 && load_font_rec(best, out)) { out->size = bestsize; return 1; }
    }
    /* old style FONT numbering */
    if (family < 256 && load_font_rec(family * 128 + size, out)) { out->size = size; return 1; }
    return 0;
}

#if defined(__APPLE__) && !defined(POP2_BUILTIN_FONT)
/* ---- host fallback (CoreText) ---- */
typedef struct { int size, face; CTFontRef font; int ascent, descent, leading; } HostFont;
static HostFont hostfonts[8]; static int hostfont_n;
static HostFont *host_font(int size, int face) {
    if (size <= 0) size = 12;
    for (int i = 0; i < hostfont_n; i++) if (hostfonts[i].size == size && hostfonts[i].face == face) return &hostfonts[i];
    if (hostfont_n == 8) hostfont_n = 0;
    HostFont *h = &hostfonts[hostfont_n++];
    CFStringRef name = CFStringCreateWithCString(NULL, (face & 1) ? "Geneva-Bold" : "Geneva", kCFStringEncodingUTF8);
    h->font = CTFontCreateWithName(name, size, NULL);
    CFRelease(name);
    h->size = size; h->face = face;
    h->ascent = (int)(CTFontGetAscent(h->font) + 0.5);
    h->descent = (int)(CTFontGetDescent(h->font) + 0.5);
    h->leading = (int)(CTFontGetLeading(h->font) + 0.5);
    return h;
}

/* Render text with CoreText into an 8-bit coverage buffer; returns advance width. */
static int host_render(HostFont *hf, const uint8_t *text, int len, uint8_t **cov, int *cw, int *ch) {
    CFStringRef s = CFStringCreateWithBytes(NULL, text, len, kCFStringEncodingMacRoman, false);
    if (!s) return 0;
    CFStringRef keys[] = { kCTFontAttributeName };
    CFTypeRef vals[] = { hf->font };
    CFDictionaryRef attr = CFDictionaryCreate(NULL, (const void **)keys, (const void **)vals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as = CFAttributedStringCreate(NULL, s, attr);
    CTLineRef line = CTLineCreateWithAttributedString(as);
    double w = CTLineGetTypographicBounds(line, NULL, NULL, NULL);
    int width = (int)(w + 0.5);
    if (cov) {
        int W = width + 2, H = hf->ascent + hf->descent + 1;
        *cw = W; *ch = H;
        *cov = calloc((size_t)W * H, 1);
        CGColorSpaceRef gs = CGColorSpaceCreateDeviceGray();
        CGContextRef ctx = CGBitmapContextCreate(*cov, W, H, 8, W, gs, kCGImageAlphaNone);
        CGContextSetShouldAntialias(ctx, false);
        CGContextSetGrayFillColor(ctx, 1.0, 1.0);
        CGContextSetTextPosition(ctx, 0, hf->descent);
        CTLineDraw(line, ctx);
        CGContextRelease(ctx);
        CGColorSpaceRelease(gs);
    }
    CFRelease(line); CFRelease(as); CFRelease(attr); CFRelease(s);
    return width;
}
#else
/* ---- host fallback (built-in bitmap font) ---- */
typedef struct { int size, face, scale; int ascent, descent, leading; } HostFont;
static HostFont hostfonts[8]; static int hostfont_n;
static HostFont *host_font(int size, int face) {
    if (size <= 0) size = 12;
    for (int i = 0; i < hostfont_n; i++) if (hostfonts[i].size == size && hostfonts[i].face == face) return &hostfonts[i];
    if (hostfont_n == 8) hostfont_n = 0;
    HostFont *h = &hostfonts[hostfont_n++];
    h->size = size; h->face = face;
    h->scale = (size + HOSTFONT_H / 2) / HOSTFONT_H;
    if (h->scale < 1) h->scale = 1;
    h->ascent = (HOSTFONT_H - HOSTFONT_DESCENT) * h->scale;
    h->descent = HOSTFONT_DESCENT * h->scale;
    h->leading = h->scale;
    return h;
}

/* Render text into an 8-bit coverage buffer (rows from the top of the ascent); returns
 * the advance width. Bold draws each pixel twice, one pixel apart. */
static int host_render(HostFont *hf, const uint8_t *text, int len, uint8_t **cov, int *cw, int *ch) {
    int sc = hf->scale, bold = hf->face & 1;
    int adv = HOSTFONT_W * sc + bold;
    int width = len * adv;
    if (cov) {
        int W = width + 2, H = hf->ascent + hf->descent + 1;
        *cw = W; *ch = H;
        *cov = calloc((size_t)W * H, 1);
        for (int k = 0; k < len; k++) {
            int c = text[k] < 0x20 ? '?' : text[k];
            const uint8_t *g = hostfont[c - 0x20];
            for (int r = 0; r < HOSTFONT_H; r++)
                for (int b = 0; b < HOSTFONT_W; b++) {
                    if (!(g[r] & (0x80 >> b))) continue;
                    for (int yy = 0; yy < sc; yy++)
                        for (int xx = 0; xx < sc + bold; xx++)
                            (*cov)[(r * sc + yy) * W + k * adv + b * sc + xx] = 255;
                }
        }
    }
    return width;
}
#endif

/* ---- port font resolution ---- */
typedef struct { BmFont *bm; HostFont *host; double scale; int face, size; } FontSel;

static FontSel select_font(maddr port) {
    FontSel fs = {0};
    int family = rds16(port + GP_txFont), size = rds16(port + GP_txSize), face = rd8(port + GP_txFace);
    if (family == 0 && size == 0) size = 12;
    if (size == 0) size = 12;
    fs.face = face; fs.size = size;
    for (int i = 0; i < cache_n; i++)
        if (cache[i].family == family && cache[i].size == size && cache[i].face == (face & 3)) {
            if (cache[i].f.valid) { fs.bm = &cache[i].f; fs.scale = cache[i].scale; return fs; }
            fs.host = host_font(size, face); return fs;
        }
    CacheEnt *c;
    if (cache_n < 32) c = &cache[cache_n++]; else { c = &cache[0]; free(c->f.rec); }
    memset(c, 0, sizeof *c);
    c->family = family; c->size = size; c->face = face & 3;
    if (find_font(family, size, face, &c->f)) {
        c->scale = (double)size / c->f.size;
        if (c->scale > 0.9 && c->scale < 1.1) c->scale = 1.0;
        fs.bm = &c->f; fs.scale = c->scale;
    } else fs.host = host_font(size, face);
    return fs;
}

static int glyph_info(BmFont *f, int c, int *x0, int *x1, int *off, int *wid) {
    const uint8_t *r = f->rec;
    int nglyph = f->last - f->first + 1;
    int i = (c < f->first || c > f->last) ? nglyph : c - f->first;
    const uint8_t *loc = r + 26 + 2 * f->rowwords * f->fheight;
    uint32_t owt = 16 + 2u * be16(r + 16);
    if (owt + 2u * (i + 1) > f->len) return 0;
    uint16_t ow = be16(r + owt + 2 * i);
    if (ow == 0xFFFF) {
        i = nglyph; ow = be16(r + owt + 2 * i);
        if (ow == 0xFFFF) return 0;
    }
    *x0 = be16(loc + 2 * i); *x1 = be16(loc + 2 * (i + 1));
    *off = (ow >> 8) + f->kernmax; *wid = ow & 0xFF;
    return 1;
}

static int bm_width(FontSel *fs, const uint8_t *t, int n, int spextra) {
    int w = 0;
    for (int k = 0; k < n; k++) {
        int x0, x1, off, wid;
        if (!glyph_info(fs->bm, t[k], &x0, &x1, &off, &wid)) continue;
        w += wid + ((fs->face & 1) ? 1 : 0) + (t[k] == ' ' ? spextra : 0);
    }
    return (int)(w * fs->scale + 0.5);
}

int font_text_width(maddr port, const uint8_t *t, int n) {
    FontSel fs = select_font(port);
    int sp = (int)((int32_t)rd32(port + GP_spExtra) >> 16);
    if (fs.bm) return bm_width(&fs, t, n, sp);
    return host_render(fs.host, t, n, NULL, NULL, NULL);
}

void font_get_info(maddr port, int *a, int *d, int *wm, int *l) {
    FontSel fs = select_font(port);
    if (fs.bm) {
        *a = (int)(fs.bm->ascent * fs.scale + 0.5); *d = (int)(fs.bm->descent * fs.scale + 0.5);
        *wm = (int)(fs.bm->widmax * fs.scale + 0.5) + ((fs.face & 1) ? 1 : 0); *l = fs.bm->leading;
    } else {
        *a = fs.host->ascent; *d = fs.host->descent; *wm = fs.size; *l = fs.host->leading;
    }
}

/* Plot glyph coverage into the port with the text transfer mode. */
static void plot(Surface *s, const RgnList *clip, int x, int y, int fg, int bk, int mode, int on) {
    if (y < s->bounds.top || y >= s->bounds.bottom || x < s->bounds.left || x >= s->bounds.right) return;
    if (!rgn_contains(clip, y, x)) return;
    maddr a = s->base + (uint32_t)(y - s->bounds.top) * s->rowbytes + (uint32_t)(x - s->bounds.left);
    if (s->depth == 1) {
        int bx = x - s->bounds.left;
        a = s->base + (uint32_t)(y - s->bounds.top) * s->rowbytes + (bx >> 3);
        uint8_t m = (uint8_t)(0x80 >> (bx & 7));
        if (on && (mode == 0 || mode == 1)) wr8(a, rd8(a) | m);
        else if (!on && mode == 0) wr8(a, rd8(a) & ~m);
        else if (on && mode == 2) wr8(a, rd8(a) ^ m);
        else if (on && mode == 3) wr8(a, rd8(a) & ~m);
        return;
    }
    switch (mode & 0x3F) {
        case 0: wr8(a, (uint8_t)(on ? fg : bk)); break;
        case 1: case 36: if (on) wr8(a, (uint8_t)fg); break;
        case 2: if (on) wr8(a, (uint8_t)~rd8(a)); break;
        case 3: if (on) wr8(a, (uint8_t)bk); break;
        case 4: wr8(a, (uint8_t)(on ? bk : fg)); break;
        default: if (on) wr8(a, (uint8_t)fg); break;
    }
}

void font_draw_text(maddr port, const uint8_t *t, int n) {
    FontSel fs = select_font(port);
    Surface s = qd_port_surface(port);
    RgnList clip; qd_port_clip(port, &clip);
    int fg = qd_port_fg_index(port), bk = qd_port_bk_index(port);
    int mode = rds16(port + GP_txMode);
    QPoint pen = rd_pt(port + GP_pnLoc);
    int sp = (int)((int32_t)rd32(port + GP_spExtra) >> 16);
    int bold = fs.face & 1, italic = fs.face & 2, under = fs.face & 4;
    int x = pen.h;
    if (fs.bm) {
        BmFont *f = fs.bm;
        const uint8_t *img = f->rec + 26;
        int rowbytes = f->rowwords * 2;
        double sc = fs.scale;
        int asc = (int)(f->ascent * sc + 0.5), h = (int)(f->fheight * sc + 0.5);
        for (int k = 0; k < n; k++) {
            int x0, x1, off, wid;
            if (!glyph_info(f, t[k], &x0, &x1, &off, &wid)) continue;
            int gw = (int)((x1 - x0) * sc + 0.5);
            for (int gy = 0; gy < h; gy++) {
                int sy = (int)(gy / sc);
                int dy = pen.v - asc + gy;
                int slant = italic ? (asc - gy) / 2 : 0;
                for (int gx = 0; gx < gw; gx++) {
                    int sx = x0 + (int)(gx / sc);
                    int on = (img[sy * rowbytes + (sx >> 3)] >> (7 - (sx & 7))) & 1;
                    int dx = x + (int)(off * sc) + gx + slant;
                    if (on || mode == 0) plot(&s, &clip, dx, dy, fg, bk, mode, on);
                    if (bold && on) plot(&s, &clip, dx + 1, dy, fg, bk, mode == 0 ? 1 : mode, 1);
                }
            }
            x += (int)((wid + bold + (t[k] == ' ' ? sp : 0)) * sc + 0.5);
        }
    } else {
        uint8_t *cov; int cw, ch;
        int w = host_render(fs.host, t, n, &cov, &cw, &ch);
        for (int gy = 0; gy < ch; gy++)
            for (int gx = 0; gx < cw; gx++) {
                int on = cov[gy * cw + gx] > 127;
                if (on || mode == 0) plot(&s, &clip, x + gx, pen.v - fs.host->ascent + gy, fg, bk, mode, on);
            }
        free(cov);
        x += w;
    }
    if (under) for (int ux = pen.h; ux < x; ux++) plot(&s, &clip, ux, pen.v + 1, fg, bk, 1, 1);
    wr16(port + GP_pnLoc + 2, (uint16_t)x);
    rgn_free(&clip);
    qd_mark_dirty(&s);
}

/* ---- Font Manager traps ---- */
static void T_InitFonts(uint16_t w) { (void)w; }
static void T_GetFNum(uint16_t w) {
    (void)w; maddr out = pop32(); char name[256]; pstr_to_c(pop32(), name, sizeof name);
    int16_t id = 0;
    if (!mac_strcasecmp(name, "Geneva")) id = 1;
    else if (!mac_strcasecmp(name, "Monaco")) id = 4;
    else if (!mac_strcasecmp(name, "Chicago")) id = 0;
    else {
        for (int i = 1; ; i++) {
            maddr h = GetIndResource(FOURCC('F','O','N','D'), (int16_t)i);
            if (!h) break;
            int16_t rid; uint32_t t; char n[256];
            GetResInfo(h, &rid, &t, n);
            if (!mac_strcasecmp(n, name)) { id = rid; break; }
        }
    }
    wr16(out, (uint16_t)id);
}
static void T_GetFontName(uint16_t w) {
    (void)w; maddr out = pop32(); int16_t fam = pops16();
    maddr h = GetResource(FOURCC('F','O','N','D'), fam);
    char n[256] = "";
    if (h) { int16_t rid; uint32_t t; GetResInfo(h, &rid, &t, n); }
    else if (fam == 0) strcpy(n, "Chicago"); else if (fam == 1) strcpy(n, "Geneva");
    c_to_pstr(n, out);
}
static void T_RealFont(uint16_t w) { (void)w; int16_t sz = pops16(), fam = pops16(); BmFont f = {0}; int ok = find_font(fam, sz, 0, &f) && f.size == sz; free(f.rec); retbool(ok); }
static void T_SetFontLock(uint16_t w) { (void)w; pop16(); }
static void T_SetFScaleDisable(uint16_t w) { (void)w; pop16(); }
static void T_SetFractEnable(uint16_t w) { (void)w; pop16(); }
static void T_FontMetrics(uint16_t w) {
    (void)w; maddr fm = pop32(); int a, d, wm, l; font_get_info(qd_the_port(), &a, &d, &wm, &l);
    wr32(fm, (uint32_t)a << 16); wr32(fm + 4, (uint32_t)(-d) << 16); wr32(fm + 8, (uint32_t)l << 16); wr32(fm + 12, (uint32_t)wm << 16); wr32(fm + 16, 0);
}

void font_flush_cache(void) {
    for (int i = 0; i < cache_n; i++) free(cache[i].f.rec);
    cache_n = 0;
}

void font_init(void) {
    trap_register(TRAP_InitFonts, T_InitFonts);
    trap_register(TRAP_GetFNum, T_GetFNum);
    trap_register(TRAP_GetFontName, T_GetFontName);
    trap_register(TRAP_RealFont, T_RealFont);
    trap_register(TRAP_SetFontLock, T_SetFontLock);
    trap_register(TRAP_SetFScaleDisable, T_SetFScaleDisable);
    trap_register(TRAP_SetFractEnable, T_SetFractEnable);
    trap_register(TRAP_FontMetrics, T_FontMetrics);
}
