/* Platform layer: shared declarations between the SDL code, input and the Cocoa glue. */
#ifndef POP2_PLATFORM_H
#define POP2_PLATFORM_H
#include <stdint.h>

/* display modes (View menu) */
enum { DISPLAY_PIXELS, DISPLAY_SHARP, DISPLAY_SMOOTH, DISPLAY_CRT, DISPLAY_COUNT };
int  platform_display_mode(void);
void platform_set_display_mode(int m);

/* input.c: key remapping and game controllers */
enum { ACT_UP, ACT_DOWN, ACT_LEFT, ACT_RIGHT, ACT_BUTTON1, ACT_BUTTON2, ACT_PAUSE, ACT_TIME, ACT_REWIND,
       ACT_QSAVE, ACT_QLOAD, ACT_COUNT };
const char *input_action_name(int a);
int  input_action_key(int a);            /* Mac key code chosen for the action, -1 = none */
int  input_action_default_key(int a);
void input_set_action_key(int a, int keycode);
const char *input_key_name(int keycode);
void input_load_keys(void);
int  input_key(int down, int keycode, uint16_t mods, int repeat);   /* 1 = handled */
union SDL_Event;
void input_controller_event(const union SDL_Event *e);
/* controller inputs: SDL game controller buttons, then PAD_AXIS + axis * 2 + positive */
enum { PAD_AXIS = 32, PAD_CODES = PAD_AXIS + 12 };
int  input_action_pad(int a);             /* controller input chosen for the action, -1 = none */
int  input_action_default_pad(int a);
void input_set_action_pad(int a, int code);
const char *input_pad_code_name(int code);
const char *input_pad_name(void);         /* connected controller, NULL = none */
int  input_pad_count(void);
int  input_pad_generation(void);          /* changes when a controller is connected or removed */
void input_pad_capture(int on);           /* Controls window open / closed */
void input_load_es_input(const char *path);  /* controller mappings from Recalbox's es_input.cfg */
int  input_pad_capture_poll(void);        /* while open: input just pressed, -1 = none */

/* macos_app.m */
int  macos_pref_int(const char *key, int def);
void macos_pref_set_int(const char *key, int v);
void macos_beep(void);
#endif
