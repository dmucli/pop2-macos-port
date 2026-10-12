/* CODE 11 "Mohawk Libs 2", second half (0x40ad0-0x43f42): Mohawk support library.
 *   - LZSS (de)compression of handles and delta-coded samples
 *   - the sampled-sound layer: sound slots ('csnd'/'snd ' resources), voices played through
 *     MusicDriverHandler, the sound VBL task, sound lists, double-buffered file playback
 *   - the config-file reader/writer (configRead/configWrite: "[type]" sections of
 *     "name=value" lines cached in memory handles)
 * Ported from the Ghidra disassembly of the original 68k code; addresses in comments are
 * Ghidra addresses (see docs/PORTING.md). */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* ---- sound globals (Ghidra addresses) ---- */
#define gSoundCount         GH(0x10713e)   /* word: number of sound slots */
#define gSoundsRegistered   GH(0x107140)   /* byte: RegisterSounds has loaded a slot table */
#define gSoundSlots         GH(0x107142)   /* long: pointer to the sound slots (0x26 bytes each) */
#define gBeginDelayRate     GH(0x107146)   /* word: SetBeginSoundDelayRate value */
#define gBeginDelay         GH(0x107148)   /* word: countdown restarted by each LowLevelPlaySample */
#define gSoundOff           GH(0x10714a)   /* byte: sound system not running (1 = off) */
#define gSoundTicks         GH(0x10714c)   /* long: sound VBL tick counter */
#define gSoundInited        GH(0x107150)   /* byte */
#define gSystemVersion      GH(0x107152)   /* word: SysEnvirons systemVersion */
#define gSoundVBCallBack    GH(0x107154)   /* long: procedure called from the sound VBL task */
#define gSoundDoneCallBack  GH(0x107158)   /* long: procedure(id) called when a sample ends */
#define gSoundList          GH(0x10715c)   /* long: sound list being played (8-byte entries) */
#define gSavedSoundList     GH(0x107160)   /* long: sound list saved while the system is off */
#define gSoundListIndex     GH(0x107164)   /* word */
#define gSoundListCount     GH(0x107166)   /* word */
#define gSoundListDelay     GH(0x107168)   /* word: VBLs before the next list entry (<0 = now) */
#define gVBLBlock           GH(0x10716a)   /* long: NewPtrClear(0x12): A5, then the VBL task */
#define gVBLTask            GH(0x10716e)   /* long: gVBLBlock + 4 (VBLTask record) */
#define gVBLZone            GH(0x107172)   /* long: heap zone set while the VBL task runs */
#define gInSoundVB          GH(0x107176)   /* byte: VBL task busy / blocked */
#define gFileResume         GH(0x107177)   /* byte: restart file playback when sound comes back */
#define gFilePlaying        GH(0x107178)   /* byte: double-buffered file playback active */
#define gFileRate           GH(0x10717a)   /* long: file playback rate */
#define gFileBuf            GH(0x10717e)   /* long[2]: the two file buffers */
#define gFileBufLen         GH(0x107186)   /* long[2]: bytes read into each buffer */
#define gFileBufSize        GH(0x10718e)   /* long: buffer size */
#define gFile192            GH(0x107192)   /* byte */
#define gFileSpec           GH(0x107194)   /* 0x46 bytes: vRefNum, dirID, name of the played file */
#define gFileErr            GH(0x1071da)   /* word */
#define gFilePos            GH(0x1071dc)   /* long: file position of the next read */
#define gFileRefNum         GH(0x1071e0)   /* word */
#define gFileWDRefNum       GH(0x1071e2)   /* word */
#define gFileBufIndex       GH(0x1071e4)   /* word: buffer to play next (0/1) */
#define gFileBufPending     GH(0x1071e6)   /* byte: a buffer must be refilled */
#define gFileRestart        GH(0x1071e7)   /* byte: restart countdown running */
#define gFileRestartDelay   GH(0x1071e8)   /* word */
#define gFileVoiceEnded     GH(0x1071ea)   /* byte: a file voice (0x8001/0x8002) has ended */
#define gVoiceCount         GH(0x10721e)   /* word */
#define gVoices             GH(0x107220)   /* long: pointer to the voices (0x7a bytes each) */
#define gLastSampleVoice    GH(0x107224)   /* long: voice returned by PlayTheSample */

/* ---- config globals ---- */
#define gConfigDelims       GH(0x107228)   /* string: strpbrk set used to find an EOF mark */
#define gConfigError        GH(0x10722a)   /* word */
#define gConfigInitCount    GH(0x10722c)   /* word */
#define gConfigClientOff    GH(0x10722e)   /* word: offset of our flag in the os client bytes */
#define gConfigFiles        GH(0x107230)   /* word: memory handle of the first cached file */

#define SLOT_SIZE  0x26
#define VOICE_SIZE 0x7a
#define FILE_VOICE_A ((int16_t)0x8001)
#define FILE_VOICE_B ((int16_t)0x8002)

/* A Boolean/char argument pushed with move.b lives in the high byte of its word. */
#define B_HI(w) ((uint8_t)((uint16_t)(w) >> 8))

/* The original leaves n bytes of an earlier call's arguments on the stack around this
 * call: run it n bytes deeper so the callee sees the same SP. */
#define DEEPER(n, call) __extension__ ({ maddr dp_ = emu_sp(); emu_set_sp(dp_ - (n)); \
    uint32_t dr_ = (call); emu_set_sp(dp_); dr_; })

/* Pascal function(word): Boolean through a pointer ("clr.b -(SP); move.w id,-(SP); jsr"). */
static uint8_t pascal_bool_w(maddr fn, int16_t w) {
    maddr sp0 = emu_sp();
    W8(sp0 - 2, 0);
    W16(sp0 - 4, w);
    emu_set_sp(sp0 - 4);
    call68_addr(fn);
    uint8_t r = R8(sp0 - 2);
    emu_set_sp(sp0);
    return r;
}

/* ======================================================================= LZSS == */

/* 00040ad0 */
uint32_t LZSSCompress(uint32_t src, uint32_t len, uint32_t dst, uint32_t outLen) {
    ENTRY_LINK(FN_LZSSCompress, 0, 0);
    uint32_t r = doLZSSEncode(src, len, dst);
    W32(outLen, r);
    return r;
}

/* 00040afe */
uint32_t LZSSUncompress(uint32_t src, uint32_t len, uint32_t dst, uint32_t dstLen) {
    ENTRY_LINK(FN_LZSSUncompress, 0, 0);
    return doLZSSDecode(src, len, dst, R32(dstLen));
}

/* 00040b2e: delta-code the samples, compress, undo the delta coding */
uint32_t LZSSDeltaCompress(uint32_t src, uint32_t len, uint32_t dst, uint32_t outLen) {
    ENTRY_LINK(FN_LZSSDeltaCompress, 0, 0);
    DeltaSample(src, len);
    W32(outLen, doLZSSEncode(src, len, dst));
    return UnDeltaSample(src, len);
}

/* 00040b78 */
uint32_t LZSSDeltaUncompress(uint32_t src, uint32_t len, uint32_t dst, uint32_t dstLen) {
    ENTRY_LINK(FN_LZSSDeltaUncompress, 0, 0);
    doLZSSDecode(src, len, dst, R32(dstLen));
    return UnDeltaSample(dst, R32(dstLen));
}

/* 00040bba: longest earlier match (window 0x1000, length 3..0x12) for position pos;
 * stores the code word (len-3)<<12 | (0x1000-dist) and returns the length (0 = none) */
uint32_t findLongestMatch(uint32_t buf, uint32_t len, uint32_t pos, uint32_t codeOut) {
    int32_t ipos = (int32_t)pos;
    int32_t win = ipos < 0x1000 ? ipos : 0x1000;          /* D2 */
    int32_t maxl = (int32_t)(len - pos);
    if (maxl >= win) maxl = win;
    if (maxl >= 0x12) maxl = 0x12;                         /* D3 */
    uint16_t d2 = (uint16_t)win, d3 = (uint16_t)maxl, d4 = 0, d5 = 0;
    uint32_t a3 = buf + pos;
    uint32_t a2 = a3 - (uint32_t)win;
    uint8_t first = R8(a3);
    while ((int16_t)d2 > 0) {
        if (R8(a2++) != first) { d2--; continue; }
        uint16_t n = 1;
        uint32_t a1 = a3 + 1;
        while ((int16_t)n < (int16_t)d3) {
            uint8_t x = R8(a2++), y = R8(a1++);
            if (x != y) break;
            n++;
        }
        if ((int16_t)n > (int16_t)d4) { d5 = d2; d4 = n; }
        a2 -= 1;
        d2 = (uint16_t)(d2 - n);
    }
    if ((int16_t)d4 < 3) return 0;
    W16(codeOut, (uint16_t)((uint16_t)(0x1000 - d5) | (uint16_t)((d4 - 3) << 12)));
    return d4;
}

/* 00040c68: LZSS-encode len bytes into dst; returns the output size, or -1 if it is not
 * smaller than the input */
uint32_t doLZSSEncode(uint32_t src, uint32_t len, uint32_t dst) {
    ENTRY_LINK(FN_doLZSSEncode, 0x18, 28);
    int32_t n = (int32_t)len, in = 0, out = 0;
    maddr flags = a6 - 1, code = a6 - 8, mlen = a6 - 6, buf = a6 - 0x18;
    while (n > in) {
        W8(flags, 0);
        int32_t k = 0, bit;
        for (bit = 0; bit < 8; bit++) {
            uint32_t r = findLongestMatch(src, len, (uint32_t)in, code);
            W32(mlen, r);
            if (r != 0) {
                int32_t c = R16(code);
                W8(buf + k, (uint8_t)(c >> 8)); k++;
                W8(buf + k, R8(code + 1)); k++;
                in += (int32_t)R32(mlen);
            } else {
                W8(buf + k, R8(src + (uint32_t)in)); k++;
                W8(flags, R8(flags) | (uint8_t)(1u << bit));
                in++;
            }
            if (n <= in) break;
        }
        if (n > out) W8(dst + (uint32_t)out, R8(flags));
        out++;
        for (int32_t i = 0; k > i; i++) {
            if (n > out) W8(dst + (uint32_t)out, R8(buf + i));
            out++;
        }
    }
    return n > out ? (uint32_t)out : 0xFFFFFFFFu;
}

/* 00040d38: LZSS-decode at most srcLen bytes into at most dstLen bytes */
uint32_t doLZSSDecode(uint32_t src, uint32_t srcLen, uint32_t dst, uint32_t dstLen) {
    int32_t d6 = (int32_t)srcLen, d7 = (int32_t)dstLen;
    for (;;) {
        if (--d6 < 0) return 0;
        uint8_t flags = R8(src++);
        int16_t d2 = 7;
        do {
            int bit = flags & 1;
            flags >>= 1;
            if (bit) {
                if (--d6 < 0) return 0;
                if (--d7 < 0) return 0;
                W8(dst++, R8(src++));
            } else {
                d6 -= 2;
                if (d6 < 0) return 0;
                uint16_t c = (uint16_t)(R8(src) << 8 | R8(src + 1));
                src += 2;
                uint16_t off = c & 0xfff;
                int16_t cnt = (int16_t)((c >> 12) + 2);
                uint32_t from = dst - 0x1000 + (uint32_t)(int32_t)(int16_t)off;
                do {
                    if (--d7 < 0) return 0;
                    W8(dst++, R8(from++));
                } while (--cnt != -1);
            }
        } while (--d2 != -1);
    }
}

/* 00040db4: replace each byte (after the first) by its difference with the previous one */
uint32_t DeltaSample(uint32_t p, uint32_t n) {
    uint8_t prev = R8(p++);
    while (--n != 0) {
        uint8_t cur = R8(p);
        W8(p, (uint8_t)(cur - prev)); p++;
        prev = cur;
    }
    return 0;
}

/* 00040de0: undo DeltaSample */
uint32_t UnDeltaSample(uint32_t p, uint32_t n) {
    uint8_t acc = R8(p++);
    while (--n != 0) {
        acc = (uint8_t)(acc + R8(p));
        W8(p, acc); p++;
    }
    return n;
}

/* 00040e0c: compress a handle (disposes it); the result starts with the original size;
 * NULL if it does not compress */
uint32_t CompressHandle(uint32_t h) {
    ENTRY_LINK(FN_CompressHandle, 8, 12);
    if (h == 0) return 0;
    W32(a6 - 4, tc_GetHandleSize(h));
    uint32_t nh = _NewHandle(R32(a6 - 4) + 4);
    if (nh == 0) return h;
    _HLock(nh);
    _HLock(h);
    LZSSCompress(R32(h), R32(a6 - 4), R32(nh) + 4, a6 - 8);
    _BlockMove(a6 - 4, R32(nh), 4);
    _HUnlock(h);
    _DisposeHandle(h);
    h = 0;
    if ((int32_t)R32(a6 - 8) > 1) {
        W32(a6 - 8, R32(a6 - 8) + 4);
        h = _NewHandle(R32(a6 - 8));
        if (h) {
            _HLock(h);
            _HLock(nh);
            _BlockMove(R32(nh), R32(h), R32(a6 - 8));
            _HUnlock(nh);
            _HUnlock(h);
        }
        _DisposeHandle(nh);
    }
    return h;
}

/* 00040ebe: decompress a CompressHandle handle (disposes it) */
uint32_t DecompressHandle(uint32_t h) {
    ENTRY_LINK(FN_DecompressHandle, 4, 8);
    _HLock(h);
    _BlockMove(R32(h), a6 - 4, 4);
    _HUnlock(h);
    uint32_t nh = _NewHandle(R32(a6 - 4) + 0x100);
    if (nh) {
        _HLock(h);
        _HLock(nh);
        uint32_t dst = R32(nh);
        uint32_t size = tc_GetHandleSize(h);
        LZSSUncompress(R32(h) + 4, size - 4, dst, a6 - 4);
        _HUnlock(nh);
        _HUnlock(h);
        _DisposeHandle(h);
    }
    return nh;
}

/* =============================================================== sound slots == */
/* Sound slot (0x26 bytes): +0 reversed, +1 from 'snd ' (resource kept), +2 locked,
 * +3 saved handle state, +4 handle, +8 sound header, +0xc sample data, +0x10 length,
 * +0x14 loop start, +0x18 loop end, +0x1c id, +0x1e rate.
 * Voice (0x7a bytes): +0 active, +2/+6 loop (in VBLs), +0x12 VBLs left (-1 = loop),
 * +0x16 start tick, +0x1e MusicDriverHandler sample block (data, len, rate, loop start,
 * loop end, &+0x44, id +0x36, A5 +0x38, callback +0x3c, voice +0x40), +0x44 envelope
 * result, +0x4a envelope on, +0x4b envelope called, +0x4c ended, +0x4e envelope proc,
 * +0x52 done callback. */

/* 00040f42: slot of sound id (loading it if needed), or 0 */
uint32_t FindSound(int16_t id) {
    ENTRY_LINK(FN_FindSound, 0, 8);
    uint32_t found = 0;
    if (R32(gSoundSlots) == 0) return 0;
    for (int16_t i = 0; i < R16(gSoundCount); i++) {
        if (R16(R32(gSoundSlots) + (int32_t)i * SLOT_SIZE + 0x1c) != id) continue;
        if (R32(R32(gSoundSlots) + (int32_t)i * SLOT_SIZE + 4) != 0) {
            found = (int32_t)i * SLOT_SIZE + R32(gSoundSlots);
            break;
        }
        if ((uint8_t)LoadSampleIntoSlot(id, (int32_t)i * SLOT_SIZE + R32(gSoundSlots), 0)) {
            found = (int32_t)i * SLOT_SIZE + R32(gSoundSlots);
            break;
        }
    }
    return found;
}

/* 00040fc6: first inactive voice (reset), or 0 */
uint32_t FindFreeVoice(void) {
    uint32_t v = 0, base = R32(gVoices);
    for (int16_t i = 0; i < R16(gVoiceCount); i++) {
        if (R8(base + (int32_t)i * VOICE_SIZE) == 0) { v = (int32_t)i * VOICE_SIZE + base; break; }
    }
    if (v) { W8(v, 0); W32(v + 0x16, 0); }
    return v;
}

/* 0004101a: voice playing sound id, or 0 */
uint32_t FindVoice(int16_t id) {
    for (int16_t i = 0; i < R16(gVoiceCount); i++)
        if (R16(R32(gVoices) + (int32_t)i * VOICE_SIZE + 0x36) == id)
            return (int32_t)i * VOICE_SIZE + R32(gVoices);
    return 0;
}

/* 00041060: clear a voice (0x79 bytes) and mark it ended */
uint32_t ClearVoice(uint32_t v) {
    uint32_t p = v;
    for (int32_t n = 0x79; n != 0; n--) W8(p++, 0);
    W8(v + 0x4c, 1);
    return v;
}

/* 0004109a: stop a voice; stopDriver/callDone are Booleans (high bytes) */
uint32_t EndVoice(uint32_t v, int16_t stopDriver, int16_t callDone) {
    ENTRY_LINK(FN_EndVoice, 0x7a, 8);
    if (v == 0 || R8(v + 0x4c)) return 0;
    W8(v + 0x4c, 1);
    W32(v + 0x12, 0);
    W8(v + 0x4a, 0);
    W8(v, 0);
    uint32_t done = R32(v + 0x52);
    W32(v + 0x52, 0);
    if (B_HI(stopDriver)) {
        W32(v + 0x3c, 0);
        MusicDriverHandler(v + 0x1e, 0x65);
    }
    mcopy(a6 - 0x7a, v, VOICE_SIZE);
    ClearVoice(v);
    if (B_HI(callDone) && done) {
        W32(a6 - 0x28, 0);
        call_ptr(done, "l", a6 - 0x7a);
    }
    return 0;
}

/* 00041122: Pascal done callback of a voice: gSoundDoneCallBack(id) */
uint32_t CallPlaySampleCallback(uint32_t v) {
    ENTRY_LINK(FN_CallPlaySampleCallback, 0, 4);
    if (R32(gSoundDoneCallBack) == 0) return 0;
    uint16_t id = (uint16_t)R16(v + 0x36);
    call_ptr(R32(gSoundDoneCallBack), "w", id);
    return 0;
}

/* 00041166: MusicDriverHandler callback when a voice's sample ends (runs with the A5
 * saved in the voice; D0 returns that A5) */
uint32_t ProcessCallback(uint32_t v) {
    ENTRY_LINK(FN_ProcessCallback, 0, 12);
    uint32_t a5v = R32(v + 0x38);
    int16_t id = R16(v + 0x36);
    if (id == FILE_VOICE_A || id == FILE_VOICE_B) W8(gFileVoiceEnded, 1);
    EndVoice(v, 0, 0x100);
    return a5v;
}

/* 000411ba: start a sample on a free voice; returns the voice or 0 */
uint32_t LowLevelPlaySample(uint32_t data, uint32_t len, uint32_t rate, int16_t id,
                            uint32_t loopStart, uint32_t loopEnd, int16_t envelope,
                            int16_t envOn, uint32_t envProc, uint32_t doneProc) {
    ENTRY_LINK(FN_LowLevelPlaySample, 0, 16);
    uint32_t v = FindFreeVoice();
    if (v == 0 || data == 0 || R8(gSoundOff)) return 0;
    W16(gBeginDelay, R16(gBeginDelayRate));
    if (envelope != -1)
        W32(v + 0x12, (int32_t)(int16_t)CalcPlaybackLength(rate, len));
    else
        W32(v + 0x12, 0xFFFFFFFFu);
    W32(v + 0x16, R32(gSoundTicks));
    W32(v + 2, (int32_t)(int16_t)CalcPlaybackLength(rate, loopStart));
    /* third call: the loopStart argument of the second one is still on the stack */
    W32(v + 6, (uint32_t)((int32_t)(int16_t)DEEPER(4, CalcPlaybackLength(rate, loopEnd))) - loopStart);
    W16(v + 0x44, envelope);
    W32(v + 0x4e, envProc);
    W32(v + 0x52, doneProc);
    W8(v + 0x4a, B_HI(envOn));
    W8(v + 0x4b, 0);
    W8(v + 0x4c, 0);
    W8(v, 1);
    uint32_t b = v + 0x1e;
    W32(b, data + 6);
    W32(b + 4, len);
    W32(b + 8, rate);
    W32(b + 0xc, loopStart);
    W32(b + 0x10, loopEnd);
    W32(b + 0x14, v + 0x44);
    W16(b + 0x18, id);
    W32(b + 0x1a, g_a5);
    W32(b + 0x1e, JT_ADDR(FN_ProcessCallback));
    W32(b + 0x22, v);
    DEEPER(8, MusicDriverHandler(v + 0x1e, 0x64));
    return v;
}

/* 000412cc: length in VBLs (1/60 s) of n samples at a Fixed rate; D0's high word is the
 * sign of the rate's integer part */
uint32_t CalcPlaybackLength(uint32_t rate, uint32_t n) {
    int16_t ip = (int16_t)((int32_t)rate >> 16);
    if (ip == 0) return 0;
    int32_t d = ip;
    int32_t m = (int32_t)(n * 0x3cu);
    int32_t q = (m == INT32_MIN && d == -1) ? m : m / d;
    return ((uint32_t)d & 0xffff0000u) | (uint16_t)q;
}

/* 00041310: the sound VBL task (A0 = VBLTask, preceded by the application's A5) */
static uint32_t DoSoundVB_task(maddr task) {
    ENTRY_LINK(FN_DoSoundVB, 2, 28);
    uint32_t a5v = R32(task - 4);
    W16(R32(gVBLTask) + 0xa, 1);
    uint32_t zone = _GetZone();
    _SetZone(R32(gVBLZone));
    if (!R8(gInSoundVB)) {
        W8(gInSoundVB, 1);
        HandleMusicVBLTask(R32(gSoundTicks));
        if (R8(gFilePlaying)) {
            if (FindVoice(FILE_VOICE_A) != 0 || FindVoice(FILE_VOICE_B) != 0) {
                W8(gFileVoiceEnded, 0);
            } else if (!R8(gFileVoiceEnded)) {
                if (R8(gFileRestart)) {
                    if (R16(gFileRestartDelay) > 0) {
                        W16(gFileRestartDelay, R16(gFileRestartDelay) - 1);
                    } else {
                        W8(gFileRestart, 0);
                        PlayBuffer(0, 0);
                    }
                } else {
                    W8(gFileRestart, 1);
                    W16(gFileRestartDelay, 8);
                }
            }
        }
        if (R16(gBeginDelay) > 0) W16(gBeginDelay, R16(gBeginDelay) - 1);
        if (R32(gSoundVBCallBack)) call_ptr(R32(gSoundVBCallBack), "");
        if (R32(gVoices)) {
            uint32_t v = R32(gVoices);
            for (int16_t i = 0; i < R16(gVoiceCount); i++, v += VOICE_SIZE) {
                if (!R8(v)) continue;
                if (R32(v + 0x12) != 0xFFFFFFFFu && (int32_t)R32(v + 0x12) > 0)
                    W32(v + 0x12, R32(v + 0x12) - 1);
                if (R8(v + 0x4a) && R32(v + 0x4e) && !R8(v + 0x4b)) {
                    W8(v + 0x4b, 1);
                    uint8_t r = pascal_bool_w(R32(v + 0x4e), R16(v + 0x36));
                    W16(v + 0x44, r ? 0 : -1);
                }
            }
        }
        if (R32(gSoundList)) {
            if (R16(gSoundListDelay) >= 0) {
                W16(gSoundListDelay, R16(gSoundListDelay) - 1);
            } else if (R16(gSoundListIndex) < R16(gSoundListCount)) {
                uint32_t e = (int32_t)R16(gSoundListIndex) * 8 + R32(gSoundList);
                uint32_t list = R32(gSoundList);
                if (R16(e) == -1) {
                    W8(gInSoundVB, 0);
                    EndSoundQuick(R16(e));
                    W8(gInSoundVB, 1);
                } else {
                    BeginSound(R16(e), R32(e + 2));
                }
                if (R16(e + 6) == -2) {
                    uint32_t v = FindVoice(R16(e));
                    if (v) W16(gSoundListDelay, R16(v + 0x14));
                    else W16(gSoundListDelay, 0);
                } else {
                    W16(gSoundListDelay, R16(e + 6));
                }
                W16(gSoundListIndex, R16(gSoundListIndex) + 1);
                W32(gSoundList, list);
            } else {
                W32(gSoundList, 0);
            }
        }
        W32(gSoundTicks, R32(gSoundTicks) + 1);
        W8(gInSoundVB, 0);
    }
    _SetZone(zone);
    return a5v;
}
uint32_t DoSoundVB(void) { return DoSoundVB_task(cpu_get(R_A0)); }
void glue_DoSoundVB(void) { emu_set_d0(DoSoundVB_task(cpu_get(R_A0))); }

/* 000414d2 */
uint32_t SetSoundVBCallBack(uint32_t proc) {
    W32(gSoundVBCallBack, proc);
    return 0;
}

/* 000414f6: only plausible addresses (> 0x1000) are kept */
uint32_t SetSoundDoneCallBack(uint32_t proc) {
    W32(gSoundDoneCallBack, 0);
    if ((int32_t)proc > 0x1000) W32(gSoundDoneCallBack, proc);
    return 0;
}

/* 00041530: decompress a 'csnd' resource (delta-coded LZSS) into a new handle */
uint32_t DeltaDecompressHandle(uint32_t h) {
    ENTRY_LINK(FN_DeltaDecompressHandle, 4, 8);
    _HLock(h);
    _BlockMove(R32(h), a6 - 4, 4);
    uint32_t nh = _NewHandle(R32(a6 - 4));
    if (nh) {
        _HLock(nh);
        uint32_t dst = R32(nh);
        uint32_t size = tc_GetHandleSize(h);
        LZSSDeltaUncompress(R32(h) + 4, size - 4, dst, a6 - 4);
        _HUnlock(nh);
    }
    _HUnlock(h);
    return nh;
}

/* 000415a6: load 'csnd' (or 'snd ') id into a slot unless dontLoad (high byte) */
uint32_t LoadSampleIntoSlot(int16_t id, uint32_t slot, int16_t dontLoad) {
    ENTRY_LINK(FN_LoadSampleIntoSlot, 8, 32);
    uint8_t ok = 0;
    uint32_t h = 0;
    if (B_HI(dontLoad)) goto done;
    W8(slot, 0);
    h = _GetResource(0x63736e64, id);                         /* 'csnd' */
    if (h) {
        W8(slot + 1, 0);
        W32(a6 - 4, DeltaDecompressHandle(h));
        _HPurge(h);
        _ReleaseResource(h);
        h = R32(a6 - 4);
    } else {
        W8(slot + 1, 1);
        W8(slot + 2, 1);
        h = _GetResource(0x736e6420, id);                     /* 'snd ' */
    }
    if (h == 0) goto done;
    W8(slot + 3, _HGetState(h));
    if ((int8_t)R8(slot + 3) >= 0) _MoveHHi(h);
    if (!(R8(slot + 3) & 0x20)) W8(slot + 2, 0);
    if (R16(0x220) == 0) {                                    /* MemErr */
        _HLock(h);
        uint32_t p = R32(h);
        int16_t fmt = R16(p), nmods = 0, ncmds = 0;
        if (fmt == 0 || fmt < 0) fmt = -1;
        else if (fmt == 2) { nmods = 0; ncmds = R16(p + 4); }
        else if (fmt > 2) fmt = -1;
        else { nmods = R16(p + 2); ncmds = R16(p + 4 + (int32_t)nmods * 6); }
        if (fmt != -1) {
            W32(a6 - 8, (uint32_t)((int32_t)nmods * 6 + 6 + (int32_t)ncmds * 8));
            uint32_t base = tc_StripAddress(R32(h));
            uint32_t hdr = R32(a6 - 8) + base;
            W32(slot + 8, hdr);
            W32(slot + 0xc, hdr + 0x16);
            W32(slot + 0x10, R32(hdr + 4));
            W32(slot + 0x14, R32(hdr + 0xc));
            W32(slot + 0x18, R32(hdr + 0x10));
            W32(slot + 0x1e, R32(hdr + 8));
            ok = 1;
            goto done;
        }
        _HUnlock(h);
    }
    if (R8(slot + 1)) _ReleaseResource(h);
    else _DisposeHandle(h);
    h = 0;
done:
    W16(slot + 0x1c, id);
    W32(slot + 4, h);
    return ok;
}

/* 000416fe: free a slot's sample (only when no voice plays and no list runs) */
uint32_t FreeSampleFromSlot(uint32_t slot) {
    ENTRY_LINK(FN_FreeSampleFromSlot, 0, 8);
    if (!(uint8_t)FUN_00042474()) return 0;
    if (R32(gSoundList)) return 0;
    uint32_t h = R32(slot + 4);
    if (h == 0) return 0;
    if (R8(slot + 1)) {
        if (R8(slot + 2)) { _HUnlock(h); _ReleaseResource(h); }
        else _HSetState(h, R8(slot + 3));
    } else {
        _HUnlock(h);
        _DisposeHandle(h);
    }
    W32(slot + 4, 0);
    W32(slot + 8, 0);
    W32(slot + 0xc, 0);
    W8(slot, 0);
    return 0;
}

/* 00041774: allocate the slot table for a -1-terminated list of ids and load them */
uint32_t LoadSamples(uint32_t ids, int16_t dontLoad) {
    ENTRY_LINK(FN_LoadSamples, 0, 16);
    int16_t err = 0, n = 0;
    uint32_t p = ids;
    while (R16(p) != -1) { p += 2; n++; }
    if (n > 0) {
        uint32_t t = _NewPtrClear((uint32_t)((int32_t)n * SLOT_SIZE));
        W32(gSoundSlots, t);
        if (t) {
            W16(gSoundCount, n);
            for (int16_t i = 0; n > i; i++)
                LoadSampleIntoSlot(R16(ids + (int32_t)i * 2), (int32_t)i * SLOT_SIZE + R32(gSoundSlots),
                                   (int16_t)(B_HI(dontLoad) << 8));
        } else {
            err = -0x6c;
        }
    }
    return (uint16_t)err;
}

/* 000417e4: free every slot and the slot table */
uint32_t FUN_000417e4(void) {
    ENTRY_NOLINK(FN_FUN_000417e4, 4);
    if (R32(gSoundSlots)) {
        for (int16_t i = 0; i < R16(gSoundCount); i++)
            FreeSampleFromSlot((int32_t)i * SLOT_SIZE + R32(gSoundSlots));
        _DisposePtr(R32(gSoundSlots));
        W32(gSoundSlots, 0);
    }
    W8(gSoundsRegistered, 0);
    return 0;
}

/* 0004181a: reverse n bytes in place (the original swaps p[0] with p[n]) */
uint32_t ReverseSample(uint32_t p, uint32_t n) {
    uint32_t lo = p, hi = n + p;
    while (hi > lo) {
        uint8_t a = R8(lo), b = R8(hi);
        W8(hi, a);
        W8(lo++, b);
        hi--;
    }
    return 0;
}

/* 00041856 */
uint32_t InitSoundChannel(uint32_t v) {
    W32(v + 0x56, 0);
    W32(v + 0x6e, 0);
    W32(v + 0x72, 0);
    W8(v + 0x77, 0x3c);
    W8(v + 0x76, 0);
    return 1;
}

/* 00041898 */
uint32_t FinisSoundChannel(uint32_t v) {
    W8(v, 0);
    return 0;
}

/* 000418ba */
uint32_t RegisterSounds(uint32_t ids, int16_t dontLoad) {
    ENTRY_LINK(FN_RegisterSounds, 0, 0);
    if (R16(gVoiceCount) == 0) return 0;
    if (R8(gSoundsRegistered)) FUN_000418fa();
    W8(gSoundsRegistered, 1);
    return LoadSamples(ids, (int16_t)(B_HI(dontLoad) << 8));
}

/* 000418fa: stop everything and free the slots */
uint32_t FUN_000418fa(void) {
    ENTRY_NOLINK(FN_FUN_000418fa, 0);
    FUN_0004239a();
    return FUN_000417e4();
}

/* 00041904: install the sound VBL task (a system-heap stub that jumps to DoSoundVB
 * through the jump table, with A0 = task). On failure the original returns the caller's
 * D7, reproduced from the register. */
uint32_t FUN_00041904(void) {
    ENTRY_NOLINK(FN_FUN_00041904, 8);
    uint16_t r = (uint16_t)cpu_get(R_D7);
    W32(gVBLBlock, _NewPtrClear(0x12));
    uint32_t stub = _NewPtrSys(0xc);
    if (R32(gVBLBlock) && stub) {
        W32(gVBLTask, R32(gVBLBlock) + 4);
        W32(R32(gVBLBlock), g_a5);
        W32(gVBLZone, _GetZone());
        W16(stub, 0x6104);                  /* bsr.s *+6 */
        W32(stub + 2, JT_ADDR(FN_DoSoundVB));
        W32(stub + 6, 0x225f2251);          /* movea.l (sp)+,a1; movea.l (a1),a1 */
        W16(stub + 0xa, 0x4ed1);            /* jmp (a1) */
        W32(R32(gVBLTask) + 6, stub);
        W16(R32(gVBLTask) + 0xa, 1);
        W16(R32(gVBLTask) + 4, 1);
        W32(R32(gVBLTask), 0);
        W16(R32(gVBLTask) + 0xc, 0);
        W8(gInSoundVB, 1);
        if ((int8_t)R8((uint32_t)(int32_t)(int8_t)R8(0x12f)) >= 2) {   /* CPUFlag */
            tc_FlushInstructionCache();
            tc_FlushDataCache();
        }
        r = (uint16_t)_VInstall(R32(gVBLTask));
        W8(gInSoundVB, 0);
    } else {
        if (stub) _DisposePtr(stub);
        if (R32(gVBLBlock)) {
            _DisposePtr(R32(gVBLBlock));
            W32(gVBLBlock, 0);
            W32(gVBLTask, 0);
        }
    }
    return r;
}

/* 000419ce: remove the sound VBL task (leaves gInSoundVB set) */
uint32_t FUN_000419ce(void) {
    ENTRY_NOLINK(FN_FUN_000419ce, 0);
    W8(gInSoundVB, 1);
    if (R32(gVBLTask)) {
        if (R32(R32(gVBLTask) + 6)) _DisposePtr(R32(R32(gVBLTask) + 6));
        _VRemove(R32(gVBLTask));
        W32(gVBLTask, 0);
    }
    if (R32(gVBLBlock)) _DisposePtr(R32(gVBLBlock));
    return 0;
}

/* 00041a08: initialise the sound layer with nvoices voices (needs System 6+) */
uint32_t InitSoundSystem(int16_t nvoices) {
    ENTRY_LINK(FN_InitSoundSystem, 0x10, 8);
    int16_t err = 0;
    W8(gSoundInited, 1);
    W16(gSystemVersion, 0x100);
    W16(gBeginDelayRate, 3);
    W16(gBeginDelay, 0);
    W32(gSoundList, 0);
    W8(gSoundOff, 1);
    W32(gSoundVBCallBack, 0);
    W32(gSoundDoneCallBack, 0);
    W32(gSoundSlots, 0);
    W8(gSoundsRegistered, 0);
    W8(gFilePlaying, 0);
    W8(gFileBufPending, 0);
    W8(gFileResume, 0);
    W16(gFileRestartDelay, 0);
    W8(gFileRestart, 0);
    W8(gFileVoiceEnded, 0);
    if ((int16_t)tc_SysEnvirons(a6 - 0x10, 1) == 0) W16(gSystemVersion, R16(a6 - 0xc));
    if (R16(gSystemVersion) >= 0x600) {
        W16(gVoiceCount, nvoices);
        uint32_t v = _NewPtrClear((uint32_t)((int32_t)nvoices * VOICE_SIZE));
        W32(gVoices, v);
        if (v) {
            for (int16_t i = 0; i < R16(gVoiceCount); i++)
                InitSoundChannel((int32_t)i * VOICE_SIZE + R32(gVoices));
            err = (int16_t)FUN_00041904();
            if (err) {
                _DisposePtr(R32(gVoices));
                W32(gVoices, 0);
            } else {
                W8(gSoundOff, 0);
            }
        } else {
            err = -0x6c;
        }
    }
    return (uint16_t)err;
}

/* 00041ae8: shut the sound layer down */
uint32_t FUN_00041ae8(void) {
    ENTRY_NOLINK(FN_FUN_00041ae8, 4);
    W8(gSoundOff, 0);
    FUN_000427b6();
    FUN_0004239a();
    FUN_000419ce();
    for (int16_t i = 0; i < R16(gVoiceCount); i++)
        FinisSoundChannel((int32_t)i * VOICE_SIZE + R32(gVoices));
    if (R32(gVoices)) {
        _DisposePtr(R32(gVoices));
        W32(gVoices, 0);
    }
    FUN_000417e4();
    return 0;
}

/* 00041b2e: suspend sound (switch-out) */
uint32_t FUN_00041b2e(void) {
    ENTRY_NOLINK(FN_FUN_00041b2e, 4);
    if (R8(gSoundOff)) return 0;
    W32(gSavedSoundList, R32(gSoundList));
    if (R8(gFilePlaying)) W8(gFileResume, 1);
    FUN_0004239a();
    FUN_000419ce();
    W8(gSoundOff, 1);
    for (int16_t i = 0; i < R16(gVoiceCount); i++)
        FinisSoundChannel((int32_t)i * VOICE_SIZE + R32(gVoices));
    return 0;
}

/* 00041b76: resume sound (switch-in) */
uint32_t FUN_00041b76(void) {
    ENTRY_NOLINK(FN_FUN_00041b76, 4);
    if (!R8(gSoundOff)) return 0;
    W8(gSoundOff, 0);
    W32(gSoundList, R32(gSavedSoundList));
    for (int16_t i = 0; i < R16(gVoiceCount); i++)
        InitSoundChannel((int32_t)i * VOICE_SIZE + R32(gVoices));
    W8(gInSoundVB, 0);
    W8(gFileVoiceEnded, 1);
    FUN_00041904();
    if (R8(gFileResume)) {
        W8(gFileResume, 0);
        W8(gFileVoiceEnded, 1);
        StartFilePlayback(gFileSpec, R32(gFileRate), R32(gFileBufSize));
    }
    return 0;
}

/* 00041bda: memory needed for a -1-terminated list of sound ids */
uint32_t SoundMemorySize(uint32_t ids) {
    ENTRY_LINK(FN_SoundMemorySize, 4, 24);
    int16_t n = 0;
    uint32_t p = ids;
    while (R16(p) != -1) { p += 2; n++; }
    int32_t total = (int32_t)n * SLOT_SIZE;
    if (n > 0) {
        for (int16_t i = 0; n > i; i++) {
            _SetResLoad(1);
            uint32_t h = _GetResource(0x63736e64, R16(ids + (int32_t)i * 2));   /* 'csnd' */
            if (h == 0) {
                _SetResLoad(0);
                h = _GetResource(0x736e6420, R16(ids + (int32_t)i * 2));       /* 'snd ' */
                if (h) W32(a6 - 4, _GetResourceSizeOnDisk(h));
            } else {
                _HLock(h);
                _BlockMove(R32(h), a6 - 4, 4);
                _HUnlock(h);
                _ReleaseResource(h);
            }
            if (h) total += (int32_t)R32(a6 - 4);
        }
        _SetResLoad(1);
    }
    return (uint32_t)total;
}

/* 00041c84 */
uint32_t GetSoundWaveform(int16_t id) {
    ENTRY_LINK(FN_GetSoundWaveform, 0, 8);
    uint32_t s = FindSound(id);
    return s ? R32(s + 0xc) : 0;
}

/* 00041cc0 */
uint32_t GetSoundLength(int16_t id) {
    ENTRY_LINK(FN_GetSoundLength, 0, 8);
    uint32_t s = FindSound(id);
    return s ? R32(s + 0x10) : 0;
}

/* 00041cfa: duration in VBLs of sound id at a rate */
uint32_t GetSoundTime(int16_t id, uint32_t rate) {
    ENTRY_LINK(FN_GetSoundTime, 0, 0);
    uint32_t len = GetSoundLength(id);
    return DEEPER(2, CalcPlaybackLength(rate, len));
}

/* 00041d24 */
uint32_t GetSoundDefaultRate(int16_t id) {
    ENTRY_LINK(FN_GetSoundDefaultRate, 0, 8);
    uint32_t s = FindSound(id);
    return s ? R32(s + 0x1e) : 0;
}

/* 00041d62 */
uint32_t GetSoundLoopStart(int16_t id) {
    ENTRY_LINK(FN_GetSoundLoopStart, 0, 8);
    uint32_t s = FindSound(id);
    return s ? R32(s + 0x14) : 0;
}

/* 00041d9e */
uint32_t GetSoundLoopEnd(int16_t id) {
    ENTRY_LINK(FN_GetSoundLoopEnd, 0, 8);
    uint32_t s = FindSound(id);
    return s ? R32(s + 0x18) : 0;
}

/* 00041dd8: master volume 0..0x100 */
uint32_t SetMasterVolume(int16_t vol) {
    ENTRY_LINK(FN_SetMasterVolume, 0, 8);
    int16_t v = vol;
    if (v < 0) v = 0;
    if (v > 0x100) v = 0x100;
    if ((uint8_t)IsNewSoundManagerInstalled()) {
        uint32_t lv = (uint32_t)(int32_t)v;
        lv |= lv << 16;
        _SetDefaultOutputVolume(lv);
    } else {
        int32_t sv = (int32_t)v * 7 / 0x100;
        tc_SetSoundVol((int16_t)sv);
    }
    return 0;
}

/* 00041e44: master volume 0..0x100 */
uint32_t GetMasterVolume(void) {
    ENTRY_LINK(FN_GetMasterVolume, 6, 0);
    uint32_t hi;
    if ((uint8_t)IsNewSoundManagerInstalled()) {
        _GetDefaultOutputVolume(a6 - 6);
        W16(a6 - 2, RU16(a6 - 4));
        hi = 0xffff0000u;
    } else {
        W8(a6 - 2, 0);
        W8(a6 - 1, R8(0x260));                                /* SdVolume */
        W16(a6 - 2, RU16(a6 - 2) + 1);
        int32_t x = (int32_t)R16(a6 - 2) << 8;
        W16(a6 - 2, (uint16_t)(x / 7));
        hi = 0;
    }
    return hi | RU16(a6 - 2);
}

/* 00041ea6 */
uint32_t SetBeginSoundDelayRate(int16_t r) {
    W16(gBeginDelayRate, r);
    return 0;
}

/* 00041ece: play raw sample data (id 0x8000) */
uint32_t PlayTheSample(uint32_t data, uint32_t len, uint32_t rate) {
    ENTRY_LINK(FN_PlayTheSample, 0, 0);
    uint32_t v = LowLevelPlaySample(data, len, rate, (int16_t)0x8000, 0, 0, 0, 0x100, 0,
                                    JT_ADDR(FN_CallPlaySampleCallback));
    W32(gLastSampleVoice, v);
    return v;
}

/* 00041f0c */
uint32_t PlayTheSampleWithID(uint32_t data, uint32_t len, uint32_t rate, int16_t id) {
    ENTRY_LINK(FN_PlayTheSampleWithID, 0, 0);
    uint32_t v = LowLevelPlaySample(data, len, rate, id, 0, 0, 0, 0x100, 0,
                                    JT_ADDR(FN_CallPlaySampleCallback));
    W32(gLastSampleVoice, v);
    return v;
}

/* 00041f50 */
uint32_t ChangeSoundPitch(int16_t id, uint32_t rate) {
    ENTRY_LINK(FN_ChangeSoundPitch, 0, 4);
    if (R8(gSoundOff)) return 0;
    uint32_t v = FindVoice(id);
    if (v == 0) return 0;
    W16(v + 0x36, id);
    W32(v + 0x26, rate);
    MusicDriverHandler(v + 0x1e, 0x68);
    return 0;
}

/* slot's sample back in forward order (shared head of the BeginSound family) */
static void unreverse(uint32_t s) {
    if (R8(s)) {
        ReverseSample(R32(s + 0xc), R32(s + 0x10));
        W8(s, 0);
    }
}

/* 00041fa2: play sound id (rate 0 = its own rate) */
uint32_t BeginSound(int16_t id, uint32_t rate) {
    ENTRY_LINK(FN_BeginSound, 0, 16);
    if (R8(gSoundOff)) return 0;
    uint32_t s = FindSound(id);
    if (s == 0) return 0;
    unreverse(s);
    if (rate == 0) rate = R32(s + 0x1e);
    uint32_t len = R32(s + 0x10);
    uint32_t data = R32(s + 0xc) - 6;
    LowLevelPlaySample(data, len, rate, id, 0, 0, 0, 0, 0, JT_ADDR(FN_CallPlaySampleCallback));
    return 0;
}

/* 00042020: make sure sound id is loaded (forward) */
uint32_t LoadSound(int16_t id) {
    ENTRY_LINK(FN_LoadSound, 8, 4);
    if (R8(gSoundOff)) return 0;
    uint32_t s = FindSound(id);
    if (s && R8(s)) {
        ReverseSample(R32(s + 0xc), R32(s + 0x10));
        W8(s, 0);
    }
    return 0;
}

/* 00042062 */
uint32_t FreeSound(int16_t id) {
    ENTRY_LINK(FN_FreeSound, 0, 4);
    if (R8(gSoundOff)) return 0;
    uint32_t s = FindSound(id);
    if (s) FreeSampleFromSlot(s);
    return 0;
}

/* 00042098: play sound id backwards */
uint32_t BeginSoundReverse(int16_t id, uint32_t rate) {
    ENTRY_LINK(FN_BeginSoundReverse, 0, 16);
    if (R8(gSoundOff)) return 0;
    uint32_t s = FindSound(id);
    if (s == 0) return 0;
    if (!R8(s)) {
        ReverseSample(R32(s + 0xc), R32(s + 0x10));
        W8(s, 1);
    }
    if (rate == 0) rate = R32(s + 0x1e);
    uint32_t len = R32(s + 0x10);
    uint32_t data = R32(s + 0xc) - 6;
    LowLevelPlaySample(data, len, rate, id, 0, 0, 0, 0, 0, JT_ADDR(FN_CallPlaySampleCallback));
    return 0;
}

/* 0004211e: play bytes start..end of sound id (end -1 = to the end) */
uint32_t BeginSoundSection(int16_t id, uint32_t rate, uint32_t start, uint32_t end) {
    ENTRY_LINK(FN_BeginSoundSection, 0, 24);
    if (R8(gSoundOff)) return 0;
    uint32_t s = FindSound(id);
    if (s == 0) return 0;
    unreverse(s);
    if (rate == 0) rate = R32(s + 0x1e);
    if (end == 0xFFFFFFFFu || (int32_t)end > (int32_t)R32(s + 0x10)) end = R32(s + 0x10);
    uint32_t len = end - start;
    uint32_t data = start - 6 + R32(s + 0xc);
    LowLevelPlaySample(data, len, rate, id, start, end, 0, 0, 0, JT_ADDR(FN_CallPlaySampleCallback));
    return 0;
}

/* 000421bc: play sound id looping between start and end (-1 = defaults) */
uint32_t BeginSoundLoop(int16_t id, uint32_t rate, uint32_t start, uint32_t end) {
    ENTRY_LINK(FN_BeginSoundLoop, 0, 24);
    if (R8(gSoundOff)) return 0;
    uint32_t s = FindSound(id);
    if (s == 0) return 0;
    unreverse(s);
    if (rate == 0) rate = R32(s + 0x1e);
    if (start == 0xFFFFFFFFu) start = 1;
    if (end == 0xFFFFFFFFu) end = R32(s + 0x10);
    if ((int32_t)end > (int32_t)R32(s + 0x10)) end = R32(s + 0x10);
    uint32_t len = end - start;
    uint32_t data = R32(s + 0xc) - 6;
    LowLevelPlaySample(data, len, rate, id, start, end, -1, 0x100, 0, JT_ADDR(FN_CallPlaySampleCallback));
    return 0;
}

/* 00042266: play sound id with its loop and an envelope value; no start delay */
uint32_t BeginSoundEnvelope(int16_t id, uint32_t rate, int16_t envelope) {
    ENTRY_LINK(FN_BeginSoundEnvelope, 0, 20);
    if (R8(gSoundOff)) return 0;
    uint32_t s = FindSound(id);
    if (s == 0) return 0;
    unreverse(s);
    if (rate == 0) rate = R32(s + 0x1e);
    int16_t saved = R16(gBeginDelayRate);
    W16(gBeginDelayRate, 0);
    W16(gBeginDelay, 0);
    uint32_t len = R32(s + 0x10);
    uint32_t data = R32(s + 0xc) - 6;
    LowLevelPlaySample(data, len, rate, id, R32(s + 0x14), R32(s + 0x18), envelope, 0x100, 0,
                       JT_ADDR(FN_CallPlaySampleCallback));
    W16(gBeginDelayRate, saved);
    return 0;
}

/* 00042306: play sound id looping until proc(id) says stop */
uint32_t BeginSoundEnvelopeProc(int16_t id, uint32_t rate, uint32_t proc) {
    ENTRY_LINK(FN_BeginSoundEnvelopeProc, 0, 16);
    if (R8(gSoundOff)) return 0;
    uint32_t s = FindSound(id);
    if (s == 0) return 0;
    unreverse(s);
    if (rate == 0) rate = R32(s + 0x1e);
    uint32_t len = R32(s + 0x10);
    uint32_t data = R32(s + 0xc) - 6;
    LowLevelPlaySample(data, len, rate, id, R32(s + 0x14), R32(s + 0x18), -1, 0x100, proc,
                       JT_ADDR(FN_CallPlaySampleCallback));
    return 0;
}

/* 0004239a: stop all voices, the file playback and the list */
uint32_t FUN_0004239a(void) {
    ENTRY_NOLINK(FN_FUN_0004239a, 4);
    FUN_000427b6();
    W32(gSoundList, 0);
    for (int16_t i = 0; i < R16(gVoiceCount); i++)
        EndVoice((int32_t)i * VOICE_SIZE + R32(gVoices), 0x100, 0);
    MusicDriverHandler(0, 0x67);
    return 0;
}

/* 000423d8: stop sound id (calls its done callback) */
uint32_t EndSound(int16_t id) {
    ENTRY_LINK(FN_EndSound, 0x7a, 4);
    if (R8(gSoundOff)) return 0;
    uint32_t v = FindVoice(id);
    if (v == 0) return 0;
    W32(gSoundList, 0);
    if (!R8(v)) return 0;
    W16(v + 0x36, id);
    W32(v + 0x38, g_a5);
    EndVoice(v, 0x100, 0x100);
    return 0;
}

/* 00042428: stop sound id without callback */
uint32_t EndSoundQuick(int16_t id) {
    ENTRY_LINK(FN_EndSoundQuick, 0, 4);
    if (R8(gSoundOff)) return 0;
    uint32_t v = FindVoice(id);
    if (v == 0) return 0;
    W16(v + 0x36, id);
    W32(v + 0x38, g_a5);
    EndVoice(v, 0x100, 0);
    return 0;
}

/* 00042474: true when no voice is active */
uint32_t FUN_00042474(void) {
    uint8_t r = 1;
    if (R8(gSoundOff)) return r;
    for (int16_t i = 0; i < R16(gVoiceCount); i++)
        if (R8((int32_t)i * VOICE_SIZE + R32(gVoices))) { r = 0; break; }
    return r;
}

/* 000424a6 */
uint32_t IsThisSoundFXFinished(int16_t id) {
    ENTRY_LINK(FN_IsThisSoundFXFinished, 0, 8);
    uint8_t r = 1;
    if (!R8(gSoundOff)) {
        uint32_t v = FindVoice(id);
        if (v && R8(v)) r = 0;
    }
    return r;
}

/* 000424ee: play a list of n {id, rate, delay} entries from the VBL task */
uint32_t BeginSoundList(uint32_t list, int16_t n) {
    ENTRY_LINK(FN_BeginSoundList, 4, 4);
    if (R8(gSoundOff)) return 0;
    FUN_0004239a();
    for (int16_t i = 0; i < n; i++) FindSound(R16(list + (int32_t)i * 8));
    W16(gSoundListIndex, 0);
    W16(gSoundListDelay, 0);
    W16(gSoundListCount, n);
    W32(gSoundList, list);
    return 0;
}

/* 00042546: stop the sound list (and its current entry) */
uint32_t FUN_00042546(void) {
    ENTRY_NOLINK(FN_FUN_00042546, 8);
    if (R32(gSoundList) == 0) return 0;
    uint32_t list = R32(gSoundList);
    W32(gSoundList, 0);
    if (R16(gSoundListIndex) < R16(gSoundListCount)) {
        uint32_t e = list + (int32_t)R16(gSoundListIndex) * 8;
        if (R16(e) != -1) EndSoundQuick(R16(e));
        W16(gSoundListIndex, R16(gSoundListIndex) + 1);
    }
    return 0;
}

/* 0004259a: free sound slots while free memory is below minFree */
uint32_t PurgeAllSounds(uint32_t minFree) {
    ENTRY_LINK(FN_PurgeAllSounds, 0, 4);
    if (R32(gSoundSlots) == 0) return 0;
    for (int16_t i = 0; i < R16(gSoundCount); i++)
        if (_FreeMem() < minFree) FreeSampleFromSlot((int32_t)i * SLOT_SIZE + R32(gSoundSlots));
    return 0;
}

/* 000425e2: lock (keep loaded) or unlock sound id */
uint32_t SoundLock(int16_t id, int16_t unlock) {
    ENTRY_LINK(FN_SoundLock, 0, 4);
    uint32_t s = FindSound(id);
    if (s == 0) return 0;
    uint8_t b = B_HI(unlock) == 0 ? 1 : 0;
    W8(s + 2, b);
    return (s & 0xffffff00u) | b;
}

/* ============================================================ file playback == */

/* 00042616: play a file (spec: vRefNum, dirID, name) through two buffers */
uint32_t StartFilePlayback(uint32_t spec, uint32_t rate, uint32_t size) {
    ENTRY_LINK(FN_StartFilePlayback, 0, 8);
    if (R8(gFilePlaying)) FUN_000427b6();
    W16(gFileErr, 0);
    W8(gFile192, 0);
    if (!R8(gFileResume)) W32(gFilePos, 0);
    W32(gFileRate, rate);
    W32(gFileBufSize, size);
    mcopy(gFileSpec, spec, 0x46);
    W16(gFileRestartDelay, 0);
    W8(gFileRestart, 0);
    if ((int32_t)size > 10000) {
        W32(gFileBuf, _NewPtrClear(size + 6));
        W32(gFileBuf + 4, _NewPtrClear(size + 6));
        if (R32(gFileBuf) && R32(gFileBuf + 4)) {
            W16(gFileErr, OPENWD(gFileWDRefNum, 0x4552494b, R32(spec + 2), R16(spec)));  /* 'ERIK' */
            if (R16(gFileErr) == 0) {
                W16(gFileErr, tc_FSOpen(gFileRefNum, R16(gFileWDRefNum), spec + 6));
                if (R16(gFileErr) != 0) CLOSEWD(R16(gFileWDRefNum));
            }
        } else {
            W16(gFileErr, -0x6c);
            if (R32(gFileBuf)) { _DisposePtr(R32(gFileBuf)); W32(gFileBuf, 0); }
            if (R32(gFileBuf + 4)) { _DisposePtr(R32(gFileBuf + 4)); W32(gFileBuf + 4, 0); }
        }
    } else {
        W16(gFileErr, -0xd2);
    }
    if (R16(gFileErr) == 0) {
        W8(gFilePlaying, 1);
        W8(gFileBufPending, 1);
        W16(gFileBufIndex, 0);
        ReadBuffer(0);
        /* ReadBuffer's argument word (0) stays on the stack */
        maddr sp = emu_sp();
        W16(sp - 2, 0);
        DEEPER(2, LowLevelPlaySample(R32(gFileBuf), R32(gFileBufLen), R32(gFileRate), FILE_VOICE_A,
                                     0, 0, 0, 0x100, 0, JT_ADDR(FN_PlayBuffer)));
        W16(gFileBufIndex, 1);
    }
    return RU16(gFileErr);
}

/* 0004276e */
uint32_t ChangeFilePlaybackRate(uint32_t rate) {
    ENTRY_LINK(FN_ChangeFilePlaybackRate, 0, 0);
    if (!R8(gFilePlaying)) return 0;
    W32(gFileRate, rate);
    ChangeSoundPitch(FILE_VOICE_A, rate);
    DEEPER(2, ChangeSoundPitch(FILE_VOICE_B, rate));
    return 0;
}

/* 000427b6: stop file playback, free the buffers, close the file */
uint32_t FUN_000427b6(void) {
    ENTRY_NOLINK(FN_FUN_000427b6, 0);
    if (!R8(gFilePlaying)) return 0;
    W8(gFilePlaying, 0);
    EndSoundQuick(FILE_VOICE_A);
    EndSoundQuick(FILE_VOICE_B);
    if (R32(gFileBuf)) { _DisposePtr(R32(gFileBuf)); W32(gFileBuf, 0); }
    if (R32(gFileBuf + 4)) { _DisposePtr(R32(gFileBuf + 4)); W32(gFileBuf + 4, 0); }
    tc_FSClose(R16(gFileRefNum));
    CLOSEWD(R16(gFileWDRefNum));
    W16(gFileErr, 0);
    return 0;
}

/* 0004280e: read the next chunk of the file into buffer i (wraps at EOF) */
uint32_t ReadBuffer(int16_t i) {
    ENTRY_LINK(FN_ReadBuffer, 4, 4);
    uint32_t buf = R32(gFileBuf + (int32_t)i * 4) + 6;
    W32(a6 - 4, R32(gFileBufSize));
    tc_SetFPos(R32(gFilePos), 1, R16(gFileRefNum));
    W16(gFileErr, tc_FSRead(buf, a6 - 4, R16(gFileRefNum)));
    W32(gFileBufLen + (int32_t)i * 4, R32(a6 - 4));
    W32(gFilePos, R32(gFilePos) + R32(a6 - 4));
    if (R16(gFileErr) == -0x27) {                             /* eofErr */
        tc_SetFPos(0, 1, R16(gFileRefNum));
        W32(gFilePos, 0);
        W16(gFileErr, 0);
    }
    return 0;
}

/* 00042898: Pascal done callback of the file voices: start the other buffer */
uint32_t PlayBuffer(int16_t p4, int16_t p6) {
    ENTRY_LINK(FN_PlayBuffer, 0, 0);
    (void)p4; (void)p6;
    if (!R8(gFilePlaying) || R8(gFileBufPending)) return 0;
    int16_t which = R16(gFileBufIndex);
    if (which == 0) {
        EndSoundQuick(FILE_VOICE_A);
        EndSoundQuick(FILE_VOICE_B);
        maddr sp = emu_sp();
        W16(sp - 2, (uint16_t)FILE_VOICE_B);
        DEEPER(2, LowLevelPlaySample(R32(gFileBuf), R32(gFileBufLen), R32(gFileRate), FILE_VOICE_A,
                                     0, 0, 0, 0x100, 0, JT_ADDR(FN_PlayBuffer)));
        W16(gFileBufIndex, 1);
        W8(gFileBufPending, 1);
    } else if (which == 1) {
        EndSoundQuick(FILE_VOICE_A);
        EndSoundQuick(FILE_VOICE_B);
        maddr sp = emu_sp();
        W16(sp - 2, (uint16_t)FILE_VOICE_B);
        DEEPER(2, LowLevelPlaySample(R32(gFileBuf + 4), R32(gFileBufLen + 4), R32(gFileRate), FILE_VOICE_B,
                                     0, 0, 0, 0x100, 0, JT_ADDR(FN_PlayBuffer)));
        W16(gFileBufIndex, 0);
        W8(gFileBufPending, 1);
    }
    return 0;
}

/* 00042956: file playback idle task: refill the pending buffer, restart when idle */
uint32_t FUN_00042956(void) {
    ENTRY_NOLINK(FN_FUN_00042956, 4);
    uint8_t r = 0;
    if (!R8(gFilePlaying)) return r;
    if (R8(gFileBufPending)) {
        W8(gInSoundVB, 1);
        int16_t which = R16(gFileBufIndex);
        if (which == 0) { ReadBuffer(0); r = 1; }
        else if (which == 1) { ReadBuffer(1); r = 1; }
        W8(gFileBufPending, 0);
        W8(gInSoundVB, 0);
    }
    FindVoice(FILE_VOICE_A);
    FindVoice(FILE_VOICE_B);
    if (FindVoice(FILE_VOICE_A) == 0 && FindVoice(FILE_VOICE_B) == 0 && !R8(gFileVoiceEnded))
        PlayBuffer(0, 0);
    return r;
}

/* 000429e0: waveObj::deviceCallback (Pascal): on WOM_DONE (0x3bd) report the finished
 * header to the object (vtable +0x1c) */
uint32_t deviceCallback__7waveObj(int16_t p4, int16_t p6, uint32_t hdr, uint32_t obj, int16_t msg, int16_t p18) {
    ENTRY_LINK(FN_deviceCallback__7waveObj, 0x10, 32);
    (void)p4; (void)p6; (void)p18;
    if (msg != 0x3bd) return 0;
    if (R16(obj + 0x44) != 0) return 0;
    if (R32(hdr + 0xc) == 0) return 0;
    W32(a6 - 8, R32(hdr + 0xc));
    W32(a6 - 4, R32(R32(a6 - 8) + 8));
    if (R32(a6 - 4) != 0) {
        W8(a6 - 0x10, 0);
        uint32_t m = R32(a6 - 4);
        W16(a6 - 0xe, R8(m + 4));
        W32(a6 - 0xc, m + 5);
        call_ptr(R32(R32(obj) + 0x1c), "ll", obj, a6 - 0x10);
    }
    if (R32(R32(a6 - 8) + 0x1c) & 8) {
        W16(obj + 0x46, 1);
        if (R16(obj + 0x48) == 0) {
            uint32_t a = R32(obj + 0x82) - R32(obj + 0x7e);
            uint32_t b = R32(R32(obj + 0x4e) + 0x20) - 1;
            W32(obj + 0x4a, a * b);
            if (R16(obj + 0x7c) != -1) W32(R32(obj + 0x4e) + 0x20, 1);
        }
    }
    uint32_t last = ((uint32_t)RU16(obj + 0x8a) - 1) * 0x2c + obj + 0x8c;
    if (last == R32(a6 - 8)) {
        W16(obj + 0x16, 0);
        W32(obj + 0x30, R32(obj + 0x74));
        W8(a6 - 0x10, 1);
        call_ptr(R32(R32(obj) + 0x1c), "ll", obj, a6 - 0x10);
    }
    return 0;
}

/* 00042bb2: address of a constant table in this segment (0x42b80) */
uint32_t FUN_00042bb2(void) {
    return fn_address(FN_FUN_00042bb2) - 0x32;
}

/* 00042b28: bra FUN_00042bb2 */
uint32_t thunk_FUN_00042bb2(void) {
    return FUN_00042bb2();
}

/* ==================================================================== config == */
/* Cached config file (memory handle, 0x116 bytes): +0 previous, +2 next (handles),
 * +4 fileSpec (0x106 bytes), +0x10a modification time (6 bytes), +0x110 text size,
 * +0x112 current offset, +0x114 handle of the text (NUL-terminated). */

/* 00042bbc: memory handle of the cached text of a config file (loaded if needed), 0 on
 * error */
uint32_t configFindFile(uint32_t spec) {
    ENTRY_LINK(FN_configFindFile, 0x20, 32);
    int16_t err;
    int16_t h = R16(gConfigFiles);
    while (h != 0) {
        W32(a6 - 0x20, memLock(h));
        err = (int16_t)equal__8fileSpec(R32(a6 - 0x20) + 4, spec);
        if (err != 0) {
            if (err != 0x2844) goto fail;
            int16_t next = R16(R32(a6 - 0x20) + 2);
            memUnlock(h);
            h = next;
            continue;
        }
        if ((int16_t)fileGetTime(a6 - 0x14, spec) != 0) goto timeerr;
        W32(a6 - 0xc, fileGetSize(spec));
        if (R32(a6 - 0xc) == 0xFFFFFFFFu) goto timeerr;
        if (tc_memcmp(R32(a6 - 0x20) + 0x10a, a6 - 0x14, 6) == 0 &&
            (uint32_t)RU16(R32(a6 - 0x20) + 0x110) == R32(a6 - 0xc) &&
            memGetHandleSize(R16(R32(a6 - 0x20) + 0x114)) == R32(a6 - 0xc) + 1) {
            memUnlock(h);
            W16(a6 - 6, 0);
            W16(gConfigError, 0);
            return (uint16_t)h;
        }
        memUnlock(h);
        configFreeFile(h);
        break;
    timeerr:
        memUnlock(h);
        err = (int16_t)fileGetError();
        W16(a6 - 8, err);
        W16(gConfigError, err);
        return 0;
    }
    W32(a6 - 0xc, fileGetSize(spec));
    if (R32(a6 - 0xc) == 0xFFFFFFFFu) {
        err = (int16_t)fileGetError();
        if (err == 0x2845) err = 0x2968;
        goto fail;
    }
    if (R32(a6 - 0xc) + 1 > 0x10000u) { err = 0x296b; goto fail; }
    h = (int16_t)memAllocHandle(1, 0x116);
    if (h == 0) { err = (int16_t)memGetError(); goto fail; }
    W32(a6 - 0x20, memLock(h));
    tc_memset(R32(a6 - 0x20), 0, 0x116);
    {
        int16_t th = (int16_t)memAllocHandle(1, R32(a6 - 0xc) + 1);
        W16(R32(a6 - 0x20) + 0x114, th);
        if (th == 0) { err = (int16_t)memGetError(); goto freerec; }
    }
    W32(a6 - 0x1c, memLock(R16(R32(a6 - 0x20) + 0x114)));
    {
        int16_t ref;
        if ((int16_t)fileGetTime(R32(a6 - 0x20) + 0x10a, spec) != 0 ||
            (ref = (int16_t)fileOpen(1, spec)) == 0) {
            err = (int16_t)fileGetError();
            goto freetext;
        }
        err = (int16_t)fileRead(a6 - 0xc, R32(a6 - 0x1c), ref);
        fileClose(ref);
        if (err != 0) goto freetext;
    }
    {
        uint16_t size = RU16(a6 - 0xa);
        uint32_t sizep = R32(a6 - 0x20) + 0x110;
        W16(sizep, size);
        W8(R32(a6 - 0x1c) + size, 0);
        uint32_t nul = tc_strchr(R32(a6 - 0x1c), 0);
        if (nul < R32(a6 - 0x1c) + RU16(sizep)) { err = 0x2969; goto freetext; }
        W32(a6 - 0x18, tc_strpbrk(R32(a6 - 0x1c), gConfigDelims));
        if (R32(a6 - 0x18) != 0 && RS8(R32(a6 - 0x18)) == 0x1a) {
            if ((uint32_t)RU16(R32(a6 - 0x20) + 0x110) - 1 + R32(a6 - 0x1c) > R32(a6 - 0x18)) {
                err = 0x2969;
                goto freetext;
            }
            W8(R32(a6 - 0x18), 0);
            W16(R32(a6 - 0x20) + 0x110, RU16(R32(a6 - 0x20) + 0x110) - 1);
        }
    }
    {
        uint32_t rec = R32(a6 - 0x20);
        W16(rec, 0);
        W16(rec + 2, R16(gConfigFiles));
        W16(gConfigFiles, h);
        mcopy(rec + 4, spec, 0x106);
        uint32_t th = R32(a6 - 0x20) + 0x114;
        memUnlock(R16(th));
        memSetPurgeState(1, R16(th));
        memUnlock(h);
        W16(a6 - 2, 0);
        W16(gConfigError, 0);
        return (uint16_t)h;
    }
freetext:
    {
        uint32_t th = R32(a6 - 0x20) + 0x114;
        memUnlock(R16(th));
        memFreeHandle(R16(th));
    }
freerec:
    memUnlock(h);
    memFreeHandle(h);
fail:
    W16(a6 - 4, err);
    W16(gConfigError, err);
    return 0;
}

/* 00042f56: offset of the start of the line containing the current offset */
uint32_t configFindLineStart(uint32_t text, uint32_t rec) {
    uint16_t off = RU16(rec + 0x112);
    while (off != 0) {
        uint8_t c = R8(text + (uint32_t)off - 1);
        if (c == 0xd || c == 0xa) break;
        off--;
    }
    return off;
}

/* 00042fc8: offset of the line after the current offset (or the text size) */
uint32_t configFindNextLine(uint32_t text, uint32_t rec) {
    ENTRY_LINK(FN_configFindNextLine, 0, 16);
    uint32_t p = tc_strchr(text + RU16(rec + 0x112), 0xd);
    if (p) {
        p++;
        if (R8(p) == 0xa) p++;
    }
    if (p) return (uint16_t)((uint16_t)p - (uint16_t)text);
    return RU16(rec + 0x110);
}

/* 0004303e: unlink and free a cached config file */
uint32_t configFreeFile(int16_t h) {
    ENTRY_LINK(FN_configFreeFile, 0, 20);
    uint32_t rec = memLock(h);
    int16_t prev = R16(rec);
    if (prev != 0) {
        uint16_t next = RU16(rec + 2);
        uint32_t p = memXlat(prev);
        W16(p + 2, next);
    } else {
        W16(gConfigFiles, R16(rec + 2));
    }
    if (R16(rec + 2) != 0) {
        uint16_t pv = RU16(rec);
        uint32_t n = memXlat(R16(rec + 2));
        W16(n, pv);
    }
    __dt__8fileSpecDv(rec + 4);
    memFreeHandle(R16(rec + 0x114));
    memUnlock(h);
    memFreeHandle(h);
    return 0;
}

/* 000430e6: from the current line, find the next "name=" line of the section; returns the
 * name length (0 at the next "[" or the end), current offset = name */
uint32_t configGotoNextName(uint32_t text, uint32_t rec) {
    ENTRY_LINK(FN_configGotoNextName, 8, 20);
    for (;;) {
        uint16_t ws = (uint16_t)configScanWhitespace(text, rec);
        W16(rec + 0x112, ws);
        if (R8(text + ws) == '[') return 0;
        uint16_t nl = (uint16_t)configFindNextLine(text, rec);
        uint32_t eq = tc_strchr(text + RU16(rec + 0x112), '=');
        if (eq && (uint16_t)((uint16_t)eq - (uint16_t)text) < nl) {
            uint8_t c;
            do {
                eq--;
                c = R8(eq);
            } while (c == ' ' || c == 8);
            return (uint16_t)((uint16_t)(eq + 1) - (uint16_t)(text + RU16(rec + 0x112)));
        }
        W16(rec + 0x112, nl);
        if (nl >= RU16(rec + 0x110)) return 0;
    }
}

/* 000431d6: find the next "[type]" line; returns the type length, current offset = type */
uint32_t configGotoNextType(uint32_t text, uint32_t rec) {
    ENTRY_LINK(FN_configGotoNextType, 8, 24);
    for (;;) {
        uint16_t ws = (uint16_t)configScanWhitespace(text, rec);
        uint16_t nl = (uint16_t)configFindNextLine(text, rec);
        uint32_t p = text + ws;
        if (R8(p) == '[') {
            uint32_t q = tc_strchr(p, ']');
            if (q && (nl == RU16(rec + 0x110) || (uint16_t)((uint16_t)q - (uint16_t)text) < nl)) {
                W16(rec + 0x112, (uint16_t)(ws + 1));
                return (uint16_t)((uint16_t)q - (uint16_t)(p + 1));
            }
        }
        W16(rec + 0x112, nl);
        if (nl >= RU16(rec + 0x110)) return 0;
    }
}

/* 0004329c: go to "name=" in section [type]; current offset = value */
uint32_t configGotoText(uint32_t name, uint32_t type, uint32_t text, uint32_t rec) {
    ENTRY_LINK(FN_configGotoText, 0, 24);
    if ((int16_t)configGotoType(type, text, rec) == 0) return 0;
    uint16_t len = (uint16_t)tc_strlen(name);
    W16(rec + 0x112, configFindNextLine(text, rec));
    for (;;) {
        uint16_t n = (uint16_t)configGotoNextName(text, rec);
        if (n == 0) return 0;
        if (len == n && (int16_t)memicmp_(name, text + RU16(rec + 0x112), len) == 0) {
            uint32_t eq = tc_strchr(text + RU16(rec + 0x112) + n, '=');
            W16(rec + 0x112, (uint16_t)((uint16_t)(eq + 1) - (uint16_t)text));
            return 1;
        }
        W16(rec + 0x112, configFindNextLine(text, rec));
    }
}

/* 000433ac: go to section [type] (case-insensitive) */
uint32_t configGotoType(uint32_t type, uint32_t text, uint32_t rec) {
    ENTRY_LINK(FN_configGotoType, 0, 24);
    uint16_t len = (uint16_t)tc_strlen(type);
    W16(rec + 0x112, 0);
    for (;;) {
        uint16_t n = (uint16_t)configGotoNextType(text, rec);
        if (n == 0) return 0;
        if (len != n) continue;
        if ((int16_t)memicmp_(type, text + RU16(rec + 0x112), len) == 0) return 1;
    }
}

/* 0004343c: initialise the config library (needs os/mem/file libraries >= 1.0) */
uint32_t configInit(void) {
    ENTRY_LINK(FN_configInit, 0, 24);
    int16_t r;
    if ((uint16_t)osQueryInit() < 0x100 || (uint16_t)memQueryInit() < 0x100 ||
        (uint16_t)fileQueryInit() < 0x100) {
        W16(gConfigError, 0x299b);
        return 0x299b;
    }
    if (R16(gConfigInitCount) == 0) {
        tc_memset(gConfigError, 0, 8);
        W16(gConfigClientOff, osReserveClientBytes(2));
        if (R16(gConfigClientOff) == 0) {
            r = (int16_t)osGetError();
            W16(gConfigError, r);
            return (uint16_t)r;
        }
    }
    uint32_t cb = osGetClientBytes(0xFFFFFFFFu);
    W16(cb + RU16(gConfigClientOff), 1);
    W16(gConfigInitCount, R16(gConfigInitCount) + 1);
    W16(gConfigError, 0);
    return 0;
}

/* 00043502: 0x100 when initialised for this client */
uint32_t configQueryInit(void) {
    ENTRY_LINK(FN_configQueryInit, 0, 4);
    if (R16(gConfigInitCount) == 0) return 0;
    uint32_t cb = osGetClientBytes(0xFFFFFFFFu);
    if (R16(cb + RU16(gConfigClientOff)) != 0) return 0x100;
    return 0;
}

/* 00043552: atexit handler: release this client, free the cache after the last one */
uint32_t configQuit(void) {
    ENTRY_LINK(FN_configQuit, 0, 12);
    if ((int16_t)configQueryInit() == 0) {
        W16(gConfigError, 0x299a);
        return 0;
    }
    uint32_t cb = osGetClientBytes(0xFFFFFFFFu);
    W16(cb + RU16(gConfigClientOff), 0);
    W16(gConfigInitCount, R16(gConfigInitCount) - 1);
    if (R16(gConfigInitCount) != 0) return 0;
    while (R16(gConfigFiles) != 0) configFreeFile(R16(gConfigFiles));
    return 0;
}

/* 000435ba: offset of the first non-blank (space/BS) at the current offset */
uint32_t configScanWhitespace(uint32_t text, uint32_t rec) {
    uint32_t p = text + RU16(rec + 0x112);
    for (;;) {
        uint8_t c = R8(p);
        if (c != ' ' && c != 8) break;
        p++;
    }
    return (uint16_t)((uint16_t)p - (uint16_t)text);
}

/* 0004362e */
uint32_t configGetError(void) {
    return RU16(gConfigError);
}

/* 00043650: read a config value: type == 0 lists the types, name == 0 lists the names
 * of [type] (NUL-separated, double NUL at the end), else the value of name */
uint32_t configRead(int16_t bufSize, uint32_t buf, uint32_t name, uint32_t type, uint32_t spec) {
    ENTRY_LINK(FN_configRead, 0x2c, 32);
    W8(buf, 0);
    int16_t h = (int16_t)configFindFile(spec);
    if (h == 0) return RU16(gConfigError);
    uint32_t rec = memXlat(h);
    uint32_t text = memXlat(R16(rec + 0x114));
    uint16_t size = (uint16_t)bufSize;
    if (type == 0 || name == 0) {
        uint16_t cnt = 0;
        if (type == 0) {
            W16(rec + 0x112, 0);
        } else if ((int16_t)configGotoType(type, text, rec) == 0) {
            W16(gConfigError, 0x296a);
            return 0x296a;
        }
        for (;;) {
            uint16_t n = type == 0 ? (uint16_t)configGotoNextType(text, rec)
                                   : (uint16_t)configGotoNextName(text, rec);
            if (n == 0) {
                W16(gConfigError, 0);
                return 0;
            }
            if ((int32_t)((uint32_t)cnt + n + 2) > (int32_t)(uint32_t)size) {
                W16(gConfigError, 0x296b);
                return 0x296b;
            }
            tc_memcpy(buf + cnt, text + RU16(rec + 0x112), n);
            cnt = (uint16_t)(cnt + n);
            W8(buf + cnt, 0);
            cnt++;
            W8(buf + cnt, 0);
            W16(rec + 0x112, configFindNextLine(text, rec));
        }
    }
    if ((int16_t)configGotoText(name, type, text, rec) == 0) {
        W16(gConfigError, 0x296a);
        return 0x296a;
    }
    uint16_t ws = (uint16_t)configScanWhitespace(text, rec);
    W16(rec + 0x112, ws);
    uint32_t start = text + ws;
    uint8_t c = R8(start);
    int8_t quote = (c == '\'' || c == '"') ? (int8_t)R8(start) : 0;
    uint32_t qend = quote ? tc_strchr(start + 1, (uint32_t)(int32_t)quote) : 0;
    uint32_t eol = tc_strchr(start, 0xd);
    if (eol == 0) eol = tc_strchr(start, 0);
    uint16_t len;
    if (qend == 0 || eol < qend) {
        len = (uint16_t)((uint16_t)eol - (uint16_t)start);
    } else {
        start++;
        len = (uint16_t)((uint16_t)qend - (uint16_t)start);
    }
    int16_t err;
    if (len >= size) { len = (uint16_t)(size - 1); err = 0x296b; }
    else err = 0;
    tc_memcpy(buf, start, len);
    W8(buf + len, 0);
    W16(gConfigError, err);
    return (uint16_t)err;
}

/* configWriteText is variadic and modifies its own argument slots: push its arguments
 * like the original and enter it through its 68k entry. vals[0] is pushed first. */
static uint32_t call_write_text(maddr sp0) {
    emu_set_sp(sp0);
    return call68_addr(fn_address(FN_configWriteText__DUsUsUsUse));
}

/* 000439d2: write a config value: name == 0 deletes section [type], value == 0 deletes
 * the name, else replaces or adds "name=value" (adding the section if needed) */
uint32_t configWrite(uint32_t value, uint32_t name, uint32_t type, uint32_t spec) {
    ENTRY_LINK(FN_configWrite, 0x10, 32);
    int16_t h = (int16_t)configFindFile(spec);
    if (h == 0) return RU16(gConfigError);
    uint32_t rec = memXlat(h);
    uint32_t text = memXlat(R16(rec + 0x114));
    maddr sp0 = emu_sp();
    uint32_t r;
    if (name == 0) {
        if ((int16_t)configGotoType(type, text, rec) == 0) {
            W16(gConfigError, 0x296a);
            return 0x296a;
        }
        uint16_t start = (uint16_t)configFindLineStart(text, rec);
        do {
            W16(rec + 0x112, configFindNextLine(text, rec));
        } while ((int16_t)configGotoNextName(text, rec) != 0);
        W16(sp0 - 2, 0);
        W16(sp0 - 4, (uint16_t)(RU16(rec + 0x112) - start));
        W16(sp0 - 6, start);
        W16(sp0 - 8, h);
        r = call_write_text(sp0 - 8);
    } else if (value == 0) {
        if ((int16_t)configGotoText(name, type, text, rec) == 0) {
            W16(gConfigError, 0x296a);
            return 0x296a;
        }
        W16(rec + 0x112, configFindLineStart(text, rec));
        W16(sp0 - 2, 0);
        emu_set_sp(sp0 - 2);
        uint16_t nl = (uint16_t)configFindNextLine(text, rec);
        W16(sp0 - 4, (uint16_t)(nl - RU16(rec + 0x112)));
        W16(sp0 - 6, RU16(rec + 0x112));
        W16(sp0 - 8, h);
        r = call_write_text(sp0 - 8);
    } else if ((int16_t)configGotoText(name, type, text, rec) != 0) {
        uint32_t eol = tc_strchr(text + RU16(rec + 0x112), 0xd);
        W32(sp0 - 4, value);
        W16(sp0 - 6, 1);
        uint32_t old;
        if (eol) old = (uint16_t)((uint16_t)eol - (uint16_t)(text + RU16(rec + 0x112)));
        else old = (uint32_t)RU16(rec + 0x110) - (uint32_t)RU16(rec + 0x112);
        W16(sp0 - 8, (uint16_t)old);
        W16(sp0 - 10, RU16(rec + 0x112));
        W16(sp0 - 12, h);
        r = call_write_text(sp0 - 12);
    } else if ((int16_t)configGotoType(type, text, rec) == 0) {
        /* new section: "[" type "]" CR name "=" value CR (strings in A5 data) */
        W32(sp0 - 4, GH(0x107232));
        W32(sp0 - 8, value);
        W32(sp0 - 12, GH(0x107234));
        W32(sp0 - 16, name);
        W32(sp0 - 20, GH(0x107236));
        W32(sp0 - 24, GH(0x107238));
        W32(sp0 - 28, type);
        W32(sp0 - 32, GH(0x10723a));
        W16(sp0 - 34, 8);
        W16(sp0 - 36, 0);
        W16(sp0 - 38, 0);
        W16(sp0 - 40, h);
        r = call_write_text(sp0 - 40);
    } else {
        /* new name at the start of the section */
        W32(sp0 - 4, GH(0x10723c));
        W32(sp0 - 8, value);
        W32(sp0 - 12, GH(0x10723e));
        W32(sp0 - 16, name);
        W16(sp0 - 18, 4);
        W16(sp0 - 20, 0);
        emu_set_sp(sp0 - 20);
        uint16_t nl = (uint16_t)configFindNextLine(text, rec);
        W16(sp0 - 22, nl);
        W16(sp0 - 24, h);
        r = call_write_text(sp0 - 24);
    }
    emu_set_sp(sp0);
    return (uint16_t)r;
}

/* 00043c40: configWriteText(h, offset, oldLen, count, char *s...): replace oldLen bytes
 * at offset of cached file h by the concatenated strings and write the file back. The
 * original advances its offset and count argument slots in place. */
static uint32_t configWriteText_args(maddr ap) {
    ENTRY_LINK(FN_configWriteText__DUsUsUsUse, 0x28, 32);
    (void)args_;
    maddr hP = ap, offP = ap + 2, oldP = ap + 4, cntP = ap + 6;
    uint32_t strs = ap + 8;
    uint16_t total = 0;
    for (uint32_t i = 0; i < (uint32_t)RU16(cntP); i++)
        total = (uint16_t)(total + tc_strlen(R32(strs + i * 4)));
    if (total == 0 && RU16(oldP) == 0) {
        W16(gConfigError, 0);
        return 0;
    }
    uint32_t rec = memXlat(R16(hP));
    uint32_t text = memXlat(R16(rec + 0x114));
    uint16_t old = RU16(oldP);
    if (total > old &&
        (uint32_t)total - (uint32_t)old + RU16(rec + 0x110) + 1 > 0x10000u) {
        W16(gConfigError, 0x296b);
        return 0x296b;
    }
    uint16_t tail = (uint16_t)(RU16(rec + 0x110) + 1 - (uint16_t)(RU16(offP) + RU16(oldP)));
    int16_t err;
    if (total > RU16(oldP)) {
        err = (int16_t)memSetHandleSize((uint32_t)RU16(offP) + total + tail, R16(rec + 0x114));
        if (err != 0) {
            W16(gConfigError, err);
            return (uint16_t)err;
        }
        rec = memLock(R16(hP));
        text = memLock(R16(rec + 0x114));
        uint32_t off = RU16(offP);
        uint32_t src = off + RU16(oldP) + text;
        uint32_t dst = off + total + text;
        tc_memmove(dst, src, tail);
    } else {
        uint32_t off = RU16(offP);
        uint32_t src = off + RU16(oldP) + text;
        uint32_t dst = off + total + text;
        tc_memcpy(dst, src, tail);
        memSetHandleSize((uint32_t)RU16(offP) + total + tail, R16(rec + 0x114));
        rec = memLock(R16(hP));
        text = memLock(R16(rec + 0x114));
    }
    for (;;) {
        uint16_t n = RU16(cntP);
        W16(cntP, n - 1);
        if (n == 0) break;
        uint16_t len = (uint16_t)tc_strlen(R32(strs));
        uint32_t s = R32(strs);
        strs += 4;
        tc_memcpy(text + RU16(offP), s, len);
        W16(offP, RU16(offP) + len);
    }
    W16(rec + 0x110, RU16(rec + 0x110) + (uint16_t)(total - RU16(oldP)));
    int16_t ref = (int16_t)fileOpen(2, rec + 4);
    if (ref == 0) {
        memUnlock(R16(rec + 0x114));
        memUnlock(R16(hP));
        err = (int16_t)fileGetError();
        W16(gConfigError, err);
        return (uint16_t)err;
    }
    err = (int16_t)fileSetEOF((uint32_t)RU16(rec + 0x110), ref);
    if (err == 0) {
        W32(a6 - 0x18, (uint32_t)RU16(rec + 0x110));
        err = (int16_t)fileWrite(a6 - 0x18, text, ref);
    }
    fileClose(ref);
    memUnlock(R16(rec + 0x114));
    memUnlock(R16(hP));
    W16(gConfigError, err);
    return (uint16_t)err;
}
void glue_configWriteText__DUsUsUsUse(void) { emu_set_d0(configWriteText_args(emu_sp())); }
uint32_t configWriteText__DUsUsUsUse(int16_t h, int16_t off, int16_t oldLen, int16_t count) {
    /* C entry without strings (count must be 0): build the argument block on the stack */
    maddr sp0 = emu_sp();
    W16(sp0 - 8, h); W16(sp0 - 6, off); W16(sp0 - 4, oldLen); W16(sp0 - 2, count);
    uint32_t r = call_write_text(sp0 - 8);
    emu_set_sp(sp0);
    return r;
}
