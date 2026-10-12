/* Event Manager, keyboard map, mouse, cursor. */
#include "qd.h"
#include "trapnums.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void runtime_idle(void);
uint32_t runtime_ticks(void);
void platform_wait_ms(int ms);
void platform_set_cursor_visible(int v);
void platform_set_cursor(maddr cursor_record);
int  wm_pending_update(maddr *out);
maddr qd_port_bits(maddr port);

typedef struct { uint16_t what; uint32_t msg, when; QPoint where; uint16_t mods; } Event;
#define QN 64
static Event q[QN];
static int qhead, qtail;
static QPoint mouse;
static int button_down;
static uint16_t cur_mods;
static int cursor_level;        /* 0 = visible, <0 hidden */
static int obscured;

enum { nullEvent = 0, mouseDown = 1, mouseUp = 2, keyDown = 3, keyUp = 4, autoKey = 5, updateEvt = 6,
       diskEvt = 7, activateEvt = 8, osEvt = 15 };

uint16_t event_modifiers(void) { return cur_mods | (button_down ? 0 : 0x80); }

static void push(uint16_t what, uint32_t msg, uint16_t extra_mods) {
    int nt = (qtail + 1) % QN;
    if (nt == qhead) qhead = (qhead + 1) % QN;   /* drop oldest */
    q[qtail] = (Event){ what, msg, runtime_ticks(), mouse, (uint16_t)(event_modifiers() | extra_mods) };
    qtail = nt;
}
void event_post(uint16_t what, uint32_t msg) {
    if (what == activateEvt) {
        /* activate flag: first post for a window after select is an activate */
        maddr front = rd32(LM_WindowList);
        push(what, msg, (msg == front) ? 1 : 0);
        return;
    }
    push(what, msg, 0);
}

/* ---- called by the platform layer ---- */
void event_key(int down, uint8_t keycode, uint8_t ch, uint16_t mods, int repeat) {
    cur_mods = mods;
    maddr km = LM_KeyMap;
    /* KeyMap: big-endian bit order per byte: key k -> byte k/8, bit k%8 */
    if (keycode < 128) {
        uint8_t b = rd8(km + keycode / 8);
        uint8_t m = (uint8_t)(1 << (keycode % 8));
        wr8(km + keycode / 8, down ? (b | m) : (b & ~m));
    }
    if (down) {
        wr8(LM_KbdLast, keycode);
        push(repeat ? autoKey : keyDown, (uint32_t)keycode << 8 | ch, 0);
    } else push(keyUp, (uint32_t)keycode << 8 | ch, 0);
}
void event_modifiers_changed(uint16_t mods) {
    cur_mods = mods;
    /* modifier keys also show up in the KeyMap */
    static const struct { uint16_t bit; uint8_t key; } map[] = { {0x0100, 55}, {0x0200, 56}, {0x0400, 57}, {0x0800, 58}, {0x1000, 59} };
    for (unsigned i = 0; i < 5; i++) {
        uint8_t k = map[i].key, b = rd8(LM_KeyMap + k / 8), m = (uint8_t)(1 << (k % 8));
        wr8(LM_KeyMap + k / 8, (mods & map[i].bit) ? (b | m) : (b & ~m));
    }
}
void event_mouse_move(int h, int v) {
    mouse.h = (int16_t)h; mouse.v = (int16_t)v;
    wr_pt(LM_MouseLocation, mouse); wr_pt(LM_MTemp, mouse); wr_pt(0x082C, mouse);
    if (obscured) { obscured = 0; if (cursor_level >= 0) platform_set_cursor_visible(1); }
}
void event_mouse_button(int down) {
    button_down = down;
    push(down ? mouseDown : mouseUp, 0, 0);
}
int event_button(void) { return button_down; }

/* After a native modal dialog: the releases it swallowed never arrive, so forget every
 * held key and the mouse button. */
void event_release_all(void) {
    mset(LM_KeyMap, 0, 16);
    cur_mods = 0;
    button_down = 0;
}

/* Native (Cocoa) menus. A command with a key equivalent arrives as the Command-key
 * press the game already handles (MenuKey); one without arrives as a click in the menu
 * bar, which MenuSelect answers with the pending choice (see wm.c). */
void event_menu_command_key(uint8_t ch) {
    static const uint8_t ansi[128] = {   /* ASCII -> Mac virtual key code (US layout) */
        ['a']=0x00,['s']=0x01,['d']=0x02,['f']=0x03,['h']=0x04,['g']=0x05,['z']=0x06,['x']=0x07,['c']=0x08,['v']=0x09,
        ['b']=0x0B,['q']=0x0C,['w']=0x0D,['e']=0x0E,['r']=0x0F,['y']=0x10,['t']=0x11,['1']=0x12,['2']=0x13,['3']=0x14,
        ['4']=0x15,['6']=0x16,['5']=0x17,['=']=0x18,['9']=0x19,['7']=0x1A,['-']=0x1B,['8']=0x1C,['0']=0x1D,[']']=0x1E,
        ['o']=0x1F,['u']=0x20,['[']=0x21,['i']=0x22,['p']=0x23,['l']=0x25,['j']=0x26,['k']=0x28,[';']=0x29,[',']=0x2B,
        ['/']=0x2C,['n']=0x2D,['m']=0x2E,['.']=0x2F,[' ']=0x31 };
    uint8_t lc = (ch >= 'A' && ch <= 'Z') ? (uint8_t)(ch + 32) : ch;
    uint8_t key = lc < 128 ? ansi[lc] : 0;
    push(keyDown, (uint32_t)key << 8 | lc, 0x0100);
    push(keyUp, (uint32_t)key << 8 | lc, 0x0100);
}
void event_menu_click(void) {
    QPoint save = mouse;
    mouse.h = 10; mouse.v = 5;            /* in the menu bar */
    push(mouseDown, 0, 0);
    push(mouseUp, 0, 0);
    mouse = save;
}

/* ---- queue access ---- */
static int matches(uint16_t what, uint16_t mask) { return (mask >> what) & 1; }

static void write_event(maddr e, const Event *ev) {
    wr16(e, ev->what); wr32(e + 2, ev->msg); wr32(e + 6, ev->when); wr_pt(e + 10, ev->where); wr16(e + 14, ev->mods);
}

/* Returns 1 if an event was stored into e. Generates update events and null events like the Toolbox. */
int event_get(uint16_t mask, maddr e, int remove) {
    for (int i = qhead; i != qtail; i = (i + 1) % QN) {
        if (!matches(q[i].what, mask)) continue;
        Event ev = q[i];
        if (remove) {
            for (int j = i; j != qhead; j = (j - 1 + QN) % QN) q[j] = q[(j - 1 + QN) % QN];
            qhead = (qhead + 1) % QN;
        }
        write_event(e, &ev);
        return 1;
    }
    maddr w;
    if (matches(updateEvt, mask) && wm_pending_update(&w)) {
        Event ev = { updateEvt, w, runtime_ticks(), mouse, event_modifiers() };
        write_event(e, &ev);
        return 1;
    }
    Event ev = { nullEvent, 0, runtime_ticks(), mouse, event_modifiers() };
    write_event(e, &ev);
    return 0;
}

static void T_GetNextEvent(uint16_t w) {
    (void)w; maddr e = pop32(); uint16_t mask = pop16();
    runtime_idle();
    retbool(event_get(mask, e, 1));
}
static void T_WaitNextEvent(uint16_t w) {
    (void)w; pop32(); uint32_t sleep = pop32(); maddr e = pop32(); uint16_t mask = pop16();
    runtime_idle();
    int got = event_get(mask, e, 1);
    if (!got && sleep) {
        uint32_t until = runtime_ticks() + (sleep > 2 ? 2 : sleep);
        while (!got && runtime_ticks() < until && cpu_running) {
            platform_wait_ms(2);
            runtime_idle();
            got = event_get(mask, e, 1);
        }
    }
    retbool(got);
}
static void T_EventAvail(uint16_t w) { (void)w; maddr e = pop32(); uint16_t mask = pop16(); runtime_idle(); retbool(event_get(mask, e, 0)); }
static void T_GetOSEvent(uint16_t w) { (void)w; int got = event_get((uint16_t)D0, A0, 1); cpu_set(R_D0, got ? 0 : (uint32_t)-1); }
static void T_OSEventAvail(uint16_t w) { (void)w; int got = event_get((uint16_t)D0, A0, 0); cpu_set(R_D0, got ? 0 : (uint32_t)-1); }
/* FlushEvents: D0 low word = events to remove, high word = stop mask (stop at the
 * first event of one of these types). */
static void T_FlushEvents(uint16_t w) {
    (void)w; uint16_t mask = (uint16_t)D0, stop = (uint16_t)(D0 >> 16);
    Event tmp[QN]; int n = 0, stopped = 0;
    for (int i = qhead; i != qtail; i = (i + 1) % QN) {
        if (!stopped && matches(q[i].what, stop)) stopped = 1;
        if (!stopped && matches(q[i].what, mask)) continue;
        tmp[n++] = q[i];
    }
    memcpy(q, tmp, sizeof(Event) * (size_t)n);
    qhead = 0; qtail = n;
    cpu_set(R_D0, 0);
}
static void T_PostEvent(uint16_t w) { (void)w; push((uint16_t)A0, D0, 0); cpu_set(R_D0, 0); }
static void T_GetKeys(uint16_t w) { (void)w; maddr k = pop32(); runtime_idle(); mcopy(k, LM_KeyMap, 16); }
static void T_Button(uint16_t w) { (void)w; runtime_idle(); retbool(button_down); }
static void T_StillDown(uint16_t w) {
    (void)w; runtime_idle();
    int pending_up = 0;
    for (int i = qhead; i != qtail; i = (i + 1) % QN) if (q[i].what == mouseUp || q[i].what == mouseDown) pending_up = 1;
    retbool(button_down && !pending_up);
}
static void T_WaitMouseUp(uint16_t w) {
    (void)w; runtime_idle();
    for (int i = qhead; i != qtail; i = (i + 1) % QN) if (q[i].what == mouseUp) {
        for (int j = i; j != qhead; j = (j - 1 + QN) % QN) q[j] = q[(j - 1 + QN) % QN];
        qhead = (qhead + 1) % QN;
        retbool(0); return;
    }
    retbool(button_down);
}
static void T_GetMouse(uint16_t w) {
    (void)w; maddr p = pop32(); runtime_idle();
    maddr port = qd_the_port();
    QPoint l = mouse;
    if (port) { QRect b = rd_rect(qd_port_bits(port) + 6); l.v += b.top; l.h += b.left; }
    wr_pt(p, l);
}
static void T_GetDblTime(uint16_t w) { (void)w; ret32(30); }
static void T_GetCaretTime(uint16_t w) { (void)w; ret32(32); }
static void T_KeyTranslate(uint16_t w) {
    (void)w; maddr state = pop32(); uint16_t code = pop16(); pop32();
    (void)state;
    ret32(code & 0xFF);   /* platform already delivers the character in the event */
}

/* ---- cursor ---- */
static void T_InitCursor(uint16_t w) { (void)w; cursor_level = 0; obscured = 0; platform_set_cursor(0); platform_set_cursor_visible(1); }
static void T_SetCursor(uint16_t w) { (void)w; platform_set_cursor(pop32()); }
static void T_HideCursor(uint16_t w) { (void)w; cursor_level--; platform_set_cursor_visible(0); }
static void T_ShowCursor(uint16_t w) { (void)w; if (cursor_level < 0) cursor_level++; if (cursor_level >= 0) { obscured = 0; platform_set_cursor_visible(1); } }
static void T_ObscureCursor(uint16_t w) { (void)w; if (cursor_level >= 0) { obscured = 1; platform_set_cursor_visible(0); } }
static void T_ShieldCursor(uint16_t w) { (void)w; pop32(); pop32(); }
static void T_GetCursor(uint16_t w) { (void)w; int16_t id = pops16(); ret32(GetResource(FOURCC('C','U','R','S'), id)); }

void traps_event_init(void) {
    trap_register(TRAP_GetNextEvent, T_GetNextEvent);
    trap_register(TRAP_WaitNextEvent, T_WaitNextEvent);
    trap_register(TRAP_EventAvail, T_EventAvail);
    trap_register(TRAP_GetOSEvent, T_GetOSEvent);
    trap_register(TRAP_OSEventAvail, T_OSEventAvail);
    trap_register(TRAP_FlushEvents, T_FlushEvents);
    trap_register(0xA02F, T_PostEvent);
    trap_register(TRAP_GetKeys, T_GetKeys);
    trap_register(TRAP_Button, T_Button);
    trap_register(TRAP_StillDown, T_StillDown);
    trap_register(TRAP_WaitMouseUp, T_WaitMouseUp);
    trap_register(TRAP_GetMouse, T_GetMouse);
    trap_register(TRAP_KeyTranslate, T_KeyTranslate);
    trap_register(TRAP_InitCursor, T_InitCursor);
    trap_register(TRAP_SetCursor, T_SetCursor);
    trap_register(TRAP_HideCursor, T_HideCursor);
    trap_register(TRAP_ShowCursor, T_ShowCursor);
    trap_register(TRAP_ObscureCursor, T_ObscureCursor);
    trap_register(TRAP_ShieldCursor, T_ShieldCursor);
    trap_register(TRAP_GetCursor, T_GetCursor);
}

/* ---- scripted input for automated testing: POP2_INPUT="ms:key[:hold],..." ----
 * keys: a-z 0-9 space return esc tab left right up down, prefix "cmd-"/"shift-"/"opt-". */
typedef struct { uint64_t t; uint8_t code, ch; uint16_t mods; int down; } ScriptEv;
static ScriptEv sev[512]; static int sev_n, sev_i, sev_init;
static int keyinfo(const char *k, uint8_t *code, uint8_t *ch) {
    static const char *letters = "asdfhgzxcv?bqweryt123465=97-80]ou[ip?lj'k;\\,/nm.";
    if (strlen(k) == 1) {
        char c = k[0];
        for (int i = 0; letters[i]; i++) if (letters[i] == c && c != '?') { *code = (uint8_t)i; *ch = (uint8_t)c; return 1; }
        if (c == '0') { *code = 0x1D; *ch = '0'; return 1; }
    }
    static const struct { const char *n; uint8_t code, ch; } named[] = {
        {"space", 0x31, ' '}, {"return", 0x24, 13}, {"esc", 0x35, 27}, {"tab", 0x30, 9},
        {"left", 0x7B, 0x1C}, {"right", 0x7C, 0x1D}, {"up", 0x7E, 0x1E}, {"down", 0x7D, 0x1F},
        {"shiftkey", 0x38, 0}, {"enter", 0x4C, 3} };
    for (unsigned i = 0; i < sizeof named / sizeof named[0]; i++) if (!strcmp(k, named[i].n)) { *code = named[i].code; *ch = named[i].ch; return 1; }
    return 0;
}
static int cmp_sev(const void *a, const void *b) { const ScriptEv *x = a, *y = b; return x->t < y->t ? -1 : x->t > y->t; }
void event_script_tick(uint64_t now_ms) {
    if (!sev_init) {
        sev_init = 1;
        const char *s = getenv("POP2_INPUT");
        if (!s) return;
        char buf[4096]; strncpy(buf, s, sizeof buf - 1); buf[sizeof buf - 1] = 0;
        for (char *tok = strtok(buf, ","); tok && sev_n < 500; tok = strtok(NULL, ",")) {
            char key[64] = ""; unsigned long t = 0, hold = 100;
            if (sscanf(tok, "%lu:%63[^:]:%lu", &t, key, &hold) < 2) continue;
            uint16_t mods = 0; char *k = key;
            for (;;) {
                if (!strncmp(k, "cmd-", 4)) { mods |= 0x0100; k += 4; }
                else if (!strncmp(k, "shift-", 6)) { mods |= 0x0200; k += 6; }
                else if (!strncmp(k, "opt-", 4)) { mods |= 0x0800; k += 4; }
                else break;
            }
            uint8_t code, ch;
            if (!keyinfo(k, &code, &ch)) { fprintf(stderr, "[input] unknown key %s\n", k); continue; }
            sev[sev_n++] = (ScriptEv){ t, code, ch, mods, 1 };
            sev[sev_n++] = (ScriptEv){ t + hold, code, ch, mods, 0 };
        }
        qsort(sev, (size_t)sev_n, sizeof sev[0], cmp_sev);
    }
    while (sev_i < sev_n && sev[sev_i].t <= now_ms) {
        ScriptEv *e = &sev[sev_i++];
        event_modifiers_changed(e->down ? e->mods : 0);
        if (e->code == 0x38) {   /* bare shift */
            event_modifiers_changed(e->down ? 0x0200 : 0);
            continue;
        }
        event_key(e->down, e->code, e->ch, e->down ? e->mods : 0, 0);
        fprintf(stderr, "[input] %llu ms key %02x %s\n", (unsigned long long)now_ms, e->code, e->down ? "down" : "up");
    }
}
