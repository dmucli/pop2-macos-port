/* Save state history (states.c): the last STATES_MAX save states, on disk, newest first;
 * usable from Objective-C too. */
#ifndef POP2_STATES_H
#define POP2_STATES_H
#include <stdint.h>

#define STATES_MAX   10
#define STATE_THUMB_W 160                 /* thumbnail of the screen, RGBA */
#define STATE_THUMB_H 120

typedef struct {
    char name[64];                        /* UTF-8; empty = not named */
    int64_t time;                         /* when saved, Unix time */
    int level, minutes;                   /* level, minutes left */
} StateInfo;

int  states_count(void);
int  states_info(int i, StateInfo *out);
int  states_thumb(int i, uint8_t *rgba);   /* STATE_THUMB_W x STATE_THUMB_H x 4 bytes */
void states_rename(int i, const char *name);
void states_delete(int i);
int  states_version(void);                /* changes whenever the list changes */
/* Loads a state at the next frame of play; outside a level the game's New Game command
 * starts one first. */
void states_request_load(int i);
int  states_load_waiting(void);           /* a load waits for play to start */

/* mods.c, at the start of a frame */
void states_save_now(int level, int minutes);
int  states_load_pending(void);
int  states_load_now(void);               /* 0: refused (incompatible state) */
#endif
