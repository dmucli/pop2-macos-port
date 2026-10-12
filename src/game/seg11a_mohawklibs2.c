/* CODE 11 "Mohawk Libs 2", first half (Ghidra 0x3d4e4-0x40aba): the music/sound layer.
 *
 *  - Live MIDI: note-on/note-off queues (QNoteOn/QNoteOff) played back from the sound VBL
 *    task (HandleLiveMusicVBLTask), fed by the "synth" MIDI parser (synthMidiData) or by
 *    MIDI Manager input (ProcessMidiEvents, SetupMIDIPort).
 *  - midiObj (Symantec C++ class, Pascal-convention methods): Standard MIDI File player
 *    driven by a timer (timerProc/advance/trackAdvance/calcTempo/calcTime2Next/getVLQ).
 *  - A Windows-MMSYSTEM-like API (midiOut*, waveOut*) used by the portable audio code of
 *    CODE 19/20: the "MIDI out" device is the synth parser, the two "wave out" devices
 *    queue WAVEHDRs that the sound VBL callback (FUN_0003eecc) plays.
 *  - MDH_*: front end of the encrypted music driver code resource ('MDRV'/'Jnth'),
 *    entered through the pointer gDrvEntry as drv(selector, arg...). MusicDriverHandler
 *    is the selector-based dispatcher used by the sound code.
 *
 * Ported from the Ghidra decompilation, checked against the disassembly (ground truth);
 * addresses in comments are Ghidra addresses (see docs/PORTING.md). */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* ---- live MIDI note queues (QNoteOn/QNoteOff) ---- */
#define gNoteOnQueue    GH(0x106e1c)   /* long: NewPtrClear(0x650): 100 note records of 16 bytes */
#define gNoteOffQueue   GH(0x106e20)   /* long: same for note-offs */
#define gMidiInRefNum   GH(0x106e24)   /* word: MIDI Manager input port refnum */
#define gMidiOutRefNum  GH(0x106e26)   /* word: MIDI Manager output port refnum */
#define gMidiSignedIn   GH(0x106e28)   /* byte: MIDISignIn done */
#define gMidiInPort     GH(0x106e29)   /* byte: input port added */
#define gMidiOutPort    GH(0x106e2a)   /* byte: output port added */
#define gMidiBaseTicks  GH(0x106e2c)   /* long: TickCount at the last reset (note time base) */
#define gChanProgram    GH(0x106e30)   /* byte[16]: program per MIDI channel */
#define gChanVolume     GH(0x106e40)   /* byte[16]: volume (controller 7) per channel */
#define gMidiMgrUsed    GH(0x106e50)   /* byte: the MIDI Manager was asked for */
#define gMidi51         GH(0x106e51)   /* byte: cleared by SetupMIDIPort */
#define gNoteQueueBusy  GH(0x106e54)   /* byte: the queues are being changed */
#define gMidiInBusy     GH(0x106e55)   /* byte: inside the MIDI Manager read hook */
#define gMidiClientName GH(0x106e10)   /* Str: MIDISignIn name */
#define gMidiPortName   GH(0x106dfe)   /* Str: MIDIAddPort name */
/* ---- synth MIDI byte parser (midiOut device) ---- */
#define gMidiOutOpen    GH(0x106eb0)   /* byte: midiOutOpen done */
#define gMidiOutBusy    GH(0x106eb1)   /* byte: reentrancy counter of midiOutShortMsg/LongMsg */
#define gChanMsgLen     GH(0x106eb2)   /* byte[8]: message length by (status & 0x70) >> 4 */
#define gSysMsgLen      GH(0x106eba)   /* byte[8]: message length by (status & 7) for 0xF0-0xF7 */
#define gSynthHandlers  GH(0x106ec2)   /* long[8]: handler per (status >> 4) & 7 (synthNoteOn ...) */
#define gSynthMsg       GH(0x106ee2)   /* 4 bytes: channel, data1, data2, pad (passed as a long) */
#define gSynthData1     GH(0x106ee3)
#define gSynthData2     GH(0x106ee4)
#define gSynthStatus    GH(0x106ee6)   /* byte: status of the message being parsed */
#define gSynthDataIdx   GH(0x106ee7)   /* byte: data bytes received */
#define gSynthRunning   GH(0x106ee8)   /* byte: running status */
#define gPatchCache     GH(0x106eea)   /* word[128]: patches cached by midiOutCachePatches */
#define gSynthRemain    GH(0x106fea)   /* byte: data bytes still expected */
#define gHSInitialised  GH(0x106fec)   /* byte: HS_SetupSystem succeeded */
#define gDeviceName     GH(0x106fee)   /* C string: device name for the *GetDevCaps calls */
#define gSmfTempoMeta   GH(0x106e9a)   /* 10 bytes compared with meta event 6 */
#define gSmfLoopMeta    GH(0x106ea6)   /* 8 bytes compared with meta event 6 ("loop" marker) */
/* ---- wave out devices: 2 records of 0x2a bytes at 0x106ffe ----
 *  +0 sample rate (from the WAVEFORMAT), +4 callback, +8 instance, +c open flags,
 *  +10 position, +14 queue head, +18 header playing, +1c loop start, +20 loop end,
 *  +24 loop count (word), +26 queue count (word), +28 flags (bit7 paused, bit6 break loop,
 *  bit5 open). WAVEHDR: +0 data, +4 length, +8 user, +10 flags (long; word +12),
 *  +16 loops, +18 next, +1c position << 16. */
#define gWaveDev        GH(0x106ffe)
#define WAVEDEV_SIZE    0x2a
#define WAVEDEV(d)      (gWaveDev + (int32_t)(d) * WAVEDEV_SIZE)
/* ---- music driver (MDH) ---- */
#define gAUXVersion     GH(0x1070d8)   /* long: -1 until FUN_0003f608 asked Gestalt('a/ux') */
#define gAUXVersionLo   GH(0x1070da)
#define gDrvEntry       GH(0x1070dc)   /* long: entry point of the locked driver code */
#define gDrvHandle      GH(0x1070e0)   /* long: handle of the driver code resource */
#define gCurSong        GH(0x1070e4)   /* word: song playing (-1 = none) */
#define gMusicPaused    GH(0x1070e6)   /* byte: music paused (FUN_00040888) */
#define gMusic_e7       GH(0x1070e7)   /* byte */
#define gSongStartTime  GH(0x1070e8)   /* long: driver time when the song started */
#define gPauseTime      GH(0x1070ec)   /* long: driver time when the music was paused */
#define gMusic_f0       GH(0x1070f0)   /* byte: selector 5 */
#define gMusic_f1       GH(0x1070f1)   /* byte: selector 6 */
#define gFadeActive     GH(0x1070f2)   /* byte: auto fade in progress */
#define gFadeDone       GH(0x1070f3)   /* byte: auto fade reached its end */
#define gFadeOut        GH(0x1070f4)   /* byte: 1 = fading out, 0 = in */
#define gMasterFade     GH(0x1070f6)   /* word: master fade level 0..0x100 */
#define gFadeStep       GH(0x1070f8)   /* word: level change per step */
#define gFadeTicks      GH(0x1070fa)   /* word: VBLs between steps */
#define gFadeCounter    GH(0x1070fc)   /* word: VBLs to the next step */
#define gSongLoaded     GH(0x1070fe)   /* byte: the current song was loaded by MDH_LoadSong */
#define gSongDoneProc   GH(0x107100)   /* long: procedure called (word song) when a song ends */
#define gDecryptSeed    GH(0x107104)   /* word: driver decryption state */
#define gPlayNoteErr    GH(0x107106)   /* Str255 for DebugStr */
#define gStopNoteErr    GH(0x107122)
#define gMusicMode      GH(0x107052)   /* word: output quality 1..7, 0x64 = instrument playback */
#define gG107e8e        GH(0x107e8e)   /* word saved around the midiObj end-of-song notification */

#define MUSIC_INSTRUMENTS 0x64

/* drv(selector, args...): the driver's C-convention entry (re-read each call) */
static uint32_t drv1(uint32_t sel) { return call_ptr(R32(gDrvEntry), "l", sel); }
static uint32_t drv2(uint32_t sel, uint32_t a) { return call_ptr(R32(gDrvEntry), "ll", sel, a); }
static uint32_t drv3(uint32_t sel, uint32_t a, uint32_t b) { return call_ptr(R32(gDrvEntry), "lll", sel, a, b); }
/* Same, but the caller pops the arguments later (the original keeps them on the stack
 * across the following call(s)): the arguments stay pushed, the caller restores SP. */
static uint32_t drv1_keep(uint32_t sel) { push32(sel); return call68_addr(R32(gDrvEntry)); }
static uint32_t drv2_keep(uint32_t sel, uint32_t a) { push32(a); push32(sel); return call68_addr(R32(gDrvEntry)); }

/* An extra argument the original pushes for a callee that takes none (or fewer): it sits
 * above the callee's frame, so it is pushed by hand around the C call. */
#define WITH_PUSH32(v, call) do { maddr sp_ = emu_sp(); push32(v); call; emu_set_sp(sp_); } while (0)
#define WITH_PUSH16(v, call) do { maddr sp_ = emu_sp(); push16(v); call; emu_set_sp(sp_); } while (0)

/* Pascal functions whose result slot is filled but whose D0 is not the result: the glue
 * ("conv": "manual") stores the result above the arguments and pops them. When such a
 * function is called from C, the caller's result slot is part of its argument bytes. */
#define PGLUE(Name, argb, slotb, call) void glue_##Name(void) { \
        maddr sp = emu_sp(); uint32_t r = (call); \
        if ((slotb) == 2) W16(sp + (argb), r); else if ((slotb) == 4) W32(sp + (argb), r); \
        emu_set_sp(sp + (argb)); }
#define ENTRYS_(fn, slot, n) \
    maddr entry_sp_ __attribute__((cleanup(port_leave))) = emu_sp(); \
    const maddr args_ = entry_sp_ == port_glue_sp ? entry_sp_ : entry_sp_ - (uint32_t)fn_info[fn].arg_bytes - (uint32_t)(slot); \
    emu_set_sp(args_ - (uint32_t)(n)); (void)args_
#define ENTRY_LINK_S(fn, slot, link, regbytes) ENTRYS_(fn, slot, 8 + (link) + (regbytes)); const maddr a6 = args_ - 8; (void)a6

/* Pascal call through a pointer (C++ virtual method): `slot` result bytes reserved (not
 * written, like subq), args pushed in Pascal order by the caller of this helper; the
 * callee pops its arguments, the caller pops the slot. Returns the slot contents. */
static uint32_t pcall(maddr fn, maddr sp_slot, int slot) {
    call68_addr(fn);
    emu_set_sp(sp_slot);
    uint32_t r = slot == 2 ? RU16(sp_slot) : slot == 4 ? R32(sp_slot) : 0;
    emu_set_sp(sp_slot + (uint32_t)slot);
    return r;
}

/* ======================================================================== */

/* 0003d4e4: sound VBL: play the queued note-ons/note-offs that are due */
uint32_t HandleLiveMusicVBLTask(uint32_t p) {
    ENTRY_LINK(FN_HandleLiveMusicVBLTask, 0, 12);      /* link 0; movem D6-D7/A4 */
    (void)p;                                           /* read into D7, then overwritten */
    if (R8(gNoteQueueBusy) != 0 || R8(gMidiInBusy) != 0) return 0;
    W8(gNoteQueueBusy, 1);
    int32_t now = (int32_t)FUN_0003f5d4();
    for (int16_t i = 0; i < 100; i++) {
        maddr n = (uint32_t)((int32_t)i << 4) + R32(gNoteOnQueue);
        if (R8(n + 0xe) != 0 && now >= (int32_t)R32(n + 0xa)) {
            MDH_PlayNote(n);
            W8(n + 0xe, 0);
        }
        n = (uint32_t)((int32_t)i << 4) + R32(gNoteOffQueue);
        if (R8(n + 0xe) != 0 && now >= (int32_t)R32(n + 0xa)) {
            MDH_StopNote(n);
            W8(n + 0xe, 0);
        }
        if (R8(gMidiInBusy) != 0) break;
    }
    W8(gNoteQueueBusy, 0);
    return 0;
}

/* copy a 16-byte note record into the first free slot (byte +0xe = in use) of a queue */
static void queue_note(maddr queue_ptr, maddr rec) {
    W8(gNoteQueueBusy, 1);
    for (int16_t i = 0; i < 100; i++) {
        uint32_t off = (uint32_t)((int32_t)i << 4);
        if (R8(R32(queue_ptr) + off + 0xe) == 0) {
            W8(rec + 0xe, 1);
            maddr dst = R32(queue_ptr) + off;          /* ([q,A5],D0.l): post-indexed */
            for (int k = 0; k < 16; k += 4) W32(dst + k, R32(rec + k));
            break;
        }
    }
    W8(gNoteQueueBusy, 0);
}

/* 0003d588 */
uint32_t QNoteOn(uint32_t rec) { queue_note(gNoteOnQueue, rec); return 0; }

/* 0003d5e4 */
uint32_t QNoteOff(uint32_t rec) { queue_note(gNoteOffQueue, rec); return 0; }

/* 0003d642: clear both queues, all notes off in the driver */
uint32_t FUN_0003d642(void) {
    ENTRY_NOLINK(FN_FUN_0003d642, 4);                  /* movem D7 */
    W8(gNoteQueueBusy, 1);
    for (int16_t i = 0; i < 100; i++) {
        uint32_t off = (uint32_t)((int32_t)i << 4);
        W8(R32(gNoteOnQueue) + off + 0xe, 0);
        W8(R32(gNoteOffQueue) + off + 0xe, 0);
    }
    FUN_000401bc();
    W8(gNoteQueueBusy, 0);
    return 0;
}

/* 0003d67e: reset the note time base */
uint32_t FUN_0003d67e(void) {
    ENTRY_NOLINK(FN_FUN_0003d67e, 0);
    W32(gMidiBaseTicks, _TickCount());
    SetSyncTime(0);
    return 0;
}

/* 0003d690: reset programs/volumes, queues and time base */
uint32_t FUN_0003d690(void) {
    ENTRY_NOLINK(FN_FUN_0003d690, 4);                  /* movem D7 */
    for (int16_t i = 0; i < 0x10; i++) {
        W8(gChanProgram + i, 0);
        W8(gChanVolume + i, 0x7f);
    }
    FUN_0003d642();
    FUN_0003d67e();
    return 0;
}

/* 0003d6b8: memset (count decremented in its own argument slot) */
uint32_t MS_SetMemory(uint32_t dst, uint32_t n, int16_t v) {
    do {
        W8(dst, (uint8_t)((uint16_t)v >> 8));          /* move.b (0x10,A6): high byte */
        dst++;
    } while (--n != 0);
    return 0;
}
void glue_MS_SetMemory(void) {
    maddr sp = emu_sp();
    MS_SetMemory(R32(sp), R32(sp + 4), R16(sp + 8));
    W32(sp + 4, 0);                                    /* subq.l #1,(0xc,A6) until 0 */
}

/* fill a note record at rec (16 bytes): program, note, duration, velocity, channel, time */
static void note_rec(maddr rec, uint8_t ch, uint8_t note, uint16_t dur, uint16_t vel, uint32_t time) {
    W16(rec + 0, R8(gChanProgram + ch));
    W16(rec + 2, note);
    W16(rec + 4, dur);
    W16(rec + 6, vel);
    W16(rec + 8, ch);
    W32(rec + 0xa, time);
}

/* 0003d6e6: note on (channel, note, velocity bytes); velocity 0 after volume = note off */
uint32_t ProcessMidiNoteOn(int16_t p_ch, int16_t p_note, int16_t p_vel) {
    ENTRY_LINK(FN_ProcessMidiNoteOn, 0x10, 12);        /* link -0x10; movem D5-D7 */
    uint8_t ch = (uint8_t)((uint16_t)p_ch >> 8), note = (uint8_t)((uint16_t)p_note >> 8);
    uint8_t vel = (uint8_t)((uint16_t)p_vel >> 8);
    int32_t prod = (int32_t)((uint32_t)vel * R8(gChanVolume + ch));   /* mulu.w */
    vel = (uint8_t)(prod / 0x7f);                                      /* divs.l */
    if (vel != 0) {
        maddr rec = a6 - 0x10;
        W16(rec + 6, vel);
        W16(rec + 0, R8(gChanProgram + ch));
        W16(rec + 2, note);
        W16(rec + 4, 0x4e20);
        W16(rec + 8, ch);
        W32(rec + 0xa, _TickCount() + 1 - R32(gMidiBaseTicks));
        QNoteOn(rec);
    } else {
        /* pushes velocity, note, channel: ProcessMidiNoteOff reads only the last two */
        WITH_PUSH16((uint16_t)vel << 8, ProcessMidiNoteOff(p_ch, p_note));
    }
    return 0;
}

/* 0003d78a: note off (channel, note bytes) */
uint32_t ProcessMidiNoteOff(int16_t p_ch, int16_t p_note) {
    ENTRY_LINK(FN_ProcessMidiNoteOff, 0x10, 0);        /* link -0x10 */
    uint8_t ch = (uint8_t)((uint16_t)p_ch >> 8), note = (uint8_t)((uint16_t)p_note >> 8);
    maddr rec = a6 - 0x10;
    W16(rec + 0, R8(gChanProgram + ch));
    W16(rec + 2, note);
    W16(rec + 4, 0);
    W16(rec + 6, 0);
    W16(rec + 8, ch);
    W32(rec + 0xa, _TickCount() + 1 - R32(gMidiBaseTicks));
    QNoteOff(rec);
    return 0;
}

/* 0003d7ee: controller (channel, controller, value bytes): 7 volume, 0x79 reset, 0x7b notes off */
uint32_t ProcessController(int16_t p_ch, int16_t p_ctl, int16_t p_val) {
    ENTRY_LINK(FN_ProcessController, 0, 0);
    uint8_t ctl = (uint8_t)((uint16_t)p_ctl >> 8);
    if (ctl & 0x80) return 0;
    if (ctl == 0x79) { FUN_0003d642(); FUN_0003d690(); }
    else if (ctl > 0x79) { if (ctl == 0x7b) FUN_0003d642(); }
    else if (ctl == 7) W8(gChanVolume + ((uint16_t)p_ch >> 8), (uint16_t)p_val >> 8);
    return 0;
}

/* 0003d844: program change (channel, program bytes) */
uint32_t ProcessProgramChange(int16_t p_ch, int16_t p_prog) {
    W8(gChanProgram + ((uint16_t)p_ch >> 8), (uint16_t)p_prog >> 8);
    return 0;
}

/* 0003d872: MIDI Manager read hook (Pascal: refCon = the application's A5, packet;
 * returns midiKeepPacket = 1). It swaps A5 with refCon around its body. */
uint32_t ProcessMidiEvents(uint32_t refcon, uint32_t pkt) {
    ENTRY_LINK_S(FN_ProcessMidiEvents, 2, 0x12, 24);   /* link -0x12; movem D3-D7/A4 */
    uint32_t old_a5 = cpu_get(R_A5);
    cpu_set(R_A5, refcon);                             /* exg D0,A5 (the old A5 goes to the arg slot) */
    W8(gMidiInBusy, 1);
    if ((R8(pkt) & 3) == 0) {
        uint32_t t = _TickCount() + 1 - R32(gMidiBaseTicks);
        uint8_t status = R8(pkt + 6) & 0xf0;
        W8(a6 - 0x11, status);
        uint8_t ch = R8(pkt + 6) & 0xf, d1 = R8(pkt + 7);
        maddr rec = a6 - 0x10;
        if (status & 0x80) {
            if (status == 0xb0) {
                if (!(d1 & 0x80)) {
                    if (d1 == 0x79) { FUN_0003d642(); FUN_0003d690(); }
                    else if (d1 > 0x79) { if (d1 == 0x7b) FUN_0003d642(); }
                    else if (d1 == 7) W8(gChanVolume + ch, R8(pkt + 8));
                }
            } else if (status == 0xc0) {
                W8(gChanProgram + ch, d1);
            } else if (status == 0x90 || status == 0x80) {
                uint8_t vel = 0;
                if (status == 0x90) {
                    int32_t prod = (int32_t)((uint32_t)R8(pkt + 8) * R8(gChanVolume + ch));
                    vel = (uint8_t)(prod / 0x7f);
                }
                if (vel != 0) {
                    W16(rec + 6, vel);
                    W16(rec + 0, R8(gChanProgram + ch));
                    W16(rec + 2, d1);
                    W16(rec + 4, 0x4e20);
                    W16(rec + 8, ch);
                    W32(rec + 0xa, t);
                    QNoteOn(rec);
                } else {
                    note_rec(rec, ch, d1, 0, 0, t);
                    QNoteOff(rec);
                }
            }
        }
    }
    W8(gMidiInBusy, 0);
    cpu_set(R_A5, old_a5);
    return 1;
}
void glue_ProcessMidiEvents(void) {
    maddr sp = emu_sp();
    uint32_t refcon = R32(sp);
    ProcessMidiEvents(refcon, R32(sp + 4));
    W32(sp, refcon);                                   /* the slot ends holding refCon again */
    W16(sp + 8, 1);
    emu_set_sp(sp + 8);
    emu_set_d0(refcon);                                /* D0 = refCon after the second exg */
}

/* MIDI Manager calls (SoundDispatch, D0 = routine << 16 | 4) */
static void midi_trap(uint32_t sel) { cpu_set(R_D0, sel); tb_trap(0xA800); }

/* add a MIDI Manager port (input 'inpt' with the read hook, or output 'outp'); the
 * MS_SetMemory arguments stay on the stack and its top word becomes the result slot */
static int16_t add_midi_port(maddr base, maddr params, uint32_t a5, int is_in) {
    maddr sp = base - 10;                               /* clr.b; pea 0x11c; pea params */
    W8(sp + 8, 0); W32(sp + 4, 0x11c); W32(sp, params);
    emu_set_sp(sp);
    MS_SetMemory(params, 0x11c, 0);
    W32(sp + 4, 0);                                     /* its count slot ends at 0 */
    W32(params, is_in ? 0x696e7074 : 0x6f757470);       /* 'inpt' / 'outp' */
    W16(params + 4, is_in ? 1 : 2);
    W16(params + 6, 0);
    W32(params + 8, 0);
    W32(params + 0xc, is_in ? JT_ADDR(FN_ProcessMidiEvents) : 0);
    W32(params + 0x10, a5);
    W16(params + 0x14, 0);
    W32(params + 0x16, 0);
    W16(params + 0x1a, 0);
    _BlockMove(gMidiPortName, params + 0x1c, (uint32_t)((int32_t)RS8(gMidiPortName) + 1));
    W16(sp, 0);                                         /* clr.w (A7): result slot */
    push32(0x4d504c52); push16(0x1f4);                  /* 'MPLR', 500 */
    push32(is_in ? gMidiInRefNum : gMidiOutRefNum); push32(params);
    midi_trap(0x1c0004);                                /* MIDIAddPort */
    int16_t err = (int16_t)pop16();
    emu_set_sp(base);                                   /* addq.l #8 */
    if (err == 0) W8(is_in ? gMidiInPort : gMidiOutPort, 1);
    return err;
}

/* 0003d9fc: allocate the note queues; when asked (output flag in the high byte of p_out,
 * input flag in the high byte of p_in) sign in to the MIDI Manager and add the ports.
 * Returns an OSErr (word). */
uint32_t SetupMIDIPort(int16_t icon_id, int16_t p_out, int16_t p_in) {
    ENTRY_LINK(FN_SetupMIDIPort, 0x11c, 12);           /* link -0x11c; movem D6-D7/A4 */
    int16_t err = 0;
    W8(gMidiMgrUsed, 0);
    W8(gMidiSignedIn, 0);
    W8(gMidiInPort, 0);
    W8(gMidiOutPort, 0);
    W8(gMidi51, 0);
    W8(gMidiInBusy, 0);
    uint32_t a5 = cpu_get(R_A5);                       /* D6 = A5 (refCon for the read hook) */
    W8(gNoteQueueBusy, 1);
    W32(gNoteOnQueue, _NewPtrClear(0x650));
    W32(gNoteOffQueue, _NewPtrClear(0x650));
    if (R32(gNoteOnQueue) == 0 || R32(gNoteOffQueue) == 0) {
        err = -0x6c;
    } else if ((uint16_t)p_out >> 8 || (uint16_t)p_in >> 8) {
        maddr base = emu_sp();
        uint32_t icon = _GetResource(0x49434e23, icon_id);       /* 'ICN#' */
        W8(gMidiMgrUsed, 1);
        push16(0); push32(0x4d504c52); push32(a5); push32(icon); push32(gMidiClientName);
        midi_trap(0x40004);                                       /* MIDISignIn */
        err = (int16_t)pop16();
        emu_set_sp(base);
        if (err == 0) {
            W8(gMidiSignedIn, 1);
            if ((uint16_t)p_in >> 8) err = add_midi_port(base, a6 - 0x11c, a5, 1);
            if ((uint16_t)p_out >> 8) err = add_midi_port(base, a6 - 0x11c, a5, 0);
        }
    }
    if (err == 0) {
        FUN_0003d690();
        W8(gNoteQueueBusy, 0);
    } else {
        FUN_0003dbc2();
    }
    return (uint16_t)err;
}

/* 0003dbc2: remove the MIDI Manager ports, sign out, free the note queues */
uint32_t FUN_0003dbc2(void) {
    ENTRY_NOLINK(FN_FUN_0003dbc2, 0);
    W8(gNoteQueueBusy, 1);
    if (R8(gMidiMgrUsed) != 0) {
        maddr base = emu_sp();
        if (R8(gMidiInPort) != 0) { push16(RU16(gMidiInRefNum)); midi_trap(0x4c0004); emu_set_sp(base); }   /* MIDIRemovePort */
        if (R8(gMidiOutPort) != 0) { push16(RU16(gMidiOutRefNum)); midi_trap(0x4c0004); emu_set_sp(base); }
        if (R8(gMidiSignedIn) != 0) { push32(0x4d504c52); midi_trap(0x80004); emu_set_sp(base); }          /* MIDISignOut */
    }
    W8(gMidiInPort, 0);
    W8(gMidiOutPort, 0);
    W8(gMidiSignedIn, 0);
    if (R32(gNoteOnQueue) != 0) { _DisposePtr(R32(gNoteOnQueue)); W32(gNoteOnQueue, 0); }
    if (R32(gNoteOffQueue) != 0) { _DisposePtr(R32(gNoteOffQueue)); W32(gNoteOffQueue, 0); }
    return 0;
}

/* ---- midiObj (Symantec C++; methods are Pascal: this pushed last, callee pops) ----
 *  +0 vtable (+0x1c event/notify method, +0x34 loop method), +16 word, +30 midiMap handle,
 *  +82 MIDIHDR for sysex (data +82, length +86, flags +92), +9e timer, +a2 time to next,
 *  +a8 elapsed, +ac/+ae tempo-derived words, +b0/+b4 Fixed tempo factors, +b8 time,
 *  +bc/+c0 position (Fixed: integer +bc, fraction word +c0), +c2 next event delta,
 *  +c6/+c8/+ca/+cc loop control, +ce loop time, +d2 tracks finished, +d6 division,
 *  +d8 track count, +da tracks (16 bytes: +4 data pointer, +8 done, +a delta, +e running status). */

/* 0003dc34: advance(this, play, ticks): move the song position and run every track's events */
uint32_t advance__7midiObj(uint32_t self, int16_t play, int16_t ticks) {
    ENTRY_LINK(FN_advance__7midiObj, 0x34, 32);        /* link -0x34; movem D3-D7/A2-A4 */
    W32(self + 0xb8, R32(self + 0xb8) + (uint16_t)ticks);
    uint32_t tempo = R32(self + 0xb0);
    W32(a6 - 0x18, tempo);
    W16(a6 - 0x1c, 0);
    W16(a6 - 0x1a, ticks);
    uint32_t fticks = (uint32_t)((int32_t)ticks << 16);
    W32(a6 - 0x14, fticks);
    uint32_t prod = _FixMul(fticks, tempo);
    uint32_t old_int = R32(self + 0xbc);
    W32(a6 - 0x34, old_int);
    uint32_t frac = (uint32_t)RU16(self + 0xc0) + (prod & 0xffff);
    W32(a6 - 0x10, prod);
    W32(a6 - 0x30, frac);
    W32(a6 - 0xc, frac);
    W16(self + 0xc0, (uint16_t)frac);
    W32(a6 - 8, prod);
    W32(a6 - 4, frac);
    int32_t inc = (int32_t)(int16_t)((int32_t)prod >> 16) + (int32_t)(uint16_t)(frac >> 16);
    W32(self + 0xbc, R32(self + 0xbc) + (uint32_t)inc);
    uint32_t delta = R32(self + 0xbc) - old_int;
    W32(a6 - 0x2c, delta);
    if (delta >= R32(self + 0xc2)) W32(self + 0xc2, 0xffffffff);
    W16(a6 - 0x28, 0);
    for (;;) {
        uint16_t i = RU16(a6 - 0x28);
        if (i >= RU16(self + 0xd8)) break;
        maddr trk = ((uint32_t)i << 4) + self + 0xda;
        W32(a6 - 0x24, trk);
        delta = R32(a6 - 0x2c);
        W32(trk + 0xa, R32(trk + 0xa) - delta);
        if ((int32_t)R32(trk + 0xa) <= 0) trackAdvance__7midiObj(self, play, trk);
        trk = R32(a6 - 0x24);
        if (R16(trk + 8) == 0 && R32(trk + 0xa) < R32(self + 0xc2))
            W32(self + 0xc2, R32(trk + 0xa));
        W16(a6 - 0x28, RU16(a6 - 0x28) + 1);
    }
    if (R16(self + 0xc8) != 0) {
        W16(self + 0xc8, 0);
        uint16_t saved = RU16(gG107e8e);
        W16(a6 - 0x20, saved);
        uint16_t f16 = RU16(self + 0x16);
        W16(a6 - 0x1e, f16);
        W16(self + 0x16, 0);
        /* subq #2 (result); push this[0xce], this; jsr vtable[0x34] (Pascal, pops 8) */
        maddr slot = emu_sp() - 2;
        emu_set_sp(slot);
        push32(R32(self + 0xce)); push32(self);
        pcall(R32(R32(self) + 0x34), slot, 2);
        W16(self + 0x16, RU16(a6 - 0x1e));
        W16(gG107e8e, RU16(a6 - 0x20));
    }
    return 0;
}

/* 0003ddc0: calcTempo(this, usPerQuarter): tempo factors from the meta tempo event */
uint32_t calcTempo__7midiObj(uint32_t self, uint32_t tempo) {
    ENTRY_LINK(FN_calcTempo__7midiObj, 0x2c, 28);      /* link -0x2c; movem D3-D7/A2-A3 */
    W16(a6 - 0x2c, 0);
    uint16_t div = RU16(self + 0xd6);
    W16(a6 - 0x2a, div);
    uint32_t fdiv = (uint32_t)((int32_t)(int16_t)div << 16);
    W32(a6 - 0x28, fdiv);
    uint32_t t8 = tempo << 8;
    W32(a6 - 0x24, t8);
    uint32_t q = _FixDiv(t8, fdiv);                    /* D5 */
    W32(a6 - 0x20, 0x3e800);
    W32(a6 - 0x1c, q);
    W32(self + 0xb4, _FixDiv(q, 0x3e800));
    W32(a6 - 0x18, q);
    W32(a6 - 0x14, 0x3e800);
    uint32_t f = _FixDiv(0x3e800, q);
    W32(self + 0xb0, f);
    W32(a6 - 0x10, f);
    W32(a6 - 0xc, 0x7fff);
    uint32_t m = _FixMul(0x7fff, f);
    W16(self + 0xac, m > 0x7fff ? 0x7fff : (uint16_t)m);
    uint32_t b4 = R32(self + 0xb4);
    W32(a6 - 8, b4);
    W32(a6 - 4, 0x7fff);
    m = _FixMul(0x7fff, b4);
    W16(self + 0xae, m > 0x7fff ? 0x7fff : (uint16_t)m);
    return 0;
}

/* 0003dec8: calcTime2Next(this): timer delay (word) to the next event */
uint32_t calcTime2Next__7midiObj(uint32_t self) {
    ENTRY_LINK(FN_calcTime2Next__7midiObj, 0x1c, 28);  /* link -0x1c; movem D3-D7/A2-A3 */
    if (R32(self + 0xc2) == 0) return 0;
    uint32_t next = R32(self + 0xc2);
    uint16_t lim = RU16(self + 0xac);
    uint32_t x;
    if (next > (uint32_t)lim) {
        x = (uint32_t)((int32_t)(int16_t)RU16(self + 0xac) << 16);
    } else {
        W16(a6 - 0x1c, (uint16_t)next);
        x = (uint32_t)((int32_t)(int16_t)(uint16_t)next << 16);
        uint16_t fr = RU16(self + 0xc0);
        W16(a6 - 0x1a, fr);
        W16(a6 - 0x18, 0);
        x -= (uint32_t)fr;
    }
    uint32_t b4 = R32(self + 0xb4);
    W32(a6 - 0x14, b4);
    W32(a6 - 0x10, x);
    uint32_t r = _FixMul(x, b4) + 0x7fff;
    W32(a6 - 0xc, r);
    W32(a6 - 8, r);
    W32(a6 - 4, r);
    uint16_t res = (uint16_t)((int32_t)r >> 16) + (RU16(a6 - 2) >= 0x8000 ? 1 : 0);
    return res;
}

/* 0003dfbc: getVLQ(&ptr): read a MIDI variable-length quantity, advance the pointer */
uint32_t getVLQ__7midiObj(uint32_t pp) {
    maddr p = R32(pp); W32(pp, p + 1);
    uint8_t b = R8(p);
    uint32_t v = b & 0x7f;
    while (b & 0x80) {
        p = R32(pp); W32(pp, p + 1);
        b = R8(p);
        v = (v << 7) + (b & 0x7f);
    }
    return v;
}

/* 0003e032: timerProc(this, refcon, task): timer callback: advance the song, re-arm the
 * timer for the next event or notify the end of the song (vtable +0x1c, event 1) */
uint32_t timerProc__7midiObj(uint32_t self, int16_t p8, int16_t p10, int16_t p12, int16_t p14) {
    ENTRY_LINK(FN_timerProc__7midiObj, 0xc, 16);       /* link -0xc; movem D3/A2-A4 */
    uint32_t a4 = (uint32_t)(uint16_t)p12 << 16 | (uint16_t)p14;
    W32(a6 - 0xc, self);
    uint16_t dt = RU16(self + 0xa2);
    W32(self + 0xa8, R32(self + 0xa8) + dt);
    advance__7midiObj(self, 1, (int16_t)RU16(self + 0xa2));
    uint32_t obj = R32(a6 - 0xc);
    if (RU16(obj + 0xd2) < RU16(obj + 0xd8)) {
        uint16_t next = (uint16_t)calcTime2Next__7midiObj(R32(a6 - 0xc));
        W16(R32(a6 - 0xc) + 0xa2, next);
        uint32_t t = timerCreate(self, JT_ADDR(FN_timerProc__7midiObj), 0, (int16_t)next,
                                 (int16_t)(a4 >> 16), (int16_t)a4);
        W32(R32(a6 - 0xc) + 0x9e, t);
    } else {
        obj = R32(a6 - 0xc);
        W32(obj + 0x9e, 0);
        W16(obj + 0x16, 0);
        W8(a6 - 8, 1);
        maddr sp = emu_sp();
        push32(a6 - 8); push32(obj);
        call68_addr(R32(R32(obj) + 0x1c));             /* Pascal: pops its 8 bytes */
        emu_set_sp(sp);
    }
    return 0;
}

/* 0003e0f6: trackAdvance(this, play, track): run the track's events that are due; returns
 * the delta to its next event (0 at the end of the track) */
uint32_t trackAdvance__7midiObj(uint32_t self, int16_t play, uint32_t trk) {
    ENTRY_LINK(FN_trackAdvance__7midiObj, 0x28, 32);   /* link -0x28; movem D3-D7/A2-A4 */
    maddr pp = a6 - 0x28;                              /* the data pointer (its address escapes) */
    uint32_t msg = cpu_get(R_D7);                      /* D7: left from the caller if no message is built */
    W32(pp, R32(trk + 4));
    for (;;) {
        maddr p = R32(pp); W32(pp, p + 1);
        W8(a6 - 0x24, R8(p));
        uint32_t len;
        if (R8(a6 - 0x24) == 0xff) {                   /* meta event */
            W8(trk + 0xe, 0);
            p = R32(pp); W32(pp, p + 1);
            uint8_t type = R8(p);
            W8(a6 - 0x23, type);
            len = getVLQ__7midiObj(pp);
            switch (R8(a6 - 0x23)) {
            case 7:                                    /* cue point: notify (event 0) */
                if (play == 0) break;
                W8(a6 - 0x20, 0);
                W16(a6 - 0x1e, len > 0xffff ? 0xffff : (uint16_t)len);
                W32(a6 - 0x1c, R32(pp));
                {
                    maddr sp = emu_sp();
                    push32(a6 - 0x20); push32(self);
                    call68_addr(R32(R32(self) + 0x1c));
                    emu_set_sp(sp);
                }
                break;
            case 0x2f:                                 /* end of track */
                W16(trk + 8, 1);
                W16(self + 0xd2, RU16(self + 0xd2) + 1);
                W32(trk + 0xa, 0);
                return 0;
            case 6:                                    /* marker: loop start / loop end */
                if (len >= 10 && S16(memicmp_(R32(pp), gSmfTempoMeta, 10)) == 0) {
                    W32(self + 0xce, R32(self + 0xb8));
                    break;
                }
                if (len < 8) break;
                if (S16(memicmp_(R32(pp), gSmfLoopMeta, 8)) != 0) break;
                if (R16(self + 0xc6) != 0 || R16(self + 0xca) != 0 || play == 0) break;
                if (R16(self + 0xcc) == 0) {
                    W32(a6 - 0x18, R32(pp) + 8);
                    uint16_t n = (uint16_t)((uint16_t)len - 8);
                    while (n != 0 && R8(R32(a6 - 0x18)) != 0x23) {   /* '#' */
                        n--;
                        W32(a6 - 0x18, R32(a6 - 0x18) + 1);
                    }
                    uint32_t cnt = n != 0 ? tc_atoi(R32(a6 - 0x18) + 1) : 0xffff;
                    W16(self + 0xcc, cnt);
                }
                if (R16(self + 0xcc) != 0) {
                    W16(self + 0xc8, 1);
                    if (RU16(self + 0xcc) != 0xffff) {
                        W16(self + 0xcc, RU16(self + 0xcc) - 1);
                        W16(self + 0xc6, RU16(self + 0xcc) != 0 ? 1 : 0);
                    }
                }
                break;
            case 0x51: {                               /* tempo */
                uint32_t v = R32(R32(pp));
                W32(a6 - 0xc, v);
                calcTempo__7midiObj(self, v >> 8);
                break;
            }
            }
            W32(pp, R32(pp) + len);
        } else if (R8(a6 - 0x24) == 0xf0) {            /* sysex: sent with its F0 restored */
            W8(trk + 0xe, 0);
            len = getVLQ__7midiObj(pp);
            if (play != 0) {
                maddr d = R32(pp) - 1;
                W32(a6 - 0x14, d);
                W8(a6 - 0x10, R8(d));
                W8(d, 0xf0);
                W32(self + 0x82, d);
                W32(self + 0x86, len + 1);
                W32(self + 0x92, 2);
                midiMapLongMsg(0x1c, self + 0x82, R32(self + 0x30));
                W8(R32(a6 - 0x14), R8(a6 - 0x10));
            }
            W32(pp, R32(pp) + len);
        } else if (R8(a6 - 0x24) == 0xf7) {            /* sysex continuation */
            W8(trk + 0xe, 0);
            len = getVLQ__7midiObj(pp);
            if (play != 0) {
                W32(self + 0x82, R32(pp));
                W32(self + 0x86, len);
                W32(self + 0x92, 2);
                midiMapLongMsg(0x1c, self + 0x82, R32(self + 0x30));
            }
            W32(pp, R32(pp) + len);
        } else if (R8(a6 - 0x24) > 0xf0) {             /* anything else: end of the track */
            W16(trk + 8, 1);
            W16(self + 0xd2, RU16(self + 0xd2) + 1);
            W32(trk + 0xa, 0);
            return 0;
        } else {                                       /* channel message (running status) */
            if (!(R8(a6 - 0x24) & 0x80)) {
                W8(a6 - 0x24, R8(trk + 0xe));
                W32(pp, R32(pp) - 1);
            }
            uint8_t st = R8(a6 - 0x24);
            W8(trk + 0xe, st);
            switch (st & 0xf0) {
            case 0xb0: case 0x80: case 0x90: case 0xe0: case 0xa0: {
                uint8_t s = R8(a6 - 0x24);
                W8(a6 - 8, s);
                maddr d = R32(pp);
                uint8_t d1 = R8(d);
                W8(a6 - 7, d1);
                uint16_t lo = (uint16_t)(d1 << 8 | s);
                W16(a6 - 6, lo);
                uint16_t d2 = R8(d + 1);
                W16(a6 - 4, d2);
                msg = (uint32_t)d2 << 16 | lo;
                W32(pp, R32(pp) + 2);
                break;
            }
            case 0xd0: case 0xc0: {
                uint8_t s = R8(a6 - 0x24);
                W8(a6 - 2, s);
                uint8_t d1 = R8(R32(pp));
                W8(a6 - 1, d1);
                msg = (uint16_t)(d1 << 8 | s);
                W32(pp, R32(pp) + 1);
                break;
            }
            }
            if (play != 0) midiMapShortMsg(msg, R32(self + 0x30));
        }
        uint32_t delta = getVLQ__7midiObj(pp);
        W32(trk + 0xa, R32(trk + 0xa) + delta);
        if ((int32_t)R32(trk + 0xa) > 0) break;
    }
    W32(trk + 4, R32(pp));
    return R32(trk + 0xa);
}

/* 0003e4f0: synthMidiData(data, n): parse MIDI bytes and call the handler of each
 * complete message (pointer and count are advanced in their own argument slots) */
uint32_t synthMidiData(uint32_t p, uint32_t n) {
    ENTRY_LINK(FN_synthMidiData, 0, 4);                /* link 0; movem D7 */
    for (; n != 0; n--) {
        uint8_t b = R8(p); p++;
        if (b >= 0xf8) continue;                       /* real-time bytes are ignored */
        int status_byte;
        if (R8(gSynthRemain) != 0 && b < 0x80) {
            status_byte = 0;                           /* data byte of the current message */
        } else {
            W8(gSynthDataIdx, 0);
            if (b >= 0xf0) {
                W8(gSynthStatus, b);
                W8(gSynthRunning, 0);
                W8(gSynthRemain, (uint8_t)(R8(gSysMsgLen + (R8(gSynthStatus) & 7)) - 1));
                W8(gSynthMsg, R8(gSynthStatus) & 0xf);
                goto check;
            }
            if (b >= 0x80) W8(gSynthRunning, b);
            else if (R8(gSynthRunning) == 0) continue;
            W8(gSynthStatus, R8(gSynthRunning));
            W8(gSynthRemain, (uint8_t)(R8(gChanMsgLen + ((R8(gSynthRunning) & 0x70) >> 4)) - 1));
            if (b >= 0x80) {
                W8(gSynthMsg, R8(gSynthStatus) & 0xf);
                goto check;
            }
            /* running status: the byte is the first data byte */
            W8(gSynthDataIdx, R8(gSynthDataIdx) + 1);
            W8(gSynthData1, b);
            W8(gSynthRemain, R8(gSynthRemain) - 1);
            goto check;
        }
        if (status_byte == 0) {
            if (R8(gSynthDataIdx) != 0) {
                W8(gSynthData2, b);
            } else {
                W8(gSynthDataIdx, R8(gSynthDataIdx) + 1);
                W8(gSynthData1, b);
            }
            W8(gSynthRemain, R8(gSynthRemain) - 1);
        }
    check:
        if (R8(gSynthRemain) == 0) {
            uint8_t k = (uint8_t)((R8(gSynthStatus) >> 4) & 7);
            if (R32(gSynthHandlers + k * 4) != 0)
                call_ptr(R32(gSynthHandlers + k * 4), "l", R32(gSynthMsg));
        }
    }
    return 0;
}
void glue_synthMidiData(void) {
    maddr sp = emu_sp();
    uint32_t p = R32(sp), n = R32(sp + 4);
    synthMidiData(p, n);
    W32(sp, p + n);                                    /* addq.l #1,(8,A6) per byte */
    W32(sp + 4, 0);                                    /* subq.l #1,(0xc,A6) down to 0 */
}

/* 0003e5ec */
uint32_t FUN_0003e5ec(void) { return 0; }

/* 0003e5ee: handler for note on: bytes channel, note, velocity */
uint32_t synthNoteOn(int16_t p4, int16_t p6) {
    ENTRY_LINK(FN_synthNoteOn, 0, 0);
    ProcessMidiNoteOn((int16_t)(p4 & 0xff00), (int16_t)((p4 & 0xff) << 8), (int16_t)(p6 & 0xff00));
    return 0;
}

/* 0003e614: handler for note off */
uint32_t synthNoteOff(int16_t p4, int16_t p6) {
    ENTRY_LINK(FN_synthNoteOff, 0, 0);
    WITH_PUSH16((uint16_t)(p6 & 0xff00),
                ProcessMidiNoteOff((int16_t)(p4 & 0xff00), (int16_t)((p4 & 0xff) << 8)));
    return 0;
}

/* 0003e63c: handler for control change */
uint32_t synthControlChange(int16_t p4, int16_t p6) {
    ENTRY_LINK(FN_synthControlChange, 0, 0);
    ProcessController((int16_t)(p4 & 0xff00), (int16_t)((p4 & 0xff) << 8), (int16_t)(p6 & 0xff00));
    return 0;
}

/* 0003e66a: handler for program change */
uint32_t synthProgramChange(int16_t p4) {
    ENTRY_LINK(FN_synthProgramChange, 0, 0);
    ProcessProgramChange((int16_t)(p4 & 0xff00), (int16_t)((p4 & 0xff) << 8));
    return 0;
}

/* 0003e694: byte-swap a long (Windows message order) */
uint32_t ConvertLong2PC(uint32_t v) {
    return (v & 0xff) << 24 | (v & 0xff00) << 8 | (v >> 8 & 0xff00) | v >> 24;
}

/* 0003e6dc: one MIDI out device (stored in the caller's result slot; no Pascal pop) */
uint32_t midiOutGetNumDevs(void) { return 1; }
void glue_midiOutGetNumDevs(void) { W16(emu_sp(), 1); }

/* 0003e6fe: midiOutGetDevCaps(size, caps, device): MIDIOUTCAPS (0x32 bytes) */
uint32_t midiOutGetDevCaps(int16_t size, uint32_t caps, int16_t dev) {
    ENTRY_LINK(FN_midiOutGetDevCaps, 0x32, 8);         /* link -0x32; movem A3-A4 */
    maddr c = a6 - 0x32;
    W16(c + 0, 0x14b1);                                /* wMid */
    W16(c + 2, 0x228c);                                /* wPid */
    W16(a6 - 0xc, 2);
    W16(a6 - 0xa, 8);
    W16(a6 - 8, 8);
    W16(a6 - 6, 0xffff);
    W16(c + 4, 0x2711);                                /* driver version */
    W32(a6 - 4, 4);
    maddr s = gDeviceName, d = a6 - 0x2c;              /* name (rest of the buffer: stack garbage) */
    while (R8(s) != 0) { W8(d, R8(s)); d++; s++; }
    W8(d, 0);
    _BlockMove(c, caps, 0x32);
    return 0;
}

/* 0003e776: not supported (8) */
uint32_t midiOutGetVolume(int16_t p4, int16_t p6, int16_t p8) { return 8; }
PGLUE(midiOutGetVolume, 6, 2, 8)

/* 0003e79a: not supported (8) */
uint32_t midiOutSetVolume(int16_t p4, int16_t p6, int16_t p8) { return 8; }
PGLUE(midiOutSetVolume, 6, 2, 8)

/* 0003e7be: midiOutOpen: open the synth device once (4 = already open) */
uint32_t midiOutOpen(int16_t p4, int16_t p6, int16_t p8, int16_t p10, int16_t p12, int16_t p14,
                     int16_t p16, int16_t p18, int16_t p20) {
    if (R8(gMidiOutOpen) != 0) return 4;
    for (int16_t i = 0; i < 0x80; i++) W16(gPatchCache + i * 2, 0);
    W8(gMidiOutOpen, 1);
    W8(gMidiOutBusy, 0);
    W8(gSynthRunning, 0);
    W8(gSynthRemain, 0);
    uint32_t phmo = (uint32_t)(uint16_t)p18 << 16 | (uint16_t)p20;
    if (phmo != 0) W16(phmo, 0);
    return 0;
}
PGLUE(midiOutOpen, 0x12, 2, midiOutOpen(R16(sp), R16(sp + 2), R16(sp + 4), R16(sp + 6), R16(sp + 8),
                                        R16(sp + 10), R16(sp + 12), R16(sp + 14), R16(sp + 16)))

/* 0003e81c: midiOutClose (0x41 = still playing) */
uint32_t midiOutClose(int16_t p4) {
    if (R8(gMidiOutBusy) != 0) return 0x41;
    W8(gMidiOutOpen, 0);
    W8(gMidiOutBusy, 0);
    return 0;
}
PGLUE(midiOutClose, 2, 2, midiOutClose(R16(sp)))

/* 0003e850: mark a MIDIHDR prepared */
uint32_t midiOutPrepareHeader(int16_t p4, uint32_t hdr, int16_t p10) {
    if (hdr != 0) W16(hdr + 0x12, RU16(hdr + 0x12) | 2);
    return 0;
}
PGLUE(midiOutPrepareHeader, 8, 2, midiOutPrepareHeader(R16(sp), R32(sp + 2), R16(sp + 6)))

/* 0003e886: mark a MIDIHDR unprepared */
uint32_t midiOutUnprepareHeader(int16_t p4, uint32_t hdr, int16_t p10) {
    if (hdr != 0) W16(hdr + 0x12, RU16(hdr + 0x12) & 0xfffd);
    return 0;
}
PGLUE(midiOutUnprepareHeader, 8, 2, midiOutUnprepareHeader(R16(sp), R32(sp + 2), R16(sp + 6)))

/* 0003e8be: midiOutShortMsg(msg, device): feed a short message to the synth parser */
uint32_t midiOutShortMsg(uint32_t msg, int16_t dev) {
    ENTRY_LINK(FN_midiOutShortMsg, 8, 12);             /* link -8; movem D6-D7/A4 */
    W32(a6 - 8, ConvertLong2PC(msg));
    int16_t r = 0x43;
    W8(gMidiOutBusy, R8(gMidiOutBusy) + 1);
    if (R8(gMidiOutOpen) != 0 && R8(gMidiOutBusy) == 1) {
        r = 0;
        maddr m = a6 - 8;
        uint32_t n;
        int send = 1;
        if (R8(m) >= 0xf8) {
            n = 1;
        } else {
            W8(gSynthRemain, 0);
            if (R8(m) & 0x80) {
                if (R8(m) >= 0xf0) n = R8(gSysMsgLen + (R8(m) & 7));
                else n = R8(gChanMsgLen + ((R8(m) & 0x70) >> 4));
            } else if (R8(gSynthRunning) == 0) {
                r = 0; send = 0; n = 0;
            } else {
                n = (uint32_t)R8(gChanMsgLen + ((R8(gSynthRunning) & 0x70) >> 4)) - 1;
            }
        }
        if (send) synthMidiData(m, n);
    }
    W8(gMidiOutBusy, R8(gMidiOutBusy) - 1);
    return (uint16_t)r;
}
PGLUE(midiOutShortMsg, 6, 2, midiOutShortMsg(R32(sp), R16(sp + 4)))

/* 0003e97e: midiOutLongMsg(p4, hdr, device): feed a prepared MIDIHDR (0x40 = unprepared) */
uint32_t midiOutLongMsg(int16_t p4, uint32_t hdr, int16_t dev) {
    ENTRY_LINK(FN_midiOutLongMsg, 4, 8);               /* link -4; movem D7/A4 */
    int16_t r = 0x43;
    W8(gMidiOutBusy, R8(gMidiOutBusy) + 1);
    if (R8(gMidiOutOpen) != 0 && R8(gMidiOutBusy) == 1) {
        r = 0;
        if (R8(hdr + 0x13) & 2) {
            synthMidiData(R32(hdr), R32(hdr + 4));
            W16(hdr + 0x12, RU16(hdr + 0x12) | 1);
        } else {
            r = 0x40;
        }
    }
    W8(gMidiOutBusy, R8(gMidiOutBusy) - 1);
    return (uint16_t)r;
}
PGLUE(midiOutLongMsg, 8, 2, midiOutLongMsg(R16(sp), R32(sp + 2), R16(sp + 6)))

/* 0003e9e6: midiOutReset: all notes off (no result) */
uint32_t midiOutReset(int16_t p4) {
    ENTRY_LINK(FN_midiOutReset, 0, 0);
    FUN_0003d642();
    return 0;
}

/* 0003ea04: midiOutGetID: 0 */
uint32_t midiOutGetID(int16_t p4, int16_t p6, int16_t p8) { return 0; }
PGLUE(midiOutGetID, 6, 2, 0)

/* 0003ea22: midiOutCachePatches(op, patches, bank, device): op 1/2 cache, 3 query, 4 uncache.
 * For op 0 or op > 4 the result slot is not written. */
static int cache_written;
uint32_t midiOutCachePatches(int16_t op, uint32_t patches, int16_t bank, int16_t dev) {
    ENTRY_LINK(FN_midiOutCachePatches, 0, 12);         /* link 0; movem D6-D7/A4 */
    cache_written = 1;
    if (bank != 0) return 8;
    int16_t r = 0;
    if (op <= 0 || op > 4) { cache_written = 0; return 0; }
    if (op == 3) {
        _BlockMove(gPatchCache, patches, 0x100);
        return (uint16_t)r;
    }
    if (op != 4) {
        for (uint16_t i = 0; i < 0x80; i++) {
            if (R16(patches + i * 2) != 0) {
                if (R16(gPatchCache + i * 2) == 0 && MDH_LoadThisInstrument((int16_t)i) != 0) {
                    if (op == 1) { r = 7; goto uncache; }
                    MMB_SetMemory(patches + i * 2, (uint32_t)(0x80 - i) * 2, 0);
                    return 7;
                }
                W16(gPatchCache + i * 2, RU16(gPatchCache + i * 2) | RU16(patches + i * 2));
            }
        }
        return 0;
    }
uncache:
    FUN_0003d642();
    for (uint16_t i = 0; i < 0x80; i++)
        if (R16(patches + i * 2) != 0 && R16(gPatchCache + i * 2) != 0)
            MDH_UnloadThisInstrument((int16_t)i);
    MMB_SetMemory(patches, 0x100, 0);
    MMB_SetMemory(gPatchCache, 0x100, 0);
    return (uint16_t)r;
}
void glue_midiOutCachePatches(void) {
    maddr sp = emu_sp();
    uint32_t r = midiOutCachePatches(R16(sp), R32(sp + 2), R16(sp + 6), R16(sp + 8));
    if (cache_written) W16(sp + 10, r);
    emu_set_sp(sp + 10);
}

/* 0003eb54: midiOutCacheDrumPatches: not supported */
uint32_t midiOutCacheDrumPatches(int16_t p4, int16_t p6, int16_t p8, int16_t p10, int16_t p12) { return 8; }
PGLUE(midiOutCacheDrumPatches, 10, 2, 8)

/* ---- HS ("Headspace") sound system set-up ---- */

/* 0003eb7e: HS_SetupSystem(maxVoices, minVoices, mixLevel, quality): init the sound and
 * music system and switch to instrument playback; returns an OSErr (word) */
uint32_t HS_SetupSystem(int16_t p4, int16_t p6, int16_t p8, int16_t p10) {
    ENTRY_LINK(FN_HS_SetupSystem, 0, 8);               /* link 0; movem D6-D7 */
    int16_t err = (int16_t)0xff2f;
    if (R8(gHSInitialised) == 0) {
        err = S16(InitSoundMusicSystem(p4, p6, p8, p10));
        if (err == 0) {
            err = S16(MDH_PrepForInstrumentPlayback(p4, p6, p8));
            if (err == 0) {
                _FreeMem();
                _FreeMem();
                W8(gHSInitialised, 1);
            }
        }
    }
    if (err != 0) FUN_0004084c();
    return (uint16_t)err;
}

/* 0003ebf6: pause the music */
uint32_t FUN_0003ebf6(void) {
    ENTRY_NOLINK(FN_FUN_0003ebf6, 0);
    return FUN_00040888();
}

/* 0003ebfc: resume the music */
uint32_t seg11_03ebfc(void) {
    ENTRY_NOLINK(FN_seg11_03ebfc, 0);
    return FUN_000408ba();
}

/* 0003ec02: shut the sound system down */
uint32_t seg11_03ec02(void) {
    ENTRY_NOLINK(FN_seg11_03ec02, 0);
    if (R8(gHSInitialised) != 0) {
        FUN_0004084c();
        W8(gHSInitialised, 0);
    }
    return 0;
}

/* 0003ec12: WAVE_MAPPER (-1) becomes the first wave device that is not open (the
 * device number is stored back into the argument slot) */
uint32_t RemapWaveDevice(int16_t dev) {
    if (dev == -1) {
        for (int16_t i = 0; i < 2; i++)
            if (!(R8(gWaveDev + 0x28 + i * WAVEDEV_SIZE) & 0x20)) { dev = i; break; }
    }
    return (uint16_t)dev;
}
void glue_RemapWaveDevice(void) {
    maddr sp = emu_sp();
    int16_t in = R16(sp);
    uint16_t r = (uint16_t)RemapWaveDevice(in);
    W16(sp, r);
    /* D0.w = device; the high word is 0 after the search loop (D0 = i * 0x2a), else unchanged */
    emu_set_d0(in == -1 ? r : (cpu_get(R_D0) & 0xffff0000) | r);
}

/* 0003ec5c: PCM, mono, 8 bits, 2000 < rate < 27000 */
uint32_t SupportThisFormat(uint32_t fmt) {
    uint8_t ok = 0;
    if (R16(fmt + 2) == 1 && R16(fmt + 0xc) == 1 && R16(fmt) == 1 && R16(fmt + 0xe) == 8 &&
        R32(fmt + 4) > 0x7d0 && R32(fmt + 4) < 0x6978)
        ok = 1;
    return ok;
}

/* 0003ecbc: memset (count decremented in its own argument slot) */
uint32_t MMB_SetMemory(uint32_t dst, uint32_t n, int16_t v) {
    do {
        W8(dst, (uint8_t)((uint16_t)v >> 8));
        dst++;
    } while (--n != 0);
    return 0;
}
void glue_MMB_SetMemory(void) {
    maddr sp = emu_sp();
    MMB_SetMemory(R32(sp), R32(sp + 4), R16(sp + 8));
    W32(sp + 4, 0);
}

/* 0003ecea: is the header in the device's queue? */
uint32_t WaveHeaderInQueue(int16_t dev, uint32_t hdr) {
    maddr h = R32(gWaveDev + 0x14 + (int32_t)dev * WAVEDEV_SIZE);
    for (; h != 0; h = R32(h + 0x18))
        if (h == hdr) return 1;
    return 0;
}

/* 0003ed2c: append a header to the device's queue */
uint32_t AddWaveHeaderToQueue(int16_t dev, uint32_t hdr) {
    maddr d = WAVEDEV(dev);
    if (R32(d + 0x14) == 0) {
        W32(d + 0x14, hdr);
        W32(R32(d + 0x14) + 0x18, 0);
        W16(d + 0x26, 0);
        W32(d + 0x18, R32(d + 0x14));
    } else {
        maddr h = R32(d + 0x14);
        while (R32(h + 0x18) != 0) h = R32(h + 0x18);
        W32(h + 0x18, hdr);
        W16(d + 0x26, RU16(d + 0x26) + 1);
    }
    if (hdr != 0) {
        W32(hdr + 0x1c, (uint32_t)((int32_t)R16(d + 0x26) << 16));
        W32(hdr + 8, R32(d));
        W16(hdr + 0x12, RU16(hdr + 0x12) & 0xfffe);
        return 0x10;                                   /* D0 = shift count of the lsl */
    }
    return hdr;                                        /* 0 */
}

/* 0003edbc: sample-done callback of LowLevelPlaySample (Pascal): advance the device's
 * queue (loops included) and call the client's callback with WOM_DONE (0x3bd) */
uint32_t HS_CallbackForPlaySample(uint32_t snd) {
    ENTRY_LINK(FN_HS_CallbackForPlaySample, 0, 28);    /* link 0; movem D4-D7/A2-A4 */
    int16_t idx = R16(snd + 0x36);
    int16_t dev = R16(snd + 0x48);
    maddr d = WAVEDEV(dev);
    maddr h = R32(d + 0x14);
    if (h != 0) {
        for (int16_t i = 0; idx > i; i++)
            if (h != 0) h = R32(h + 0x18);
        if (h != 0) W16(h + 0x12, RU16(h + 0x12) & 0xfffe);
    }
    if (h != 0) {
        if (R32(d + 0x1c) == 0) {
            W32(d + 0x18, R32(h + 0x18));
        } else {
            int32_t loops = R16(d + 0x24);
            if (loops == 0) {
                W16(h + 0x12, RU16(h + 0x12) & 0xffdf);
                W32(d + 0x18, R32(h + 0x18));
                W32(d + 0x1c, 0);
                W32(d + 0x20, 0);
            } else {
                loops--;
                W16(d + 0x24, (uint16_t)loops);
                if (h == R32(d + 0x20)) W32(d + 0x18, R32(d + 0x1c));
                else if (R32(d + 0x1c) != R32(d + 0x20)) W32(d + 0x18, R32(h + 0x18));
            }
        }
    } else {
        W32(d + 0x18, 0);
    }
    if (R32(d + 0x18) == 0) W32(d + 0x14, 0);
    if (RU16(d + 0xc) & 3) {                           /* bftst (0xc,A2){14:2} */
        maddr cb = R32(d + 4);
        if (cb != 0) {
            W32(d + 0x10, R32(d + 0x10) + R32(h + 4));
            /* Pascal callback(device, WOM_DONE, instance, header, 0) */
            call_ptr(cb, "lllww", 0, h, R32(d + 8), 0x3bd, (uint16_t)dev);
        }
    }
    if (R8(d + 0x28) & 0x40) {
        W32(d + 0x18, 0);
        W32(d + 0x14, 0);
        W8(d + 0x28, R8(d + 0x28) & ~0x40);
    }
    return 0;
}

/* 0003eecc: sound VBL callback: start the next queued header of each open device */
uint32_t FUN_0003eecc(void) {
    ENTRY_NOLINK(FN_FUN_0003eecc, 24);                 /* movem D5-D7/A2-A4 */
    for (int32_t i = 0; i < 2; i++) {
        maddr d = gWaveDev + (uint32_t)i * WAVEDEV_SIZE;
        if (R8(d + 0x28) & 0x80) continue;             /* paused */
        if (R32(d + 0x18) == 0) continue;
        if (R8(R32(d + 0x18) + 0x13) & 1) continue;    /* already playing */
        W16(R32(d + 0x18) + 0x12, RU16(R32(d + 0x18) + 0x12) | 1);
        uint32_t data = R32(R32(d + 0x18)) - 6;
        uint32_t pos = R32(R32(d + 0x18) + 0x1c) >> 16;
        maddr snd = LowLevelPlaySample(data, R32(R32(d + 0x18) + 4), R32(R32(d + 0x18) + 8) << 16,
                                       (int16_t)pos, 0, 0, 0, 0, 0, JT_ADDR(FN_HS_CallbackForPlaySample));
        if (snd != 0) W32(snd + 0x46, (uint32_t)i);
        maddr h = R32(d + 0x18);
        if (R8(h + 0x13) & 4) {                        /* loop start */
            W32(d + 0x1c, h);
            if (!(R8(h + 0x13) & 0x20)) {
                W16(h + 0x12, RU16(h + 0x12) | 0x20);
                W16(d + 0x24, RU16(h + 0x16));
            }
        }
        if (R8(h + 0x13) & 8) W32(d + 0x20, h);        /* loop end */
    }
    return 0;
}

/* ---- waveOut* (Pascal; results go to the caller's slot) ---- */

/* 0003ef98: two wave out devices (stored in the caller's result slot; no Pascal pop) */
uint32_t waveOutGetNumDevs(void) { return 2; }
void glue_waveOutGetNumDevs(void) { W16(emu_sp(), 2); }

/* 0003efba: waveOutGetDevCaps(size, caps, device) */
uint32_t waveOutGetDevCaps(int16_t size, uint32_t caps, int16_t dev) {
    ENTRY_LINK(FN_waveOutGetDevCaps, 0, 4);            /* link 0; movem A4 */
    if ((uint16_t)dev >= 2) return 5;
    if (size != 0x30) return 9;
    W16(caps, 0x14b1);
    W16(caps + 2, 0);
    W16(caps + 4, 0);
    _BlockMove(gDeviceName, caps + 6, 0x10);
    W32(caps + 0x26, 0x11);
    W16(caps + 0x2a, 1);
    W32(caps + 0x2c, 0);
    return 0;
}
PGLUE(waveOutGetDevCaps, 8, 2, waveOutGetDevCaps(R16(sp), R32(sp + 2), R16(sp + 6)))

/* 0003f02c: not supported */
uint32_t waveOutGetVolume(int16_t p4, int16_t p6, int16_t p8) { return 8; }
PGLUE(waveOutGetVolume, 6, 2, 8)

/* 0003f050: not supported */
uint32_t waveOutSetVolume(int16_t p4, int16_t p6, int16_t p8) { return 8; }
PGLUE(waveOutSetVolume, 6, 2, 8)

/* 0003f074: waveOutOpen(flags, instance, callback, format, device, phwo) */
uint32_t waveOutOpen(uint32_t flags, uint32_t inst, uint32_t cb, uint32_t fmt, int16_t devarg, uint32_t phwo) {
    ENTRY_LINK(FN_waveOutOpen, 2, 12);                 /* link -2; movem D6-D7/A4 */
    int16_t r = 0;
    uint16_t dev = (uint16_t)RemapWaveDevice(devarg);
    if (dev > 2) return 2;
    if ((uint8_t)SupportThisFormat(fmt) == 0) return 0x20;
    if ((flags & 1) == 1) return 0;                    /* WAVE_FORMAT_QUERY */
    if (phwo == 0) return 5;
    maddr d = gWaveDev + (uint32_t)dev * WAVEDEV_SIZE;
    if (R8(d + 0x28) & 0x20) return 4;
    W16(phwo, dev);
    W16(d + 0x26, 0);
    W32(d + 0x14, 0);
    W32(d + 0x18, 0);
    W16(d + 0x24, 0);
    W32(d + 0x1c, 0);
    W32(d + 0x20, 0);
    W32(d + 0x10, 0);
    W8(d + 0x28, R8(d + 0x28) & ~0x80);
    W8(d + 0x28, R8(d + 0x28) & ~0x40);
    W32(d + 4, cb);
    W32(d + 8, inst);
    W32(d + 0xc, flags);
    W32(d, R32(fmt + 4));
    W8(d + 0x28, R8(d + 0x28) | 0x20);
    SetSoundVBCallBack(JT_ADDR(FN_FUN_0003eecc));
    return (uint16_t)r;
}
PGLUE(waveOutOpen, 0x16, 2, waveOutOpen(R32(sp), R32(sp + 4), R32(sp + 8), R32(sp + 12), R16(sp + 16), R32(sp + 18)))

/* 0003f150: waveOutClose(device) (0x21 = still playing) */
uint32_t waveOutClose(int16_t devarg) {
    ENTRY_LINK(FN_waveOutClose, 0, 16);                /* link 0; movem D4-D7 */
    uint16_t dev = (uint16_t)devarg;
    int16_t r;
    if (dev < 2) {
        maddr d = gWaveDev + (uint32_t)dev * WAVEDEV_SIZE;
        if (R32(d + 0x18) != 0) {
            r = 0x21;
        } else {
            W8(d + 0x28, R8(d + 0x28) | 0x80);
            W8(d + 0x28, R8(d + 0x28) & ~0x80);
            W32(d + 0x14, 0);
            W32(d + 0x18, 0);
            W8(d + 0x28, R8(d + 0x28) & ~0x20);
            r = 0;
        }
    } else {
        r = 2;
    }
    if (r == 0) {
        uint8_t any = 0;
        for (int16_t i = 0; i < 2; i++) any |= (R8(gWaveDev + 0x28 + i * WAVEDEV_SIZE) >> 5) & 1;
        if (any == 0) SetSoundVBCallBack(0);
    }
    return (uint16_t)r;
}
PGLUE(waveOutClose, 2, 2, waveOutClose(R16(sp)))

/* 0003f1fa: mark a WAVEHDR prepared */
uint32_t waveOutPrepareHeader(int16_t p4, uint32_t hdr, int16_t dev) {
    if ((uint16_t)dev >= 2) return 5;
    W16(hdr + 0x12, RU16(hdr + 0x12) | 2);
    return 0;
}
PGLUE(waveOutPrepareHeader, 8, 2, waveOutPrepareHeader(R16(sp), R32(sp + 2), R16(sp + 6)))

/* 0003f23a: mark a WAVEHDR unprepared */
uint32_t waveOutUnprepareHeader(int16_t p4, uint32_t hdr, int16_t dev) {
    if ((uint16_t)dev >= 2) return 5;
    W16(hdr + 0x12, RU16(hdr + 0x12) & 0xfffd);
    return 0;
}
PGLUE(waveOutUnprepareHeader, 8, 2, waveOutUnprepareHeader(R16(sp), R32(sp + 2), R16(sp + 6)))

/* 0003f27c: waveOutWrite(p4, hdr, device): queue a prepared header */
uint32_t waveOutWrite(int16_t p4, uint32_t hdr, int16_t devarg) {
    ENTRY_LINK(FN_waveOutWrite, 0, 12);                /* link 0; movem D6-D7/A4 */
    uint16_t dev = (uint16_t)devarg;
    if (dev >= 2) return 5;
    if ((R32(hdr + 0x10) & 2) != 2 || R32(hdr + 4) == 0) return 0x22;
    W32(hdr + 0x18, 0);
    if ((uint8_t)WaveHeaderInQueue((int16_t)dev, hdr) != 0) return 0x21;
    AddWaveHeaderToQueue((int16_t)dev, hdr);
    return 0;
}
PGLUE(waveOutWrite, 8, 2, waveOutWrite(R16(sp), R32(sp + 2), R16(sp + 6)))

/* 0003f2ee: waveOutPause(device) */
uint32_t waveOutPause(int16_t devarg) {
    ENTRY_LINK_S(FN_waveOutPause, 2, 0, 8);            /* link 0; movem D6-D7 */
    uint16_t dev = (uint16_t)devarg;
    if (dev >= 2) return 5;
    maddr d = gWaveDev + (uint32_t)dev * WAVEDEV_SIZE;
    if (!(R8(d + 0x28) & 0x80)) {
        W8(d + 0x28, R8(d + 0x28) | 0x80);
        if (R32(d + 0x18) != 0) EndSound((int16_t)(R32(R32(d + 0x18) + 0x1c) >> 16));
    }
    return 0;
}
PGLUE(waveOutPause, 2, 2, waveOutPause(R16(sp)))

/* 0003f362: waveOutRestart(device) (no range check) */
uint32_t waveOutRestart(int16_t devarg) {
    maddr d = gWaveDev + (uint32_t)(uint16_t)devarg * WAVEDEV_SIZE;
    if (!(R8(d + 0x28) & 0x80)) return 5;
    W8(d + 0x28, R8(d + 0x28) & ~0x80);
    return 0;
}
PGLUE(waveOutRestart, 2, 2, waveOutRestart(R16(sp)))

/* 0003f3a6: waveOutReset(device): stop, mark every header done, empty the queue */
uint32_t waveOutReset(int16_t devarg) {
    ENTRY_LINK(FN_waveOutReset, 0, 8);                 /* link 0; movem D7/A4 */
    uint16_t dev = (uint16_t)devarg;
    if (dev >= 2) return 5;
    waveOutPause((int16_t)dev);
    maddr d = gWaveDev + (uint32_t)dev * WAVEDEV_SIZE;
    for (maddr h = R32(d + 0x14); h != 0; h = R32(h + 0x18))
        W16(h + 0x12, RU16(h + 0x12) & 0xfffe);
    W32(d + 0x18, 0);
    W32(d + 0x14, 0);
    W8(d + 0x28, R8(d + 0x28) & ~0x40);
    W32(d + 0x10, 0);
    {   /* clr.w -(A7); move.w D7,-(A7); jsr waveOutRestart (the slot is never popped) */
        maddr sp = emu_sp() - 4;
        W16(sp + 2, 0); W16(sp, dev);
        emu_set_sp(sp);
        waveOutRestart((int16_t)dev);
    }
    return 0;
}
PGLUE(waveOutReset, 2, 2, waveOutReset(R16(sp)))

/* 0003f436: waveOutBreakLoop(device) */
uint32_t waveOutBreakLoop(int16_t devarg) {
    uint16_t dev = (uint16_t)devarg;
    if (dev >= 2) return 5;
    maddr d = gWaveDev + (uint32_t)dev * WAVEDEV_SIZE;
    W8(d + 0x28, R8(d + 0x28) | 0x40);
    return 0;
}
PGLUE(waveOutBreakLoop, 2, 2, waveOutBreakLoop(R16(sp)))

/* 0003f476: waveOutGetPosition(size, mmtime, device): TIME_SAMPLES (2) / TIME_BYTES (4) */
uint32_t waveOutGetPosition(int16_t size, uint32_t mmt, int16_t devarg) {
    uint16_t dev = (uint16_t)devarg;
    if (dev >= 2) return 5;
    int16_t type = R16(mmt);
    if (type == 2 || type == 4) {
        W32(mmt + 2, R32(gWaveDev + 0x10 + (uint32_t)dev * WAVEDEV_SIZE));
        return 0;
    }
    return 8;
}
PGLUE(waveOutGetPosition, 8, 2, waveOutGetPosition(R16(sp), R32(sp + 2), R16(sp + 6)))

/* 0003f500: not supported */
uint32_t waveOutGetPitch(int16_t p4, int16_t p6, int16_t p8) { return 8; }
PGLUE(waveOutGetPitch, 6, 2, 8)

/* 0003f522: not supported */
uint32_t waveOutSetPitch(int16_t p4, int16_t p6, int16_t p8) { return 8; }
PGLUE(waveOutSetPitch, 6, 2, 8)

/* 0003f544: not supported */
uint32_t waveOutGetPlaybackRate(int16_t p4, int16_t p6, int16_t p8) { return 8; }
PGLUE(waveOutGetPlaybackRate, 6, 2, 8)

/* 0003f56e: not supported */
uint32_t waveOutSetPlaybackRate(int16_t p4, int16_t p6, int16_t p8) { return 8; }
PGLUE(waveOutSetPlaybackRate, 6, 2, 8)

/* 0003f598: waveOutGetID(pid, device) */
uint32_t waveOutGetID(uint32_t pid, int16_t devarg) {
    if ((uint16_t)devarg >= 2) return 5;
    if (pid != 0) W16(pid, devarg);
    return 0;
}
PGLUE(waveOutGetID, 6, 2, waveOutGetID(R32(sp), R16(sp + 4)))

/* ---- music driver front end ---- */

/* 0003f5d4: driver time (selector 0xf) */
uint32_t FUN_0003f5d4(void) {
    ENTRY_NOLINK(FN_FUN_0003f5d4, 0);
    return drv2(0xf, 0);
}

/* 0003f5e4: set the driver's sync time (selector 0x10) */
uint32_t SetSyncTime(uint32_t t) {
    ENTRY_LINK(FN_SetSyncTime, 0, 0);
    return drv2(0x10, t);
}

/* 0003f608: A/UX version (Gestalt 'a/ux'), asked once; returns its low word */
uint32_t FUN_0003f608(void) {
    ENTRY_NOLINK(FN_FUN_0003f608, 8);                  /* movem D7/A4 */
    uint32_t d0 = 0xffffffff;                          /* moveq #-1,D0 */
    if (R32(gAUXVersion) == 0xffffffff) {
        W32(gAUXVersion, 0);
        int16_t err = S16(tc_Gestalt(gAUXVersion, 0x612f7578));   /* 'a/ux' */
        if ((err == -0x15ae || err == -0x15af) && (R8(0xb22) & 2))
            W32(gAUXVersion, 0x100);
        d0 = (uint32_t)((int32_t)R32(gAUXVersion) >> 8);
        W32(gAUXVersion, d0);
    }
    return (d0 & 0xffff0000) | RU16(gAUXVersionLo);
}

/* 0003f65c: decrypt one byte (byte argument in the high byte of its word) */
uint32_t Decrypt(int16_t p) {
    uint8_t b = (uint8_t)((uint16_t)p >> 8);
    uint16_t seed = RU16(gDecryptSeed);
    uint8_t out = (uint8_t)(b ^ (uint8_t)(seed >> 8));
    uint32_t prod = (uint32_t)(uint16_t)(b + seed) * 0xce6d;          /* mulu.w */
    uint16_t lo = (uint16_t)(prod + 0x58bf);                         /* addi.w */
    W16(gDecryptSeed, lo);
    return (prod & 0xffff0000) | (uint32_t)(lo & 0xff00) | out;
}

/* 0003f698: decrypt a driver code handle in place (from byte 7 on) */
uint32_t DecryptHandle(uint32_t h) {
    ENTRY_LINK(FN_DecryptHandle, 0, 12);               /* link 0; movem A2-A4 */
    if (h == 0) return 0;
    W16(gDecryptSeed, 0xdce5);
    _HLock(h);
    maddr p = DEREF(h);
    maddr end = tc_GetHandleSize(h) + p;
    for (p += 7; p < end; p++) W8(p, Decrypt((int16_t)(R8(p) << 8)));
    _HUnlock(h);
    return 0;
}

/* 0003f6f2: load and detach the driver code: 'Jnth' as is, or 'MDRV' (encrypted and
 * compressed); *err = 0, -108 (decompression failed) or -204 (no resource) */
uint32_t GetDriverCodeResource(int16_t id, uint32_t perr) {
    ENTRY_LINK(FN_GetDriverCodeResource, 0, 4);        /* link 0; movem A3 */
    W16(perr, 0);
    uint32_t h = _GetResource(0x4a6e7468, id);         /* 'Jnth' */
    if (h != 0) {
        _DetachResource(h);
    } else {
        h = _GetResource(0x4d445256, id);              /* 'MDRV' */
        if (h != 0) {
            _DetachResource(h);
            DecryptHandle(h);
            h = DecompressHandle(h);
            if (h == 0) W16(perr, 0xff94);
        } else {
            W16(perr, 0xff34);
        }
    }
    return h;
}

/* 0003f776: Sound Manager 3.0 or later present (SoundDispatch implemented and version >= 3) */
uint32_t IsNewSoundManagerInstalled(void) {
    ENTRY_LINK(FN_IsNewSoundManagerInstalled, 4, 12);  /* link -4; movem D7/A3-A4 */
    uint8_t r = 0;
    tb_begin(); cpu_set(R_D0, 0xa800); tb_trap(0xA746); maddr a4 = cpu_get(R_A0); tb_end();   /* GetToolTrapAddress */
    tb_begin(); cpu_set(R_D0, 0xa89f); tb_trap(0xA746); maddr a3 = cpu_get(R_A0); tb_end();   /* _Unimplemented */
    if (a3 != a4) {
        maddr sp = emu_sp();
        push32(0);
        cpu_set(R_D0, 0xc0008);                        /* SndSoundManagerVersion */
        tb_trap(0xA800);
        W32(a6 - 4, pop32());
        emu_set_sp(sp);
        if (R8(a6 - 4) >= 3) r = 1;
    }
    return r;
}

/* look up the song's MIDI data: 'SONG' id -> 'cmid' / 'MIDI' / 'Midi' (0 = no SONG) */
static uint32_t song_midi(int16_t id, int *have_song) {
    uint32_t h = _GetResource(0x534f4e47, id);         /* 'SONG' */
    *have_song = h != 0;
    if (h == 0) return 0;
    int16_t mid = R16(DEREF(h));
    h = _GetResource(0x636d6964, mid);                 /* 'cmid' */
    if (h == 0) h = _GetResource(0x4d494449, mid);     /* 'MIDI' */
    if (h == 0) h = _GetResource(0x4d696469, mid);     /* 'Midi' */
    return h;
}

/* 0003f7d4: MDH_LoadSong(id) */
uint32_t MDH_LoadSong(uint32_t id) {
    ENTRY_LINK(FN_MDH_LoadSong, 0, 16);                /* link 0; movem D5-D7/A4 */
    uint32_t err = 0;
    if (R8(gMusicPaused) != 0 || R16(gMusicMode) == MUSIC_INSTRUMENTS) return 0;
    int have;
    uint32_t h = song_midi((int16_t)id, &have);
    if (!have) return 0xffffff40;                      /* -192 resNotFound */
    if (h == 0) return 0xfffffaec;                     /* -1300 */
    if (R16(gCurSong) != -1) MusicDriverHandler(0, 2);
    err = drv2(1, id);
    if (err == 0) {
        W16(gCurSong, id);
        W8(gSongLoaded, 1);
    }
    return err;
}

/* 0003f89e: MDH_FreeSong */
uint32_t MDH_FreeSong(void) {
    ENTRY_LINK(FN_MDH_FreeSong, 0, 4);                 /* link 0; movem D7 */
    if (R8(gMusicPaused) == 0 && R16(gMusicMode) != MUSIC_INSTRUMENTS) drv1(7);
    return 0;
}

/* 0003f8d8: MDH_BeginSong(id, loop byte) */
uint32_t MDH_BeginSong(uint32_t id, int16_t p8) {
    ENTRY_LINK(FN_MDH_BeginSong, 0, 16);               /* link 0; movem D5-D7/A4 */
    uint32_t err = 0;
    if (R8(gMusicPaused) != 0 || R16(gMusicMode) == MUSIC_INSTRUMENTS) return 0;
    uint32_t flag = (uint16_t)p8 >> 8;
    int have;
    uint32_t h = song_midi((int16_t)id, &have);
    if (!have) err = 0xffffff40;
    else if (h == 0) err = 0xfffffaec;
    else {
        if (R8(gSongLoaded) != 0 && (int16_t)id == R16(gCurSong)) {
            drv2(0xd, flag);
            drv2(2, flag);
        } else {
            if (R16(gCurSong) != -1) MusicDriverHandler(0, 2);
            drv2(0xd, flag);
            err = drv3(0, id, flag);
        }
        if (R8(gFadeActive) != 0) drv2(0x13, (uint32_t)(int32_t)R16(gMasterFade));
        W16(gCurSong, id);
        W32(gSongStartTime, FUN_0003f5d4());
        W8(gSongLoaded, 0);
    }
    if (err != 0) drv1(7);
    return err;
}

/* 0003fa0e: BeginSongFromMemory(id, data, length, loop byte) */
uint32_t BeginSongFromMemory(int16_t id, uint32_t data, uint32_t len, int16_t p14) {
    ENTRY_LINK(FN_BeginSongFromMemory, 0xa, 4);        /* link -0xa; movem D6 */
    uint32_t d0 = cpu_get(R_D0);
    if (R8(gMusicPaused) == 0 && R16(gMusicMode) != MUSIC_INSTRUMENTS) {
        W32(gSongStartTime, FUN_0003f5d4());
        W8(gSongLoaded, 0);
        if (R16(gCurSong) != -1) MusicDriverHandler(0, 2);
        drv2(0xd, (uint16_t)p14 >> 8);
        if (R8(gFadeActive) != 0) drv2(0x13, (uint32_t)(int32_t)R16(gMasterFade));
        W16(gCurSong, id);
        W32(a6 - 0xa, data);
        W32(a6 - 6, len);
        W16(a6 - 2, id);
        d0 = drv2(0x25, a6 - 0xa);
        W16(gCurSong, id);
    }
    return d0 & 0xffff0000;                            /* move.w D6 (0),D0 */
}

/* 0003fac0: MDH_EndSong */
uint32_t MDH_EndSong(void) {
    ENTRY_LINK(FN_MDH_EndSong, 0, 4);                  /* link 0; movem D7 */
    if (R8(gMusicPaused) == 0 && R16(gMusicMode) != MUSIC_INSTRUMENTS && R16(gCurSong) != -1) {
        drv1(5);
        drv1(7);
        W16(gCurSong, 0xffff);
        W8(gSongLoaded, 0);
        W32(gSongStartTime, 0);
    }
    return 0;
}

/* 0003fb18: MDH_IsSongDone */
uint32_t MDH_IsSongDone(void) {
    ENTRY_LINK(FN_MDH_IsSongDone, 0, 4);               /* link 0; movem D7 */
    uint32_t done = R16(gCurSong) == -1 ? 1 : 0;
    if (R8(gMusicPaused) == 0 && R16(gMusicMode) != MUSIC_INSTRUMENTS && R16(gCurSong) != -1)
        done = drv1(4) != 0 ? 0 : 1;
    return done;
}

/* Call the driver and keep D1 too (MDH_GetErrors): the driver is entered through a native
 * "jump" thunk that sets a "capture" thunk as its return address. */
static uint32_t cap_d1;
static maddr cap_target;
static maddr cap_thunk_addr;
static void cap_return(void) {                         /* reached by the driver's rts */
    cap_d1 = cpu_get(R_D1);
    push32(trap_ret);                                  /* the hook popped the first argument */
    trap_ret = THUNK_BASE;                             /* finish the nested call */
}
static void cap_jump(void) {                           /* entered by call68_addr */
    push32(cap_thunk_addr);
    trap_ret = cap_target;
}
static uint32_t drv2_d1(uint32_t sel, uint32_t a, uint32_t *d1) {
    static maddr jump_addr;
    if (!jump_addr) { jump_addr = cpu_thunk(cap_jump, "seg11a_jump"); cap_thunk_addr = cpu_thunk(cap_return, "seg11a_capture_d1"); }
    maddr sp = emu_sp();
    push32(a); push32(sel);
    cap_target = R32(gDrvEntry);
    uint32_t r = call68_addr(jump_addr);
    emu_set_sp(sp);
    *d1 = cap_d1;
    return r;
}

/* 0003fb76: MDH_GetErrors(buf): error code; the message (Pascal string at D1) is copied
 * as a C string into buf */
uint32_t MDH_GetErrors(uint32_t buf) {
    ENTRY_LINK(FN_MDH_GetErrors, 0, 16);               /* link 0; movem D6-D7/A3-A4 */
    uint32_t r = 0;
    if (R8(gMusicPaused) == 0) {
        W16(gCurSong, 0xffff);
        uint32_t d1;
        int16_t e = (int16_t)drv2_d1(0xe, 0, &d1);
        r = (uint32_t)(int32_t)e;
        if (buf != 0) {
            maddr s = d1 + 1, d = buf;
            while (R8(s) != 0) { W8(d, R8(s)); d++; s++; }
            W8(d, 0);
        }
    }
    return r;
}

/* 0003fbd6: voices the machine can mix (by SysEnvirons machineType) */
uint32_t MaxVoiceLoad(void) {
    ENTRY_LINK(FN_MaxVoiceLoad, 0x10, 4);              /* link -0x10; movem D7 */
    uint32_t n = 0;
    if (S16(tc_SysEnvirons(a6 - 0x10, 2)) == 0) {
        int16_t mt = R16(a6 - 0xa);
        if (mt == 0) n = 0x10;
        else if (mt < 0) n = 0;
        else if (mt == 4) n = 8;
        else if (mt > 4) n = mt == 5 ? 0x10 : 0;
        else n = mt == 3 ? 6 : 3;
    }
    return n;
}

/* 0003fc32: MDH_PrepForInstrumentPlayback(maxVoices, minVoices, mix): switch the driver
 * to playing instruments (gMusicMode = 0x64) */
uint32_t MDH_PrepForInstrumentPlayback(int16_t maxv, int16_t minv, int16_t mix) {
    ENTRY_LINK(FN_MDH_PrepForInstrumentPlayback, 0, 4);   /* link 0; movem D6 */
    uint32_t err = 0;
    if (R8(gMusicPaused) != 0 || R16(gMusicMode) == MUSIC_INSTRUMENTS) return 0;
    if (maxv < minv || maxv > 0x10) return 0xffffffce;    /* -50 paramErr */
    err = (uint32_t)(int32_t)S16(SetupMIDIPort(0x80, 0, 0));
    if (err != 0) return err;
    err = (uint32_t)(int32_t)S16(ChangeSystemVoices(maxv, minv, mix));
    if (err != 0) return err;
    if (drv2(0x23, 0) != 0) return 0xffffffff;
    W16(gMusicMode, MUSIC_INSTRUMENTS);
    return 0;
}

/* 0003fcd0: MDH_EnableMIDIInput(maxVoices, minVoices, mix, input byte) */
uint32_t MDH_EnableMIDIInput(int16_t maxv, int16_t minv, int16_t mix, int16_t p10) {
    ENTRY_LINK(FN_MDH_EnableMIDIInput, 0, 4);          /* link 0; movem D6 */
    W32(a6 - 4, cpu_get(R_D6));                        /* saved D6: read below as stack data */
    uint32_t err = 0;
    if (R8(gMusicPaused) != 0 || R16(gMusicMode) == MUSIC_INSTRUMENTS) return 0;
    if (maxv < minv || maxv > 0x10) return 0xffffffce;
    err = (uint32_t)(int32_t)S16(SetupMIDIPort(0x80, 0, (int16_t)(p10 & 0xff00)));
    if (err != 0) return err;
    err = (uint32_t)(int32_t)S16(ChangeSystemVoices(maxv, minv, mix));
    /* the ChangeSystemVoices arguments stay pushed; its top long becomes the selector, so
     * the second argument is (mix << 16 | high word of the saved D6) */
    maddr sp = a6 - 4 - 6;
    W16(sp + 4, mix);
    W32(sp, 0x19);
    emu_set_sp(sp);
    uint32_t r = call68_addr(R32(gDrvEntry));
    emu_set_sp(a6 - 4);
    if (r != 0) return 0xffffffff;
    W16(gMusicMode, MUSIC_INSTRUMENTS);
    return err;
}

/* 0003fd60: MDH_ChangeOutputQuality(q): driver selector 0x18 with the rate/voices code */
uint32_t MDH_ChangeOutputQuality(uint32_t q) {
    ENTRY_LINK(FN_MDH_ChangeOutputQuality, 0, 4);      /* link 0; movem D7 */
    static const uint16_t code[8] = { 0, 0, 0xb, 0x16, 0x10b, 0x116, 0x20b, 0x216 };
    uint32_t r = 0;
    if (R8(gMusicPaused) != 0) return 0;
    if (q > 7 || q == 0) return 0xffffffce;
    drv2(0x18, code[q]);
    return r;
}

/* 0003fe34: MDH_SetMasterFade(level 0..0x100): set it and wait 5 driver ticks */
uint32_t MDH_SetMasterFade(uint32_t level) {
    ENTRY_LINK(FN_MDH_SetMasterFade, 0, 8);            /* link 0; movem D6-D7 */
    int32_t v = (int32_t)level;
    if (R8(gMusicPaused) != 0) return 0;
    W8(gFadeActive, 0);
    if (v == (int32_t)R16(gMasterFade)) return 0;
    if (v > 0x100) v = 0x100;
    if (v < 0) v = 0;
    maddr sp = emu_sp();
    drv2_keep(0x13, (uint32_t)(int32_t)(int16_t)v);
    W16(gMasterFade, v);
    int32_t until = (int32_t)FUN_0003f5d4() + 5;       /* still with the driver arguments pushed */
    emu_set_sp(sp);
    while (until > (int32_t)FUN_0003f5d4()) {}
    return 0;
}

/* 0003fea6: MDH_AutoFadeSoundIn(ticks): fade in from 0 over the given VBL count */
uint32_t MDH_AutoFadeSoundIn(uint32_t ticks) {
    ENTRY_LINK(FN_MDH_AutoFadeSoundIn, 0, 4);          /* link 0; movem D7 */
    if (R8(gMusicPaused) != 0) return 0;
    maddr sp = emu_sp();
    drv2_keep(0x13, 0);
    int32_t until = (int32_t)FUN_0003f5d4() + 5;
    emu_set_sp(sp);
    while (until > (int32_t)FUN_0003f5d4()) {}
    W8(gFadeOut, 0);
    W8(gFadeActive, 1);
    W8(gFadeDone, 0);
    W16(gMasterFade, 0);
    int32_t t16 = (int16_t)(uint16_t)ticks;            /* movea.w (0xa,A6): low word */
    W16(gFadeStep, (uint16_t)(0x400 / t16));
    W16(gFadeTicks, (uint16_t)((int32_t)ticks / 4));
    W16(gFadeCounter, (uint16_t)((int32_t)ticks / 4));
    return 0;
}

/* 0003ff24: MDH_AutoFadeSoundOut(ticks) */
uint32_t MDH_AutoFadeSoundOut(uint32_t ticks) {
    if (R8(gMusicPaused) != 0) return 0;
    W8(gFadeOut, 1);
    W8(gFadeActive, 1);
    W8(gFadeDone, 0);
    W16(gMasterFade, 0x100);
    int32_t t16 = (int16_t)(uint16_t)ticks;
    W16(gFadeStep, (uint16_t)(0x400 / t16));
    W16(gFadeTicks, (uint16_t)((int32_t)ticks / 4));
    W16(gFadeCounter, (uint16_t)((int32_t)ticks / 4));
    return 0;
}

/* 0003ff86: MDH_IsSoundDone(snd) */
uint32_t MDH_IsSoundDone(uint32_t snd) {
    ENTRY_LINK(FN_MDH_IsSoundDone, 0, 4);              /* link 0; movem D7 */
    uint32_t r = 0;
    if (snd != 0 && R8(gMusicPaused) == 0) r = drv2(0x14, snd);
    return r;
}

/* 0003ffc6: MDH_PlaySoundData(snd) */
uint32_t MDH_PlaySoundData(uint32_t snd) {
    ENTRY_LINK(FN_MDH_PlaySoundData, 0, 0);
    if (snd != 0 && R8(gMusicPaused) == 0) drv2(0x11, snd);
    return 0;
}

/* 0003fffe: MDH_StopSoundData(snd) */
uint32_t MDH_StopSoundData(uint32_t snd) {
    ENTRY_LINK(FN_MDH_StopSoundData, 0, 0);
    if (snd != 0 && R8(gMusicPaused) == 0) drv2(0x12, snd);
    return 0;
}

/* 00040036: MDH_SetupCallbackProc(p) */
uint32_t MDH_SetupCallbackProc(uint32_t p) {
    ENTRY_LINK(FN_MDH_SetupCallbackProc, 0, 0);
    if (p != 0 && R8(gMusicPaused) == 0) drv2(0x1b, p);
    return 0;
}

/* 00040072: MDH_ChangeSamplePitch(snd) */
uint32_t MDH_ChangeSamplePitch(uint32_t snd) {
    ENTRY_LINK(FN_MDH_ChangeSamplePitch, 0, 0);
    if (snd != 0 && R8(gMusicPaused) == 0) drv2(0x17, snd);
    return 0;
}

/* 000400ae: MDH_LoadThisInstrument(n) (instrument mode only); always 0 */
uint32_t MDH_LoadThisInstrument(int16_t n) {
    ENTRY_LINK(FN_MDH_LoadThisInstrument, 0, 0);
    if (R8(gMusicPaused) == 0 && R16(gMusicMode) == MUSIC_INSTRUMENTS) drv2(0x22, (uint32_t)(int32_t)n);
    return 0;
}

/* 000400f2: MDH_UnloadThisInstrument(n); always 0 */
uint32_t MDH_UnloadThisInstrument(int16_t n) {
    ENTRY_LINK(FN_MDH_UnloadThisInstrument, 0, 0);
    if (R8(gMusicPaused) == 0 && R16(gMusicMode) == MUSIC_INSTRUMENTS) drv2(0x24, (uint32_t)(int32_t)n);
    return 0;
}

/* 00040138: MDH_PlayNote(note record) */
uint32_t MDH_PlayNote(uint32_t n) {
    ENTRY_LINK(FN_MDH_PlayNote, 0, 0);
    if (n == 0) return 0;
    if (RU16(n + 2) > 0x80) _DebugStr(gPlayNoteErr);
    else drv2(9, n);
    return 0;
}

/* 0004017a: MDH_StopNote(note record) */
uint32_t MDH_StopNote(uint32_t n) {
    ENTRY_LINK(FN_MDH_StopNote, 0, 0);
    if (n == 0) return 0;
    if (RU16(n + 2) > 0x80) _DebugStr(gStopNoteErr);
    else drv2(0xc, n);
    return 0;
}

/* 000401bc: all notes off (selector 6) */
uint32_t FUN_000401bc(void) {
    ENTRY_NOLINK(FN_FUN_000401bc, 0);
    return drv1(6);
}

/* song query (driver selector) with no current song during the call */
static uint32_t song_query(uint32_t sel) {
    uint32_t r = 0;
    if (R8(gMusicPaused) == 0 && R16(gMusicMode) != MUSIC_INSTRUMENTS && R16(gCurSong) != -1) {
        uint16_t cur = RU16(gCurSong);
        W16(gCurSong, 0xffff);
        r = drv2(sel, 0);
        W16(gCurSong, cur);
    }
    return r;
}
/* 000401ca */
uint32_t FUN_000401ca(void) { ENTRY_NOLINK(FN_FUN_000401ca, 8); return song_query(0x1c); }
/* 0004020c */
uint32_t FUN_0004020c(void) { ENTRY_NOLINK(FN_FUN_0004020c, 8); return song_query(0x1e); }
/* 0004024e */
uint32_t FUN_0004024e(void) { ENTRY_NOLINK(FN_FUN_0004024e, 8); return song_query(0x20); }
/* 00040290 */
uint32_t FUN_00040290(void) { ENTRY_NOLINK(FN_FUN_00040290, 8); return song_query(0x1d); }
/* 000402d2 */
uint32_t FUN_000402d2(void) { ENTRY_NOLINK(FN_FUN_000402d2, 8); return song_query(0x1f); }

/* 00040314: no-op */
uint32_t MDH_SetMIDIClock(void) { return 0; }

/* 00040330: time since the song started (driver time, >= 0) */
uint32_t FUN_00040330(void) {
    ENTRY_NOLINK(FN_FUN_00040330, 4);                  /* movem D7 */
    int32_t t = 0;
    if (R8(gMusicPaused) == 0 && R16(gMusicMode) != MUSIC_INSTRUMENTS && R16(gCurSong) != -1) {
        t = (int32_t)(FUN_0003f5d4() - R32(gSongStartTime));
        if (t < 0) t = 0;
    }
    return (uint32_t)t;
}

/* 00040360: MusicDriverHandler(arg, selector) (Pascal, long result): dispatcher of the
 * music/sound requests (0 begin song, 1 begin looped, 2 end, 3 done?, 0x65..0x69 sound
 * data, 0xc8/0xc9 notes, ...). -50 when no music mode is set. */
uint32_t MusicDriverHandler(uint32_t arg, int16_t sel) {
    ENTRY_LINK_S(FN_MusicDriverHandler, 4, 0, 8);      /* link 0; movem D6-D7 */
    if (R16(gMusicMode) == 0) return 0xffffffce;
    uint32_t r = 0;
    switch (sel) {
    case 0:  r = MDH_BeginSong(arg, 0); break;
    case 1:  r = MDH_BeginSong(arg, 0x100); break;
    case 2:  WITH_PUSH32(arg, r = MDH_EndSong()); break;
    case 3:  WITH_PUSH32(arg, r = MDH_IsSongDone()); break;
    case 4:  r = FUN_00040330(); break;
    case 5:  W8(gMusic_f0, arg); break;
    case 6:  W8(gMusic_f1, arg); break;
    case 7:  drv2(0xd, arg & 0xff); break;
    case 10: MDH_AutoFadeSoundOut(arg); break;
    case 11: MDH_AutoFadeSoundIn(arg); break;
    case 12: r = (uint32_t)(int32_t)R16(gMasterFade); break;
    case 13: MDH_SetMasterFade(arg); break;
    case 14: r = MDH_ChangeOutputQuality(arg); break;
    case 15: r = MDH_LoadSong(arg); break;
    case 16: WITH_PUSH32(arg, r = MDH_FreeSong()); break;
    case 17: r = MDH_GetErrors(arg); break;
    case 18: r = FUN_000401ca(); break;
    case 19: r = FUN_0004020c(); break;
    case 20: r = FUN_0004024e(); break;
    case 21: WITH_PUSH32(arg, r = MDH_SetMIDIClock()); break;
    case 22: r = FUN_00040290(); break;
    case 23: r = FUN_000402d2(); break;
    case 100: MDH_PlaySoundData(arg); break;
    case 101: MDH_StopSoundData(arg); break;
    case 102: r = MDH_IsSoundDone(arg); break;
    case 103:
        if (R8(gMusicPaused) == 0) { W8(gFadeActive, 0); drv1(0x16); }
        break;
    case 104: MDH_ChangeSamplePitch(arg); break;
    case 105: MDH_SetupCallbackProc(arg); break;
    case 200: MDH_PlayNote(arg); break;
    case 201: MDH_StopNote(arg); break;
    case 202: FUN_000401bc(); break;
    case 203: r = MDH_EnableMIDIInput((int16_t)(arg >> 16), (int16_t)arg, 0, 0x100); break;
    default: break;
    }
    return r;
}
PGLUE(MusicDriverHandler, 6, 4, MusicDriverHandler(R32(sp), R16(sp + 4)))

/* 00040610: ChangeSystemVoices(maxVoices, minVoices, mix) (driver selector 0x15) */
uint32_t ChangeSystemVoices(int16_t maxv, int16_t minv, int16_t mix) {
    ENTRY_LINK(FN_ChangeSystemVoices, 6, 0);           /* link -6 */
    if (R16(gMusicMode) == 0) return 0xffffffce;
    if (R8(gMusicPaused) != 0) return (cpu_get(R_D0) & 0xffff0000) | 0xff2d;   /* move.w #-211,D0 */
    W16(a6 - 6, maxv);
    W16(a6 - 4, minv);
    W16(a6 - 2, mix);
    drv2(0x15, a6 - 6);
    return 0;
}

/* 00040666: InitSoundMusicSystem(maxVoices, minVoices, mix, quality 1..7): load the music
 * driver, init the sound system and the driver; returns an OSErr (word) */
uint32_t InitSoundMusicSystem(int16_t maxv, int16_t minv, int16_t mix, int16_t quality) {
    ENTRY_LINK(FN_InitSoundMusicSystem, 0x20, 12);     /* link -0x20; movem D5-D7 */
    if (thunk_FUN_00042bb2() == 0) return 0x4e22;
    int16_t err = S16(tc_SysEnvirons(a6 - 0x12, 1));
    int16_t aux = S16(FUN_0003f608());
    if (err != 0 && R16(a6 - 0xe) < 0x600 && aux == 0) err = 0x66;
    if (maxv < minv) err = -50;
    if ((uint8_t)IsNewSoundManagerInstalled() == 0 && R8(0x27e) != 0) err = (int16_t)0xff2f;
    W16(gMusicMode, 0);
    W8(gFadeActive, 0);
    if (err == 0) {
        int16_t id = 0xb;
        uint32_t code;
        switch ((uint16_t)quality) {
        case 1: W16(gMusicMode, 1); code = 0; break;
        case 2: W16(gMusicMode, 2); code = 0xb; break;
        case 3: W16(gMusicMode, 3); code = 0x16; break;
        case 4: W16(gMusicMode, 4); code = 0x10b; break;
        case 5: W16(gMusicMode, 5); code = 0x116; break;
        case 6: W16(gMusicMode, 6); code = 0x20b; break;
        case 7: W16(gMusicMode, 7); code = 0x216; break;
        default: id = -1; W16(gMusicMode, 0); code = 0xffffffff; break;
        }
        if (id == -1) {
            err = -50;
        } else {
            uint32_t h = GetDriverCodeResource(id, a6 - 2);
            W32(gDrvHandle, h);
            err = R16(a6 - 2);
            if (h != 0) {
                _MoveHHi(R32(gDrvHandle));
                _HNoPurge(R32(gDrvHandle));
                _HLock(R32(gDrvHandle));
                W32(gDrvEntry, DEREF(R32(gDrvHandle)));
                W16(gCurSong, 0xffff);
                W32(gSongStartTime, 0);
                W16(gMasterFade, 0x100);
                W8(gMusicPaused, 0);
                W8(gMusic_e7, 0);
                W32(gSongDoneProc, 0);
                err = S16(InitSoundSystem(mix));
                if (err == 0) {
                    W16(a6 - 0x18, maxv);
                    W16(a6 - 0x16, minv);
                    W16(a6 - 0x14, mix);
                    err = (int16_t)drv2(0x15, a6 - 0x18);
                    if (err != 0) {
                        err = -50;
                    } else {
                        W32(a6 - 0x20, 0);
                        W32(a6 - 0x1c, 0);
                        MusicDriverHandler(a6 - 0x20, 0x69);
                        err = (int16_t)drv2(0x18, code);
                        if (err == 0) {
                            W8(gMusic_f0, 1);
                            W8(gMusic_f1, 0);
                        } else {
                            err = -50;
                        }
                    }
                }
            }
        }
    }
    return (uint16_t)err;
}

/* 0004084c: shut the music down: MIDI ports, sound system, end song, dispose the driver */
uint32_t FUN_0004084c(void) {
    ENTRY_NOLINK(FN_FUN_0004084c, 0);
    FUN_0003dbc2();
    if (R32(gDrvEntry) == 0) return 0;
    FUN_00041ae8();
    W8(gMusicPaused, 0);
    MusicDriverHandler(0, 2);
    drv1(8);
    _HUnlock(R32(gDrvHandle));
    _DisposeHandle(R32(gDrvHandle));
    W32(gDrvHandle, 0);
    return 0;
}

/* 00040888: pause the music (selector 0xa), remember the driver time */
uint32_t FUN_00040888(void) {
    ENTRY_NOLINK(FN_FUN_00040888, 0);
    if (R16(gMusicMode) == 0) return 0xffffffce;
    FUN_00041b2e();
    if (R8(gMusicPaused) == 0) {
        maddr sp = emu_sp();
        drv1_keep(0xa);
        W8(gMusicPaused, 1);
        W32(gPauseTime, FUN_0003f5d4());               /* with the driver argument still pushed */
        emu_set_sp(sp);
    }
    return 0;
}

/* 000408ba: resume the music (selector 0xb); the song start time skips the pause */
uint32_t FUN_000408ba(void) {
    ENTRY_NOLINK(FN_FUN_000408ba, 4);                  /* movem D7 */
    if (R16(gMusicMode) == 0) return 0xffffffce;
    int16_t err = 0;
    if ((uint8_t)IsNewSoundManagerInstalled() == 0 && R8(0x27e) != 0) err = (int16_t)0xff2f;
    if (err == 0) {
        FUN_00041b76();
        if (R8(gMusicPaused) != 0) {
            maddr sp = emu_sp();
            drv1_keep(0xb);
            W8(gMusicPaused, 0);
            FUN_0003d67e();                            /* with the driver argument still pushed */
            uint32_t now = FUN_0003f5d4();
            W32(gSongStartTime, R32(gSongStartTime) + (now - R32(gPauseTime)));
            emu_set_sp(sp);
        }
    }
    return (uint16_t)err;
}

/* 00040912: VBL step of the automatic fade in/out */
uint32_t AutoFadeHandler(void) {
    ENTRY_LINK(FN_AutoFadeHandler, 0, 0);
    W16(gFadeCounter, RU16(gFadeCounter) - 1);
    if (R8(gFadeOut) == 1) {
        if (R8(gFadeDone) == 0) {
            if (R16(gFadeCounter) >= 0) return 0;
            W16(gFadeCounter, RU16(gFadeTicks));
            if (R16(gMasterFade) < 0) {
                W8(gFadeDone, 1);
                MusicDriverHandler(0, 2);
                drv1(6);
                drv2(0x13, 0);
                W16(gMasterFade, 0);
            } else {
                W16(gMasterFade, RU16(gMasterFade) - RU16(gFadeStep));
                if (R16(gMasterFade) >= 0) drv2(0x13, (uint32_t)(int32_t)R16(gMasterFade));
            }
        } else {
            W8(gFadeActive, 0);
            maddr sp = emu_sp();
            drv2_keep(0x13, 0x100);
            W16(gMasterFade, 0x100);
            FUN_0004239a();                            /* with the driver arguments still pushed */
            emu_set_sp(sp);
        }
    } else {
        if (R8(gFadeDone) == 0) {
            if (R16(gFadeCounter) >= 0) return 0;
            W16(gFadeCounter, RU16(gFadeTicks));
            if (R16(gMasterFade) > 0x100) {
                W8(gFadeDone, 1);
            } else {
                W16(gMasterFade, RU16(gMasterFade) + RU16(gFadeStep));
                if (R16(gMasterFade) < 0x100) drv2(0x13, (uint32_t)(int32_t)R16(gMasterFade));
            }
        } else {
            W8(gFadeActive, 0);
            drv2(0x13, 0x100);
            W16(gMasterFade, 0x100);
        }
    }
    return 0;
}

/* 00040a2c: music VBL task (from DoSoundVB): fades, end-of-song callback, live notes */
uint32_t HandleMusicVBLTask(uint32_t p) {
    ENTRY_LINK(FN_HandleMusicVBLTask, 0, 0);
    if (R8(gFadeActive) != 0) WITH_PUSH32(p, AutoFadeHandler());
    if (R8(gMusicPaused) == 0 && R16(gMusicMode) != MUSIC_INSTRUMENTS && R16(gCurSong) != -1 &&
        drv1(4) == 0 && R32(gSongDoneProc) != 0) {
        /* move.w song,-(A7); jsr (proc): the word is not popped before the end */
        push16(RU16(gCurSong));
        call68_addr(R32(gSongDoneProc));
        W16(gCurSong, 0xffff);
        W32(gSongStartTime, 0);
    }
    if (R16(gMusicMode) == MUSIC_INSTRUMENTS) HandleLiveMusicVBLTask(p);
    return 0;
}

/* 00040aac: SetSongDoneCallBack(proc) */
uint32_t SetSongDoneCallBack(uint32_t proc) {
    W32(gSongDoneProc, proc);
    return 0;
}
