/* CODE 20 "Mohawk Libs 1", second half (Ghidra 0x5d8be-0x5fb3e): the portable audio layer
 * of the Mohawk library, on top of the MMSYSTEM-like midiOut* / waveOut* API of CODE 11.
 *
 *  - midiMap*: the MIDI mapper. A table of MIDI devices lives in the client bytes of the
 *    Mohawk OS layer (offset gMidiMapBytes): word "open", long resource file of the patch
 *    maps, then one device record pointer per midiOut device. Each device record (0x4e
 *    bytes, memAllocPtr) holds the MIDIOUTCAPS, the patch-map number read from the config
 *    file, the per-channel mute flags, the open count and the midiOut handle. midiMapOpen
 *    gives a "handle" (0x81c bytes) that counts the notes held on each channel/key, so that
 *    a reset or a mute can send the matching note-offs.
 *  - mmaudio*: start/stop of the sound system (HS_SetupSystem with the voice counts and
 *    the output quality read from the config file), and pause/resume.
 *  - waveObj (Symantec C++ class, Pascal-convention methods): a digitised sound from a
 *    'MHWK'/'WAVE' resource ('Data' and 'Cue#' chunks), cut into blocks at the cue points
 *    and the loop points, played through waveOut* (one WAVEHDR per block).
 *
 * Ported from the Ghidra decompilation, checked against the disassembly (ground truth);
 * addresses in comments are Ghidra addresses (see docs/PORTING.md). */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* ---- MIDI mapper ---- */
#define gMidiMapInits   (-0x13e)       /* A5 off, word: midiMapInit count */
#define gMidiMapNDevs   (-0x13c)       /* A5 off, word: number of midiOut devices */
#define gMidiMapBytes   (-0x13a)       /* A5 off, word: offset of the mapper's client bytes:
                                        *   +0 word open, +2 long patch-map resource file,
                                        *   +6 long device record per midiOut device */
#define gMidiMapType    (-0x144)       /* A5 off, long: resource type of the patch maps */
#define gMidiMapCfgFile (-0x16c)       /* A5 off, C string: config file name */
#define gMidiMapSection (-0x160)       /* A5 off, C string: config section of the devices */
#define gMidiMapResFile (-0x150)       /* A5 off, C string: patch-map resource file name */
/* device record (0x4e): +0 device id, +2 MIDIOUTCAPS (0x32; name at +8), +0x28 technology,
 * +0x34 patch-map number, +0x36 mute flag per channel (16 bytes), +0x46 open count,
 * +0x48 midiOut handle (word), +0x4a first open handle.
 * handle (0x81c): +0 device record, +4 next, +8 prev, +0xc mute flag per channel,
 * +0x1c note-on count per channel (16 x 128 bytes). */

/* ---- audio ---- */
#define gAudioError     (-0x172)       /* A5 off, word: last audio error (0x29cc...) */
#define gAudioBytes     (-0x16e)       /* A5 off, word: offset of the audio client bytes */
#define gAudioObjVtbl   (-0x1b6)       /* A5 off: vtable of the base class audioObj */
#define gWaveObjVtbl    (-0x44)        /* A5 off: vtable of waveObj */
#define gMMAudioPaused  (-0x46)        /* A5 off, word: mmaudioEnable(0) nesting count */
#define gMMAudioInits   (-0x48)        /* A5 off, word: mmaudioInit count */
/* config strings of mmaudioInit: (file, section, name) for the 5 settings */
static const int16_t cfg_file[5]    = { -0x138, -0x10e, -0xe4, -0xb2, -0x7e };
static const int16_t cfg_name[5]    = { -0x11a, -0xf0, -0xc6, -0x94, -0x60 };
static const int16_t cfg_section[5] = { -0x12c, -0x102, -0xd8, -0xa6, -0x72 };
#define CFG_FILE(i)     GA(cfg_file[i])
#define CFG_NAME(i)     GA(cfg_name[i])
#define CFG_SECTION(i)  GA(cfg_section[i])

/* waveObj (0xb8 bytes + 0x2c per block, in a locked handle):
 *  +0 vtable, +8 own handle (long), +c 'MHWK' resource handle, +e client, +12 device flag,
 *  +14 paused, +16 playing, +18 prepared, +1a wave device, +1c/+20 start arguments,
 *  +24 resource data, +28 resource size, +2c 'Cue#' chunk, +30 position (frames),
 *  +34 bytes per frame, +36 frames per ms (Fixed), +3a ms per frame (Fixed),
 *  +3e waveOut handle, +40 frame at the device's position 0, +44 stopping,
 *  +46 started past the loop, +48 loop released, +4a frames skipped by the release,
 *  +4e loop block, +52 WAVEHDR (0x20) of the loop tail (flags at +62),
 *  +72 rate, +74 frames, +78 bits, +79 channels, +7a flags, +7c loop count,
 *  +7e loop start, +82 loop end, +86 samples, +8a block count, +8c blocks.
 * block (0x2c): +0 first frame, +4 frames, +8 cue point, +c WAVEHDR (flags at +1c,
 *  loops at +20, user at +18), flag 1 = before the start, 4 = loop start, 8 = loop end. */
#define WOBJ_BLOCK(o, i) ((maddr)(o) + 0x8c + (uint32_t)(uint16_t)(i) * 0x2c)

/* Pascal virtual call (Symantec C++ method): reserve `slot` result bytes, push the
 * arguments (callee pops them), call, read the result and restore SP. */
static maddr vbegin(int slot) { maddr s = emu_sp(); emu_set_sp(s - (uint32_t)slot); return s; }
static uint32_t vend(maddr s, int slot, maddr self, int off) {
    call68_addr(R32(R32(self) + (uint32_t)off));
    uint32_t r = slot == 2 ? RU16(s - 2) : slot == 4 ? R32(s - 4) : 0;
    emu_set_sp(s);
    return r;
}
/* this->notify(&event) (vtable +0x1c): the event is 8 bytes on the caller's frame, only
 * its first byte is set */
static void wobj_notify(maddr self, maddr ev, uint8_t code) {
    W8(ev, code);
    maddr s = vbegin(0);
    push32(ev); push32(self);
    vend(s, 0, self, 0x1c);
}

/* fileSpec(const fileSpec &dir, const char *name) (CODE 21, C convention): the fileSpec
 * argument is passed by value (0x106 bytes on the stack). The caller has already pushed
 * the name and the copy; this pushes `this` and calls it. Returns D0 (this). */
static uint32_t fileSpec_ctor_dir(maddr self) {
    push32(self);
    return call68_fn(FN___ct__8fileSpecDC8fileSpecPCc);
}

/* The midiOut and waveOut functions of CODE 11 are Pascal functions with hand-written
 * glue ("conv": "manual"): their result is only in the slot above the arguments. They are
 * called like the original does: result slot, arguments (fmt as call_ptr, in prototype
 * order), jsr through the jump table; returns the slot. */
#include <stdarg.h>
static uint16_t mmcall(int fn, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = (int)strlen(fmt);
    uint32_t v[8];
    for (int i = 0; i < n && i < 8; i++) v[i] = va_arg(ap, uint32_t);
    va_end(ap);
    maddr s = emu_sp() - 2;
    emu_set_sp(s);
    for (int i = n - 1; i >= 0; i--) { if (fmt[i] == 'w') push16((uint16_t)v[i]); else push32(v[i]); }
    call68_addr(JT_ADDR(fn));
    uint16_t r = RU16(s);
    emu_set_sp(s + 2);
    return r;
}
#define MM_HDR(fn, size, hdr, dev) mmcall(FN_##fn, "wlw", (uint32_t)(uint16_t)(size), (uint32_t)(hdr), (uint32_t)(uint16_t)(dev))
#define MM_DEV(fn, dev)            mmcall(FN_##fn, "w", (uint32_t)(uint16_t)(dev))

/* ======================================================================== */

/* 0005d8be: midiMapGetDevice(&dev, id): the device record of midiOut device `id`, made on
 * first use (MIDIOUTCAPS, patch-map number from the config file) */
uint32_t midiMapGetDevice(uint32_t pdev, int16_t id) {
    ENTRY_LINK(FN_midiMapGetDevice, 0x438, 28);       /* link -0x438; movem D3-D6/A2-A4 */
    uint16_t dev = (uint16_t)id;
    if (dev >= GU16(gMidiMapNDevs)) return 2;
    W32(a6 - 4, 0xffffffff);
    maddr base = osGetClientBytes(0xffffffff);
    maddr tab = base + GU16(gMidiMapBytes);
    maddr slot = tab + 6 + (uint32_t)dev * 4;
    uint32_t d = R32(slot);
    W32(pdev, d);
    if (d != 0) return 0;
    d = memAllocPtr(0, 0x4e);
    W32(pdev, d);
    if (d == 0) return 7;
    tc_memset(R32(pdev), 0, 0x4e);
    W16(R32(pdev), dev);
    int16_t err = S16(midiOutGetDevCaps(0x32, R32(pdev) + 2, (int16_t)dev));
    if (err != 0) {
        memFreePtr(R32(pdev));
        return (uint16_t)err;
    }
    W32(tab + 6 + (uint32_t)dev * 4, R32(pdev));
    if (R16(R32(pdev) + 0x28) == 1) return 0;
    /* fileSpec launch; fileSpec spec(launch, cfgfile) — the copy of `launch` passed by
     * value stays on the stack (only 8 bytes are popped) until the end */
    __ct__8fileSpecDv(a6 - 0x438);
    fileGetLaunchDir(a6 - 0x438);
    maddr s0 = emu_sp();
    push32(GA(gMidiMapCfgFile));
    maddr copy = s0 - 0x10a;
    emu_set_sp(copy);
    __ct__8fileSpecFRC8fileSpec(copy, a6 - 0x438);
    emu_set_sp(copy);
    fileSpec_ctor_dir(a6 - 0x330);
    emu_set_sp(s0 - 0x106);
    if (configRead(0x200, a6 - 0x228, 0, GA(gMidiMapSection), a6 - 0x330) == 0) {
        W32(a6 - 0x28, a6 - 0x228);
        while (R8(R32(a6 - 0x28)) != 0) {
            int16_t num = (int16_t)tc_atoi(R32(a6 - 0x28));
            if (num != 0
                && configRead(0x20, a6 - 0x24, R32(a6 - 0x28), GA(gMidiMapSection), a6 - 0x330) == 0) {
                uint32_t n = tc_strlen(R32(pdev) + 8);
                if (S16(memicmp_(a6 - 0x24, R32(pdev) + 8, n)) == 0) {
                    W16(R32(pdev) + 0x34, num);
                    break;
                }
            }
            uint32_t n = tc_strlen(R32(a6 - 0x28));
            W32(a6 - 0x28, R32(a6 - 0x28) + n + 1);
        }
    }
    __dt__8fileSpecDv(a6 - 0x330);
    __dt__8fileSpecDv(a6 - 0x438);
    return 0;
}

/* 0005daae: fileSpec copy constructor (0x106 bytes); returns this */
uint32_t __ct__8fileSpecFRC8fileSpec(uint32_t self, uint32_t src) {
    mcopy(self, src, 0x106);
    return self;
}

/* 0005dafa: midiMapGetID(&id, handle): the midiOut device id of an open handle */
uint32_t midiMapGetID(uint32_t pid, uint32_t h) {
    if (h == 0) return 5;
    W16(pid, R16(R32(h)));
    return 0;
}

/* 0005db52: midiMapInit(): device table in the client bytes, patch-map resource file */
uint32_t midiMapInit(void) {
    ENTRY_LINK(FN_midiMapInit, 0x214, 16);            /* link -0x214; movem D3/A2-A4 */
    if (G16(gMidiMapInits) == 0) {
        /* midiOutGetNumDevs stores its result in the caller's slot */
        maddr s = emu_sp() - 2;
        emu_set_sp(s);
        call68_addr(JT_ADDR(FN_midiOutGetNumDevs));
        uint16_t n = RU16(s);
        emu_set_sp(s + 2);
        SG16(gMidiMapNDevs, n);
        uint16_t off = (uint16_t)osReserveClientBytes((int16_t)(GU16(gMidiMapNDevs) * 4 + 6));
        SG16(gMidiMapBytes, off);
        if (off == 0) return 7;
    }
    maddr tab = osGetClientBytes(0xffffffff) + GU16(gMidiMapBytes);
    tc_memset(tab, 0, (uint32_t)GU16(gMidiMapNDevs) * 4 + 6);
    __ct__8fileSpecDv(a6 - 0x214);
    fileGetLaunchDir(a6 - 0x214);
    /* resOpenFile(fileSpec(launch, resfile)): the result slot is reserved first */
    maddr s0 = emu_sp();
    emu_set_sp(s0 - 4);
    push32(GA(gMidiMapResFile));
    emu_set_sp(emu_sp() - 0x106);
    mcopy(emu_sp(), a6 - 0x214, 0x106);
    uint32_t spec = fileSpec_ctor_dir(a6 - 0x10c);
    emu_set_sp(s0);
    W32(tab + 2, resOpenFile(spec));
    midiMapGetDevice(a6 - 4, 0);
    tab = osGetClientBytes(0xffffffff) + GU16(gMidiMapBytes);
    W16(tab, 1);
    SG16(gMidiMapInits, G16(gMidiMapInits) + 1);
    __dt__8fileSpecDv(a6 - 0x10c);
    __dt__8fileSpecDv(a6 - 0x214);
    return 0;
}

/* 0005dc6a: midiMapLongMsg(size, hdr, handle): sysex; the Mohawk sysex F0 00 00 34 ...
 * mutes/unmutes a channel of the handle (1/2) or of the device (3/4) */
uint32_t midiMapLongMsg(int16_t size, uint32_t hdr, uint32_t h) {
    ENTRY_LINK(FN_midiMapLongMsg, 0xc, 24);           /* link -0xc; movem D3-D5/A2-A4 */
    if ((R32(hdr + 0x10) & 2) == 0) return 0x40;
    W32(a6 - 4, h);
    W32(a6 - 0xc, h);
    if (h == 0) return 5;
    W32(a6 - 8, R32(hdr));
    maddr p = R32(a6 - 8);
    W32(a6 - 8, p + 1);
    if (R8(p) == 0xf0) {
        maddr q = R32(a6 - 8);
        if (R8(q) == 0 && R8(q + 1) == 0 && R8(q + 2) == 0x34) {
            q = R32(a6 - 8);
            if (R8(q + 3) == 0 || (uint16_t)R8(q + 3) == RU16(R32(R32(a6 - 0xc)) + 0x34)) {
                switch (R8(q + 4)) {
                case 1: W8(R32(a6 - 0xc) + 0xc + (R8(R32(a6 - 8) + 5) & 0xf), 1); break;
                case 2: W8(R32(a6 - 0xc) + 0xc + (R8(R32(a6 - 8) + 5) & 0xf), 0); break;
                case 3: W8(R32(R32(a6 - 0xc)) + 0x36 + (R8(R32(a6 - 8) + 5) & 0xf), 1); break;
                case 4: W8(R32(R32(a6 - 0xc)) + 0x36 + (R8(R32(a6 - 8) + 5) & 0xf), 0); break;
                }
            }
            W32(hdr + 0x10, R32(hdr + 0x10) | 1);
            return 0;
        }
    }
    int16_t r = S16(MM_HDR(midiOutLongMsg, size, hdr, R16(R32(R32(a6 - 0xc)) + 0x48)));
    if (r == 0)
        while (R32(hdr + 0x10) & 1) { }
    return (uint16_t)r;
}

/* 0005de12: midiMapMuteChannel(mute, channel, device): when a channel is muted, the notes
 * held on it by every open handle are released */
uint32_t midiMapMuteChannel(int16_t mute, int16_t chan, int16_t dev) {
    ENTRY_LINK(FN_midiMapMuteChannel, 8, 28);         /* link -8; movem D3-D7/A2-A3 */
    uint16_t ch = (uint16_t)chan;
    if (ch >= 0x10) return 0xb;
    int16_t err = S16(midiMapGetDevice(a6 - 8, dev));
    if (err != 0) return (uint16_t)err;
    uint16_t old = R8(R32(a6 - 8) + 0x36 + ch);
    W16(a6 - 4, old);
    uint8_t m = (uint8_t)mute;
    W8(R32(a6 - 8) + 0x36 + ch, m);
    if (m == 0 && old != 0 && R16(R32(a6 - 8) + 0x46) != 0) {
        maddr h = R32(R32(a6 - 8) + 0x4a);
        do {
            midiMapResetChannel(ch, h);
            h = R32(h + 4);
        } while (h != R32(R32(a6 - 8) + 0x4a));
    }
    return 0;
}

/* 0005dee0: midiMapOpen(p4, p8, p12, device, &handle): opens the midiOut device on first
 * use and sends its patch map (sysex resource), then links a new handle */
uint32_t midiMapOpen(uint32_t p4, uint32_t p8, uint32_t p12, int16_t dev, uint32_t phandle) {
    ENTRY_LINK(FN_midiMapOpen, 0x34, 32);             /* link -0x34; movem D3-D7/A2-A4 */
    (void)p8; (void)p12;
    if (p4 != 0) return 0xb;
    int16_t err = S16(midiMapGetDevice(a6 - 0x30, dev));
    W16(a6 - 0x34, err);
    if (err != 0) return (uint16_t)err;
    maddr h = memAllocPtr(0, 0x81c);
    if (h == 0) return 7;
    maddr d = R32(a6 - 0x30);
    if (R16(d + 0x46) == 0) {
        maddr ph = d + 0x48;
        err = S16(mmcall(FN_midiOutOpen, "lllwl", 0, 0, 0, (uint32_t)(uint16_t)dev, ph));
        W16(a6 - 0x34, err);
        if (err != 0) {
            memFreePtr(h);
            return RU16(a6 - 0x34);
        }
    }
    memPageLock(R32(a6 - 0x30));
    W16(R32(a6 - 0x30) + 0x46, R16(R32(a6 - 0x30) + 0x46) + 1);
    W32(a6 - 4, 0xffffffff);
    maddr tab = osGetClientBytes(0xffffffff) + GU16(gMidiMapBytes);
    W32(a6 - 0x2c, tab);
    if (R32(tab + 2) != 0) {
        W32(a6 - 0x28, resFind(0, R16(R32(a6 - 0x30) + 0x34), G32(gMidiMapType)));
        if (R32(a6 - 0x28) != 0) {
            W16(a6 - 0x24, resLoad(R32(a6 - 0x28)));
            if (R16(a6 - 0x24) != 0) {
                /* send the patch map: a MIDIHDR at a6-0x20 */
                tc_memset(a6 - 0x20, 0, 0x1c);
                W32(a6 - 0x20, memLock(R16(a6 - 0x24)));
                W32(a6 - 0x1c, memGetHandleSize(R16(a6 - 0x24)));
                if (S16(MM_HDR(midiOutPrepareHeader, 0x1c, a6 - 0x20, R16(R32(a6 - 0x30) + 0x48))) == 0) {
                    if (S16(MM_HDR(midiOutLongMsg, 0x1c, a6 - 0x20, R16(R32(a6 - 0x30) + 0x48))) == 0)
                        while ((R32(a6 - 0x10) & 1) == 0) { }
                    MM_HDR(midiOutUnprepareHeader, 0x1c, a6 - 0x20, R16(R32(a6 - 0x30) + 0x48));
                }
                memUnlock(R16(a6 - 0x24));
                resSetPurgeable(1, R32(a6 - 0x28));
            }
        }
    }
    memPageLock(h);
    tc_memset(h, 0, 0x81c);
    W32(h, R32(a6 - 0x30));
    uint32_t first = R32(R32(a6 - 0x30) + 0x4a);
    W32(h + 4, first);
    if (first != 0) {
        W32(h + 8, R32(R32(R32(a6 - 0x30) + 0x4a) + 8));
        W32(R32(h + 8) + 4, h);
        W32(R32(R32(a6 - 0x30) + 0x4a) + 8, h);
    } else {
        W32(h + 8, h);
        W32(h + 4, h);
    }
    W32(R32(a6 - 0x30) + 0x4a, h);
    W32(phandle, h);
    return 0;
}

/* 0005e134: midiMapPrepareHeader(size, hdr, handle) */
uint32_t midiMapPrepareHeader(int16_t size, uint32_t hdr, uint32_t h) {
    ENTRY_LINK(FN_midiMapPrepareHeader, 4, 20);       /* link -4; movem D3-D4/A2-A4 */
    W32(a6 - 4, h);
    if (h == 0) return 5;
    return (uint16_t)MM_HDR(midiOutPrepareHeader, size, hdr, R16(R32(h) + 0x48));
}

/* 0005e1a2: midiMapQuit(): resets and closes every handle, frees the device records and
 * closes the patch-map file (D0 is garbage, unused by audioQuit: "ret": 0) */
uint32_t midiMapQuit(void) {
    ENTRY_LINK(FN_midiMapQuit, 4, 16);                /* link -4; movem D3/A2-A4 */
    if (G16(gMidiMapInits) == 0) return 0;
    W32(a6 - 4, 0xffffffff);
    maddr tab = osGetClientBytes(0xffffffff) + GU16(gMidiMapBytes);
    if (R16(tab) == 0) return 0;
    for (int32_t i = 0; i < (int32_t)GU16(gMidiMapNDevs); i++) {
        maddr d = R32(tab + 6 + (uint32_t)i * 4);
        if (d == 0) continue;
        while (R16(d + 0x46) != 0) {
            midiMapReset(R32(d + 0x4a));
            midiMapClose(R32(d + 0x4a));
        }
        memFreePtr(d);
    }
    if (R32(tab + 2) != 0) resCloseFile(0, R32(tab + 2));
    W16(tab, 0);
    SG16(gMidiMapInits, G16(gMidiMapInits) - 1);
    return 0;
}

/* 0005e262: midiMapReset(handle): releases the held notes of the 16 channels */
uint32_t midiMapReset(uint32_t h) {
    ENTRY_LINK(FN_midiMapReset, 0, 20);               /* link 0; movem D3-D4/A2-A4 */
    if (h == 0) return 5;
    for (int32_t ch = 0; ch < 0x10; ch++) midiMapResetChannel((uint32_t)ch, h);
    return 0;
}

/* 0005e2c2: midiMapResetChannel(channel, handle): a note-off for each held note */
uint32_t midiMapResetChannel(uint32_t ch, uint32_t h) {
    ENTRY_LINK(FN_midiMapResetChannel, 0, 20);        /* link 0; movem D3-D5/A2-A3 */
    for (int32_t key = 0; key < 0x80; key++) {
        while (R8(h + 0x1c + (ch << 7) + (uint32_t)key) != 0) {
            mmcall(FN_midiOutShortMsg, "lw", ((uint32_t)key << 8) + (ch | 0x80), (uint32_t)RU16(R32(h) + 0x48));
            maddr c = h + 0x1c + (ch << 7) + (uint32_t)key;
            W8(c, R8(c) - 1);
        }
    }
    return 0;
}

/* 0005e34e: midiMapShortMsg(msg, handle): counts the notes held per channel/key; muted
 * channels and unbalanced note-offs are dropped */
uint32_t midiMapShortMsg(uint32_t msg, uint32_t h) {
    ENTRY_LINK(FN_midiMapShortMsg, 4, 24);            /* link -4; movem D3-D5/A2-A4 */
    W32(a6 - 4, h);
    if (h == 0) return 5;
    uint32_t st = msg & 0xf0;
    int on = 0;
    if (st == 0x90) on = ((msg >> 16) & 0x7f) != 0;
    else if (st != 0x80) goto send;
    {
        uint32_t ch = msg & 0xf;
        if (R8(h + 0xc + ch) != 0 || R8(R32(h) + 0x36 + ch) != 0) return 0;
        uint32_t key = (msg & 0xffff) >> 8;
        maddr c = h + 0x1c + (ch << 7) + key;
        if (on) {
            uint8_t n = (uint8_t)(R8(c) + 1);
            W8(c, n);
            if (n != 0) goto send;
            W8(h + 0x1c + (ch << 7) + key, 0xff);
            return 0;
        } else {
            uint8_t n = R8(c);
            W8(c, n - 1);
            if (n != 0) goto send;
            W8(h + 0x1c + (ch << 7) + key, 0);
            return 0;
        }
    }
send:
    return mmcall(FN_midiOutShortMsg, "lw", msg, (uint32_t)RU16(R32(h) + 0x48));
}

/* 0005e4be: midiMapUnprepareHeader(size, hdr, handle) */
uint32_t midiMapUnprepareHeader(int16_t size, uint32_t hdr, uint32_t h) {
    ENTRY_LINK(FN_midiMapUnprepareHeader, 4, 20);     /* link -4; movem D3-D4/A2-A4 */
    W32(a6 - 4, h);
    if (h == 0) return 5;
    return (uint16_t)MM_HDR(midiOutUnprepareHeader, size, hdr, R16(R32(h) + 0x48));
}

/* 0005e52e: mmaudioEnable(on): nested pause (0) / resume (1) of the sound system;
 * returns 1 when the pause/resume call failed */
uint32_t mmaudioEnable(int16_t on) {
    ENTRY_LINK(FN_mmaudioEnable, 0, 4);               /* link 0; move.l D3 */
    if (on != 0) {
        if (G16(gMMAudioPaused) == 0) return 0;
        SG16(gMMAudioPaused, G16(gMMAudioPaused) - 1);
        if (G16(gMMAudioPaused) != 0) return 0;
        if (S16(seg11_03ebfc()) != 0) {
            SG16(gMMAudioPaused, 1);
            return 1;
        }
        return 0;
    }
    if (G16(gMMAudioPaused) == 0 && S16(FUN_0003ebf6()) != 0) return 1;
    SG16(gMMAudioPaused, G16(gMMAudioPaused) + 1);
    return 0;
}

/* 0005e5aa: mmaudioInit(): voices and output quality from the config file, then
 * HS_SetupSystem(voices, max voices, file voices, quality); returns 1 on failure */
uint32_t mmaudioInit(void) {
    ENTRY_LINK(FN_mmaudioInit, 0x644, 20);            /* link -0x644; movem D3-D7 */
    static const int32_t spec_off[5] = { -0x53c, -0x434, -0x32c, -0x224, -0x11c };
    maddr buf = a6 - 0x644;
    int16_t voices = 6, filevoices = 1, maxvoices = 7;
    W16(a6 - 0x544, 1);                                /* quality */
    if (S16(configQueryInit()) != 0) {
        W16(a6 - 0x540, 0);                            /* output flags */
        for (int i = 0; i < 5; i++) {
            /* configRead(0x100, buf, name, section, fileSpec(file)): the result slot is
             * reserved before the fileSpec is built */
            maddr s0 = emu_sp();
            emu_set_sp(s0 - 2);
            uint32_t spec = __ct__8fileSpecDPCc(a6 + spec_off[i], CFG_FILE(i));
            emu_set_sp(s0);
            int16_t err = S16(configRead(0x100, buf, CFG_NAME(i), CFG_SECTION(i), spec));
            W16(a6 - 0x542, err);
            if (err != 0) continue;
            switch (i) {
            case 0: voices = (int16_t)tc_atoi(buf); break;
            case 1: filevoices = (int16_t)tc_atoi(buf); break;
            case 2: {
                int32_t v = (int32_t)tc_atoi(buf);
                int32_t sum = (int32_t)voices + (int32_t)filevoices;
                maxvoices = (int16_t)(v < sum ? (int32_t)tc_atoi(buf) : sum);
                break;
            }
            case 3:                                    /* "mono"/"ste"(reo) */
                if (S16(strnicmp_(buf, GA(-0x88), 4)) == 0) W16(a6 - 0x540, R16(a6 - 0x540) | 0x20);
                else if (S16(strnicmp_(buf, GA(-0x82), 3)) == 0) W16(a6 - 0x540, R16(a6 - 0x540) | 0x10);
                break;
            case 4:                                    /* sample size */
                if (S16(strnicmp_(buf, GA(-0x52), 3)) == 0) W16(a6 - 0x540, R16(a6 - 0x540) | 1);
                else if (S16(strnicmp_(buf, GA(-0x4e), 4)) == 0) W16(a6 - 0x540, R16(a6 - 0x540) | 2);
                break;
            }
        }
        int32_t f = R16(a6 - 0x540);
        W32(a6 - 0x10, (uint32_t)f);
        if ((f & 0x10) && (R32(a6 - 0x10) & 2)) W16(a6 - 0x544, 6);
        else {
            f = R16(a6 - 0x540);
            W32(a6 - 0xc, (uint32_t)f);
            if ((f & 0x20) && (R32(a6 - 0xc) & 2)) W16(a6 - 0x544, 7);
            else {
                f = R16(a6 - 0x540);
                W32(a6 - 8, (uint32_t)f);
                if ((f & 0x10) && (R32(a6 - 8) & 1)) W16(a6 - 0x544, 4);
                else {
                    f = R16(a6 - 0x540);
                    W32(a6 - 4, (uint32_t)f);
                    if ((f & 0x20) && (R32(a6 - 4) & 1)) W16(a6 - 0x544, 5);
                    else if (R16(a6 - 0x540) & 0x10) W16(a6 - 0x544, 2);
                    else if (R16(a6 - 0x540) & 0x20) W16(a6 - 0x544, 3);
                }
            }
        }
        for (int i = 4; i >= 0; i--) __dt__8fileSpecDv(a6 + spec_off[i]);
    }
    if (S16(HS_SetupSystem((int16_t)(voices + filevoices), maxvoices, filevoices, R16(a6 - 0x544))) != 0)
        return 1;
    SG16(gMMAudioInits, G16(gMMAudioInits) + 1);
    return 0;
}

/* 0005e8aa: mmaudioQuit(): shuts the sound system down with the last mmaudioInit
 * (D0 is garbage, unused by the callers: "ret": 0) */
uint32_t mmaudioQuit(void) {
    ENTRY_LINK(FN_mmaudioQuit, 0, 0);                 /* link 0 */
    if (G16(gMMAudioInits) == 0) return 0;
    if (G16(gMMAudioInits) == 1) seg11_03ec02();
    SG16(gMMAudioInits, G16(gMMAudioInits) - 1);
    return 0;
}

/* 0005e8d8: waveObj::calcBlocks(): cuts the sound into blocks at the cue points and at
 * the loop start/end */
uint32_t calcBlocks__7waveObj(uint32_t self) {
    ENTRY_LINK(FN_calcBlocks__7waveObj, 0x20, 32);    /* link -0x20; movem D3-D7/A2-A4 */
    W16(self + 0x8a, 0);
    uint32_t loopStart = R16(self + 0x7c) != 0 ? R32(self + 0x7e) : 0xffffffff;   /* D4 */
    uint32_t loopEnd = 0xffffffff;                                                /* D5 */
    uint32_t nextCue = 0xffffffff;                                                /* D6 */
    /* registers the original leaves unset without a cue chunk (never used then) */
    maddr cue = cpu_get(R_A4);                                                    /* A4 */
    uint16_t ncue = (uint16_t)cpu_get(R_D7);                                      /* D7 */
    if (R32(self + 0x2c) != 0) {
        uint16_t n = RU16(R32(self + 0x2c) + 8);
        W16(a6 - 0x10, n);
        W16(a6 - 0x20, n);
        ncue = 0;
        cue = R32(self + 0x2c) + 0xa;
        nextCue = R32(cue);
        W32(a6 - 0xc, nextCue);
    }
    W32(a6 - 0x1c, 0);
    do {
        maddr blk = WOBJ_BLOCK(self, RU16(self + 0x8a));
        W32(a6 - 0x18, blk);
        W32(a6 - 0x14, R32(self + 0x74));
        tc_memset(blk, 0, 0x2c);
        uint32_t pos = R32(a6 - 0x1c);
        W32(R32(a6 - 0x18), pos);
        if (pos == nextCue) {
            W32(a6 - 0x14, R32(a6 - 0x1c));
            maddr b = R32(a6 - 0x18);
            W32(b + 4, 0);
            W32(b + 8, cue);
            cue = cue + (((uint32_t)R8(cue + 4) + 6) & 0xfffffffe);
            ncue++;
            if (ncue < RU16(a6 - 0x20)) { nextCue = R32(cue); W32(a6 - 8, nextCue); }
            else nextCue = 0xffffffff;
        } else {
            if (R32(a6 - 0x1c) == loopStart) {
                maddr b = R32(a6 - 0x18);
                W32(self + 0x4e, b);
                W32(b + 0x1c, R32(b + 0x1c) | 4);
                loopEnd = R32(self + 0x82);
                W32(a6 - 0x14, loopEnd);
                loopStart = 0xffffffff;
                W32(self + 0x4e, b);
            }
            if (loopStart <= R32(a6 - 0x14)) W32(a6 - 0x14, loopStart);
            if (nextCue <= R32(a6 - 0x14)) {
                W32(a6 - 0x14, nextCue);
                W32(R32(a6 - 0x18) + 8, cue);
                cue = cue + (((uint32_t)R8(cue + 4) + 6) & 0xfffffffe);
                ncue++;
                if (ncue < RU16(a6 - 0x20)) { nextCue = R32(cue); W32(a6 - 4, nextCue); }
                else nextCue = 0xffffffff;
            }
            if (loopEnd <= R32(a6 - 0x14)) {
                W32(a6 - 0x14, loopEnd);
                maddr b = R32(a6 - 0x18);
                W32(b + 0x1c, R32(b + 0x1c) | 8);
                loopEnd = 0xffffffff;
            }
        }
        W32(R32(a6 - 0x18) + 4, R32(a6 - 0x14) - R32(a6 - 0x1c));
        W32(a6 - 0x1c, R32(a6 - 0x14));
        W16(self + 0x8a, RU16(self + 0x8a) + 1);
    } while (R32(a6 - 0x1c) < R32(self + 0x74));
    return 0;
}

/* 0005eab6: waveObj::create(resource): builds a waveObj from a 'MHWK'/'WAVE' resource;
 * returns its handle, or 0 (error in gAudioError) */
uint32_t create__7waveObj(int16_t res) {
    ENTRY_LINK(FN_create__7waveObj, 0x6c, 32);        /* link -0x6c; movem D3-D7/A2-A4 */
    maddr data = memLockXtd(res);                      /* A3 */
    uint32_t size = R32(data + 4);
    W32(a6 - 0x60, size);
    size += 8;                                         /* D5 */
    uint32_t tag = R32(data);
    W32(a6 - 0x5c, tag);
    int ok = 0;
    if (tag == 0x4d48574b) {                           /* 'MHWK' */
        tag = R32(data + 8);
        W32(a6 - 0x58, tag);
        ok = tag == 0x57415645;                        /* 'WAVE' */
    }
    if (!ok) {
        memUnlock(res);
        W16(a6 - 0x54, 0x29d0);
        SG16(gAudioError, R16(a6 - 0x54));
        return 0;
    }
    int16_t hobj = S16(memAllocHandle(1, 0xb8));       /* D6 */
    if (hobj == 0) {
        memUnlock(res);
        int16_t e = S16(memGetError());
        W16(a6 - 0x52, e);
        SG16(gAudioError, e);
        return 0;
    }
    maddr p = memLockXtd(hobj);
    W32(a6 - 0x50, p);
    W32(a6 - 0x4c, 0xb8);
    maddr obj = tc_memset(p, 0, 0xb8);                 /* A4 */
    if (obj != 0) {                                    /* inlined constructors */
        W32(a6 - 0x48, obj);
        W32(a6 - 0x44, obj);
        W32(obj, GA(gAudioObjVtbl));
        W32(R32(a6 - 0x48), GA(gWaveObjVtbl));
    }
    W32(a6 - 0x6c, obj);
    W32(obj + 8, (uint16_t)hobj);
    W16(obj + 0xc, res);
    uint32_t client = osGetClient();
    W32(R32(a6 - 0x6c) + 0xe, client);
    W32(a6 - 0x40, client);
    maddr cb = osGetClientBytes(client) + GU16(gAudioBytes);
    W16(R32(a6 - 0x6c) + 0x12, R16(cb + 2));
    W32(R32(a6 - 0x6c) + 0x24, data);
    W32(R32(a6 - 0x6c) + 0x28, size);
    W32(a6 - 0x68, data + 0xc);
    do {
        maddr ck = R32(a6 - 0x68);
        uint32_t id = R32(ck);
        W32(a6 - 0x3c, id);
        if (id == 0x43756523) {                        /* 'Cue#' */
            if (R32(R32(a6 - 0x6c) + 0x2c) != 0) {
                W16(a6 - 0x38, 0x29d1);
                SG16(gAudioError, R16(a6 - 0x38));
                goto fail;
            }
            W32(R32(a6 - 0x6c) + 0x2c, R32(a6 - 0x68));
        } else if (id == 0x44617461) {                 /* 'Data' */
            if (R32(R32(a6 - 0x6c) + 0x86) != 0) {
                W16(a6 - 0x36, 0x29d1);
                SG16(gAudioError, R16(a6 - 0x36));
                goto fail;
            }
            ck = R32(a6 - 0x68);
            W32(a6 - 0x64, ck);
            maddr o = R32(a6 - 0x6c);
            W32(o + 0x86, ck + 0x1c);
            uint16_t w = RU16(ck + 8);   W16(a6 - 0x34, w); W16(o + 0x72, w);
            uint32_t l = R32(ck + 0xa);  W32(a6 - 0x30, l); W32(o + 0x74, l);
            W8(o + 0x78, R8(ck + 0xe));
            W8(o + 0x79, R8(ck + 0xf));
            w = RU16(ck + 0x10);         W16(a6 - 0x2c, w); W16(o + 0x7a, w);
            w = RU16(ck + 0x12);         W16(a6 - 0x2a, w); W16(o + 0x7c, w);
            l = R32(ck + 0x14);          W32(a6 - 0x28, l); W32(o + 0x7e, l);
            l = R32(ck + 0x18);          W32(a6 - 0x24, l); W32(o + 0x82, l);
            if ((R8(o + 0x78) & 7) != 0 || R32(o + 0x74) == 0 || (RU16(o + 0x7a) & 0x7fff) != 0
                || R8(o + 0x78) > 8) {
                W16(a6 - 0x20, 0x29d1);
                SG16(gAudioError, R16(a6 - 0x20));
                goto fail;
            }
            o = R32(a6 - 0x6c);
            W16(o + 0x34, (uint16_t)((uint32_t)(R8(o + 0x78) >> 3) * R8(o + 0x79)));
            W32(a6 - 0x1c, 1000);
            uint32_t rate = RU16(o + 0x72);
            W32(a6 - 0x18, rate);
            W32(o + 0x36, _FixDiv(rate, 1000));
            rate = RU16(o + 0x72);
            W32(a6 - 0x14, rate);
            W32(a6 - 0x10, 1000);
            W32(o + 0x3a, _FixDiv(1000, rate));
        }
        ck = R32(a6 - 0x68);
        uint32_t len = R32(ck + 4);
        W32(a6 - 0xc, len);
        W32(a6 - 0x68, ck + 8 + ((len + 1) & 0xfffffffe));
    } while (data + size > R32(a6 - 0x68));
    if (data + size < R32(a6 - 0x68) || R32(R32(a6 - 0x6c) + 0x86) == 0) {
        W16(a6 - 8, 0x29d0);
        SG16(gAudioError, R16(a6 - 8));
        goto fail;
    }
    {
        maddr o = R32(a6 - 0x6c);
        uint16_t nblk = R16(o + 0x7c) != 0 ? 3 : 1;
        if (R32(o + 0x2c) != 0) {
            uint16_t n = RU16(R32(o + 0x2c) + 8);
            W16(a6 - 6, n);
            nblk = (uint16_t)(nblk + n);
        }
        maddr np = memReallocPtr(0, (uint32_t)nblk * 0x2c + 0x8c, o);
        W32(a6 - 0x6c, np);
        if (np == 0) {
            int16_t e = S16(memGetError());
            W16(a6 - 4, e);
            SG16(gAudioError, e);
            goto fail;
        }
        calcBlocks__7waveObj(R32(a6 - 0x6c));
        maddr s = vbegin(2);                           /* this->seek(0) */
        push32(0); push32(R32(a6 - 0x6c));
        vend(s, 2, R32(a6 - 0x6c), 0x34);
        s = vbegin(0);                                 /* this->resetLoop() */
        push32(R32(a6 - 0x6c));
        vend(s, 0, R32(a6 - 0x6c), 0x2c);
        o = R32(a6 - 0x6c);
        memReallocPtr(0, (uint32_t)RU16(o + 0x8a) * 0x2c + 0x8c, o);
        return R32(R32(a6 - 0x6c) + 8);
    }
fail:
    memUnlock(hobj);
    memFreeHandle(hobj);
    memUnlock(res);
    return 0;
}

/* 0005eee4: waveObj::devicePrepare(): opens the wave device (WAVEFORMAT at a6-0x10) */
uint32_t devicePrepare__7waveObj(uint32_t self) {
    ENTRY_LINK(FN_devicePrepare__7waveObj, 0x10, 20); /* link -0x10; movem D3-D5/A2-A3 */
    W16(a6 - 0x10, 1);                                 /* format tag */
    W16(a6 - 0xe, R8(self + 0x79));                    /* channels */
    W32(a6 - 0xc, (uint32_t)RU16(self + 0x72) * R8(self + 0x79));       /* samples/s */
    W32(a6 - 8, (uint32_t)(R8(self + 0x78) >> 3) * R32(a6 - 0xc));       /* bytes/s */
    W16(a6 - 4, R16(self + 0x34));                     /* block align */
    W16(a6 - 2, R8(self + 0x78));                      /* bits */
    int16_t r = S16(mmcall(FN_waveOutOpen, "llllwl", 0x30000, self, JT_ADDR(FN_deviceCallback__7waveObj),
                           a6 - 0x10, (uint32_t)RU16(self + 0x1a), self + 0x3e));
    uint16_t err;
    if (r == 0) {
        memPageLock(R32(self + 0x24));
        err = 0;
    } else if (r == 4) err = 0x29cd;
    else if (r == 2) err = 0x29ce;
    else err = 0x29cc;
    SG16(gAudioError, err);
    return err;
}

/* 0005efda: waveObj::deviceStart(paused): queues the blocks from the current position
 * (and the rest of the loop first when starting inside it) */
uint32_t deviceStart__7waveObj(uint32_t self, int16_t paused) {
    ENTRY_LINK(FN_deviceStart__7waveObj, 4, 32);      /* link -4; movem D3-D7/A2-A4 */
    MM_DEV(waveOutReset, R16(self + 0x3e));
    MM_DEV(waveOutPause, R16(self + 0x3e));
    deviceUnprepareBlks__7waveObj(self);
    uint32_t pos = R32(self + 0x30);                   /* D5 */
    W32(self + 0x40, R32(self + 0x30));
    if (R16(self + 0x7c) != 0) {
        W16(self + 0x46, pos >= R32(self + 0x82) ? 1 : 0);
        W32(self + 0x4a, 0);
        if (pos > R32(self + 0x7e) && pos < R32(self + 0x82) && R32(R32(self + 0x4e) + 0x20) > 1) {
            tc_memset(self + 0x52, 0, 0x20);
            uint32_t bpf = RU16(self + 0x34);
            W32(self + 0x52, pos * bpf + R32(self + 0x86));
            bpf = RU16(self + 0x34);
            W32(self + 0x56, R32(self + 0x82) * bpf + R32(self + 0x86) - R32(self + 0x52));
            MM_HDR(waveOutPrepareHeader, 0x20, self + 0x52, R16(self + 0x3e));
            MM_HDR(waveOutWrite, 0x20, self + 0x52, R16(self + 0x3e));
            maddr lb = R32(self + 0x4e);
            W32(lb + 0x20, R32(lb + 0x20) - 1);
            pos = R32(self + 0x7e);
        }
    }
    for (uint16_t i = 0; i < RU16(self + 0x8a); i++) {
        maddr blk = WOBJ_BLOCK(self, i);
        W32(a6 - 4, blk);
        if (R32(blk) + R32(blk + 4) <= pos) {
            maddr b = R32(a6 - 4);
            W32(b + 0x1c, R32(b + 0x1c) | 1);
            continue;
        }
        maddr b = R32(a6 - 4);
        W32(b + 0x18, R32(a6 - 4));
        if (pos >= R32(b)) {
            uint32_t v = pos * (uint32_t)RU16(self + 0x34) + R32(self + 0x86);
            maddr b3 = R32(a6 - 4);
            W32(b3 + 0xc, v);
            uint32_t n = R32(b3 + 4) - (pos - R32(b3));
            W32(b3 + 0x10, n * (uint32_t)RU16(self + 0x34));
        } else {
            uint32_t v = R32(b) * (uint32_t)RU16(self + 0x34) + R32(self + 0x86);
            W32(b + 0xc, v);
            W32(b + 0x10, R32(b + 4) * (uint32_t)RU16(self + 0x34));
        }
        MM_HDR(waveOutPrepareHeader, 0x20, R32(a6 - 4) + 0xc, R16(self + 0x3e));
        MM_HDR(waveOutWrite, 0x20, R32(a6 - 4) + 0xc, R16(self + 0x3e));
    }
    if (paused == 0) MM_DEV(waveOutRestart, R16(self + 0x3e));
    SG16(gAudioError, 0);
    return 0;
}

/* 0005f240: waveObj::deviceStop(): pauses, accounts for the loops played, saves the
 * position and resets the device */
uint32_t deviceStop__7waveObj(uint32_t self) {
    ENTRY_LINK(FN_deviceStop__7waveObj, 8, 12);       /* link -8; movem D3/A2-A3 */
    MM_DEV(waveOutPause, R16(self + 0x3e));
    W16(a6 - 8, 2);                                    /* MMTIME: TIME_SAMPLES */
    MM_HDR(waveOutGetPosition, 8, a6 - 8, R16(self + 0x3e));
    uint32_t pos = R32(a6 - 6) + R32(self + 0x40);
    if (R16(self + 0x7c) != 0 && R16(self + 0x46) == 0 && R16(self + 0x48) == 0
        && pos >= R32(self + 0x7e)) {
        if (R16(self + 0x7c) != -1) {
            uint32_t d = R32(self + 0x82) - R32(self + 0x7e);
            if (d == 0) port_divide_by_zero(__func__);
            uint32_t loops = (pos - R32(self + 0x7e)) / d;
            maddr lb = R32(self + 0x4e);
            W32(lb + 0x20, R32(lb + 0x20) - loops);
        }
        if (R32(self + 0x62) & 2) {
            maddr lb = R32(self + 0x4e);
            W32(lb + 0x20, R32(lb + 0x20) + 1);
        }
    }
    W32(self + 0x30, getFrame__7waveObj(self));
    W16(self + 0x44, 1);
    MM_DEV(waveOutReset, R16(self + 0x3e));
    W16(self + 0x44, 0);
    return 0;
}

/* 0005f320: waveObj::deviceUnprepare(): closes the wave device */
uint32_t deviceUnprepare__7waveObj(uint32_t self) {
    ENTRY_LINK(FN_deviceUnprepare__7waveObj, 0, 8);   /* link 0; movem A2-A3 */
    memPageUnlock(R32(self + 0x24));
    deviceUnprepareBlks__7waveObj(self);
    MM_DEV(waveOutReset, R16(self + 0x3e));
    MM_DEV(waveOutClose, R16(self + 0x3e));
    return 0;
}

/* 0005f382: waveObj::deviceUnprepareBlks(): unprepares the prepared WAVEHDRs */
uint32_t deviceUnprepareBlks__7waveObj(uint32_t self) {
    ENTRY_LINK(FN_deviceUnprepareBlks__7waveObj, 4, 16);  /* link -4; movem D3-D4/A2-A3 */
    if (R32(self + 0x62) & 2) {
        MM_HDR(waveOutUnprepareHeader, 0x20, self + 0x52, R16(self + 0x3e));
        W32(self + 0x62, R32(self + 0x62) & 0xfffffffd);
    }
    for (uint16_t i = 0; i < RU16(self + 0x8a); i++) {
        if ((R32(WOBJ_BLOCK(self, i) + 0x1c) & 2) == 0) continue;
        W32(a6 - 4, i);
        MM_HDR(waveOutUnprepareHeader, 0x2c, WOBJ_BLOCK(self, i) + 0xc, R16(self + 0x3e));
        maddr b = WOBJ_BLOCK(self, (uint16_t)R32(a6 - 4));
        W32(b + 0x1c, R32(b + 0x1c) & 0xfffffffd);
    }
    return 0;
}

/* 0005f488: waveObj::getFrame(): the current frame (device position while playing,
 * folded into the loop) */
uint32_t getFrame__7waveObj(uint32_t self) {
    ENTRY_LINK(FN_getFrame__7waveObj, 8, 12);         /* link -8; movem D3/A2-A3 */
    if (R16(self + 0x12) == 0 || (R16(self + 0x14) == 0 && R16(self + 0x16) == 0))
        return R32(self + 0x30);
    W16(a6 - 8, 2);                                    /* MMTIME: TIME_SAMPLES */
    MM_HDR(waveOutGetPosition, 8, a6 - 8, R16(self + 0x3e));
    uint32_t pos = R32(a6 - 6) + R32(self + 0x40);
    if (R16(self + 0x7c) != 0 && pos > R32(self + 0x82)) {
        if (R16(self + 0x46) != 0 || R16(self + 0x48) != 0) {
            pos -= R32(self + 0x4a);
        } else {
            uint32_t d = R32(self + 0x82) - R32(self + 0x7e);
            if (d == 0) port_divide_by_zero(__func__);
            pos = (pos - R32(self + 0x7e)) % d + R32(self + 0x7e);
        }
    }
    return pos;
}

/* 0005f53a: waveObj::getPosition(): the position in ms (Fixed) */
uint32_t getPosition__7waveObj(uint32_t self) {
    ENTRY_LINK(FN_getPosition__7waveObj, 0, 16);      /* link 0; movem D3-D4/A2-A3 */
    uint32_t msPerFrame = R32(self + 0x3a);
    uint32_t f = getFrame__7waveObj(self);
    return _FixMul(f, msPerFrame);
}

/* 0005f58e: waveObj::pause() */
uint32_t pause__7waveObj(uint32_t self) {
    ENTRY_LINK(FN_pause__7waveObj, 8, 16);            /* link -8; movem D3-D4/A2-A3 */
    if (R16(self + 0x16) == 0) {
        SG16(gAudioError, 0x2a02);
        return 0x2a02;
    }
    MM_DEV(waveOutPause, R16(self + 0x3e));
    W16(self + 0x16, 0);
    W16(self + 0x14, 1);
    wobj_notify(self, a6 - 8, 2);
    SG16(gAudioError, 0);
    return 0;
}

/* 0005f60c: waveObj::prepare(device): devicePrepare() through the vtable; -1 = the
 * client's default device */
uint32_t prepare__7waveObj(uint32_t self, int16_t dev) {
    ENTRY_LINK(FN_prepare__7waveObj, 0, 28);          /* link 0; movem D3-D6/A2-A4 */
    if (R16(self + 0x18) != 0) {
        SG16(gAudioError, 0x2a06);
        return 0x2a06;
    }
    int16_t d = dev;
    if (d == -1) {
        maddr cb = osGetClientBytes(R32(self + 0xe));
        d = R16(cb + GU16(gAudioBytes) + 8);
    }
    W16(self + 0x1a, d);
    maddr s = vbegin(2);
    push32(self);
    uint16_t r = (uint16_t)vend(s, 2, self, 8);
    if (r == 0) {
        memPageLock(self);
        W16(self + 0x18, 1);
    }
    return r;
}

/* 0005f6b4: waveObj::releaseLoop(): lets a playing loop run out */
uint32_t releaseLoop__7waveObj(uint32_t self) {
    ENTRY_LINK(FN_releaseLoop__7waveObj, 8, 28);      /* link -8; movem D3-D7/A2-A3 */
    if (R16(self + 0x18) == 0) {
        SG16(gAudioError, 0x2a03);
        return 0x2a03;
    }
    int16_t intr = S16(osIntrEnable(0));
    if ((R16(self + 0x14) != 0 || R16(self + 0x16) != 0) && R16(self + 0x7c) != 0
        && R16(self + 0x46) == 0 && R16(self + 0x48) == 0) {
        MM_DEV(waveOutBreakLoop, R16(self + 0x3e));
        W16(a6 - 8, 2);                                /* MMTIME: TIME_SAMPLES */
        MM_HDR(waveOutGetPosition, 8, a6 - 8, R16(self + 0x3e));
        uint32_t pos = R32(a6 - 6) + R32(self + 0x40);
        W32(R32(self + 0x4e) + 0x20, 1);
        uint32_t end = R32(self + 0x82);
        uint32_t skipped = 0;
        if (pos > end) {
            uint32_t d = end - R32(self + 0x7e);
            if (d == 0) port_divide_by_zero(__func__);
            uint32_t loops = (pos - R32(self + 0x7e)) / d;
            skipped = loops * (end - R32(self + 0x7e));
        }
        W32(self + 0x4a, skipped);
        W16(self + 0x48, 1);
    }
    osIntrEnable(intr);
    SG16(gAudioError, 0);
    return 0;
}

/* 0005f7ac: waveObj::resetLoop(): loop count back to its initial value */
uint32_t resetLoop__7waveObj(uint32_t self) {
    if (R16(self + 0x7c) == 0) return 0;
    W16(self + 0x48, 0);
    uint32_t n = R16(self + 0x7c) == -1 ? 0xffffffff : (uint32_t)RU16(self + 0x7c) + 1;
    W32(R32(self + 0x4e) + 0x20, n);
    return 0;
}

/* 0005f80c: waveObj::resume() */
uint32_t resume__7waveObj(uint32_t self) {
    ENTRY_LINK(FN_resume__7waveObj, 8, 16);           /* link -8; movem D3-D4/A2-A3 */
    if (R16(self + 0x14) == 0) {
        SG16(gAudioError, 0x2a01);
        return 0x2a01;
    }
    MM_DEV(waveOutRestart, R16(self + 0x3e));
    W16(self + 0x14, 0);
    W16(self + 0x16, 1);
    wobj_notify(self, a6 - 8, 3);
    SG16(gAudioError, 0);
    return 0;
}

/* 0005f88a: waveObj::seek(ms): -1 = the end */
uint32_t seek__7waveObj(uint32_t self, uint32_t ms) {
    ENTRY_LINK(FN_seek__7waveObj, 8, 28);             /* link -8; movem D3-D7/A2-A3 */
    if (R16(self + 0x14) != 0) {
        SG16(gAudioError, 0x2a04);
        return 0x2a04;
    }
    if (R16(self + 0x16) != 0) {
        SG16(gAudioError, 0x2a05);
        return 0x2a05;
    }
    uint32_t f;
    if (ms == 0xffffffff) f = R32(self + 0x74);
    else {
        uint32_t fpms = R32(self + 0x36);
        W32(a6 - 8, ms);
        f = _FixMul(ms, fpms);
    }
    if (f > R32(self + 0x74)) {
        W32(self + 0x30, R32(self + 0x74));
        W16(a6 - 4, 0x29cf);
        SG16(gAudioError, R16(a6 - 4));
        return 0x29cf;
    }
    W32(self + 0x30, f);
    W16(a6 - 2, 0);
    SG16(gAudioError, 0);
    return 0;
}

/* 0005f950: waveObj::start(p8, p12): deviceStart(0) through the vtable */
uint32_t start__7waveObj(uint32_t self, uint32_t p8, uint32_t p12) {
    ENTRY_LINK(FN_start__7waveObj, 8, 32);            /* link -8; movem D3-D7/A2-A4 */
    if (R16(self + 0x18) == 0) {
        SG16(gAudioError, 0x2a03);
        return 0x2a03;
    }
    if (R16(self + 0x14) != 0) {
        SG16(gAudioError, 0x2a04);
        return 0x2a04;
    }
    if (R16(self + 0x16) != 0) {
        SG16(gAudioError, 0x2a05);
        return 0x2a05;
    }
    W32(self + 0x1c, p12);
    W32(self + 0x20, p8);
    maddr s = vbegin(2);
    push16(0); push32(self);
    uint16_t r = (uint16_t)vend(s, 2, self, 0xc);
    if (r == 0) {
        W16(self + 0x16, 1);
        wobj_notify(self, a6 - 8, 4);
    }
    return r;
}

/* 0005fa10: waveObj::stop(): deviceStop() through the vtable when the device is used */
uint32_t stop__7waveObj(uint32_t self) {
    ENTRY_LINK(FN_stop__7waveObj, 8, 20);             /* link -8; movem D3-D5/A2-A3 */
    if (R16(self + 0x18) == 0) {
        SG16(gAudioError, 0x2a03);
        return 0x2a03;
    }
    if (R16(self + 0x16) == 0 && R16(self + 0x14) == 0) {
        SG16(gAudioError, 0);
        return 0;
    }
    if (R16(self + 0x12) != 0) {
        maddr s = vbegin(0);
        push32(self);
        vend(s, 0, self, 0x10);
    }
    W16(self + 0x14, 0);
    W16(self + 0x16, 0);
    wobj_notify(self, a6 - 8, 5);
    SG16(gAudioError, 0);
    return 0;
}

/* 0005faae: waveObj::unprepare(): deviceUnprepare() through the vtable */
uint32_t unprepare__7waveObj(uint32_t self) {
    ENTRY_LINK(FN_unprepare__7waveObj, 0, 20);        /* link 0; movem D3-D6/A2 */
    if (R16(self + 0x18) == 0) {
        SG16(gAudioError, 0x2a03);
        return 0x2a03;
    }
    if (R16(self + 0x14) != 0) {
        SG16(gAudioError, 0x2a04);
        return 0x2a04;
    }
    if (R16(self + 0x16) != 0) {
        SG16(gAudioError, 0x2a05);
        return 0x2a05;
    }
    memPageUnlock(self);
    if (R16(self + 0x12) != 0) {
        maddr s = vbegin(0);
        push32(self);
        vend(s, 0, self, 0x14);
    }
    W16(self + 0x18, 0);
    SG16(gAudioError, 0);
    return 0;
}
