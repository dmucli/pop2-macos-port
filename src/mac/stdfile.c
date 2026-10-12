/* Standard File (Pack3) and the synthetic System resources. */
#include "mac.h"
#include "../fs/vfs.h"
#include "../cpu/trapinfo.h"
#include <stdio.h>
#include <stdlib.h>

int  platform_is_headless(void);
int  macos_choose_file(const char *prompt, const char *const *names, int n);
int  macos_ask_save_name(const char *prompt, const char *def, char *out, int outsz, int (*exists)(const char *));
void event_release_all(void);

/* Interactive runs show native dialogs; headless and scripted runs choose by themselves. */
static int interactive(void) { return !platform_is_headless() && !getenv("POP2_INPUT"); }
static uint32_t put_dir;
static int name_exists(const char *name) { VNode *n = vfs_lookup(put_dir, name); return n && !n->isdir; }

/* SFReply: good(1) copy(1) fType(4) vRefNum(2) version(2) fName(Str63) */
static void reply_for(maddr reply, VNode *n, uint32_t dir, const char *name) {
    mset(reply, 0, 74);
    wr8(reply, 1);
    if (n) mput(reply + 2, n->finfo, 4);
    wr16(reply + 6, (uint16_t)fm_open_wd(dir, 0));
    c_to_pstr(name, reply + 10);
}

void stdfile_dispatch(uint16_t sel) {
    switch (sel) {
        case 1: {   /* SFPutFile(where, prompt, origName, dlgHook, reply) */
            maddr reply = pop32(); pop32(); maddr orig = pop32(); maddr prompt = pop32(); pop32();
            char name[64], pr[256]; pstr_to_c(orig, name, sizeof name); pstr_to_c(prompt, pr, sizeof pr);
            uint32_t dir = fm_default_dir();
            if (interactive()) {
                char chosen[64];
                put_dir = dir;
                int ok = macos_ask_save_name(pr, name, chosen, sizeof chosen, name_exists);
                event_release_all();
                if (!ok) { mset(reply, 0, 74); fprintf(stderr, "[stdfile] SFPutFile cancelled\n"); break; }
                snprintf(name, sizeof name, "%s", chosen);
            }
            fprintf(stderr, "[stdfile] SFPutFile \"%s\" -> %s\n", pr, name);
            reply_for(reply, vfs_lookup(dir, name), dir, name);
            break;
        }
        case 2: {   /* SFGetFile(where, prompt, fileFilter, numTypes, typeList, dlgHook, reply) */
            maddr reply = pop32(); pop32(); maddr types = pop32(); int16_t nt = pops16(); pop32(); pop32(); pop32();
            uint32_t dir = fm_default_dir();
            VNode *best = NULL;
            const char *want = getenv("POP2_OPENFILE");
            if (want) {
                best = vfs_lookup(dir, want);
                if (best) { fprintf(stderr, "[stdfile] SFGetFile -> %s (POP2_OPENFILE)\n", best->name); reply_for(reply, best, dir, best->name); break; }
            }
            VNode *cand[256]; int nc = 0;
            for (int i = 1; ; i++) {
                VNode *n = vfs_child_index(dir, i);
                if (!n) break;
                if (n->isdir) continue;
                int ok = nt <= 0;
                for (int k = 0; k < nt && !ok; k++) if (be32(n->finfo) == rd32(types + 4 * k)) ok = 1;
                if (ok && (!best || n->mddate > best->mddate)) best = n;
                if (ok && nc < 256) cand[nc++] = n;
            }
            if (interactive()) {
                for (int i = 1; i < nc; i++)           /* newest first */
                    for (int j = i; j > 0 && cand[j]->mddate > cand[j - 1]->mddate; j--) { VNode *t = cand[j]; cand[j] = cand[j - 1]; cand[j - 1] = t; }
                const char *names[256];
                for (int i = 0; i < nc; i++) names[i] = cand[i]->name;
                int k = macos_choose_file("Open Game", names, nc);
                event_release_all();
                best = k >= 0 ? cand[k] : NULL;
                if (!best) { mset(reply, 0, 74); fprintf(stderr, "[stdfile] SFGetFile cancelled\n"); break; }
            }
            if (best) { fprintf(stderr, "[stdfile] SFGetFile -> %s\n", best->name); reply_for(reply, best, dir, best->name); }
            else { mset(reply, 0, 74); fprintf(stderr, "[stdfile] SFGetFile: nothing to open\n"); }
            break;
        }
        default:
            if (!dispatch_default(0xA9EA, sel)) fprintf(stderr, "[stdfile] selector %d unsupported\n", sel);
    }
}

static void missing(uint32_t type, int16_t id) {
    static int n;
    if (n++ < 200) fprintf(stderr, "[rsrc] missing '%c%c%c%c' %d\n", type >> 24, type >> 16 & 0xFF, type >> 8 & 0xFF, type & 0xFF, id);
}

void runtime_system_resources(void) {
    /* empty resource fork: header + map with no types */
    static uint8_t sys[256 + 30];
    memset(sys, 0, sizeof sys);
    put32(sys, 256); put32(sys + 4, 256); put32(sys + 8, 0); put32(sys + 12, 30);
    memcpy(sys + 256, sys, 16);
    put16(sys + 256 + 24, 28); put16(sys + 256 + 26, 30); put16(sys + 256 + 28, 0xFFFF);
    rsrc_open_system(sys, sizeof sys);
    rsrc_missing_hook = missing;
}
