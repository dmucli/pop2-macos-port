/* Input: remappable keys and controller buttons (Extras > Controls...).
 *
 * The game reads the keyboard through the KeyMap and key events, by Mac key code. A
 * remapped key and the controller simply produce the key the action has by default
 * (arrows, keypad "." and "0", Esc, space), so the game code is unchanged. Rewind and
 * quick save / load are Extras and produce no key. */
#include "platform.h"
#include "../mac/mac.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void event_key(int down, uint8_t keycode, uint8_t ch, uint16_t mods, int repeat);
int  mods_playing(void);

typedef struct { const char *name, *pref, *padpref; int key; uint8_t ch; int def, paddef; } Action;
#define AXIS(axis, pos) (PAD_AXIS + (axis) * 2 + (pos))
static const Action acts[ACT_COUNT] = {
    { "Up (jump, climb)",            "Keys.Up",      "Pad.Up",      0x7E, 0x1E, 0x7E, SDL_CONTROLLER_BUTTON_DPAD_UP },
    { "Down (crouch, descend)",      "Keys.Down",    "Pad.Down",    0x7D, 0x1F, 0x7D, SDL_CONTROLLER_BUTTON_DPAD_DOWN },
    { "Left",                        "Keys.Left",    "Pad.Left",    0x7B, 0x1C, 0x7B, SDL_CONTROLLER_BUTTON_DPAD_LEFT },
    { "Right",                       "Keys.Right",   "Pad.Right",   0x7C, 0x1D, 0x7C, SDL_CONTROLLER_BUTTON_DPAD_RIGHT },
    { "Action (grab, strike, careful step)", "Keys.Button1", "Pad.Button1", 0x41, '.', -1, SDL_CONTROLLER_BUTTON_A },   /* also Shift */
    { "Second button",               "Keys.Button2", "Pad.Button2", 0x52, '0', -1, SDL_CONTROLLER_BUTTON_B },          /* also Control */
    { "Pause",                       "Keys.Pause",   "Pad.Pause",   0x35, 0x1B, 0x35, SDL_CONTROLLER_BUTTON_START },
    { "Show time left",              "Keys.Time",    "Pad.Time",    0x31, ' ', 0x31, SDL_CONTROLLER_BUTTON_BACK },
    { "Rewind (hold)",               "Keys.Rewind",  "Pad.Rewind",  -1, 0, 0x33, SDL_CONTROLLER_BUTTON_LEFTSHOULDER },
    { "Quick save",                  "Keys.QuickSave", "Pad.QuickSave", -1, 0, 0x60, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER },
    { "Quick load",                  "Keys.QuickLoad", "Pad.QuickLoad", -1, 0, 0x65, SDL_CONTROLLER_BUTTON_Y },
    { "Add 5 minutes",               "Keys.AddTime", "Pad.AddTime", -1, 0, -1, -1 },
    { "Add one health",              "Keys.AddHealth", "Pad.AddHealth", -1, 0, -1, -1 },
};
static int key_for[ACT_COUNT], pad_for[ACT_COUNT];

const char *input_action_name(int a) { return a >= 0 && a < ACT_COUNT ? acts[a].name : ""; }
int  input_action_key(int a) { return key_for[a]; }
int  input_action_default_key(int a) { return acts[a].def; }
void input_load_keys(void) {
    const char *hk = getenv("POP2_HOTKEYS");
    for (int a = 0; a < ACT_COUNT; a++) {
        int paddef = acts[a].paddef;
        if (hk && *hk == '1' && (a == ACT_QSAVE || a == ACT_QLOAD)) paddef = -1;   /* hotkey combos instead */
        key_for[a] = macos_pref_int(acts[a].pref, acts[a].def);
        pad_for[a] = macos_pref_int(acts[a].padpref, paddef);
        if (pad_for[a] >= PAD_CODES) pad_for[a] = -1;
    }
}
void input_set_action_key(int a, int k) {
    if (a < 0 || a >= ACT_COUNT) return;
    for (int b = 0; b < ACT_COUNT; b++) if (b != a && k >= 0 && key_for[b] == k) {   /* a key has one action */
        key_for[b] = -1; macos_pref_set_int(acts[b].pref, -1);
    }
    key_for[a] = k;
    macos_pref_set_int(acts[a].pref, k);
}

const char *input_key_name(int k) {
    static char buf[16];
    static const char *names[128] = {
        [0x00]="A",[0x01]="S",[0x02]="D",[0x03]="F",[0x04]="H",[0x05]="G",[0x06]="Z",[0x07]="X",[0x08]="C",[0x09]="V",
        [0x0B]="B",[0x0C]="Q",[0x0D]="W",[0x0E]="E",[0x0F]="R",[0x10]="Y",[0x11]="T",[0x12]="1",[0x13]="2",[0x14]="3",
        [0x15]="4",[0x16]="6",[0x17]="5",[0x18]="=",[0x19]="9",[0x1A]="7",[0x1B]="-",[0x1C]="8",[0x1D]="0",[0x1E]="]",
        [0x1F]="O",[0x20]="U",[0x21]="[",[0x22]="I",[0x23]="P",[0x24]="Return",[0x25]="L",[0x26]="J",[0x27]="'",
        [0x28]="K",[0x29]=";",[0x2A]="\\",[0x2B]=",",[0x2C]="/",[0x2D]="N",[0x2E]="M",[0x2F]=".",[0x30]="Tab",
        [0x31]="Space",[0x32]="`",[0x33]="Delete",[0x35]="Esc",[0x41]="Keypad .",[0x43]="Keypad *",[0x45]="Keypad +",
        [0x47]="Clear",[0x4B]="Keypad /",[0x4C]="Enter",[0x4E]="Keypad -",[0x51]="Keypad =",[0x52]="Keypad 0",
        [0x53]="Keypad 1",[0x54]="Keypad 2",[0x55]="Keypad 3",[0x56]="Keypad 4",[0x57]="Keypad 5",[0x58]="Keypad 6",
        [0x59]="Keypad 7",[0x5B]="Keypad 8",[0x5C]="Keypad 9",[0x60]="F5",[0x61]="F6",[0x62]="F7",[0x63]="F3",
        [0x64]="F8",[0x65]="F9",[0x67]="F11",[0x6D]="F10",[0x6F]="F12",[0x72]="Help",[0x73]="Home",[0x74]="Page Up",
        [0x75]="Forward Delete",[0x76]="F4",[0x77]="End",[0x78]="F2",[0x79]="Page Down",[0x7A]="F1",
        [0x7B]="\xe2\x86\x90",[0x7C]="\xe2\x86\x92",[0x7D]="\xe2\x86\x93",[0x7E]="\xe2\x86\x91" };
    if (k < 0) return "\xe2\x80\x94";   /* em dash: none */
    if (k < 128 && names[k]) return names[k];
    snprintf(buf, sizeof buf, "key %d", k);
    return buf;
}

/* Extras that only make sense during play: elsewhere their key keeps its normal use. */
static int play_only(int a) { return a == ACT_REWIND || a == ACT_QSAVE || a == ACT_ADDTIME || a == ACT_ADDHEALTH; }

static void press(int a, int down, uint16_t mods, int repeat) {
    switch (a) {
        case ACT_REWIND: mods_set_rewind_held(down); return;
        case ACT_QSAVE: if (down && !repeat && mods_playing()) mods_quick_save(); return;
        case ACT_QLOAD: if (down && !repeat) mods_quick_load(); return;   /* from the title too */
        case ACT_ADDTIME: if (down && !repeat && mods_playing()) mods_add_time(); return;
        case ACT_ADDHEALTH: if (down && !repeat && mods_playing()) mods_add_health(); return;
    }
    event_key(down, (uint8_t)acts[a].key, acts[a].ch, mods, repeat);
}

int input_key(int down, int kc, uint16_t mods, int repeat) {
    if (mods & 0x0100) return 0;                       /* Command shortcuts are untouched */
    for (int a = 0; a < ACT_COUNT; a++) {
        if (key_for[a] != kc) continue;
        if (play_only(a) && down && !mods_playing()) return 0;     /* e.g. Delete outside play */
        if (a != ACT_REWIND && kc == acts[a].key) return 0;          /* the default key: as is */
        press(a, down, mods, repeat);
        return 1;
    }
    return 0;
}

/* ---- game controllers ----
 *
 * A controller input is a code: an SDL game controller button (Xbox layout), or
 * PAD_AXIS + axis * 2 + (1 = positive direction) for a stick direction or a trigger.
 * Each action has one, chosen in Extras > Controls...; an input drives one action. The
 * left stick also moves the kid unless one of its directions is assigned. */
static uint8_t code_down[PAD_CODES];
static uint32_t pad_held;              /* bit per action */
static SDL_GameController *pads[8];
static int pad_gen;

int  input_action_pad(int a) { return pad_for[a]; }
int  input_action_default_pad(int a) { return acts[a].paddef; }
void input_set_action_pad(int a, int code) {
    if (a < 0 || a >= ACT_COUNT) return;
    for (int b = 0; b < ACT_COUNT; b++) if (b != a && code >= 0 && pad_for[b] == code) {
        pad_for[b] = -1; macos_pref_set_int(acts[b].padpref, -1);
    }
    pad_for[a] = code;
    macos_pref_set_int(acts[a].padpref, code);
}

static void pad_update(void) {
    uint32_t now = 0;
    int stick_free = 1;
    for (int a = 0; a < ACT_COUNT; a++) {
        int c = pad_for[a];
        if (c < 0) continue;
        if (code_down[c]) now |= 1u << a;
        if (c >= AXIS(SDL_CONTROLLER_AXIS_LEFTX, 0) && c <= AXIS(SDL_CONTROLLER_AXIS_LEFTY, 1)) stick_free = 0;
    }
    if (stick_free) {
        if (code_down[AXIS(SDL_CONTROLLER_AXIS_LEFTX, 0)]) now |= 1u << ACT_LEFT;
        if (code_down[AXIS(SDL_CONTROLLER_AXIS_LEFTX, 1)]) now |= 1u << ACT_RIGHT;
        if (code_down[AXIS(SDL_CONTROLLER_AXIS_LEFTY, 0)]) now |= 1u << ACT_UP;
        if (code_down[AXIS(SDL_CONTROLLER_AXIS_LEFTY, 1)]) now |= 1u << ACT_DOWN;
    }
    for (int a = 0; a < ACT_COUNT; a++) {
        uint32_t b = 1u << a;
        if ((now ^ pad_held) & b) press(a, (now & b) != 0, 0, 0);
    }
    pad_held = now;
}

static void pad_open(int index) {
    if (!SDL_IsGameController(index)) return;
    SDL_GameController *gc = SDL_GameControllerOpen(index);
    if (!gc) return;
    for (int i = 0; i < 8; i++) if (pads[i] == gc) return;          /* already open */
    for (int i = 0; i < 8; i++) if (!pads[i]) { pads[i] = gc; pad_gen++; return; }
}

static void pad_close(SDL_JoystickID id) {
    for (int i = 0; i < 8; i++)
        if (pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == id) {
            SDL_GameControllerClose(pads[i]); pads[i] = NULL; pad_gen++;
        }
}

/* Updates code_down from a controller event; returns the code that just went down, or -1. */
static int pad_track(const SDL_Event *e) {
    switch (e->type) {
        case SDL_CONTROLLERDEVICEADDED: pad_open(e->cdevice.which); return -1;
        case SDL_CONTROLLERDEVICEREMOVED:
            pad_close(e->cdevice.which);
            memset(code_down, 0, sizeof code_down);
            return -1;
        case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: {
            int c = e->cbutton.button, down = e->type == SDL_CONTROLLERBUTTONDOWN;
            if (c < 0 || c >= PAD_AXIS) return -1;
            int was = code_down[c];
            code_down[c] = (uint8_t)down;
            return down && !was ? c : -1;
        }
        case SDL_CONTROLLERAXISMOTION: {
            const int on = 16000, off = 12000;   /* hysteresis */
            int v = e->caxis.value, ax = e->caxis.axis;
            if (ax < 0 || ax >= SDL_CONTROLLER_AXIS_MAX) return -1;
            int r = -1;
            for (int pos = 0; pos < 2; pos++) {
                int c = AXIS(ax, pos), d = pos ? v : -v;
                if (d > on && !code_down[c]) { code_down[c] = 1; r = c; }
                else if (d < off) code_down[c] = 0;
            }
            return r;
        }
    }
    return -1;
}

/* ---- Recalbox conventions (POP2_HOTKEYS=1): Hotkey (Select, or the pad's hotkey button,
 * which SDL reports as Guide) with Start quits, with Y saves a state, with X loads the
 * newest, with Left rewinds while held, with Up adds one health, with Right adds 5
 * minutes; Select alone keeps its action (show time left).
 * While a dialog waits for an answer, A or Start is Return and B is Esc. ---- */
int  wm_modal_active(void);
static int hotkeys = -1, hk_held, hk_combo;
static uint8_t swallowed[PAD_AXIS];    /* buttons whose release belongs to a combo or a dialog */

static void key_tap(uint8_t kc, uint8_t ch) { event_key(1, kc, ch, 0, 0); event_key(0, kc, ch, 0, 0); }

static void hotkey_combo(int b, int down) {
    switch (b) {
        case SDL_CONTROLLER_BUTTON_START:
            if (down) { SDL_Event q = { .type = SDL_QUIT }; SDL_PushEvent(&q); }
            break;
        case SDL_CONTROLLER_BUTTON_Y: if (down) press(ACT_QSAVE, 1, 0, 0); break;
        case SDL_CONTROLLER_BUTTON_X: if (down) press(ACT_QLOAD, 1, 0, 0); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: if (mods_playing() || !down) press(ACT_REWIND, down, 0, 0); break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: if (down) press(ACT_ADDHEALTH, 1, 0, 0); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: if (down) press(ACT_ADDTIME, 1, 0, 0); break;
    }
}

/* 1: the event is used up here */
static int special_button(const SDL_Event *e) {
    if (e->type != SDL_CONTROLLERBUTTONDOWN && e->type != SDL_CONTROLLERBUTTONUP) return 0;
    int b = e->cbutton.button, down = e->type == SDL_CONTROLLERBUTTONDOWN;
    if (b < 0 || b >= PAD_AXIS) return 0;
    if (!down && swallowed[b]) {
        swallowed[b] = 0;
        if (hk_combo) hotkey_combo(b, 0);
        return 1;
    }
    if (down && wm_modal_active()) {
        if (b == SDL_CONTROLLER_BUTTON_A || b == SDL_CONTROLLER_BUTTON_START) { key_tap(0x24, 13); swallowed[b] = 1; return 1; }
        if (b == SDL_CONTROLLER_BUTTON_B) { key_tap(0x35, 27); swallowed[b] = 1; return 1; }
    }
    if (hotkeys < 0) hotkeys = getenv("POP2_HOTKEYS") && *getenv("POP2_HOTKEYS") == '1';
    if (!hotkeys) return 0;
    if (b == SDL_CONTROLLER_BUTTON_BACK || b == SDL_CONTROLLER_BUTTON_GUIDE) {
        if (down) { if (!hk_held) hk_combo = 0; hk_held |= 1 << b; return 1; }
        hk_held &= ~(1 << b);
        if (!hk_held && !hk_combo)             /* Select alone: its own action */
            for (int a = 0; a < ACT_COUNT; a++)
                if (pad_for[a] == SDL_CONTROLLER_BUTTON_BACK) { press(a, 1, 0, 0); press(a, 0, 0, 0); }
        return 1;
    }
    if (down && hk_held) {
        /* a lost release must not leave the hotkey held: ask the controller */
        SDL_GameController *gc = SDL_GameControllerFromInstanceID(e->cbutton.which);
        if (gc && !SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK) &&
            !SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_GUIDE)) hk_held = 0;
    }
    if (down && hk_held) { hk_combo = 1; swallowed[b] = 1; hotkey_combo(b, 1); return 1; }
    return 0;
}

void input_controller_event(const SDL_Event *e) {
    if (special_button(e)) return;
    pad_track(e);
    pad_update();
}

/* ---- controller mappings from EmulationStation's es_input.cfg (Recalbox): the pads as
 * configured in Recalbox, converted as its configgen does for SDL games ---- */
static int xml_attr(const char *tag, const char *end, const char *name, char *out, size_t n) {
    char pat[40];
    snprintf(pat, sizeof pat, " %s=\"", name);
    const char *p = strstr(tag, pat);
    if (!p || p > end) return 0;
    p += strlen(pat);
    const char *q = strchr(p, '"');
    if (!q || q > end) return 0;
    size_t l = (size_t)(q - p) < n - 1 ? (size_t)(q - p) : n - 1;
    memcpy(out, p, l); out[l] = 0;
    return 1;
}
static const char *sdl_name(const char *es) {
    static const char *const map[][2] = {
        { "south", "a" }, { "b", "a" }, { "east", "b" }, { "a", "b" }, { "north", "y" }, { "x", "y" },
        { "west", "x" }, { "y", "x" }, { "start", "start" }, { "select", "back" }, { "hotkey", "guide" },
        { "l1", "leftshoulder" }, { "pageup", "leftshoulder" }, { "r1", "rightshoulder" }, { "pagedown", "rightshoulder" },
        { "l2", "lefttrigger" }, { "r2", "righttrigger" }, { "l3", "leftstick" }, { "r3", "rightstick" },
        { "up", "dpup" }, { "down", "dpdown" }, { "left", "dpleft" }, { "right", "dpright" },
        { "joystick1up", "-lefty" }, { "joystick1left", "-leftx" }, { "joystick1down", "+lefty" }, { "joystick1right", "+leftx" },
        { "joystick2up", "-righty" }, { "joystick2left", "-rightx" }, { "joystick2down", "+righty" }, { "joystick2right", "+rightx" } };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) if (!strcmp(map[i][0], es)) return map[i][1];
    return NULL;
}
void input_load_es_input(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[input] %s: not found\n", path); return; }
    static char buf[1 << 18];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    int count = 0;
    for (char *c = strstr(buf, "<inputConfig"); c; c = strstr(c + 1, "<inputConfig")) {
        char *ce = strstr(c, "</inputConfig>"), *te = strchr(c, '>');
        if (!ce || !te) break;
        char type[16], name[128], guid[64];
        if (!xml_attr(c, te, "type", type, sizeof type) || strcmp(type, "joystick") ||
            !xml_attr(c, te, "deviceGUID", guid, sizeof guid) || !xml_attr(c, te, "deviceName", name, sizeof name)) continue;
        for (char *k = name; *k; k++) if (*k == ',') *k = ' ';
        char line[2048];
        int l = snprintf(line, sizeof line, "%s,%s,platform:Linux,", guid, name);
        for (char *in = strstr(c, "<input "); in && in < ce; in = strstr(in + 1, "<input ")) {
            char *ie = strchr(in, '>');
            char iname[32], itype[16], id[16], val[16];
            if (!ie || !xml_attr(in, ie, "name", iname, sizeof iname) || !xml_attr(in, ie, "type", itype, sizeof itype) ||
                !xml_attr(in, ie, "id", id, sizeof id) || !xml_attr(in, ie, "value", val, sizeof val)) continue;
            const char *sn = sdl_name(iname);
            if (!sn || l >= (int)sizeof line - 64) continue;
            if (!strcmp(itype, "axis")) l += snprintf(line + l, sizeof line - l, "%s:%sa%s,", sn, atoi(val) < 0 ? "-" : "+", id);
            else if (!strcmp(itype, "hat")) l += snprintf(line + l, sizeof line - l, "%s:h%s.%s,", sn, id, val);
            else if (!strcmp(itype, "button")) l += snprintf(line + l, sizeof line - l, "%s:b%s,", sn, id);
        }
        if (SDL_GameControllerAddMapping(line) >= 0) count++;
        else fprintf(stderr, "[input] mapping refused (%s): %s\n", SDL_GetError(), line);
    }
    fprintf(stderr, "[input] %d controller mapping(s) from %s\n", count, path);
}

/* Controls window: called about 60 times a second while it is open, outside the game's
 * event loop. Controller events are consumed here (none reach the game afterwards);
 * returns the controller input that was just pressed, or -1. */
int input_pad_capture_poll(void) {
    SDL_GameControllerUpdate();
    SDL_Event e;
    int r = -1;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_CONTROLLERAXISMOTION, SDL_CONTROLLERDEVICEREMAPPED) > 0) {
        int c = pad_track(&e);
        if (c >= 0 && r < 0) r = c;
    }
    SDL_FlushEvents(SDL_JOYAXISMOTION, SDL_JOYDEVICEREMOVED);
    return r;
}

/* The Controls window takes controller input although the game window is not in front. */
void input_pad_capture(int on) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, on ? "1" : "0");
    if (!on) {                           /* buttons pressed in the window do nothing in the game */
        uint32_t held = pad_held;
        pad_held = 0;
        if (held & (1u << ACT_REWIND)) mods_set_rewind_held(0);
    }
}

int input_pad_generation(void) { return pad_gen; }

static SDL_GameController *first_pad(void) {
    for (int i = 0; i < 8; i++) if (pads[i]) return pads[i];
    return NULL;
}

/* Name of the connected controller (the first one if several), NULL if none. */
const char *input_pad_name(void) {
    SDL_GameController *gc = first_pad();
    if (!gc) return NULL;
    const char *n = SDL_GameControllerName(gc);
    return n && *n ? n : "Game controller";
}

int input_pad_count(void) {
    int n = 0;
    for (int i = 0; i < 8; i++) n += pads[i] != NULL;
    return n;
}

/* Name of a controller input, with the labels of the connected controller's family. */
const char *input_pad_code_name(int c) {
    enum { XBOX, PS, NINTENDO };
    int fam = XBOX, ps5 = 0, ps3 = 0, xone = 0;
    SDL_GameController *gc = first_pad();
    if (gc) switch (SDL_GameControllerGetType(gc)) {
        case SDL_CONTROLLER_TYPE_PS3: fam = PS; ps3 = 1; break;
        case SDL_CONTROLLER_TYPE_PS4: fam = PS; break;
        case SDL_CONTROLLER_TYPE_PS5: fam = PS; ps5 = 1; break;
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: fam = NINTENDO; break;
        case SDL_CONTROLLER_TYPE_XBOXONE: xone = 1; break;
        default: break;
    }
    /* face buttons by position (south, east, west, north): Nintendo labels differ from
     * the position SDL reports depending on the SDL version, so positions are named */
    static const char *const face[3][4] = {
        { "A", "B", "X", "Y" },
        { "Cross", "Circle", "Square", "Triangle" },
        { "Bottom button", "Right button", "Left button", "Top button" } };
    static const char *const shoulder[3][4] = {     /* LB, RB, LT, RT */
        { "LB", "RB", "LT", "RT" }, { "L1", "R1", "L2", "R2" }, { "L", "R", "ZL", "ZR" } };
    static char buf[32];
    if (c < 0) return "\xe2\x80\x94";
    switch (c) {
        case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_B:
        case SDL_CONTROLLER_BUTTON_X: case SDL_CONTROLLER_BUTTON_Y: return face[fam][c - SDL_CONTROLLER_BUTTON_A];
        case SDL_CONTROLLER_BUTTON_BACK:
            return fam == PS ? (ps3 ? "Select" : ps5 ? "Create" : "Share") : fam == NINTENDO ? "\xe2\x88\x92" : xone ? "View" : "Back";
        case SDL_CONTROLLER_BUTTON_GUIDE: return fam == PS ? "PS button" : fam == NINTENDO ? "Home" : "Xbox button";
        case SDL_CONTROLLER_BUTTON_START: return fam == PS ? (ps3 ? "Start" : "Options") : fam == NINTENDO ? "+" : xone ? "Menu" : "Start";
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: return fam == PS ? "L3" : "Left stick click";
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return fam == PS ? "R3" : "Right stick click";
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return shoulder[fam][0];
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return shoulder[fam][1];
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return "D-pad up";
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "D-pad down";
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "D-pad left";
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "D-pad right";
        case SDL_CONTROLLER_BUTTON_MISC1: return fam == PS ? "Mute" : fam == NINTENDO ? "Capture" : "Share";
        case SDL_CONTROLLER_BUTTON_PADDLE1: return "Paddle 1";
        case SDL_CONTROLLER_BUTTON_PADDLE2: return "Paddle 2";
        case SDL_CONTROLLER_BUTTON_PADDLE3: return "Paddle 3";
        case SDL_CONTROLLER_BUTTON_PADDLE4: return "Paddle 4";
        case SDL_CONTROLLER_BUTTON_TOUCHPAD: return "Touchpad";
        case AXIS(SDL_CONTROLLER_AXIS_LEFTX, 0): return "Left stick left";
        case AXIS(SDL_CONTROLLER_AXIS_LEFTX, 1): return "Left stick right";
        case AXIS(SDL_CONTROLLER_AXIS_LEFTY, 0): return "Left stick up";
        case AXIS(SDL_CONTROLLER_AXIS_LEFTY, 1): return "Left stick down";
        case AXIS(SDL_CONTROLLER_AXIS_RIGHTX, 0): return "Right stick left";
        case AXIS(SDL_CONTROLLER_AXIS_RIGHTX, 1): return "Right stick right";
        case AXIS(SDL_CONTROLLER_AXIS_RIGHTY, 0): return "Right stick up";
        case AXIS(SDL_CONTROLLER_AXIS_RIGHTY, 1): return "Right stick down";
        case AXIS(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1): return shoulder[fam][2];
        case AXIS(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 1): return shoulder[fam][3];
    }
    snprintf(buf, sizeof buf, "input %d", c);
    return buf;
}
