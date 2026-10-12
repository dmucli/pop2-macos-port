/* QuickDraw internals: record layouts in emulated memory and drawing helpers.
 * Everything is 8-bit indexed (the game requires 256 colors); 1-bit BitMaps are
 * supported as sources/destinations for CopyBits and regions. */
#pragma once
#include "mac.h"

typedef struct { int16_t top, left, bottom, right; } QRect;
typedef struct { int16_t v, h; } QPoint;

static inline QRect rd_rect(maddr a) { QRect r = { rds16(a), rds16(a + 2), rds16(a + 4), rds16(a + 6) }; return r; }
static inline void wr_rect(maddr a, QRect r) { wr16(a, (uint16_t)r.top); wr16(a + 2, (uint16_t)r.left); wr16(a + 4, (uint16_t)r.bottom); wr16(a + 6, (uint16_t)r.right); }
static inline QPoint rd_pt(maddr a) { QPoint p = { rds16(a), rds16(a + 2) }; return p; }
static inline void wr_pt(maddr a, QPoint p) { wr16(a, (uint16_t)p.v); wr16(a + 2, (uint16_t)p.h); }
static inline QPoint pt_from32(uint32_t v) { QPoint p = { (int16_t)(v >> 16), (int16_t)v }; return p; }
static inline int rect_empty(QRect r) { return r.bottom <= r.top || r.right <= r.left; }
static inline QRect rect_sect(QRect a, QRect b) {
    QRect r = { a.top > b.top ? a.top : b.top, a.left > b.left ? a.left : b.left,
               a.bottom < b.bottom ? a.bottom : b.bottom, a.right < b.right ? a.right : b.right };
    if (rect_empty(r)) r.top = r.left = r.bottom = r.right = 0;
    return r;
}
static inline QRect rect_offset(QRect r, int dv, int dh) { r.top += dv; r.bottom += dv; r.left += dh; r.right += dh; return r; }

/* GrafPort / CGrafPort field offsets */
enum {
    GP_device = 0, GP_portBits = 2, GP_portPixMap = 2, GP_portVersion = 6, GP_grafVars = 8,
    GP_chExtra = 12, GP_pnLocHFrac = 14, GP_portRect = 16, GP_visRgn = 24, GP_clipRgn = 28,
    GP_bkPat = 32, GP_bkPixPat = 32, GP_rgbFgColor = 36, GP_rgbBkColor = 42, GP_fillPat = 40,
    GP_pnLoc = 48, GP_pnSize = 52, GP_pnMode = 56, GP_pnPat = 58, GP_pnPixPat = 58,
    GP_fillPixPat = 62, GP_pnVis = 66, GP_txFont = 68, GP_txFace = 70, GP_txMode = 72,
    GP_txSize = 74, GP_spExtra = 76, GP_fgColor = 80, GP_bkColor = 84, GP_colrBit = 88,
    GP_patStretch = 90, GP_picSave = 92, GP_rgnSave = 96, GP_polySave = 100, GP_grafProcs = 104,
    GP_SIZE = 108
};
/* PixMap */
enum { PM_baseAddr = 0, PM_rowBytes = 4, PM_bounds = 6, PM_pmVersion = 14, PM_packType = 16,
       PM_packSize = 18, PM_hRes = 22, PM_vRes = 26, PM_pixelType = 30, PM_pixelSize = 32,
       PM_cmpCount = 34, PM_cmpSize = 36, PM_planeBytes = 38, PM_pmTable = 42, PM_pmReserved = 46,
       PM_SIZE = 50 };
/* GDevice */
enum { GD_refNum = 0, GD_id = 2, GD_type = 4, GD_iTable = 6, GD_resPref = 10, GD_searchProc = 12,
       GD_compProc = 16, GD_flags = 20, GD_pMap = 22, GD_refCon = 26, GD_nextGD = 30, GD_rect = 34,
       GD_mode = 42, GD_SIZE = 62 };
/* ColorTable */
enum { CT_seed = 0, CT_flags = 4, CT_size = 6, CT_table = 8 };

/* A resolved drawing surface: 8-bit or 1-bit. */
typedef struct {
    maddr base;
    uint32_t rowbytes;
    QRect bounds;          /* local coordinates of the first pixel = bounds.topLeft */
    int depth;            /* 1 or 8 */
    maddr ctab;           /* CTabHandle (8-bit) or 0 */
} Surface;

/* QD globals live at *(A5) - offsets */
maddr qd_globals_thePort_addr(void);   /* address of qd.thePort */
maddr qd_the_port(void);
void  qd_set_port(maddr port);
int   qd_is_color_port(maddr port);
Surface qd_port_surface(maddr port);
Surface qd_bits_surface(maddr bitmap_or_pixmap);   /* BitMap* or PixMap* */
maddr qd_screen_device(void);
maddr qd_device_ctab(void);            /* CTabHandle of the main device */
uint32_t qd_device_rgb(int index);     /* 0x00RRGGBB */
int   qd_rgb_to_index(uint16_t r, uint16_t g, uint16_t b);
int   qd_port_fg_index(maddr port);
int   qd_port_bk_index(maddr port);
void  qd_init_screen(int w, int h);
extern int qd_screen_w, qd_screen_h;
extern uint32_t qd_clut_version;       /* bumps whenever the device CLUT changes */
extern uint32_t qd_screen_dirty;       /* bumps whenever pixels in the screen buffer may change */

/* regions (stored in emulated memory, see qd_region.c) */
typedef struct { int n; QRect *r; QRect bbox; } RgnList;
void  rgn_get(maddr rgnh, RgnList *out);
void  rgn_set(maddr rgnh, const RgnList *in);
void  rgn_free(RgnList *l);
void  rgn_from_rect(RgnList *l, QRect r);
void  rgn_op(const RgnList *a, const RgnList *b, RgnList *out, int op);  /* 0 sect, 1 union, 2 diff, 3 xor */
void  rgn_offset(RgnList *l, int dv, int dh);
int   rgn_contains(const RgnList *l, int v, int h);
maddr qd_new_rgn(void);
void  qd_set_rect_rgn(maddr rgnh, QRect r);

/* drawing */
typedef struct { RgnList clip; int dv, dh; } ClipCtx;     /* clip in surface-local coords */
void  qd_port_clip(maddr port, RgnList *out);               /* portRect ∩ visRgn ∩ clipRgn, local coords */
void  qd_fill_rect_index(Surface *s, const RgnList *clip, QRect r, int index);
void  qd_fill_rect_pattern(Surface *s, const RgnList *clip, QRect r, const uint8_t pat[8], int fg, int bk, int mode);
void  qd_copybits(maddr srcBits, maddr dstBits, QRect sr, QRect dr, int mode, maddr maskRgn, int fg, int bk, const RgnList *extra_clip);
void  qd_mark_dirty(Surface *s);

/* text */
void  font_draw_text(maddr port, const uint8_t *text, int len);
int   font_text_width(maddr port, const uint8_t *text, int len);
void  font_get_info(maddr port, int *ascent, int *descent, int *widmax, int *leading);
void  font_init(void);
