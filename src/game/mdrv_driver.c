/* The SoundMusicSys music driver (Halestorm, Steve Hales): code resource 'MDRV' 11.
 *
 * InitSoundMusicSystem (seg11a) loads the resource, decrypts and decompresses it
 * (GetDriverCodeResource) and calls it through gDrvEntry as drv(selector, arg) with the
 * C convention. The original is hand-written 68k assembly: a software MIDI sequencer and
 * a sample mixer that feeds a Sound Manager 3 double buffer (or, on older machines, the
 * hardware sound buffer from a VBL task, or a PAS16 sound card).
 *
 * Layout of the decompressed code (Ghidra program mdrv11.bin, base 0):
 *   0x0000          entry: selector dispatch (38 selectors, a table of bra at 0x3c)
 *   0x0080a         Sound Manager doubleBack procedure (interrupt level)
 *   0x00924         VBL task for the hardware sound buffer (old Macs; through a stub)
 *   0x03c78/0x3cb8  PAS16 sound card completion / interrupt routines
 *   0x04600..       globals and constant data, reached as (d,A4) with A4 = code + 0x4600
 *
 * Translation conventions (see docs/notes/mdrv.md):
 * - internal routines are static functions; register arguments become C parameters,
 *   the carry/extend flag returned with "andi #$ee,CCR" / "ori #$11,CCR" is the int
 *   result (1 = error);
 * - every internal routine starts with SUB(saved bytes): the emulated SP goes down like the
 *   original's bsr + movem, so Toolbox calls and 68k callbacks run at the original depth;
 * - globals are addressed from the code base: G(off) is the original (off,A4).
 */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* ---- base addresses ---- */
#define MDRV_BASE()  fn_address(FN_MDRV_Entry)     /* start of the decompressed code */
static maddr code_;                                /* MDRV_BASE() of the current entry */
static maddr a4_;                                  /* code + 0x4600: the driver's A4 */
#define G(off)       (a4_ + (uint32_t)(off))
#define CODE(off)    (code_ + (uint32_t)(off))
static void set_base(void) { code_ = MDRV_BASE(); a4_ = code_ + 0x4600; }

/* ---- globals: (offset,A4) ---- */
#define gSelector     0x0000   /* long: selector of the current call */
#define gArg          0x0004   /* long: argument of the current call */
#define gResult       0x0008   /* word: result / error code returned in D0 */
#define gErrMsg       0x000a   /* long: Pascal string of the last error */
#define gErrValue     0x000e   /* word: value that came with the last error */
#define sInvalidRate  0x0010   /* "Invalid rate!" */
#define gSpeedByte    0x001e   /* byte: left over by the CPU speed test */
#define gInterpTab1   0x0022   /* long: 64K table (s * (256-f)) >> 8 for interpolation */
#define gInterpTab2   0x0026   /* long: 64K table (s * f) >> 8 */
#define gInterpBlock  0x002a   /* long: the 128K block of both tables */
#define gInterpMode   0x002e   /* word (byte at 0x2e set): 0 none, 1 average, -1 interpolate */
#define gVolLevels    0x0030   /* long: 128 amplitude tables of 256 bytes */
#define gSongActive   0x0034   /* word: the sequencer runs */
#define gSongLoop     0x0036   /* word: restart the song at its end */
#define gHWOpen       0x0038   /* word: the output is open (VBL or sound channel) */
#define gVBLTask      0x003a   /* VBLTask (14 bytes) for the hardware output */
#define gVBLFlag50    0x0050   /* word: set to -1 when the VBL task is installed */
#define gHWActive     0x0052   /* word: the VBL task owns the sound hardware */
#define gSndChan      0x0056   /* long: SndChannelPtr of the Sound Manager output */
#define gSavedZone    0x005a   /* long: TheZone when the VBL task was installed */
#define gBufFrames    0x005e   /* word: mixed frames per buffer (370 or 185) */
#define gBufFramesM1  0x0060   /* word: gBufFrames - 1 (dbra count) */
#define gBuf5M1       0x0062   /* word: gBufFrames / 5 - 1 */
#define gClear5M1     0x0064   /* word: dbra count of the 10-word clear loops */
#define gFullRate     0x0066   /* word: 1 = 22 kHz mixing, 0 = 11 kHz (doubled on output) */
#define gSongPtr      0x0068   /* long: locked 'SONG' data */
#define gMidiPtr      0x006c   /* long: locked MIDI file data */
#define gInstLoaded   0x0070   /* word */
#define gInstTable    0x0072   /* 128 x { long INST data, long sample data } */
#define gInstUsed     0x0482   /* 128 bytes: instrument used by the song (scan) */
#define gSndPtrs      0x0582   /* longs: locked 'snd ' data, parallel to gSndIds */
#define gSndIds       0x0ee2   /* words: 'snd ' ids (0 free, -1 end, -2 private copy) */
#define gMixCount     0x13a2   /* long: buffers mixed */
#define gSongTicks0   0x13a6   /* long: Ticks when the song started */
#define gSong13aa     0x13aa   /* long */
#define gSongLock     0x13ce   /* word: nonzero = no song may start */
#define gSongFlags    0x13d0   /* word: song flags (high byte bits are tested with btst) */
#define gSingleInst   0x13d2   /* word: instrument of every note (flag bit 0) */
#define gMaxUsed      0x13d4   /* word: most voices used at once */
#define gNoteVoices   0x13d6   /* word: voices for notes */
#define gMixLevel     0x13d8   /* word: voices mixed at full level */
#define gSampleVoices 0x13da   /* word: voices for sound effects (after the note voices) */
#define gCmdFlush     0x13dc   /* SndCommand flushCmd */
#define gCmdQuiet     0x13e4   /* SndCommand quietCmd */
#define gDBHeader     0x1404   /* SndDoubleBufferHeader */
#define gDBBack       0x1418   /*   .dbhDoubleBack */
#define gDBBuf0       0x141c   /* SndDoubleBuffer 0 (370 frames) */
#define gDBBuf1       0x15a0   /* SndDoubleBuffer 1 */
#define gMixBuf       0x1724   /* 375 words: the mix of one buffer */
#define gPaused       0x1a40   /* word */
#define gLastTicks    0x1a42   /* long: Ticks at the previous VBL (overload check) */
#define gErrPending   0x1a46   /* word (byte st): an error was reported */
#define gFlags2       0x1a48   /* byte: instrument flags */
#define gVBLStub      0x1a90   /* long: system-heap stub that jumps to the VBL task */
#define gInVBL        0x1a94   /* word: VBL task busy */
#define gTickCount    0x1a96   /* long: buffers/VBLs since start (selector 0xf) */
#define gTimeStep     0x1b70   /* long: song time per buffer */
#define gDivision     0x1b74   /* long: MIDI division */
#define gTempoQ       0x1b78   /* long: tempo / gTempoDiv (word at 0x1b7a used) */
#define gTempo        0x1b7c   /* long: microseconds per quarter note */
#define gTempoDiv     0x1b80   /* word */
#define gTSNum        0x1b82   /* word: time signature */
#define gTSLen        0x1b84   /* long */
#define gSongLength   0x1b88   /* long */
#define gSongTime     0x1b8c   /* long */
#define gTranspose    0x1b90   /* word */
#define gTrackBusy    0x1b92   /* word: a track was active in this tick */
/* 24 tracks, fields are arrays of 24 longs: field + 4 * track */
#define tLength       0x1b94
#define tStart        0x1bf4
#define tState        0x1c54   /* byte: 'F' first event, 'R' running, 0 done */
#define tField1cb4    0x1cb4
#define tProgram      0x1d14   /* word: program change (or track number) */
#define tField1d74    0x1d74
#define tSongId       0x1dd4   /* word */
#define tField1e34    0x1e34
#define tPtr          0x1e94
#define tDelta        0x1ef4
#define tRunStatus    0x1f54   /* byte */
#define tChannel      0x1fb4   /* word */
#define gChanVolume   0x2014   /* 16 bytes */
#define gChanProgram  0x2024   /* 24 bytes */
#define gSongId       0x2034   /* word */
#define gTrackCount   0x2036   /* word */
#define gTrackNum     0x2038   /* word */
#define gTrackList    0x203a   /* { long start, long length } per MTrk */
#define gSeedInit     0x23b4   /* 17 words copied to gSeeds */
#define gSeeds        0x23d6   /* 17 words: additive random generator */
#define gSeedI        0x23f8   /* word */
#define gSeedJ        0x23fa   /* word */
#define gVolTable     0x248c   /* long: mix sum -> output byte */
#define gVolTabSize   0x2490   /* word */
#define gVolTabLevel  0x2492   /* word */
#define gVolTabSmp    0x2494   /* word */
#define gVoices       0x24f2   /* voice fields: arrays of 16 longs, field + 4 * voice */
#define gVoiceLoop    0x2bf2   /* word: voices left in the mix loop */
#define gPitchTable   0x2c56   /* longs: 16.16 step per note */
#define gDoneProc     0x2ecc   /* long: sample done callback */
#define gDoneRefCon   0x2ed0   /* long */
#define gScanMode     0x2ed4   /* word: 1 = note-ons only mark instruments as used */
#define gNoBend       0x2ed6   /* word 0x8000: empty pitch envelope */
#define nNote         0x2ed8   /* note-on temporaries */
#define nDuration     0x2eda
#define nVolume       0x2edc
#define nKey          0x2ede
#define nChannel      0x2ee0
#define nInst         0x2ee2
#define nFlags        0x2ee4   /* word (INST +6): bits tested on the high byte */
#define nFlags5       0x2ee6   /* byte (INST +5) */
#define nId           0x2ee8
#define nSplitTries   0x2eea
#define nOctave       0x2eec   /* byte at 0x2eec: +12 / -12 */
#define gCardPBName   0x2f4c   /* ".BD_PAS16" */
#define gCardBuf      0x2f56   /* long: 16K card buffer */
#define gCardPB       0x2f5a   /* parameter block (51 bytes cleared) */
#define gCardOut      0x2f9a   /* 370 bytes of card output */
#define gCardRef      0x3114   /* word: driver refNum */
#define gCardDMA      0x3116   /* long */
#define gCardBase     0x311a   /* long */
#define gCardVBL      0x311e   /* VBLTask */
#define gCardPos      0x3134   /* long */
#define gCardFirst    0x3138   /* word */
#define gCardOpen     0x313a   /* word */
#define gCardIOBusy   0x313c   /* word */
#define gCardBusy     0x313e   /* word */
#define gCardMask     0x3140   /* long: StripAddress mask */
#define gSMODs        0x3194   /* 16 longs: locked 'SMOD' code */
#define gSongResId    0x31d4   /* word */
#define gFromMemory   0x31d6   /* word: the song came from memory (selector 0x25) */
#define gStartSong    0x31d8   /* word: start playing after loading */
#define iFlags        0x3278   /* INST temporaries */
#define iFrom         0x327c
#define iTo           0x327e
#define iSMOD         0x3280
#define gDetach       0x32d4   /* word: detach loaded resources instead of copying */
#define lHandle       0x32d6   /* load temporaries */
#define lSize         0x32da
#define lPacked       0x32de
#define lPackedSize   0x32e2
#define lType         0x32e6
#define lPackedType   0x32ea
#define lDelta        0x32ee   /* word: delta-decode after LZSS ('csnd') */

/* error strings (Pascal) */
#define sNoRemoveVBL  0x1a9a
#define sNoLocateVBL  0x1ab4
#define sIncompatible 0x1ace
#define sDBFailed     0x1afe
#define sIncompat2    0x1b1a
#define sOverload     0x1b4a
#define sTempoLarge   0x235a
#define sDivision     0x237c
#define sNoInterp     0x23fc
#define sNoAmpBuf     0x242e
#define sNoVBLMem     0x2462
#define sMaxNotes     0x2496
#define sNoConvBuf    0x24be
#define sCorruptSnd   0x2eee
#define sRateConv1    0x2f08
#define sRateConv2    0x2f2a
#define sNoVBLMem2    0x3144
#define sOverload2    0x316e
#define sNoMidi       0x31da
#define sLoadSong     0x31f6
#define sLoadInsts    0x321e
#define sNoINST       0x3242
#define sNoSMOD       0x3282
#define sNoSnd1       0x3290
#define sNoSnd2       0x32b2

/* voice fields (offsets from the voice's slot gVoices + 4*v) */
#define vPtr      0x000   /* long: sample position */
#define vStep     0x040   /* long: 16.16 step */
#define vRamp     0x080   /* word: 1 = one buffer of silence first, <0 = fade the old sound */
#define vOldPtr   0x0c0
#define vOldStep  0x100
#define vOldFrac  0x140
#define vOldTable 0x180
#define vOldLevel 0x1c0
#define vTimer    0x200   /* word: <0 free, 0 released, else note time left */
#define vFrac     0x240   /* word: fraction of the position */
#define vEnd      0x280   /* long: last sample */
#define vLoopStart 0x2c0
#define vLoopEnd  0x300
#define vBend     0x340   /* long: pitch envelope (words) */
#define vBendStep 0x380
#define vAdvance  0x3c0   /* long: position advance per buffer (cache) */
#define vId       0x400
#define vChannel  0x440   /* word (sample id for sound effects) */
#define vKey      0x480
#define vInst     0x4c0
#define vRelease  0x500   /* word: release countdown */
#define vLoopCnt  0x540   /* long: pointer to a loop count */
#define vInterp   0x580   /* word */
#define vLevel    0x5c0   /* word: 0 = full level (no table) */
#define vTable    0x600   /* long: amplitude table */
#define vDouble   0x640   /* long: sound effect voice (mixed at double level) */
#define vRefCon   0x680
#define vDoneProc 0x6c0

/* ---- 68k register arithmetic ---- */
static inline uint32_t setw(uint32_t r, uint32_t v) { return (r & 0xffff0000u) | (v & 0xffffu); }
static inline uint32_t setb(uint32_t r, uint32_t v) { return (r & 0xffffff00u) | (v & 0xffu); }
static inline uint32_t sxw(uint32_t v) { return (uint32_t)(int32_t)(int16_t)(uint16_t)v; }
static inline uint32_t swap32(uint32_t v) { return (v << 16) | (v >> 16); }
/* divu.w: 1 on overflow (V set, the register is left unchanged) */
static int divu_w(uint32_t *d, uint16_t s) {
    if (s == 0) port_divide_by_zero("MDRV divu");
    uint32_t q = *d / s;
    if (q > 0xffff) return 1;
    *d = ((*d % s) << 16) | q;
    return 0;
}

/* ---- emulated stack depth (see the header) ---- */
static void sp_leave(maddr *sp) { emu_set_sp(*sp); }
#define SUB(saved) maddr sub_sp_ __attribute__((cleanup(sp_leave))) = emu_sp(); \
                   emu_set_sp(sub_sp_ - 4 - (uint32_t)(saved))

/* ---- Toolbox calls with the original's register / stack usage ---- */
static uint32_t os_a0;   /* A0 after the last register-based trap */
static uint32_t ostrap(uint16_t w, uint32_t d0, uint32_t a0) {
    tb_begin();
    cpu_set(R_D0, d0); cpu_set(R_A0, a0);
    tb_trap(w);
    uint32_t r = cpu_get(R_D0);
    os_a0 = cpu_get(R_A0);
    tb_end();
    return r;
}
#define T_RecoverHandle 0xA128
#define T_HUnlock       0xA02A
#define T_HLock         0xA029
#define T_DisposHandle  0xA023
#define T_DisposPtr     0xA01F
#define T_NewPtr        0xA11E
#define T_NewPtrSys     0xA51E
#define T_NewHandle     0xA122
#define T_HPurge        0xA049
#define T_HNoPurge      0xA04A
#define T_MoveHHi       0xA064
#define T_GetHandleSize 0xA025
#define T_VInstall      0xA033
#define T_VRemove       0xA034
#define T_BlockMove     0xA02E
#define T_StripAddress  0xA055
#define T_SwapMMUMode   0xA05D
#define T_HWPriv        0xA198
#define T_GetTrapAddr   0xA146
#define T_GetToolTrap   0xA746
#define T_HandToHand    0xA9E1

/* "RecoverHandle; HUnlock; DisposHandle" on the pointer in A0 */
static void dispose_locked_ptr(uint32_t p) {
    ostrap(T_RecoverHandle, cpu_get(R_D0), p);
    uint32_t h = os_a0;
    ostrap(T_HUnlock, cpu_get(R_D0), h);
    ostrap(T_DisposHandle, cpu_get(R_D0), os_a0);
}
/* clr.l -(A7); move.l type,-(A7); move.w id,-(A7); _GetResource; move.l (A7)+,Dn */
static uint32_t get_resource(uint32_t type, uint16_t id) {
    tb_begin(); push32(0); push32(type); push16(id); tb_trap(0xA9A0); uint32_t r = pop32(); tb_end();
    return r;
}
/* move.l A0,-(A7); move.l A0,-(A7); _DetachResource; movea.l (A7)+,A0 */
static void detach_resource2(uint32_t h) {
    tb_begin(); push32(h); push32(h); tb_trap(0xA992); pop32(); tb_end();
}

/* the error report used everywhere: code -> (8), message -> (a), value -> (e), st (1a46).
 * Returns the message address, which the original leaves in A0. */
static maddr drv_error(uint16_t code, uint32_t msg, uint32_t value) {
    W16(G(gResult), code);
    maddr a0 = G(msg);
    W32(G(gErrMsg), a0);
    W16(G(gErrValue), value);
    W8(G(gErrPending), 0xff);
    return a0;
}

static void regs_save(uint32_t *r) { for (int i = 0; i < 15; i++) r[i] = cpu_get(i); }
static void regs_restore(const uint32_t *r, uint32_t mask) { for (int i = 0; i < 15; i++) if (mask & (1u << i)) cpu_set(i, r[i]); }
#define RM_D(n) (1u << (n))
#define RM_A(n) (1u << (8 + (n)))

/* ---- forward declarations ---- */
static void     setup_voices(maddr a0);
static int      init_mixer(void);
static uint32_t close_output(void);
static void     silence_hw_buffer(void);
static void     flush_caches(void);
static int      open_output(void);
static void     out_zero(maddr a1);
static void     out_half(maddr a0, maddr a1, maddr a2);
static void     out_full(maddr a0, maddr a1, maddr a2);
static void     pause_output(void);
static void     resume_output(void);
static void     stop_all_notes(void);
static void     stop_song_notes(uint32_t d0);
static uint32_t song_is_playing(void);
static uint32_t song_time_raw(void);
static uint32_t song_ticks_raw(void);
static uint32_t song_time_rounded(void);
static uint32_t song_seconds(void);
static uint32_t song_length_raw(void);
static uint32_t song_length_seconds(void);
static int      start_song(void);
static int      setup_song(void);
static void     sequencer_tick(void);
static uint32_t set_time_step(uint32_t d0, maddr *a0);
static void     seq_note_on(maddr a0, uint32_t d1, uint32_t d3);
static void     seq_note_off(maddr a0, uint32_t d1, uint32_t d3);
static uint32_t random_word(void);
static int      alloc_interp_tables(void);
static int      alloc_amp_tables(void);
static void     build_amp_table(maddr a0, uint32_t d0);
static int      alloc_vbl_stub(void);
static int      alloc_vol_table(void);
static int      set_master_volume(uint32_t d7);
static void     mix_buffer(void);
static void     mix_voice(maddr a1);
static void     set_done_proc(uint32_t d0, uint32_t d1);
static void     call_done_proc(maddr a1);
static void     free_all_voices(void);
static void     note_off(uint32_t d0, uint32_t d1, uint32_t d5, uint32_t d6);
static void     note_on(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3, uint32_t d5, uint32_t d6);
static void     play_sample(maddr a0);
static void     stop_all_samples(void);
static void     change_sample_rate(maddr a0);
static void     stop_sample(maddr a0);
static uint32_t sample_done(maddr a0);
static void     card_clear_pb(void);
static uint32_t card_alloc_stub(void);
static uint32_t card_present(void);
static void     card_silence(void);
static uint32_t card_open(void);
static void     card_close(uint32_t z);
static uint32_t card_get_base(void);
static uint32_t card_position(void);
static void     card_start(void);
static uint32_t card_stop(void);
static uint32_t rate_to_step(uint32_t rate, uint32_t msg, maddr *a0);
static void     scan_song_instruments(void);
static int      begin_song(uint32_t d0, uint32_t d1, uint32_t d2, int from_memory);
static int      prepare_instruments(void);
static void     unload_instrument(uint32_t d7);
static int      load_instrument(uint32_t d7);
static uint32_t call_smod(uint32_t d0, uint32_t d1, uint32_t d2, maddr a0, uint32_t d7, maddr a6, maddr a3, uint32_t d5, uint32_t d6);
static void     free_snd(uint32_t d6);
static int      load_snd(uint32_t d6, uint32_t *d0, maddr *a0);
static int      load_snd_copy(uint32_t d6, uint32_t *d0, maddr *a0);
static uint32_t lock_detached(maddr a0);
static uint32_t lock_handle(maddr a0);
static int      free_song(void);
static void     shutdown_driver(void);
static void     dispose_ptr_at(maddr a0);
static void     lzss_decode(maddr src, uint32_t srclen, maddr dst, uint32_t dstlen);
static void     delta_decode(maddr p, uint32_t n);
static uint32_t load_resource(uint32_t d6);
static uint32_t load_snd_resource(uint32_t d6);
static uint32_t load_midi_resource(uint32_t d6);

/* ======================================================================== entry === */

/* clr.b (1a48); bset #1; bclr #4; move.w #5,(13d0); bset #2/#4/#5/#6,(13d0): the flags of
 * instrument (live note) playback */
static void set_live_flags(void) {
    W8(G(gFlags2), 0);
    W8(G(gFlags2), R8(G(gFlags2)) | 0x02);
    W8(G(gFlags2), R8(G(gFlags2)) & ~0x10);
    W16(G(gSongFlags), 5);
    W8(G(gSongFlags), R8(G(gSongFlags)) | 0x04);
    W8(G(gSongFlags), R8(G(gSongFlags)) | 0x10);
    W8(G(gSongFlags), R8(G(gSongFlags)) | 0x20);
    W8(G(gSongFlags), R8(G(gSongFlags)) | 0x40);
}

/* 00352: selector 0x18, output quality. Low byte of the code: 0x16 = 22 kHz mixing,
 * 0x0b = 11 kHz, 0 = measure the CPU and pick one; bit 8 / bit 9 = average / interpolate. */
static void set_quality(uint32_t d4) {
    uint32_t d0 = R32(G(gArg)), d1;
l356:
    d1 = 0;
    if (d0 & 0x100) d1 = 1;
    if (d0 & 0x200) d1 = 0xffffffff;
    W8(G(gInterpMode), d1);
    d0 = setw(d0, d0 & 0xff);
    if ((uint8_t)d0 == 0x16) {                       /* 003f8 (interrupts masked) */
        W16(G(gBufFrames), 0x172); W16(G(gBuf5M1), 0x49); W16(G(gClear5M1), 0x24);
        W16(G(gBufFramesM1), 0x171); W16(G(gFullRate), 1);
        return;
    }
    if ((uint8_t)d0 == 0x0b) {                       /* 00422 */
        W16(G(gBufFrames), 0xb9); W16(G(gBuf5M1), 0x24); W16(G(gClear5M1), 0x11);
        W16(G(gBufFramesM1), 0xb8); W16(G(gFullRate), 0);
        return;
    }
    if ((uint16_t)d0 != 0) { drv_error(0xffff, sInvalidRate, d0); return; }
    /* CPU speed test: count loop passes during 5 ticks. The loop also reads code bytes
     * into D3 (unused) and doubles the caller's D4, whose low byte it stores. */
    d0 = R32(0x16a);
    while (R32(0x16a) == d0) ;
    d0 = R32(0x16a) + 5;
    d1 = 0;
    do {
        d4 = setw(d4, d4 + d4);
        W8(G(gSpeedByte), d4);
        d1++;
    } while (d0 >= R32(0x16a));
    d0 = setw(d0, 0x0b);
    if (d1 < 0xa000) goto l356;
    d0 = setw(d0, 0x10b);
    if (d1 < 0xe400) goto l356;
    d0 = setw(d0, 0x116);
    if (d1 < 0x18000) goto l356;
    d0 = setw(d0, 0x216);
    goto l356;
}

/* 00000: drv(selector, arg) */
uint32_t MDRV_Entry(uint32_t sel, uint32_t arg) {
    uint32_t r[16];
    regs_save(r);
    set_base();
    ENTRY_NOLINK(FN_MDRV_Entry, 52 + 8);             /* movem.l D2-D7/A0-A6; two saved globals */
    uint32_t saved_sel = R32(G(gSelector)), saved_arg = R32(G(gArg));
    W32(G(gSelector), sel);
    W32(G(gArg), arg);
    W16(G(gResult), 0);
    uint32_t d0 = R32(G(gSelector)), d1 = arg;
    int keep_d0 = 0;                                 /* "bra $e0": D0 is the result */
    int c;
    maddr a0;
    if ((uint16_t)d0 >= 0x26) {
        W16(G(gResult), 0xffeb);                     /* -21: unknown selector */
    } else switch ((uint16_t)d0) {
    case 0x00:                                       /* 0031e: begin song (and play) */
        W16(G(gStartSong), 0xffff);
        c = begin_song(R32(G(gArg)), d1, r[R_D2], 0);
        W16(G(gResult), c);
        break;
    case 0x01:                                       /* 00338: load song without starting */
        W16(G(gStartSong), 0);
        c = begin_song(R32(G(gArg)), d1, r[R_D2], 0);
        W16(G(gResult), c);
        break;
    case 0x02: start_song(); break;                  /* 00454 */
    case 0x03:                                       /* 001c0: reload all instruments */
        if (free_song()) break;
        W16(G(gSongFlags), 0);
        prepare_instruments();
        set_live_flags();
        init_mixer();
        break;
    case 0x04: W16(G(gResult), song_is_playing()); break;   /* 00468 */
    case 0x05:                                       /* 00476: end song */
        W16(G(gSongLoop), 0);
        stop_song_notes(0xffffffff);
        break;
    case 0x06: free_all_voices(); break;             /* 00484 */
    case 0x07: free_song(); break;                   /* 0048c */
    case 0x08: shutdown_driver(); break;             /* 00494 */
    case 0x09:                                       /* 00206: note on {inst, key, dur, vel, chan} */
        a0 = d1;
        d0 = setw(d0, R16(a0));
        d1 = setw(d1, R16(a0 + 2));
        note_on(d0, d1, setw(r[R_D2], R16(a0 + 4)), setw(r[R_D3], R16(a0 + 6)),
                setw(r[R_D5], R16(a0 + 8)), setw(r[R_D6], 0));
        break;
    case 0x0a: pause_output(); break;                /* 0049c */
    case 0x0b: resume_output(); break;               /* 004a4 */
    case 0x0c:                                       /* 00224: note off */
        a0 = d1;
        d0 = setw(d0, R16(a0));
        d1 = setw(d1, R16(a0 + 2));
        note_off(d0, d1, setw(r[R_D5], R16(a0 + 8)), setw(r[R_D6], 0));
        break;
    case 0x0d: W16(G(gSongLoop), R32(G(gArg))); break;      /* 0045c */
    case 0x0e:                                       /* 00298: last error (message in D1) */
        d0 = RU16(G(gErrValue));
        W16(G(gResult), d0);
        d1 = R32(G(gErrMsg));
        break;
    case 0x0f: d0 = R32(G(gTickCount)); keep_d0 = 1; break; /* 00260 */
    case 0x10:                                       /* 00252 */
        W32(G(gTickCount), d1);
        W16(G(gErrValue), 0);
        break;
    case 0x11: play_sample(R32(G(gArg))); break;     /* 002aa */
    case 0x12: stop_sample(R32(G(gArg))); break;     /* 002be */
    case 0x13: set_master_volume(r[R_D7]); break;   /* 002f2 */
    case 0x14: W16(G(gResult), sample_done(R32(G(gArg)))); break;      /* 002ca */
    case 0x15: setup_voices(R32(G(gArg))); break;    /* 002e6 */
    case 0x16: stop_all_samples(); break;            /* 002b6 */
    case 0x17: change_sample_rate(R32(G(gArg))); break;     /* 002da */
    case 0x18: set_quality(r[R_D4]); break;          /* 00352 */
    case 0x19:                                       /* 000ee: all instruments, live playback */
        c = free_song();
        if (!c) {
            W16(G(gSongFlags), 0);
            c = prepare_instruments();
            if (!c) { set_live_flags(); init_mixer(); c = 0; }
        }
        W16(G(gResult), c);
        break;
    case 0x1a: W16(G(gResult), 1); break;            /* 0013e */
    case 0x1b:                                       /* 00242: sample done callback {proc, refCon} */
        a0 = d1;
        d1 = R32(a0 + 4);
        set_done_proc(R32(a0), d1);
        break;
    case 0x1c: d0 = song_time_raw(); keep_d0 = 1; break;    /* 00268 */
    case 0x1d: d0 = song_ticks_raw(); keep_d0 = 1; break;   /* 00290 */
    case 0x1e:                                       /* 00280 */
        d0 = song_seconds(); keep_d0 = 1;
        if (R16(G(gSongActive))) d1 = R32(G(gTimeStep));
        break;
    case 0x1f: d0 = song_length_raw(); keep_d0 = 1; break;  /* 00278 */
    case 0x20:                                       /* 00288 */
        d0 = song_time_rounded(); keep_d0 = 1;
        if (R16(G(gSongActive))) d1 = R32(G(gTimeStep));
        break;
    case 0x21:                                       /* 00270 */
        d0 = song_length_seconds(); keep_d0 = 1;
        if (R16(G(gSongActive))) d1 = R32(G(gTimeStep));
        break;
    case 0x22:                                       /* 00198: load instrument */
        c = load_instrument(R32(G(gArg)));
        W16(G(gResult), c);
        break;
    case 0x23:                                       /* 00146: live playback, no instruments yet */
        if (free_song()) break;
        W16(G(gSongFlags), 0);
        for (int i = 0; i < 0x80; i++) W8(G(gInstUsed) + i, 1);
        set_live_flags();
        init_mixer();
        W16(G(gDetach), 0);
        break;
    case 0x24:                                       /* 001ac: unload instrument */
        unload_instrument(R32(G(gArg)));
        /* roxl takes X, which the unload path always leaves clear (lsr.w #3 of D7 << 3,
         * then only flag-neutral or borrow-free instructions; the traps keep X) */
        W16(G(gResult), 0);
        break;
    case 0x25:                                       /* 002fa: song from memory {SONG h, MIDI h, id} */
        W16(G(gStartSong), 0xffff);
        a0 = R32(G(gArg));
        d0 = setw(d0, R16(a0 + 8));
        d1 = R32(a0);
        c = begin_song(d0, d1, R32(a0 + 4), 1);
        W16(G(gResult), c);
        break;
    }
    if (!keep_d0) d0 = RU16(G(gResult));             /* 000da */
    W32(G(gArg), saved_arg);                         /* 000e0 */
    W32(G(gSelector), saved_sel);
    regs_restore(r, 0x7ffcu);                        /* D2-D7/A0-A6 */
    cpu_set(R_D1, d1);
    return d0;
}

/* ===================================================================== setup === */

/* 004ac: selector 0x15 {note voices, mix level, sample voices}: set up and open the output */
static void setup_voices(maddr a0) {
    SUB(0);
    W16(G(gNoteVoices), R16(a0));
    W16(G(gMixLevel), R16(a0 + 2));
    W16(G(gSampleVoices), R16(a0 + 4));
    init_mixer();
    open_output();
    set_done_proc(0, 0);
}

/* 004ce: reset the mixer state and allocate its tables */
static int init_mixer(void) {
    SUB(0);
    W16(G(gInVBL), 0);
    W16(G(gPaused), 0);
    W16(G(gDetach), 1);
    for (int i = 0; i <= 0x10; i++) W16(G(gSeeds) + 2 * i, R16(G(gSeedInit) + 2 * i));
    if (alloc_vol_table()) return 1;
    if (alloc_amp_tables()) return 1;
    if (alloc_interp_tables()) return 1;
    W32(G(gMixCount), 0);
    return 0;
}

/* Sound Manager traps with the original's pushes (Boolean true pushed as $FFFF) */
static uint16_t snd_do_immediate(uint32_t chan, maddr cmd) {
    tb_begin(); push16(0); push32(chan); push32(cmd); tb_trap(0xA804); uint16_t r = pop16(); tb_end();
    return r;
}
static uint16_t snd_dispose_channel(uint32_t chan) {
    tb_begin(); push16(0); push32(chan); push16(0xffff); tb_trap(0xA801); uint16_t r = pop16(); tb_end();
    return r;
}

/* 00516: close the sound output (card, Sound Manager channel or VBL task) */
static uint32_t close_output(void) {
    SUB(0);
    if ((uint16_t)card_stop() == 0) goto l5d0;
    if (R32(G(gSndChan)) != 0) {                     /* 0059c */
        uint32_t chan = R32(G(gSndChan));
        W32(G(gSndChan), 0);
        if (chan == 0) goto l5d0;
        snd_do_immediate(chan, G(gCmdFlush));
        snd_do_immediate(chan, G(gCmdQuiet));
        snd_dispose_channel(chan);
        goto l5d0;
    }
    if (R16(G(gHWOpen)) == 0) return 0;
    W16(G(gHWOpen), 0);
    if (R8(0x27e) != 0x81) return 0;
    {
        uint32_t d0 = ostrap(T_VRemove, cpu_get(R_D0), G(gVBLTask));
        maddr stub = R32(G(gVBLStub));
        /* "movea.l (1a90),A0; beq": movea leaves the flags of _VRemove (TST.W D0) */
        if ((uint16_t)d0 != 0) d0 = ostrap(T_DisposPtr, d0, stub);
        if ((uint16_t)d0 != 0) drv_error(0xfff1, sNoRemoveVBL, d0);
        W16(G(gHWActive), 0);
        silence_hw_buffer();
        d0 = R32(0x16a) + 4;
        while (d0 >= R32(0x16a)) ;
        maddr via = R32(0x1d4);
        W8(via, R8(via) | 0x80);
        W8(0x27e, 0);
        d0 = R32(0x16a) + 0x30;
        while (d0 >= R32(0x16a)) ;
    }
    return 0;
l5d0:
    W16(G(gHWOpen), 0);
    return 0;
}

/* 005da: fill the hardware sound buffer (every other byte) with silence */
static void silence_hw_buffer(void) {
    SUB(0);
    maddr a0 = R32(0x266);
    for (int i = 0; i <= 0x171; i++) { W8(a0, 0x80); a0 += 2; }
}

/* 005f2: flush the caches on a 68020 or later (HWPriv 1 and 3) */
static void flush_caches(void) {
    SUB(0);
    if ((int8_t)R8(0x12f) >= 2) {
        ostrap(T_HWPriv, 1, cpu_get(R_A0));
        ostrap(T_HWPriv, 3, cpu_get(R_A0));
    }
}

/* 00604: open the sound output: a Sound Manager 3 double buffer, else the hardware
 * buffer from a VBL task ($27E = $81 marks the hardware as ours) */
static int open_output(void) {
    SUB(0);
    uint32_t d0 = cpu_get(R_D0);
    if (R16(G(gHWOpen)) != 0) goto l71e;
    {
        ostrap(T_GetToolTrap, 0xa800, cpu_get(R_A0));          /* SoundDispatch */
        maddr a3 = os_a0;
        ostrap(T_GetToolTrap, 0xa89f, os_a0);                  /* Unimplemented */
        if (os_a0 == a3) goto l642;
        ostrap(T_GetTrapAddr, 0xa000, os_a0);
        if (os_a0 == a3) goto l642;
        tb_begin(); push32(0); cpu_set(R_D0, 0xc0008); tb_trap(0xA800); d0 = pop32(); tb_end();   /* SndSoundManagerVersion */
        if (d0 >= 0x03000000u) goto l748;
    }
l642:
    card_stop();
    d0 = card_present();
    if ((uint16_t)d0 == 0) { card_start(); goto l71e; }      /* never: card_present fails */
    if (R8(0x27e) != 0) {
        if (R8(0x27e) == 0x81) goto l670;
        if (R8(0x27e) == 5) goto l748;
l670:
        close_output();
    }
    silence_hw_buffer();
    alloc_vbl_stub();
    W32(G(gSavedZone), R32(0x118));
    {
        maddr t = G(gVBLTask);
        W32(t, 0); W16(t + 4, 1); W32(t + 6, R32(G(gVBLStub))); W16(t + 0xa, 1); W16(t + 0xc, 0);
    }
    W8(G(gHWOpen), 0xff);
    W16(G(gVBLFlag50), 0xffff);
    flush_caches();
    d0 = ostrap(T_VInstall, d0, G(gVBLTask));
    if ((uint16_t)d0 != 0) { drv_error(0xfff0, sIncompatible, d0); return 1; }
    {
        /* find our task in the VBL queue and move it to the head */
        d0 = G(gVBLTask);
        maddr q = 0x160, a1 = R32(q + 2);
        for (;;) {
            if (d0 == R32(a1)) break;
            a1 = R32(a1);
            if (a1 == 0) { drv_error(0xfff1, sNoLocateVBL, d0); return 1; }
        }
        W32(q + 6, a1);
        W32(a1, 0);
        a1 = G(gVBLTask);
        W32(a1, R32(q + 2));
        W32(q + 2, a1);
        W8(0x27e, 0x81);
        W16(G(gHWActive), 1);
    }
l71e:
    W32(G(gLastTicks), 0);
    W16(G(gErrPending), 0);
    return 0;
l748:
    if (R32(G(gSndChan)) != 0) goto l71e;
    {
        uint16_t e;
        tb_begin(); cpu_set(R_D0, d0);
        push16(0); push32(G(gSndChan)); push16(5); push32(0x8c); push32(0);     /* SndNewChannel(&chan, sampledSynth, initMono+initNoDrop..., NULL) */
        tb_trap(0xA807); e = pop16(); d0 = cpu_get(R_D0); tb_end();
        if (e != 0) {                                                            /* 007ec */
            W32(G(gSndChan), 0);
            drv_error(0xfff0, sIncompat2, d0);
            return 1;
        }
        W32(G(gDBBack), CODE(0x80a));                                            /* doubleBack = MDRV_DoubleBack */
        W32(G(gDBHeader) + 0xc, G(gDBBuf0));
        W32(G(gDBHeader) + 0x10, G(gDBBuf1));
        maddr p0 = G(gDBBuf0) + 0x10, p1 = G(gDBBuf1) + 0x10;
        for (int i = 0; i <= 0x171; i++) { W8(p0, 0x80); p0++; W8(p1, 0x80); p1++; }
        W32(G(gDBBuf0) + 4, 1);                                                  /* dbBufferReady */
        W32(G(gDBBuf1) + 4, 1);
        tb_begin(); push16(0); push32(R32(G(gSndChan))); push32(G(gDBHeader)); cpu_set(R_D0, 0x200008);
        tb_trap(0xA800); e = pop16(); tb_end();                                  /* SndPlayDoubleBuffer */
        d0 = (0x200008u & 0xffff0000u) | e;
        if (e == 0) goto l71e;
        drv_error(0xffef, sDBFailed, d0);
        /* the original leaves SndDisposeChannel's result word on the stack here and then
         * returns through it (a crash on a path that needs SndPlayDoubleBuffer to fail) */
        snd_dispose_channel(R32(G(gSndChan)));
        W32(G(gSndChan), 0);
        return 1;
    }
}

/* 0080a: Sound Manager doubleBack procedure: pascal (chan, doubleBuffer). Mixes the next
 * buffer, runs the sequencer, converts the previous mix into the buffer. */
uint32_t MDRV_DoubleBack(uint32_t dbuf, uint32_t chan) {
    uint32_t r[16];
    regs_save(r);
    set_base();
    ENTRY_LINK(FN_MDRV_DoubleBack, 0, 44);           /* link A6,#0; movem.l D1-D7/A2-A5 */
    if (R32(G(gSndChan)) == 0) {
        W32(dbuf + 4, R32(dbuf + 4) | 4);            /* dbLastBuffer */
    } else {
        W32(G(gTickCount), R32(G(gTickCount)) + 1);
        emu_set_sp(emu_sp() - 4);                    /* move.l A6,-(A7) */
        mix_buffer();
        sequencer_tick();
        emu_set_sp(emu_sp() + 4);
        maddr a0 = G(gMixBuf), a1 = R32(a6 + 8) + 0x10, a2 = R32(G(gVolTable));
        if (R8(0x260) == 0) out_zero(a1);            /* SdVolume 0 */
        else if (R16(G(gFullRate)) == 0) out_half(a0, a1, a2);
        else out_full(a0, a1, a2);
        maddr b = R32(a6 + 8);
        W32(b + 4, R32(b + 4) | 1);                  /* dbBufferReady */
    }
    regs_restore(r, 0x7ffeu & ~(RM_A(0) | RM_A(1)));   /* D1-D7/A2-A6 */
    cpu_set(R_A0, R32(a6 + 8));
    return 0;
}

/* 0087c: 370 zero bytes */
static void out_zero(maddr a1) {
    SUB(0);
    for (int i = 0; i < 370; i++) W8(a1 + i, 0);
}

/* 00894: 185 mixed words -> 370 bytes through the volume table, every other byte the
 * average of two neighbours */
static void out_half(maddr a0, maddr a1, maddr a2) {
    SUB(0);
    uint16_t d1, d2;
    for (int i = 0; i <= 0x5b; i++) {
        d1 = RU16(a0); a0 += 2;
        W8(a1++, R8(a2 + sxw(d1)));
        d2 = RU16(a0); a0 += 2;
        d1 = (uint16_t)(d1 + d2) >> 1;
        W8(a1++, R8(a2 + sxw(d1)));
        W8(a1++, R8(a2 + sxw(d2)));
        d2 = (uint16_t)(d2 + RU16(a0)) >> 1;
        W8(a1++, R8(a2 + sxw(d2)));
    }
    d1 = RU16(a0);
    W8(a1++, R8(a2 + sxw(d1)));
    W8(a1, R8(a2 + sxw(d1)));
}

/* 008c4: 370 mixed words -> 370 bytes (movem.w of 7 words, then 6) */
static void out_full(maddr a0, maddr a1, maddr a2) {
    SUB(0);
    uint16_t w[7];
    for (int i = 0; i <= 0x33; i++) {
        for (int k = 0; k < 7; k++) { w[k] = RU16(a0); a0 += 2; }
        for (int k = 0; k < 7; k++) W8(a1++, R8(a2 + sxw(w[k])));
    }
    for (int k = 0; k < 6; k++) { w[k] = RU16(a0); a0 += 2; }
    for (int k = 0; k < 6; k++) W8(a1++, R8(a2 + sxw(w[k])));
}

/* 0090a: selector 0xa */
static void pause_output(void) {
    SUB(0);
    close_output();
    W8(G(gPaused), 0xff);
}

/* 00914: selector 0xb */
static void resume_output(void) {
    SUB(0);
    if (R16(G(gPaused)) != 0) {
        open_output();
        W16(G(gPaused), 0);
    }
}

/* 00924: VBL task of the hardware output (entered through the stub at gVBLStub): writes
 * the previous mix into the hardware sound buffer (every other byte of SoundBase), then
 * mixes the next one. */
uint32_t MDRV_VBLTask(void) {
    set_base();
    ENTRY_NOLINK(FN_MDRV_VBLTask, 60 + 4);           /* movem.l D0-D7/A0-A6; move.l $118,-(A7) */
    uint32_t zone = R32(0x118);
    W16(G(gVBLTask) + 0xa, 1);                       /* vblCount: run again next tick */
    W32(0x118, R32(G(gSavedZone)));
    if (R16(G(gInVBL)) == 0) {
        W16(G(gInVBL), 1);
        W32(G(gTickCount), R32(G(gTickCount)) + 1);
        maddr via = R32(0x1d4);
        W8(via, R8(via) & ~0x80);
        if ((int8_t)R8(G(gSongFlags)) < 0) {
            if (R16(G(gErrPending)) == 0) {
                uint32_t d0 = R32(0x16a) - R32(G(gLastTicks));
                if (d0 != 1 && d0 < 5) {
                    drv_error(0xfffd, sOverload, d0);
                    W8(G(gErrPending), 0xff);
                }
            }
            W32(G(gLastTicks), R32(0x16a));
        }
        maddr a0 = G(gMixBuf), a2 = R32(G(gVolTable));
        if (R8(0x260) == 0) {
            silence_hw_buffer();
        } else if (R16(G(gFullRate)) == 0) {
            /* 185 words, each followed by the average with the next; the buffer is written
             * from offset 0x40 to its end, then from its start */
            maddr a3 = R32(0x266) + 0x40;
            uint32_t off = 0;
            for (int i = 0; i < 185; i++) {
                if (i == 169) { a3 = R32(0x266); off = 0; }   /* 015a6 */
                uint16_t d1 = RU16(a0); a0 += 2;
                W8(a3 + off, R8(a2 + sxw(d1)));
                if (i < 184) d1 = (uint16_t)(d1 + RU16(a0)) >> 1;
                W8(a3 + off + 2, R8(a2 + sxw(d1)));
                off += 4;
            }
        } else {
            maddr a3 = R32(0x266) + 0x50;
            uint16_t w[5];
            for (int i = 0; i <= 0x41; i++) {
                for (int k = 0; k < 5; k++) { w[k] = RU16(a0); a0 += 2; }
                for (int k = 0; k < 5; k++) W8(a3 + 2 * k, R8(a2 + sxw(w[k])));
                a3 += 10;
            }
            a3 = R32(0x266);
            for (int i = 0; i <= 7; i++) {
                for (int k = 0; k < 5; k++) { w[k] = RU16(a0); a0 += 2; }
                for (int k = 0; k < 5; k++) W8(a3 + 2 * k, R8(a2 + sxw(w[k])));
                a3 += 10;
            }
        }
        mix_buffer();                                /* 016c4 */
        sequencer_tick();
        W16(G(gInVBL), 0);
    }
    W32(0x118, zone);
    return 0;
}
void glue_MDRV_VBLTask(void) {                       /* all registers preserved */
    uint32_t r[16];
    regs_save(r);
    MDRV_VBLTask();
    regs_restore(r, 0x7fffu);
}

/* ================================================================ sequencer === */

/* 0173c: stop the notes of every song */
static void stop_all_notes(void) {
    SUB(0);
    stop_song_notes(0xffffffff);
}

/* 01744: stop the tracks of song D0 (all when D0.w < 0) and free the voices */
static void stop_song_notes(uint32_t d0) {
    SUB(0);
    for (int d1 = 0x17; d1 >= 0; d1--) {
        uint32_t d2 = (uint32_t)d1 << 2;
        if ((int16_t)d0 < 0 || R16(G(tSongId) + d2) == (int16_t)d0)
            W16(G(tState) + d2, 0);
    }
    free_all_voices();
}

/* 0176a: D0 = -1 while the song plays (or loops) */
static uint32_t song_is_playing(void) {
    SUB(0);
    if (R16(G(gSongActive)) == 0) return 0;
    if (R16(G(gSongLoop)) != 0) return 0xffffffff;
    for (int i = 0; i <= 0x17; i++)
        if (R8(G(tState) + 4 * i) != 0) return 0xffffffff;
    return 0;
}

/* 0178e .. 01808: song position / length queries (selectors 0x1c..0x21) */
static uint32_t song_time_raw(void) {
    SUB(0);
    return R16(G(gSongActive)) ? R32(G(gSongTime)) : 0;
}
static uint32_t song_ticks_raw(void) {
    SUB(0);
    return R16(G(gSongActive)) ? R32(G(gTimeStep)) : 0;
}
static uint32_t song_time_rounded(void) {                    /* 017aa */
    SUB(0);
    if (R16(G(gSongActive)) == 0) return 0;
    uint32_t d0 = R32(G(gSongTime)), d1 = R32(G(gTimeStep));
    divu_w(&d0, (uint16_t)d1);
    d0 &= 0xffff;
    divu_w(&d0, 0x3c);
    d0 = setw(d0, d0 + 1);
    d0 &= 0xffff;
    d0 = (uint16_t)d0 * 0x3cu;
    return (uint16_t)d0 * (uint32_t)(uint16_t)d1;
}
static uint32_t song_seconds(void) {                         /* 017d6 */
    SUB(0);
    if (R16(G(gSongActive)) == 0) return 0;
    uint32_t d0 = R32(G(gSongTime)), d1 = R32(G(gTimeStep));
    divu_w(&d0, (uint16_t)d1);
    d0 &= 0xffff;
    divu_w(&d0, 0x3c);
    return d0 & 0xffff;
}
static uint32_t song_length_raw(void) {                      /* 017fa */
    SUB(0);
    return R16(G(gSongActive)) ? R32(G(gSongLength)) : 0;
}
static uint32_t song_length_seconds(void) {                  /* 01808 */
    SUB(0);
    if (R16(G(gSongActive)) == 0) return 0;
    uint32_t d0 = R32(G(gSongLength)), d1 = R32(G(gTimeStep));
    divu_w(&d0, (uint16_t)d1);
    d0 &= 0xffff;
    divu_w(&d0, 0x3c);
    return d0 & 0xffff;
}

/* 0182c: (re)start the loaded song */
static int start_song(void) {
    SUB(0);
    if (setup_song()) return 1;
    if (init_mixer()) return 1;
    W32(G(gSongTicks0), R32(0x16a));
    W32(G(gMixCount), 0);
    W32(G(gSong13aa), 0);
    return 0;
}

/* 01850: read the SONG settings and the tracks of the MIDI file */
static int setup_song(void) {
    SUB(0);
    uint32_t d0, d1, d2;
    maddr a0, a1, a2, a3;
    W32(G(gSongTime), 0);
    W16(G(gTSNum), 4);
    W32(G(gTSLen), 0x78);
    W16(G(gMaxUsed), 0);
    if (R16(G(gSongLock)) != 0) return 1;
    for (int i = 0; i < 0x18; i++) W8(G(gChanProgram) + i, i);
    for (int i = 0; i <= 0xf; i++) W8(G(gChanVolume) + i, 0x7f);
    a0 = R32(G(gSongPtr));
    if (a0 == 0) return 1;
    W16(G(gSongId), R16(a0));
    d0 = R8(a0 + 2);
    W16(G(gSingleInst), d0);
    d0 = setw(d0, R16(a0 + 4));
    if ((uint16_t)d0 == 0) d0 = setw(d0, 0x411b);
    W16(G(gTempoDiv), d0);
    d0 = 0x7a120;
    if (R16(G(gCardOpen)) != 0) d0 = 0x78f39;
    W32(G(gTempo), d0);
    divu_w(&d0, RU16(G(gTempoDiv)));
    d0 &= 0xffff;
    W32(G(gTempoQ), d0);
    W32(G(gDivision), 0x3c);
    d0 = set_time_step(d0, &a0);
    W16(G(gTranspose), R16(a0 + 6));
    d0 = setw(d0, 0);
    d0 = setb(d0, R8(a0 + 9));
    d0 = setb(d0, d0 + R8(a0 + 8));
    if ((int16_t)d0 > 0x10) d0 = 0x10;
    d0 = setb(d0, d0 - R8(a0 + 8));
    W16(G(gNoteVoices), d0);
    d0 = setw(d0, 0);
    d0 = setb(d0, R8(a0 + 8));
    W16(G(gSampleVoices), d0);
    d0 = setw(d0, R16(a0 + 0xa));
    if ((int16_t)d0 > 0x10) d0 = 0x10;
    W16(G(gMixLevel), d0);
    W16(G(gSongFlags), R16(a0 + 0xc));
    W8(G(gFlags2), R8(a0 + 0xf));
    a0 = R32(G(gMidiPtr));
    if (a0 == 0) return 1;
    a1 = G(gTrackList);
    W16(G(gTrackCount), 0);
    d1 = 0x1388;
    while (R32(a0) != 0x4d546864) {                  /* 'MThd' at even offsets */
        a0 += 2;
        if (--d1 == 0) return 1;
    }
    d0 = RU16(a0 + 0xc);                             /* division */
    if (!(d0 & 0x8000)) {
        W32(G(gDivision), d0);
        d0 = set_time_step(d0, &a0);
        d1 = 100000;
    }
    for (;;) {                                       /* 0196a: 'MTrk' at any offset */
        if (R8(a0) == 'M' && R8(a0 + 1) == 'T' && R8(a0 + 2) == 'r' && R8(a0 + 3) == 'k') break;
        a0 += 1;
        if (--d1 == 0) return 1;
    }
    do {                                             /* 01992 */
        W16(G(gTrackCount), R16(G(gTrackCount)) + 1);
        W32(a1, a0);
        W32(a1, R32(a1) + 8);
        a1 += 4;
        a0 += 4;
        for (int i = 0; i < 4; i++) { d0 = (d0 << 8) | R8(a0); a0++; }
        a0 += d0;
        W32(a1, d0);
        a1 += 4;
    } while (R8(a0) == 'M' && R8(a0 + 1) == 'T' && R8(a0 + 2) == 'r' && R8(a0 + 3) == 'k');
    a2 = G(gTrackList);
    W16(G(gTrackNum), 0);
    do {                                             /* 019d6: one free track per MTrk */
        a0 = G(tState);
        d1 = 0;
        for (;;) {
            d2 = (uint16_t)(d1 << 2);
            if (R16(a0 + sxw(d2)) == 0) break;
            d1 = setw(d1, d1 + 1);
            if ((uint16_t)d1 == 0x17) return 0;
        }
        d1 <<= 2;
        a3 = d1 + a4_;
        W16(a3 + tSongId, R16(G(gSongId)));
        d0 = R32(a2); a2 += 4;
        W32(a3 + tPtr, d0);
        W32(a3 + tStart, d0);
        W32(a3 + tLength, R32(a2)); a2 += 4;
        W32(a3 + tDelta, 0);
        W32(a3 + tField1d74, 0);
        W32(a3 + tProgram, 0);
        W16(a3 + tProgram, R16(G(gTrackNum)));
        W16(G(gTrackNum), R16(G(gTrackNum)) + 1);
        W32(a3 + tField1e34, 0);
        W16(a3 + tField1cb4, 0xffff);
        W32(a3 + tState, 0);
        W8(a3 + tState, 0x46);
        W16(G(gTrackCount), R16(G(gTrackCount)) - 1);
    } while (R16(G(gTrackCount)) != 0);
    return 0;
}

/* read a MIDI variable-length number at *a1 (D7 is the scratch byte) */
static uint32_t read_vlq(maddr *a1) {
    uint32_t d1 = 0, b;
    do {
        b = R8(*a1);
        d1 = (d1 << 7) | (b & 0x7f);
    } while (R8((*a1)++) & 0x80);
    return d1;
}

/* 01a4a: advance the song by one buffer: every track plays its due events */
static void sequencer_tick(void) {
    SUB(0);
    if (R16(G(gSongActive)) == 0) return;
    W32(G(gSongTime), R32(G(gSongTime)) + R32(G(gTimeStep)));
    W16(G(gTrackBusy), 0);
    maddr a0 = a4_, a1;                              /* track slot: field + a0 */
    uint32_t d0 = 0, d1 = 0, d2 = 0, d3 = 0;
    for (int d6 = 0x17; d6 >= 0; d6--) {
        emu_set_sp(emu_sp() - 2);                    /* move.w D6,-(A7) */
        d0 = R32(G(tPtr));                           /* (1e94,A4): only its low byte is replaced */
        a1 = R32(a0 + tPtr);
        d0 = setb(d0, R8(a0 + tState));
        if ((uint8_t)d0 == 0) goto l1ac8;
        W8(G(gTrackBusy), 0xff);
        if ((uint8_t)d0 == 0x46) { W8(a0 + tState, 0x52); goto l1aa8; }
        {
            uint32_t t = R32(a0 + tDelta), s = R32(G(gTimeStep));
            W32(a0 + tDelta, t - s);
            if (t >= s) goto l1ac8;                  /* still waiting */
        }
    l1a94:
        d0 = R32(a0 + tPtr) - R32(a0 + tStart);
        if (d0 < R32(a0 + tLength)) goto l1aee;
        W16(a0 + tState, 0);                         /* end of the track data */
        goto l1ac8;
    l1aa8:                                           /* delta time of the next event */
        d1 = read_vlq(&a1);
        if (d1 == 0) goto l1aee;
        d1 <<= 6;
        {
            uint32_t t = R32(a0 + tDelta) + d1;
            W32(a0 + tDelta, t);
            if ((int32_t)t >= 0) goto l1ac8;
        }
        goto l1a94;
    l1aee:                                           /* one event */
        d0 = setb(d0, R8(a1)); a1++;
        if (!(d0 & 0x80)) {                          /* 01c02: running status */
            a1 -= 1;
            d1 = setb(d1, R8(a0 + tRunStatus));
            d0 = setb(d0, d1);
            goto l1b26;
        }
        if ((uint8_t)d0 == 0xff) {                   /* meta event */
            d0 = setb(d0, R8(a1)); a1++;
            if ((uint8_t)d0 == 0x51) goto l1c2c;
            if ((uint8_t)d0 == 0x58) goto l1ca0;
            if ((uint8_t)d0 == 0x2f) { W16(a0 + tState, 0); goto l1ac8; }   /* 01c24 */
            goto l1c0e;
        }
        d1 = setb(d1, d0);
        if ((uint8_t)d1 == 0) goto l1aee;
        W8(a0 + tRunStatus, d1);
    l1b26:
        d1 = setw(d1, d1 & 0xf0);
        d0 = setw(d0, d0 & 0x0f);
        W16(a0 + tChannel, d0);
        switch ((uint8_t)d1) {
        case 0x80:                                   /* 01bf4: note off */
            d1 = setb(d1, R8(a1)); a1++;
            d3 = setw(d3, 0); d3 = setb(d3, R8(a1)); a1++;
            seq_note_off(a0, d1, d3);
            goto l1aa8;
        case 0x90:                                   /* 01bc8: note on */
            d1 = setb(d1, R8(a1)); a1++;
            d3 = setw(d3, 0); d3 = setb(d3, R8(a1)); a1++;
            if ((uint8_t)d3 != 0) {
                d2 = R8(G(gChanVolume) + sxw(d0));
                d2 = (uint16_t)d2 * (uint32_t)(uint16_t)d3;
                divu_w(&d2, 0x7f);
                if ((uint16_t)d2 != 0) {
                    d3 = setb(d3, d2);
                    seq_note_on(a0, d1, d3);
                    goto l1aa8;
                }
            }
            seq_note_off(a0, d1, d3);                /* 01bfa */
            goto l1aa8;
        case 0xa0:                                   /* 01b8e: aftertouch */
            d1 = setb(d1, R8(a1)); a1++;
            d2 = setb(d2, R8(a1)); a1++;
            goto l1aa8;
        case 0xb0:                                   /* 01b6e: controller (7 = volume) */
            d1 = setb(d1, R8(a1)); a1++;
            d2 = setb(d2, R8(a1)); a1++;
            /* the original saves A2 with move.w / restores it with movea.w (A2 is
             * sign-extended from its low word; not used afterwards) */
            if ((uint8_t)d1 == 7) W8(G(gChanVolume) + sxw(d0), d2);
            goto l1aa8;
        case 0xc0:                                   /* 01b9c: program change */
            d1 = setw(d1, 0); d1 = setb(d1, R8(a1)); a1++;
            if (R8(G(gSongFlags)) & 0x04) {
                W16(a0 + tProgram, d1);
                d0 = setw(d0, R16(a0 + tChannel));
                W8(G(gChanProgram) + sxw(d0), d1);
            }
            goto l1aa8;
        case 0xd0:                                   /* 01b96: channel pressure */
            d1 = setb(d1, R8(a1)); a1++;
            goto l1aa8;
        case 0xe0:                                   /* 01bc0: pitch bend */
            d1 = setb(d1, R8(a1)); a1++;
            d2 = setb(d2, R8(a1)); a1++;
            goto l1aa8;
        default:                                     /* 01d98: sysex: skip */
            d1 = read_vlq(&a1);
            a1 += d1;
            goto l1aa8;
        }
    l1c2c:                                           /* tempo */
        d0 = 0;
        d0 = R8(a1 + 1);
        d0 = (d0 << 8) | R8(a1 + 2);
        d0 = (d0 << 8) | R8(a1 + 3);
        W32(G(gTempo), d0);
        if (divu_w(&d0, RU16(G(gTempoDiv)))) a0 = drv_error(0xfff3, sTempoLarge, d0);
        d0 &= 0xffff;
        W32(G(gTempoQ), d0);
        d0 = set_time_step(R32(G(gDivision)), &a0);
        goto l1c0e;
    l1ca0:                                           /* time signature */
        d0 = R8(a1 + 1);
        d1 = R8(a1 + 2);
        if ((uint8_t)d1 == 2) {
            W16(G(gTSNum), d0);
            W32(G(gTSLen), R32(G(gDivision)));
        } else if ((uint8_t)d1 == 3) {
            d0 = (uint16_t)d0 * 3u;
            divu_w(&d0, 2);
            W16(G(gTSNum), d0);
            d0 = R32(G(gDivision));
            d0 = (uint16_t)d0 * 3u;
            divu_w(&d0, 2);
            W32(G(gTSLen), d0);
        }
    l1c0e:                                           /* skip the meta data */
        d1 = read_vlq(&a1);
        a1 += d1;
        goto l1aa8;
    l1ac8:
        W32(a0 + tPtr, a1);
        a0 += 4;
        emu_set_sp(emu_sp() + 2);
    }
    if (R16(G(gTrackBusy)) == 0 && R16(G(gScanMode)) != 1 && R16(G(gSongLoop)) != 0)
        setup_song();                                /* loop the song */
}

/* 01c74: time step = (D0 << 6) / (tempo quotient). A division overflow reports an error
 * (and leaves the message address in the caller's A0). */
static uint32_t set_time_step(uint32_t d0, maddr *a0) {
    SUB(0);
    d0 <<= 6;
    if (divu_w(&d0, RU16(G(gTempoQ) + 2))) *a0 = drv_error(0xfff2, sDivision, d0);
    d0 &= 0xffff;
    W32(G(gTimeStep), d0);
    return d0;
}

/* 01ce4: note on from a track: instrument by program (or remapped by the SONG's table),
 * transposed key; D6 is the low word of the track slot address */
static void seq_note_on(maddr a0, uint32_t d1, uint32_t d3) {
    SUB(24);                                         /* movem.l D0-D1/D6/A0-A2 */
    uint32_t d5 = R16(a0 + tChannel), d6 = (uint16_t)a0, d0, d7;
    if (R8(G(gSongFlags)) & 0x08) {
        d0 = R16(a0 + tProgram);
    } else {
        d0 = R16(a0 + tChannel);
        d0 = R8(G(gChanProgram) + sxw(d0)) & 0xff;
    }
    maddr a2 = R32(G(gSongPtr)) + 0x10;
    d7 = RU16(a2); a2 += 2;
    while (d7 != 0) {
        if ((uint16_t)d0 == RU16(a2)) { a2 += 2; d0 = setw(d0, R16(a2)); break; }
        a2 += 4;
        d7 = (uint16_t)(d7 - 1);
    }
    d1 = setw(d1, d1 + R16(G(gTranspose)));
    if ((int8_t)d1 < 0) d1 = setw(d1, d1 - R16(G(gTranspose)));
    note_on(d0, d1, 0x2710, d3, d5, d6);
}

/* 01d40: note off from a track */
static void seq_note_off(maddr a0, uint32_t d1, uint32_t d3) {
    SUB(24);
    uint32_t d5 = R16(a0 + tChannel), d6 = (uint16_t)a0, d0, d7;
    if (R8(G(gSongFlags)) & 0x08) {
        d0 = R16(a0 + tProgram);
    } else {
        d0 = R16(a0 + tChannel);
        d0 = R8(G(gChanProgram) + sxw(d0)) & 0xff;
    }
    maddr a2 = R32(G(gSongPtr)) + 0x10;
    d7 = RU16(a2); a2 += 2;
    while (d7 != 0) {
        if ((uint16_t)d0 == RU16(a2)) { a2 += 2; d0 = setw(d0, R16(a2)); break; }
        a2 += 4;
        d7 = (uint16_t)(d7 - 1);
    }
    d1 = setw(d1, d1 + R16(G(gTranspose)));
    if ((int8_t)d1 < 0) d1 = setw(d1, d1 - R16(G(gTranspose)));
    note_off(d0, d1, d5, d6);
}

/* 01dae: additive random generator over 17 words; D7.w = the next value */
static uint32_t random_word(void) {
    SUB(4);                                          /* move.l A0,-(A7) */
    maddr a0 = G(gSeeds) + sxw(R16(G(gSeedI)));
    uint16_t d7 = RU16(a0);
    a0 = G(gSeeds) + sxw(R16(G(gSeedJ)));
    W16(a0, R16(a0) + d7);
    W16(G(gSeedI), R16(G(gSeedI)) + 2);
    if (RU16(G(gSeedI)) >= 0x22) W16(G(gSeedI), 0);
    W16(G(gSeedJ), R16(G(gSeedJ)) + 2);
    if (RU16(G(gSeedJ)) >= 0x22) W16(G(gSeedJ), 0);
    return d7;
}

/* ================================================================== tables === */

/* 01dec: 2 x 64K interpolation tables (when the song asks for interpolation) */
static int alloc_interp_tables(void) {
    SUB(0);
    if (!(R8(G(gSongFlags)) & 0x20) && !(R8(G(gSongFlags)) & 0x10)) return 0;
    if ((int8_t)R8(G(gInterpMode)) >= 0) return 0;
    if (R32(G(gInterpBlock)) != 0) return 0;
    uint32_t d0 = ostrap(T_NewPtr, 0x20000, cpu_get(R_A0));
    maddr a0 = os_a0;
    if ((uint16_t)d0 != 0) { drv_error(0xfff7, sNoInterp, d0); return 1; }
    W32(G(gInterpBlock), a0);
    W32(G(gInterpTab1), a0);
    W32(G(gInterpTab2), a0 + 0x10000);
    for (uint32_t d7 = 0x100; d7 != 0; d7--)
        for (uint32_t d6 = 0; d6 < 0x100; d6++)
            W8(a0++, (uint16_t)((uint16_t)(d6 * d7) + 0x7f) >> 8);
    a0 = R32(G(gInterpTab2));
    for (uint32_t d7 = 0; d7 != 0x100; d7++)
        for (uint32_t d6 = 0; d6 < 0x100; d6++)
            W8(a0++, (uint16_t)((uint16_t)(d6 * d7) + 0x80) >> 8);
    return 0;
}

/* 01e8c: 128 amplitude tables of 256 bytes (32K) */
static int alloc_amp_tables(void) {
    SUB(0);
    if (!(R8(G(gFlags2)) & 0x02)) return 0;
    maddr a3 = G(gVolLevels);
    if (R32(a3) != 0) return 0;
    uint32_t d0 = ostrap(T_NewPtr, 0x8000, cpu_get(R_A0));
    maddr a0 = os_a0;
    if ((uint16_t)d0 != 0) { drv_error(0xfff6, sNoAmpBuf, d0); return 1; }
    W32(a3, a0);
    for (d0 = 0; d0 != 0x80; d0++) {
        emu_set_sp(emu_sp() - 2);                    /* move.w D0,-(A7) */
        build_amp_table(a0, d0);
        a0 += 0x100;
        emu_set_sp(emu_sp() + 2);
    }
    return 0;
}

/* 01edc: one amplitude table: 0x80 - (level + 1) rising by (level + 1) * 2 / 256 per step */
static void build_amp_table(maddr a0, uint32_t d0) {
    SUB(0);
    d0 = setw(d0, d0 + 1);
    uint32_t d2 = (uint8_t)(0x80 - (uint8_t)d0);
    uint32_t d3 = (uint16_t)d0;
    d0 = setw(d0, d0 << 1);
    for (int d1 = 0; d1 <= 0x3f; d1++) {
        for (int k = 0; k < 4; k++) {
            W8(a0++, d2);
            uint32_t s = (d3 & 0xff) + (d0 & 0xff);
            d3 = setb(d3, s);
            d2 = setb(d2, (d2 & 0xff) + (s >> 8));
        }
    }
}

/* 01f0a: the 36-byte system-heap VBL stub: bsr.s *+4; dc.l MDRV_VBLTask;
 * movea.l (A7)+,A1; movea.l (A1),A1; jmp (A1) */
static int alloc_vbl_stub(void) {
    SUB(0);
    if (R32(G(gVBLStub)) == 0) {
        uint32_t d0 = ostrap(T_NewPtrSys, 0x24, cpu_get(R_A0));
        maddr a0 = os_a0;
        if ((uint16_t)d0 != 0) { drv_error(0xfffe, sNoVBLMem, d0); return -1; }
        W32(G(gVBLStub), a0);
        W16(a0, 0x6104);
        W32(a0 + 2, CODE(0x924));
        W16(a0 + 6, 0x225f);
        W16(a0 + 8, 0x2251);
        W16(a0 + 10, 0x4ed1);
    }
    return 0;
}

/* 01f54: the output table: mix sum -> output byte (n*128 zeros, a ramp of 256 values
 * each gMixLevel wide, n*128 x 0xff) */
static int alloc_vol_table(void) {
    SUB(0);
    maddr a0 = R32(G(gVolTable));
    uint32_t d0, d1 = 0;
    if (a0 != 0) {
        if (R16(G(gVolTabSmp)) == R16(G(gSampleVoices)) && R16(G(gVolTabLevel)) == R16(G(gMixLevel)) &&
            (uint16_t)(R16(G(gVolTabSize)) - R16(G(gSampleVoices)) - R16(G(gSampleVoices))) == RU16(G(gNoteVoices)))
            return 0;
        ostrap(T_DisposPtr, cpu_get(R_D0), a0);
    }
    for (int i = 0; i <= 0xf; i++) W32(G(gVoices + vTimer) + 4 * i, 0xffffffff);   /* all voices free */
    W16(G(gVolTabLevel), R16(G(gMixLevel)));
    W16(G(gVolTabSmp), R16(G(gSampleVoices)));
    d0 = (uint16_t)(R16(G(gNoteVoices)) + R16(G(gSampleVoices)) + R16(G(gSampleVoices)));
    if ((int16_t)d0 <= 0) { drv_error(0xfff5, sMaxNotes, d0); return 1; }
    W16(G(gVolTabSize), d0);
    d0 = (d0 << 8) + 4;
    d0 = ostrap(T_NewPtr, d0, cpu_get(R_A0));
    a0 = os_a0;
    if ((uint16_t)d0 != 0) { drv_error(0xfff4, sNoConvBuf, d0); return 1; }
    W32(G(gVolTable), a0);
    d0 = setw(d0, R16(G(gNoteVoices)) + R16(G(gSampleVoices)) + R16(G(gSampleVoices)) - R16(G(gMixLevel)));
    if ((uint16_t)d0 != 0) {
        d0 = (uint16_t)d0 * 0x80u;
        do { W8(a0++, 0); d0 = setw(d0, d0 - 1); } while ((uint16_t)d0 != 0);
    }
    d0 = setw(d0, 0);
    do {
        d1 = setw(d1, R16(G(gMixLevel)));
        do { W8(a0++, d0); d1 = setw(d1, d1 - 1); } while ((uint16_t)d1 != 0);
        d0 = setb(d0, d0 + 1);
    } while ((uint8_t)d0 != 0);
    d0 = setw(d0, R16(G(gNoteVoices)) + R16(G(gSampleVoices)) + R16(G(gSampleVoices)) - R16(G(gMixLevel)));
    if ((uint16_t)d0 != 0) {
        d0 = (uint16_t)d0 * 0x80u;
        do { W8(a0++, 0xff); d0 = setw(d0, d0 - 1); } while ((uint16_t)d0 != 0);
    }
    return 0;
}

static inline uint32_t ror8w(uint32_t r) { return setw(r, ((r & 0xff) << 8) | ((r >> 8) & 0xff)); }

/* 02058: selector 0x13: master volume (0..0x100): rebuild the output table around 0x80.
 * When there is nothing below the ramp the start level is the caller's D7 (never set). */
static int set_master_volume(uint32_t d7) {
    SUB(0);
    maddr a0 = R32(G(gVolTable));
    uint32_t d6 = R32(G(gArg)), d0 = 0, d1 = 0, d4;
    d0 = setw(d0, R16(G(gNoteVoices)) + R16(G(gSampleVoices)) + R16(G(gSampleVoices)) - R16(G(gMixLevel)));
    if ((uint16_t)d0 != 0) {
        d7 = 0x80u * (uint16_t)d6;
        d7 = setw(d7, (uint16_t)d7 >> 8);
        d7 = setw(d7, 0u - d7);
        d7 = setw(d7, d7 + 0x80);
        d0 = (uint16_t)d0 * 0x80u;
        do { W8(a0++, d7); d0 = setw(d0, d0 - 1); } while ((uint16_t)d0 != 0);
    }
    d0 = setw(d0, 0);
    d6 = R32(G(gArg));
    d4 = (uint16_t)(R16(G(gMixLevel)) - 1);
    do {
        d1 = setw(d1, d4);
        do { W8(a0++, d7); d1 = setw(d1, d1 - 1); } while ((uint16_t)d1 != 0xffff);
        d7 = ror8w(d7);
        d7 = setw(d7, d7 + d6);
        d7 = ror8w(d7);
        d0 = setb(d0, d0 + 1);
    } while ((uint8_t)d0 != 0);
    if ((uint8_t)d7 == 0) d7 = setb(d7, 0xff);
    d0 = setw(d0, R16(G(gNoteVoices)) + R16(G(gSampleVoices)) + R16(G(gSampleVoices)) - R16(G(gMixLevel)));
    if ((uint16_t)d0 != 0) {
        d0 = (uint16_t)d0 * 0x80u;
        d0 = setw(d0, d0 - 1);
        do { W8(a0++, d7); d0 = setw(d0, d0 - 1); } while ((uint16_t)d0 != 0xffff);
    }
    return 0;
}

/* ==================================================================== mixer === */

/* 020da: mix one buffer of all voices into gMixBuf (words). Free voices add the silence
 * level 0x80 (twice for sound effect voices); with no voice at all the buffer is just
 * filled with that level. */
static void mix_buffer(void) {
    SUB(0);
    uint32_t d0, d1 = 0, d7 = 0;
    maddr a0 = G(gVoices + vTimer), a1;
    d0 = (uint16_t)(R16(G(gNoteVoices)) + R16(G(gSampleVoices)) - 1);
    do {
        d7 = setw(d7, d7 + 1);
        if (R16(a0) < 0) {
            d7 = setw(d7, d7 - 1);
            d1 += 0x800080;
            if ((uint16_t)d0 < RU16(G(gSampleVoices))) d1 += 0x800080;
        }
        a0 += 4;
        d0 = setw(d0, d0 - 1);
    } while ((uint16_t)d0 != 0xffff);
    if ((uint16_t)d7 == 0) {                         /* 02122 */
        a1 = G(gMixBuf);
        d1 = setw(d1, R16(G(gNoteVoices)) + R16(G(gSampleVoices)) + R16(G(gSampleVoices)));
        d1 = swap32(d1);
        d1 = setw(d1, R16(G(gNoteVoices)) + R16(G(gSampleVoices)) + R16(G(gSampleVoices)));
        d1 <<= 7;
        d0 = setw(d0, R16(G(gClear5M1)));
        do {
            for (int k = 0; k < 5; k++) { W32(a1, d1); a1 += 4; }
            d0 = setw(d0, d0 - 1);
        } while ((uint16_t)d0 != 0xffff);
        W32(a1, d1); a1 += 4;
        W32(a1, d1); a1 += 4;
        W16(a1, d1);
    } else {
        if ((uint16_t)d7 >= RU16(G(gMaxUsed))) W16(G(gMaxUsed), d7);
        a0 = G(gMixBuf);                             /* 02160 */
        d0 = setw(d0, R16(G(gClear5M1)));
        do {
            for (int k = 0; k < 5; k++) { W32(a0, d1); a0 += 4; }
            d0 = setw(d0, d0 - 1);
        } while ((uint16_t)d0 != 0xffff);
        W32(a0, d1); a0 += 4;
        W32(a0, d1); a0 += 4;
        W16(a0, d1);
        W16(G(gVoiceLoop), R16(G(gNoteVoices)) + R16(G(gSampleVoices)));
        a1 = G(gVoices);
        do {
            if (R16(a1 + vTimer) >= 0) mix_voice(a1);
            a1 += 4;
            W16(G(gVoiceLoop), R16(G(gVoiceLoop)) - 1);
        } while (R16(G(gVoiceLoop)) != 0);
    }
    W32(G(gMixCount), R32(G(gMixCount)) + 1);        /* 0215a */
}

/* register-level helpers of the voice mixers */
#define STEP()   do { uint32_t s_ = (d3 & 0xffffu) + (d2 & 0xffffu); d3 = setw(d3, s_); \
                      d5 = setw(d5, d5 + d1 + (s_ >> 16)); } while (0)      /* add.w D2,D3; addx.w D1,D5 */
#define SMP(o)   R8(a2 + sxw(d5) + (o))                                       /* (o,A2,D5.w) */
#define ADDW(v)  do { W16(a0, RU16(a0) + (uint16_t)(v)); a0 += 2; } while (0) /* add.w v,(A0)+ */
#define ADDW0(v) W16(a0, RU16(a0) + (uint16_t)(v))                            /* add.w v,(A0) */
#define ADDL(v)  do { W32(a0, R32(a0) + (uint32_t)(v)); a0 += 4; } while (0)  /* add.l v,(A0)+ */
#define DBRA(d)  ((d = setw(d, d - 1)), (uint16_t)d != 0xffff)
#define AVG(d4)  do { uint32_t s_ = ((d4) & 0xff) + SMP(1); d4 = setb(d4, s_ >> 1); } while (0)  /* add.b (1,A2,D5.w),D4; roxr.b #1 */

/* common start of the four voice mixers (02262 / 0260c / 02a50 / 02e4c) */
static void mix_start(maddr a1, maddr *a2, uint32_t *d1, uint32_t *d2, uint32_t *d3, uint32_t *d5) {
    *a2 = R32(a1 + vPtr);
    *d1 = RU16(a1 + vStep);
    *d2 = RU16(a1 + vStep + 2);
    if (R16(G(gFullRate)) != 0) {                    /* lsr.w #1,D1; roxr.w #1,D2 */
        uint32_t x = *d1 & 1;
        *d1 = (*d1 & 0xffff) >> 1;
        *d2 = ((*d2 & 0xffff) >> 1) | (x << 15);
    }
    *d3 = RU16(a1 + vFrac);
    *d5 = R32(a1 + vAdvance);
    if (*d5 == 0) {
        uint32_t d0 = (uint32_t)RU16(a1 + vStep) * RU16(G(gBufFrames));
        *d5 = ((uint32_t)RU16(a1 + vStep + 2) * RU16(G(gBufFrames))) >> 16;
        *d5 += d0;
        W32(a1 + vAdvance, *d5);
    }
}

/* 02262: 8-bit voice, full level */
static void mix_plain(maddr a1) {
    maddr a0 = G(gMixBuf), a2, a3, a5, a6;
    uint32_t d0 = 0, d1, d2, d3, d4 = 0, d5, d7;
    mix_start(a1, &a2, &d1, &d2, &d3, &d5);
    if (R16(a1 + vRamp) != 0) goto l239a;
    if (R16(a1 + vTimer) != 0 && R32(a1 + vLoopEnd) != 0) {
        d0 = R32(a1 + vPtr) + d5;
        if (d0 < R32(a1 + vLoopEnd)) goto l22e0;
        goto l2472;
    }
l22d0:
    d0 = R32(a1 + vEnd) - R32(a1 + vPtr);
    if ((int32_t)(d0 - d5) <= 0) goto l243c;
l22e0:
    d0 = setw(d0, R16(G(gBuf5M1)));
    if ((uint16_t)d2 == 0) goto l2414;
    d5 = setw(d5, 0);
    if (R16(a1 + vInterp) != 0) goto l2330;
    do {                                             /* 022f4 */
        d4 = setb(d4, SMP(0)); STEP(); d4 = swap32(d4); d4 = setb(d4, SMP(0)); STEP(); ADDL(d4);
        d4 = setb(d4, SMP(0)); STEP(); d4 = swap32(d4); d4 = setb(d4, SMP(0)); STEP(); ADDL(d4);
        d4 = setb(d4, SMP(0)); STEP(); ADDW(d4);
    } while (DBRA(d0));
    a2 += sxw(d5);
    goto l2602;
l2330:
    if ((int16_t)R16(a1 + vInterp) > 0) goto l2364;
    a3 = R32(G(gInterpTab1)); a6 = R32(G(gInterpTab2));
    d0 = setw(d0, R16(G(gBufFramesM1)));
    d7 = 0;
    do {                                             /* 02342 */
        d7 = setw(d7, d3); d7 = setb(d7, SMP(0));
        d4 = setb(d4, R8(a3 + d7));
        { uint32_t s_ = (d3 & 0xffff) + (d2 & 0xffff); d3 = setw(d3, s_);
          d7 = setb(d7, SMP(1));
          d5 = setw(d5, d5 + d1 + (s_ >> 16)); }
        d4 = setb(d4, d4 + R8(a6 + d7));
        ADDW(d4);
    } while (DBRA(d0));
    a2 += sxw(d5);
    goto l2602;
l2364:
    d0 = setw(d0, R16(G(gBufFramesM1)));
    do {                                             /* 02368 */
        if ((int16_t)d3 < 0) { d4 = setb(d4, SMP(0)); AVG(d4); }
        else d4 = setb(d4, SMP(0));
        ADDW(d4);
        STEP();
    } while (DBRA(d0));
    a2 += sxw(d5);
    goto l2602;
l239a:
    if ((int16_t)R16(a1 + vRamp) >= 0) {             /* first buffer of a new note: silence */
        W16(a1 + vRamp, 0);
        d1 = 0x800080;
        d0 = setw(d0, R16(G(gBuf5M1)));
        do { ADDL(d1); ADDL(d1); ADDW(d1); } while (DBRA(d0));
        return;
    }
    W16(a1 + vRamp, 0);                              /* 023bc: finish the old sound */
    a2 = R32(a1 + vOldPtr);
    d1 = setw(d1, R16(a1 + vOldStep));
    d2 = setw(d2, R16(a1 + vOldStep + 2));
    if (R16(G(gFullRate)) != 0) {
        uint32_t x = d1 & 1;
        d1 = setw(d1, (d1 & 0xffff) >> 1);
        d2 = setw(d2, ((d2 & 0xffff) >> 1) | (x << 15));
    }
    d3 = setw(d3, R16(a1 + vOldFrac));
    d0 = setw(d0, R16(G(gBufFramesM1)));
    d7 = 0;
    d7 = setb(d7, R8(a2));
    do {                                             /* 023e0: until it crosses the middle */
        d4 = setb(d4, R8(a2));
        d5 = setw(d5, 0);
        d7 = setb(d7, d7 ^ d4);
        if (d7 & 0x80) goto l23fa;
        d7 = setb(d7, d4);
        ADDW(d4);
        STEP();
        a2 += sxw(d5);
    } while (DBRA(d0));
    return;
l23fa:
    d7 = setb(d7, d7 ^ d4);
    { uint32_t s_ = (d4 & 0xff) + (d7 & 0xff); d4 = setb(d4, s_ >> 1); }
    d1 = setb(d1, 0x80);
    do {                                             /* 02404: then decay to the middle */
        ADDW(d4);
        d4 = setb(d4, d4 - 0x80);
        d4 = setb(d4, (uint8_t)((int8_t)d4 >> 1));
        d4 = setb(d4, d4 + 0x80);
    } while (DBRA(d0));
    return;
l2414:                                               /* integer step */
    d4 = 0;
    do {
        d4 = setb(d4, R8(a2)); d4 = swap32(d4); a2 += sxw(d1); d4 = setb(d4, R8(a2)); ADDL(d4); a2 += sxw(d1);
        d4 = setb(d4, R8(a2)); d4 = swap32(d4); a2 += sxw(d1); d4 = setb(d4, R8(a2)); ADDL(d4); a2 += sxw(d1);
        d4 = setb(d4, R8(a2)); a2 += sxw(d1); ADDW(d4);
    } while (DBRA(d0));
    goto l2602;
l243c:                                               /* the sample ends in this buffer */
    d0 = setw(d0, R16(G(gBufFramesM1)));
    a6 = R32(a1 + vEnd);
    do {
        if (a2 >= a6) {
            do ADDW(0x80); while (DBRA(d0));
            break;
        }
        d4 = setb(d4, R8(a2));
        d5 = setw(d5, 0);
        ADDW(d4);
        STEP();
        a2 += sxw(d5);
    } while (DBRA(d0));
    W16(a1 + vTimer, 0xffff);                        /* 0245a */
    call_done_proc(a1);
    return;
l2472:                                               /* the loop end is in this buffer */
    d0 = R32(a1 + vLoopCnt);
    if (d0 != 0) {
        a6 = d0;
        W16(a1 + vTimer, 0x7fff);
        if (R16(a6) >= 0) {
            if (R16(a6) == 0) goto l22d0;
            W16(a6, R16(a6) - 1);
            if (R16(a6) == 0) goto l22d0;
        }
    }
    if ((uint16_t)d2 == 0) goto l2524;
    if (R16(a1 + vInterp) != 0) goto l2546;
    d0 = setw(d0, R16(G(gBuf5M1)));
    a6 = R32(a1 + vLoopEnd);
    do {                                             /* 024a4 */
        for (int k = 0; k < 5; k++) {
            if (a2 >= a6) a2 = a2 - a6 + R32(a1 + vLoopStart);
            d4 = setb(d4, R8(a2));
            d5 = setw(d5, 0);
            ADDW(d4);
            STEP();
            a2 += sxw(d5);
        }
    } while (DBRA(d0));
    goto l2602;
l2524:
    d0 = setw(d0, R16(G(gBufFramesM1)));
    a6 = R32(a1 + vLoopEnd);
    do {
        if (a2 >= a6) a2 = a2 - a6 + R32(a1 + vLoopStart);
        d4 = setb(d4, R8(a2));
        ADDW(d4);
        a2 += sxw(d1);
    } while (DBRA(d0));
    goto l2602;
l2546:
    if ((int16_t)R16(a1 + vInterp) > 0) goto l25b8;
    d7 = 0;
    emu_set_sp(emu_sp() - 4);                        /* move.l A5,-(A7) */
    d0 = setw(d0, R16(G(gBufFramesM1)));
    a5 = R32(a1 + vLoopEnd);
    d1 = setw(d1, d1 - 2);
    a5 -= 1;
    a3 = R32(G(gInterpTab1)); a6 = R32(G(gInterpTab2));
    do {                                             /* 02562 */
        if (a2 >= a5) goto l258a;
    l2568:
        d7 = setw(d7, d3); d7 = setb(d7, R8(a2)); a2++;
        d5 = setw(d5, 0);
        d4 = setb(d4, R8(a3 + d7));
        { uint32_t s_ = (d3 & 0xffff) + (d2 & 0xffff); d3 = setw(d3, s_);
          d7 = setb(d7, R8(a2)); a2++;
          d5 = setw(d5, d5 + d1 + (s_ >> 16)); }
        d4 = setb(d4, d4 + R8(a6 + d7));
        a2 += sxw(d5);
        ADDW(d4);
    } while (DBRA(d0));
    emu_set_sp(emu_sp() + 4);
    goto l2602;
l258a:
    a5 += 1;
    d4 = setw(d4, 0);
    d1 = setw(d1, d1 + 2);
    do {                                             /* 02590 */
        if (a2 >= a5) {                              /* 025ac */
            d1 = setw(d1, d1 - 2);
            a2 = a2 - a5 + R32(a1 + vLoopStart);
            a5 -= 1;
            goto l2568;
        }
        d4 = setb(d4, R8(a2));
        d5 = setw(d5, 0);
        ADDW(d4);
        STEP();
        a2 += sxw(d5);
    } while (DBRA(d0));
    emu_set_sp(emu_sp() + 4);
    goto l2602;
l25b8:
    d0 = setw(d0, R16(G(gBufFramesM1)));
    a6 = R32(a1 + vLoopEnd);
    do {                                             /* 025c0 */
        if (a2 >= a6) a2 = a2 - a6 + R32(a1 + vLoopStart);
        d4 = setb(d4, R8(a2));
        d5 = setw(d5, 0);
        if ((int16_t)d3 < 0) { uint32_t s_ = (d4 & 0xff) + R8(a2 + 1); d4 = setb(d4, s_ >> 1); }
        ADDW(d4);
        STEP();
        a2 += sxw(d5);
    } while (DBRA(d0));
    a2 += sxw(d5);                                   /* added once more on the way out */
l2602:
    W32(a1 + vPtr, a2);
    W16(a1 + vFrac, d3);
}

/* 0260c: 8-bit voice through its amplitude table (vTable) */
static void mix_vol(maddr a1) {
    maddr a0 = G(gMixBuf), a2, a3, a5, a6, a4t;
    uint32_t d0 = 0, d1, d2, d3, d4 = 0, d5, d6 = 0, d7;
    mix_start(a1, &a2, &d1, &d2, &d3, &d5);
    if (R16(a1 + vRamp) != 0) goto l2776;
    if (R16(a1 + vTimer) != 0 && R32(a1 + vLoopEnd) != 0) {
        d0 = R32(a1 + vPtr) + d5;
        if (d0 < R32(a1 + vLoopEnd)) goto l268a;
        goto l287c;
    }
l267a:
    d0 = R32(a1 + vEnd) - R32(a1 + vPtr);
    if ((int32_t)(d0 - d5) <= 0) goto l283e;
l268a:
    d0 = setw(d0, R16(G(gBuf5M1)));
    if ((uint16_t)d2 == 0) goto l27fe;
    d5 = setw(d5, 0);
    if (R16(a1 + vInterp) != 0) goto l26f2;
    a3 = R32(a1 + vTable);
    do {                                             /* 026a2 */
        d6 = setb(d6, SMP(0)); d4 = setb(d4, R8(a3 + sxw(d6))); STEP(); d4 = swap32(d4);
        d6 = setb(d6, SMP(0)); d4 = setb(d4, R8(a3 + sxw(d6))); STEP(); ADDL(d4);
        d6 = setb(d6, SMP(0)); d4 = setb(d4, R8(a3 + sxw(d6))); STEP(); d4 = swap32(d4);
        d6 = setb(d6, SMP(0)); d4 = setb(d4, R8(a3 + sxw(d6))); STEP(); ADDL(d4);
        d6 = setb(d6, SMP(0)); d4 = setb(d4, R8(a3 + sxw(d6))); STEP(); ADDW(d4);
    } while (DBRA(d0));
    a2 += sxw(d5);
    goto l2a46;
l26f2:
    if ((int16_t)R16(a1 + vInterp) > 0) goto l2734;
    d4 = setw(d4, 0);
    a3 = R32(G(gInterpTab1)); a6 = R32(G(gInterpTab2));
    emu_set_sp(emu_sp() - 4);                        /* move.l A4,-(A7): A4 = the table */
    d0 = setw(d0, R16(G(gBufFramesM1)));
    a4t = R32(a1 + vTable);
    d7 = 0;
    do {                                             /* 0270c */
        d7 = setw(d7, d3); d7 = setb(d7, SMP(0));
        d4 = setb(d4, R8(a3 + d7));
        { uint32_t s_ = (d3 & 0xffff) + (d2 & 0xffff); d3 = setw(d3, s_);
          d7 = setb(d7, SMP(1));
          d5 = setw(d5, d5 + d1 + (s_ >> 16)); }
        d4 = setb(d4, d4 + R8(a6 + d7));
        d4 = setb(d4, R8(a4t + sxw(d4)));
        ADDW(d4);
    } while (DBRA(d0));
    a2 += sxw(d5);
    emu_set_sp(emu_sp() + 4);
    goto l2a46;
l2734:
    d0 = setw(d0, R16(G(gBufFramesM1)));
    a3 = R32(a1 + vTable);
    do {                                             /* 0273c */
        if ((int16_t)d3 < 0) { d4 = setb(d4, SMP(0)); AVG(d4); }
        else d4 = setb(d4, SMP(0));
        d4 = setb(d4, R8(a3 + sxw(d4)));
        ADDW(d4);
        STEP();
    } while (DBRA(d0));
    a2 += sxw(d5);
    goto l2a46;
l2776:
    if ((int16_t)R16(a1 + vRamp) >= 0) {
        W16(a1 + vRamp, 0);
        d1 = 0x800080;
        d0 = setw(d0, R16(G(gBuf5M1)));
        do { ADDL(d1); ADDL(d1); ADDW(d1); } while (DBRA(d0));
        return;
    }
    W16(a1 + vRamp, 0);                              /* 02798 */
    a2 = R32(a1 + vOldPtr);
    d1 = setw(d1, R16(a1 + vOldStep));
    d2 = setw(d2, R16(a1 + vOldStep + 2));
    if (R16(G(gFullRate)) != 0) {
        uint32_t x = d1 & 1;
        d1 = setw(d1, (d1 & 0xffff) >> 1);
        d2 = setw(d2, ((d2 & 0xffff) >> 1) | (x << 15));
    }
    d3 = setw(d3, R16(a1 + vOldFrac));
    d0 = setw(d0, R16(G(gBufFramesM1)));
    a3 = R32(a1 + vOldTable);
    d7 = 0;
    d7 = setb(d7, R8(a2));
    d4 = setw(d4, 0);
    do {                                             /* 027c2 */
        d4 = setb(d4, R8(a2));
        d5 = setw(d5, 0);
        d7 = setb(d7, d7 ^ d4);
        if (d7 & 0x80) goto l27e0;
        d7 = setb(d7, d4);
        d4 = setb(d4, R8(a3 + sxw(d4)));
        ADDW(d4);
        STEP();
        a2 += sxw(d5);
    } while (DBRA(d0));
    return;
l27e0:
    d7 = setb(d7, d7 ^ d4);
    { uint32_t s_ = (d4 & 0xff) + (d7 & 0xff); d4 = setb(d4, s_ >> 1); }
    d4 = setb(d4, R8(a3 + sxw(d4)));
    d1 = setb(d1, 0x80);
    do {                                             /* 027ee */
        ADDW(d4);
        d4 = setb(d4, d4 - 0x80);
        d4 = setb(d4, (uint8_t)((int8_t)d4 >> 1));
        d4 = setb(d4, d4 + 0x80);
    } while (DBRA(d0));
    return;
l27fe:
    d4 = 0;
    a3 = R32(a1 + vTable);
    do {                                             /* 02804 */
        d6 = setb(d6, R8(a2)); d4 = setb(d4, R8(a3 + sxw(d6))); d4 = swap32(d4); a2 += sxw(d1);
        d6 = setb(d6, R8(a2)); d4 = setb(d4, R8(a3 + sxw(d6))); ADDL(d4); a2 += sxw(d1);
        d6 = setb(d6, R8(a2)); d4 = setb(d4, R8(a3 + sxw(d6))); d4 = swap32(d4); a2 += sxw(d1);
        d6 = setb(d6, R8(a2)); d4 = setb(d4, R8(a3 + sxw(d6))); ADDL(d4); a2 += sxw(d1);
        d6 = setb(d6, R8(a2)); d4 = setb(d4, R8(a3 + sxw(d6))); a2 += sxw(d1); ADDW(d4);
    } while (DBRA(d0));
    goto l2a46;
l283e:
    d0 = setw(d0, R16(G(gBufFramesM1)));
    a6 = R32(a1 + vEnd);
    a3 = R32(a1 + vTable);
    do {                                             /* 0284a */
        if (a2 >= a6) {
            do ADDW(0x80); while (DBRA(d0));
            break;
        }
        d6 = setb(d6, R8(a2));
        d4 = setb(d4, R8(a3 + sxw(d6)));
        d5 = setw(d5, 0);
        ADDW(d4);
        STEP();
        a2 += sxw(d5);
    } while (DBRA(d0));
    W16(a1 + vTimer, 0xffff);                        /* 02864 */
    call_done_proc(a1);
    return;
l287c:
    d0 = R32(a1 + vLoopCnt);
    if (d0 != 0) {
        a6 = d0;
        W16(a1 + vTimer, 0x7fff);
        if (R16(a6) >= 0) {
            if (R16(a6) == 0) goto l267a;
            W16(a6, R16(a6) - 1);
            if (R16(a6) == 0) goto l267a;
        }
    }
    if ((uint16_t)d2 == 0) goto l2946;
    if (R16(a1 + vInterp) != 0) goto l2970;
    d0 = setw(d0, R16(G(gBuf5M1)));
    a6 = R32(a1 + vLoopEnd);
    a3 = R32(a1 + vTable);
    do {                                             /* 028b2 */
        for (int k = 0; k < 5; k++) {
            if (a2 >= a6) a2 = a2 - a6 + R32(a1 + vLoopStart);
            d6 = setb(d6, R8(a2));
            d4 = setb(d4, R8(a3 + sxw(d6)));
            d5 = setw(d5, 0);
            ADDW(d4);
            STEP();
            a2 += sxw(d5);
        }
    } while (DBRA(d0));
    goto l2a46;
l2946:
    d0 = setw(d0, R16(G(gBufFramesM1)));
    a6 = R32(a1 + vLoopEnd);
    a3 = R32(a1 + vTable);
    do {                                             /* 02952 */
        if (a2 >= a6) a2 = a2 - a6 + R32(a1 + vLoopStart);
        d6 = setb(d6, R8(a2));
        d4 = setb(d4, R8(a3 + sxw(d6)));
        ADDW(d4);
        a2 += sxw(d1);
    } while (DBRA(d0));
    goto l2a46;
l2970:
    if ((int16_t)R16(a1 + vInterp) > 0) goto l29ee;
    d7 = 0;
    emu_set_sp(emu_sp() - 4);                        /* move.l A5,-(A7) */
    d0 = setw(d0, R16(G(gBufFramesM1)));
    a5 = R32(a1 + vLoopEnd);
    d1 = setw(d1, d1 - 2);
    a5 -= 1;
    a3 = R32(G(gInterpTab1)); a6 = R32(G(gInterpTab2));
    emu_set_sp(emu_sp() - 4);                        /* move.l A4,-(A7) */
    a4t = R32(a1 + vTable);
    do {                                             /* 02992 */
        if (a2 >= a5) goto l29c0;
    l2998:
        d7 = setw(d7, d3); d7 = setb(d7, R8(a2)); a2++;
        d5 = setw(d5, 0);
        d4 = setb(d4, R8(a3 + d7));
        { uint32_t s_ = (d3 & 0xffff) + (d2 & 0xffff); d3 = setw(d3, s_);
          d7 = setb(d7, R8(a2)); a2++;
          d5 = setw(d5, d5 + d1 + (s_ >> 16)); }
        d4 = setb(d4, d4 + R8(a6 + d7));
        d4 = setb(d4, R8(a4t + sxw(d4)));
        a2 += sxw(d5);
        ADDW(d4);
    } while (DBRA(d0));
    emu_set_sp(emu_sp() + 8);                        /* 029b8 */
    goto l2a46;
l29c0:
    a5 += 1;
    d4 = setw(d4, 0);
    d1 = setw(d1, d1 + 2);
    do {                                             /* 029c6 */
        if (a2 >= a5) {                              /* 029e2 */
            d1 = setw(d1, d1 - 2);
            a2 = a2 - a5 + R32(a1 + vLoopStart);
            a5 -= 1;
            goto l2998;
        }
        d4 = setb(d4, R8(a2));
        d4 = setb(d4, R8(a4t + sxw(d4)));
        d5 = setw(d5, 0);
        ADDW(d4);
        STEP();
        a2 += sxw(d5);
    } while (DBRA(d0));
    emu_set_sp(emu_sp() + 8);
    goto l2a46;
l29ee:
    d4 = setw(d4, 0);
    d0 = setw(d0, R16(G(gBufFramesM1)));
    a6 = R32(a1 + vLoopEnd);
    a3 = R32(a1 + vTable);
    do {                                             /* 029fc */
        if (a2 >= a6) a2 = a2 - a6 + R32(a1 + vLoopStart);
        d4 = setb(d4, R8(a2));
        d5 = setw(d5, 0);
        if ((int16_t)d3 < 0) { uint32_t s_ = (d4 & 0xff) + R8(a2 + 1); d4 = setb(d4, s_ >> 1); }
        d4 = setb(d4, R8(a3 + sxw(d4)));
        ADDW(d4);
        STEP();
        a2 += sxw(d5);
    } while (DBRA(d0));
    a2 += sxw(d5);
l2a46:
    W32(a1 + vPtr, a2);
    W16(a1 + vFrac, d3);
}

/* 02a50: sound effect voice (vDouble), full level: every sample counts twice */
static void mix_dbl(maddr a1) {
    maddr a0 = G(gMixBuf), a2, a3, a5, a6;
    uint32_t d0 = 0, d1, d2, d3, d4 = 0, d5, d6 = 0, d7;
    mix_start(a1, &a2, &d1, &d2, &d3, &d5);
    if (R16(a1 + vRamp) != 0) goto l2b98;
    if (R16(a1 + vTimer) != 0 && R32(a1 + vLoopEnd) != 0) {
        d0 = R32(a1 + vPtr) + d5;
        if (d0 < R32(a1 + vLoopEnd)) goto l2ac6;
        goto l2c8a;
    }
l2ab6:
    d0 = R32(a1 + vEnd) - R32(a1 + vPtr);
    if ((int32_t)(d0 - d5) <= 0) goto l2c4a;
l2ac6:
    d0 = setw(d0, R16(G(gBuf5M1)));
    if ((uint16_t)d2 == 0) goto l2c16;
    d5 = setw(d5, 0);
    if (R16(a1 + vInterp) != 0) goto l2b22;
    do {                                             /* 02ada */
        d4 = 0; d4 = setb(d4, SMP(0)); STEP(); d4 = swap32(d4); d4 = setb(d4, SMP(0)); STEP(); d4 += d4; ADDL(d4);
        d4 = 0; d4 = setb(d4, SMP(0)); STEP(); d4 = swap32(d4); d4 = setb(d4, SMP(0)); STEP(); d4 += d4; ADDL(d4);
        d4 = 0; d4 = setb(d4, SMP(0)); STEP(); d4 = setw(d4, d4 + d4); ADDW(d4);
    } while (DBRA(d0));
    a2 += sxw(d5);
    goto l2e42;
l2b22:
    if ((int16_t)R16(a1 + vInterp) > 0) goto l2b5a;
    a3 = R32(G(gInterpTab1)); a6 = R32(G(gInterpTab2));
    d0 = setw(d0, R16(G(gBufFramesM1)));
    d7 = 0;
    do {                                             /* 02b34 */
        d4 = setw(d4, 0);
        d7 = setw(d7, d3); d7 = setb(d7, SMP(0));
        d4 = setb(d4, R8(a3 + d7));
        { uint32_t s_ = (d3 & 0xffff) + (d2 & 0xffff); d3 = setw(d3, s_);
          d7 = setb(d7, SMP(1));
          d5 = setw(d5, d5 + d1 + (s_ >> 16)); }
        d4 = setb(d4, d4 + R8(a6 + d7));
        d4 = setw(d4, d4 + d4);
        ADDW(d4);
    } while (DBRA(d0));
    a2 += sxw(d5);
    goto l2e42;
l2b5a:
    d0 = setw(d0, R16(G(gBufFramesM1)));
    d6 = setw(d6, 0);
    do {                                             /* 02b60 */
        if ((int16_t)d3 < 0) {
            d4 = setw(d4, 0); d4 = setb(d4, SMP(0));
            d6 = setb(d6, SMP(1));
            d4 = setw(d4, d4 + d6);
        } else {
            d4 = setw(d4, 0); d4 = setb(d4, SMP(0));
            d4 = setw(d4, d4 + d4);
        }
        ADDW(d4);
        STEP();
    } while (DBRA(d0));
    a2 += sxw(d5);
    goto l2e42;
l2b98:
    if ((int16_t)R16(a1 + vRamp) >= 0) {
        W16(a1 + vRamp, 0);
        d1 = 0x1000100;
        d0 = setw(d0, R16(G(gBuf5M1)));
        do { ADDL(d1); ADDL(d1); ADDW(d1); } while (DBRA(d0));
        return;
    }
    W16(a1 + vRamp, 0);                              /* 02bba */
    a2 = R32(a1 + vOldPtr);
    d1 = setw(d1, R16(a1 + vOldStep));
    d2 = setw(d2, R16(a1 + vOldStep + 2));
    if (R16(G(gFullRate)) != 0) {
        uint32_t x = d1 & 1;
        d1 = setw(d1, (d1 & 0xffff) >> 1);
        d2 = setw(d2, ((d2 & 0xffff) >> 1) | (x << 15));
    }
    d3 = setw(d3, R16(a1 + vOldFrac));
    d0 = setw(d0, R16(G(gBufFramesM1)));
    d7 = 0;
    d7 = setb(d7, R8(a2));
    do {                                             /* 02bde */
        d4 = setb(d4, R8(a2));
        d5 = setw(d5, 0);
        d7 = setb(d7, d7 ^ d4);
        if (d7 & 0x80) goto l2bfa;
        d7 = setb(d7, d4);
        ADDW0(d4); ADDW(d4);
        STEP();
        a2 += sxw(d5);
    } while (DBRA(d0));
    return;
l2bfa:
    d7 = setb(d7, d7 ^ d4);
    { uint32_t s_ = (d4 & 0xff) + (d7 & 0xff); d4 = setb(d4, s_ >> 1); }
    d1 = setb(d1, 0x80);
    do {                                             /* 02c04 */
        ADDW0(d4); ADDW(d4);
        d4 = setb(d4, d4 - 0x80);
        d4 = setb(d4, (uint8_t)((int8_t)d4 >> 1));
        d4 = setb(d4, d4 + 0x80);
    } while (DBRA(d0));
    return;
l2c16:
    d4 = 0;
    do {                                             /* 02c18 */
        d4 = 0; d4 = setb(d4, R8(a2)); d4 = swap32(d4); a2 += sxw(d1); d4 = setb(d4, R8(a2)); d4 += d4; ADDL(d4); a2 += sxw(d1);
        d4 = 0; d4 = setb(d4, R8(a2)); d4 = swap32(d4); a2 += sxw(d1); d4 = setb(d4, R8(a2)); d4 += d4; ADDL(d4); a2 += sxw(d1);
        d4 = 0; d4 = setb(d4, R8(a2)); a2 += sxw(d1); d4 = setw(d4, d4 + d4); ADDW(d4);
    } while (DBRA(d0));
    goto l2e42;
l2c4a:
    d0 = setw(d0, R16(G(gBufFramesM1)));
    a6 = R32(a1 + vEnd);
    do {                                             /* 02c52 */
        if (a2 >= a6) {
            do ADDW(0x100); while (DBRA(d0));
            break;
        }
        d4 = setw(d4, 0); d4 = setb(d4, R8(a2)); d4 = setw(d4, d4 + d4);
        d5 = setw(d5, 0);
        ADDW(d4);
        STEP();
        a2 += sxw(d5);
    } while (DBRA(d0));
    W16(a1 + vTimer, 0xffff);                        /* 02c6c (D0/D1 loaded, unused) */
    call_done_proc(a1);
    return;
l2c8a:
    d0 = R32(a1 + vLoopCnt);
    if (d0 != 0) {
        a6 = d0;
        W16(a1 + vTimer, 0x7fff);
        if (R16(a6) >= 0) {
            if (R16(a6) == 0) goto l2ab6;
            W16(a6, R16(a6) - 1);
            if (R16(a6) == 0) goto l2ab6;
        }
    }
    if ((uint16_t)d2 == 0) goto l2d50;
    if (R16(a1 + vInterp) != 0) goto l2d76;
    d0 = setw(d0, R16(G(gBuf5M1)));
    a6 = R32(a1 + vLoopEnd);
    do {                                             /* 02cbc */
        for (int k = 0; k < 5; k++) {
            if (a2 >= a6) a2 = a2 - a6 + R32(a1 + vLoopStart);
            d4 = setw(d4, 0); d4 = setb(d4, R8(a2)); d4 = setw(d4, d4 + d4);
            d5 = setw(d5, 0);
            ADDW(d4);
            STEP();
            a2 += sxw(d5);
        }
    } while (DBRA(d0));
    goto l2e42;
l2d50:
    d0 = setw(d0, R16(G(gBufFramesM1)));
    a6 = R32(a1 + vLoopEnd);
    do {                                             /* 02d58 */
        if (a2 >= a6) a2 = a2 - a6 + R32(a1 + vLoopStart);
        d4 = setw(d4, 0); d4 = setb(d4, R8(a2)); d4 = setw(d4, d4 + d4);
        ADDW(d4);
        a2 += sxw(d1);
    } while (DBRA(d0));
    goto l2e42;
l2d76:
    if ((int16_t)R16(a1 + vInterp) > 0) goto l2df0;
    d7 = 0; d6 = 0;
    emu_set_sp(emu_sp() - 4);                        /* move.l A5,-(A7) */
    d0 = setw(d0, R16(G(gBufFramesM1)));
    a5 = R32(a1 + vLoopEnd);
    d1 = setw(d1, d1 - 2);
    a5 -= 1;
    a3 = R32(G(gInterpTab1)); a6 = R32(G(gInterpTab2));
    do {                                             /* 02d94 */
        if (a2 >= a5) goto l2dc0;
    l2d9a:
        d7 = setw(d7, d3); d7 = setb(d7, R8(a2)); a2++;
        d5 = setw(d5, 0);
        d4 = setw(d4, 0);
        d4 = setb(d4, R8(a3 + d7));
        { uint32_t s_ = (d3 & 0xffff) + (d2 & 0xffff); d3 = setw(d3, s_);
          d7 = setb(d7, R8(a2)); a2++;
          d5 = setw(d5, d5 + d1 + (s_ >> 16)); }
        d6 = setb(d6, R8(a6 + d7));
        d4 = setw(d4, d4 + d6);
        a2 += sxw(d5);
        ADDW(d4);
    } while (DBRA(d0));
    emu_set_sp(emu_sp() + 4);
    goto l2e42;
l2dc0:
    a5 += 1;
    d4 = setw(d4, 0);
    d1 = setw(d1, d1 + 2);
    do {                                             /* 02dc6 */
        if (a2 >= a5) {                              /* 02de4 */
            d1 = setw(d1, d1 - 2);
            a2 = a2 - a5 + R32(a1 + vLoopStart);
            a5 -= 1;
            goto l2d9a;
        }
        d4 = setb(d4, R8(a2));
        d5 = setw(d5, 0);
        ADDW0(d4); ADDW(d4);
        STEP();
        a2 += sxw(d5);
    } while (DBRA(d0));
    emu_set_sp(emu_sp() + 4);
    goto l2e42;
l2df0:
    d0 = setw(d0, R16(G(gBufFramesM1)));
    d6 = 0;
    a6 = R32(a1 + vLoopEnd);
    do {                                             /* 02dfa */
        if (a2 >= a6) a2 = a2 - a6 + R32(a1 + vLoopStart);
        if ((int16_t)d3 < 0) {
            d4 = setw(d4, 0); d4 = setb(d4, R8(a2));
            d5 = setw(d5, 0);
            d6 = setb(d6, R8(a2 + 1));
            d4 = setw(d4, d4 + d6);
        } else {
            d4 = setw(d4, 0); d4 = setb(d4, R8(a2)); d4 = setw(d4, d4 + d4);
            d5 = setw(d5, 0);
        }
        ADDW(d4);
        STEP();
        a2 += sxw(d5);
    } while (DBRA(d0));
    a2 += sxw(d5);
l2e42:
    W32(a1 + vPtr, a2);
    W16(a1 + vFrac, d3);
}

/* 02e4c: sound effect voice through its amplitude table, counted twice */
static void mix_dblvol(maddr a1) {
    maddr a0 = G(gMixBuf), a2, a3, a5, a6, a4t;
    uint32_t d0 = 0, d1, d2, d3, d4 = 0, d5, d6 = 0, d7;
    mix_start(a1, &a2, &d1, &d2, &d3, &d5);
    if (R16(a1 + vRamp) != 0) goto l2fc4;
    if (R16(a1 + vTimer) != 0 && R32(a1 + vLoopEnd) != 0) {
        d0 = R32(a1 + vPtr) + d5;
        if (d0 < R32(a1 + vLoopEnd)) goto l2ec2;
        goto l30dc;
    }
l2eb2:
    d0 = R32(a1 + vEnd) - R32(a1 + vPtr);
    if ((int32_t)(d0 - d5) <= 0) goto l309c;
l2ec2:
    d0 = setw(d0, R16(G(gBuf5M1)));
    if ((uint16_t)d2 == 0) goto l3050;
    d5 = setw(d5, 0);
    if (R16(a1 + vInterp) != 0) goto l2f36;
    a3 = R32(a1 + vTable);
    do {                                             /* 02eda */
        d4 = 0;
        d6 = setb(d6, SMP(0)); d4 = setb(d4, R8(a3 + sxw(d6))); STEP(); d4 = swap32(d4);
        d6 = setb(d6, SMP(0)); d4 = setb(d4, R8(a3 + sxw(d6))); STEP(); d4 += d4; ADDL(d4);
        d4 = 0;
        d6 = setb(d6, SMP(0)); d4 = setb(d4, R8(a3 + sxw(d6))); STEP(); d4 = swap32(d4);
        d6 = setb(d6, SMP(0)); d4 = setb(d4, R8(a3 + sxw(d6))); STEP(); d4 += d4; ADDL(d4);
        d4 = 0;
        d6 = setb(d6, SMP(0)); d4 = setb(d4, R8(a3 + sxw(d6))); STEP(); d4 = setw(d4, d4 + d4); ADDW(d4);
    } while (DBRA(d0));
    a2 += sxw(d5);
    goto l32d0;
l2f36:
    if ((int16_t)R16(a1 + vInterp) > 0) goto l2f7a;
    d4 = setw(d4, 0);
    a3 = R32(G(gInterpTab1)); a6 = R32(G(gInterpTab2));
    emu_set_sp(emu_sp() - 4);                        /* move.l A4,-(A7) */
    d0 = setw(d0, R16(G(gBufFramesM1)));
    a4t = R32(a1 + vTable);
    d7 = 0;
    do {                                             /* 02f50 */
        d7 = setw(d7, d3); d7 = setb(d7, SMP(0));
        d4 = setb(d4, R8(a3 + d7));
        { uint32_t s_ = (d3 & 0xffff) + (d2 & 0xffff); d3 = setw(d3, s_);
          d7 = setb(d7, SMP(1));
          d5 = setw(d5, d5 + d1 + (s_ >> 16)); }
        d4 = setb(d4, d4 + R8(a6 + d7));
        d4 = setb(d4, R8(a4t + sxw(d4)));
        ADDW0(d4); ADDW(d4);
    } while (DBRA(d0));
    a2 += sxw(d5);
    emu_set_sp(emu_sp() + 4);
    goto l32d0;
l2f7a:
    d0 = setw(d0, R16(G(gBufFramesM1)));
    a3 = R32(a1 + vTable);
    do {                                             /* 02f82 */
        d4 = setw(d4, 0);
        if ((int16_t)d3 < 0) { d4 = setb(d4, SMP(0)); AVG(d4); }
        else d4 = setb(d4, SMP(0));
        d4 = setb(d4, R8(a3 + sxw(d4)));
        d4 = setw(d4, d4 + d4);
        ADDW(d4);
        STEP();
    } while (DBRA(d0));
    a2 += sxw(d5);
    goto l32d0;
l2fc4:
    if ((int16_t)R16(a1 + vRamp) >= 0) {
        W16(a1 + vRamp, 0);
        d1 = 0x1000100;
        d0 = setw(d0, R16(G(gBuf5M1)));
        do { ADDL(d1); ADDL(d1); ADDW(d1); } while (DBRA(d0));
        return;
    }
    W16(a1 + vRamp, 0);                              /* 02fe6 */
    a2 = R32(a1 + vOldPtr);
    d1 = setw(d1, R16(a1 + vOldStep));
    d2 = setw(d2, R16(a1 + vOldStep + 2));
    if (R16(G(gFullRate)) != 0) {
        uint32_t x = d1 & 1;
        d1 = setw(d1, (d1 & 0xffff) >> 1);
        d2 = setw(d2, ((d2 & 0xffff) >> 1) | (x << 15));
    }
    d3 = setw(d3, R16(a1 + vOldFrac));
    d0 = setw(d0, R16(G(gBufFramesM1)));
    a3 = R32(a1 + vOldTable);
    d7 = 0;
    d7 = setb(d7, R8(a2));
    d4 = setw(d4, 0);
    do {                                             /* 03010 */
        d4 = setb(d4, R8(a2));
        d5 = setw(d5, 0);
        d7 = setb(d7, d7 ^ d4);
        if (d7 & 0x80) goto l3030;
        d7 = setb(d7, d4);
        d4 = setb(d4, R8(a3 + sxw(d4)));
        ADDW0(d4); ADDW(d4);
        STEP();
        a2 += sxw(d5);
    } while (DBRA(d0));
    return;
l3030:
    d7 = setb(d7, d7 ^ d4);
    { uint32_t s_ = (d4 & 0xff) + (d7 & 0xff); d4 = setb(d4, s_ >> 1); }
    d4 = setb(d4, R8(a3 + sxw(d4)));
    d1 = setb(d1, 0x80);
    do {                                             /* 0303e */
        ADDW0(d4); ADDW(d4);
        d4 = setb(d4, d4 - 0x80);
        d4 = setb(d4, (uint8_t)((int8_t)d4 >> 1));
        d4 = setb(d4, d4 + 0x80);
    } while (DBRA(d0));
    return;
l3050:
    d4 = 0;
    a3 = R32(a1 + vTable);
    do {                                             /* 03056 */
        d4 = 0;
        d6 = setb(d6, R8(a2)); d4 = setb(d4, R8(a3 + sxw(d6))); d4 = swap32(d4); a2 += sxw(d1);
        d6 = setb(d6, R8(a2)); d4 = setb(d4, R8(a3 + sxw(d6))); d4 += d4; ADDL(d4); a2 += sxw(d1);
        d4 = 0;
        d6 = setb(d6, R8(a2)); d4 = setb(d4, R8(a3 + sxw(d6))); d4 = swap32(d4); a2 += sxw(d1);
        d6 = setb(d6, R8(a2)); d4 = setb(d4, R8(a3 + sxw(d6))); d4 += d4; ADDL(d4); a2 += sxw(d1);
        d4 = 0;
        d6 = setb(d6, R8(a2)); d4 = setb(d4, R8(a3 + sxw(d6))); a2 += sxw(d1); d4 = setw(d4, d4 + d4); ADDW(d4);
    } while (DBRA(d0));
    goto l32d0;
l309c:
    d0 = setw(d0, R16(G(gBufFramesM1)));
    a6 = R32(a1 + vEnd);
    a3 = R32(a1 + vTable);
    do {                                             /* 030a8 */
        if (a2 >= a6) {
            do ADDW(0x100); while (DBRA(d0));
            break;
        }
        d6 = setb(d6, R8(a2));
        d4 = setb(d4, R8(a3 + sxw(d6)));
        d5 = setw(d5, 0);
        ADDW0(d4); ADDW(d4);
        STEP();
        a2 += sxw(d5);
    } while (DBRA(d0));
    W16(a1 + vTimer, 0xffff);                        /* 030c4 */
    call_done_proc(a1);
    return;
l30dc:
    d0 = R32(a1 + vLoopCnt);
    if (d0 != 0) {
        a6 = d0;
        W16(a1 + vTimer, 0x7fff);
        if (R16(a6) >= 0) {
            if (R16(a6) == 0) goto l2eb2;
            W16(a6, R16(a6) - 1);
            if (R16(a6) == 0) goto l2eb2;
        }
    }
    if ((uint16_t)d2 == 0) goto l31bc;
    if (R16(a1 + vInterp) != 0) goto l31ea;
    d0 = setw(d0, R16(G(gBuf5M1)));
    a6 = R32(a1 + vLoopEnd);
    a3 = R32(a1 + vTable);
    do {                                             /* 03112 */
        for (int k = 0; k < 5; k++) {
            if (a2 >= a6) a2 = a2 - a6 + R32(a1 + vLoopStart);
            d4 = setw(d4, 0);
            d6 = setb(d6, R8(a2));
            d4 = setb(d4, R8(a3 + sxw(d6)));
            d5 = setw(d5, 0);
            d4 = setw(d4, d4 + d4);
            ADDW(d4);
            STEP();
            a2 += sxw(d5);
        }
    } while (DBRA(d0));
    goto l32d0;
l31bc:
    d0 = setw(d0, R16(G(gBufFramesM1)));
    a6 = R32(a1 + vLoopEnd);
    a3 = R32(a1 + vTable);
    do {                                             /* 031c8 */
        if (a2 >= a6) a2 = a2 - a6 + R32(a1 + vLoopStart);
        d4 = setw(d4, 0);
        d6 = setb(d6, R8(a2));
        d4 = setb(d4, R8(a3 + sxw(d6)));
        d4 = setw(d4, d4 + d4);
        ADDW(d4);
        a2 += sxw(d1);
    } while (DBRA(d0));
    goto l32d0;
l31ea:
    if ((int16_t)R16(a1 + vInterp) > 0) goto l3270;
    d7 = 0;
    emu_set_sp(emu_sp() - 4);                        /* move.l A5,-(A7) */
    d0 = setw(d0, R16(G(gBufFramesM1)));
    a5 = R32(a1 + vLoopEnd);
    d1 = setw(d1, d1 - 2);
    a5 -= 1;
    a3 = R32(G(gInterpTab1)); a6 = R32(G(gInterpTab2));
    emu_set_sp(emu_sp() - 4);                        /* move.l A4,-(A7) */
    a4t = R32(a1 + vTable);
    do {                                             /* 0320c */
        if (a2 >= a5) goto l323e;
    l3212:
        d4 = setw(d4, 0);
        d7 = setw(d7, d3); d7 = setb(d7, R8(a2)); a2++;
        d5 = setw(d5, 0);
        d4 = setb(d4, R8(a3 + d7));
        { uint32_t s_ = (d3 & 0xffff) + (d2 & 0xffff); d3 = setw(d3, s_);
          d7 = setb(d7, R8(a2)); a2++;
          d5 = setw(d5, d5 + d1 + (s_ >> 16)); }
        d4 = setb(d4, d4 + R8(a6 + d7));
        d4 = setb(d4, R8(a4t + sxw(d4)));
        d4 = setw(d4, d4 + d4);
        a2 += sxw(d5);
        ADDW(d4);
    } while (DBRA(d0));
    emu_set_sp(emu_sp() + 8);                        /* 03236 */
    goto l32d0;
l323e:
    a5 += 1;
    d4 = setw(d4, 0);
    d1 = setw(d1, d1 + 2);
    do {                                             /* 03244 */
        if (a2 >= a5) {                              /* 03264 */
            d1 = setw(d1, d1 - 2);
            a2 = a2 - a5 + R32(a1 + vLoopStart);
            a5 -= 1;
            goto l3212;
        }
        d4 = setw(d4, 0);
        d4 = setb(d4, R8(a2));
        d4 = setb(d4, R8(a4t + sxw(d4)));
        d5 = setw(d5, 0);
        d4 = setw(d4, d4 + d4);
        ADDW(d4);
        STEP();
        a2 += sxw(d5);
    } while (DBRA(d0));
    emu_set_sp(emu_sp() + 8);
    goto l32d0;
l3270:
    d4 = setw(d4, 0);
    d0 = setw(d0, R16(G(gBufFramesM1)));
    a6 = R32(a1 + vLoopEnd);
    a3 = R32(a1 + vTable);
    do {                                             /* 0327e */
        if (a2 >= a6) a2 = a2 - a6 + R32(a1 + vLoopStart);
        d4 = setw(d4, 0);
        d4 = setb(d4, R8(a2));
        d5 = setw(d5, 0);
        if ((int16_t)d3 < 0) { uint32_t s_ = (d4 & 0xff) + R8(a2 + 1); d4 = setb(d4, s_ >> 1); }
        d4 = setb(d4, R8(a3 + sxw(d4)));
        d4 = setw(d4, d4 + d4);
        ADDW(d4);
        STEP();
        a2 += sxw(d5);
    } while (DBRA(d0));
    a2 += sxw(d5);
l32d0:
    W32(a1 + vPtr, a2);
    W16(a1 + vFrac, d3);
}
#undef STEP
#undef SMP
#undef ADDW
#undef ADDW0
#undef ADDL
#undef DBRA
#undef AVG

/* 0219e: mix one voice (A1 = its slot): note timing, release, pitch envelope, then one of
 * the four mixers */
static void mix_voice(maddr a1) {
    SUB(0);
    if (R16(a1 + vTimer) != 0) goto l2200;
    if (R16(a1 + vRamp) != 0) goto l224a;
    if (R16(a1 + vRelease) >= 0) {
        W16(a1 + vRelease, R16(a1 + vRelease) - 1);
        goto l220c;
    }
    W16(a1 + vTimer, 0xffff);                        /* released: the voice is free now */
    call_done_proc(a1);
    if (!(R8(G(gSongFlags)) & 0x02)) {
        uint32_t d0 = R32(a1 + vEnd) - R32(a1 + vPtr), d5 = R32(a1 + vAdvance);
        if (d5 != 0 && (int32_t)(d0 - d5) > 0) {    /* fade out the rest next buffer */
            W8(a1 + vRamp, 0xff);
            W32(a1 + vOldPtr, R32(a1 + vPtr));
            W32(a1 + vOldStep, R32(a1 + vStep));
            W32(a1 + vOldFrac, R32(a1 + vFrac));
            W32(a1 + vOldLevel, R32(a1 + vLevel));
            W32(a1 + vOldTable, R32(a1 + vTable));
        }
    }
l2200:
    if (R16(a1 + vRamp) != 0) goto l224a;
    W16(a1 + vTimer, R16(a1 + vTimer) - 1);
l220c:
    {
        maddr a2 = R32(a1 + vBend);
        uint32_t d0;
        for (;;) {
            d0 = RU16(a2); a2 += 2;
            if (d0 == 0x8000) goto l224a;
            W32(a1 + vBend, a2);
            if ((d0 & 0x8000) && d0 < 0x8200) {     /* jump within the envelope */
                d0 = (uint16_t)(d0 - 0x8100);
                d0 = (uint16_t)(d0 << 1);
                a2 += sxw(d0);
                a2 -= 2;
                continue;
            }
            break;
        }
        d0 = sxw(d0);
        if (d0 != 0) {
            uint32_t d1 = (uint32_t)((int32_t)(int16_t)R32(a1 + vBendStep) * (int32_t)(int16_t)d0);
            W32(a1 + vStep, R32(a1 + vStep) + d1);
            W32(a1 + vAdvance, 0);
        }
    }
l224a:
    {
        int vol = R16(a1 + vRamp) == 0 ? R16(a1 + vLevel) != 0 : R16(a1 + vOldLevel) != 0;
        int dbl = R16(a1 + vDouble) != 0;
        if (!vol) { if (dbl) mix_dbl(a1); else mix_plain(a1); }
        else      { if (dbl) mix_dblvol(a1); else mix_vol(a1); }
    }
    W32(G(gMixCount), R32(G(gMixCount)) + 1);        /* bra $215a */
}

/* ================================================================== voices === */

/* 032da: selector 0x1b: callback for the sound effects started next */
static void set_done_proc(uint32_t d0, uint32_t d1) {
    SUB(0);
    W32(G(gDoneProc), d0);
    W32(G(gDoneRefCon), d1);
}

/* 032e4: (not referenced) copy the callback into the voice A1 */
static void __attribute__((unused)) voice_set_done_proc(maddr a1) {
    SUB(0);
    W32(a1 + vDoneProc, R32(G(gDoneProc)));
    W32(a1 + vRefCon, R32(G(gDoneRefCon)));
}

/* 032f6: call the voice's done callback once: proc(refCon), C convention. Values below
 * 0x1000 are not procedures. */
static void call_done_proc(maddr a1) {
    SUB(60);                                         /* movem.l D0-D7/A0-A6 */
    uint32_t d0 = R32(a1 + vDoneProc);
    if (d0 == 0 || (int32_t)d0 < 0x1000) return;
    W32(a1 + vDoneProc, 0);
    uint32_t r[16];
    regs_save(r);
    cpu_set(R_D0, d0); cpu_set(R_A0, d0); cpu_set(R_A1, a1); cpu_set(R_A4, a4_);
    call_ptr(d0, "l", R32(a1 + vRefCon));
    regs_restore(r, 0x7fffu);
}

/* 03322: selector 6: free every voice */
static void free_all_voices(void) {
    SUB(0);
    uint32_t d0 = RU16(G(gNoteVoices));
    if (d0 == 0) return;
    d0 = (uint16_t)(d0 - 1);
    maddr a0 = G(gVoices + vTimer);
    do { W32(a0, 0xffffffff); a0 += 4; d0 = (uint16_t)(d0 - 1); } while (d0 != 0xffff);
}

/* 0333a: note off: release the note voices playing key D1 for (D6 id, D5 channel) */
static void note_off(uint32_t d0, uint32_t d1, uint32_t d5, uint32_t d6) {
    SUB(0);
    if (R8(G(gSongFlags)) & 0x01) d0 = setw(d0, R16(G(gSingleInst)));
    maddr a0 = G(gVoices);
    uint32_t d7 = RU16(G(gNoteVoices));
    if (d7 == 0) return;
    d7 = (uint16_t)(d7 - 1);
    do {
        if (R16(a0 + vTimer) >= 0 && (uint16_t)d1 == RU16(a0 + vKey) &&
            (uint16_t)d6 == RU16(a0 + vId) && (uint16_t)d5 == RU16(a0 + vChannel)) {
            uint32_t d2 = RU16(G(gSongFlags)) & 0x3f;
            W16(a0 + vTimer, 0);
            W16(a0 + vRelease, d2);
        }
        a0 += 4;
        d7 = (uint16_t)(d7 - 1);
    } while (d7 != 0xffff);
}

/* 03382: note on: D0 instrument, D1 key, D2 duration, D3 velocity, D5 channel, D6 id.
 * Picks the key split of the instrument, a voice (same note, then free, released,
 * free or released), and starts its 'snd ' at the pitch of the key. */
static void note_on(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3, uint32_t d5, uint32_t d6) {
    SUB(0);
    maddr a0, a1, a2;
    uint32_t d4 = 0, d7 = 0;
    if (R16(G(gScanMode)) == 1) {                    /* scanning the song: mark the instrument */
        W8(G(gInstUsed) + sxw(d0), 0);
        return;
    }
    W16(G(nInst), d0);
    W16(G(nKey), d1);
    W16(G(nChannel), d5);
    W16(G(nId), d6);
l33a4:
    d1 = setw(d1, R16(G(nKey)));
    d0 = setw(d0, d0 << 3);
    a1 = G(gInstTable);
    d7 = R32(a1 + sxw(d0));
    if (d7 == 0) return;
    a0 = d7;
    W16(G(nFlags), R16(a0 + 6));
    W8(G(nFlags5), R8(a0 + 5));
    if (R16(a0 + 2) != 0) {                          /* base key */
        d1 = setw(d1, d1 - R16(a0 + 2));
        d1 = setw(d1, d1 + 0x3c);
    }
    if (R16(a0 + 0xc) == 0) goto l3478;              /* no key splits */
    W16(G(nSplitTries), 0xc);
l33e0:
    W16(G(nSplitTries), R16(G(nSplitTries)) - 1);
    if (R16(G(nSplitTries)) == 0) goto l3478;
    d7 = setw(d7, d1);
    if (R8(G(nFlags)) & 0x01) {                      /* random split */
        d7 = setw(d7, random_word());
        d7 = setw(d7, d7 & 0x7f);
    }
    if (R8(G(nFlags5)) & 0x01) d7 = setw(d7, d3);    /* split by velocity */
    a2 = a0 + 0xc;
    d5 = setw(d5, R16(a2)); a2 += 2;
    W16(G(nOctave), 0);
    do {
        if (R8(a2) != 0 && (uint8_t)d7 < R8(a2)) { W8(G(nOctave), 0x0c); goto l3434; }
        if (R8(a2 + 1) >= 0x7f) goto l3452;
        if ((int8_t)d7 <= (int8_t)R8(a2 + 1)) goto l3452;
        W8(G(nOctave), 0xf4);
    l3434:
        a2 += 8;
        d5 = setw(d5, d5 - 1);
    } while ((uint16_t)d5 != 0);
    if (R8(G(nFlags)) & 0x01) goto l33e0;
    if (!(R8(G(nFlags)) & 0x20)) return;
    d1 = setb(d1, d1 + R8(G(nOctave)));              /* octave up/down and try again */
    if (!(d1 & 0x80)) goto l33e0;
l3452:
    if (R8(G(nFlags)) & 0x08) {                      /* the split names another instrument */
        d0 = setw(d0, R16(a2 + 2));
        if ((uint16_t)d0 == 0) goto l3478;
        goto l33a4;
    }
    if (R16(a2 + 2) == 0) goto l3478;
    d0 = R32(a2 + 4);
    if (d0 == 0) goto l3478;
    goto l347c;
l3478:
    d0 = R32(a1 + 4 + sxw(d0));
l347c:
    if (d0 == 0) return;
    d5 = 0;
    d5 = setw(d5, R16(a0 + 0xc));
    d5 = setw(d5, d5 << 3);
    d5 += a0;
    d5 += 0x10;                                      /* pitch envelope after the splits */
    W16(G(nNote), d1);
    W16(G(nDuration), d2);
    if (!(R8(G(gFlags2)) & 0x02) || (!(R8(G(gFlags2)) & 0x04) && !(R8(G(nFlags5)) & 0x40)))
        d3 = setw(d3, 0);
    W16(G(nVolume), d3);
    a1 = G(gVoices);
    d2 = setw(d2, 0);
    d4 = setw(d4, R16(G(gNoteVoices)));
    if ((uint16_t)d4 == 0) return;
    d7 = setw(d7, R16(G(nInst)));
    d1 = setw(d1, R16(G(nKey)));
    d4 = setw(d4, d4 - 1);
    d3 = setw(d3, R16(G(nChannel)));
    do {                                             /* the same note already playing? */
        if (R16(a1 + vTimer) >= 0) d2 = setw(d2, d2 + 1);
        if ((uint16_t)d1 == RU16(a1 + vKey) && (uint16_t)d3 == RU16(a1 + vChannel) &&
            (uint16_t)d6 == RU16(a1 + vId) && (uint16_t)d7 == RU16(a1 + vInst)) {
            if (R8(G(nFlags)) & 0x04) goto l3570;
            if (R16(a1 + vTimer) == 0) goto l3570;
        }
        a1 += 4;
        d4 = setw(d4, d4 - 1);
    } while ((uint16_t)d4 != 0xffff);
    if ((R8(G(gSongFlags)) & 0x40) && (uint16_t)d2 >= RU16(G(gMixLevel))) goto l3538;
    a1 = G(gVoices);                                 /* a free voice */
    d1 = setw(d1, R16(G(gNoteVoices)) - 1);
    do { if (R16(a1 + vTimer) < 0) goto l3570; a1 += 4; d1 = setw(d1, d1 - 1); } while ((uint16_t)d1 != 0xffff);
    goto l3550;
l3538:
    a1 = G(gVoices);                                 /* a released voice */
    d1 = setw(d1, R16(G(gNoteVoices)) - 1);
    do { if (R16(a1 + vTimer) == 0) goto l3570; a1 += 4; d1 = setw(d1, d1 - 1); } while ((uint16_t)d1 != 0xffff);
l3550:
    a1 = G(gVoices);
    d1 = setw(d1, R16(G(gNoteVoices)) - 1);
    do { if (R16(a1 + vTimer) <= 0) goto l3570; a1 += 4; d1 = setw(d1, d1 - 1); } while ((uint16_t)d1 != 0xffff);
    return;
l3570:
    W16(a1 + vId, R16(G(nId)));
    W16(a1 + vChannel, R16(G(nChannel)));
    if (!(R8(G(gSongFlags)) & 0x02)) {
        W16(a1 + vRamp, 1);
        if (R16(a1 + vTimer) >= 0) {
            d6 = R32(a1 + vAdvance);
            if (d6 != 0) {
                d1 = R32(a1 + vEnd) - R32(a1 + vPtr);
                if ((int32_t)(d1 - d6) > 0) {        /* fade out the old note */
                    W8(a1 + vRamp, 0xff);
                    W32(a1 + vOldPtr, R32(a1 + vPtr));
                    W32(a1 + vOldStep, R32(a1 + vStep));
                    W32(a1 + vOldFrac, R32(a1 + vFrac));
                    W32(a1 + vOldTable, R32(a1 + vTable));
                    W32(a1 + vOldLevel, R32(a1 + vLevel));
                }
            }
        }
    }
    a0 = d0;                                         /* 'snd ' resource data */
    if (R16(a0) == 2) {
        d0 = setw(d0, R16(a0 + 4));
        d0 = setw(d0, d0 << 3);
        a0 += sxw(d0);
        a0 += 6;
    } else if (R16(a0) == 1) {
        d0 = setw(d0, R16(a0 + 0xa));
        d0 = setw(d0, d0 << 3);
        a0 += sxw(d0);
        a0 += 0xc;
    } else {
        d0 = setw(d0, R16(G(nInst)));
        drv_error(0xffee, sCorruptSnd, d0);
        return;
    }
    a0 += 0x16;                                      /* sample data after the SoundHeader */
    W32(a1 + vPtr, a0);
    W32(a1 + vEnd, a0);
    W32(a1 + vLoopStart, a0);
    W32(a1 + vLoopEnd, a0);
    a0 -= 0x16;
    W32(a1 + vLoopCnt, 0);
    W16(a1 + vInterp, 0);
    if (!(R8(G(nFlags)) & 0x80)) {
        if ((R8(G(gSongFlags)) & 0x20) ||
            ((R8(G(gSongFlags)) & 0x10) && ((int8_t)R8(G(nFlags5)) < 0 || (uint16_t)d0 == RU16(G(gSingleInst)))))
            W16(a1 + vInterp, R16(G(gInterpMode)));
    }
    d0 = setw(d0, R16(G(nInst)));
    W16(a1 + vInst, d0);
    W16(a1 + vKey, R16(G(nKey)));
    d0 = setw(d0, RU16(G(gSongFlags)) & 0x3f);
    W16(a1 + vRelease, d0);
    if (R8(G(nFlags)) & 0x40) {
        d0 = 0x20000;                                /* fixed pitch: twice the rate */
    } else {
        d0 = setw(d0, R16(G(nNote)) + 0x3c - R16(a0 + 0x14));
        a2 = G(gPitchTable);
        d0 = setw(d0, d0 << 2);
        d0 = R32(a2 + sxw(d0));
        if ((uint16_t)d0 < 4) d0 = setw(d0, 0);
    }
    W32(a1 + vStep, d0);
    if ((R8(G(nFlags)) & 0x02) && (R8(G(gFlags2)) & 0x10)) {   /* random detune */
        d7 = setw(d7, random_word());
        uint32_t t = d0 >> 10;
        d7 = (uint32_t)((int32_t)(int16_t)d7 * (int32_t)(int16_t)t);
        d7 = swap32(d7);
        d7 = sxw(d7);
        W32(a1 + vStep, R32(a1 + vStep) + d7);
    }
    W32(a1 + vBend, d5);
    d0 >>= 10;
    W32(a1 + vBendStep, d0);
    W32(a1 + vAdvance, 0);
    W32(a1 + vDoneProc, 0);
    a2 = R32(G(gVolLevels)) + 0x7f00;
    d0 = setw(d0, R16(G(nVolume)));
    if ((uint16_t)d0 != 0) {
        if ((uint16_t)d0 >= 0x7d) {
            W16(G(nVolume), 0);
        } else {
            a2 = R32(G(gVolLevels));
            d0 = RU16(G(nVolume));
            d0 = setw(d0, d0 << 8);
            a2 += d0;
        }
    }
    W32(a1 + vTable, a2);
    W16(a1 + vLevel, R16(G(nVolume)));
    W32(a1 + vFrac, 0);
    d0 = R32(a0 + 4) - 1;
    W32(a1 + vEnd, R32(a1 + vEnd) + d0);
    d0 = R32(a0 + 0xc);
    if (d0 != 0 && !(R8(G(nFlags5)) & 0x20)) {       /* loop */
        W32(a1 + vLoopStart, R32(a1 + vLoopStart) + d0);
        d0 = R32(a0 + 0x10);
        if (d0 != 0) {
            W32(a1 + vLoopEnd, R32(a1 + vLoopEnd) + d0);
            d0 = R32(a0 + 0x10) - R32(a0 + 0xc);
            if ((uint16_t)d0 >= 100) goto l3760;
        }
    }
    W32(a1 + vLoopStart, 0);
    W32(a1 + vLoopEnd, 0);
l3760:
    W16(a1 + vTimer, R16(G(nDuration)));
}

/* rate (Fixed) -> 16.16 step for an 11127 Hz mix (shared by 03816 and 038d8) */
static uint32_t rate_to_step(uint32_t rate, uint32_t msg, maddr *a0) {
    uint32_t d0 = rate >> 5;
    if (divu_w(&d0, 0x2b77)) {
        *a0 = drv_error(0xffed, msg, d0);
        d0 = 0x1000;
    }
    d0 &= 0xffff;
    d0 <<= 5;
    if ((uint16_t)d0 == 0x20) d0 = setw(d0, 0);
    if ((uint16_t)d0 == 0xffe0) d0 += 0x20;
    return d0;
}

/* 0376a: selector 0x11: play a sound effect {ptr, length, rate, loop start, loop end,
 * loop count ptr, id (+0x18), proc (+0x1e), refCon (+0x22)} on a free (or the oldest)
 * sample voice */
static void play_sample(maddr a0) {
    SUB(0);
    if (R32(a0) == 0) return;
    maddr a2 = a0, a1 = G(gVoices) + sxw((uint16_t)(R16(G(gNoteVoices)) << 2)), a3;
    uint32_t d0 = RU16(G(gSampleVoices)), d1;
    if (d0 == 0) return;
    a3 = a1;
    d1 = 0x7fff;
    do {
        if (R16(a1 + vTimer) < 0) goto l37a6;
        if ((uint16_t)d1 >= RU16(a1 + vTimer)) { a3 = a1; d1 = RU16(a1 + vTimer); }
        a1 += 4;
        d0 = (uint16_t)(d0 - 1);
    } while (d0 != 0);
    a1 = a3;
l37a6:
    W16(a1 + vTimer, 0xffff);
    call_done_proc(a1);
    d0 = R32(a2);
    W32(a1 + vPtr, d0);
    W32(a1 + vEnd, d0);
    W32(a1 + vEnd, R32(a1 + vEnd) + R32(a2 + 4));
    W32(a1 + vLoopStart, 0);
    W32(a1 + vLoopEnd, 0);
    W16(a1 + vRelease, 0x7ffe);
    W32(a1 + vRamp, 0);
    W32(a1 + vFrac, 0);
    W32(a1 + vAdvance, 0);
    W32(a1 + vLevel, 0);
    W32(a1 + vBend, G(gNoBend));
    W32(a1 + vLoopCnt, R32(a2 + 0x14));
    d0 = RU16(G(gInterpMode)) & 1;
    W16(a1 + vInterp, d0);
    if (R32(a2 + 0x10) != 0) {
        d0 = R32(a2 + 0xc);
        if (d0 != 0) {
            d0 += R32(a2);
            W32(a1 + vLoopStart, d0);
            d0 = R32(a2 + 0x10) + R32(a2);
            W32(a1 + vLoopEnd, d0);
        }
    }
    maddr ignored;
    d0 = rate_to_step(R32(a2 + 8), sRateConv1, &ignored);
    W32(a1 + vStep, d0);
    W16(a1 + vChannel, R16(a2 + 0x18));
    W32(a1 + vDouble, 0x800080);
    W32(a1 + vDoneProc, R32(a2 + 0x1e));
    W32(a1 + vRefCon, R32(a2 + 0x22));
    W16(a1 + vTimer, 0x7ffe);
}

/* 03884: selector 0x16: stop every sound effect (the dbra runs once more than there are
 * sample voices, like the original) */
static void stop_all_samples(void) {
    SUB(0);
    maddr a1 = G(gVoices) + sxw((uint16_t)(R16(G(gNoteVoices)) << 2));
    uint32_t d0 = RU16(G(gSampleVoices));
    if (d0 == 0) return;
    do {
        if (R16(a1 + vTimer) >= 0) call_done_proc(a1);
        W16(a1 + vTimer, 0xffff);
        W32(a1 + vDouble, 0);
        a1 += 4;
        d0 = (uint16_t)(d0 - 1);
    } while (d0 != 0xffff);
}

/* 038b4: selector 0x17: new rate for the sound effect with id (+0x18) */
static void change_sample_rate(maddr a0) {
    SUB(0);
    uint32_t d2 = RU16(a0 + 0x18);
    maddr a1 = G(gVoices) + sxw((uint16_t)(R16(G(gNoteVoices)) << 2));
    uint32_t d0 = RU16(G(gSampleVoices));
    if (d0 == 0) return;
    do {
        if (R16(a1 + vTimer) >= 0 && (uint16_t)d2 == RU16(a1 + vChannel)) {
            d0 = rate_to_step(R32(a0 + 8), sRateConv2, &a0);
            uint16_t t = RU16(a1 + vTimer);
            W16(a1 + vTimer, 0xffff);
            W32(a1 + vStep, d0);
            W32(a1 + vAdvance, 0);
            W16(a1 + vTimer, t);
            return;
        }
        a1 += 4;
        d0 = (uint16_t)(d0 - 1);
    } while (d0 != 0);
}

/* 0393c: selector 0x12: stop the sound effect with id (+0x18) and call its callback */
static void stop_sample(maddr a0) {
    SUB(0);
    uint32_t d2 = RU16(a0 + 0x18);
    maddr a1 = G(gVoices) + sxw((uint16_t)(R16(G(gNoteVoices)) << 2));
    uint32_t d0 = RU16(G(gSampleVoices));
    if (d0 == 0) return;
    do {
        if (R16(a1 + vTimer) >= 0 && (uint16_t)d2 == RU16(a1 + vChannel)) {
            W16(a1 + vRamp, 1);
            W16(a1 + vTimer, 0xffff);
            W16(a1 + vChannel, 0xffff);
            W32(a1 + vDouble, 0);
            W32(a1 + vDoneProc, R32(a0 + 0x1e));
            W32(a1 + vRefCon, R32(a0 + 0x22));
            call_done_proc(a1);
            return;
        }
        a1 += 4;
        d0 = (uint16_t)(d0 - 1);
    } while (d0 != 0);
}

/* 03994: selector 0x14: 1 when the sound effect with id (+0x18) is not playing */
static uint32_t sample_done(maddr a0) {
    SUB(0);
    uint32_t d2 = RU16(a0 + 0x18);
    maddr a1 = G(gVoices) + sxw((uint16_t)(R16(G(gNoteVoices)) << 2));
    uint32_t d0 = RU16(G(gSampleVoices));
    if (d0 == 0) return 1;
    do {
        if ((uint16_t)d2 == RU16(a1 + vChannel)) return R16(a1 + vTimer) < 0 ? 1 : 0;
        a1 += 4;
        d0 = (uint16_t)(d0 - 1);
    } while (d0 != 0);
    return 1;
}

/* ========================================================== PAS16 sound card === */
/* The card path is reached only when card_present() succeeds, and that routine starts
 * with a branch to its failure exit: none of this runs. Translated for completeness. */

/* 039c8: clear the driver parameter block (51 bytes) */
static void card_clear_pb(void) {
    SUB(8);                                          /* move.l D0/A0,-(A7) */
    for (int i = 0; i <= 0x32; i++) W8(G(gCardPB) + i, 0);
}

/* 039de: system-heap stub "jmp MDRV_CardInterrupt" (36 bytes), then BlockMove onto itself
 * (flushes the instruction cache) */
static uint32_t card_alloc_stub(void) {
    SUB(0);
    if (R32(G(gVBLStub)) == 0) {
        uint32_t d0 = ostrap(T_NewPtrSys, 0x24, cpu_get(R_A0));
        maddr a0 = os_a0;
        if ((uint16_t)d0 != 0) { drv_error(0xfffe, sNoVBLMem2, d0); return 0xffffffff; }
        W32(G(gVBLStub), a0);
        W16(a0, 0x4ef9);
        W32(a0 + 2, CODE(0x3cb8));
        _BlockMove(a0, a0, 0x20);
    }
    return 0;
}

/* 03a24: is the card there? Starts with "bra $3a58" (D0 = -1). The unreachable rest
 * (03a26..03a56) would open ".BD_PAS16" and test its refNum. */
static uint32_t card_present(void) {
    SUB(0);
    return 0xffffffff;
}

/* 03a5c: silence the card buffer and the output block */
static void card_silence(void) {
    SUB(0);
    maddr a0 = R32(G(gCardBuf));
    if (a0 != 0) for (int i = 0; i <= 0x3fff; i++) W8(a0++, 0x80);
    a0 = G(gCardOut);
    for (int i = 0; i <= 0x171; i++) W8(a0++, 0x80);
}

/* 03a84: open the card driver and install its VBL task */
static uint32_t card_open(void) {
    SUB(0);
    uint32_t d0 = ostrap(T_StripAddress, 0xffffffff, cpu_get(R_A0));
    W32(G(gCardMask), d0);
    d0 = ostrap(T_NewPtr, 0x4000, cpu_get(R_A0));
    maddr a0 = os_a0;
    if ((uint16_t)d0 != 0) return d0;
    W32(G(gCardBuf), a0);
    card_silence();
    card_clear_pb();
    W16(G(gCardRef), 0);
    a0 = G(gCardPB);
    W32(a0 + 0xc, 0);
    W32(a0 + 0x12, G(gCardPBName));
    d0 = ostrap(0xA000, d0, a0);                     /* _Open */
    a0 = os_a0;
    if ((uint16_t)d0 != 0) return d0;
    W16(G(gCardRef), R16(a0 + 0x18));
    d0 = card_alloc_stub();
    if ((uint16_t)d0 != 0) return d0;
    a0 = G(gCardVBL);
    W32(a0, 0); W16(a0 + 4, 1); W32(a0 + 6, R32(G(gVBLStub))); W16(a0 + 0xc, 0); W16(a0 + 0xa, 1);
    d0 = ostrap(T_VInstall, d0, a0);
    if ((uint16_t)d0 != 0) d0 = 0xffffffff;
    return d0;
}

/* 03b00: remove the card task and free its blocks. Each step is taken or skipped on the
 * condition codes left by the previous trap (Z = its D0.W was 0), as in the original. */
static void card_close(uint32_t z) {
    SUB(0);
    maddr a0 = G(gCardVBL);
    uint32_t d0;
    if (z) { d0 = ostrap(T_VRemove, cpu_get(R_D0), a0); z = (uint16_t)d0 == 0; }
    a0 = R32(G(gCardBuf));
    if (!z) { d0 = ostrap(T_DisposPtr, cpu_get(R_D0), a0); z = (uint16_t)d0 == 0; }
    a0 = R32(G(gVBLStub));
    if (!z) ostrap(T_DisposPtr, cpu_get(R_D0), a0);
    W16(G(gCardRef), 0);
}

/* 03b26: Status 0x66: the card's buffer address */
static uint32_t card_get_base(void) {
    SUB(0);
    W32(G(gCardDMA), 0);
    W32(G(gCardBase), 0);
    card_clear_pb();
    maddr a0 = G(gCardPB);
    W16(a0 + 0x18, R16(G(gCardRef)));
    W16(a0 + 0x1a, 0x66);
    uint32_t d0 = ostrap(0xA005, cpu_get(R_D0), a0);  /* _Status */
    a0 = os_a0;
    if ((uint16_t)d0 == 0) {
        a0 = R32(a0 + 0x1c);
        W32(G(gCardBase), a0);
        W32(G(gCardDMA), a0 + 0x80000);
    }
    return d0;
}

/* 03b5c: current playback position of the card */
static uint32_t card_position(void) {
    SUB(0);
    uint32_t d0 = 0, d1 = R32(G(gCardBase));
    if (d1 != 0) {
        d1 += 0x40000;
        d0 = R32(d1);
        d0 = swap32(d0);
        d0 &= 0x7ffc;
    }
    return d0;
}

/* 03b7a: open and start the card (Control 0x7d9 with MDRV_CardCompletion, then 0x7da) */
static void card_start(void) {
    SUB(0);
    if (R16(G(gCardOpen)) != 0) return;
    W8(G(gCardOpen), 0xff);
    W8(G(gCardBusy), 0xff);
    W8(G(gCardFirst), 0xff);
    uint32_t d0 = card_open();
    if ((uint16_t)d0 != 0) return;
    card_clear_pb();
    W16(G(gCardIOBusy), 0);
    maddr a0 = G(gCardPB);
    W32(a0 + 0xc, 0);
    W16(a0 + 0x18, R16(G(gCardRef)));
    W16(a0 + 0x1a, 0x7d9);
    W32(a0 + 0x1c, 0x56ee8ba3);
    W16(a0 + 0x20, 2);
    W16(a0 + 0x22, 8);
    W16(a0 + 0x24, 1);
    W32(a0 + 0x26, CODE(0x3c78));
    W32(a0 + 0x2a, 0);
    W32(a0 + 0x2e, 0);
    d0 = ostrap(0xA004, d0, a0);                     /* _Control */
    if ((uint16_t)d0 != 0) return;
    d0 = card_get_base();
    if ((uint16_t)d0 != 0) return;
    card_clear_pb();
    a0 = G(gCardPB);
    W32(a0 + 0xc, 0);
    W16(a0 + 0x18, R16(G(gCardRef)));
    W16(a0 + 0x1a, 0x7da);
    ostrap(0xA004, d0, a0);
    W8(G(gCardIOBusy), 0xff);
    W16(G(gCardBusy), 0);
}

/* 03c22: stop the card if it runs; D0 = 1 when it was not open */
static uint32_t card_stop(void) {
    SUB(0);
    if (R16(G(gCardOpen)) == 0) return 1;
    W16(G(gCardOpen), 0);
    card_clear_pb();
    maddr a0 = G(gCardPB);
    W32(a0 + 0xc, 0);
    W16(a0 + 0x18, R16(G(gCardRef)));
    W16(a0 + 0x1a, 0x7d3);
    ostrap(0xA004, cpu_get(R_D0), a0);
    W16(G(gCardIOBusy), 0);
    card_clear_pb();
    a0 = G(gCardPB);
    W32(a0 + 0xc, 0);
    W16(a0 + 0x18, R16(G(gCardRef)));
    W16(a0 + 0x1a, 0x7db);
    uint32_t d0 = ostrap(0xA004, cpu_get(R_D0), a0);
    card_close((uint16_t)d0 == 0);
    return 0;
}

/* 03c78: completion routine handed to the card driver: pascal (4 longs); gives the card
 * the next buffer */
uint32_t MDRV_CardCompletion(uint32_t p8, uint32_t pc, uint32_t p10, uint32_t p14) {
    set_base();
    ENTRY_LINK(FN_MDRV_CardCompletion, 0, 4);        /* link A6,#0; movem.l A4 */
    if (R16(G(gCardIOBusy)) != 0) {
        W16(G(gCardIOBusy), 0);
        W32(R32(a6 + 0x14), 0x4000);
        W32(R32(a6 + 0x10), R32(G(gCardBuf)));
        W16(G(gCardIOBusy), 1);
    }
    return cpu_get(R_D0);
}

/* 03cb8: the card's interrupt task (through the "jmp" stub): mix into the card's
 * circular DMA buffer while it has room */
uint32_t MDRV_CardInterrupt(void) {
    set_base();
    ENTRY_NOLINK(FN_MDRV_CardInterrupt, 4);          /* move.l A4,-(A7) */
    W16(G(gCardVBL) + 0xa, 1);
    if (R16(G(gCardBusy)) != 0) return 0;
    W8(G(gCardBusy), 0xff);
    emu_set_sp(emu_sp() - 40);                       /* movem.l D2-D7/A2-A3/A5-A6 */
    maddr saved_a4 = a4_;
    a4_ = a4_ & R32(G(gCardMask));                   /* 32-bit clean A4 (and PC) */
    uint32_t d0 = ostrap(T_SwapMMUMode, 1, cpu_get(R_A0)), d1;
    emu_set_sp(emu_sp() - 4);                        /* move.l D0,-(A7) */
    uint32_t mode = d0;
    if (R16(G(gCardFirst)) != 0) {
        W16(G(gCardFirst), 0);
        d0 = card_position();
        maddr a0 = R32(G(gCardDMA));
        d1 = d0;
        for (int i = 0; i <= 0x5c7; i++) {
            W8(a0 + sxw(d1), 0x80);
            d1 = setw(d1, d1 + 1);
            d1 = setw(d1, d1 & 0x7fff);
        }
        W32(G(gCardPos), d1);
    } else {
        d0 = card_position();
        d1 = R32(G(gCardPos)) - d0;
        if ((int32_t)d1 < 0 || (int32_t)d1 <= 0x5c8) {
            mix_buffer();
            sequencer_tick();
            if ((int8_t)R8(G(gSongFlags)) < 0) {
                if (R16(G(gErrPending)) == 0) {
                    d0 = R32(0x16a) - R32(G(gLastTicks));
                    if (d0 != 1 && d0 < 5) {
                        drv_error(0xfffd, sOverload2, d0);
                        W8(G(gErrPending), 0xff);
                    }
                }
                W32(G(gLastTicks), R32(0x16a));
            }
            maddr a0 = G(gMixBuf), a2 = R32(G(gVolTable)), a1 = G(gCardOut);
            if (R16(G(gFullRate)) == 0) out_half(a0, a1, a2);
            else out_full(a0, a1, a2);
            d1 = R32(G(gCardPos));
            a0 = R32(G(gCardDMA));
            a1 = G(gCardOut);
            for (int i = 0; i <= 0x171; i++) {
                W8(a0 + sxw(d1), R8(a1++));
                d1 = setw(d1, d1 + 1);
                d1 = setw(d1, d1 & 0x7fff);
            }
            W32(G(gCardPos), d1);
        }
    }
    W32(G(gTickCount), R32(G(gTickCount)) + 1);      /* 03dca */
    emu_set_sp(emu_sp() + 4);
    ostrap(T_SwapMMUMode, mode, cpu_get(R_A0));
    W16(G(gCardBusy), 0);
    a4_ = saved_a4;
    return 0;
}
void glue_MDRV_CardInterrupt(void) {                 /* D2-D7/A2-A6 preserved */
    uint32_t r[16];
    regs_save(r);
    MDRV_CardInterrupt();
    regs_restore(r, 0x7ffcu & ~(RM_A(0) | RM_A(1)));
}

/* 03dde, 03e0e: not referenced: a copy of out_half, and a copy of 370 words inverted */
static void __attribute__((unused)) unused_3dde(maddr a0, maddr a1, maddr a2) { out_half(a0, a1, a2); }
static void __attribute__((unused)) unused_3e0e(maddr a0, maddr a1) {
    SUB(0);
    for (int i = 0; i <= 0x171; i++) { W16(a1, RU16(a0) ^ 0xffff); a0 += 2; a1 += 2; }
}

/* ===================================================================== songs === */

/* 03e22: play the whole song silently once: marks the instruments it uses (gScanMode)
 * and measures its length */
static void scan_song_instruments(void) {
    SUB(0);
    for (int i = 0; i <= 0x7f; i++) W8(G(gInstUsed) + i, 0xff);
    W16(G(gScanMode), 1);
    stop_all_notes();
    start_song();
    uint16_t loop = RU16(G(gSongLoop));
    emu_set_sp(emu_sp() - 2);
    W16(G(gSongLoop), 0);
    W8(G(gSongActive), 0xff);
    do sequencer_tick(); while (song_is_playing() != 0);
    W32(G(gSongLength), R32(G(gSongTime)));
    W16(G(gSongLoop), loop);
    emu_set_sp(emu_sp() + 2);
    W16(G(gScanMode), 0);
    W16(G(gSongActive), 0);
}

/* 03e68 / 03e6e: load song D0 ('SONG' resource and its MIDI resource), or from memory
 * (D1 = SONG handle, D2 = MIDI handle), its SMODs and instruments; start it when
 * gStartSong is set. 1 = error. */
static int begin_song(uint32_t d0, uint32_t d1, uint32_t d2, int from_memory) {
    SUB(0);
    uint32_t d6 = 0, d7 = 0;
    maddr a0, a3;
    W16(G(gFromMemory), from_memory ? 0xffff : 0);
    W16(G(gSongResId), d0);
    uint16_t fm = RU16(G(gFromMemory));
    emu_set_sp(emu_sp() - 10);                       /* move.w (31d6),-(A7); move.l D1/D2,-(A7) */
    W16(G(gFromMemory), 0);
    free_song();
    emu_set_sp(emu_sp() + 10);
    W16(G(gFromMemory), fm);
    W16(G(gSongFlags), 0);
    if (R16(G(gFromMemory)) != 0) {
        a0 = R32(d1);
        W16(a0, R16(G(gSongResId)));
        d0 = d1;
    } else {
        d7 = RU16(G(gSongResId));
        d0 = get_resource(0x534f4e47, d7);           /* 'SONG' */
        if (d0 == 0) { drv_error(0xfffc, sLoadSong, d0); return 1; }
    }
    a0 = d0;
    d0 = lock_detached(a0);
    a0 = d0;
    W32(G(gSongPtr), d0);
    if (R16(G(gFromMemory)) != 0) {
        d0 = d2;
    } else {
        d6 = RU16(a0);
        d0 = load_midi_resource(d6);
        if (d0 == 0) { drv_error(0xffec, sNoMidi, d6); return 1; }
    }
    a0 = d0;
    d0 = lock_handle(a0);
    W32(G(gMidiPtr), d0);
    scan_song_instruments();
    d7 = 0;
    a3 = G(gSMODs);
    if (R32(a3) == 0) {
        do {
            d0 = get_resource(0x534d4f44, d7);       /* 'SMOD' */
            if (d0 == 0) break;
            d0 = lock_detached(d0);
            W32(a3, d0); a3 += 4;
            d7 = setw(d7, d7 + 1);
        } while ((uint16_t)d7 != 0x10);
    }
    for (d7 = 0; (uint16_t)d7 < 0x80; d7 = setw(d7, d7 + 1))
        if (load_instrument(d7)) return 1;
    W8(G(gSongActive), 0xff);
    W8(G(gInstLoaded), 0xff);
    stop_all_notes();
    if (R16(G(gStartSong)) != 0) start_song();
    if (open_output()) return 1;
    return 0;
}

/* 03f8e: load every instrument (live playback) and open the output. (03fc0, an error
 * report "Error loading full instrument set.", is never branched to.) */
static int prepare_instruments(void) {
    SUB(0);
    for (int i = 0; i <= 0x7f; i++) W8(G(gInstUsed) + i, 1);
    for (uint32_t d7 = 0; d7 < 0x80; d7++)
        if (load_instrument(d7)) return 1;
    W8(G(gInstLoaded), 0xff);
    if (open_output()) return 1;
    return 0;
}

/* 03fdc: selector 0x24: unload instrument D7 and its sounds */
static void unload_instrument(uint32_t d7) {
    SUB(0);
    maddr a3 = G(gInstTable) + sxw((uint16_t)(d7 << 3));
    if (R32(a3) == 0) return;
    maddr a6 = R32(a3);
    free_snd(RU16(a6));
    a6 += 0xc;
    uint32_t d5 = RU16(a6);
    while (d5 != 0) {
        uint32_t d6 = RU16(a6 + 4);
        if (d6 != 0) free_snd(d6);
        a6 += 8;
        d5 = (uint16_t)(d5 - 1);
    }
    maddr a0 = R32(a3);
    W32(a3, 0);
    dispose_locked_ptr(a0);
}

/* 04018: selector 0x22: load instrument D7 ('INST') and its sounds, unless the song does
 * not use it (gInstUsed < 0). 1 = error. */
static int load_instrument(uint32_t d7) {
    SUB(0);
    maddr a3 = G(gInstTable) + sxw((uint16_t)(d7 << 3)), a5, a6, a0;
    uint32_t d0 = 0, d1, d2, d5, d6;
    d7 = setw(d7, (uint16_t)(d7 << 3) >> 3);
    W32(a3, 0);
    W32(a3 + 4, 0);
    a5 = G(gInstUsed) + sxw(d7);
    if ((int8_t)R8(a5) < 0) return 0;
    d0 = get_resource(0x494e5354, d7);               /* 'INST' */
    if (d0 == 0) {
        a5 = G(gInstUsed) + sxw(d7);
        if (R8(a5) != 0) return 0;
        drv_error(0xfffa, sNoINST, d7);
        return 1;
    }
    a0 = d0;
    d0 = lock_detached(a0);
    a0 = d0;
    W32(a3, d0); a3 += 4;
    a6 = a0;
    W16(G(iFlags), R16(a6 + 6));
    W8(G(iSMOD), R8(a6 + 7));
    W16(G(iFrom), R16(a6 + 8));
    W16(G(iTo), R16(a6 + 0xa));
    d6 = RU16(a6);
    if (R8(G(iFlags)) & 0x10) {
        d1 = RU16(G(iFrom));
        d2 = RU16(G(iTo));
        if (d1 != d2) {                              /* converted by an SMOD */
            if (load_snd_copy(d6, &d0, &a0)) return 1;
            d1 = RU16(G(iFrom));
            d2 = RU16(G(iTo));
            if (d1 != 0 || d2 != 0) {
                d0 = R8(G(iSMOD));
                a0 = call_smod(d0, d1, d2, a0, d7, a6, a3, 0, d6);
            }
            goto l40d4;
        }
    }
    if (load_snd(d6, &d0, &a0)) return 1;
l40d4:
    W32(a3, a0); a3 += 4;
    if (R8(a6 + 6) & 0x08) return 0;
    a6 += 0xc;
    d5 = RU16(a6); a6 += 2;
    if (d5 == 0) return 0;
    do {                                             /* key splits */
        d6 = RU16(a6 + 2);
        if (d6 == 0) return 1;
        if (R32(a6 + 4) != 0 && (R8(G(iFlags)) & 0x10) && RU16(a6 + 6) != 0) {
            d1 = RU16(a6 + 4);
            d2 = RU16(a6 + 6);
            if (load_snd_copy(d6, &d0, &a0)) return 1;
            d1 = RU16(a6 + 4);
            d2 = RU16(a6 + 6);
            d0 = R8(G(iSMOD));
            d0 = call_smod(d0, d1, d2, a0, d7, a6, a3, d5, d6);
        } else {
            if (load_snd(d6, &d0, &a0)) return 1;
        }
        W32(a6 + 4, d0);
        a6 += 8;
        d5 = (uint16_t)(d5 - 1);
    } while (d5 != 0);
    return 0;
}

/* 04140: convert a sound with SMOD D0 (from format D1 to D2): jsr to the SMOD code with
 * A0 = the sound's handle. Returns the sound pointer (or, when the SMOD is missing, the
 * address of the error message that the original leaves in A0). */
static uint32_t call_smod(uint32_t d0, uint32_t d1, uint32_t d2, maddr a0, uint32_t d7, maddr a6,
                          maddr a3, uint32_t d5, uint32_t d6) {
    SUB(0);
    if ((uint16_t)d1 == (uint16_t)d2) return a0;
    maddr a1 = G(gSMODs);
    if ((uint16_t)d0 >= 0x10) goto missing;
    d0 = setw(d0, d0 << 2);
    a1 += sxw(d0);
    if (R32(a1) == 0) goto missing;
    {
        emu_set_sp(emu_sp() - 4 - 8);                /* move.l A0,-(A7); movem.l D1/A1,-(A7) */
        d0 = ostrap(T_RecoverHandle, d0, a0);
        maddr h = os_a0;
        emu_set_sp(emu_sp() + 8);
        a1 = R32(a1);
        uint32_t r[16];
        regs_save(r);
        cpu_set(R_D0, d0); cpu_set(R_D1, d1); cpu_set(R_D2, d2); cpu_set(R_D5, d5); cpu_set(R_D6, d6);
        cpu_set(R_D7, d7); cpu_set(R_A0, h); cpu_set(R_A1, a1); cpu_set(R_A3, a3); cpu_set(R_A4, a4_);
        cpu_set(R_A5, G(gInstUsed) + sxw(d7)); cpu_set(R_A6, a6);
        call68_addr(a1);                             /* jsr (A1) */
        regs_restore(r, 0x7fffu);
        emu_set_sp(emu_sp() + 4);                    /* movea.l (A7)+,A0 */
        return a0;
    }
missing:
    return drv_error(0xfff8, sNoSMOD, d0);
}

/* 0418a: unload 'snd ' D6 */
static void free_snd(uint32_t d6) {
    SUB(60);
    maddr a2 = G(gSndIds), a3 = G(gSndPtrs);
    for (;;) {
        if ((uint16_t)d6 == RU16(a2)) break;
        if (R16(a2) == -1) return;
        a2 += 2;
        a3 += 4;
    }
    W16(a2, 0);
    maddr a0 = R32(a3);
    W32(a3, 0);
    if (a0 != 0) dispose_locked_ptr(a0);
}

/* 041c2: load 'snd ' D6 once (shared by id); D0 = A0 = its locked data. 1 = error. */
static int load_snd(uint32_t d6, uint32_t *d0o, maddr *a0o) {
    SUB(48);                                         /* movem.l D1-D6/A1-A6 */
    if ((uint16_t)d6 == 0) return 1;
    maddr a2 = G(gSndIds), a3 = G(gSndPtrs);
    for (;;) {
        if ((uint16_t)d6 == RU16(a2)) { *d0o = R32(a3); *a0o = *d0o; return 0; }
        if (R16(a2) == -1) { W16(a2 + 2, 0xffff); break; }
        if (R16(a2) == 0) break;
        a3 += 4;
        a2 += 2;
    }
    uint32_t d0 = load_snd_resource(d6);
    if (d0 == 0) {
        *a0o = drv_error(0xfff9, sNoSnd1, d6);
        *d0o = setw(d0, d6);
        return 1;
    }
    W16(a2, d6);
    d0 = lock_handle(d0);
    W32(a3, d0); a3 += 4;
    W32(a3, 0);
    *d0o = d0;
    *a0o = d0;
    return 0;
}

/* 0423a: load a private copy of 'snd ' D6 (to be converted), id -2 in the table */
static int load_snd_copy(uint32_t d6, uint32_t *d0o, maddr *a0o) {
    SUB(52);                                         /* movem.l D1-D7/A1-A6 */
    if ((uint16_t)d6 == 0) return 1;
    maddr a2 = G(gSndIds), a3 = G(gSndPtrs);
    for (;;) {
        if (R16(a2) == -1) { W16(a2 + 2, 0xffff); break; }
        if (R16(a2) == 0) break;
        a2 += 2;
        a3 += 4;
    }
    uint32_t d0 = load_snd_resource(d6);
    if (d0 == 0) {
        *a0o = drv_error(0xfff9, sNoSnd2, d6);
        *d0o = setw(d0, d6);
        return 1;
    }
    W16(a2, 0xfffe);
    d0 = lock_handle(d0);
    W32(a3, d0); a3 += 4;
    W32(a3, 0);
    *d0o = d0;
    *a0o = d0;
    return 0;
}

/* 042a8: detach, lock high; D0 = A0 = the data (0 for no handle) */
static uint32_t lock_detached(maddr a0) {
    SUB(0);
    if (a0 == 0) return 0;
    detach_resource2(a0);
    ostrap(T_HNoPurge, cpu_get(R_D0), a0);
    ostrap(T_MoveHHi, cpu_get(R_D0), os_a0);
    ostrap(T_HLock, cpu_get(R_D0), os_a0);
    return R32(os_a0);
}

/* 042c0: lock high; D0 = A0 = the data */
static uint32_t lock_handle(maddr a0) {
    SUB(0);
    if (a0 == 0) return 0;
    ostrap(T_HNoPurge, cpu_get(R_D0), a0);
    ostrap(T_MoveHHi, cpu_get(R_D0), os_a0);
    ostrap(T_HLock, cpu_get(R_D0), os_a0);
    return R32(os_a0);
}

/* 042d0: selector 7: stop the song, free it, its instruments and sounds */
static int free_song(void) {
    SUB(0);
    W16(G(gSongActive), 0);
    free_all_voices();
    W16(G(gPaused), 0);
    for (int k = 0; k < 2; k++) {
        maddr slot = G(k == 0 ? gSongPtr : gMidiPtr);
        if (R32(slot) != 0) {
            ostrap(T_RecoverHandle, cpu_get(R_D0), R32(slot));
            ostrap(T_HUnlock, cpu_get(R_D0), os_a0);
            if (R16(G(gFromMemory)) == 0) ostrap(T_DisposHandle, cpu_get(R_D0), os_a0);
            W32(slot, 0);
        }
    }
    maddr a3 = G(gInstTable);
    for (int i = 0; i <= 0x7f; i++) {
        if (R32(a3) != 0) dispose_locked_ptr(R32(a3));
        W32(a3, 0); a3 += 4;
        W32(a3, 0); a3 += 4;
    }
    W16(G(gInstLoaded), 0);
    a3 = G(gSndPtrs);
    while (R32(a3) != 0) {
        maddr a0 = R32(a3);
        W32(a3, 0); a3 += 4;
        dispose_locked_ptr(a0);
    }
    W32(G(gSndIds), 0xffffffff);
    return 0;
}

/* 04358: selector 8: close everything and free all memory */
static void shutdown_driver(void) {
    SUB(0);
    close_output();
    W16(G(gPaused), 0);
    free_song();
    maddr a3 = G(gSMODs);
    while (R32(a3) != 0) {
        maddr a0 = R32(a3);
        W32(a3, 0); a3 += 4;
        dispose_locked_ptr(a0);
    }
    dispose_ptr_at(G(gVolTable));
    dispose_ptr_at(G(gVBLStub));
    dispose_ptr_at(G(gVolLevels));
    dispose_ptr_at(G(gInterpBlock));
}

/* 04392: DisposPtr(*A0) and clear it */
static void dispose_ptr_at(maddr a0) {
    SUB(0);
    if (R32(a0) != 0) {
        emu_set_sp(emu_sp() - 4);
        ostrap(T_DisposPtr, cpu_get(R_D0), R32(a0));
        emu_set_sp(emu_sp() + 4);
        W32(a0, 0);
    }
}

/* 043a2: LZSS decoder (C: src, srcLen, dst, dstLen): flag bytes, 1 = literal,
 * 0 = 12-bit offset into the 4K before the output + 4-bit length (+3) */
static void lzss_decode(maddr a2, uint32_t d6, maddr a3, uint32_t d7) {
    SUB(4 + 32);                                     /* link A6; movem.l D2-D7/A2-A3 */
    for (;;) {
        d6 -= 1;
        if ((int32_t)d6 < 0) return;
        uint8_t d3 = R8(a2++);
        for (int d2 = 7; d2 >= 0; d2--) {
            int lit = d3 & 1;
            d3 >>= 1;
            if (lit) {
                d6 -= 1; if ((int32_t)d6 < 0) return;
                d7 -= 1; if ((int32_t)d7 < 0) return;
                W8(a3++, R8(a2++));
            } else {
                d6 -= 2; if ((int32_t)d6 < 0) return;
                uint16_t w = (uint16_t)(R8(a2) << 8); a2++;
                w |= R8(a2); a2++;
                uint32_t d5 = w & 0xfff, d4 = ((w >> 12) & 0xf) + 2;
                maddr a0 = a3 - 0x1000 + d5;
                for (;;) {
                    d7 -= 1; if ((int32_t)d7 < 0) return;
                    W8(a3++, R8(a0++));
                    if (d4-- == 0) break;
                }
            }
        }
    }
}

/* 0440e: running sum (delta decoding) over D0 bytes */
static void delta_decode(maddr a0, uint32_t d0) {
    SUB(4);                                          /* link A6 */
    uint8_t d1 = R8(a0++);
    for (;;) {
        d0 -= 1;
        if (d0 == 0) break;
        d1 = (uint8_t)(d1 + R8(a0));
        W8(a0++, d1);
    }
}

/* 0442a / 0445e: (src, srcLen, dst, &dstLen): decompress, then (0442a) delta-decode */
static void decompress(maddr src, uint32_t srclen, maddr dst, maddr pdstlen, int delta) {
    SUB(4);                                          /* link A6 */
    emu_set_sp(emu_sp() - 16);
    lzss_decode(src, srclen, dst, R32(pdstlen));
    emu_set_sp(emu_sp() + 16);
    if (delta) {
        emu_set_sp(emu_sp() - 8);
        delta_decode(dst, R32(pdstlen));
        emu_set_sp(emu_sp() + 8);
    }
}

/* 04482 (entered by bsr or jmp): resource lPackedType D6 (compressed: decompressed into a
 * new handle) or else lType D6 (detached, or a copy). D0 = the handle (0 = none). */
static uint32_t load_resource_body(uint32_t d6) {
    emu_set_sp(emu_sp() - 56);                       /* movem.l D1-D7/A0-A6 */
    uint32_t d0 = get_resource(R32(G(lPackedType)), d6);
    maddr a0;
    if (d0 == 0) {
        a0 = get_resource(R32(G(lType)), d6);
        if (R16(G(gDetach)) != 0) goto l44ac;
        W32(G(lHandle), a0);
        tb_begin(); cpu_set(R_A0, a0); tb_trap(T_HandToHand); d0 = cpu_get(R_D0); a0 = cpu_get(R_A0); tb_end();
        if ((uint16_t)d0 != 0) goto l44ac;
        emu_set_sp(emu_sp() - 4);
        ostrap(T_HPurge, d0, R32(G(lHandle)));
        d0 = a0;
        emu_set_sp(emu_sp() + 4);
        goto l4556;
    l44ac:
        detach_resource2(a0);
        d0 = a0;
        goto l4556;
    }
    a0 = d0;
    W32(G(lPacked), a0);
    a0 = R32(a0);
    d0 = R32(a0);                                    /* unpacked size */
    W32(G(lSize), d0);
    a0 = R32(G(lPacked));
    d0 = ostrap(T_GetHandleSize, d0, a0);
    W32(G(lPackedSize), d0);
    a0 = R32(G(lPacked));
    ostrap(T_HLock, d0, a0);
    W32(G(lHandle), 0);
    d0 = R32(G(lSize));
    d0 = ostrap(T_NewHandle, d0, os_a0);
    a0 = os_a0;
    if ((uint16_t)d0 != 0) goto l4556;
    W32(G(lHandle), a0);
    ostrap(T_HLock, d0, a0);
    {
        maddr src = R32(R32(G(lPacked))) + 4, dst = R32(R32(G(lHandle)));
        uint32_t len = R32(G(lPackedSize)) - 4;
        emu_set_sp(emu_sp() - 16);                   /* the four pushed arguments */
        decompress(src, len, dst, G(lSize), R16(G(lDelta)) != 0);
        emu_set_sp(emu_sp() + 16);
    }
    a0 = R32(G(lPacked));
    ostrap(T_HUnlock, cpu_get(R_D0), a0);
    ostrap(T_HPurge, cpu_get(R_D0), os_a0);
    if (R16(G(gDetach)) != 0) {
        tb_begin(); push32(R32(G(lPacked))); tb_trap(0xA9A3); tb_end();   /* ReleaseResource */
    }
    a0 = R32(G(lHandle));
    ostrap(T_HUnlock, cpu_get(R_D0), a0);
    d0 = os_a0;
l4556:
    emu_set_sp(emu_sp() - 4);
    ostrap(T_MoveHHi, d0, d0);
    emu_set_sp(emu_sp() + 4);
    emu_set_sp(emu_sp() + 56);
    return d0;
}
static uint32_t load_resource(uint32_t d6) {
    SUB(0);
    return load_resource_body(d6);
}

/* 0456a: 'csnd' (LZSS + delta) or 'snd ' D6 */
static uint32_t load_snd_resource(uint32_t d6) {
    SUB(0);
    W8(G(lDelta), 0xff);
    W32(G(lPackedType), 0x63736e64);                 /* 'csnd' */
    W32(G(lType), 0x736e6420);                       /* 'snd ' */
    return load_resource_body(d6);                   /* jmp */
}

/* 04582: 'cmid' (LZSS) or 'Midi' or 'MIDI' D6 */
static uint32_t load_midi_resource(uint32_t d6) {
    SUB(0);
    W16(G(lDelta), 0);
    W32(G(lPackedType), 0x636d6964);                 /* 'cmid' */
    W32(G(lType), 0x4d696469);                       /* 'Midi' */
    uint32_t d0 = load_resource(d6);
    if (d0 == 0) {
        W32(G(lType), 0x4d494449);                   /* 'MIDI' */
        d0 = load_resource(d6);
    }
    return d0;
}


