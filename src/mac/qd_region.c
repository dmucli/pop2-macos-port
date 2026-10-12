/* Regions: rgnSize(2) rgnBBox(8) [count(2) rects(8*count)]. A size of 10 means
 * rectangular (like real QuickDraw). Non-rectangular regions use our own
 * payload (a list of disjoint rectangles) instead of QuickDraw's inversion points. */
#include "qd.h"
#include <stdlib.h>
#include <stdio.h>

void rgn_free(RgnList *l) { free(l->r); l->r = NULL; l->n = 0; }

static void bbox(RgnList *l) {
    if (!l->n) { l->bbox = (QRect){0, 0, 0, 0}; return; }
    QRect b = l->r[0];
    for (int i = 1; i < l->n; i++) {
        if (l->r[i].top < b.top) b.top = l->r[i].top;
        if (l->r[i].left < b.left) b.left = l->r[i].left;
        if (l->r[i].bottom > b.bottom) b.bottom = l->r[i].bottom;
        if (l->r[i].right > b.right) b.right = l->r[i].right;
    }
    l->bbox = b;
}

void rgn_from_rect(RgnList *l, QRect r) {
    l->r = NULL; l->n = 0;
    if (!rect_empty(r)) { l->r = malloc(sizeof(QRect)); l->r[0] = r; l->n = 1; }
    bbox(l);
}

void rgn_get(maddr h, RgnList *out) {
    out->r = NULL; out->n = 0;
    maddr p = rd32(h);
    if (!p) { bbox(out); return; }
    uint16_t sz = rd16(p);
    QRect bb = rd_rect(p + 2);
    if (sz <= 10) { rgn_from_rect(out, bb); return; }
    int n = rd16(p + 10);
    out->r = malloc(sizeof(QRect) * (n ? n : 1)); out->n = n;
    for (int i = 0; i < n; i++) out->r[i] = rd_rect(p + 12 + 8 * i);
    bbox(out);
}

void rgn_set(maddr h, const RgnList *in) {
    RgnList l = *in;
    bbox(&l);
    if (l.n <= 1) {
        SetHandleSize(h, 10);
        maddr p = rd32(h);
        wr16(p, 10); wr_rect(p + 2, l.n ? l.r[0] : (QRect){0, 0, 0, 0});
        return;
    }
    uint32_t sz = 12 + 8u * l.n;
    SetHandleSize(h, sz);
    maddr p = rd32(h);
    wr16(p, (uint16_t)(sz > 0x7FFF ? 0x7FFF : sz));
    wr_rect(p + 2, l.bbox);
    wr16(p + 10, (uint16_t)l.n);
    for (int i = 0; i < l.n; i++) wr_rect(p + 12 + 8 * i, l.r[i]);
}

maddr qd_new_rgn(void) {
    maddr h = NewHandleClear(10);
    wr16(rd32(h), 10);
    return h;
}
void qd_set_rect_rgn(maddr h, QRect r) {
    if (rect_empty(r)) r = (QRect){0, 0, 0, 0};
    SetHandleSize(h, 10);
    wr16(rd32(h), 10); wr_rect(rd32(h) + 2, r);
}

void rgn_offset(RgnList *l, int dv, int dh) {
    for (int i = 0; i < l->n; i++) l->r[i] = rect_offset(l->r[i], dv, dh);
    bbox(l);
}
int rgn_contains(const RgnList *l, int v, int h) {
    for (int i = 0; i < l->n; i++)
        if (v >= l->r[i].top && v < l->r[i].bottom && h >= l->r[i].left && h < l->r[i].right) return 1;
    return 0;
}

/* --- band-based set operations --- */
typedef struct { int16_t a, b; } Span;
static int cmp16(const void *x, const void *y) { return *(const int16_t *)x - *(const int16_t *)y; }
static int cmpspan(const void *x, const void *y) { return ((const Span *)x)->a - ((const Span *)y)->a; }

/* spans covering band [y0,y1) for list l, merged */
static int spans_for(const RgnList *l, int y0, int y1, Span *out) {
    int n = 0;
    for (int i = 0; i < l->n; i++)
        if (l->r[i].top <= y0 && l->r[i].bottom >= y1) out[n++] = (Span){ l->r[i].left, l->r[i].right };
    qsort(out, n, sizeof(Span), cmpspan);
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (m && out[i].a <= out[m - 1].b) { if (out[i].b > out[m - 1].b) out[m - 1].b = out[i].b; }
        else out[m++] = out[i];
    }
    return m;
}

static int inside(const Span *s, int n, int x) { for (int i = 0; i < n; i++) if (x >= s[i].a && x < s[i].b) return 1; return 0; }

void rgn_op(const RgnList *A, const RgnList *B, RgnList *out, int op) {
    int ne = 2 * (A->n + B->n);
    int16_t *ys = malloc(sizeof(int16_t) * (ne ? ne : 1));
    int k = 0;
    for (int i = 0; i < A->n; i++) { ys[k++] = A->r[i].top; ys[k++] = A->r[i].bottom; }
    for (int i = 0; i < B->n; i++) { ys[k++] = B->r[i].top; ys[k++] = B->r[i].bottom; }
    qsort(ys, k, sizeof(int16_t), cmp16);
    Span *sa = malloc(sizeof(Span) * (A->n + 1)), *sb = malloc(sizeof(Span) * (B->n + 1));
    int16_t *xs = malloc(sizeof(int16_t) * (2 * (A->n + B->n) + 2));
    int cap = 16, cnt = 0;
    QRect *res = malloc(sizeof(QRect) * cap);
    int prev_start = 0, prev_end = 0;   /* rects of previous band, for vertical merge */
    for (int i = 0; i + 1 < k; i++) {
        int y0 = ys[i], y1 = ys[i + 1];
        if (y0 == y1) continue;
        int na = spans_for(A, y0, y1, sa), nb = spans_for(B, y0, y1, sb);
        int nx = 0;
        for (int j = 0; j < na; j++) { xs[nx++] = sa[j].a; xs[nx++] = sa[j].b; }
        for (int j = 0; j < nb; j++) { xs[nx++] = sb[j].a; xs[nx++] = sb[j].b; }
        qsort(xs, nx, sizeof(int16_t), cmp16);
        int band_start = cnt;
        int run = 0; int16_t run_x = 0;
        for (int j = 0; j < nx; j++) {
            int x = xs[j];
            if (j + 1 < nx && xs[j + 1] == x) continue;
            int ia = inside(sa, na, x), ib = inside(sb, nb, x);
            int in = op == 0 ? (ia && ib) : op == 1 ? (ia || ib) : op == 2 ? (ia && !ib) : (ia != ib);
            if (in && !run) { run = 1; run_x = (int16_t)x; }
            else if (!in && run) {
                run = 0;
                if (cnt == cap) { cap *= 2; res = realloc(res, sizeof(QRect) * cap); }
                res[cnt++] = (QRect){ (int16_t)y0, run_x, (int16_t)y1, (int16_t)x };
            }
        }
        /* merge with previous band if identical spans and touching */
        int nthis = cnt - band_start, nprev = prev_end - prev_start;
        if (nthis && nthis == nprev && res[prev_start].bottom == y0) {
            int same = 1;
            for (int j = 0; j < nthis; j++)
                if (res[prev_start + j].left != res[band_start + j].left || res[prev_start + j].right != res[band_start + j].right) { same = 0; break; }
            if (same) {
                for (int j = 0; j < nthis; j++) res[prev_start + j].bottom = (int16_t)y1;
                cnt = band_start;
                continue;
            }
        }
        prev_start = band_start; prev_end = cnt;
    }
    free(ys); free(sa); free(sb); free(xs);
    out->r = res; out->n = cnt;
    bbox(out);
}
