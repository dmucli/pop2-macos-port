/* SDL2 platform: window + 8-bit framebuffer presentation, input, audio init. */
#include "../mac/qd.h"
#include <SDL.h>
#include <stdio.h>

int  sound_init(void);
void event_key(int down, uint8_t keycode, uint8_t ch, uint16_t mods, int repeat);
void event_modifiers_changed(uint16_t mods);
void event_mouse_move(int h, int v);
void event_mouse_button(int down);
void ExitToShell(void);

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static int headless = 1;
static uint32_t last_dirty, last_clut;
static uint32_t last_present_ms;
static uint32_t rgb[256];
static int cursor_visible = 1;

int platform_init(int scale) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return -1; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_SetHint(SDL_HINT_MAC_CTRL_CLICK_EMULATE_RIGHT_CLICK, "0");
    if (scale < 1) scale = 1;
    win = SDL_CreateWindow("Prince of Persia 2", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           qd_screen_w * scale, qd_screen_h * scale, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { fprintf(stderr, "SDL window: %s\n", SDL_GetError()); return -1; }
    /* never wait for the display: the game runs on this thread and presents from inside
     * its own loops (a blocking present costs up to a frame each time) */
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    SDL_RenderSetLogicalSize(ren, qd_screen_w, qd_screen_h);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, qd_screen_w, qd_screen_h);
    headless = 0;
    sound_init();
    return 0;
}
void platform_shutdown(void) { if (!headless) SDL_Quit(); }
int  platform_is_headless(void) { return headless; }

void platform_present(void) {
    if (headless) return;
    if (last_clut != qd_clut_version) {
        for (int i = 0; i < 256; i++) rgb[i] = 0xFF000000u | qd_device_rgb(i);
        last_clut = qd_clut_version;
    }
    void *pixels; int pitch;
    if (SDL_LockTexture(tex, NULL, &pixels, &pitch) == 0) {
        const uint8_t *src = mptr(SCREEN_BASE);
        for (int y = 0; y < qd_screen_h; y++) {
            uint32_t *d = (uint32_t *)((uint8_t *)pixels + y * pitch);
            const uint8_t *s = src + y * qd_screen_w;
            for (int x = 0; x < qd_screen_w; x++) d[x] = rgb[s[x]];
        }
        SDL_UnlockTexture(tex);
    }
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
}

void platform_present_if_due(void) {
    if (headless) return;
    uint32_t now = SDL_GetTicks();
    if (now - last_present_ms < 16) return;
    /* present when QuickDraw changed the screen or the colour table; the game may also write
     * pixels directly (GetPixBaseAddr), so refresh at least every 100 ms regardless */
    if (qd_screen_dirty == last_dirty && qd_clut_version == last_clut && now - last_present_ms < 100) return;
    last_present_ms = now;
    { extern uint32_t frames_changed; if (qd_screen_dirty != last_dirty) frames_changed++; }   /* stats */
    last_dirty = qd_screen_dirty;
    platform_present();
}

void platform_wait_ms(int ms) { SDL_Delay((Uint32)ms); }

/* ---- keyboard ---- */
static int mac_keycode(SDL_Scancode s) {
    switch (s) {
        case SDL_SCANCODE_A: return 0x00; case SDL_SCANCODE_S: return 0x01; case SDL_SCANCODE_D: return 0x02;
        case SDL_SCANCODE_F: return 0x03; case SDL_SCANCODE_H: return 0x04; case SDL_SCANCODE_G: return 0x05;
        case SDL_SCANCODE_Z: return 0x06; case SDL_SCANCODE_X: return 0x07; case SDL_SCANCODE_C: return 0x08;
        case SDL_SCANCODE_V: return 0x09; case SDL_SCANCODE_B: return 0x0B; case SDL_SCANCODE_Q: return 0x0C;
        case SDL_SCANCODE_W: return 0x0D; case SDL_SCANCODE_E: return 0x0E; case SDL_SCANCODE_R: return 0x0F;
        case SDL_SCANCODE_Y: return 0x10; case SDL_SCANCODE_T: return 0x11; case SDL_SCANCODE_1: return 0x12;
        case SDL_SCANCODE_2: return 0x13; case SDL_SCANCODE_3: return 0x14; case SDL_SCANCODE_4: return 0x15;
        case SDL_SCANCODE_6: return 0x16; case SDL_SCANCODE_5: return 0x17; case SDL_SCANCODE_EQUALS: return 0x18;
        case SDL_SCANCODE_9: return 0x19; case SDL_SCANCODE_7: return 0x1A; case SDL_SCANCODE_MINUS: return 0x1B;
        case SDL_SCANCODE_8: return 0x1C; case SDL_SCANCODE_0: return 0x1D; case SDL_SCANCODE_RIGHTBRACKET: return 0x1E;
        case SDL_SCANCODE_O: return 0x1F; case SDL_SCANCODE_U: return 0x20; case SDL_SCANCODE_LEFTBRACKET: return 0x21;
        case SDL_SCANCODE_I: return 0x22; case SDL_SCANCODE_P: return 0x23; case SDL_SCANCODE_RETURN: return 0x24;
        case SDL_SCANCODE_L: return 0x25; case SDL_SCANCODE_J: return 0x26; case SDL_SCANCODE_APOSTROPHE: return 0x27;
        case SDL_SCANCODE_K: return 0x28; case SDL_SCANCODE_SEMICOLON: return 0x29; case SDL_SCANCODE_BACKSLASH: return 0x2A;
        case SDL_SCANCODE_COMMA: return 0x2B; case SDL_SCANCODE_SLASH: return 0x2C; case SDL_SCANCODE_N: return 0x2D;
        case SDL_SCANCODE_M: return 0x2E; case SDL_SCANCODE_PERIOD: return 0x2F; case SDL_SCANCODE_TAB: return 0x30;
        case SDL_SCANCODE_SPACE: return 0x31; case SDL_SCANCODE_GRAVE: return 0x32; case SDL_SCANCODE_BACKSPACE: return 0x33;
        case SDL_SCANCODE_ESCAPE: return 0x35; case SDL_SCANCODE_LGUI: return 0x37; case SDL_SCANCODE_RGUI: return 0x37;
        case SDL_SCANCODE_LSHIFT: return 0x38; case SDL_SCANCODE_CAPSLOCK: return 0x39; case SDL_SCANCODE_LALT: return 0x3A;
        case SDL_SCANCODE_LCTRL: return 0x3B; case SDL_SCANCODE_RSHIFT: return 0x3C; case SDL_SCANCODE_RALT: return 0x3D;
        case SDL_SCANCODE_RCTRL: return 0x3E;
        case SDL_SCANCODE_KP_PERIOD: return 0x41; case SDL_SCANCODE_KP_MULTIPLY: return 0x43; case SDL_SCANCODE_KP_PLUS: return 0x45;
        case SDL_SCANCODE_NUMLOCKCLEAR: return 0x47; case SDL_SCANCODE_KP_DIVIDE: return 0x4B; case SDL_SCANCODE_KP_ENTER: return 0x4C;
        case SDL_SCANCODE_KP_MINUS: return 0x4E; case SDL_SCANCODE_KP_EQUALS: return 0x51; case SDL_SCANCODE_KP_0: return 0x52;
        case SDL_SCANCODE_KP_1: return 0x53; case SDL_SCANCODE_KP_2: return 0x54; case SDL_SCANCODE_KP_3: return 0x55;
        case SDL_SCANCODE_KP_4: return 0x56; case SDL_SCANCODE_KP_5: return 0x57; case SDL_SCANCODE_KP_6: return 0x58;
        case SDL_SCANCODE_KP_7: return 0x59; case SDL_SCANCODE_KP_8: return 0x5B; case SDL_SCANCODE_KP_9: return 0x5C;
        case SDL_SCANCODE_F5: return 0x60; case SDL_SCANCODE_F6: return 0x61; case SDL_SCANCODE_F7: return 0x62;
        case SDL_SCANCODE_F3: return 0x63; case SDL_SCANCODE_F8: return 0x64; case SDL_SCANCODE_F9: return 0x65;
        case SDL_SCANCODE_F11: return 0x67; case SDL_SCANCODE_F10: return 0x6D; case SDL_SCANCODE_F12: return 0x6F;
        case SDL_SCANCODE_INSERT: return 0x72; case SDL_SCANCODE_HOME: return 0x73; case SDL_SCANCODE_PAGEUP: return 0x74;
        case SDL_SCANCODE_DELETE: return 0x75; case SDL_SCANCODE_F4: return 0x76; case SDL_SCANCODE_END: return 0x77;
        case SDL_SCANCODE_F2: return 0x78; case SDL_SCANCODE_PAGEDOWN: return 0x79; case SDL_SCANCODE_F1: return 0x7A;
        case SDL_SCANCODE_LEFT: return 0x7B; case SDL_SCANCODE_RIGHT: return 0x7C; case SDL_SCANCODE_DOWN: return 0x7D;
        case SDL_SCANCODE_UP: return 0x7E;
        default: return -1;
    }
}
static uint8_t mac_char(SDL_Keycode k, uint16_t mods) {
    int shift = (mods & 0x0200) || (mods & 0x0400);
    switch (k) {
        case SDLK_LEFT: return 0x1C; case SDLK_RIGHT: return 0x1D; case SDLK_UP: return 0x1E; case SDLK_DOWN: return 0x1F;
        case SDLK_RETURN: return 0x0D; case SDLK_KP_ENTER: return 0x03; case SDLK_BACKSPACE: return 0x08;
        case SDLK_TAB: return 0x09; case SDLK_ESCAPE: return 0x1B; case SDLK_DELETE: return 0x7F;
        case SDLK_HOME: return 0x01; case SDLK_END: return 0x04; case SDLK_PAGEUP: return 0x0B; case SDLK_PAGEDOWN: return 0x0C;
        case SDLK_INSERT: return 0x05;
        case SDLK_F1: case SDLK_F2: case SDLK_F3: case SDLK_F4: case SDLK_F5: case SDLK_F6: case SDLK_F7: case SDLK_F8:
        case SDLK_F9: case SDLK_F10: case SDLK_F11: case SDLK_F12: return 0x10;
        case SDLK_KP_0: return '0'; case SDLK_KP_1: return '1'; case SDLK_KP_2: return '2'; case SDLK_KP_3: return '3';
        case SDLK_KP_4: return '4'; case SDLK_KP_5: return '5'; case SDLK_KP_6: return '6'; case SDLK_KP_7: return '7';
        case SDLK_KP_8: return '8'; case SDLK_KP_9: return '9'; case SDLK_KP_PERIOD: return '.'; case SDLK_KP_PLUS: return '+';
        case SDLK_KP_MINUS: return '-'; case SDLK_KP_MULTIPLY: return '*'; case SDLK_KP_DIVIDE: return '/'; case SDLK_KP_EQUALS: return '=';
    }
    if (k >= 'a' && k <= 'z') return (uint8_t)(shift ? k - 32 : k);
    if (k >= 32 && k < 127) {
        if (!shift) return (uint8_t)k;
        static const char *from = "1234567890-=[]\\;',./`", *to = "!@#$%^&*()_+{}|:\"<>?~";
        const char *p = strchr(from, (int)k);
        return (uint8_t)(p ? to[p - from] : k);
    }
    return 0;
}
static uint16_t mac_mods(SDL_Keymod m) {
    uint16_t r = 0;
    if (m & KMOD_GUI) r |= 0x0100;
    if (m & KMOD_SHIFT) r |= 0x0200;
    if (m & KMOD_CAPS) r |= 0x0400;
    if (m & KMOD_ALT) r |= 0x0800;
    if (m & KMOD_CTRL) r |= 0x1000;
    return r;
}

static void toggle_fullscreen(void) {
    Uint32 f = SDL_GetWindowFlags(win);
    SDL_SetWindowFullscreen(win, (f & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
}

void platform_menu_update(void);
void platform_poll(void) {
    if (headless) return;
    platform_menu_update();
    static uint32_t last;
    uint32_t now = SDL_GetTicks();
    if (now == last) return;
    last = now;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT: ExitToShell(); break;
            case SDL_KEYDOWN: case SDL_KEYUP: {
                uint16_t mods = mac_mods(SDL_GetModState());
                if (e.type == SDL_KEYDOWN && (mods & 0x0100) && e.key.keysym.sym == SDLK_f) { toggle_fullscreen(); break; }
                if (e.type == SDL_KEYDOWN && (mods & 0x0800) && e.key.keysym.sym == SDLK_RETURN) { toggle_fullscreen(); break; }
                int kc = mac_keycode(e.key.keysym.scancode);
                event_modifiers_changed(mods);
                if (kc < 0 || (kc >= 0x37 && kc <= 0x3E)) break;
                event_key(e.type == SDL_KEYDOWN, (uint8_t)kc, mac_char(e.key.keysym.sym, mods), mods, e.key.repeat);
                break;
            }
            case SDL_MOUSEMOTION: event_mouse_move(e.motion.x, e.motion.y); break;
            case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
                event_mouse_move(e.button.x, e.button.y);
                event_mouse_button(e.type == SDL_MOUSEBUTTONDOWN);
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_EXPOSED) platform_present();
                break;
        }
    }
}

void platform_set_cursor_visible(int v) {
    if (headless || v == cursor_visible) return;
    cursor_visible = v;
    SDL_ShowCursor(v ? SDL_ENABLE : SDL_DISABLE);
}
void platform_set_cursor(maddr c) { (void)c; }

/* Native menus are provided by platform_menu.m when built with Cocoa; these are fallbacks. */
__attribute__((weak)) void platform_menu_sync(void) {}
__attribute__((weak)) int platform_menu_pending(uint32_t *r) { *r = 0; return 0; }
__attribute__((weak)) int platform_menu_has_pending(void) { return 0; }
__attribute__((weak)) void platform_menu_update(void) {}
void platform_toggle_fullscreen(void) { if (!headless) toggle_fullscreen(); }
