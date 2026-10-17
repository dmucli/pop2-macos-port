/* Window Manager, Menu Manager, Control Manager (minimal), Dialog Manager,
 * TextEdit (minimal), Scrap, Printing/Standard File stubs. */
#include "qd.h"
#include "trapnums.h"
#include <stdio.h>
#include <stdlib.h>

void  InitCPort(maddr port);
void  InitPort(maddr port);
maddr qd_port_bits(maddr port);
void  qd_activate_window_palette(maddr window);
void  font_draw_text(maddr port, const uint8_t *t, int n);
void  runtime_idle(void);
int   event_get(uint16_t mask, maddr evt, int remove);
void  event_post(uint16_t what, uint32_t msg);
void  platform_wait_ms(int ms);
void  platform_menu_sync(void);
int   platform_menu_pending(uint32_t *result);
int   platform_menu_has_pending(void);

/* WindowRecord offsets (after the 108-byte port) */
enum { W_kind = 108, W_visible = 110, W_hilited = 111, W_goAway = 112, W_struc = 114, W_cont = 118,
       W_update = 122, W_defProc = 126, W_data = 130, W_title = 134, W_titleWidth = 138,
       W_ctlList = 140, W_next = 144, W_pic = 148, W_refCon = 152, W_SIZE = 156,
       D_items = 156, D_textH = 160, D_editField = 164, D_editOpen = 166, D_defItem = 168, D_SIZE = 170 };

static maddr wmgr_port;
static int menubar_visible = 1;
#define MBAR_H 20

static maddr window_list(void) { return rd32(LM_WindowList); }
static QRect screen_rect(void) { return (QRect){0, 0, (int16_t)qd_screen_h, (int16_t)qd_screen_w}; }

/* Global content rect of a window = portRect offset by -portBits.bounds.topLeft */
static QRect win_global_content(maddr w) {
    QRect pr = rd_rect(w + GP_portRect);
    QRect b = rd_rect(qd_port_bits(w) + 6);
    return rect_offset(pr, -b.top, -b.left);
}
static int win_proc(maddr w) { return (int)(rd32(w + W_defProc) & 0xFFFF); }
static QRect win_struct(maddr w) {
    QRect c = win_global_content(w);
    int p = win_proc(w) & 0xF;
    switch (p) {
        case 2: return (QRect){ (int16_t)(c.top - 1), (int16_t)(c.left - 1), (int16_t)(c.bottom + 1), (int16_t)(c.right + 1) };  /* plainDBox */
        case 1: return (QRect){ (int16_t)(c.top - 8), (int16_t)(c.left - 8), (int16_t)(c.bottom + 8), (int16_t)(c.right + 8) };  /* dBoxProc */
        case 3: return (QRect){ (int16_t)(c.top - 1), (int16_t)(c.left - 1), (int16_t)(c.bottom + 3), (int16_t)(c.right + 3) };  /* altDBox */
        default: return (QRect){ (int16_t)(c.top - 19), (int16_t)(c.left - 1), (int16_t)(c.bottom + 1), (int16_t)(c.right + 1) };/* document */
    }
}

static void set_rgn_rect(maddr rgnfield, QRect r) { maddr h = rd32(rgnfield); if (h) qd_set_rect_rgn(h, r); }

/* Recompute structure/content regions and visRgns of all windows. */
static void calc_vis_all(void) {
    RgnList covered;
    QRect gray = screen_rect();
    if (menubar_visible) gray.top = MBAR_H;
    rgn_from_rect(&covered, (QRect){0, 0, 0, 0});
    RgnList avail; rgn_from_rect(&avail, gray);
    for (maddr w = window_list(); w; w = rd32(w + W_next)) {
        QRect c = win_global_content(w), s = win_struct(w);
        set_rgn_rect(w + W_cont, c); set_rgn_rect(w + W_struc, s);
        RgnList vis;
        if (rd8(w + W_visible)) {
            RgnList cr, t; rgn_from_rect(&cr, c);
            rgn_op(&cr, &avail, &t, 0);
            rgn_op(&t, &covered, &vis, 2);
            rgn_free(&cr); rgn_free(&t);
            RgnList sr, u; rgn_from_rect(&sr, s); rgn_op(&covered, &sr, &u, 1); rgn_free(&covered); rgn_free(&sr); covered = u;
        } else rgn_from_rect(&vis, (QRect){0});
        /* to local coordinates */
        QRect b = rd_rect(qd_port_bits(w) + 6);
        rgn_offset(&vis, b.top, b.left);
        rgn_set(rd32(w + GP_visRgn), &vis);
        rgn_free(&vis);
    }
    rgn_free(&covered); rgn_free(&avail);
}

/* ---- drawing of desktop, frames, menu bar ---- */
static void with_wmgr_port(void (*fn)(void *), void *arg) {
    maddr save = qd_the_port();
    qd_set_port(wmgr_port);
    fn(arg);
    qd_set_port(save);
}
static Surface screen_surface(void) {
    Surface s = { SCREEN_BASE, (uint32_t)qd_screen_w, screen_rect(), 8, qd_device_ctab() };
    return s;
}
static int idx_white(void) { return qd_rgb_to_index(0xFFFF, 0xFFFF, 0xFFFF); }
static int idx_black(void) { return qd_rgb_to_index(0, 0, 0); }

static void draw_frame(maddr w) {
    if (!rd8(w + W_visible)) return;
    Surface s = screen_surface();
    QRect c = win_global_content(w), st = win_struct(w);
    RgnList clip, sr, cr, ring, avail;
    QRect gray = screen_rect(); if (menubar_visible) gray.top = MBAR_H;
    rgn_from_rect(&avail, gray);
    /* frame area not covered by windows in front */
    rgn_from_rect(&sr, st); rgn_from_rect(&cr, c);
    rgn_op(&sr, &cr, &ring, 2);
    RgnList front; rgn_from_rect(&front, (QRect){0});
    for (maddr o = window_list(); o && o != w; o = rd32(o + W_next)) {
        if (!rd8(o + W_visible)) continue;
        RgnList os, u; rgn_from_rect(&os, win_struct(o)); rgn_op(&front, &os, &u, 1); rgn_free(&front); rgn_free(&os); front = u;
    }
    RgnList t; rgn_op(&ring, &front, &t, 2); rgn_op(&t, &avail, &clip, 0); rgn_free(&t);
    int black = idx_black(), white = idx_white();
    int p = win_proc(w) & 0xF;
    qd_fill_rect_index(&s, &clip, st, black);
    QRect inner = { (int16_t)(st.top + 1), (int16_t)(st.left + 1), (int16_t)(st.bottom - 1), (int16_t)(st.right - 1) };
    if (p == 1) {
        qd_fill_rect_index(&s, &clip, inner, white);
        QRect i2 = { (int16_t)(st.top + 3), (int16_t)(st.left + 3), (int16_t)(st.bottom - 3), (int16_t)(st.right - 3) };
        qd_fill_rect_index(&s, &clip, i2, black);
        QRect i3 = { (int16_t)(st.top + 5), (int16_t)(st.left + 5), (int16_t)(st.bottom - 5), (int16_t)(st.right - 5) };
        qd_fill_rect_index(&s, &clip, i3, white);
    } else if (p == 0 || p == 4 || p == 8 || p == 12) {
        QRect title = { (int16_t)(st.top + 1), (int16_t)(st.left + 1), (int16_t)(c.top - 1), (int16_t)(st.right - 1) };
        qd_fill_rect_index(&s, &clip, title, white);
        if (rd8(w + W_hilited))
            for (int y = title.top + 3; y < title.bottom - 2; y += 2) {
                QRect line = { (int16_t)y, (int16_t)(title.left + 2), (int16_t)(y + 1), (int16_t)(title.right - 2) };
                qd_fill_rect_index(&s, &clip, line, black);
            }
        /* title text */
        maddr th = rd32(w + W_title);
        if (th && rd32(th)) {
            uint8_t buf[256]; int n = rd8(rd32(th)); mget(buf, rd32(th) + 1, (uint32_t)n);
            maddr save = qd_the_port(); qd_set_port(wmgr_port);
            int tw = n * 7;
            QRect tr = { title.top, (int16_t)((title.left + title.right) / 2 - tw / 2 - 6), title.bottom, (int16_t)((title.left + title.right) / 2 + tw / 2 + 6) };
            qd_fill_rect_index(&s, &clip, tr, white);
            wr16(wmgr_port + GP_pnLoc, (uint16_t)(title.bottom - 4)); wr16(wmgr_port + GP_pnLoc + 2, (uint16_t)(tr.left + 6));
            rgn_set(rd32(wmgr_port + GP_clipRgn), &clip);
            font_draw_text(wmgr_port, buf, n);
            qd_set_rect_rgn(rd32(wmgr_port + GP_clipRgn), (QRect){-32767, -32767, 32767, 32767});
            qd_set_port(save);
        }
    } else if (p == 3) {
        QRect sh1 = { (int16_t)(c.bottom + 1), (int16_t)(c.left + 1), (int16_t)st.bottom, (int16_t)st.right };
        qd_fill_rect_index(&s, &clip, sh1, black);
    }
    rgn_free(&clip); rgn_free(&sr); rgn_free(&cr); rgn_free(&ring); rgn_free(&avail); rgn_free(&front);
    qd_screen_dirty++;
}

static void paint_desktop(const RgnList *area) {
    Surface s = screen_surface();
    static const uint8_t gray[8] = {0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55};
    RgnList a = *area;
    if (menubar_visible) { RgnList mb, t; rgn_from_rect(&mb, (QRect){0, 0, MBAR_H, (int16_t)qd_screen_w}); rgn_op(area, &mb, &t, 2); rgn_free(&mb); a = t; }
    (void)gray;
    qd_fill_rect_index(&s, &a, s.bounds, idx_black());
    if (a.r != area->r) rgn_free(&a);
}

/* ---- menus ---- */
typedef struct { maddr handle; int16_t id; } MenuEnt;
static MenuEnt menus[32]; static int nmenus;
static int hilited_menu;

static void draw_menubar(void) {
    if (!menubar_visible) return;
    Surface s = screen_surface();
    RgnList all; rgn_from_rect(&all, screen_rect());
    QRect bar = {0, 0, MBAR_H, (int16_t)qd_screen_w};
    qd_fill_rect_index(&s, &all, bar, idx_white());
    QRect line = {MBAR_H - 1, 0, MBAR_H, (int16_t)qd_screen_w};
    qd_fill_rect_index(&s, &all, line, idx_black());
    maddr save = qd_the_port(); qd_set_port(wmgr_port);
    int x = 10;
    for (int i = 0; i < nmenus; i++) {
        maddr m = rd32(menus[i].handle);
        int n = rd8(m + 14);
        uint8_t t[256]; mget(t, m + 15, (uint32_t)n);
        if (n == 1 && t[0] == 0x14) { t[0] = '@'; }   /* apple */
        wr16(wmgr_port + GP_pnLoc, 14); wr16(wmgr_port + GP_pnLoc + 2, (uint16_t)(x + 4));
        font_draw_text(wmgr_port, t, n);
        x += 16 + n * 7;
    }
    qd_set_port(save);
    rgn_free(&all);
    qd_screen_dirty++;
}

void wm_redraw_all(void) {
    calc_vis_all();
    RgnList all; rgn_from_rect(&all, screen_rect());
    RgnList cov; rgn_from_rect(&cov, (QRect){0});
    for (maddr w = window_list(); w; w = rd32(w + W_next)) if (rd8(w + W_visible)) {
        RgnList sr, u; rgn_from_rect(&sr, win_struct(w)); rgn_op(&cov, &sr, &u, 1); rgn_free(&cov); rgn_free(&sr); cov = u;
    }
    RgnList desk; rgn_op(&all, &cov, &desk, 2);
    paint_desktop(&desk);
    rgn_free(&all); rgn_free(&cov); rgn_free(&desk);
    for (maddr w = window_list(); w; w = rd32(w + W_next)) draw_frame(w);
    draw_menubar();
}

/* Invalidate (global rect) for all windows: add to update regions, erase content */
static void inval_global(QRect g) {
    for (maddr w = window_list(); w; w = rd32(w + W_next)) {
        if (!rd8(w + W_visible)) continue;
        QRect c = rect_sect(win_global_content(w), g);
        if (rect_empty(c)) continue;
        RgnList u, a, o; rgn_get(rd32(w + W_update), &u); rgn_from_rect(&a, c); rgn_op(&u, &a, &o, 1);
        rgn_set(rd32(w + W_update), &o); rgn_free(&u); rgn_free(&a); rgn_free(&o);
    }
}

/* ---- window creation ---- */
static maddr new_window(maddr storage, QRect bounds, const char *title, int visible, int16_t proc, maddr behind, int goaway, uint32_t refcon, int color) {
    maddr w = storage ? storage : NewPtrClear(D_SIZE);
    mset(w, 0, W_SIZE);
    maddr save = qd_the_port();
    if (color) InitCPort(w); else InitPort(w);
    /* port coordinates: portRect = (0,0,h,w); bounds topLeft = -global topLeft */
    int h = bounds.bottom - bounds.top, wd = bounds.right - bounds.left;
    wr_rect(w + GP_portRect, (QRect){0, 0, (int16_t)h, (int16_t)wd});
    maddr bm = qd_port_bits(w);
    QRect b = rd_rect(bm + 6);
    wr_rect(bm + 6, rect_offset(b, -bounds.top, -bounds.left));
    qd_set_port(save);
    wr16(w + W_kind, 8);            /* userKind */
    wr8(w + W_visible, 0);
    wr8(w + W_goAway, (uint8_t)(goaway ? 1 : 0));
    wr32(w + W_struc, qd_new_rgn()); wr32(w + W_cont, qd_new_rgn()); wr32(w + W_update, qd_new_rgn());
    wr32(w + W_defProc, (uint32_t)(uint16_t)proc);
    maddr th = NewHandle(256); c_to_pstr(title ? title : "", rd32(th)); wr32(w + W_title, th);
    wr32(w + W_refCon, refcon);
    /* insert into window list */
    if (behind == (maddr)0xFFFFFFFF || behind == 0xFFFFFF || !window_list()) {
        wr32(w + W_next, window_list()); wr32(LM_WindowList, w);
    } else if (behind == 0) {
        maddr p = window_list(); while (rd32(p + W_next)) p = rd32(p + W_next); wr32(p + W_next, w); wr32(w + W_next, 0);
    } else {
        wr32(w + W_next, rd32(behind + W_next)); wr32(behind + W_next, w);
    }
    if (visible) {
        wr8(w + W_visible, 1);
        if (w == window_list()) wr8(w + W_hilited, 1);
        calc_vis_all();
        inval_global(win_global_content(w));
        draw_frame(w);
        RgnList cr; rgn_from_rect(&cr, win_global_content(w));
        /* erase content */
        maddr save2 = qd_the_port(); qd_set_port(w);
        Surface s = qd_port_surface(w); RgnList clip; qd_port_clip(w, &clip);
        qd_fill_rect_index(&s, &clip, rd_rect(w + GP_portRect), qd_port_bk_index(w));
        rgn_free(&clip); rgn_free(&cr); qd_set_port(save2);
    } else calc_vis_all();
    return w;
}

static void T_InitWindows(uint16_t w) {
    (void)w;
    wmgr_port = NewPtrClear(GP_SIZE);
    maddr save = qd_the_port();
    InitCPort(wmgr_port);
    qd_set_port(save);
    wr32(LM_WMgrPort, wmgr_port); wr32(LM_WMgrCPort, wmgr_port);
    maddr gray = qd_new_rgn(); qd_set_rect_rgn(gray, (QRect){MBAR_H, 0, (int16_t)qd_screen_h, (int16_t)qd_screen_w});
    wr32(LM_GrayRgn, gray);
    wr32(LM_WindowList, 0);
    wm_redraw_all();
}
static void T_GetWMgrPort(uint16_t w) { (void)w; wr32(pop32(), wmgr_port); }
static void T_GetCWMgrPort(uint16_t w) { (void)w; wr32(pop32(), wmgr_port); }

static void T_NewWindow(uint16_t w) {
    uint32_t refcon = pop32(); int goaway = pop8(); maddr behind = pop32(); int16_t proc = pops16();
    int vis = pop8(); maddr title = pop32(); maddr br = pop32(); maddr storage = pop32();
    char t[256]; pstr_to_c(title, t, sizeof t);
    ret32(new_window(storage, rd_rect(br), t, vis, proc, behind, goaway, refcon, w == TRAP_NewCWindow));
}
static maddr get_new_window(int16_t id, maddr storage, maddr behind, int color) {
    maddr r = GetResource(FOURCC('W','I','N','D'), id);
    if (!r) return 0;
    maddr p = rd32(r);
    char t[256]; pstr_to_c(p + 18, t, sizeof t);
    return new_window(storage, rd_rect(p), t, rd16(p + 10) >> 8, rds16(p + 8), behind, rd16(p + 12) >> 8, rd32(p + 14), color);
}
static void T_GetNewWindow(uint16_t w) { maddr behind = pop32(), storage = pop32(); int16_t id = pops16(); ret32(get_new_window(id, storage, behind, w == TRAP_GetNewCWindow)); }

static void unlink_window(maddr w) {
    maddr p = window_list();
    if (p == w) { wr32(LM_WindowList, rd32(w + W_next)); return; }
    while (p && rd32(p + W_next) != w) p = rd32(p + W_next);
    if (p) wr32(p + W_next, rd32(w + W_next));
}
static void close_window(maddr w) {
    QRect s = win_struct(w);
    int vis = rd8(w + W_visible);
    unlink_window(w);
    if (vis) { calc_vis_all(); inval_global(s); wm_redraw_all(); }
    if (qd_the_port() == w) qd_set_port(wmgr_port);
    maddr f = window_list();
    if (f && !rd8(f + W_hilited)) { wr8(f + W_hilited, 1); event_post(8, f | 0); }
}
static void T_CloseWindow(uint16_t w) { (void)w; close_window(pop32()); }
static void T_DisposeWindow(uint16_t w) { (void)w; maddr win = pop32(); close_window(win); }

static void show_hide(maddr w, int show) {
    if (rd8(w + W_visible) == show) return;
    wr8(w + W_visible, (uint8_t)show);
    QRect s = win_struct(w);
    calc_vis_all();
    if (show) { inval_global(win_global_content(w)); draw_frame(w); }
    else { inval_global(s); wm_redraw_all(); }
    qd_screen_dirty++;
}
static void T_ShowWindow(uint16_t w) { (void)w; show_hide(pop32(), 1); }
static void T_HideWindow(uint16_t w) { (void)w; show_hide(pop32(), 0); }
static void T_ShowHide(uint16_t w) { (void)w; int s = pop8(); show_hide(pop32(), s); }

static void select_window(maddr w) {
    maddr old = window_list();
    if (old == w) return;
    unlink_window(w);
    wr32(w + W_next, window_list()); wr32(LM_WindowList, w);
    if (old) { wr8(old + W_hilited, 0); }
    wr8(w + W_hilited, 1);
    calc_vis_all();
    inval_global(win_global_content(w));
    wm_redraw_all();
    qd_activate_window_palette(w);
    if (old) event_post(8, old);           /* deactivate */
    event_post(8, w | 0);                  /* activate (modifiers set by event code) */
}
static void T_SelectWindow(uint16_t w) { (void)w; select_window(pop32()); }
static void T_BringToFront(uint16_t w) { (void)w; select_window(pop32()); }
static void T_SendBehind(uint16_t w) {
    (void)w; maddr behind = pop32(), win = pop32();
    unlink_window(win);
    if (!behind) { maddr p = window_list(); if (!p) { wr32(LM_WindowList, win); wr32(win + W_next, 0); } else { while (rd32(p + W_next)) p = rd32(p + W_next); wr32(p + W_next, win); wr32(win + W_next, 0); } }
    else { wr32(win + W_next, rd32(behind + W_next)); wr32(behind + W_next, win); }
    calc_vis_all(); wm_redraw_all();
}
static void T_FrontWindow(uint16_t w) {
    (void)w;
    maddr f = window_list();
    while (f && !rd8(f + W_visible)) f = rd32(f + W_next);
    ret32(f);
}
static void T_HiliteWindow(uint16_t w) { (void)w; int f = pop8(); maddr win = pop32(); wr8(win + W_hilited, (uint8_t)f); draw_frame(win); }
static void move_window(maddr w, int h, int v, int front) {
    QRect old = win_struct(w);
    maddr bm = qd_port_bits(w);
    QRect pr = rd_rect(w + GP_portRect), b = rd_rect(bm + 6);
    int bw = b.right - b.left, bh = b.bottom - b.top;
    b.left = (int16_t)(pr.left - h); b.top = (int16_t)(pr.top - v); b.right = (int16_t)(b.left + bw); b.bottom = (int16_t)(b.top + bh);
    wr_rect(bm + 6, b);
    if (front) select_window(w);
    if (rd8(w + W_visible)) { calc_vis_all(); inval_global(old); inval_global(win_global_content(w)); wm_redraw_all(); }
    else calc_vis_all();
}
static void T_MoveWindow(uint16_t w) { (void)w; int front = pop8(); int16_t v = pops16(), h = pops16(); move_window(pop32(), h, v, front); }
static void T_SizeWindow(uint16_t w) {
    (void)w; pop8(); int16_t hh = pops16(), ww = pops16(); maddr win = pop32();
    if (!hh || !ww) return;
    QRect old = win_struct(win);
    QRect pr = rd_rect(win + GP_portRect); pr.bottom = (int16_t)(pr.top + hh); pr.right = (int16_t)(pr.left + ww);
    wr_rect(win + GP_portRect, pr);
    calc_vis_all(); inval_global(old); inval_global(win_global_content(win)); wm_redraw_all();
}
static void T_DragWindow(uint16_t w) { (void)w; pop32(); pop32(); maddr win = pop32(); select_window(win); }
static void T_GrowWindow(uint16_t w) { (void)w; pop32(); pop32(); pop32(); ret32(0); }
static void T_TrackBox(uint16_t w) { (void)w; pop16(); pop32(); pop32(); retbool(0); }
static void T_TrackGoAway(uint16_t w) { (void)w; pop32(); pop32(); retbool(1); }
static void T_ZoomWindow(uint16_t w) { (void)w; pop8(); pop16(); pop32(); }
static void T_DrawGrowIcon(uint16_t w) { (void)w; pop32(); }
static void T_SetWTitle(uint16_t w) { (void)w; maddr t = pop32(); maddr win = pop32(); mcopy(rd32(rd32(win + W_title)), t, rd8(t) + 1u); draw_frame(win); }
static void T_GetWTitle(uint16_t w) { (void)w; maddr t = pop32(); maddr win = pop32(); maddr h = rd32(rd32(win + W_title)); mcopy(t, h, rd8(h) + 1u); }
static void T_SetWRefCon(uint16_t w) { (void)w; uint32_t r = pop32(); wr32(pop32() + W_refCon, r); }
static void T_GetWRefCon(uint16_t w) { (void)w; ret32(rd32(pop32() + W_refCon)); }
static void T_SetWindowPic(uint16_t w) { (void)w; maddr p = pop32(); wr32(pop32() + W_pic, p); }

/* FindWindow(pt, &w): part code */
static void T_FindWindow(uint16_t w) {
    (void)w; maddr out = pop32(); QPoint p = pt_from32(pop32());
    wr32(out, 0);
    if (p.v < MBAR_H && (menubar_visible || platform_menu_has_pending())) { ret16(1); return; }   /* inMenuBar */
    for (maddr win = window_list(); win; win = rd32(win + W_next)) {
        if (!rd8(win + W_visible)) continue;
        QRect s = win_struct(win), c = win_global_content(win);
        if (p.v >= s.top && p.v < s.bottom && p.h >= s.left && p.h < s.right) {
            wr32(out, win);
            if (p.v >= c.top && p.v < c.bottom && p.h >= c.left && p.h < c.right) { ret16(3); return; }  /* inContent */
            ret16(4); return;                                                                        /* inDrag */
        }
    }
    ret16(0);  /* inDesk */
}

static void T_BeginUpdate(uint16_t w) {
    (void)w; maddr win = pop32();
    /* visRgn = visRgn ∩ updateRgn (update is global; convert to local) */
    RgnList vis, upd, out;
    rgn_get(rd32(win + GP_visRgn), &vis);
    rgn_get(rd32(win + W_update), &upd);
    QRect b = rd_rect(qd_port_bits(win) + 6);
    rgn_offset(&upd, b.top, b.left);
    rgn_op(&vis, &upd, &out, 0);
    rgn_set(rd32(win + GP_visRgn), &out);
    rgn_free(&vis); rgn_free(&upd); rgn_free(&out);
    qd_set_rect_rgn(rd32(win + W_update), (QRect){0});
}
static void T_EndUpdate(uint16_t w) { (void)w; pop32(); calc_vis_all(); }
static void inval_local(maddr win, QRect r) {
    QRect b = rd_rect(qd_port_bits(win) + 6);
    QRect g = rect_offset(r, -b.top, -b.left);
    g = rect_sect(g, win_global_content(win));
    if (rect_empty(g)) return;
    RgnList u, a, o; rgn_get(rd32(win + W_update), &u); rgn_from_rect(&a, g); rgn_op(&u, &a, &o, 1);
    rgn_set(rd32(win + W_update), &o); rgn_free(&u); rgn_free(&a); rgn_free(&o);
}
static void T_InvalRect(uint16_t w) { (void)w; inval_local(qd_the_port(), rd_rect(pop32())); }
static void T_InvalRgn(uint16_t w) { (void)w; RgnList l; rgn_get(pop32(), &l); inval_local(qd_the_port(), l.bbox); rgn_free(&l); }
static void valid_local(maddr win, QRect r) {
    QRect b = rd_rect(qd_port_bits(win) + 6);
    RgnList u, a, o; rgn_get(rd32(win + W_update), &u); rgn_from_rect(&a, rect_offset(r, -b.top, -b.left)); rgn_op(&u, &a, &o, 2);
    rgn_set(rd32(win + W_update), &o); rgn_free(&u); rgn_free(&a); rgn_free(&o);
}
static void T_ValidRect(uint16_t w) { (void)w; valid_local(qd_the_port(), rd_rect(pop32())); }
static void T_ValidRgn(uint16_t w) { (void)w; RgnList l; rgn_get(pop32(), &l); valid_local(qd_the_port(), l.bbox); rgn_free(&l); }
static void T_CalcVis(uint16_t w) { (void)w; pop32(); calc_vis_all(); }
static void T_CalcVisBehind(uint16_t w) { (void)w; pop32(); pop32(); calc_vis_all(); }
static void T_PaintOne(uint16_t w) { (void)w; pop32(); pop32(); wm_redraw_all(); }
static void T_PaintBehind(uint16_t w) {
    (void)w; maddr clobbered = pop32(); pop32();
    RgnList l; rgn_get(clobbered, &l);
    if (l.n) { inval_global(l.bbox); }
    rgn_free(&l);
    wm_redraw_all();
}
static void T_ClipAbove(uint16_t w) { (void)w; pop32(); }
static void T_SaveOld(uint16_t w) { (void)w; pop32(); }
static void T_DrawNew(uint16_t w) { (void)w; pop8(); pop32(); wm_redraw_all(); }
static void T_CheckUpdate(uint16_t w) { (void)w; pop32(); retbool(0); }

/* Find a window needing update (called by the event manager). */
int wm_pending_update(maddr *out) {
    for (maddr w = window_list(); w; w = rd32(w + W_next)) {
        if (!rd8(w + W_visible)) continue;
        RgnList u; rgn_get(rd32(w + W_update), &u);
        int n = u.n; rgn_free(&u);
        if (n) { *out = w; return 1; }
    }
    return 0;
}

/* Hide/show the menu bar (games do it by editing GrayRgn + MBarHeight; we also offer an API). */
void wm_set_menubar_visible(int v) {
    menubar_visible = v;
    calc_vis_all();
    wm_redraw_all();
}
int wm_menubar_visible(void) { return menubar_visible && rd16(LM_MBarHeight) != 0; }

/* ======================================================= Menu Manager == */
/* MenuInfo: menuID(2) menuWidth(2) menuHeight(2) menuProc(4) enableFlags(4) menuData: title, items... */
static void T_InitMenus(uint16_t w) { (void)w; nmenus = 0; }
static maddr menu_by_id(int16_t id) { for (int i = 0; i < nmenus; i++) if (rds16(rd32(menus[i].handle)) == id) return menus[i].handle; return 0; }
static void insert_menu(maddr h, int16_t before) {
    int16_t id = rds16(rd32(h));
    for (int i = 0; i < nmenus; i++) if (menus[i].id == id) return;
    int pos = nmenus;
    if (before) for (int i = 0; i < nmenus; i++) if (menus[i].id == before) { pos = i; break; }
    if (before == -1) { pos = nmenus; }
    memmove(&menus[pos + 1], &menus[pos], sizeof(MenuEnt) * (nmenus - pos));
    menus[pos].handle = h; menus[pos].id = id; nmenus++;
    platform_menu_sync();
}
static void T_InsertMenu(uint16_t w) { (void)w; int16_t before = pops16(); insert_menu(pop32(), before); }
static void T_DeleteMenu(uint16_t w) { (void)w; int16_t id = pops16(); for (int i = 0; i < nmenus; i++) if (menus[i].id == id) { memmove(&menus[i], &menus[i + 1], sizeof(MenuEnt) * (nmenus - i - 1)); nmenus--; break; } platform_menu_sync(); }
static void T_GetMenuHandle(uint16_t w) { (void)w; ret32(menu_by_id(pops16())); }
static void T_GetMenu(uint16_t w) {
    (void)w; int16_t id = pops16();
    maddr h = GetResource(FOURCC('M','E','N','U'), id);
    ret32(h);
}
static void T_GetNewMBar(uint16_t w) {
    (void)w; int16_t id = pops16();
    maddr r = GetResource(FOURCC('M','B','A','R'), id);
    if (!r) { ret32(0); return; }
    maddr list = NewHandleClear(2 + 64 * 4);
    int n = rd16(rd32(r));
    wr16(rd32(list), (uint16_t)n);
    for (int i = 0; i < n; i++) {
        int16_t mid = rds16(rd32(r) + 2 + 2 * i);
        wr32(rd32(list) + 2 + 4 * i, GetResource(FOURCC('M','E','N','U'), mid));
    }
    ret32(list);
}
static void T_SetMenuBar(uint16_t w) {
    (void)w; maddr list = pop32();
    nmenus = 0;
    int n = rd16(rd32(list));
    for (int i = 0; i < n; i++) insert_menu(rd32(rd32(list) + 2 + 4 * i), 0);
    platform_menu_sync();
}
static void T_GetMenuBar(uint16_t w) {
    (void)w; maddr list = NewHandleClear(2 + 4u * nmenus);
    wr16(rd32(list), (uint16_t)nmenus);
    for (int i = 0; i < nmenus; i++) wr32(rd32(list) + 2 + 4 * i, menus[i].handle);
    ret32(list);
}
static void T_ClearMenuBar(uint16_t w) { (void)w; nmenus = 0; platform_menu_sync(); }
static void T_DrawMenuBar(uint16_t w) { (void)w; draw_menubar(); platform_menu_sync(); }
static void T_HiliteMenu(uint16_t w) { (void)w; hilited_menu = pops16(); }
static void T_FlashMenuBar(uint16_t w) { (void)w; pop16(); }
static void T_NewMenu(uint16_t w) {
    (void)w; maddr title = pop32(); int16_t id = pops16();
    int tl = rd8(title);
    maddr h = NewHandleClear(14 + tl + 2u);
    maddr m = rd32(h);
    wr16(m, (uint16_t)id); wr32(m + 10, 0xFFFFFFFF);
    mcopy(m + 14, title, tl + 1u);
    wr8(m + 15 + tl, 0);
    ret32(h);
}
/* item iteration: returns address of item i (1-based) name pstr, or 0 */
maddr menu_item(maddr h, int item) {
    maddr m = rd32(h);
    maddr p = m + 14 + 1 + rd8(m + 14);
    for (int i = 1; rd8(p); i++) {
        if (i == item) return p;
        p += 1 + rd8(p) + 4;
    }
    return 0;
}
int menu_count(maddr h) { int n = 0; while (menu_item(h, n + 1)) n++; return n; }
static void append_item(maddr h, const char *text, int len) {
    /* metacharacters: ; separates items, ^icon !mark <style /cmd (disable */
    int start = 0;
    for (int i = 0; i <= len; i++) {
        if (i == len || text[i] == ';' || text[i] == '\r') {
            char name[256]; int nl = 0; uint8_t icon = 0, key = 0, mark = 0, style = 0; int disabled = 0;
            for (int k = start; k < i; k++) {
                char c = text[k];
                if (c == '^' && k + 1 < i) { icon = (uint8_t)(text[++k] - '0'); }
                else if (c == '!' && k + 1 < i) { mark = (uint8_t)text[++k]; }
                else if (c == '<' && k + 1 < i) { k++; }
                else if (c == '/' && k + 1 < i) { key = (uint8_t)text[++k]; }
                else if (c == '(') { disabled = 1; }
                else if (nl < 255) name[nl++] = c;
            }
            if (nl == 0 && i == len && start == len) break;
            int count = menu_count(h);
            maddr m = rd32(h);
            maddr end = m + 14 + 1 + rd8(m + 14);
            while (rd8(end)) end += 1 + rd8(end) + 4;
            uint32_t off = end - m;
            SetHandleSize(h, off + 1 + nl + 4 + 1);
            m = rd32(h);
            end = m + off;
            wr8(end, (uint8_t)nl); mput(end + 1, name, (uint32_t)nl);
            wr8(end + 1 + nl, icon); wr8(end + 2 + nl, key); wr8(end + 3 + nl, mark); wr8(end + 4 + nl, style);
            wr8(end + 5 + nl, 0);
            if (disabled && count + 1 < 32) wr32(m + 10, rd32(m + 10) & ~(1u << (count + 1)));
            start = i + 1;
        }
    }
    platform_menu_sync();
}
static void T_AppendMenu(uint16_t w) { (void)w; maddr data = pop32(); maddr h = pop32(); char t[256]; int n = rd8(data); mget(t, data + 1, (uint32_t)n); append_item(h, t, n); }
static void T_InsertMenuItem(uint16_t w) { (void)w; pop16(); maddr data = pop32(); maddr h = pop32(); char t[256]; int n = rd8(data); mget(t, data + 1, (uint32_t)n); append_item(h, t, n); }
static void T_AppendResMenu(uint16_t w) { (void)w; pop32(); pop32(); }
static void T_InsertResMenu(uint16_t w) { (void)w; pop16(); pop32(); pop32(); }
static void T_CountMItems(uint16_t w) { (void)w; ret16((uint16_t)menu_count(pop32())); }
static void set_enable(maddr h, int item, int on) {
    maddr m = rd32(h);
    if (item < 32) wr32(m + 10, on ? (rd32(m + 10) | (1u << item)) : (rd32(m + 10) & ~(1u << item)));
    platform_menu_sync();
}
static void T_EnableItem(uint16_t w) { (void)w; int16_t i = pops16(); set_enable(pop32(), i, 1); }
static void T_DisableItem(uint16_t w) { (void)w; int16_t i = pops16(); set_enable(pop32(), i, 0); }
static void T_CheckItem(uint16_t w) { (void)w; int on = pop8(); int16_t i = pops16(); maddr p = menu_item(pop32(), i); if (p) wr8(p + 1 + rd8(p) + 2, on ? 0x12 : 0); platform_menu_sync(); }
static void T_SetItemMark(uint16_t w) { (void)w; uint8_t c = (uint8_t)pop16(); int16_t i = pops16(); maddr p = menu_item(pop32(), i); if (p) wr8(p + 1 + rd8(p) + 2, c); platform_menu_sync(); }
static void T_GetItemMark(uint16_t w) { (void)w; maddr out = pop32(); int16_t i = pops16(); maddr p = menu_item(pop32(), i); wr16(out, p ? rd8(p + 1 + rd8(p) + 2) : 0); }
static void T_SetItemCmd(uint16_t w) { (void)w; uint8_t c = (uint8_t)pop16(); int16_t i = pops16(); maddr p = menu_item(pop32(), i); if (p) wr8(p + 1 + rd8(p) + 1, c); platform_menu_sync(); }
static void T_GetItemCmd(uint16_t w) { (void)w; maddr out = pop32(); int16_t i = pops16(); maddr p = menu_item(pop32(), i); wr16(out, p ? rd8(p + 1 + rd8(p) + 1) : 0); }
static void T_SetItemStyle(uint16_t w) { (void)w; pop16(); pop16(); pop32(); }
static void T_GetMenuItemText(uint16_t w) { (void)w; maddr out = pop32(); int16_t i = pops16(); maddr p = menu_item(pop32(), i); if (p) mcopy(out, p, rd8(p) + 1u); else wr8(out, 0); }
static void T_SetMenuItemText(uint16_t w) {
    (void)w; maddr str = pop32(); int16_t i = pops16(); maddr h = pop32();
    maddr p = menu_item(h, i); if (!p) return;
    maddr m = rd32(h);
    uint32_t off = p - m, oldlen = rd8(p), newlen = rd8(str);
    uint32_t total = GetHandleSize(h);
    uint8_t *buf = malloc(total + newlen); mget(buf, m, total);
    uint8_t *out = malloc(total + newlen + 1);
    memcpy(out, buf, off); out[off] = (uint8_t)newlen; mget(out + off + 1, str + 1, newlen);
    memcpy(out + off + 1 + newlen, buf + off + 1 + oldlen, total - off - 1 - oldlen);
    uint32_t nt = total - oldlen + newlen;
    SetHandleSize(h, nt); mput(rd32(h), out, nt);
    free(buf); free(out);
    platform_menu_sync();
}
/* MenuKey(ch): long (menu << 16 | item) */
static void T_MenuKey(uint16_t w) {
    (void)w; uint8_t ch = (uint8_t)pop16();
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    for (int i = 0; i < nmenus; i++) {
        maddr h = menus[i].handle;
        int n = menu_count(h);
        for (int k = 1; k <= n; k++) {
            maddr p = menu_item(h, k);
            uint8_t key = rd8(p + 1 + rd8(p) + 1);
            if (key >= 'a' && key <= 'z') key -= 32;
            if (key && key == ch && (rd32(rd32(h) + 10) & 1) && (k >= 32 || (rd32(rd32(h) + 10) & (1u << k)))) {
                ret32((uint32_t)menus[i].id << 16 | (uint32_t)k); hilited_menu = menus[i].id; return;
            }
        }
    }
    ret32(0);
}
/* MenuSelect(startPt): long — native menus deliver their pending choice here */
static void T_MenuSelect(uint16_t w) {
    (void)w; pop32();
    uint32_t r = 0;
    platform_menu_pending(&r);
    if (r) hilited_menu = (int16_t)(r >> 16);
    ret32(r);
}
static void T_PopUpMenuSelect(uint16_t w) { (void)w; pop16(); pop16(); pop16(); pop32(); ret32(0); }
static void T_CalcMenuSize(uint16_t w) { (void)w; pop32(); }
static void T_SetMenuFlash(uint16_t w) { (void)w; pop16(); }
static void T_DisposeMenu(uint16_t w) { (void)w; pop32(); }

int wm_menu_count(void) { return nmenus; }
maddr wm_menu_handle(int i) { return menus[i].handle; }

/* ==================================================== Control Manager == */
/* ControlRecord: next(4) owner(4) rect(8) vis(1) hilite(1) value(2) min(2) max(2) defProc(4) data(4) action(4) refCon(4) title(256) */
static maddr new_control(maddr win, QRect r, const char *title, int vis, int16_t value, int16_t min, int16_t max, int16_t proc, uint32_t refcon) {
    maddr h = NewHandleClear(296);
    maddr c = rd32(h);
    wr32(c + 4, win); wr_rect(c + 8, r); wr8(c + 16, (uint8_t)(vis ? 255 : 0));
    wr16(c + 18, (uint16_t)value); wr16(c + 20, (uint16_t)min); wr16(c + 22, (uint16_t)max);
    wr32(c + 24, (uint32_t)(uint16_t)proc); wr32(c + 36, refcon);
    c_to_pstr(title, c + 40);
    wr32(c, rd32(win + W_ctlList)); wr32(win + W_ctlList, h);
    return h;
}
static void draw_control(maddr h) {
    maddr c = rd32(h);
    if (!rd8(c + 16)) return;
    maddr port = rd32(c + 4);
    maddr save = qd_the_port(); qd_set_port(port);
    QRect r = rd_rect(c + 8);
    Surface s = qd_port_surface(port);
    RgnList clip; qd_port_clip(port, &clip);
    int black = idx_black(), white = idx_white();
    int kind = rd32(c + 24) & 0xF;
    uint8_t t[256]; int n = rd8(c + 40); mget(t, c + 41, (uint32_t)n);
    int tw = font_text_width(port, t, n);
    wr16(port + GP_txFont, 0); wr16(port + GP_txSize, 12);
    if (kind == 0) {           /* push button */
        qd_fill_rect_index(&s, &clip, r, black);
        QRect in = { (int16_t)(r.top + 1), (int16_t)(r.left + 1), (int16_t)(r.bottom - 1), (int16_t)(r.right - 1) };
        qd_fill_rect_index(&s, &clip, in, rd8(c + 17) == 1 ? black : white);
        int fg = rd8(c + 17) == 1 ? white : black;
        uint32_t ofg = rd32(port + GP_fgColor);
        wr32(port + GP_fgColor, qd_is_color_port(port) ? (uint32_t)fg : (fg == white ? 30u : 33u));
        wr16(port + GP_pnLoc, (uint16_t)((r.top + r.bottom) / 2 + 4)); wr16(port + GP_pnLoc + 2, (uint16_t)((r.left + r.right - tw) / 2));
        font_draw_text(port, t, n);
        wr32(port + GP_fgColor, ofg);
    } else {                   /* checkbox / radio */
        QRect box = { (int16_t)((r.top + r.bottom) / 2 - 6), (int16_t)(r.left + 2), (int16_t)((r.top + r.bottom) / 2 + 6), (int16_t)(r.left + 14) };
        qd_fill_rect_index(&s, &clip, box, black);
        QRect in = { (int16_t)(box.top + 1), (int16_t)(box.left + 1), (int16_t)(box.bottom - 1), (int16_t)(box.right - 1) };
        qd_fill_rect_index(&s, &clip, in, white);
        if (rds16(c + 18)) { QRect dot = { (int16_t)(box.top + 3), (int16_t)(box.left + 3), (int16_t)(box.bottom - 3), (int16_t)(box.right - 3) }; qd_fill_rect_index(&s, &clip, dot, black); }
        wr16(port + GP_pnLoc, (uint16_t)((r.top + r.bottom) / 2 + 4)); wr16(port + GP_pnLoc + 2, (uint16_t)(r.left + 18));
        font_draw_text(port, t, n);
    }
    rgn_free(&clip);
    qd_set_port(save);
}
static void T_GetControlValue(uint16_t w) { (void)w; ret16(rd16(rd32(pop32()) + 18)); }
static void T_SetControlValue(uint16_t w) { (void)w; int16_t v = pops16(); maddr h = pop32(); wr16(rd32(h) + 18, (uint16_t)v); draw_control(h); }
static void T_GetControlMinimum(uint16_t w) { (void)w; ret16(rd16(rd32(pop32()) + 20)); }
static void T_GetControlMaximum(uint16_t w) { (void)w; ret16(rd16(rd32(pop32()) + 22)); }
static void T_SetControlMinimum(uint16_t w) { (void)w; int16_t v = pops16(); wr16(rd32(pop32()) + 20, (uint16_t)v); }
static void T_SetControlMaximum(uint16_t w) { (void)w; int16_t v = pops16(); wr16(rd32(pop32()) + 22, (uint16_t)v); }
static void T_HiliteControl(uint16_t w) { (void)w; int16_t v = pops16(); maddr h = pop32(); wr8(rd32(h) + 17, (uint8_t)v); draw_control(h); }
static void T_DrawControls(uint16_t w) { (void)w; maddr win = pop32(); for (maddr h = rd32(win + W_ctlList); h; h = rd32(rd32(h))) draw_control(h); }
static void T_SetControlTitle(uint16_t w) { (void)w; maddr t = pop32(); maddr h = pop32(); mcopy(rd32(h) + 40, t, rd8(t) + 1u); draw_control(h); }
static void T_GetControlTitle(uint16_t w) { (void)w; maddr t = pop32(); maddr h = pop32(); mcopy(t, rd32(h) + 40, rd8(rd32(h) + 40) + 1u); }

/* ===================================================== Dialog Manager == */
/* DITL item: handle/proc(4) rect(8) type(1) len(1) data(len, padded to even) */
static maddr param_text[4];
static maddr dlg_filter_std;

static maddr ditl_item(maddr dlg, int item, int *type, QRect *r) {
    maddr items = rd32(dlg + D_items);
    if (!items) return 0;
    maddr p = rd32(items);
    int n = rds16(p) + 1;
    if (item < 1 || item > n) return 0;
    p += 2;
    for (int i = 1; i < item; i++) { int l = rd8(p + 13); p += 14 + l + (l & 1); }
    if (type) *type = rd8(p + 12);
    if (r) *r = rd_rect(p + 4);
    return p;
}
static int ditl_count(maddr dlg) { maddr items = rd32(dlg + D_items); return items ? rds16(rd32(items)) + 1 : 0; }

static void expand_params(const uint8_t *in, int n, uint8_t *out, int *on) {
    int k = 0;
    for (int i = 0; i < n && k < 255; i++) {
        if (in[i] == '^' && i + 1 < n && in[i + 1] >= '0' && in[i + 1] <= '3') {
            maddr h = param_text[in[i + 1] - '0'];
            if (h && rd32(h)) { int l = rd8(rd32(h)); for (int j = 0; j < l && k < 255; j++) out[k++] = rd8(rd32(h) + 1 + j); }
            i++;
        } else out[k++] = in[i];
    }
    *on = k;
}

static void draw_text_box(maddr port, QRect r, const uint8_t *t, int n) {
    /* simple word wrap */
    int a, d, wm, l; font_get_info(port, &a, &d, &wm, &l);
    int lh = a + d + l; if (lh <= 0) lh = 16;
    int y = r.top + a, start = 0;
    while (start < n && y <= r.bottom) {
        int end = start, lastsp = -1;
        while (end < n && t[end] != '\r') {
            if (t[end] == ' ') lastsp = end;
            if (font_text_width(port, t + start, end - start + 1) > r.right - r.left) break;
            end++;
        }
        int brk = end;
        if (end < n && t[end] != '\r' && lastsp > start) brk = lastsp;
        wr16(port + GP_pnLoc, (uint16_t)y); wr16(port + GP_pnLoc + 2, (uint16_t)r.left);
        font_draw_text(port, t + start, brk - start);
        start = brk; if (start < n && (t[start] == ' ' || t[start] == '\r')) start++;
        y += lh;
    }
}

static void draw_dialog_items(maddr dlg) {
    maddr save = qd_the_port(); qd_set_port(dlg);
    int n = ditl_count(dlg);
    for (int i = 1; i <= n; i++) {
        int type; QRect r; maddr it = ditl_item(dlg, i, &type, &r);
        int t = type & 0x7F;
        if (t == 0) {   /* userItem: call the draw proc */
            maddr proc = rd32(it);
            if (proc) { push32(dlg); push16((uint16_t)i); cpu_call(proc, NULL); cpu_set(R_SP, cpu_get(R_SP) + 6); }
            continue;
        }
        if ((t & 0x7C) == 4) { if (rd32(it)) draw_control(rd32(it)); continue; }
        if (t == 8 || t == 16) {
            maddr h = rd32(it);
            uint8_t buf[512]; int l = 0;
            if (h && rd32(h)) { l = (int)GetHandleSize(h); if (l > 255) l = 255; mget(buf, rd32(h), (uint32_t)l); }
            uint8_t ex[256]; int el; expand_params(buf, l, ex, &el);
            Surface s = qd_port_surface(dlg); RgnList clip; qd_port_clip(dlg, &clip);
            if (t == 16) {
                QRect fr = { (int16_t)(r.top - 3), (int16_t)(r.left - 3), (int16_t)(r.bottom + 3), (int16_t)(r.right + 3) };
                qd_fill_rect_index(&s, &clip, fr, idx_black());
                QRect in = { (int16_t)(r.top - 2), (int16_t)(r.left - 2), (int16_t)(r.bottom + 2), (int16_t)(r.right + 2) };
                qd_fill_rect_index(&s, &clip, in, idx_white());
            }
            rgn_free(&clip);
            draw_text_box(dlg, r, ex, el);
            continue;
        }
        if (t == 32) {   /* icon */
            int16_t id = (int16_t)rd16(it + 14);
            maddr ic = GetResource(FOURCC('I','C','O','N'), id);
            if (ic) {
                maddr bm = NewPtrClear(14);
                wr32(bm, rd32(ic)); wr16(bm + 4, 4); wr_rect(bm + 6, (QRect){0, 0, 32, 32});
                qd_copybits(bm, qd_port_bits(dlg) == dlg + GP_portBits ? dlg + GP_portBits : rd32(rd32(dlg + GP_portPixMap)), (QRect){0,0,32,32}, r, 1, 0, idx_black(), idx_white(), NULL);
                DisposePtr(bm);
            }
        }
    }
    qd_set_port(save);
}

static maddr new_dialog(maddr storage, QRect bounds, const char *title, int vis, int16_t proc, maddr behind, int goaway, uint32_t refcon, maddr items) {
    maddr d = storage ? storage : NewPtrClear(D_SIZE);
    new_window(d, bounds, title, 0, proc, behind, goaway, refcon, 1);
    wr16(d + W_kind, 2);   /* dialogKind */
    wr32(d + D_items, items);
    wr16(d + D_editField, (uint16_t)-1);
    wr16(d + D_defItem, 1);
    wr16(d + GP_txFont, 0); wr16(d + GP_txSize, 12);
    /* instantiate items */
    int n = ditl_count(d);
    for (int i = 1; i <= n; i++) {
        int type; QRect r; maddr it = ditl_item(d, i, &type, &r);
        int t = type & 0x7F;
        int len = rd8(it + 13);
        if ((t & 0x7C) == 4 && t != 7) {
            char title2[256]; int l = len; memcpy(title2, mptr(it + 14), (size_t)l); title2[l] = 0;
            wr32(it, new_control(d, r, title2, 1, 0, 0, 1, (int16_t)(t - 4), 0));
        } else if (t == 8 || t == 16) {
            maddr h = NewHandle((uint32_t)len); mcopy(rd32(h), it + 14, (uint32_t)len); wr32(it, h);
            if (t == 16 && rds16(d + D_editField) < 0) wr16(d + D_editField, (uint16_t)(i - 1));
        }
    }
    if (vis) show_hide(d, 1);
    return d;
}
static maddr get_new_dialog(int16_t id, maddr storage, maddr behind) {
    maddr r = GetResource(FOURCC('D','L','O','G'), id);
    if (!r) return 0;
    maddr p = rd32(r);
    int16_t ditl = rds16(p + 18);
    maddr items = GetResource(FOURCC('D','I','T','L'), ditl);
    if (!items) return 0;
    maddr copy = NewHandle(GetHandleSize(items)); mcopy(rd32(copy), rd32(items), GetHandleSize(items));
    char t[256]; pstr_to_c(p + 20, t, sizeof t);
    QRect b = rd_rect(p);
    /* center like the system would for the standard alert position */
    return new_dialog(storage, b, t, rd8(p + 10), rds16(p + 8), behind, rd8(p + 12), rd32(p + 14), copy);
}
static void T_InitDialogs(uint16_t w) { (void)w; pop32(); for (int i = 0; i < 4; i++) if (!param_text[i]) param_text[i] = NewHandleClear(256); }
static void T_GetNewDialog(uint16_t w) { (void)w; maddr behind = pop32(), storage = pop32(); int16_t id = pops16(); maddr d = get_new_dialog(id, storage, behind); if (d && rd8(d + W_visible)) draw_dialog_items(d); ret32(d); }
static void T_NewDialog(uint16_t w) {
    (void)w; maddr items = pop32(); uint32_t refcon = pop32(); int goaway = pop8(); maddr behind = pop32(); int16_t proc = pops16();
    int vis = pop8(); maddr title = pop32(); maddr br = pop32(); maddr storage = pop32();
    char t[256]; pstr_to_c(title, t, sizeof t);
    ret32(new_dialog(storage, rd_rect(br), t, vis, proc, behind, goaway, refcon, items));
}
static void T_DisposeDialog(uint16_t w) { (void)w; close_window(pop32()); }
static void T_CloseDialog(uint16_t w) { (void)w; close_window(pop32()); }
static void T_DrawDialog(uint16_t w) { (void)w; maddr d = pop32(); draw_dialog_items(d); }
static void T_UpdateDialog(uint16_t w) { (void)w; pop32(); maddr d = pop32(); draw_dialog_items(d); }
static void T_ParamText(uint16_t w) {
    (void)w; maddr s[4]; for (int i = 3; i >= 0; i--) s[i] = pop32();
    for (int i = 0; i < 4; i++) { if (!param_text[i]) param_text[i] = NewHandleClear(256); if (s[i]) mcopy(rd32(param_text[i]), s[i], rd8(s[i]) + 1u); else wr8(rd32(param_text[i]), 0); }
}
/* GetDialogItem(dlg, item, &type, &handle, &rect) */
static void T_GetDialogItem(uint16_t w) {
    (void)w; maddr box = pop32(), hp = pop32(), tp = pop32(); int16_t item = pops16(); maddr d = pop32();
    int type; QRect r; maddr it = ditl_item(d, item, &type, &r);
    if (!it) return;
    if (tp) wr16(tp, (uint16_t)type);
    if (hp) wr32(hp, rd32(it));
    if (box) wr_rect(box, r);
}
static void T_SetDialogItem(uint16_t w) {
    (void)w; maddr box = pop32(), h = pop32(); int16_t type = pops16(), item = pops16(); maddr d = pop32();
    maddr it = ditl_item(d, item, NULL, NULL);
    if (!it) return;
    wr32(it, h); wr_rect(it + 4, rd_rect(box)); wr8(it + 12, (uint8_t)type);
}
static void T_SetDialogItemText(uint16_t w) {
    (void)w; maddr s = pop32(), h = pop32();
    int n = rd8(s); SetHandleSize(h, (uint32_t)n); mcopy(rd32(h), s + 1, (uint32_t)n);
}
static void T_GetDialogItemText(uint16_t w) {
    (void)w; maddr s = pop32(), h = pop32();
    uint32_t n = GetHandleSize(h); if (n > 255) n = 255;
    wr8(s, (uint8_t)n); mcopy(s + 1, rd32(h), n);
}
static void T_SelectDialogItemText(uint16_t w) { (void)w; pop16(); pop16(); int16_t item = pops16(); maddr d = pop32(); wr16(d + D_editField, (uint16_t)(item - 1)); }
static void T_HideDialogItem(uint16_t w) { (void)w; pop16(); pop32(); }
static void T_ShowDialogItem(uint16_t w) { (void)w; pop16(); pop32(); }
static void T_FindDialogItem(uint16_t w) {
    (void)w; QPoint p = pt_from32(pop32()); maddr d = pop32();
    for (int i = 1; i <= ditl_count(d); i++) { QRect r; ditl_item(d, i, NULL, &r); if (p.v >= r.top && p.v < r.bottom && p.h >= r.left && p.h < r.right) { ret16((uint16_t)(i - 1)); return; } }
    ret16((uint16_t)-1);
}
static void T_IsDialogEvent(uint16_t w) { (void)w; maddr e = pop32(); (void)e; maddr f = window_list(); retbool(f && rd16(f + W_kind) == 2); }
static void T_DialogSelect(uint16_t w) { (void)w; pop32(); pop32(); pop32(); retbool(0); }
static void T_ErrorSound(uint16_t w) { (void)w; pop32(); }
static void T_CouldDialog(uint16_t w) { (void)w; pop16(); }
static void T_FreeDialog(uint16_t w) { (void)w; pop16(); }
static void T_SetDAFont(uint16_t w) { (void)w; wr16(LM_DlgFont, pop16()); }

/* Edit text: append/backspace on the current edit field */
static void edit_key(maddr d, uint8_t ch) {
    int ef = rds16(d + D_editField);
    if (ef < 0) return;
    int type; QRect r; maddr it = ditl_item(d, ef + 1, &type, &r);
    maddr h = rd32(it);
    uint32_t n = GetHandleSize(h);
    if (ch == 8) { if (n) SetHandleSize(h, n - 1); }
    else if (ch >= 32 && n < 255) { SetHandleSize(h, n + 1); wr8(rd32(h) + n, ch); }
    draw_dialog_items(d);
}

/* Modal loop: returns item hit. filter: 68k ModalFilterProc or 0 */
/* a modal dialog is waiting for an answer (the controller then answers with Return / Esc) */
static int modal_depth;
int wm_modal_active(void) { return modal_depth > 0; }

static int modal_loop(maddr d, maddr filter) {
    modal_depth++;
    maddr evt = NewPtrClear(16);
    maddr itemp = NewPtrClear(2);
    draw_dialog_items(d);
    int result = 0;
    while (cpu_running && !result) {
        runtime_idle();
        int got = event_get(0xFFFF, evt, 1);
        if (!got) { platform_wait_ms(5); wr16(evt, 0); }
        if (filter) {
            /* FUNCTION filter(theDialog, VAR theEvent, VAR itemHit): BOOLEAN */
            uint32_t sp = cpu_get(R_SP);
            push16(0); push32(d); push32(evt); push32(itemp);
            cpu_call(filter, NULL);
            int handled = rd16(cpu_get(R_SP)) >> 8;
            cpu_set(R_SP, sp);
            if (handled) { result = rds16(itemp); break; }
        }
        uint16_t what = rd16(evt);
        if (what == 1) {           /* mouseDown */
            QPoint g = rd_pt(evt + 10);
            QRect b = rd_rect(qd_port_bits(d) + 6);
            QPoint lp = { (int16_t)(g.v + b.top), (int16_t)(g.h + b.left) };
            for (int i = 1; i <= ditl_count(d); i++) {
                int type; QRect r; ditl_item(d, i, &type, &r);
                if (type & 0x80) continue;
                if (lp.v >= r.top && lp.v < r.bottom && lp.h >= r.left && lp.h < r.right) { result = i; break; }
            }
        } else if (what == 3 || what == 5) {   /* keyDown / autoKey */
            uint8_t ch = (uint8_t)rd32(evt + 2);
            if (ch == 13 || ch == 3) result = rds16(d + D_defItem) > 0 ? rds16(d + D_defItem) : 1;
            else if (ch == 27) result = 2 <= ditl_count(d) ? 2 : 1;
            else edit_key(d, ch);
        } else if (what == 6) {
            draw_dialog_items(d);
        }
    }
    DisposePtr(evt); DisposePtr(itemp);
    modal_depth--;
    return result;
}
static void T_ModalDialog(uint16_t w) {
    (void)w; maddr itemp = pop32(), filter = pop32();
    maddr d = window_list();
    while (d && rd16(d + W_kind) != 2) d = rd32(d + W_next);
    if (!d) { wr16(itemp, 1); return; }
    wr16(itemp, (uint16_t)modal_loop(d, filter));
}

static void run_alert(int16_t id, int icon) {
    maddr r = GetResource(FOURCC('A','L','R','T'), id);
    if (!r) { ret16(1); return; }
    maddr p = rd32(r);
    QRect b = rd_rect(p);
    int16_t ditl = rds16(p + 8);
    maddr items = GetResource(FOURCC('D','I','T','L'), ditl);
    if (!items) { ret16(1); return; }
    maddr copy = NewHandle(GetHandleSize(items)); mcopy(rd32(copy), rd32(items), GetHandleSize(items));
    /* center horizontally, upper third vertically */
    int wd = b.right - b.left, ht = b.bottom - b.top;
    b.left = (int16_t)((qd_screen_w - wd) / 2); b.right = (int16_t)(b.left + wd);
    b.top = (int16_t)((qd_screen_h - ht) / 3); b.bottom = (int16_t)(b.top + ht);
    maddr d = new_dialog(0, b, "", 1, 1, 0xFFFFFFFF, 0, 0, copy);
    (void)icon;
    char msg[256] = ""; if (param_text[0] && rd32(param_text[0])) pstr_to_c(rd32(param_text[0]), msg, sizeof msg);
    fprintf(stderr, "[alert %d] %s\n", id, msg);
    int item;
    extern int platform_is_headless(void); extern int runtime_deterministic;
    if (!platform_is_headless() && !runtime_deterministic) {
        /* in the window: a native alert (the game's own text, its buttons) */
        char text[600] = "", btn[8][64]; const char *bp[8]; int bitem[8], nb = 0;
        for (int i = 1; i <= ditl_count(d); i++) {
            int type; maddr it = ditl_item(d, i, &type, NULL);
            int t = type & 0x7F, l = rd8(it + 13);
            uint8_t raw[256], ex[256]; int en;
            for (int j = 0; j < l; j++) raw[j] = rd8(it + 14 + j);
            expand_params(raw, l, ex, &en); ex[en] = 0;
            for (int j = 0; j < en; j++) if (ex[j] == '\r') ex[j] = '\n';
            if (t == 8 && strlen(text) + en + 2 < sizeof text) { if (*text) strcat(text, "\n"); strcat(text, (char *)ex); }
            else if ((t & 0x7C) == 4 && nb < 8) { snprintf(btn[nb], sizeof btn[nb], "%s", (char *)ex); bp[nb] = btn[nb]; bitem[nb++] = i; }
        }
        int macos_game_alert(int kind, const char *text, const char *const *buttons, int n);
        extern void event_release_all(void);
        int k = macos_game_alert(icon, text, bp, nb);   /* -1: no native alerts (Linux) */
        event_release_all();
        item = k < 0 ? modal_loop(d, 0) : nb ? bitem[k] : 1;
    } else item = modal_loop(d, 0);
    close_window(d);
    ret16((uint16_t)item);
}
static void T_Alert(uint16_t w) { (void)w; pop32(); run_alert(pops16(), -1); }
static void T_StopAlert(uint16_t w) { (void)w; pop32(); run_alert(pops16(), 0); }
static void T_NoteAlert(uint16_t w) { (void)w; pop32(); run_alert(pops16(), 1); }
static void T_CautionAlert(uint16_t w) { (void)w; pop32(); run_alert(pops16(), 2); }
static void T_DialogDispatch(uint16_t w) {
    (void)w;
    switch (D0 & 0xFF) {
        case 0x03: { maddr p = pop32(); wr32(p, dlg_filter_std); ret16(0); break; }
        case 0x04: { int16_t i = pops16(); maddr d = pop32(); wr16(d + D_defItem, (uint16_t)i); ret16(0); break; }
        case 0x05: case 0x06: pop16(); pop32(); ret16(0); break;
        default: fprintf(stderr, "[dlg] DialogDispatch %x\n", D0); cpu_set(R_SP, cpu_get(R_SP) + ((D0 >> 8) & 0xFF) * 2); ret16(0);
    }
}

/* ========================================================= TextEdit == */
/* Minimal TERec: destRect 0, viewRect 8, selStart 32, selEnd 34, active 36, teLength 60, hText 62, inPort 82 */
static void T_TEInit(uint16_t w) { (void)w; }
static void T_TENew(uint16_t w) {
    (void)w; maddr view = pop32(), dest = pop32();
    maddr h = NewHandleClear(128);
    maddr t = rd32(h);
    mcopy(t, dest, 8); mcopy(t + 8, view, 8);
    wr16(t + 24, 16); wr16(t + 26, 12);
    wr32(t + 62, NewHandleClear(0));
    wr32(t + 82, qd_the_port());
    wr16(t + 74, rd16(qd_the_port() + GP_txFont)); wr16(t + 80, rd16(qd_the_port() + GP_txSize));
    ret32(h);
}
static maddr te(maddr h) { return rd32(h); }
static void te_draw(maddr h) {
    maddr t = te(h);
    maddr port = rd32(t + 82);
    maddr save = qd_the_port(); qd_set_port(port);
    QRect r = rd_rect(t + 8);
    Surface s = qd_port_surface(port); RgnList clip; qd_port_clip(port, &clip);
    qd_fill_rect_index(&s, &clip, r, qd_port_bk_index(port));
    rgn_free(&clip);
    maddr txt = rd32(t + 62); int n = rd16(t + 60);
    uint8_t buf[1024]; if (n > 1024) n = 1024; mget(buf, rd32(txt), (uint32_t)n);
    draw_text_box(port, rd_rect(t), buf, n);
    qd_set_port(save);
}
static void T_TEDispose(uint16_t w) { (void)w; maddr h = pop32(); DisposeHandle(rd32(te(h) + 62)); DisposeHandle(h); }
static void T_TESetText(uint16_t w) {
    (void)w; maddr h = pop32(); int32_t len = (int32_t)pop32(); maddr text = pop32();
    maddr t = te(h); maddr th = rd32(t + 62);
    SetHandleSize(th, (uint32_t)len); mcopy(rd32(th), text, (uint32_t)len);
    t = te(h); wr16(t + 60, (uint16_t)len); wr16(t + 32, (uint16_t)len); wr16(t + 34, (uint16_t)len);
}
static void T_TEGetText(uint16_t w) { (void)w; ret32(rd32(te(pop32()) + 62)); }
static void T_TEUpdate(uint16_t w) { (void)w; maddr h = pop32(); pop32(); te_draw(h); }
static void T_TECalText(uint16_t w) { (void)w; pop32(); }
static void T_TESetSelect(uint16_t w) { (void)w; maddr h = pop32(); int32_t e = (int32_t)pop32(), s = (int32_t)pop32(); maddr t = te(h); int n = rd16(t + 60); if (s > n) s = n; if (e > n) e = n; wr16(t + 32, (uint16_t)s); wr16(t + 34, (uint16_t)e); }
static void T_TEActivate(uint16_t w) { (void)w; wr16(te(pop32()) + 36, 1); }
static void T_TEDeactivate(uint16_t w) { (void)w; wr16(te(pop32()) + 36, 0); }
static void T_TEIdle(uint16_t w) { (void)w; pop32(); }
static void T_TEClick(uint16_t w) { (void)w; pop32(); pop16(); pop32(); }
static void te_replace_sel(maddr h, const uint8_t *ins, int n) {
    maddr t = te(h); maddr th = rd32(t + 62);
    int len = rd16(t + 60), s = rd16(t + 32), e = rd16(t + 34);
    if (s > e) { int x = s; s = e; e = x; }
    uint8_t *buf = malloc((size_t)len + n + 1); mget(buf, rd32(th), (uint32_t)len);
    uint8_t *out = malloc((size_t)len + n + 1);
    memcpy(out, buf, (size_t)s); memcpy(out + s, ins, (size_t)n); memcpy(out + s + n, buf + e, (size_t)(len - e));
    int nl = len - (e - s) + n;
    SetHandleSize(th, (uint32_t)nl); mput(rd32(th), out, (uint32_t)nl);
    t = te(h); wr16(t + 60, (uint16_t)nl); wr16(t + 32, (uint16_t)(s + n)); wr16(t + 34, (uint16_t)(s + n));
    free(buf); free(out);
}
static void T_TEKey(uint16_t w) {
    (void)w; maddr h = pop32(); uint8_t ch = (uint8_t)pop16();
    maddr t = te(h);
    if (ch == 8) {
        int s = rd16(t + 32), e = rd16(t + 34);
        if (s == e && s > 0) wr16(t + 32, (uint16_t)(s - 1));
        te_replace_sel(h, NULL, 0);
    } else if (ch == 28) { int s = rd16(t + 32); if (s > 0) { wr16(t + 32, (uint16_t)(s - 1)); wr16(t + 34, (uint16_t)(s - 1)); } }
    else if (ch == 29) { int s = rd16(t + 32); if (s < rd16(t + 60)) { wr16(t + 32, (uint16_t)(s + 1)); wr16(t + 34, (uint16_t)(s + 1)); } }
    else if (ch >= 32 || ch == 13) te_replace_sel(h, &ch, 1);
    te_draw(h);
}
static uint8_t scrap[4096]; static int scrap_len;
static void T_TECopy(uint16_t w) { (void)w; maddr h = pop32(); maddr t = te(h); int s = rd16(t + 32), e = rd16(t + 34); scrap_len = e - s; if (scrap_len < 0) scrap_len = 0; mget(scrap, rd32(rd32(t + 62)) + s, (uint32_t)scrap_len); }
static void T_TECut(uint16_t w) { (void)w; maddr h = pop32(); maddr t = te(h); int s = rd16(t + 32), e = rd16(t + 34); scrap_len = e - s; if (scrap_len < 0) scrap_len = 0; mget(scrap, rd32(rd32(t + 62)) + s, (uint32_t)scrap_len); te_replace_sel(h, NULL, 0); te_draw(h); }
static void T_TEPaste(uint16_t w) { (void)w; maddr h = pop32(); te_replace_sel(h, scrap, scrap_len); te_draw(h); }
static void T_TEDelete(uint16_t w) { (void)w; maddr h = pop32(); te_replace_sel(h, NULL, 0); te_draw(h); }
static void T_TEInsert(uint16_t w) { (void)w; maddr h = pop32(); int32_t len = (int32_t)pop32(); maddr txt = pop32(); uint8_t *b = malloc((size_t)len + 1); mget(b, txt, (uint32_t)len); te_replace_sel(h, b, len); free(b); te_draw(h); }
static void T_TETextBox(uint16_t w) {
    (void)w; pop16(); maddr box = pop32(); int32_t len = (int32_t)pop32(); maddr txt = pop32();
    uint8_t *b = malloc((size_t)len + 1); mget(b, txt, (uint32_t)len);
    draw_text_box(qd_the_port(), rd_rect(box), b, len); free(b);
}

/* ============================================================ Scrap == */
static void T_ZeroScrap(uint16_t w) { (void)w; scrap_len = 0; ret32(0); }
static void T_PutScrap(uint16_t w) { (void)w; maddr src = pop32(); pop32(); int32_t len = (int32_t)pop32(); if (len > 4096) len = 4096; mget(scrap, src, (uint32_t)len); scrap_len = len; ret32(0); }
static void T_GetScrap(uint16_t w) { (void)w; pop32(); pop32(); maddr h = pop32(); if (h) { SetHandleSize(h, (uint32_t)scrap_len); mput(rd32(h), scrap, (uint32_t)scrap_len); } ret32((uint32_t)scrap_len); }
static void T_UnloadScrap(uint16_t w) { (void)w; ret32(0); }
static void T_LoadScrap(uint16_t w) { (void)w; ret32(0); }
static void T_InfoScrap(uint16_t w) { (void)w; ret32(LM_ScrapSize); }

/* ======================================================= misc stubs == */
static void T_PrGlue(uint16_t w) {
    (void)w;
    /* PrGlue: selector on stack (long), only PrOpen/PrError style calls are expected */
    uint32_t sel = pop32();
    int args = (sel >> 16) & 0xFF;   /* parameter bytes encoded in the selector? best effort */
    (void)args;
    fprintf(stderr, "[print] PrGlue %08x (printing is not supported)\n", sel);
}
static void T_SystemTask(uint16_t w) { (void)w; }
static void T_SystemEvent(uint16_t w) { (void)w; pop32(); retbool(0); }
static void T_SystemClick(uint16_t w) { (void)w; pop32(); pop32(); }
static void T_SystemEdit(uint16_t w) { (void)w; pop16(); retbool(0); }
static void T_OpenDeskAcc(uint16_t w) { (void)w; pop32(); ret16(0); }
static void T_SysBeep(uint16_t w) { (void)w; pop16(); fprintf(stderr, "\a"); }

/* Standard File (Pack3): only used for saving/restoring games. We pick files
 * in the game folder without a dialog: SFPutFile uses the default name; SFGetFile
 * returns the most recent matching file. */
static void T_Pack3(uint16_t w) {
    (void)w;
    uint16_t sel = pop16();
    extern void stdfile_dispatch(uint16_t sel);
    stdfile_dispatch(sel);
}

void wm_trap_misc_register(void) {
    trap_register(TRAP_SystemTask, T_SystemTask);
    trap_register(TRAP_SystemEvent, T_SystemEvent);
    trap_register(TRAP_SystemClick, T_SystemClick);
    trap_register(TRAP_SystemEdit, T_SystemEdit);
    trap_register(TRAP_OpenDeskAcc, T_OpenDeskAcc);
    trap_register(TRAP_SysBeep, T_SysBeep);
    trap_register(TRAP_Pack3, T_Pack3);
}

void traps_wm_init(void) {
    dlg_filter_std = 0;
    trap_register(TRAP_InitWindows, T_InitWindows);
    trap_register(TRAP_GetWMgrPort, T_GetWMgrPort);
    trap_register(TRAP_GetCWMgrPort, T_GetCWMgrPort);
    trap_register(TRAP_NewWindow, T_NewWindow);
    trap_register(TRAP_NewCWindow, T_NewWindow);
    trap_register(TRAP_GetNewWindow, T_GetNewWindow);
    trap_register(TRAP_GetNewCWindow, T_GetNewWindow);
    trap_register(TRAP_CloseWindow, T_CloseWindow);
    trap_register(TRAP_DisposeWindow, T_DisposeWindow);
    trap_register(TRAP_ShowWindow, T_ShowWindow);
    trap_register(TRAP_HideWindow, T_HideWindow);
    trap_register(TRAP_ShowHide, T_ShowHide);
    trap_register(TRAP_SelectWindow, T_SelectWindow);
    trap_register(TRAP_BringToFront, T_BringToFront);
    trap_register(TRAP_SendBehind, T_SendBehind);
    trap_register(TRAP_FrontWindow, T_FrontWindow);
    trap_register(TRAP_HiliteWindow, T_HiliteWindow);
    trap_register(TRAP_MoveWindow, T_MoveWindow);
    trap_register(TRAP_SizeWindow, T_SizeWindow);
    trap_register(TRAP_DragWindow, T_DragWindow);
    trap_register(TRAP_GrowWindow, T_GrowWindow);
    trap_register(TRAP_TrackBox, T_TrackBox);
    trap_register(TRAP_TrackGoAway, T_TrackGoAway);
    trap_register(TRAP_ZoomWindow, T_ZoomWindow);
    trap_register(TRAP_DrawGrowIcon, T_DrawGrowIcon);
    trap_register(TRAP_SetWTitle, T_SetWTitle);
    trap_register(TRAP_GetWTitle, T_GetWTitle);
    trap_register(TRAP_SetWRefCon, T_SetWRefCon);
    trap_register(TRAP_GetWRefCon, T_GetWRefCon);
    trap_register(TRAP_SetWindowPic, T_SetWindowPic);
    trap_register(TRAP_FindWindow, T_FindWindow);
    trap_register(TRAP_BeginUpdate, T_BeginUpdate);
    trap_register(TRAP_EndUpdate, T_EndUpdate);
    trap_register(TRAP_InvalRect, T_InvalRect);
    trap_register(TRAP_InvalRgn, T_InvalRgn);
    trap_register(TRAP_ValidRect, T_ValidRect);
    trap_register(TRAP_ValidRgn, T_ValidRgn);
    trap_register(TRAP_CalcVis, T_CalcVis);
    trap_register(TRAP_CalcVisBehind, T_CalcVisBehind);
    trap_register(TRAP_PaintOne, T_PaintOne);
    trap_register(TRAP_PaintBehind, T_PaintBehind);
    trap_register(TRAP_ClipAbove, T_ClipAbove);
    trap_register(TRAP_SaveOld, T_SaveOld);
    trap_register(TRAP_DrawNew, T_DrawNew);
    trap_register(TRAP_CheckUpdate, T_CheckUpdate);
    trap_register(TRAP_InitMenus, T_InitMenus);
    trap_register(TRAP_InsertMenu, T_InsertMenu);
    trap_register(TRAP_DeleteMenu, T_DeleteMenu);
    trap_register(TRAP_GetMenuHandle, T_GetMenuHandle);
    trap_register(TRAP_GetMenu, T_GetMenu);
    trap_register(TRAP_GetNewMBar, T_GetNewMBar);
    trap_register(TRAP_SetMenuBar, T_SetMenuBar);
    trap_register(TRAP_GetMenuBar, T_GetMenuBar);
    trap_register(TRAP_ClearMenuBar, T_ClearMenuBar);
    trap_register(TRAP_DrawMenuBar, T_DrawMenuBar);
    trap_register(TRAP_HiliteMenu, T_HiliteMenu);
    trap_register(TRAP_FlashMenuBar, T_FlashMenuBar);
    trap_register(TRAP_NewMenu, T_NewMenu);
    trap_register(TRAP_AppendMenu, T_AppendMenu);
    trap_register(TRAP_InsertMenuItem, T_InsertMenuItem);
    trap_register(TRAP_AppendResMenu, T_AppendResMenu);
    trap_register(TRAP_InsertResMenu, T_InsertResMenu);
    trap_register(TRAP_CountMItems, T_CountMItems);
    trap_register(TRAP_EnableItem, T_EnableItem);
    trap_register(TRAP_DisableItem, T_DisableItem);
    trap_register(TRAP_CheckItem, T_CheckItem);
    trap_register(TRAP_SetItemMark, T_SetItemMark);
    trap_register(TRAP_GetItemMark, T_GetItemMark);
    trap_register(TRAP_SetItemCmd, T_SetItemCmd);
    trap_register(TRAP_GetItemCmd, T_GetItemCmd);
    trap_register(TRAP_SetItemStyle, T_SetItemStyle);
    trap_register(TRAP_GetMenuItemText, T_GetMenuItemText);
    trap_register(TRAP_SetMenuItemText, T_SetMenuItemText);
    trap_register(TRAP_MenuKey, T_MenuKey);
    trap_register(TRAP_MenuSelect, T_MenuSelect);
    trap_register(TRAP_PopUpMenuSelect, T_PopUpMenuSelect);
    trap_register(TRAP_CalcMenuSize, T_CalcMenuSize);
    trap_register(TRAP_SetMenuFlash, T_SetMenuFlash);
    trap_register(TRAP_DisposeMenu, T_DisposeMenu);
    trap_register(TRAP_GetControlValue, T_GetControlValue);
    trap_register(TRAP_SetControlValue, T_SetControlValue);
    trap_register(TRAP_GetControlMinimum, T_GetControlMinimum);
    trap_register(TRAP_GetControlMaximum, T_GetControlMaximum);
    trap_register(TRAP_SetControlMinimum, T_SetControlMinimum);
    trap_register(TRAP_SetControlMaximum, T_SetControlMaximum);
    trap_register(TRAP_HiliteControl, T_HiliteControl);
    trap_register(TRAP_DrawControls, T_DrawControls);
    trap_register(TRAP_SetControlTitle, T_SetControlTitle);
    trap_register(TRAP_GetControlTitle, T_GetControlTitle);
    trap_register(TRAP_InitDialogs, T_InitDialogs);
    trap_register(TRAP_GetNewDialog, T_GetNewDialog);
    trap_register(TRAP_NewDialog, T_NewDialog);
    trap_register(TRAP_DisposeDialog, T_DisposeDialog);
    trap_register(TRAP_CloseDialog, T_CloseDialog);
    trap_register(TRAP_DrawDialog, T_DrawDialog);
    trap_register(TRAP_UpdateDialog, T_UpdateDialog);
    trap_register(TRAP_ParamText, T_ParamText);
    trap_register(TRAP_GetDialogItem, T_GetDialogItem);
    trap_register(TRAP_SetDialogItem, T_SetDialogItem);
    trap_register(TRAP_SetDialogItemText, T_SetDialogItemText);
    trap_register(TRAP_GetDialogItemText, T_GetDialogItemText);
    trap_register(TRAP_SelectDialogItemText, T_SelectDialogItemText);
    trap_register(TRAP_HideDialogItem, T_HideDialogItem);
    trap_register(TRAP_ShowDialogItem, T_ShowDialogItem);
    trap_register(TRAP_FindDialogItem, T_FindDialogItem);
    trap_register(TRAP_IsDialogEvent, T_IsDialogEvent);
    trap_register(TRAP_DialogSelect, T_DialogSelect);
    trap_register(TRAP_ErrorSound, T_ErrorSound);
    trap_register(TRAP_CouldDialog, T_CouldDialog);
    trap_register(TRAP_FreeDialog, T_FreeDialog);
    trap_register(TRAP_ModalDialog, T_ModalDialog);
    trap_register(TRAP_Alert, T_Alert);
    trap_register(TRAP_StopAlert, T_StopAlert);
    trap_register(TRAP_NoteAlert, T_NoteAlert);
    trap_register(TRAP_CautionAlert, T_CautionAlert);
    trap_register(TRAP_DialogDispatch, T_DialogDispatch);
    trap_register(TRAP_TEInit, T_TEInit);
    trap_register(TRAP_TENew, T_TENew);
    trap_register(TRAP_TEDispose, T_TEDispose);
    trap_register(TRAP_TESetText, T_TESetText);
    trap_register(TRAP_TEGetText, T_TEGetText);
    trap_register(TRAP_TEUpdate, T_TEUpdate);
    trap_register(TRAP_TECalText, T_TECalText);
    trap_register(TRAP_TESetSelect, T_TESetSelect);
    trap_register(TRAP_TEActivate, T_TEActivate);
    trap_register(TRAP_TEDeactivate, T_TEDeactivate);
    trap_register(TRAP_TEIdle, T_TEIdle);
    trap_register(TRAP_TEClick, T_TEClick);
    trap_register(TRAP_TEKey, T_TEKey);
    trap_register(TRAP_TECopy, T_TECopy);
    trap_register(TRAP_TECut, T_TECut);
    trap_register(TRAP_TEPaste, T_TEPaste);
    trap_register(TRAP_TEDelete, T_TEDelete);
    trap_register(TRAP_TEInsert, T_TEInsert);
    trap_register(TRAP_TETextBox, T_TETextBox);
    trap_register(TRAP_ZeroScrap, T_ZeroScrap);
    trap_register(TRAP_PutScrap, T_PutScrap);
    trap_register(TRAP_GetScrap, T_GetScrap);
    trap_register(TRAP_UnloadScrap, T_UnloadScrap);
    trap_register(TRAP_LoadScrap, T_LoadScrap);
    trap_register(TRAP_InfoScrap, T_InfoScrap);
    trap_register(TRAP_PrGlue, T_PrGlue);
    wm_trap_misc_register();
}
