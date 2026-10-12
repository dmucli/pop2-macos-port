/* QuickDraw drawing: shapes, CopyBits, regions, rect utilities, pen. */
#include "qd.h"
#include "trapnums.h"
#include <stdio.h>
#include <stdlib.h>

maddr qd_port_bits(maddr port);
void  qd_port_pattern(maddr port, int which, uint8_t out[8]);
void  qd_port_set_pattern(maddr port, int which, const uint8_t pat[8]);

/* Resolve BitMap*, PixMap*, or &CGrafPort.portPixMap (handle + 0xC000 version word). */
static maddr resolve_bits(maddr bm) {
    if ((rd16(bm + 4) & 0xC000) == 0xC000) return rd32(rd32(bm));
    return bm;
}

static inline maddr px_addr(const Surface *s, int x, int y) {
    return s->base + (uint32_t)(y - s->bounds.top) * s->rowbytes + (uint32_t)(x - s->bounds.left);
}
static inline int get_px(const Surface *s, int x, int y) {
    if (s->depth == 1) {
        int bx = x - s->bounds.left;
        return (rd8(s->base + (uint32_t)(y - s->bounds.top) * s->rowbytes + (bx >> 3)) >> (7 - (bx & 7))) & 1;
    }
    return rd8(px_addr(s, x, y));
}
static inline void put_px(Surface *s, int x, int y, int v) {
    if (s->depth == 1) {
        int bx = x - s->bounds.left;
        maddr a = s->base + (uint32_t)(y - s->bounds.top) * s->rowbytes + (bx >> 3);
        uint8_t m = (uint8_t)(0x80 >> (bx & 7));
        wr8(a, v ? (rd8(a) | m) : (rd8(a) & ~m));
        return;
    }
    wr8(px_addr(s, x, y), (uint8_t)v);
}

/* For 1-bit destinations colors are 0 (white) / 1 (black). */
static int dst_color(const Surface *s, int index) {
    if (s->depth != 1) return index;
    uint32_t rgb = qd_device_rgb(index);
    int lum = ((rgb >> 16) * 3 + ((rgb >> 8) & 0xFF) * 6 + (rgb & 0xFF)) / 10;
    return lum < 128;
}

void qd_fill_rect_pattern(Surface *s, const RgnList *clip, QRect r, const uint8_t pat[8], int fg, int bk, int mode) {
    r = rect_sect(r, s->bounds);
    if (rect_empty(r)) return;
    fg = dst_color(s, fg); bk = dst_color(s, bk);
    int solid = 1;
    for (int i = 0; i < 8; i++) if (pat[i] != pat[0] || (pat[0] != 0 && pat[0] != 0xFF)) solid = 0;
    mode &= 0x3F;
    for (int k = 0; k < clip->n; k++) {
        QRect c = rect_sect(r, clip->r[k]);
        if (rect_empty(c)) continue;
        for (int y = c.top; y < c.bottom; y++) {
            uint8_t row = pat[(y - s->bounds.top) & 7];
            if (solid && s->depth == 8 && (mode == 8 || mode == 0)) {
                int v = (row & 0x80) ? fg : bk;
                mset(px_addr(s, c.left, y), v, (uint32_t)(c.right - c.left));
                continue;
            }
            for (int x = c.left; x < c.right; x++) {
                int bit = (row >> (7 - ((x - s->bounds.left) & 7))) & 1;
                switch (mode) {
                    case 0: case 8:  put_px(s, x, y, bit ? fg : bk); break;            /* patCopy */
                    case 1: case 9:  if (bit) put_px(s, x, y, fg); break;               /* patOr */
                    case 2: case 10: if (bit) put_px(s, x, y, s->depth == 1 ? !get_px(s, x, y) : (~get_px(s, x, y)) & 0xFF); break; /* patXor */
                    case 3: case 11: if (bit) put_px(s, x, y, bk); break;               /* patBic */
                    case 4: case 12: put_px(s, x, y, bit ? bk : fg); break;             /* notPatCopy */
                    case 5: case 13: if (!bit) put_px(s, x, y, fg); break;              /* notPatOr */
                    case 6: case 14: if (!bit) put_px(s, x, y, s->depth == 1 ? !get_px(s, x, y) : (~get_px(s, x, y)) & 0xFF); break;
                    case 7: case 15: if (!bit) put_px(s, x, y, bk); break;
                    default: put_px(s, x, y, bit ? fg : bk); break;
                }
            }
        }
    }
    qd_mark_dirty(s);
}

void qd_fill_rect_index(Surface *s, const RgnList *clip, QRect r, int index) {
    static const uint8_t solid[8] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    qd_fill_rect_pattern(s, clip, r, solid, index, index, 8);
}

/* ---- shape drawing on the current port ---- */
enum { V_FRAME, V_PAINT, V_ERASE, V_INVERT, V_FILL };

static void port_fill(maddr port, const RgnList *shape, int verb, const uint8_t *fillpat) {
    if (verb == V_FRAME) return;
    Surface s = qd_port_surface(port);
    RgnList clip, area;
    qd_port_clip(port, &clip);
    rgn_op(shape, &clip, &area, 0);
    uint8_t pat[8]; int fg = qd_port_fg_index(port), bk = qd_port_bk_index(port), mode = 8;
    switch (verb) {
        case V_PAINT: qd_port_pattern(port, 1, pat); mode = rd16(port + GP_pnMode); if (rd16(port + GP_pnVis) & 0x8000) goto out; break;
        case V_ERASE: qd_port_pattern(port, 0, pat); fg = bk; { uint8_t inv[8]; for (int i = 0; i < 8; i++) inv[i] = 0xFF; memcpy(pat, inv, 8); } break;
        case V_INVERT: memset(pat, 0xFF, 8); mode = 10; break;
        case V_FILL: memcpy(pat, fillpat, 8); mode = 8; break;
    }
    for (int i = 0; i < area.n; i++) {
        RgnList one = { 1, &area.r[i], area.r[i] };
        RgnList all = { 1, &area.r[i], area.r[i] };
        (void)one;
        qd_fill_rect_pattern(&s, &all, area.r[i], pat, fg, bk, mode);
    }
out:
    rgn_free(&clip); rgn_free(&area);
}

/* Frame: draw the outline with the pen size */
static void frame_rgn(maddr port, const RgnList *shape) {
    if (rd16(port + GP_pnVis) & 0x8000) return;
    QPoint ps = rd_pt(port + GP_pnSize);
    RgnList inner, ring, tmp;
    tmp = *shape;
    /* inner = shape inset by pen size: approximate with offset intersections */
    RgnList a, b, c, d, ab, cd;
    a = (RgnList){0}; b = (RgnList){0}; c = (RgnList){0}; d = (RgnList){0};
    rgn_op(&tmp, &tmp, &a, 0); rgn_offset(&a, ps.v, 0);
    rgn_op(&tmp, &tmp, &b, 0); rgn_offset(&b, -ps.v, 0);
    rgn_op(&tmp, &tmp, &c, 0); rgn_offset(&c, 0, ps.h);
    rgn_op(&tmp, &tmp, &d, 0); rgn_offset(&d, 0, -ps.h);
    rgn_op(&a, &b, &ab, 0); rgn_op(&c, &d, &cd, 0);
    RgnList i1; rgn_op(&ab, &cd, &i1, 0);
    rgn_op(&i1, &tmp, &inner, 0);
    rgn_op(&tmp, &inner, &ring, 2);
    port_fill(port, &ring, V_PAINT, NULL);
    rgn_free(&a); rgn_free(&b); rgn_free(&c); rgn_free(&d); rgn_free(&ab); rgn_free(&cd); rgn_free(&i1);
    rgn_free(&inner); rgn_free(&ring);
}

static void shape_rect(maddr port, QRect r, int verb, const uint8_t *pat) {
    RgnList l; rgn_from_rect(&l, r);
    if (verb == V_FRAME) frame_rgn(port, &l); else port_fill(port, &l, verb, pat);
    rgn_free(&l);
}

static void oval_rgn(QRect r, RgnList *out) {
    int h = r.bottom - r.top, w = r.right - r.left;
    out->n = 0; out->r = malloc(sizeof(QRect) * (h > 0 ? h : 1));
    if (h <= 0 || w <= 0) { out->n = 0; out->bbox = (QRect){0}; return; }
    double cy = (r.top + r.bottom) / 2.0, cx = (r.left + r.right) / 2.0, ry = h / 2.0, rx = w / 2.0;
    for (int y = r.top; y < r.bottom; y++) {
        double dy = (y + 0.5 - cy) / ry;
        double k = 1 - dy * dy; if (k < 0) k = 0;
        int half = (int)(rx * __builtin_sqrt(k) + 0.5);
        QRect row = { (int16_t)y, (int16_t)(cx - half), (int16_t)(y + 1), (int16_t)(cx + half) };
        if (row.right > row.left) out->r[out->n++] = row;
    }
    RgnList t; rgn_op(out, out, &t, 1); rgn_free(out); *out = t;
}
static void rrect_rgn(QRect r, int ow, int oh, RgnList *out) {
    int h = r.bottom - r.top;
    out->n = 0; out->r = malloc(sizeof(QRect) * (h > 0 ? h : 1));
    if (ow > r.right - r.left) ow = r.right - r.left;
    if (oh > h) oh = h;
    double ry = oh / 2.0, rx = ow / 2.0;
    for (int y = r.top; y < r.bottom; y++) {
        int inset = 0;
        double dy = 0;
        if (y < r.top + ry) dy = (r.top + ry - (y + 0.5)) / ry;
        else if (y >= r.bottom - ry) dy = ((y + 0.5) - (r.bottom - ry)) / ry;
        if (dy > 0 && ry > 0) { double k = 1 - dy * dy; if (k < 0) k = 0; inset = (int)(rx - rx * __builtin_sqrt(k) + 0.5); }
        QRect row = { (int16_t)y, (int16_t)(r.left + inset), (int16_t)(y + 1), (int16_t)(r.right - inset) };
        if (row.right > row.left) out->r[out->n++] = row;
    }
    RgnList t; rgn_op(out, out, &t, 1); rgn_free(out); *out = t;
}

/* ---- CopyBits ---- */
static uint8_t map_cache[256]; static maddr map_src, map_dst; static uint32_t map_sseed, map_dseed, map_ver;
static const uint8_t *color_map(maddr sct, maddr dct) {
    if (!sct || !dct || sct == dct || !rd32(sct) || !rd32(dct)) return NULL;
    uint32_t ss = rd32(rd32(sct) + CT_seed), ds = rd32(rd32(dct) + CT_seed);
    if (ss == ds) return NULL;
    if (sct == map_src && dct == map_dst && ss == map_sseed && ds == map_dseed && map_ver == qd_clut_version) return map_cache;
    maddr s = rd32(sct);
    int n = rd16(s + CT_size);
    for (int i = 0; i < 256; i++) {
        int k = i <= n ? i : n;
        maddr e = s + CT_table + 8 * k;
        int idx = (rd16(s + CT_flags) & 0x8000) ? k : rd16(e) & 0xFF;
        (void)idx;
        map_cache[i] = (uint8_t)qd_rgb_to_index(rd16(e + 2), rd16(e + 4), rd16(e + 6));
    }
    map_src = sct; map_dst = dct; map_sseed = ss; map_dseed = ds; map_ver = qd_clut_version;
    return map_cache;
}

void qd_copybits(maddr srcBits, maddr dstBits, QRect sr, QRect dr, int mode, maddr maskRgn, int fg, int bk, const RgnList *extra_clip) {
    Surface s = qd_bits_surface(resolve_bits(srcBits));
    Surface d = qd_bits_surface(resolve_bits(dstBits));
    if (rect_empty(sr) || rect_empty(dr)) return;
    RgnList clip; rgn_from_rect(&clip, rect_sect(dr, d.bounds));
    if (extra_clip) { RgnList t; rgn_op(&clip, extra_clip, &t, 0); rgn_free(&clip); clip = t; }
    if (maskRgn) { RgnList m, t; rgn_get(maskRgn, &m); rgn_op(&clip, &m, &t, 0); rgn_free(&clip); rgn_free(&m); clip = t; }
    const uint8_t *map = (s.depth == 8 && d.depth == 8) ? color_map(s.ctab, d.ctab) : NULL;
    int sw = sr.right - sr.left, sh = sr.bottom - sr.top, dw = dr.right - dr.left, dh = dr.bottom - dr.top;
    int scaled = (sw != dw || sh != dh);
    mode &= ~0x40;   /* ditherCopy */
    int dfg = dst_color(&d, fg), dbk = dst_color(&d, bk);
    for (int k = 0; k < clip.n; k++) {
        QRect c = clip.r[k];
        for (int y = c.top; y < c.bottom; y++) {
            int sy = scaled ? sr.top + (int)((int64_t)(y - dr.top) * sh / dh) : y - dr.top + sr.top;
            if (sy < s.bounds.top || sy >= s.bounds.bottom) continue;
            if (!scaled && mode == 0 && s.depth == 8 && d.depth == 8 && !map) {
                int x0 = c.left, x1 = c.right;
                int sx0 = x0 - dr.left + sr.left;
                if (sx0 < s.bounds.left) { x0 += s.bounds.left - sx0; sx0 = s.bounds.left; }
                if (sx0 + (x1 - x0) > s.bounds.right) x1 = x0 + (s.bounds.right - sx0);
                if (x1 > x0) memmove(mptr(px_addr(&d, x0, y)), mptr(px_addr(&s, sx0, sy)), (size_t)(x1 - x0));
                continue;
            }
            for (int x = c.left; x < c.right; x++) {
                int sx = scaled ? sr.left + (int)((int64_t)(x - dr.left) * sw / dw) : x - dr.left + sr.left;
                if (sx < s.bounds.left || sx >= s.bounds.right) continue;
                int v = get_px(&s, sx, sy);
                int out;
                if (s.depth == 1) {
                    /* 1-bit source: black -> fg, white -> bk */
                    switch (mode) {
                        case 0: out = v ? dfg : dbk; break;
                        case 1: if (!v) continue; out = dfg; break;
                        case 2: if (!v) continue; out = d.depth == 1 ? !get_px(&d, x, y) : (~get_px(&d, x, y)) & 0xFF; break;
                        case 3: if (!v) continue; out = dbk; break;
                        case 4: out = v ? dbk : dfg; break;
                        case 5: if (v) continue; out = dfg; break;
                        case 6: if (v) continue; out = d.depth == 1 ? !get_px(&d, x, y) : (~get_px(&d, x, y)) & 0xFF; break;
                        case 7: if (v) continue; out = dbk; break;
                        case 36: if (!v) continue; out = dfg; break;   /* transparent */
                        default: out = v ? dfg : dbk; break;
                    }
                } else {
                    if (map) v = map[v];
                    if (d.depth == 1) v = dst_color(&d, v);
                    switch (mode) {
                        case 0: out = v; break;
                        case 1: if (d.depth == 1 ? !v : v == 0) continue; out = v; break;
                        case 2: out = get_px(&d, x, y) ^ v; break;
                        case 3: if (v == 0) continue; out = dbk; break;
                        case 4: out = (~v) & 0xFF; break;
                        case 36: if (v == bk) continue; out = v; break;
                        default: out = v; break;
                    }
                }
                put_px(&d, x, y, out);
            }
        }
    }
    rgn_free(&clip);
    qd_mark_dirty(&d);
}

/* Is dstBits the current port's drawing surface? Then clip to the port. */
static int is_port_bits(maddr port, maddr bits) {
    if (!port) return 0;
    if (bits == port + GP_portBits) return 1;
    if (qd_is_color_port(port) && resolve_bits(bits) == rd32(rd32(port + GP_portPixMap))) return 1;
    return 0;
}

static maddr std_bits_thunk, std_text_thunk;
static void std_bits(maddr srcBits, QRect sr, QRect dr, int mode, maddr mask) {
    maddr port = qd_the_port();
    RgnList clip; qd_port_clip(port, &clip);
    qd_copybits(srcBits, qd_port_bits(port), sr, dr, mode, mask, qd_port_fg_index(port), qd_port_bk_index(port), &clip);
    rgn_free(&clip);
}

static maddr custom_proc(maddr port, int offset, maddr std) {
    maddr procs = rd32(port + GP_grafProcs);
    if (!procs) return 0;
    maddr p = rd32(procs + offset);
    return (p && p != std) ? p : 0;
}

void CopyBits(maddr src, maddr dst, QRect sr, QRect dr, int mode, maddr mask) {
    maddr port = qd_the_port();
    if (is_port_bits(port, dst)) {
        maddr proc = custom_proc(port, 32, std_bits_thunk);
        if (proc) {
            maddr tmp = cpu_get(R_SP) - 64;   /* scratch for rects below the stack */
            wr_rect(tmp, sr); wr_rect(tmp + 8, dr);
            uint32_t sp = cpu_get(R_SP);
            cpu_set(R_SP, tmp);
            push32(src); push32(tmp); push32(tmp + 8); push16((uint16_t)mode); push32(mask);
            cpu_call(proc, NULL);
            cpu_set(R_SP, sp);
            return;
        }
        std_bits(src, sr, dr, mode, mask);
        return;
    }
    qd_copybits(src, dst, sr, dr, mode, mask, port ? qd_port_fg_index(port) : 255, port ? qd_port_bk_index(port) : 0, NULL);
}

static void T_CopyBits(uint16_t w) {
    (void)w; maddr mask = pop32(); int16_t mode = pops16(); maddr dr = pop32(), sr = pop32(), dst = pop32(), src = pop32();
    CopyBits(src, dst, rd_rect(sr), rd_rect(dr), mode, mask);
}
static void T_StdBits(uint16_t w) {
    (void)w; maddr mask = pop32(); int16_t mode = pops16(); maddr dr = pop32(), sr = pop32(), src = pop32();
    std_bits(src, rd_rect(sr), rd_rect(dr), mode, mask);
}
static void std_bits_native(void) { T_StdBits(0); }

/* CopyMask(srcBits, maskBits, dstBits, srcRect, maskRect, dstRect) */
static void T_CopyMask(uint16_t w) {
    (void)w; maddr dr = pop32(), mr = pop32(), sr = pop32(), dst = pop32(), msk = pop32(), src = pop32();
    Surface s = qd_bits_surface(resolve_bits(src)), m = qd_bits_surface(resolve_bits(msk)), d = qd_bits_surface(resolve_bits(dst));
    QRect srr = rd_rect(sr), mrr = rd_rect(mr), drr = rd_rect(dr);
    maddr port = qd_the_port();
    RgnList clip;
    if (is_port_bits(port, dst)) qd_port_clip(port, &clip); else rgn_from_rect(&clip, d.bounds);
    int fg = qd_port_fg_index(port), bk = qd_port_bk_index(port);
    for (int y = drr.top; y < drr.bottom; y++) for (int x = drr.left; x < drr.right; x++) {
        if (!rgn_contains(&clip, y, x) || y < d.bounds.top || y >= d.bounds.bottom || x < d.bounds.left || x >= d.bounds.right) continue;
        int sx = x - drr.left + srr.left, sy = y - drr.top + srr.top, mx = x - drr.left + mrr.left, my = y - drr.top + mrr.top;
        if (!get_px(&m, mx, my)) continue;
        int v = get_px(&s, sx, sy);
        if (s.depth == 1) v = v ? fg : bk;
        put_px(&d, x, y, dst_color(&d, v));
    }
    rgn_free(&clip);
    qd_mark_dirty(&d);
}

/* ScrollRect(r, dh, dv, updateRgn) */
static void T_ScrollRect(uint16_t w) {
    (void)w; maddr upd = pop32(); int16_t dv = pops16(), dh = pops16(); maddr rp = pop32();
    maddr port = qd_the_port();
    QRect r = rd_rect(rp);
    maddr bits = qd_port_bits(port);
    QRect dst = rect_offset(r, dv, dh);
    RgnList clip; qd_port_clip(port, &clip);
    RgnList rr, cl; rgn_from_rect(&rr, r); rgn_op(&clip, &rr, &cl, 0);
    /* copy via a temporary buffer */
    Surface s = qd_port_surface(port);
    int wdt = r.right - r.left, hgt = r.bottom - r.top;
    if (wdt > 0 && hgt > 0 && s.depth == 8) {
        uint8_t *tmp = malloc((size_t)wdt * hgt);
        for (int y = 0; y < hgt; y++) for (int x = 0; x < wdt; x++) {
            int sx = r.left + x, sy = r.top + y;
            tmp[y * wdt + x] = (sy >= s.bounds.top && sy < s.bounds.bottom && sx >= s.bounds.left && sx < s.bounds.right) ? (uint8_t)get_px(&s, sx, sy) : 0;
        }
        for (int y = dst.top; y < dst.bottom; y++) for (int x = dst.left; x < dst.right; x++)
            if (rgn_contains(&cl, y, x) && y >= s.bounds.top && y < s.bounds.bottom && x >= s.bounds.left && x < s.bounds.right)
                put_px(&s, x, y, tmp[(y - dst.top) * wdt + (x - dst.left)]);
        free(tmp);
    }
    (void)bits;
    /* exposed area = r - dst, erased */
    RgnList dl, exposed; rgn_from_rect(&dl, dst); rgn_op(&rr, &dl, &exposed, 2);
    port_fill(port, &exposed, V_ERASE, NULL);
    if (upd) rgn_set(upd, &exposed);
    rgn_free(&clip); rgn_free(&rr); rgn_free(&cl); rgn_free(&dl); rgn_free(&exposed);
    qd_mark_dirty(&s);
}

/* ---- rect traps ---- */
static void T_FrameRect(uint16_t w)  { (void)w; shape_rect(qd_the_port(), rd_rect(pop32()), V_FRAME, NULL); }
static void T_PaintRect(uint16_t w)  { (void)w; shape_rect(qd_the_port(), rd_rect(pop32()), V_PAINT, NULL); }
static void T_EraseRect(uint16_t w)  { (void)w; shape_rect(qd_the_port(), rd_rect(pop32()), V_ERASE, NULL); }
static void T_InvertRect(uint16_t w) { (void)w; shape_rect(qd_the_port(), rd_rect(pop32()), V_INVERT, NULL); }
static void T_FillRect(uint16_t w)   { (void)w; maddr pat = pop32(); QRect r = rd_rect(pop32()); uint8_t p[8]; mget(p, pat, 8); shape_rect(qd_the_port(), r, V_FILL, p); }
static void T_FillCRect(uint16_t w)  { (void)w; maddr pp = pop32(); QRect r = rd_rect(pop32()); uint8_t p[8]; mget(p, rd32(pp) + 20, 8); shape_rect(qd_the_port(), r, V_FILL, p); }

static void do_oval(QRect r, int verb, const uint8_t *pat) {
    RgnList l; oval_rgn(r, &l);
    if (verb == V_FRAME) frame_rgn(qd_the_port(), &l); else port_fill(qd_the_port(), &l, verb, pat);
    rgn_free(&l);
}
static void T_FrameOval(uint16_t w)  { (void)w; do_oval(rd_rect(pop32()), V_FRAME, NULL); }
static void T_PaintOval(uint16_t w)  { (void)w; do_oval(rd_rect(pop32()), V_PAINT, NULL); }
static void T_EraseOval(uint16_t w)  { (void)w; do_oval(rd_rect(pop32()), V_ERASE, NULL); }
static void T_InvertOval(uint16_t w) { (void)w; do_oval(rd_rect(pop32()), V_INVERT, NULL); }
static void T_FillOval(uint16_t w)   { (void)w; maddr pat = pop32(); uint8_t p[8]; mget(p, pat, 8); do_oval(rd_rect(pop32()), V_FILL, p); }

static void do_rrect(int verb, const uint8_t *pat) {
    int16_t oh = pops16(), ow = pops16(); QRect r = rd_rect(pop32());
    RgnList l; rrect_rgn(r, ow, oh, &l);
    if (verb == V_FRAME) frame_rgn(qd_the_port(), &l); else port_fill(qd_the_port(), &l, verb, pat);
    rgn_free(&l);
}
static void T_FrameRoundRect(uint16_t w)  { (void)w; do_rrect(V_FRAME, NULL); }
static void T_PaintRoundRect(uint16_t w)  { (void)w; do_rrect(V_PAINT, NULL); }
static void T_EraseRoundRect(uint16_t w)  { (void)w; do_rrect(V_ERASE, NULL); }
static void T_InvertRoundRect(uint16_t w) { (void)w; do_rrect(V_INVERT, NULL); }
static void T_FillRoundRect(uint16_t w)   { (void)w; maddr pat = pop32(); uint8_t p[8]; mget(p, pat, 8); do_rrect(V_FILL, p); }

/* ---- regions ---- */
static void rgn_verb(maddr rgn, int verb, const uint8_t *pat) {
    RgnList l; rgn_get(rgn, &l);
    if (verb == V_FRAME) frame_rgn(qd_the_port(), &l); else port_fill(qd_the_port(), &l, verb, pat);
    rgn_free(&l);
}
static void T_FrameRgn(uint16_t w)  { (void)w; rgn_verb(pop32(), V_FRAME, NULL); }
static void T_PaintRgn(uint16_t w)  { (void)w; rgn_verb(pop32(), V_PAINT, NULL); }
static void T_EraseRgn(uint16_t w)  { (void)w; rgn_verb(pop32(), V_ERASE, NULL); }
static void T_InvertRgn(uint16_t w) { (void)w; rgn_verb(pop32(), V_INVERT, NULL); }
static void T_FillRgn(uint16_t w)   { (void)w; maddr pat = pop32(); uint8_t p[8]; mget(p, pat, 8); rgn_verb(pop32(), V_FILL, p); }
static void T_FillCRgn(uint16_t w)  { (void)w; maddr pp = pop32(); uint8_t p[8]; mget(p, rd32(pp) + 20, 8); rgn_verb(pop32(), V_FILL, p); }

static void T_NewRgn(uint16_t w) { (void)w; ret32(qd_new_rgn()); }
static void T_DisposeRgn(uint16_t w) { (void)w; DisposeHandle(pop32()); }
static void T_OpenRgn(uint16_t w) { (void)w; fprintf(stderr, "[qd] OpenRgn (region recording) unsupported\n"); }
static void T_CloseRgn(uint16_t w) { (void)w; maddr r = pop32(); qd_set_rect_rgn(r, (QRect){0}); }
static void T_CopyRgn(uint16_t w) { (void)w; maddr d = pop32(), s = pop32(); uint32_t sz = GetHandleSize(s); SetHandleSize(d, sz); mcopy(rd32(d), rd32(s), sz); }
static void T_SetEmptyRgn(uint16_t w) { (void)w; qd_set_rect_rgn(pop32(), (QRect){0}); }
static void T_SetRectRgn(uint16_t w) { (void)w; int16_t b = pops16(), r = pops16(), t = pops16(), l = pops16(); qd_set_rect_rgn(pop32(), (QRect){t, l, b, r}); }
static void T_RectRgn(uint16_t w) { (void)w; maddr r = pop32(); qd_set_rect_rgn(pop32(), rd_rect(r)); }
static void T_OffsetRgn(uint16_t w) { (void)w; int16_t dv = pops16(), dh = pops16(); maddr r = pop32(); RgnList l; rgn_get(r, &l); rgn_offset(&l, dv, dh); rgn_set(r, &l); rgn_free(&l); }
static void T_InsetRgn(uint16_t w) {
    (void)w; int16_t dv = pops16(), dh = pops16(); maddr r = pop32();
    RgnList l; rgn_get(r, &l);
    for (int i = 0; i < l.n; i++) { l.r[i].top += dv; l.r[i].bottom -= dv; l.r[i].left += dh; l.r[i].right -= dh; }
    int k = 0; for (int i = 0; i < l.n; i++) if (!rect_empty(l.r[i])) l.r[k++] = l.r[i]; l.n = k;
    rgn_set(r, &l); rgn_free(&l);
}
static void binop(int op) {
    maddr d = pop32(), b = pop32(), a = pop32();
    RgnList la, lb, lo; rgn_get(a, &la); rgn_get(b, &lb); rgn_op(&la, &lb, &lo, op); rgn_set(d, &lo);
    rgn_free(&la); rgn_free(&lb); rgn_free(&lo);
}
static void T_SectRgn(uint16_t w)  { (void)w; binop(0); }
static void T_UnionRgn(uint16_t w) { (void)w; binop(1); }
static void T_DiffRgn(uint16_t w)  { (void)w; binop(2); }
static void T_XorRgn(uint16_t w)   { (void)w; binop(3); }
static void T_EmptyRgn(uint16_t w) { (void)w; RgnList l; rgn_get(pop32(), &l); retbool(l.n == 0); rgn_free(&l); }
static void T_EqualRgn(uint16_t w) {
    (void)w; maddr b = pop32(), a = pop32();
    RgnList la, lb, x; rgn_get(a, &la); rgn_get(b, &lb); rgn_op(&la, &lb, &x, 3);
    retbool(x.n == 0); rgn_free(&la); rgn_free(&lb); rgn_free(&x);
}
static void T_PtInRgn(uint16_t w) { (void)w; maddr r = pop32(); QPoint p = pt_from32(pop32()); RgnList l; rgn_get(r, &l); retbool(rgn_contains(&l, p.v, p.h)); rgn_free(&l); }
static void T_RectInRgn(uint16_t w) {
    (void)w; maddr r = pop32(); QRect rc = rd_rect(pop32());
    RgnList l, a, x; rgn_get(r, &l); rgn_from_rect(&a, rc); rgn_op(&l, &a, &x, 0);
    retbool(x.n > 0); rgn_free(&l); rgn_free(&a); rgn_free(&x);
}
static void T_BitMapToRegion(uint16_t w) {
    (void)w; maddr bm = pop32(), rgn = pop32();
    Surface s = qd_bits_surface(resolve_bits(bm));
    RgnList acc = {0};
    int cap = 64; acc.r = malloc(sizeof(QRect) * cap);
    for (int y = s.bounds.top; y < s.bounds.bottom; y++) {
        int x = s.bounds.left;
        while (x < s.bounds.right) {
            while (x < s.bounds.right && !(s.depth == 1 ? get_px(&s, x, y) : get_px(&s, x, y) != 0)) x++;
            int x0 = x;
            while (x < s.bounds.right && (s.depth == 1 ? get_px(&s, x, y) : get_px(&s, x, y) != 0)) x++;
            if (x > x0) {
                if (acc.n == cap) { cap *= 2; acc.r = realloc(acc.r, sizeof(QRect) * cap); }
                acc.r[acc.n++] = (QRect){ (int16_t)y, (int16_t)x0, (int16_t)(y + 1), (int16_t)x };
            }
        }
    }
    RgnList out; rgn_op(&acc, &acc, &out, 1);
    rgn_set(rgn, &out); rgn_free(&acc); rgn_free(&out);
    ret16(0);
}

/* ---- rect utilities ---- */
static void T_SetRect(uint16_t w) { (void)w; int16_t b = pops16(), r = pops16(), t = pops16(), l = pops16(); wr_rect(pop32(), (QRect){t, l, b, r}); }
static void T_OffsetRect(uint16_t w) { (void)w; int16_t dv = pops16(), dh = pops16(); maddr r = pop32(); wr_rect(r, rect_offset(rd_rect(r), dv, dh)); }
static void T_InsetRect(uint16_t w) {
    (void)w; int16_t dv = pops16(), dh = pops16(); maddr r = pop32(); QRect x = rd_rect(r);
    x.top += dv; x.bottom -= dv; x.left += dh; x.right -= dh;
    if (rect_empty(x)) x = (QRect){0};
    wr_rect(r, x);
}
static void T_SectRect(uint16_t w) { (void)w; maddr d = pop32(), b = pop32(), a = pop32(); QRect r = rect_sect(rd_rect(a), rd_rect(b)); wr_rect(d, r); retbool(!rect_empty(r)); }
static void T_UnionRect(uint16_t w) {
    (void)w; maddr d = pop32(), b = pop32(), a = pop32(); QRect x = rd_rect(a), y = rd_rect(b);
    if (rect_empty(x)) { wr_rect(d, y); return; }
    if (rect_empty(y)) { wr_rect(d, x); return; }
    QRect r = { x.top < y.top ? x.top : y.top, x.left < y.left ? x.left : y.left, x.bottom > y.bottom ? x.bottom : y.bottom, x.right > y.right ? x.right : y.right };
    wr_rect(d, r);
}
static void T_EqualRect(uint16_t w) { (void)w; maddr b = pop32(), a = pop32(); retbool(!memcmp(mptr(a), mptr(b), 8)); }
static void T_EmptyRect(uint16_t w) { (void)w; retbool(rect_empty(rd_rect(pop32()))); }
static void T_PtInRect(uint16_t w) { (void)w; maddr r = pop32(); QPoint p = pt_from32(pop32()); QRect x = rd_rect(r); retbool(p.v >= x.top && p.v < x.bottom && p.h >= x.left && p.h < x.right); }
static void T_Pt2Rect(uint16_t w) {
    (void)w; maddr d = pop32(); QPoint b = pt_from32(pop32()), a = pt_from32(pop32());
    wr_rect(d, (QRect){ a.v < b.v ? a.v : b.v, a.h < b.h ? a.h : b.h, a.v > b.v ? a.v : b.v, a.h > b.h ? a.h : b.h });
}
static void T_AddPt(uint16_t w) { (void)w; maddr d = pop32(); QPoint s = pt_from32(pop32()); QPoint p = rd_pt(d); p.v += s.v; p.h += s.h; wr_pt(d, p); }
static void T_SubPt(uint16_t w) { (void)w; maddr d = pop32(); QPoint s = pt_from32(pop32()); QPoint p = rd_pt(d); p.v -= s.v; p.h -= s.h; wr_pt(d, p); }
static void T_SetPt(uint16_t w) { (void)w; int16_t v = pops16(), h = pops16(); maddr p = pop32(); wr16(p, (uint16_t)v); wr16(p + 2, (uint16_t)h); }
static void T_EqualPt(uint16_t w) { (void)w; uint32_t b = pop32(), a = pop32(); retbool(a == b); }

/* ---- pen ---- */
static void T_PenSize(uint16_t w) { (void)w; int16_t v = pops16(), h = pops16(); wr16(qd_the_port() + GP_pnSize, (uint16_t)v); wr16(qd_the_port() + GP_pnSize + 2, (uint16_t)h); }
static void T_PenMode(uint16_t w) { (void)w; wr16(qd_the_port() + GP_pnMode, pop16()); }
static void T_PenPat(uint16_t w) { (void)w; uint8_t p[8]; mget(p, pop32(), 8); qd_port_set_pattern(qd_the_port(), 1, p); }
static void T_BackPat(uint16_t w) { (void)w; uint8_t p[8]; mget(p, pop32(), 8); qd_port_set_pattern(qd_the_port(), 0, p); }
static void T_PenPixPat(uint16_t w) { (void)w; maddr pp = pop32(); if (qd_is_color_port(qd_the_port())) wr32(qd_the_port() + GP_pnPixPat, pp); }
static void T_BackPixPat(uint16_t w) { (void)w; maddr pp = pop32(); if (qd_is_color_port(qd_the_port())) wr32(qd_the_port() + GP_bkPixPat, pp); }
static void T_PenNormal(uint16_t w) {
    (void)w; maddr p = qd_the_port();
    static const uint8_t black[8] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    wr32(p + GP_pnSize, 0x00010001); wr16(p + GP_pnMode, 8); qd_port_set_pattern(p, 1, black);
}
static void T_HidePen(uint16_t w) { (void)w; maddr p = qd_the_port(); wr16(p + GP_pnVis, (uint16_t)(rds16(p + GP_pnVis) - 1)); }
static void T_ShowPen(uint16_t w) { (void)w; maddr p = qd_the_port(); wr16(p + GP_pnVis, (uint16_t)(rds16(p + GP_pnVis) + 1)); }
static void T_GetPen(uint16_t w) { (void)w; maddr d = pop32(); mcopy(d, qd_the_port() + GP_pnLoc, 4); }
/* PenState: pnLoc(4) pnSize(4) pnMode(2) pnPat(8) */
static void T_GetPenState(uint16_t w) {
    (void)w; maddr d = pop32(); maddr p = qd_the_port();
    mcopy(d, p + GP_pnLoc, 10);
    uint8_t pat[8]; qd_port_pattern(p, 1, pat); mput(d + 10, pat, 8);
}
static void T_SetPenState(uint16_t w) {
    (void)w; maddr s = pop32(); maddr p = qd_the_port();
    mcopy(p + GP_pnLoc, s, 10);
    uint8_t pat[8]; mget(pat, s + 10, 8); qd_port_set_pattern(p, 1, pat);
}
static void T_MoveTo(uint16_t w) { (void)w; int16_t v = pops16(), h = pops16(); maddr p = qd_the_port(); wr16(p + GP_pnLoc, (uint16_t)v); wr16(p + GP_pnLoc + 2, (uint16_t)h); }
static void T_Move(uint16_t w) { (void)w; int16_t dv = pops16(), dh = pops16(); maddr p = qd_the_port(); QPoint q = rd_pt(p + GP_pnLoc); q.v += dv; q.h += dh; wr_pt(p + GP_pnLoc, q); }

static void line_to(int16_t h, int16_t v) {
    maddr p = qd_the_port();
    QPoint a = rd_pt(p + GP_pnLoc);
    QPoint ps = rd_pt(p + GP_pnSize);
    if (!(rds16(p + GP_pnVis) < 0) && ps.v > 0 && ps.h > 0) {
        int dx = abs(h - a.h), dy = -abs(v - a.v), sx = a.h < h ? 1 : -1, sy = a.v < v ? 1 : -1, err = dx + dy;
        int x = a.h, y = a.v;
        RgnList acc = {0}; int cap = 64; acc.r = malloc(sizeof(QRect) * cap);
        for (;;) {
            if (acc.n == cap) { cap *= 2; acc.r = realloc(acc.r, sizeof(QRect) * cap); }
            acc.r[acc.n++] = (QRect){ (int16_t)y, (int16_t)x, (int16_t)(y + ps.v), (int16_t)(x + ps.h) };
            if (x == h && y == v) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x += sx; }
            if (e2 <= dx) { err += dx; y += sy; }
        }
        RgnList u; rgn_op(&acc, &acc, &u, 1);
        port_fill(p, &u, V_PAINT, NULL);
        rgn_free(&acc); rgn_free(&u);
    }
    wr16(p + GP_pnLoc, (uint16_t)v); wr16(p + GP_pnLoc + 2, (uint16_t)h);
}
static void T_LineTo(uint16_t w) { (void)w; int16_t v = pops16(), h = pops16(); line_to(h, v); }
static void T_Line(uint16_t w) { (void)w; int16_t dv = pops16(), dh = pops16(); QPoint a = rd_pt(qd_the_port() + GP_pnLoc); line_to((int16_t)(a.h + dh), (int16_t)(a.v + dv)); }

/* GetPixel(h, v): Boolean */
static void T_GetPixel(uint16_t w) {
    (void)w; int16_t v = pops16(), h = pops16();
    Surface s = qd_port_surface(qd_the_port());
    if (v < s.bounds.top || v >= s.bounds.bottom || h < s.bounds.left || h >= s.bounds.right) { retbool(0); return; }
    int px = get_px(&s, h, v);
    retbool(s.depth == 1 ? px : px != 0);
}

/* SetStdProcs / SetStdCProcs: fill a QDProcs record with standard routine addresses */
static void std_text_native(void);
static void T_SetStdProcs(uint16_t w) {
    (void)w; maddr procs = pop32();
    for (int i = 0; i < 13; i++) wr32(procs + 4 * i, 0);
    wr32(procs + 0, std_text_thunk);
    wr32(procs + 32, std_bits_thunk);
}
static void T_SetStdCProcs(uint16_t w) {
    (void)w; maddr procs = pop32();
    for (int i = 0; i < 20; i++) wr32(procs + 4 * i, 0);
    wr32(procs + 0, std_text_thunk);
    wr32(procs + 32, std_bits_thunk);
}

/* ---- text ---- */
static void draw_text_bytes(const uint8_t *t, int n) {
    maddr port = qd_the_port();
    maddr proc = custom_proc(port, 0, std_text_thunk);
    if (proc) {
        uint32_t sp = cpu_get(R_SP);
        maddr buf = sp - 512;
        mput(buf, t, (uint32_t)n);
        cpu_set(R_SP, buf);
        push16((uint16_t)n); push32(buf); push32(0x00010001); push32(0x00010001);
        cpu_call(proc, NULL);
        cpu_set(R_SP, sp);
        return;
    }
    font_draw_text(port, t, n);
}
static void T_DrawString(uint16_t w) { (void)w; maddr s = pop32(); uint8_t b[256]; int n = rd8(s); mget(b, s + 1, (uint32_t)n); draw_text_bytes(b, n); }
static void T_DrawText(uint16_t w) { (void)w; int16_t cnt = pops16(), first = pops16(); maddr buf = pop32(); if (cnt <= 0) return; uint8_t *b = malloc((size_t)cnt); mget(b, buf + first, (uint32_t)cnt); draw_text_bytes(b, cnt); free(b); }
static void T_DrawChar(uint16_t w) { (void)w; uint8_t c = (uint8_t)pop16(); draw_text_bytes(&c, 1); }
static void T_StdText(uint16_t w) {
    (void)w; pop32(); pop32(); maddr buf = pop32(); int16_t cnt = pops16();
    if (cnt <= 0) return;
    uint8_t *b = malloc((size_t)cnt); mget(b, buf, (uint32_t)cnt); font_draw_text(qd_the_port(), b, cnt); free(b);
}
static void std_text_native(void) { T_StdText(0); }
static void T_TextWidth(uint16_t w) { (void)w; int16_t cnt = pops16(), first = pops16(); maddr buf = pop32(); if (cnt <= 0) { ret16(0); return; } uint8_t *b = malloc((size_t)cnt); mget(b, buf + first, (uint32_t)cnt); ret16((uint16_t)font_text_width(qd_the_port(), b, cnt)); free(b); }
static void T_StringWidth(uint16_t w) { (void)w; maddr s = pop32(); uint8_t b[256]; int n = rd8(s); mget(b, s + 1, (uint32_t)n); ret16((uint16_t)font_text_width(qd_the_port(), b, n)); }
static void T_CharWidth(uint16_t w) { (void)w; uint8_t c = (uint8_t)pop16(); ret16((uint16_t)font_text_width(qd_the_port(), &c, 1)); }
static void T_TextFont(uint16_t w) { (void)w; wr16(qd_the_port() + GP_txFont, pop16()); }
static void T_TextFace(uint16_t w) { (void)w; wr8(qd_the_port() + GP_txFace, (uint8_t)pop16()); }
static void T_TextMode(uint16_t w) { (void)w; wr16(qd_the_port() + GP_txMode, pop16()); }
static void T_TextSize(uint16_t w) { (void)w; wr16(qd_the_port() + GP_txSize, pop16()); }
static void T_SpaceExtra(uint16_t w) { (void)w; wr32(qd_the_port() + GP_spExtra, pop32()); }
static void T_GetFontInfo(uint16_t w) {
    (void)w; maddr fi = pop32(); int a, d, wm, l; font_get_info(qd_the_port(), &a, &d, &wm, &l);
    wr16(fi, (uint16_t)a); wr16(fi + 2, (uint16_t)d); wr16(fi + 4, (uint16_t)wm); wr16(fi + 6, (uint16_t)l);
}

void traps_qd_draw_init(void) {
    std_bits_thunk = cpu_thunk(std_bits_native, "StdBits");
    std_text_thunk = cpu_thunk(std_text_native, "StdText");
    trap_register(TRAP_CopyBits, T_CopyBits);
    trap_register(TRAP_StdBits, T_StdBits);
    trap_register(TRAP_CopyMask, T_CopyMask);
    trap_register(TRAP_ScrollRect, T_ScrollRect);
    trap_register(TRAP_FrameRect, T_FrameRect);
    trap_register(TRAP_PaintRect, T_PaintRect);
    trap_register(TRAP_EraseRect, T_EraseRect);
    trap_register(TRAP_InvertRect, T_InvertRect);
    trap_register(TRAP_FillRect, T_FillRect);
    trap_register(TRAP_FillCRect, T_FillCRect);
    trap_register(TRAP_FrameOval, T_FrameOval);
    trap_register(TRAP_PaintOval, T_PaintOval);
    trap_register(TRAP_EraseOval, T_EraseOval);
    trap_register(TRAP_InvertOval, T_InvertOval);
    trap_register(TRAP_FillOval, T_FillOval);
    trap_register(TRAP_FrameRoundRect, T_FrameRoundRect);
    trap_register(TRAP_PaintRoundRect, T_PaintRoundRect);
    trap_register(TRAP_EraseRoundRect, T_EraseRoundRect);
    trap_register(TRAP_InvertRoundRect, T_InvertRoundRect);
    trap_register(TRAP_FillRoundRect, T_FillRoundRect);
    trap_register(TRAP_FrameRgn, T_FrameRgn);
    trap_register(TRAP_PaintRgn, T_PaintRgn);
    trap_register(TRAP_EraseRgn, T_EraseRgn);
    trap_register(TRAP_InvertRgn, T_InvertRgn);
    trap_register(TRAP_FillRgn, T_FillRgn);
    trap_register(TRAP_FillCRgn, T_FillCRgn);
    trap_register(TRAP_NewRgn, T_NewRgn);
    trap_register(TRAP_DisposeRgn, T_DisposeRgn);
    trap_register(TRAP_OpenRgn, T_OpenRgn);
    trap_register(TRAP_CloseRgn, T_CloseRgn);
    trap_register(TRAP_CopyRgn, T_CopyRgn);
    trap_register(TRAP_SetEmptyRgn, T_SetEmptyRgn);
    trap_register(TRAP_SetRectRgn, T_SetRectRgn);
    trap_register(TRAP_RectRgn, T_RectRgn);
    trap_register(TRAP_OffsetRgn, T_OffsetRgn);
    trap_register(TRAP_InsetRgn, T_InsetRgn);
    trap_register(TRAP_SectRgn, T_SectRgn);
    trap_register(TRAP_UnionRgn, T_UnionRgn);
    trap_register(TRAP_DiffRgn, T_DiffRgn);
    trap_register(TRAP_XorRgn, T_XorRgn);
    trap_register(TRAP_EmptyRgn, T_EmptyRgn);
    trap_register(TRAP_EqualRgn, T_EqualRgn);
    trap_register(TRAP_PtInRgn, T_PtInRgn);
    trap_register(TRAP_RectInRgn, T_RectInRgn);
    trap_register(TRAP_BitMapToRegion, T_BitMapToRegion);
    trap_register(TRAP_SetRect, T_SetRect);
    trap_register(TRAP_OffsetRect, T_OffsetRect);
    trap_register(TRAP_InsetRect, T_InsetRect);
    trap_register(TRAP_SectRect, T_SectRect);
    trap_register(TRAP_UnionRect, T_UnionRect);
    trap_register(TRAP_EqualRect, T_EqualRect);
    trap_register(TRAP_EmptyRect, T_EmptyRect);
    trap_register(TRAP_PtInRect, T_PtInRect);
    trap_register(TRAP_Pt2Rect, T_Pt2Rect);
    trap_register(TRAP_AddPt, T_AddPt);
    trap_register(TRAP_SubPt, T_SubPt);
    trap_register(TRAP_SetPt, T_SetPt);
    trap_register(TRAP_EqualPt, T_EqualPt);
    trap_register(TRAP_PenSize, T_PenSize);
    trap_register(TRAP_PenMode, T_PenMode);
    trap_register(TRAP_PenPat, T_PenPat);
    trap_register(TRAP_BackPat, T_BackPat);
    trap_register(TRAP_PenPixPat, T_PenPixPat);
    trap_register(TRAP_BackPixPat, T_BackPixPat);
    trap_register(TRAP_PenNormal, T_PenNormal);
    trap_register(TRAP_HidePen, T_HidePen);
    trap_register(TRAP_ShowPen, T_ShowPen);
    trap_register(TRAP_GetPen, T_GetPen);
    trap_register(TRAP_GetPenState, T_GetPenState);
    trap_register(TRAP_SetPenState, T_SetPenState);
    trap_register(TRAP_MoveTo, T_MoveTo);
    trap_register(TRAP_Move, T_Move);
    trap_register(TRAP_LineTo, T_LineTo);
    trap_register(TRAP_Line, T_Line);
    trap_register(TRAP_GetPixel, T_GetPixel);
    trap_register(TRAP_SetStdProcs, T_SetStdProcs);
    trap_register(TRAP_SetStdCProcs, T_SetStdCProcs);
    trap_register(TRAP_DrawString, T_DrawString);
    trap_register(TRAP_DrawText, T_DrawText);
    trap_register(TRAP_DrawChar, T_DrawChar);
    trap_register(TRAP_StdText, T_StdText);
    trap_register(TRAP_TextWidth, T_TextWidth);
    trap_register(TRAP_StringWidth, T_StringWidth);
    trap_register(TRAP_CharWidth, T_CharWidth);
    trap_register(TRAP_TextFont, T_TextFont);
    trap_register(TRAP_TextFace, T_TextFace);
    trap_register(TRAP_TextMode, T_TextMode);
    trap_register(TRAP_TextSize, T_TextSize);
    trap_register(TRAP_SpaceExtra, T_SpaceExtra);
    trap_register(TRAP_GetFontInfo, T_GetFontInfo);
}
