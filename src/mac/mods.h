/* Extras (mods.c): cheats, save states and rewind; usable from Objective-C too. */
#ifndef POP2_MODS_H
#define POP2_MODS_H
enum { MOD_CHEAT_MODE, MOD_INFINITE_TIME, MOD_INFINITE_HEALTH, MOD_REWIND, MOD_REMEMBER_COPYPROT, MOD_COUNT };
int  mods_get(int m);
void mods_set(int m, int v);
void mods_next_level(void);
void mods_tick(void);
void mods_frame(void);
void mods_level_start(void);
void mods_set_rewind_held(int held);
void mods_quick_save(int n);
void mods_quick_load(int n);
int  mods_slot_used(int n);
int  mods_playing(void);
#endif
