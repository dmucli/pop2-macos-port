/* Input: remappable keys (Extras > Controls...) and game controllers.
 *
 * The game reads the keyboard through the KeyMap and key events, by Mac key code. A
 * remapped key and the controller simply produce the key the action has by default
 * (arrows, keypad "." and "0", Esc, space), so the game code is unchanged. */
#include "platform.h"
#include "../mac/mac.h"
#include <SDL.h>
#include <stdio.h>

void event_key(int down, uint8_t keycode, uint8_t ch, uint16_t mods, int repeat);
int  mods_playing(void);

typedef struct { const char *name, *pref; int key; uint8_t ch; int def; } Action;
static const Action acts[ACT_COUNT] = {
    { "Up (jump, climb)",            "Keys.Up",      0x7E, 0x1E, 0x7E },
    { "Down (crouch, descend)",      "Keys.Down",    0x7D, 0x1F, 0x7D },
    { "Left",                        "Keys.Left",    0x7B, 0x1C, 0x7B },
    { "Right",                       "Keys.Right",   0x7C, 0x1D, 0x7C },
    { "Action (grab, strike, careful step)", "Keys.Button1", 0x41, '.', -1 },   /* also Shift */
    { "Second button",               "Keys.Button2", 0x52, '0', -1 },          /* also Control */
    { "Pause",                       "Keys.Pause",   0x35, 0x1B, 0x35 },
    { "Show time left",              "Keys.Time",    0x31, ' ', 0x31 },
    { "Rewind (hold)",               "Keys.Rewind",  -1, 0, 0x33 },
};
static int key_for[ACT_COUNT];

const char *input_action_name(int a) { return a >= 0 && a < ACT_COUNT ? acts[a].name : ""; }
int  input_action_key(int a) { return key_for[a]; }
int  input_action_default_key(int a) { return acts[a].def; }
void input_load_keys(void) { for (int a = 0; a < ACT_COUNT; a++) key_for[a] = macos_pref_int(acts[a].pref, acts[a].def); }
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

static void press(int a, int down, uint16_t mods, int repeat) {
    if (a == ACT_REWIND) { mods_set_rewind_held(down); return; }
    event_key(down, (uint8_t)acts[a].key, acts[a].ch, mods, repeat);
}

int input_key(int down, int kc, uint16_t mods, int repeat) {
    if (mods & 0x0100) return 0;                       /* Command shortcuts are untouched */
    /* quick save / load (F5 / F9), only during play */
    if (kc == 0x60 || kc == 0x65) {
        if (!mods_playing()) return 0;
        if (down && !repeat) { if (kc == 0x60) mods_quick_save(0); else mods_quick_load(0); }
        return 1;
    }
    for (int a = 0; a < ACT_COUNT; a++) {
        if (key_for[a] != kc) continue;
        if (a == ACT_REWIND && down && !mods_playing()) return 0;   /* Delete keeps its use outside play */
        if (a != ACT_REWIND && kc == acts[a].key) return 0;          /* the default key: as is */
        press(a, down, mods, repeat);
        return 1;
    }
    return 0;
}

/* ---- game controllers: d-pad or left stick = arrows, A = action, B = second button,
 * X = action too, Start = pause, Back = show time, left shoulder = rewind (held),
 * right shoulder / Y = quick save / quick load ---- */
static uint32_t pad_dpad, pad_stick, pad_buttons, pad_held;   /* bit per action */

static void pad_update(void) {
    uint32_t now = pad_dpad | pad_stick | pad_buttons;
    for (int a = 0; a < ACT_COUNT; a++) {
        uint32_t b = 1u << a;
        if ((now ^ pad_held) & b) press(a, (now & b) != 0, 0, 0);
    }
    pad_held = now;
}

void input_controller_event(const SDL_Event *e) {
    switch (e->type) {
        case SDL_CONTROLLERDEVICEADDED:
            if (SDL_IsGameController(e->cdevice.which)) SDL_GameControllerOpen(e->cdevice.which);
            return;
        case SDL_CONTROLLERDEVICEREMOVED:
            pad_dpad = pad_stick = pad_buttons = 0; pad_update();
            return;
        case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: {
            int down = e->type == SDL_CONTROLLERBUTTONDOWN, a = -1;
            switch (e->cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP: a = ACT_UP; break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN: a = ACT_DOWN; break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: a = ACT_LEFT; break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: a = ACT_RIGHT; break;
                case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_X: a = ACT_BUTTON1; break;
                case SDL_CONTROLLER_BUTTON_B: a = ACT_BUTTON2; break;
                case SDL_CONTROLLER_BUTTON_START: a = ACT_PAUSE; break;
                case SDL_CONTROLLER_BUTTON_BACK: a = ACT_TIME; break;
                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: a = ACT_REWIND; break;
                case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: if (down && mods_playing()) mods_quick_save(0); return;
                case SDL_CONTROLLER_BUTTON_Y: if (down && mods_playing()) mods_quick_load(0); return;
                default: return;
            }
            uint32_t *set = a <= ACT_RIGHT ? &pad_dpad : &pad_buttons;
            if (down) *set |= 1u << a; else *set &= ~(1u << a);
            pad_update();
            return;
        }
        case SDL_CONTROLLERAXISMOTION: {
            const int on = 16000, off = 12000;   /* hysteresis */
            int v = e->caxis.value;
            int lo, hi;
            if (e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) { lo = ACT_LEFT; hi = ACT_RIGHT; }
            else if (e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) { lo = ACT_UP; hi = ACT_DOWN; }
            else return;
            uint32_t bl = 1u << lo, bh = 1u << hi;
            if (v < -on) pad_stick |= bl; else if (v > -off) pad_stick &= ~bl;
            if (v > on) pad_stick |= bh; else if (v < off) pad_stick &= ~bh;
            pad_update();
            return;
        }
    }
}
