/* Overlay: drawn by the host over the game picture, never into the game's memory (the game
 * runs exactly as without it). Short messages ("State saved"), the rewind indicator, and a
 * menu for controllers and keyboards (Extras: save states with pictures, levels, cheats,
 * display, statistics), opened with F1, the controller's Guide button or, on Recalbox,
 * Hotkey + B. The menu is modal: the game waits while it is open.
 *
 * Text uses the built-in bitmap font (Spleen 6x12) in the 640x480 logical space. */
#include "platform.h"
#include "../mac/mac.h"
#include "../mac/mods.h"
#include "../mac/states.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../mac/hostfont.h"

SDL_Renderer *platform_renderer(void);
void platform_render_game(void);
int  platform_is_headless(void);
void event_release_all(void);

/* ---- text ---- */
static SDL_Texture *atlas;                 /* the 224 glyphs, 16 to a row, white */

static void make_atlas(SDL_Renderer *r) {
    if (atlas) return;
    enum { W = 16 * HOSTFONT_W, H = 14 * HOSTFONT_H };
    static uint32_t px[W * H];
    for (int c = 0; c < 224; c++)
        for (int y = 0; y < HOSTFONT_H; y++)
            for (int x = 0; x < HOSTFONT_W; x++)
                px[((c / 16) * HOSTFONT_H + y) * W + (c % 16) * HOSTFONT_W + x] =
                    (hostfont[c][y] & (0x80 >> x)) ? 0xFFFFFFFFu : 0;
    atlas = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, W, H);
    if (!atlas) return;
    SDL_UpdateTexture(atlas, NULL, px, W * 4);
    SDL_SetTextureBlendMode(atlas, SDL_BLENDMODE_BLEND);
}

/* UTF-8 (save state names) to Mac Roman, the font's encoding: ASCII, else '?' */
static int next_char(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    if (*p < 0x80) { (*s)++; return *p; }
    int n = *p >= 0xF0 ? 4 : *p >= 0xE0 ? 3 : 2;
    while (n-- && **s) (*s)++;
    return '?';
}
static int text_width(const char *t, int sc) { int n = 0; while (*t) { next_char(&t); n++; } return n * HOSTFONT_W * sc; }

static void text(SDL_Renderer *r, int x, int y, int sc, uint32_t rgb, const char *t) {
    SDL_SetTextureColorMod(atlas, rgb >> 16, rgb >> 8 & 0xFF, rgb & 0xFF);
    while (*t) {
        int c = next_char(&t);
        if (c < 0x20) c = '?';
        c -= 0x20;
        SDL_Rect src = { (c % 16) * HOSTFONT_W, (c / 16) * HOSTFONT_H, HOSTFONT_W, HOSTFONT_H };
        SDL_Rect dst = { x, y, HOSTFONT_W * sc, HOSTFONT_H * sc };
        SDL_RenderCopy(r, atlas, &src, &dst);
        x += HOSTFONT_W * sc;
    }
}
static void box(SDL_Renderer *r, int x, int y, int w, int h, uint32_t argb) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, argb >> 16 & 0xFF, argb >> 8 & 0xFF, argb & 0xFF, argb >> 24);
    SDL_Rect rc = { x, y, w, h };
    SDL_RenderFillRect(r, &rc);
}

/* ---- messages and the rewind indicator ---- */
static char toast_text[64];
static uint32_t toast_until;
static int rewinding;

void platform_toast(const char *t) {
    if (platform_is_headless()) return;
    snprintf(toast_text, sizeof toast_text, "%s", t);
    toast_until = SDL_GetTicks() + 1500;
}
void platform_rewinding(int on) { rewinding = on; }
int  overlay_visible(void) { return rewinding || (toast_until && !SDL_TICKS_PASSED(SDL_GetTicks(), toast_until)); }

void overlay_draw(void) {
    SDL_Renderer *r = platform_renderer();
    if (!r) return;
    make_atlas(r);
    if (!atlas) return;
    uint32_t now = SDL_GetTicks();
    if (toast_until && !SDL_TICKS_PASSED(now, toast_until)) {
        int w = text_width(toast_text, 2) + 24;
        int a = toast_until - now < 300 ? (int)(toast_until - now) * 200 / 300 : 200;   /* fades out */
        box(r, (640 - w) / 2, 26, w, 36, (uint32_t)a << 24 | 0x101018);
        text(r, (640 - w) / 2 + 12, 32, 2, 0xF0E0B0, toast_text);
    } else toast_until = 0;
    if (rewinding && (now / 400) % 2 == 0) {
        box(r, 16, 26, 132, 36, 0xB0101018);
        text(r, 28, 32, 2, 0xFFFFFF, "<< Rewind");
    }
}

static int menu_shooting;
static void draw_menu_body(void);
/* debug (POP2_OVERLAY_SHOTS=dir): what is on the screen, as dir/<name>.ppm */
void platform_overlay_shot(const char *name) {
    const char *dir = getenv("POP2_OVERLAY_SHOTS");
    SDL_Renderer *r = platform_renderer();
    if (!dir || !r) return;
    platform_render_game();
    if (!menu_shooting) overlay_draw(); else draw_menu_body();
    int w, h;
    SDL_GetRendererOutputSize(r, &w, &h);
    uint8_t *px = malloc((size_t)w * h * 3);
    if (px && SDL_RenderReadPixels(r, NULL, SDL_PIXELFORMAT_RGB24, px, w * 3) == 0) {
        char path[1024]; snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
        FILE *f = fopen(path, "wb");
        if (f) { fprintf(f, "P6\n%d %d\n255\n", w, h); fwrite(px, 1, (size_t)w * h * 3, f); fclose(f); }
    }
    free(px);
}

/* ---- the menu ---- */
enum { PG_MAIN, PG_STATES, PG_LEVELS, PG_STATS };
enum { IT_RESUME, IT_SAVE, IT_LOAD, IT_LEVEL, IT_TIME, IT_HEALTH, IT_INFHEALTH, IT_INFTIME, IT_CHEAT,
       IT_REWIND, IT_DISPLAY, IT_MENUBAR, IT_STATS, IT_QUIT, IT_COUNT };
static const char *const item_names[IT_COUNT] = {
    "Resume", "Save State", "Load State...", "Go to Level...", "Add 5 Minutes", "Add One Health",
    "Infinite Health", "Infinite Time", "Cheat Mode", "Rewind", "Display", "Mac Menu Bar", "Statistics...", "Quit Game" };
static const char *const display_names[DISPLAY_COUNT] = { "Pixels", "Sharp", "Smooth", "CRT" };

static int menu_wanted, page, sel[4], top[4], confirm_delete = -1, quit_after;
static SDL_Texture *thumbs[STATES_MAX];

void overlay_menu_request(void) { if (!platform_is_headless()) menu_wanted = 1; }
int  overlay_menu_pending(void) { return menu_wanted; }

static int item_enabled(int it) {
    switch (it) {
        case IT_SAVE: case IT_LEVEL: case IT_TIME: case IT_HEALTH: return mods_playing();
        case IT_LOAD: return states_count() > 0;
    }
    return 1;
}
static const char *item_value(int it) {
    switch (it) {
        case IT_INFHEALTH: return mods_get(MOD_INFINITE_HEALTH) ? "On" : "Off";
        case IT_INFTIME: return mods_get(MOD_INFINITE_TIME) ? "On" : "Off";
        case IT_CHEAT: return mods_get(MOD_CHEAT_MODE) ? "On" : "Off";
        case IT_REWIND: return mods_get(MOD_REWIND) ? "On" : "Off";
        case IT_DISPLAY: return display_names[platform_display_mode()];
        case IT_MENUBAR: return platform_menubar_hidden() ? "Hidden" : "Shown";
    }
    return NULL;
}
static int page_count(void) {
    switch (page) {
        case PG_MAIN: return IT_COUNT;
        case PG_STATES: return states_count();
        case PG_LEVELS: return 14;
    }
    return 0;
}

static void free_thumbs(void) { for (int i = 0; i < STATES_MAX; i++) if (thumbs[i]) { SDL_DestroyTexture(thumbs[i]); thumbs[i] = NULL; } }
static void load_thumbs(SDL_Renderer *r) {
    free_thumbs();
    static uint8_t px[STATE_THUMB_W * STATE_THUMB_H * 4];
    for (int i = 0; i < states_count() && i < STATES_MAX; i++) {
        if (!states_thumb(i, px)) continue;
        thumbs[i] = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, STATE_THUMB_W, STATE_THUMB_H);
        if (thumbs[i]) SDL_UpdateTexture(thumbs[i], NULL, px, STATE_THUMB_W * 4);
    }
}

/* returns 1 to close the menu */
static int activate(int dir) {   /* dir: 0 = select, -1 / +1 = left / right on a value */
    SDL_Renderer *r = platform_renderer();
    if (page == PG_MAIN) {
        int it = sel[PG_MAIN];
        if (!item_enabled(it)) return 0;
        switch (it) {
            case IT_RESUME: return dir == 0;
            case IT_SAVE: if (dir) return 0; mods_quick_save(); return 1;
            case IT_LOAD: if (dir) return 0; page = PG_STATES; sel[PG_STATES] = top[PG_STATES] = 0; load_thumbs(r); return 0;
            case IT_LEVEL: if (dir) return 0; page = PG_LEVELS; sel[PG_LEVELS] = mods_current_level() > 0 ? mods_current_level() - 1 : 0; return 0;
            case IT_TIME: if (dir) return 0; mods_add_time(); return 1;
            case IT_HEALTH: if (dir) return 0; mods_add_health(); return 1;
            case IT_INFHEALTH: mods_set(MOD_INFINITE_HEALTH, !mods_get(MOD_INFINITE_HEALTH)); return 0;
            case IT_INFTIME: mods_set(MOD_INFINITE_TIME, !mods_get(MOD_INFINITE_TIME)); return 0;
            case IT_CHEAT: mods_set(MOD_CHEAT_MODE, !mods_get(MOD_CHEAT_MODE)); return 0;
            case IT_REWIND: mods_set(MOD_REWIND, !mods_get(MOD_REWIND)); return 0;
            case IT_DISPLAY: platform_set_display_mode((platform_display_mode() + (dir < 0 ? DISPLAY_COUNT - 1 : 1)) % DISPLAY_COUNT); return 0;
            case IT_MENUBAR: platform_set_menubar_hidden(!platform_menubar_hidden()); return 0;
            case IT_STATS: if (dir) return 0; page = PG_STATS; return 0;
            case IT_QUIT: if (dir) return 0; quit_after = 1; return 1;
        }
    } else if (page == PG_STATES && dir == 0 && states_count() > 0) {
        states_request_load(sel[PG_STATES]);
        return 1;
    } else if (page == PG_LEVELS && dir == 0) {
        mods_goto_level(sel[PG_LEVELS] + 1);
        return 1;
    }
    return 0;
}

static void back(void) { if (page == PG_STATES) free_thumbs(); page = PG_MAIN; confirm_delete = -1; }

static void delete_selected(void) {
    if (page != PG_STATES || states_count() == 0) return;
    int i = sel[PG_STATES];
    if (confirm_delete != i) { confirm_delete = i; return; }
    states_delete(i);
    confirm_delete = -1;
    if (sel[PG_STATES] >= states_count()) sel[PG_STATES] = states_count() - 1;
    if (sel[PG_STATES] < 0) sel[PG_STATES] = 0;
    load_thumbs(platform_renderer());
    if (states_count() == 0) back();
}

static void fmt_time(char *out, size_t n, int secs) {
    if (secs <= 0) snprintf(out, n, "-");
    else snprintf(out, n, "%d:%02d", secs / 60, secs % 60);
}

static void draw_menu_body(void) {
    SDL_Renderer *r = platform_renderer();
    make_atlas(r);
    box(r, 0, 0, 640, 480, 0x90000000);                         /* dim the game */
    const int px = 70, pw = 500, py = 24, ph = 432;
    box(r, px, py, pw, ph, 0xE8181620);
    box(r, px, py, pw, 2, 0xFFC8A050);
    const uint32_t gold = 0xE8C070, white = 0xF0F0F0, grey = 0x707070, dim = 0xA0A0A0;
    static const char *const titles[] = { "Extras", "Load State", "Go to Level", "Statistics" };
    text(r, px + 16, py + 10, 2, gold, titles[page]);
    int n = page_count(), *s = &sel[page], *t = &top[page];
    if (page == PG_MAIN || page == PG_LEVELS) {
        const int rowh = 26, rows = 14;
        if (*s < *t) *t = *s;
        if (*s >= *t + rows) *t = *s - rows + 1;
        for (int k = *t; k < n && k < *t + rows; k++) {
            int y = py + 46 + (k - *t) * rowh;
            int en = page == PG_MAIN ? item_enabled(k) : mods_playing();
            if (k == *s) box(r, px + 8, y - 2, pw - 16, rowh - 2, 0xFF3A3448);
            char lv[24]; snprintf(lv, sizeof lv, "Level %d%s", k + 1, mods_current_level() == k + 1 ? "  (now)" : "");
            text(r, px + 20, y, 2, en ? (k == *s ? white : dim) : grey, page == PG_MAIN ? item_names[k] : lv);
            const char *v = page == PG_MAIN ? item_value(k) : NULL;
            if (v) {
                char b[24]; snprintf(b, sizeof b, k == *s ? "< %s >" : "%s", v);
                text(r, px + pw - 20 - text_width(b, 2), y, 2, k == *s ? gold : dim, b);
            }
        }
    } else if (page == PG_STATES) {
        const int rowh = 80, rows = 4;
        if (*s < *t) *t = *s;
        if (*s >= *t + rows) *t = *s - rows + 1;
        for (int k = *t; k < n && k < *t + rows; k++) {
            int y = py + 48 + (k - *t) * rowh;
            StateInfo si;
            if (!states_info(k, &si)) continue;
            if (k == *s) box(r, px + 8, y - 4, pw - 16, rowh - 4, 0xFF3A3448);
            SDL_Rect d = { px + 16, y, 96, 72 };
            if (thumbs[k]) SDL_RenderCopy(r, thumbs[k], NULL, &d);
            char name[64];
            if (si.name[0]) snprintf(name, sizeof name, "%s", si.name); else snprintf(name, sizeof name, "Level %d", si.level);
            text(r, px + 124, y + 4, 2, k == *s ? white : dim, name);
            char det[64]; snprintf(det, sizeof det, "Level %d, %d minute%s left", si.level, si.minutes, si.minutes == 1 ? "" : "s");
            text(r, px + 124, y + 34, 1, dim, det);
            char when[48]; time_t tt = (time_t)si.time; struct tm tmv; localtime_r(&tt, &tmv);
            strftime(when, sizeof when, "%d %b %Y, %H:%M", &tmv);
            text(r, px + 124, y + 50, 1, dim, when);
            if (confirm_delete == k) text(r, px + 124, y + 64, 1, 0xFF8060, "Press Y (or Delete) again to delete");
        }
    } else {   /* statistics */
        int y = py + 50;
        text(r, px + 20, y, 1, gold, "Level   Best clean run   Completed   Deaths   This session");
        y += 20;
        int total_deaths = 0;
        for (int l = 1; l <= 14; l++, y += 17) {
            LevelStats st; mods_level_stats(l, &st);
            total_deaths += st.deaths;
            char best[16], ses[16], line[96];
            fmt_time(best, sizeof best, st.best);
            fmt_time(ses, sizeof ses, st.session_time);
            snprintf(line, sizeof line, "%5d   %14s   %9d   %6d   %s, %d death%s", l, best, st.completions, st.deaths,
                     ses, st.session_deaths, st.session_deaths == 1 ? "" : "s");
            text(r, px + 20, y, 1, mods_current_level() == l ? white : dim, line);
        }
        char tot[64]; snprintf(tot, sizeof tot, "Deaths in all: %d", total_deaths);
        text(r, px + 20, y + 8, 1, dim, tot);
        text(r, px + 20, y + 26, 1, dim, mods_playing() ? (mods_attempt_clean() ? "This level so far: a clean run (no extras used)"
                                                                               : "This level so far: extras used, not timed")
                                                       : "Clean runs: no cheat, save state, rewind or added time/health");
    }
    const char *hint = page == PG_STATES ? "A load    Y delete    B back    Start close"
                     : page == PG_MAIN ? "A select    Left/Right change    B / Start close"
                     : page == PG_LEVELS ? "A go    B back" : "B back";
    text(r, px + (pw - text_width(hint, 1)) / 2, py + ph - 20, 1, grey, hint);
}
static void draw_menu(void) {
    platform_render_game();
    draw_menu_body();
    SDL_RenderPresent(platform_renderer());
}

/* The menu, until it is closed. Input goes to the menu only. */
void overlay_menu_run(void) {
    menu_wanted = 0;
    if (platform_is_headless() || !platform_renderer()) return;
    page = PG_MAIN; sel[PG_MAIN] = 0; top[PG_MAIN] = 0; confirm_delete = -1; quit_after = 0;
    if (getenv("POP2_OVERLAY_SHOTS")) {               /* debug: every page, then close */
        menu_shooting = 1;
        sel[PG_MAIN] = IT_INFHEALTH; platform_overlay_shot("menu_main");
        page = PG_STATES; sel[PG_STATES] = top[PG_STATES] = 0; load_thumbs(platform_renderer()); platform_overlay_shot("menu_states");
        confirm_delete = 0; platform_overlay_shot("menu_states_delete"); confirm_delete = -1; free_thumbs();
        page = PG_LEVELS; sel[PG_LEVELS] = 6; platform_overlay_shot("menu_levels");
        page = PG_STATS; platform_overlay_shot("menu_stats");
        menu_shooting = 0; page = PG_MAIN;
        return;
    }
    int open = 1, stick_dir = 0, held = 0;
    uint32_t next_repeat = 0;
    while (open) {
        draw_menu();
        SDL_Event e;
        int move = 0, act = 2;                                   /* act 2: nothing */
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_QUIT: quit_after = 1; open = 0; break;
                case SDL_KEYDOWN:
                    switch (e.key.keysym.sym) {
                        case SDLK_UP: move = -1; break;
                        case SDLK_DOWN: move = 1; break;
                        case SDLK_LEFT: act = -1; break;
                        case SDLK_RIGHT: act = 1; break;
                        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: act = 0; break;
                        case SDLK_ESCAPE: case SDLK_BACKSPACE: if (page == PG_MAIN) open = 0; else back(); break;
                        case SDLK_F1: open = 0; break;
                        case SDLK_DELETE: delete_selected(); break;
                    }
                    break;
                case SDL_CONTROLLERBUTTONDOWN:
                    switch (e.cbutton.button) {
                        case SDL_CONTROLLER_BUTTON_DPAD_UP: move = -1; held = -1; next_repeat = SDL_GetTicks() + 400; break;
                        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: move = 1; held = 1; next_repeat = SDL_GetTicks() + 400; break;
                        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: act = -1; break;
                        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: act = 1; break;
                        case SDL_CONTROLLER_BUTTON_A: act = 0; break;
                        case SDL_CONTROLLER_BUTTON_B: if (page == PG_MAIN) open = 0; else back(); break;
                        case SDL_CONTROLLER_BUTTON_Y: delete_selected(); break;
                        case SDL_CONTROLLER_BUTTON_START: case SDL_CONTROLLER_BUTTON_GUIDE: open = 0; break;
                    }
                    break;
                case SDL_CONTROLLERBUTTONUP:
                    if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP || e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) held = 0;
                    break;
                case SDL_CONTROLLERAXISMOTION:
                    if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                        int d = e.caxis.value < -16000 ? -1 : e.caxis.value > 16000 ? 1 : 0;
                        if (d != stick_dir) { stick_dir = d; held = d; if (d) { move = d; next_repeat = SDL_GetTicks() + 400; } }
                    } else if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
                        static int xdir;
                        int d = e.caxis.value < -16000 ? -1 : e.caxis.value > 16000 ? 1 : 0;
                        if (d != xdir) { xdir = d; if (d) act = d; }
                    }
                    break;
                case SDL_CONTROLLERDEVICEADDED: case SDL_CONTROLLERDEVICEREMOVED:
                    input_controller_event(&e);
                    break;
            }
        }
        if (!move && held && SDL_TICKS_PASSED(SDL_GetTicks(), next_repeat)) { move = held; next_repeat = SDL_GetTicks() + 110; }
        int n = page_count();
        if (move && n > 0 && page != PG_STATS) {
            sel[page] = (sel[page] + move + n) % n;
            confirm_delete = -1;
        }
        if (act != 2 && open && activate(act)) open = 0;
        SDL_Delay(16);
    }
    free_thumbs();
    page = PG_MAIN;
    input_reset();                                    /* buttons pressed in the menu do nothing in the game */
    event_release_all();
    if (quit_after) { SDL_Event q = { .type = SDL_QUIT }; SDL_PushEvent(&q); }
}
