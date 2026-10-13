/* Extras (the "Extras" menu): cheats, level selection, quick save slots and rewind,
 * skipping the copy protection question.
 *
 * Everything here sits outside the ported game code and is inactive in deterministic
 * runs (tools/check.sh, tools/equiv.sh), so the verification of the port is unaffected.
 *
 * Save states are taken at the start of a game frame (the call of the one-frame function
 * from the level's frame loop): at that point the native call chain is always the same
 * within one level, so restoring the emulated memory, the CPU registers and the runtime's
 * host-side state brings the game back exactly. They are therefore only valid within the
 * level being played (a new level clears them). */
#include "mac.h"
#include "../cpu/cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern maddr g_a5;
extern int runtime_deterministic;
#include "../platform/platform.h"
void event_menu_command_key(uint8_t ch);
void event_clear_queue(void);
extern uint32_t qd_screen_dirty, qd_clut_version;

/* ---- game globals (A5-relative; see docs/notes/seg02b.md, seg04b.md) ---- */
#define A5(off)          (g_a5 + (uint32_t)(int32_t)(off))
#define G_CHEAT_MODE     A5(-0x2824)    /* word: the game's built-in cheat mode */
#define G_MINUTES_LEFT   A5(-0x4fce)    /* word */
#define G_FRAMES_LEFT    A5(-0x4fc6)    /* word: frames left in the current minute */
#define G_KID            A5(-0x50d2)    /* the kid's character record (0x3e bytes) */
#define G_CUR_CHAR       A5(-0x5056)    /* the character being animated */
#define CH_LIFE          0x1e           /* word: life points */
#define CH_MAXLIFE       0x20           /* word: maximum life points */
#define G_COPY_OK        A5(-0x56d8)    /* word: copy protection passed this session */

/* ---- settings ---- */
static const char *pref_name[MOD_COUNT] = {
    "Extras.CheatMode", "Extras.InfiniteTime", "Extras.InfiniteHealth", "Extras.Rewind",
    "Extras.SkipCopyProtection" };
static const int pref_default[MOD_COUNT] = { 0, 0, 0, 1, 1 };   /* rewind and skip copy protection on */
static int mod[MOD_COUNT], prefs_loaded;
static int cheat_forced;                 /* we set the game's cheat flag */
static int saved_minutes = -1;

static void load_prefs(void) {
    if (prefs_loaded) return;
    prefs_loaded = 1;
    for (int i = 0; i < MOD_COUNT; i++) mod[i] = macos_pref_int(pref_name[i], pref_default[i]);
}
static int active(void) {
    static int check = -1;
    if (check < 0) check = getenv("POP2_CHECK") != NULL;
    return !runtime_deterministic && !check && g_a5 && rd32(LM_CurrentA5);
}
int mods_get(int m) { load_prefs(); return m >= 0 && m < MOD_COUNT ? mod[m] : 0; }
static void ring_clear(void);
void mods_set(int m, int v) {
    load_prefs();
    if (m < 0 || m >= MOD_COUNT) return;
    mod[m] = v;
    macos_pref_set_int(pref_name[m], v);
    if (m == MOD_INFINITE_TIME) saved_minutes = -1;
    if (m == MOD_REWIND && !v) ring_clear();
    if (m == MOD_CHEAT_MODE && active()) {   /* takes effect at once */
        if (v) { wr16(G_CHEAT_MODE, 1); cheat_forced = 1; }
        else if (cheat_forced) { wr16(G_CHEAT_MODE, 0); cheat_forced = 0; }
    }
}

/* Next level: the game's own Command-L, which works on any level in cheat mode. */
void mods_next_level(void) {
    if (!active()) { macos_beep(); return; }
    if (!mod[MOD_CHEAT_MODE]) mods_set(MOD_CHEAT_MODE, 1);
    wr16(G_CHEAT_MODE, 1);
    event_menu_command_key('l');
}

/* Go to a level: the game's own mechanism (as Command-L), applied at the start of a frame. */
#define G_LEVEL          A5(-0x4fd2)    /* word: the level being played */
#define G_NEXT_LEVEL     A5(-0x4f92)    /* word: the level to go to (== level while playing it) */
uint32_t nat_ResetSoundItem(int16_t p4);
static int want_level;
void mods_goto_level(int level) {
    if (!mods_playing() || level < 1 || level > 14) { macos_beep(); return; }
    want_level = level;
}
int mods_current_level(void) { return mods_playing() ? (int16_t)rd16(G_LEVEL) : 0; }

/* ---- copy protection ----
 * CopyProtectionDialog asks only while its "passed" flag is clear (and sets it itself);
 * with the option on, the flag is set as soon as the game has initialised its globals. */
void mods_tick(void) {
    load_prefs();
    if (!mod[MOD_SKIP_COPYPROT] || !active()) return;
    extern uint64_t runtime_us(void);
    if (runtime_us() < 2000000) return;   /* after the THINK C start-up has set up the globals */
    if (rd16(G_COPY_OK) != 1) wr16(G_COPY_OK, 1);
}

/* ---- save states ---- */
#define PG   4096u
#define NPG  (MEM_SIZE / PG)
typedef struct Pg { int refs; uint8_t d[PG]; } Pg;
typedef struct Snap { int used; uint64_t level; Pg **pg; uint8_t *host; size_t host_len; } Snap;
#define RING 150                          /* frames of rewind (~15 s at the game's ~10 fps) */
static Snap ring[RING]; static int ring_head, ring_count;
static Snap slot[3];
static uint64_t level_id = 1;
static int rewind_held, want_save = -1, want_load = -1;

static void snap_free(Snap *s) {
    if (!s->used) return;
    for (unsigned p = 0; p < NPG; p++) if (s->pg[p] && --s->pg[p]->refs == 0) free(s->pg[p]);
    free(s->pg); free(s->host);
    memset(s, 0, sizeof *s);
}
static void ring_clear(void) { for (int i = 0; i < RING; i++) snap_free(&ring[i]); ring_head = ring_count = 0; }
static Snap *ring_newest(void) { return ring_count ? &ring[(ring_head + RING - 1) % RING] : NULL; }

/* host-side runtime state, in a fixed order */
void *mm_state(size_t *n); void *file_state(int i, size_t *n); void *loader_state(size_t *n);
void *native_state(size_t *n);
size_t time_state_size(void); void time_state_save(uint8_t *o); void time_state_load(const uint8_t *i);
size_t rsrc_state_size(void); void rsrc_state_save(uint8_t *o); int rsrc_state_load(const uint8_t *i);
static void *region(int i, size_t *n) {
    switch (i) {
        case 0: return mm_state(n);
        case 1: case 2: case 3: return file_state(i - 1, n);
        case 4: return loader_state(n);
        case 5: return native_state(n);
        default: *n = 0; return NULL;
    }
}
#define NREGIONS 6
#define NREGS 17                          /* D0-D7, A0-A7, SR */

static size_t host_size(void) {
    size_t len = NREGS * 4 + time_state_size() + rsrc_state_size();
    for (int i = 0; i < NREGIONS; i++) { size_t n; region(i, &n); len += n; }
    return len;
}
static void host_save(Snap *s) {
    s->host_len = host_size();
    s->host = malloc(s->host_len);
    uint8_t *o = s->host;
    for (int r = 0; r < 16; r++) { put32(o, cpu_get(r)); o += 4; }
    put32(o, cpu_get(R_SR)); o += 4;
    for (int i = 0; i < NREGIONS; i++) { size_t n; void *p = region(i, &n); memcpy(o, p, n); o += n; }
    time_state_save(o); o += time_state_size();
    rsrc_state_save(o);
}
static void host_load(const Snap *s) {
    const uint8_t *in = s->host;
    for (int r = 0; r < 16; r++) cpu_set(r, be32(in + 4 * r));
    cpu_set(R_SR, be32(in + 64));
    in += NREGS * 4;
    for (int i = 0; i < NREGIONS; i++) { size_t n; void *p = region(i, &n); memcpy(p, in, n); in += n; }
    time_state_load(in); in += time_state_size();
    rsrc_state_load(in);
}

static void snap_take(Snap *s, const Snap *base) {
    s->used = 1; s->level = level_id;
    s->pg = malloc(NPG * sizeof(Pg *));
    for (unsigned p = 0; p < NPG; p++) {
        const uint8_t *cur = M + (size_t)p * PG;
        if (base && base->pg[p] && !memcmp(base->pg[p]->d, cur, PG)) { s->pg[p] = base->pg[p]; s->pg[p]->refs++; continue; }
        Pg *n = malloc(sizeof(Pg)); n->refs = 1; memcpy(n->d, cur, PG); s->pg[p] = n;
    }
    host_save(s);
}
static int snap_restore(const Snap *s) {
    /* same level, same point of the frame loop, same set of resources: else refuse */
    if (!s->used || s->level != level_id) return 0;
    if (be32(s->host + 15 * 4) != cpu_get(R_SP) || s->host_len != host_size()) return 0;
    uint8_t keys[16];
    memcpy(keys, M + LM_KeyMap, 16);                    /* the keys held now stay held */
    for (unsigned p = 0; p < NPG; p++) memcpy(M + (size_t)p * PG, s->pg[p]->d, PG);
    memcpy(M + LM_KeyMap, keys, 16);
    host_load(s);
    event_clear_queue();
    qd_screen_dirty++; qd_clut_version++;               /* redraw with the restored colours */
    return 1;
}

void mods_set_rewind_held(int held) { rewind_held = held; }
void mods_quick_save(int n) { if (n >= 0 && n < 3) want_save = n; }
void mods_quick_load(int n) { if (n >= 0 && n < 3) want_load = n; }
int  mods_slot_used(int n) { return n >= 0 && n < 3 && slot[n].used && slot[n].level == level_id; }

/* the level's frame loop starts: save states from another level are no longer valid */
void mods_level_start(void) { level_id++; ring_clear(); saved_minutes = -1; }

/* a level is being played (its frame loop ran recently) */
static uint64_t last_frame_us;
int mods_playing(void) {
    extern uint64_t runtime_us(void);
    return active() && last_frame_us && runtime_us() - last_frame_us < 500000;
}

/* start of a game frame */
void mods_frame(void) {
    load_prefs();
    { static int once; if (!once++ && getenv("POP2_MODS_SELFTEST")) fprintf(stderr, "[mods] first frame, active %d\n", active()); }
    { extern uint64_t runtime_us(void); last_frame_us = runtime_us(); }
    if (!active()) return;
    if (mod[MOD_CHEAT_MODE] && rd16(G_CHEAT_MODE) == 0) { wr16(G_CHEAT_MODE, 1); cheat_forced = 1; }
    if (mod[MOD_INFINITE_TIME]) {
        int m = (int16_t)rd16(G_MINUTES_LEFT);
        if (saved_minutes < 0 || m > saved_minutes) saved_minutes = m;   /* (cheat keys add time) */
        else if (m < saved_minutes) { wr16(G_MINUTES_LEFT, (uint16_t)saved_minutes); wr16(G_FRAMES_LEFT, 0x2cf); }
    }
    if (mod[MOD_INFINITE_HEALTH]) {
        maddr kid = G_KID, cur = G_CUR_CHAR;
        int16_t max = (int16_t)rd16(kid + CH_MAXLIFE);
        if (max > 0 && (int16_t)rd16(kid + CH_LIFE) > 0 && (int16_t)rd16(kid + CH_LIFE) < max) wr16(kid + CH_LIFE, (uint16_t)max);
        if (rd16(cur) == rd16(kid) && (int16_t)rd16(cur + CH_LIFE) > 0 && (int16_t)rd16(cur + CH_LIFE) < max)
            wr16(cur + CH_LIFE, (uint16_t)max);
    }
    static int selftest = -1, frame_no;
    if (selftest < 0) selftest = getenv("POP2_MODS_SELFTEST") != NULL;
    if (selftest) {   /* debug: quick save, load, rewind, with checks */
        frame_no++;
        static uint32_t sum_at_save; static maddr kid_at_save;
        uint32_t sum = 0; for (unsigned i = 0; i < 0x40000; i++) sum = sum * 31 + M[0x100000 + i];
        if (*getenv("POP2_MODS_SELFTEST") == '3') {   /* level selector: visit every level */
            static int lv_frames, target = 1;
            if (++lv_frames == 30) {
                fprintf(stderr, "[mods] in level %d, going to %d\n", (int16_t)rd16(A5(-0x4fd2)), target);
                mods_goto_level(target++); lv_frames = -100000;
            }
            if (rd16(A5(-0x4fd2)) == target - 1 && lv_frames < 0) lv_frames = 0;
            goto selftest_done;
        }
        if (*getenv("POP2_MODS_SELFTEST") == '2') {   /* cheats */
            if (frame_no == 1) { mod[MOD_INFINITE_TIME] = mod[MOD_INFINITE_HEALTH] = 1; }
            if (frame_no % 40 == 0) fprintf(stderr, "[mods] frame %d level %d minutes %d frames %d life %d/%d\n", frame_no,
                (int16_t)rd16(A5(-0x4fd2)), (int16_t)rd16(G_MINUTES_LEFT), (int16_t)rd16(G_FRAMES_LEFT),
                (int16_t)rd16(G_KID + CH_LIFE), (int16_t)rd16(G_KID + CH_MAXLIFE));
            if (frame_no == 60) { fprintf(stderr, "[mods] next level\n"); mods_next_level(); }
            goto selftest_done;
        }
        if (frame_no == 30) { want_save = 0; sum_at_save = sum; kid_at_save = rd32(G_KID + 2); }
        if (frame_no == 90) want_load = 0;
        if (frame_no == 91) fprintf(stderr, "[mods] after load: globals %s, kid %s\n",
                                    sum == sum_at_save ? "same" : "DIFFERENT", rd32(G_KID + 2) == kid_at_save ? "same" : "DIFFERENT");
        if (frame_no == 150) rewind_held = 1;
        if (frame_no == 180) { rewind_held = 0; fprintf(stderr, "[mods] rewound, ring %d\n", ring_count); }
        if (frame_no % 50 == 0) fprintf(stderr, "[mods] frame %d level %d ring %d\n", frame_no, (int16_t)rd16(A5(-0x4fd2)), ring_count);
    }
    selftest_done:
    if (want_level) {
        int l = want_level; want_level = 0;
        if (rd16(G_NEXT_LEVEL) == rd16(G_LEVEL)) {   /* not already leaving the level */
            wr16(G_NEXT_LEVEL, (uint16_t)l);
            nat_ResetSoundItem(0);                    /* as Command-L: stop the level's sounds */
        }
        return;
    }
    if (want_load >= 0) {
        int n = want_load; want_load = -1;
        if (!snap_restore(&slot[n])) macos_beep();
        else ring_clear();
        return;
    }
    if (want_save >= 0) {
        int n = want_save; want_save = -1;
        snap_free(&slot[n]);
        snap_take(&slot[n], ring_newest());
    }
    if (rewind_held && mod[MOD_REWIND]) {
        if (ring_count <= 1) return;                     /* nothing older */
        ring_head = (ring_head + RING - 1) % RING; ring_count--;
        snap_free(&ring[ring_head]);                     /* drop the newest */
        if (!snap_restore(ring_newest())) { ring_clear(); macos_beep(); }
        return;
    }
    if (mod[MOD_REWIND]) {
        Snap *base = ring_newest();
        if (ring_count == RING) { snap_free(&ring[ring_head]); ring_count--; }
        Snap tmp = {0};
        snap_take(&tmp, base);
        ring[ring_head] = tmp;
        ring_head = (ring_head + 1) % RING; ring_count++;
    }
}
