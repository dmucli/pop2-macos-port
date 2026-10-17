/* Extras (mods.c): cheats, save states and rewind; usable from Objective-C too. */
#ifndef POP2_MODS_H
#define POP2_MODS_H
enum { MOD_CHEAT_MODE, MOD_INFINITE_TIME, MOD_INFINITE_HEALTH, MOD_REWIND, MOD_SKIP_COPYPROT, MOD_COUNT };
int  mods_get(int m);
void mods_set(int m, int v);
void mods_next_level(void);
void mods_goto_level(int level);   /* 1-14 */
void mods_add_time(void);          /* 5 more minutes */
void mods_add_health(void);        /* one more life point (a flask when full) */
int  mods_current_level(void);    /* 0 when not playing */
void mods_tick(void);
void mods_frame(void);
void mods_level_start(void);
void mods_level_end(void);
void mods_set_rewind_held(int held);
void mods_quick_save(void);         /* a new save state in the history (states.h) */
void mods_quick_load(void);         /* the newest save state */
int  mods_playing(void);            /* a level is being played (not the attract mode's) */
int  mods_in_level(void);           /* a level's frame loop is running, attract mode included */

/* statistics, per level (1-14): the best time of a clean run (no extras, save state, rewind
 * or cheat used since the level started), deaths and completions, all time (kept in the
 * settings) and in this session; times in seconds of the game's clock */
typedef struct { int best, deaths, completions, session_time, session_deaths; } LevelStats;
void mods_level_stats(int level, LevelStats *out);
int  mods_attempt_clean(void);      /* the level being played is a clean run so far */
void mods_assisted(void);           /* an extra was used: not a clean run */

/* host messages over the picture (platform overlay; nothing when headless) */
void platform_toast(const char *text);
void platform_rewinding(int on);
#endif
