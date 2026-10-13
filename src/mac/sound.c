/* Sound Manager 3 (subset) on SDL audio.
 * Main thread: command queues, 68k callbacks, double-buffer refills.
 * Audio thread: mixes host copies of sample data. */
#include "mac.h"
#include "trapnums.h"
#include "../cpu/trapinfo.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

void runtime_idle(void);
void platform_wait_ms(int ms);

#define OUT_RATE 44100
#define MAXCH 16
#define DB_FIFO 6

enum { nullCmd = 0, quietCmd = 3, flushCmd = 4, reInitCmd = 5, waitCmd = 10, pauseCmd = 11, resumeCmd = 12,
       callBackCmd = 13, syncCmd = 14, availableCmd = 24, versionCmd = 25, totalLoadCmd = 26, loadCmd = 27,
       freqDurationCmd = 40, restCmd = 41, freqCmd = 42, ampCmd = 43, timbreCmd = 44, getAmpCmd = 45,
       volumeCmd = 46, getVolumeCmd = 47, waveTableCmd = 60, soundCmd = 80, bufferCmd = 81, rateCmd = 82,
       continueCmd = 83, doubleBufferCmd = 84, getRateCmd = 85, rateMultiplierCmd = 86, getRateMultiplierCmd = 87 };

typedef struct {               /* host copy of a sampled sound */
    int16_t *pcm;              /* interleaved, 1 or 2 channels */
    uint32_t frames;
    int chans;
    double rate;               /* Hz */
    uint32_t loop_start, loop_end;
    int base_note;
} Sample;

typedef struct { uint16_t cmd, p1; uint32_t p2; } Cmd;

typedef struct {
    int used;
    maddr chan;                /* SndChannel record */
    maddr user_routine;
    Cmd *queue; int qn, qcap;
    /* playback (shared with the audio thread under the device lock) */
    Sample cur;                /* currently playing buffer */
    Sample installed;          /* sound installed by soundCmd (for freqCmd) */
    double pos, step;
    double db_carry;           /* fractional source position carried into the next double buffer */
    int playing, looping, done_flag;
    double rate_mult;
    int amp;                   /* 0..255 */
    uint16_t vol_l, vol_r;     /* 0x0100 = unity */
    int paused;
    uint32_t wait_until_ms;
    /* double buffering */
    maddr dbh;                 /* SndDoubleBufferHeader */
    int db_next;               /* which buffer to fetch next */
    int db_last_seen;          /* last buffer flagged dbLastBuffer was queued */
    Sample dbq[DB_FIFO]; int dbq_n;
    int busy_buffer;           /* waiting for a bufferCmd to finish */
} Chan;

static Chan ch[MAXCH];
static SDL_AudioDeviceID dev;
static uint16_t master_l = 0x100, master_r = 0x100;
uint32_t sound_frames_mixed;
static float sound_peak;

/* Sound timing follows the runtime clock (virtual in deterministic mode). */
uint64_t runtime_us(void);
extern int runtime_deterministic;
static uint32_t now_ms(void) { return (uint32_t)(runtime_us() / 1000u); }
/* One step of a synchronous wait: real sleep only when time is real. */
static void wait_slice(void) { if (!runtime_deterministic) platform_wait_ms(2); runtime_idle(); }

/* ------------------------------------------------------------ mixer -- */
static void free_sample(Sample *s) { free(s->pcm); memset(s, 0, sizeof *s); }

static uint32_t sound_starved;   /* output frames lost to double-buffer underruns */
static void audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    float *out = (float *)stream;
    int frames = len / (int)(sizeof(float) * 2);
    memset(out, 0, (size_t)len);
    for (int c = 0; c < MAXCH; c++) {
        Chan *k = &ch[c];
        if (!k->used || k->paused) continue;
        for (int f = 0; f < frames; f++) {
            if (!k->playing) {
                /* pull next double-buffer block */
                if (k->dbq_n > 0) {
                    free_sample(&k->cur);
                    k->cur = k->dbq[0];
                    memmove(&k->dbq[0], &k->dbq[1], sizeof(Sample) * (size_t)(k->dbq_n - 1));
                    k->dbq_n--;
                    memset(&k->dbq[k->dbq_n], 0, sizeof(Sample));
                    k->pos = k->db_carry; k->db_carry = 0;   /* continue the resampling phase across buffers */
                    k->step = k->cur.rate * k->rate_mult / OUT_RATE; k->playing = 1; k->looping = 0;
                } else { if (k->dbh) sound_starved += (uint32_t)(frames - f); break; }   /* underrun */
            }
            Sample *s = &k->cur;
            uint32_t i = (uint32_t)k->pos;
            if (i >= s->frames) {
                if (k->looping && s->loop_end > s->loop_start + 1) { k->pos = s->loop_start + (k->pos - s->loop_end); i = (uint32_t)k->pos; if (i >= s->frames) { k->playing = 0; k->done_flag = 1; continue; } }
                else { if (k->dbh) k->db_carry = k->pos - s->frames; k->playing = 0; k->done_flag = 1; f--; continue; }
            }
            if (k->looping && s->loop_end > s->loop_start + 1 && k->pos >= s->loop_end) { k->pos -= (s->loop_end - s->loop_start); i = (uint32_t)k->pos; }
            float l, r;
            if (s->chans == 2) { l = s->pcm[2 * i] / 32768.f; r = s->pcm[2 * i + 1] / 32768.f; }
            else { l = r = s->pcm[i] / 32768.f; }
            float a = k->amp / 255.f;
            out[2 * f]     += l * a * (k->vol_l / 256.f) * (master_l / 256.f) * 0.7f;
            out[2 * f + 1] += r * a * (k->vol_r / 256.f) * (master_r / 256.f) * 0.7f;
            k->pos += k->step;
        }
    }
    for (int i = 0; i < frames * 2; i++) { if (out[i] > 1) out[i] = 1; if (out[i] < -1) out[i] = -1; }
    sound_frames_mixed += (uint32_t)frames;
    for (int i = 0; i < frames * 2; i++) { float a = out[i] < 0 ? -out[i] : out[i]; if (a > sound_peak) sound_peak = a; }
}

int sound_init(void) {
    SDL_AudioSpec want = {0}, have;
    want.freq = OUT_RATE; want.format = AUDIO_F32SYS; want.channels = 2; want.samples = 1024; want.callback = audio_cb;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) { fprintf(stderr, "audio: %s\n", SDL_GetError()); return -1; }
    SDL_PauseAudioDevice(dev, 0);
    return 0;
}
static void lock(void) { if (dev) SDL_LockAudioDevice(dev); }
static void unlock(void) { if (dev) SDL_UnlockAudioDevice(dev); }

/* ------------------------------------------------- sample conversion -- */
static double fixed_rate(uint32_t f) { return f / 65536.0; }

/* Parse a SoundHeader / ExtSoundHeader at address h into a host sample. */
static int load_header(maddr h, Sample *s) {
    memset(s, 0, sizeof *s);
    uint8_t enc = rd8(h + 20);
    s->rate = fixed_rate(rd32(h + 8));
    if (s->rate < 1000) s->rate = 22254.5454;
    s->loop_start = rd32(h + 12); s->loop_end = rd32(h + 16);
    s->base_note = rd8(h + 21) ? rd8(h + 21) : 60;
    maddr data;
    int bits = 8;
    if (enc == 0) {              /* stdSH: 8-bit mono offset binary */
        s->chans = 1; s->frames = rd32(h + 4);
        data = rd32(h) ? rd32(h) : h + 22;
    } else if (enc == 0xFF) {    /* extSH */
        s->chans = (int)rd32(h + 4); if (s->chans < 1 || s->chans > 2) s->chans = 1;
        s->frames = rd32(h + 22);
        bits = rd16(h + 48);
        data = rd32(h) ? rd32(h) : h + 64;
    } else {
        fprintf(stderr, "[snd] compressed sound header (encode %d) unsupported\n", enc);
        return 0;
    }
    if (s->frames > 50000000u) return 0;
    s->pcm = malloc(sizeof(int16_t) * s->frames * s->chans + 2);
    uint32_t n = s->frames * (uint32_t)s->chans;
    if (bits == 16) for (uint32_t i = 0; i < n; i++) s->pcm[i] = (int16_t)rd16(data + 2 * i);
    else for (uint32_t i = 0; i < n; i++) s->pcm[i] = (int16_t)(((int)rd8(data + i) - 128) << 8);
    return 1;
}

/* ------------------------------------------------------- channels -- */
static Chan *chan_for(maddr chan) { for (int i = 0; i < MAXCH; i++) if (ch[i].used && ch[i].chan == (chan & ADDR_MASK)) return &ch[i]; return NULL; }

static void start_sample(Chan *k, Sample *s, int loop, double rate) {
    lock();
    free_sample(&k->cur);
    k->cur = *s; memset(s, 0, sizeof *s);
    k->pos = 0; k->step = rate * k->rate_mult / OUT_RATE;
    k->looping = loop; k->playing = 1; k->done_flag = 0;
    unlock();
}

static void call_user(Chan *k, Cmd c) {
    maddr routine = rd32(k->chan + 8);
    if (!routine) return;
    maddr cmdp = NewPtrClear(8);
    wr16(cmdp, c.cmd); wr16(cmdp + 2, c.p1); wr32(cmdp + 4, c.p2);
    uint32_t args[2] = { k->chan, cmdp };
    cpu_call_irq(routine, 0, 0, 0, 2, args);
    DisposePtr(cmdp);
}

/* Execute one command now. Returns 0 if the channel must wait (command keeps it busy). */
static int exec_cmd(Chan *k, Cmd c, int immediate) {
    switch (c.cmd) {
        case nullCmd: case syncCmd: case reInitCmd: case timbreCmd: case loadCmd: case continueCmd: return 1;
        case quietCmd: lock(); k->playing = 0; k->looping = 0; for (int i = 0; i < k->dbq_n; i++) free_sample(&k->dbq[i]); k->dbq_n = 0; unlock(); k->busy_buffer = 0; return 1;
        case flushCmd: k->qn = 0; return 1;
        case pauseCmd: k->paused = 1; return 1;
        case resumeCmd: k->paused = 0; return 1;
        case waitCmd: k->wait_until_ms = now_ms() + c.p1 / 2; return 0;
        case callBackCmd: call_user(k, c); return 1;
        case ampCmd: k->amp = c.p1 & 0xFF; return 1;
        case getAmpCmd: wr16(c.p2, (uint16_t)k->amp); return 1;
        case volumeCmd: k->vol_l = (uint16_t)(c.p2 & 0xFFFF); k->vol_r = (uint16_t)(c.p2 >> 16); return 1;
        case getVolumeCmd: wr32(c.p2, (uint32_t)k->vol_r << 16 | k->vol_l); return 1;
        case availableCmd: wr16(c.p2, 1); return 1;
        case versionCmd: wr32(c.p2, 0x00030000); return 1;
        case totalLoadCmd: wr16(c.p2, 0); return 1;
        case rateCmd: lock(); k->rate_mult = c.p2 / 65536.0; if (k->cur.rate > 0) k->step = k->cur.rate * k->rate_mult / OUT_RATE; unlock(); return 1;
        case rateMultiplierCmd: lock(); k->rate_mult = c.p2 / 65536.0; if (k->cur.rate > 0) k->step = k->cur.rate * k->rate_mult / OUT_RATE; unlock(); return 1;
        case getRateCmd: case getRateMultiplierCmd: wr32(c.p2, (uint32_t)(k->rate_mult * 65536)); return 1;
        case soundCmd: {
            Sample s; if (load_header(c.p2, &s)) { free_sample(&k->installed); k->installed = s; }
            return 1;
        }
        case bufferCmd: {
            Sample s; if (!load_header(c.p2, &s)) return 1;
            start_sample(k, &s, 0, s.rate);
            k->busy_buffer = 1;
            return immediate;
        }
        case freqCmd: case freqDurationCmd: {
            if (!k->installed.pcm) return 1;
            int note = (int)(c.p2 & 0xFF);
            double rate = k->installed.rate * pow(2.0, (note - k->installed.base_note) / 12.0);
            Sample s = k->installed;
            s.pcm = malloc(sizeof(int16_t) * s.frames * s.chans + 2);
            memcpy(s.pcm, k->installed.pcm, sizeof(int16_t) * s.frames * s.chans);
            start_sample(k, &s, s.loop_end > s.loop_start + 1, rate);
            if (c.cmd == freqDurationCmd && c.p1) { k->wait_until_ms = now_ms() + c.p1 / 2; return 0; }
            return 1;
        }
        case restCmd: k->wait_until_ms = now_ms() + c.p1 / 2; return 0;
        default:
            fprintf(stderr, "[snd] command %d unsupported\n", c.cmd);
            return 1;
    }
}

static void enqueue(Chan *k, Cmd c) {
    if (k->qn == k->qcap) { k->qcap = k->qcap ? k->qcap * 2 : 32; k->queue = realloc(k->queue, sizeof(Cmd) * (size_t)k->qcap); }
    k->queue[k->qn++] = c;
}

/* Fetch ready double buffers and ask the 68k code for more. */
static void service_double_buffer(Chan *k) {
    if (!k->dbh) return;
    maddr h = k->dbh;
    for (int guard = 0; guard < 4; guard++) {
        lock(); int queued = k->dbq_n + (k->playing ? 1 : 0); unlock();
        if (queued >= 3) break;
        maddr buf = rd32(h + 12 + 4 * k->db_next);
        uint32_t flags = rd32(buf + 4);
        if (!(flags & 1)) break;                       /* dbBufferReady */
        Sample s = {0};
        s.chans = rd16(h); if (s.chans < 1 || s.chans > 2) s.chans = 1;
        int bits = rd16(h + 2);
        s.rate = fixed_rate(rd32(h + 8));
        s.frames = rd32(buf);
        s.pcm = malloc(sizeof(int16_t) * s.frames * s.chans + 2);
        uint32_t n = s.frames * (uint32_t)s.chans;
        if (bits == 16) for (uint32_t i = 0; i < n; i++) s.pcm[i] = (int16_t)rd16(buf + 16 + 2 * i);
        else for (uint32_t i = 0; i < n; i++) s.pcm[i] = (int16_t)(((int)rd8(buf + 16 + i) - 128) << 8);
        lock();
        if (k->dbq_n < DB_FIFO) k->dbq[k->dbq_n++] = s; else free_sample(&s);
        unlock();
        wr32(buf + 4, flags & ~1u);
        if (flags & 4) { k->dbh = 0; break; }          /* dbLastBuffer */
        /* ask for a refill: doubleBack(chan, doubleBuffer) */
        maddr back = rd32(h + 20);
        if (back) {
            uint32_t args[2] = { k->chan, buf };
            cpu_call_irq(back, 0, 0, 0, 2, args);
        }
        k->db_next ^= 1;
    }
}

/* In deterministic mode there is no audio device: consume audio by virtual time. */
static void virtual_audio(void) {
    static uint64_t last; static float buf[2 * 4096];
    uint64_t now = runtime_us();
    if (!last) { last = now; return; }
    uint64_t frames = (now - last) * OUT_RATE / 1000000u;
    if (frames < 256) return;
    last += frames * 1000000u / OUT_RATE;
    while (frames) {
        int n = frames > 4096 ? 4096 : (int)frames;
        audio_cb(NULL, (Uint8 *)buf, n * (int)sizeof(float) * 2);
        frames -= (uint64_t)n;
    }
}

/* Main-thread service: run queues, callbacks, refills. */
void sound_service(void) {
    if (runtime_deterministic && !dev) virtual_audio();
    static int stats = -1; static uint32_t next;
    if (stats < 0) stats = getenv("POP2_AUDIOSTATS") != NULL;
    if (stats && now_ms() >= next) {
        next = now_ms() + 2000;
        int used = 0, dbn = 0, play = 0;
        for (int i = 0; i < MAXCH; i++) if (ch[i].used) { used++; dbn += ch[i].dbq_n; play += ch[i].playing; }
        extern uint32_t vbl_tick_count;
        fprintf(stderr, "[audio] chans=%d playing=%d dbq=%d mixed=%u peak=%.3f starved=%u vbl=%u\n", used, play, dbn, sound_frames_mixed, sound_peak, sound_starved, vbl_tick_count);
        sound_peak = 0;
    }
    for (int i = 0; i < MAXCH; i++) {
        Chan *k = &ch[i];
        if (!k->used) continue;
        if (k->dbh) service_double_buffer(k);
        if (k->busy_buffer) {
            lock(); int done = k->done_flag || !k->playing; unlock();
            if (!done) continue;
            k->busy_buffer = 0;
        }
        if (k->wait_until_ms) {
            if (now_ms() < k->wait_until_ms) continue;
            k->wait_until_ms = 0;
        }
        while (k->qn > 0 && !k->busy_buffer && !k->wait_until_ms && !k->paused) {
            Cmd c = k->queue[0];
            memmove(&k->queue[0], &k->queue[1], sizeof(Cmd) * (size_t)(k->qn - 1));
            k->qn--;
            exec_cmd(k, c, 0);
        }
    }
}

/* ------------------------------------------------------------ traps -- */
static Chan *new_chan(maddr *chanp_out, maddr chanpp, maddr user) {
    int slot = -1;
    for (int i = 0; i < MAXCH; i++) if (!ch[i].used) { slot = i; break; }
    if (slot < 0) return NULL;
    Chan *k = &ch[slot];
    memset(k, 0, sizeof *k);
    maddr chan = rd32(chanpp);
    if (!chan) { chan = NewPtrClear(36 + 8 * 128); wr16(chan + 30, 128); wr32(chanpp, chan); }
    k->used = 1; k->chan = chan & ADDR_MASK; k->amp = 255; k->vol_l = k->vol_r = 0x100; k->rate_mult = 1.0;
    wr32(chan + 8, user);
    *chanp_out = chan;
    return k;
}
static void T_SndNewChannel(uint16_t w) {
    (void)w; maddr user = pop32(); pop32(); pop16(); maddr chanpp = pop32();
    maddr chan; Chan *k = new_chan(&chan, chanpp, user);
    ret16(k ? 0 : (uint16_t)-205);   /* notEnoughHardwareErr */
}
static void T_SndDisposeChannel(uint16_t w) {
    (void)w; int quiet = pop8(); maddr chan = pop32();
    Chan *k = chan_for(chan);
    if (!k) { ret16((uint16_t)-205); return; }
    if (!quiet) {   /* wait for the queue to drain */
        uint32_t t0 = now_ms();
        while ((k->qn || k->busy_buffer || k->playing) && now_ms() - t0 < 5000 && cpu_running) { sound_service(); wait_slice(); }
    }
    lock();
    k->used = 0; free_sample(&k->cur); free_sample(&k->installed);
    for (int i = 0; i < k->dbq_n; i++) free_sample(&k->dbq[i]);
    unlock();
    free(k->queue);
    ret16(0);
}
static Cmd read_cmd(maddr p) { Cmd c = { rd16(p), rd16(p + 2), rd32(p + 4) }; return c; }
static void T_SndDoCommand(uint16_t w) {
    (void)w; pop8(); maddr cmdp = pop32(), chan = pop32();
    Chan *k = chan_for(chan);
    if (!k) { ret16((uint16_t)-205); return; }
    enqueue(k, read_cmd(cmdp));
    sound_service();
    ret16(0);
}
static void T_SndDoImmediate(uint16_t w) {
    (void)w; maddr cmdp = pop32(), chan = pop32();
    Chan *k = chan_for(chan);
    if (!k) { ret16((uint16_t)-205); return; }
    exec_cmd(k, read_cmd(cmdp), 1);
    ret16(0);
}
static void T_SndControl(uint16_t w) { (void)w; maddr cmdp = pop32(); pop16(); Cmd c = read_cmd(cmdp); if (c.cmd == availableCmd) wr16(cmdp + 2, 1); if (c.cmd == versionCmd) wr32(cmdp + 4, 0x00030000); ret16(0); }

/* SndPlay(chan, sndHdl, async) */
static void T_SndPlay(uint16_t w) {
    (void)w; int async = pop8(); maddr hdl = pop32(); maddr chan = pop32();
    if (!hdl || !rd32(hdl)) { ret16((uint16_t)-201); return; }
    HLock(hdl);
    maddr p = rd32(hdl);
    int fmt = rd16(p);
    maddr cmds;
    if (fmt == 1) { int nmods = rd16(p + 2); cmds = p + 4 + 6 * nmods; }
    else cmds = p + 4;
    int ncmd = rd16(cmds);
    maddr tmpchan = 0; Chan *k;
    if (!chan) {
        maddr pp = NewPtrClear(4);
        k = new_chan(&tmpchan, pp, 0);
        DisposePtr(pp);
        async = 0;
    } else k = chan_for(chan);
    if (!k) { ret16((uint16_t)-205); return; }
    for (int i = 0; i < ncmd; i++) {
        Cmd c = read_cmd(cmds + 2 + 8 * i);
        if (c.cmd & 0x8000) { c.cmd &= 0x7FFF; c.p2 += p; }
        if (c.cmd == soundCmd) c.cmd = bufferCmd;
        enqueue(k, c);
    }
    sound_service();
    if (!async) {
        uint32_t t0 = now_ms();
        while ((k->qn || k->busy_buffer) && now_ms() - t0 < 30000 && cpu_running) { sound_service(); wait_slice(); }
    }
    if (tmpchan) { lock(); k->used = 0; free_sample(&k->cur); free_sample(&k->installed); unlock(); free(k->queue); DisposePtr(tmpchan); }
    ret16(0);
}

static void T_SoundDispatch(uint16_t w) {
    uint32_t sel = D0;
    /* match on routine (bits 16-23) + group (low word); old callers omit the size byte */
    switch (sel & 0x00FFFFFF) {
        case 0x000C0008: ret32(0x03008000); return;                         /* SndSoundManagerVersion 3.0 */
        case 0x002C0018: { maddr p = pop32(); wr32(p, (uint32_t)master_r << 16 | master_l); ret16(0); return; }   /* GetDefaultOutputVolume */
        case 0x00300018: { uint32_t v = pop32(); master_l = (uint16_t)v; master_r = (uint16_t)(v >> 16); ret16(0); return; }  /* SetDefaultOutputVolume */
        case 0x00240018: { maddr p = pop32(); wr32(p, 0x01000100); ret16(0); return; }  /* GetSysBeepVolume */
        case 0x00280018: pop32(); ret16(0); return;
        case 0x00200008: {                                                  /* SndPlayDoubleBuffer(chan, header) */
            maddr hdr = pop32(), chan = pop32();
            Chan *k = chan_for(chan);
            if (!k) { ret16((uint16_t)-205); return; }
            k->dbh = hdr; k->db_next = 0;
            /* both buffers are expected to be primed by the caller */
            service_double_buffer(k);
            ret16(0);
            return;
        }
        case 0x00100008: {                                                  /* SndChannelStatus(chan, len, status) */
            maddr st = pop32(); int16_t len = pops16(); maddr chan = pop32();
            Chan *k = chan_for(chan);
            mset(st, 0, (uint32_t)(len > 0 ? len : 0));
            if (k && len >= 13) { wr8(st + 12, (uint8_t)(k->playing || k->qn || k->busy_buffer || k->dbh)); wr8(st + 14, (uint8_t)k->paused); }
            ret16(k ? 0 : (uint16_t)-205);
            return;
        }
        case 0x00140008: {                                                  /* SndManagerStatus(len, status) */
            maddr st = pop32(); int16_t len = pops16();
            mset(st, 0, (uint32_t)(len > 0 ? len : 0));
            int n = 0; for (int i = 0; i < MAXCH; i++) n += ch[i].used;
            if (len >= 4) { wr16(st, 100); wr16(st + 2, (uint16_t)n); }
            ret16(0);
            return;
        }
    }
    {
        const DispatchInfo *d = NULL;
        for (const DispatchInfo *x = dispatch_info; x->name; x++) if (x->trap == 0xA800 && (x->selector & 0x00FFFFFF) == (sel & 0x00FFFFFF)) d = x;
        if (d) sel = d->selector;
    }
    if (!dispatch_default(w, sel)) {
        fprintf(stderr, "[snd] SoundDispatch %08x: unknown selector; guessing stack size\n", sel);
        cpu_set(R_SP, cpu_get(R_SP) + ((sel >> 16) & 0xFF) * 2);
        ret16(0);
    }
}

void traps_sound_init(void) {
    trap_register(TRAP_SndNewChannel, T_SndNewChannel);
    trap_register(TRAP_SndDisposeChannel, T_SndDisposeChannel);
    trap_register(TRAP_SndDoCommand, T_SndDoCommand);
    trap_register(TRAP_SndDoImmediate, T_SndDoImmediate);
    trap_register(TRAP_SndControl, T_SndControl);
    trap_register(TRAP_SndPlay, T_SndPlay);
    trap_register(TRAP_SoundDispatch, T_SoundDispatch);
}
