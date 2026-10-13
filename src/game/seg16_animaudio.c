/* CODE 16 "Anim & Audio" (Ghidra 0x4b604-0x4e432).
 *
 *  - tAnimation / KBAnimation (Symantec C++ classes, methods are "Pascal": C argument
 *    order, `this` first, callee pops): the scripted multi-layer animations of the
 *    cut-scenes, played by PlayTheAnimation through their vtables.
 *  - MohawkInit (start-up of the Mohawk libraries), Mohawk resource files, SwitchIn/Out.
 *  - The sound item table (CreateSound, SetSoundItem, ...) on top of the Mohawk audio
 *    library (audioCreate/audioStart/...).
 *  - Trap address cache (GetPOPTrap), cheat/debug helpers, the "go to level" dialog.
 *
 * Ported from the Ghidra decompilation, checked against the disassembly (ground truth);
 * addresses in comments are Ghidra addresses (see docs/PORTING.md). */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* ---- tAnimation (0x2ca bytes for a KBAnimation) ----
 *  +0 vtable, +4 frame (long), +8 running (word), +a "just started" (word),
 *  +c frame delay in ticks (word), +e next frame time (long), +12 'SCRP' script handle,
 *  +16 script offset (long), +1a bounds (Rect), +22 layer count (long),
 *  +26 layers: 0x1c bytes each (KBAnimation: +2c6 base shape id = 'ANI ' id + 1).
 * Layer: +0 changed (long), +4 rect drawn last frame, +c x, +10 y, +14 shape, +18 mode.
 * vtable: +0 __sd, +4 create, +8 draw, +10 getBounds, +18 isRunning, +1c maintain,
 *  +20 neglect, +24 prepare, +28 runScript, +2c seekFrame, +30 setPos, +34 startAnim,
 *  +38 stop, +40 notifyApp, +44 shapeDraw, +48 shapeTestPixel, +4c soundControl.
 * Script: long length, then commands {byte op, byte size, words...}:
 *  0 end, 1 next frame, 2 notify, 3 sound, 4 delay, 5 layer shape, 6 layer x/y, 7 layer mode. */
#define A_FRAME         4
#define A_RUNNING       8
#define A_STARTING      0xa
#define A_DELAY         0xc
#define A_NEXTTIME      0xe
#define A_SCRIPT        0x12
#define A_OFFSET        0x16
#define A_BOUNDS        0x1a
#define A_COUNT         0x22
#define A_LAYERS        0x26
#define LAYER_SIZE      0x1c
#define KB_BASEID       0x2c6
#define VMETHOD(obj, off)  R32(R32(obj) + (off))

#define gAnimVtable     GH(0x105d1c)   /* tAnimation vtable */
#define gKBAnimVtable   GH(0x105d78)   /* KBAnimation vtable */
#define gAnimEmptyRect  GH(0x105d6c)   /* Rect: empty */
#define gKBShapes       GH(0x105d74)   /* handle: 0x202 shape handles of the KBAnimation */
#define gKBRect         GH(0x105dc8)   /* Rect copied (unused) by KBAnimation::prepare */
#define gAnimEmptyRect2 GH(0x105dd0)   /* Rect: empty */
#define gAnimUpdRects   GH(0x105dd8)   /* Rect[24]: areas to update (CombineBounds) */
#define gAnimOldRects   GH(0x105e98)   /* Rect[24]: layer rects of the previous frame */
#define gAnimNewRects   GH(0x105f58)   /* Rect[24]: layer rects drawn this frame */
#define gAnimShapeSel   GH(0x10601a)   /* word: special-case one shape of the animation */
#define gAnimShapeId    GH(0x106018)   /* word: that shape id (made a private unpacked copy) */
#define gAnimWorld      GH(0x10601c)   /* offscreen world for the animation background */
#define gAnimWorldBuilt GH(0x10603c)   /* byte: gAnimWorld built (BuildOffGWorld) */
#define gOffWorld       GH(0x102dc2)   /* offscreen world of the game */
#define gMainWindow     GH(0x102dbe)   /* long: the game window */
#define gScreenRect     GH(0x103014)   /* Rect */

/* Toolbox rectangle routines cached by FUN_0004ddfa (Pascal, called through pointers) */
#define pTrapGetKeys    GH(0x106098)   /* GetKeys */
#define pClipRect       GH(0x10609c)   /* ClipRect(r) */
#define pEmptyRect      GH(0x1060a0)   /* EmptyRect(r): Boolean */
#define pUnionRect      GH(0x1060a4)   /* UnionRect(a, b, dst) */
#define pSectRect       GH(0x1060a8)   /* SectRect(a, b, dst): Boolean */

/* ---- sound items: handle of 0xc8 records of 0xe bytes, numbered from 1 ----
 *  +0 audio reference (audioCreate), +4 resource ('snd ' or 'MIDI'), +8 resource id,
 *  +a type (0 sound effect, 1 music), +c use count. */
#define gSndItems       GH(0x106094)   /* handle */
#define gSndItemMax     GH(0x106082)   /* word: highest item number in use */
#define gSndCue         GH(0x106092)   /* word: cue reached by the music (audio callback) */
#define gMasterVolume   GH(0x106074)   /* word: volume saved at start-up / switch-in */
#define gSndResFile     GH(0x10608e)   /* long: resource file for 'snd ' (0 = any) */
#define gMidiResFile    GH(0x10608a)   /* long: resource file for 'MIDI' */
#define gMusicDoneRef   GH(0x106076)   /* long: audio ref of a finished music (to unprepare) */
#define gSndDoneRef     GH(0x10607a)   /* long: audio ref of a finished sound */
#define gSndPlaying     GH(0x106086)   /* word: sound effect item playing */
#define gMusicPlaying   GH(0x106084)   /* word: music item playing */
#define gAudioPaused    GH(0x106080)   /* word */
#define gAudioLastMsg   GH(0x10607e)   /* word: last audio callback message */
#define gBGSetState     GH(0x103906)   /* word */
#define gAppActive      GH(0x103012)   /* word: the application is in front */

/* ---- misc game globals ---- */
#define gGameState      GH(0x102fec)   /* long: game state block (+218a level number) */
#define gChar           GH(0x102faa)   /* character record (0x3e bytes) */
#define gKid            GH(0x102f2e)   /* saved kid record */
#define gChar_ee        GH(0x102dee)   /* 10 bytes saved around DrawFootmark */
#define gCheatMode      GH(0x1057dc)   /* word */
#define gCheatStep      GH(0x1057ee)   /* word */
#define gKidSeq         GH(0x102f54)   /* word */
#define gKidPos         GH(0x102f38)   /* word */
#define gMsgTimer       GH(0x103034)
#define gMsgTimer2      GH(0x103036)
#define gCurPort        GH(0x106782)   /* long: a GrafPort (+50: fgColor) */
#define CH_ROW          GH(0x102fb8)
#define gRowY           GH(0x1034b2)   /* word[]: y of each row */
#define gMsgShown       GH(0x102f4a)   /* word */
#define gMsg70          GH(0x103070)   /* word */
#define gLevelJump      GH(0x103054)   /* word: a new level was asked for */
#define gLevelReq       GH(0x10302e)
#define gNISLevel       GH(0x103056)
#define gLevelLoaded    GH(0x10306e)

/* ---- helpers ---- */
#define SND_ITEM(base, i)  ((maddr)((base) + (int32_t)(int16_t)(i) * 0xe - 0xe))

static uint8_t sect_rect(maddr a, maddr b, maddr dst) {
    maddr sp0 = emu_sp();
    emu_set_sp(sp0 - 2);
    push32(a); push32(b); push32(dst);
    call68_addr(R32(pSectRect));          /* the callee pops its arguments */
    uint8_t r = R8(sp0 - 2);
    emu_set_sp(sp0);
    return r;
}
static uint8_t empty_rect(maddr r) {
    maddr sp0 = emu_sp();
    emu_set_sp(sp0 - 2);
    push32(r);
    call68_addr(R32(pEmptyRect));
    uint8_t v = R8(sp0 - 2);
    emu_set_sp(sp0);
    return v;
}
static void union_rect(maddr a, maddr b, maddr dst) {
    maddr sp0 = emu_sp();
    push32(a); push32(b); push32(dst);
    call68_addr(R32(pUnionRect));
    emu_set_sp(sp0);
}
static void clip_rect(maddr r) {
    maddr sp0 = emu_sp();
    push32(r);
    call68_addr(R32(pClipRect));
    emu_set_sp(sp0);
}

/* Pascal functions whose result is a Boolean byte in the slot above the arguments
 * ("conv": "manual"): the glue writes only that byte, like the original. When called
 * from C the caller's result slot is reserved too (ENTRY_LINK_S). */
#define ENTRYS_(fn, slot, n) \
    maddr entry_sp_ __attribute__((cleanup(port_leave))) = emu_sp(); \
    const maddr args_ = entry_sp_ == port_glue_sp ? entry_sp_ : entry_sp_ - (uint32_t)fn_info[fn].arg_bytes - (uint32_t)(slot); \
    emu_set_sp(args_ - (uint32_t)(n)); (void)args_
#define ENTRY_LINK_S(fn, slot, link, regbytes) ENTRYS_(fn, slot, 8 + (link) + (regbytes)); const maddr a6 = args_ - 8; (void)a6

/* ================================ tAnimation ================================ */

/* 0004b604: clear a layer (Pascal) */
uint32_t InitLayer__FP15tAnimationLayer(uint32_t layer) {
    ENTRY_LINK(FN_InitLayer__FP15tAnimationLayer, 0, 8);     /* link 0; movem A2/D3 */
    _SetRect(layer + 4, 0, 0, 0, 0);
    W32(layer + 0xc, 0);
    W32(layer + 0x10, 0);
    W32(layer + 0x14, 0);
    W32(layer + 0x18, 0);
    return 0;
}

/* 0004b660: tAnimation::tAnimation() */
uint32_t __ct__10tAnimationFv(uint32_t self) {
    ENTRY_LINK(FN___ct__10tAnimationFv, 0, 12);              /* link 0; movem A2/D4/D3 */
    W32(self, gAnimVtable);
    W32(self + A_FRAME, 0);
    W16(self + A_RUNNING, 0);
    W16(self + A_STARTING, 0);
    W16(self + A_DELAY, 0);
    W32(self + A_NEXTTIME, 0);
    W32(self + A_SCRIPT, 0);
    W32(self + A_OFFSET, 0);
    _SetRect(self + A_BOUNDS, 0, 0, 0, 0);
    W32(self + A_COUNT, 0);
    return self;
}

/* 0004b6d6: deleting destructor */
uint32_t __sd__10tAnimationFUi(uint32_t self, uint32_t flags) {
    ENTRY_LINK(FN___sd__10tAnimationFUi, 0, 0);              /* link 0 */
    if (self != 0) {
        __dt__10tAnimationFv(self);
        if (flags & 1) tc_operator_delete(self);
    }
    return self;
}

/* 0004b71e: tAnimation::~tAnimation() */
uint32_t __dt__10tAnimationFv(uint32_t self) {
    ENTRY_LINK(FN___dt__10tAnimationFv, 0, 4);               /* link 0; A2 */
    W32(self, gAnimVtable);
    free__10tAnimationFv(self);
    return 0;
}

/* 0004b754: load 'ANI ' id (bounds, layer count, script id) and its 'SCRP' script.
 * Returns ResError() when the 'ANI ' is missing, else 1 if the script is missing. */
uint32_t create__10tAnimationFii(uint32_t self, uint32_t id, uint32_t unused) {
    ENTRY_LINK(FN_create__10tAnimationFii, 0, 12);           /* link 0; movem A4/A3/A2 */
    uint32_t h = _GetResource(0x414e4920, (int16_t)id);      /* 'ANI ' */
    if (h == 0) return _ResError() & 0xffff;
    _MoveHHi(h);
    _HLock(h);
    uint32_t p = DEREF(h);
    W32(self + A_COUNT, (int32_t)R16(p + 8));
    W16(self + A_DELAY, 3);
    W32(self + A_BOUNDS, R32(p));
    W32(self + A_BOUNDS + 4, R32(p + 4));
    uint32_t s = _GetResource(0x53435250, R16(p + 0xa));    /* 'SCRP' */
    W32(self + A_SCRIPT, s);
    _HUnlock(h);
    return R32(self + A_SCRIPT) != 0 ? 0 : 1;
}

/* 0004b7f8: draw every layer through shapeDraw (vtable +44); the rect each layer drew
 * goes to gAnimNewRects */
uint32_t draw__10tAnimationFv(uint32_t self) {
    ENTRY_LINK(FN_draw__10tAnimationFv, 0x28, 28);           /* link -0x28; movem D3-D6/A2-A4 */
    int32_t left = R16(self + A_BOUNDS + 2);
    int32_t top = R16(self + A_BOUNDS);
    uint32_t empty0 = R32(gAnimEmptyRect), empty1 = R32(gAnimEmptyRect + 4);
    if (0 < (int32_t)R32(self + A_COUNT)) {
        maddr out = gAnimNewRects;
        maddr layer = self + A_LAYERS;
        int32_t off = 0;
        do {
            W32(out, empty0);
            W32(out + 4, empty1);
            uint32_t shape = R32(layer + 0x14);
            if (shape != 0) {
                uint32_t r = call_ptr(VMETHOD(self, 0x44), "llllll", self, R32(layer + 0x18),
                                      top + (int32_t)R32(layer + 0x10), left + (int32_t)R32(layer + 0xc),
                                      shape, a6 - 0x1c);
                W32(out, R32(r));
                W32(out + 4, R32(r + 4));
            }
            out += 8;
            off += LAYER_SIZE;
            layer += LAYER_SIZE;
        } while (off / LAYER_SIZE < (int32_t)R32(self + A_COUNT));
    }
    return 0;
}

/* 0004b90e: stop (vtable +38), neglect (+20) and make the script purgeable */
uint32_t free__10tAnimationFv(uint32_t self) {
    ENTRY_LINK(FN_free__10tAnimationFv, 0, 4);               /* link 0; A2 */
    call_ptr(VMETHOD(self, 0x38), "l", self);
    call_ptr(VMETHOD(self, 0x20), "l", self);
    uint32_t s = R32(self + A_SCRIPT);
    if (s != 0) _HPurge(s);
    return 0;
}

/* 0004b966: run the script for one frame when it is time; returns 1 if a layer changed */
uint32_t maintain__10tAnimationFUl(uint32_t self, uint32_t now) {
    ENTRY_LINK(FN_maintain__10tAnimationFUl, 0x10, 32);      /* link -0x10; movem D3-D7/A2-A4 */
    int32_t changed = 0;
    if (R16(self + A_STARTING) != 0) {
        W32(self + A_NEXTTIME, now);
        W16(self + A_STARTING, 0);
    }
    if (R16(self + A_RUNNING) != 0 && now >= R32(self + A_NEXTTIME)) {
        if ((int32_t)R32(self + A_COUNT) > 0) {
            maddr l = self + A_LAYERS;
            int32_t i = 0;
            do {
                W32(l, 0);
                l += LAYER_SIZE;
                i++;
            } while (i < (int32_t)R32(self + A_COUNT));
        }
        call_ptr(VMETHOD(self, 0x28), "lwl", self, 0, 1);
        _SetRect(a6 - 0x10, 0, 0, 0, 0);
        maddr first = self + A_LAYERS;
        maddr l = self + (uint32_t)((int32_t)R32(self + A_COUNT) * LAYER_SIZE) + 0xa;
        if ((int32_t)first <= (int32_t)l) {
            do {
                if (R32(l) != 0) {
                    changed++;
                    if (R32(l + 0x14) == 0) {
                        W32(l + 4, R32(a6 - 0x10));
                        W32(l + 8, R32(a6 - 0xc));
                    }
                }
                l -= LAYER_SIZE;
            } while ((int32_t)first <= (int32_t)l);
        }
        W32(self + A_NEXTTIME, now + (int32_t)R16(self + A_DELAY));
    }
    return changed != 0 ? 1 : 0;
}

/* 0004ba98: go to frame `frame` (rewinding the layers when going back) */
uint32_t seekFrame__10tAnimationFi(uint32_t self, uint32_t frame) {
    ENTRY_LINK(FN_seekFrame__10tAnimationFi, 0xc, 24);       /* link -0xc; movem D3-D5/A2-A4 */
    int32_t cur = (int32_t)R32(self + A_FRAME);
    int32_t want = (int32_t)frame;
    if (want == cur) return 0;
    if (want < cur) {
        if ((int32_t)R32(self + A_COUNT) > 0) {
            maddr l = self + A_LAYERS;
            int32_t i = 0;
            do {
                InitLayer__FP15tAnimationLayer(l);
                l += LAYER_SIZE;
                i++;
            } while (i < (int32_t)R32(self + A_COUNT));
        }
        W32(self + A_FRAME, 0);
        W32(self + A_OFFSET, 0);
    }
    uint16_t was = RU16(self + A_RUNNING);
    W16(self + A_RUNNING, 1);
    /* the count is from the old frame even after a rewind (as in the original) */
    call_ptr(VMETHOD(self, 0x28), "lwl", self, 1, (uint32_t)(want - cur));
    W16(self + A_RUNNING, RU16(self + A_RUNNING) & was);
    return 0;
}

/* 0004bb38: move the bounds to (v, h) */
uint32_t setPos__10tAnimationFii(uint32_t self, uint32_t v, uint32_t h) {
    ENTRY_LINK(FN_setPos__10tAnimationFii, 0, 4);            /* link 0; A2 */
    int16_t dh = (int16_t)((int16_t)h - R16(self + A_BOUNDS + 2));
    int16_t dv = (int16_t)((int16_t)v - R16(self + A_BOUNDS));
    _OffsetRect(self + A_BOUNDS, dh, dv);
    return 0;
}

/* 0004bb86 */
uint32_t startAnim__10tAnimationFv(uint32_t self) {
    W16(self + A_STARTING, 1);
    W16(self + A_RUNNING, 1);
    return 0;
}

/* 0004bbc4 */
uint32_t stop__10tAnimationFv(uint32_t self) {
    W16(self + A_RUNNING, 0);
    return 0;
}

/* 0004bbf0: number (1-based) of the topmost layer with a pixel at (v, h), or 0 */
uint32_t testPixel__10tAnimationFii(uint32_t self, uint32_t v, uint32_t h) {
    ENTRY_LINK(FN_testPixel__10tAnimationFii, 0xc, 16);      /* link -0xc; movem D3/A2-A4 */
    if (R32(self + A_FRAME) == 0) return 0;
    h -= (int32_t)R16(self + A_BOUNDS + 2);
    v -= (int32_t)R16(self + A_BOUNDS);
    maddr l = self + (uint32_t)((int32_t)R32(self + A_COUNT) * LAYER_SIZE) + 0xa;
    if ((int32_t)(self + A_LAYERS) > (int32_t)l) return 0;
    do {
        maddr rect = l + 4;
        uint32_t pt = (uint32_t)(uint16_t)v << 16 | (uint16_t)h;
        if (R32(l + 0x14) != 0 && _PtInRect(pt, rect) != 0) {
            uint32_t r = call_ptr(VMETHOD(self, 0x48), "lllll", self, R32(l + 0x18),
                                  v - (int32_t)R16(rect), h - (int32_t)R16(rect + 2), R32(l + 0x14));
            if (r != 0) return (uint32_t)((int32_t)(l - (self + A_LAYERS)) / LAYER_SIZE + 1);
        }
        l -= LAYER_SIZE;
    } while ((int32_t)(self + A_LAYERS) <= (int32_t)l);
    return 0;
}

/* 0004bd16: copy the bounds to *r (returns r) */
uint32_t getBounds__10tAnimationFv(uint32_t self, uint32_t r) {
    W32(r, R32(self + A_BOUNDS));
    W32(r + 4, R32(self + A_BOUNDS + 4));
    return r;
}

/* 0004bd5c */
uint32_t getFrame__10tAnimationFv(uint32_t self) {
    return R32(self + A_FRAME);
}

/* 0004bd8a: D0.w = running (the high word is the caller's) */
uint32_t isRunning__10tAnimationFv(uint32_t self) {
    return (cpu_get(R_D0) & 0xffff0000u) | RU16(self + A_RUNNING);
}

/* 0004bdba */
uint32_t neglect__10tAnimationFv(uint32_t self) {
    return cpu_get(R_D0) & 0xffff0000u;
}

/* 0004bde2 */
uint32_t prepare__10tAnimationFv(uint32_t self) {
    return cpu_get(R_D0) & 0xffff0000u;
}

/* 0004be0a */
uint32_t notifyApp__10tAnimationFP23tAnimationScriptCommand(uint32_t self, uint32_t cmd) {
    return 0;
}

/* 0004be4a: returns its rect argument */
uint32_t shapeDraw__10tAnimationFiiii(uint32_t self, uint32_t mode, uint32_t v, uint32_t h, uint32_t shape, uint32_t rect) {
    return rect;
}

/* 0004be78 */
uint32_t shapeTestPixel__10tAnimationFiiii(uint32_t self, uint32_t mode, uint32_t v, uint32_t h, uint32_t shape) {
    return 1;
}

/* 0004beaa */
uint32_t soundControl__10tAnimationFP23tAnimationScriptCommand(uint32_t self, uint32_t cmd) {
    return 0;
}

/* 0004beee: run script commands until `count` frames have passed or the script ends;
 * notify/sound commands only when not seeking (seek == 0) */
uint32_t runScript__10tAnimationFiUs(uint32_t self, int16_t seek, uint32_t count) {
    ENTRY_LINK(FN_runScript__10tAnimationFiUs, 0x20, 16);    /* link -0x20; movem D4/A2-A4 */
    _HLock(R32(self + A_SCRIPT));
    maddr script = DEREF(R32(self + A_SCRIPT));
    int32_t start = (int32_t)R32(self + A_FRAME);
    int32_t end = start + (int32_t)count;
    if (R16(self + A_RUNNING) != 0 && start < end) {
        maddr layers = self + A_LAYERS;
        do {
            maddr cmd = script + 4 + R32(self + A_OFFSET);
            int16_t op = R8(cmd);
            maddr l;
            switch (op) {
            case 0: W16(self + A_RUNNING, 0); break;
            case 1: W32(self + A_FRAME, R32(self + A_FRAME) + 1); break;
            case 2:
                if (seek == 0) call_ptr(VMETHOD(self, 0x40), "ll", self, cmd);
                break;
            case 3:
                if (seek == 0) call_ptr(VMETHOD(self, 0x4c), "ll", self, cmd);
                break;
            case 4: W16(self + A_DELAY, RU16(cmd + 2)); break;
            case 5:
                l = layers + (uint32_t)((int32_t)R16(cmd + 2) * LAYER_SIZE) - LAYER_SIZE;
                W32(l + 0x14, (int32_t)R16(cmd + 4));
                W32(l, R32(l) + 1);
                break;
            case 6:
                l = layers + (uint32_t)((int32_t)R16(cmd + 2) * LAYER_SIZE) - LAYER_SIZE;
                W32(l + 0xc, (int32_t)R16(cmd + 4));
                W32(l + 0x10, (int32_t)R16(cmd + 6));
                W32(l, R32(l) + 1);
                break;
            case 7:
                l = layers + (uint32_t)((int32_t)R16(cmd + 2) * LAYER_SIZE) - LAYER_SIZE;
                W32(l + 0x18, (int32_t)R16(cmd + 4));
                W32(l, R32(l) + 1);
                break;
            default: break;
            }
            if (R16(self + A_RUNNING) != 0) W32(self + A_OFFSET, R32(self + A_OFFSET) + R8(cmd + 1));
        } while (R16(self + A_RUNNING) != 0 && (int32_t)R32(self + A_FRAME) < end);
    }
    _HUnlock(R32(self + A_SCRIPT));
    return 0;
}

/* ================================ KBAnimation ================================ */

/* 0004c100: KBAnimation::KBAnimation() */
uint32_t __ct__11KBAnimationFv(uint32_t self) {
    ENTRY_LINK(FN___ct__11KBAnimationFv, 0, 0);              /* link 0 */
    __ct__10tAnimationFv(self);
    W32(self, gKBAnimVtable);
    return self;
}

/* 0004c138: deleting destructor */
uint32_t __sd__11KBAnimationFUi(uint32_t self, uint32_t flags) {
    ENTRY_LINK(FN___sd__11KBAnimationFUi, 0, 0);             /* link 0 */
    if (self != 0) {
        __dt__11KBAnimationFv(self);
        if (flags & 1) tc_operator_delete(self);
    }
    return self;
}

/* 0004c180 */
uint32_t maintain__11KBAnimationFUl(uint32_t self, uint32_t now) {
    ENTRY_LINK(FN_maintain__11KBAnimationFUl, 4, 0);         /* link -4 */
    return maintain__10tAnimationFUl(self, now);
}

/* 0004c1b4: create, remember the base shape id and keep the 'ANI ' locked;
 * returns 0 when everything is there */
uint32_t create__11KBAnimationFii(uint32_t self, uint32_t id, uint32_t base) {
    ENTRY_LINK(FN_create__11KBAnimationFii, 0, 8);           /* link 0; movem A2/D3 */
    int16_t err = S16(create__10tAnimationFii(self, id, base));
    if (err == 0) {
        W32(self + KB_BASEID, base);
        err = 1;
        uint32_t h = _GetResource(0x414e4920, (int16_t)id);  /* 'ANI ' */
        if (h != 0) {
            err = 0;
            _MoveHHi(h);
            _HLock(h);
        }
    }
    return (uint16_t)err;
}

/* 0004c224: KBAnimation::~KBAnimation() */
uint32_t __dt__11KBAnimationFv(uint32_t self) {
    ENTRY_LINK(FN___dt__11KBAnimationFv, 0, 4);              /* link 0; A2 */
    W32(self, gKBAnimVtable);
    __dt__10tAnimationFv(self);
    return 0;
}

/* 0004c25c */
uint32_t neglect__11KBAnimationFv(uint32_t self) {
    return cpu_get(R_D0) & 0xffff0000u;
}

/* 0004c284: load the shapes (gKBShapes) and the sounds the script uses */
uint32_t prepare__11KBAnimationFv(uint32_t self) {
    ENTRY_LINK(FN_prepare__11KBAnimationFv, 0x18, 32);       /* link -0x18; movem D3-D7/A2-A4 */
    W32(a6 - 0x18, R32(gKBRect));
    W32(a6 - 0x14, R32(gKBRect + 4));
    _HLock(R32(self + A_SCRIPT));
    maddr script = DEREF(R32(self + A_SCRIPT));
    tc_memset(gAnimNewRects, 0, 0xc0);
    tc_memset(gAnimOldRects, 0, 0xc0);
    tc_memset(gAnimUpdRects, 0, 0xc0);
    if (R32(gKBShapes) != 0) {
        _HUnlock(R32(gKBShapes));
        _DisposeHandle(R32(gKBShapes));
        W32(gKBShapes, 0);
    }
    uint32_t th = _NewHandle(0x808);
    W32(gKBShapes, th);
    if (th == 0) return 1;
    for (int32_t off = 0; off < 0x808; off += 4) W32(DEREF(R32(gKBShapes)) + off, 0);
    W32(self + A_OFFSET, 0);
    if (0 < R32(script)) {
        do {
            maddr cmd = script + 4 + R32(self + A_OFFSET);
            int16_t op = R8(cmd);
            if (op == 5) {
                int16_t n = R16(cmd + 4);
                if (n >= 1 && R32(DEREF(R32(gKBShapes)) + (int32_t)n * 4) == 0) {
                    if (R16(gAnimShapeSel) != 0 &&
                        (int32_t)R32(self + KB_BASEID) + n - 1 == (int32_t)R16(gAnimShapeId)) {
                        uint32_t h = GetShape((int16_t)(R32(self + KB_BASEID) + n - 1));
                        W32(a6 - 0x10, h);
                        if (h != 0) {
                            int32_t off = (int32_t)n * 4;
                            W32(a6 - 4, off);
                            W32(DEREF(R32(gKBShapes)) + off, h);
                            /* make a private copy of the shape and unpack it */
                            if (S16(tc_HandToHand(DEREF(R32(gKBShapes)) + R32(a6 - 4))) == 0)
                                UnPackAndMapSHAP(0x0f00, DEREF(R32(gKBShapes)) + R32(a6 - 4));
                            if (R32(DEREF(R32(gKBShapes)) + (int32_t)n * 4) != R32(a6 - 0x10)) {
                                _HUnlock(R32(a6 - 0x10));
                                _HPurge(R32(a6 - 0x10));
                            }
                        } else {
                            W32(DEREF(R32(gKBShapes)) + (int32_t)n * 4, 0);
                        }
                    } else {
                        uint32_t h = GetShape((int16_t)(R32(self + KB_BASEID) + n - 1));
                        W32(DEREF(R32(gKBShapes)) + (int32_t)n * 4, h);
                    }
                    uint32_t h = R32(DEREF(R32(gKBShapes)) + (int32_t)n * 4);
                    if (h != 0) _HNoPurge(h);
                }
            } else if (op == 3) {
                if (R16(cmd + 2) != 0 && resFind(0, R16(cmd + 2), 0x736e6420) != 0)    /* 'snd ' */
                    CreateSound(-1, R16(cmd + 2), 1);
            }
            W32(self + A_OFFSET, R32(self + A_OFFSET) + R8(cmd + 1));
        } while (R32(self + A_OFFSET) < R32(script));
    }
    W32(self + A_OFFSET, 0);
    _HUnlock(R32(self + A_SCRIPT));
    return 0;
}

/* 0004c534: script command 3: play a sound */
uint32_t soundControl__11KBAnimationFP23tAnimationScriptCommand(uint32_t self, uint32_t cmd) {
    ENTRY_LINK(FN_soundControl__11KBAnimationFP23tAnimationScriptCommand, 8, 4);  /* link -8; D3 */
    int16_t id = R16(cmd + 2);
    if (id != 0) PlaySound(-1, id, 1);
    return 0;
}

/* 0004c596: draw shape `shape` of gKBShapes; returns its rect argument (filled by DrawIt) */
uint32_t shapeDraw__11KBAnimationFiiii(uint32_t self, uint32_t mode, uint32_t v, uint32_t h, uint32_t shape, uint32_t rect) {
    ENTRY_LINK(FN_shapeDraw__11KBAnimationFiiii, 8, 4);      /* link -8; D3 */
    if (R32(gKBShapes) != 0 && (int32_t)shape >= 0 && (int32_t)shape < 0x202) {
        uint32_t sh = R32(DEREF(R32(gKBShapes)) + shape * 4);
        DrawIt(DEREF(sh), 0, (int16_t)h, (int16_t)v, (int16_t)mode, 0, rect);
    }
    return rect;
}

/* 0004c60c: play animation 'ANI ' base+offset (shapes base+n) until it ends or, when
 * `abortable`, a key/click. `callback(frame)` runs after every frame (non-zero ends the
 * animation). Flags: 1 simple loop, 2 position (h, v) given, 4 update only the
 * changed rects, 8 background from an offscreen copy, 0x10 keep the resource position,
 * 0x100 redraw from row 0x6c. Returns -1/-2 on errors, else the abort/callback code. */
uint32_t PlayTheAnimation(int16_t base, int16_t offset, int16_t p8, uint32_t callback, int16_t p14,
                          int16_t p16, int16_t abortable, int16_t flags, int16_t color, int16_t posH, int16_t posV) {
    ENTRY_LINK(FN_PlayTheAnimation, 0x4c, 32);               /* link -0x4c; movem D3-D7/A2-A4 */
    maddr bounds = a6 - 0x3c, clip = a6 - 0x30, wrect = a6 - 0x28, game = a6 - 0x44;
    int32_t frames = 0;
    int16_t cbres = 0, quit = 0, result = 0;
    W8(gAnimWorldBuilt, 0);
    uint32_t anim = tc_operator_new(0x2ca);
    if (anim != 0) __ct__11KBAnimationFv(anim);
    int16_t err = S16(call_ptr(VMETHOD(anim, 4), "lll", anim, (uint32_t)((int32_t)base + offset),
                               (uint32_t)((int32_t)base + 1)));
    if (err != 0) {
        if (anim != 0) call_ptr(VMETHOD(anim, 0), "ll", anim, 1);
        return 0xffffffffu;
    }
    uint32_t r = call_ptr(VMETHOD(anim, 0x10), "ll", anim, a6 - 0x14);
    W32(bounds, R32(r));
    W32(bounds + 4, R32(r + 4));
    int16_t posh = 0, posv = 0;
    W16(a6 - 0x1e, 0);
    W16(a6 - 0x20, 0);
    if (flags & 2) { posh = posH; posv = posV; }
    else if (flags & 0x10) { posh = R16(bounds + 2); posv = R16(bounds); }
    W16(a6 - 0x1e, posh);
    W16(a6 - 0x20, posv);
    call_ptr(VMETHOD(anim, 0x30), "lll", anim, (int32_t)posv, (int32_t)posh);
    MoveRect(bounds, posh, posv);
    call_ptr(VMETHOD(anim, 0x24), "l", anim);
    call_ptr(VMETHOD(anim, 0x2c), "ll", anim, 0);
    call_ptr(VMETHOD(anim, 0x34), "l", anim);
    _ObscureCursor();
    int32_t id = (int32_t)base + offset;
    if (id != 0x61ac && id != 0x7534 && id != 0x791a && R16(bounds + 4) > 0x16c) W16(bounds + 4, 0x16d);
    SaveCurrentWorld(gOffWorld);
    SetGameRect(game);
    if (sect_rect(bounds, game, clip)) clip_rect(clip);
    if (color != 0) {
        _PenNormal();
        SetForeColor(color);
        _PaintRect(clip);
    }
    RestoreOriginalWorld(gOffWorld);
    if (callback != 0) cbres = S16(call_ptr(callback, "w", 0));
    if (cbres == 0 && (flags & 8)) {
        W32(wrect, R32(clip));
        W32(wrect + 4, R32(clip + 4));
        W16(wrect + 2, 0);
        W16(wrect, 0);
        W16(a6 - 0xc, BuildOffGWorld(gAnimWorld, wrect));
        if (R8(gAnimWorldBuilt) != 0) {
            DrawOffToOff(gOffWorld, gAnimWorld, clip, clip, 0);
        } else {
            if (anim != 0) call_ptr(VMETHOD(anim, 0), "ll", anim, 1);
            return 0xfffffffeu;
        }
    }
    if (cbres == 0) {
        if (flags & 1) {
            if (quit == 0 && S16(call_ptr(VMETHOD(anim, 0x18), "l", anim)) != 0) {
                do {
                    uint32_t now = _TickCount();
                    if (S16(call_ptr(VMETHOD(anim, 0x1c), "ll", anim, now)) != 0) {
                        SaveCurrentWorld(gOffWorld);
                        call_ptr(VMETHOD(anim, 8), "l", anim);
                        RestoreOriginalWorld(gOffWorld);
                        DrawSOffWorldToWindow(gOffWorld, R32(gMainWindow), bounds, bounds, 0);
                        W16(bounds, 0x6c);
                        frames++;
                        if (frames & 1) CheckCursorVisibility();
                    }
                    if ((uint16_t)abortable >> 8) {
                        result = S16(FUN_00044bd6());
                        if (_Button()) { quit = 1; result = 1; }
                        else if (result != 0) quit = 1;
                    }
                } while (quit == 0 && S16(call_ptr(VMETHOD(anim, 0x18), "l", anim)) != 0);
            }
        } else if (quit == 0 && S16(call_ptr(VMETHOD(anim, 0x18), "l", anim)) != 0) {
            do {
                uint32_t now = _TickCount();
                if (S16(call_ptr(VMETHOD(anim, 0x1c), "ll", anim, now)) != 0) {
                    SaveCurrentWorld(gOffWorld);
                    if (flags & 8) {
                        maddr u = gAnimUpdRects;
                        for (int16_t i = 0; i < 0x18; i++, u += 8)
                            if (!empty_rect(u)) DrawOffToOff(gAnimWorld, gOffWorld, u, u, 0);
                    } else if (color != 0) {
                        SetForeColor(color);
                        _PaintRect(bounds);
                    }
                    call_ptr(VMETHOD(anim, 8), "l", anim);
                    RestoreOriginalWorld(gOffWorld);
                    if (callback != 0) cbres = S16(call_ptr(callback, "w", (uint16_t)(frames + 1)));
                    if (flags & 4) {
                        CombineBounds__Fv();
                        maddr u = gAnimUpdRects;
                        for (int16_t i = 0; i < 0x18; i++, u += 8)
                            if (!empty_rect(u)) DrawSOffWorldToWindow(gOffWorld, R32(gMainWindow), u, u, 0);
                    } else {
                        DrawSOffWorldToWindow(gOffWorld, R32(gMainWindow), bounds, bounds, 0);
                    }
                    frames++;
                    if (frames & 1) CheckCursorVisibility();
                    if (flags & 0x100) W16(bounds, 0x6c);
                }
                if ((uint16_t)abortable >> 8) {
                    result = S16(FUN_00044bd6());
                    if (_Button()) { quit = 1; result = 1; }
                    else if (result != 0) quit = 1;
                    else if (cbres != 0) quit = 1;
                }
            } while (quit == 0 && S16(call_ptr(VMETHOD(anim, 0x18), "l", anim)) != 0);
        }
    }
    if (R32(gKBShapes) != 0) {
        for (int32_t off = 0; off < 0x808; off += 4) {
            uint32_t h = R32(DEREF(R32(gKBShapes)) + off);
            if (h != 0) {
                _HUnlock(h);
                _HPurge(R32(DEREF(R32(gKBShapes)) + off));
            }
        }
        _HUnlock(R32(gKBShapes));
        _DisposeHandle(R32(gKBShapes));
        W32(gKBShapes, 0);
    }
    if (anim != 0) call_ptr(VMETHOD(anim, 0), "ll", anim, 1);
    SaveCurrentWorld(gOffWorld);
    clip_rect(R32(gMainWindow) + 0x10);
    RestoreOriginalWorld(gOffWorld);
    if (R8(gAnimWorldBuilt) != 0) DisposeSOffWorld(gAnimWorld);
    if (cbres != 0 && result == 0) result = cbres;
    return (uint16_t)result;
}

/* 0004cbb6: merge last frame's and this frame's layer rects into gAnimUpdRects, then
 * merge the overlapping update rects */
uint32_t CombineBounds__Fv(void) {
    ENTRY_LINK(FN_CombineBounds__Fv, 0x10, 24);              /* link -0x10; movem D3-D5/A2-A4 */
    W32(a6 - 8, R32(gAnimEmptyRect2));
    W32(a6 - 4, R32(gAnimEmptyRect2 + 4));
    tc_memset(gAnimUpdRects, 0, 0xc0);
    maddr upd = gAnimUpdRects, old = gAnimOldRects, cur = gAnimNewRects;
    uint16_t i = 0;
    do {
        if (!empty_rect(old)) {
            if (!empty_rect(cur)) union_rect(old, cur, upd);
            else mcopy(upd, old, 8);
        } else if (!empty_rect(cur)) {
            mcopy(upd, cur, 8);
        }
        mcopy(old, cur, 8);
        i++; upd += 8; old += 8; cur += 8;
    } while (i < 0x17);
    tc_memset(gAnimNewRects, 0, 0xc0);
    maddr a = gAnimUpdRects;
    uint16_t n = 1;
    do {
        if (!empty_rect(a)) {
            maddr b = a + 8;
            int16_t k = (int16_t)n;
            if (n < 0x18) {
                do {
                    if (!empty_rect(b) && sect_rect(a, b, a6 - 0x10)) {
                        union_rect(a, b, a);
                        W32(b, R32(a6 - 8));
                        W32(b + 4, R32(a6 - 4));
                    }
                    k++; b += 8;
                } while (k < 0x18);
            }
        }
        n++; a += 8;
    } while (n < 0x18);
    return 0;
}

/* 0004ccea: (bottom - top) * (right - left) (Pascal) */
uint32_t RectArea__FP4Rect(uint32_t r) {
    int32_t w = (int32_t)R16(r + 6) - R16(r + 2);
    int32_t hgt = (int32_t)R16(r + 4) - R16(r);
    return (uint32_t)(hgt * w);
}

/* ============================== Mohawk libraries ============================== */

/* 0004cd38: initialise the Mohawk libraries (each one registers its quit routine),
 * then the sound item table.
 * The library routines save D3-D5/A2 on the stack and some of them later copy stack
 * bytes they did not initialise into their globals (fileGetDir copies a partly filled
 * local FSSpec): the CPU registers mirror the original's D3 (mask), D4 (error),
 * D5 (libraries still to initialise) and A2 so that these stack bytes are the same. */
#define SETW(reg, v)  cpu_set(reg, (cpu_get(reg) & 0xffff0000u) | (uint16_t)(v))
uint32_t MohawkInit(void) {
    ENTRY_LINK(FN_MohawkInit, 0x41c, 16);                    /* link -0x41c; movem D3-D5/A2 */
    const uint32_t sv_d3 = cpu_get(R_D3), sv_d4 = cpu_get(R_D4), sv_d5 = cpu_get(R_D5), sv_a2 = cpu_get(R_A2);
    int16_t err = 0;
    uint16_t todo = 0xbf;
    SETW(R_D4, err);
    SETW(R_D5, todo);
    if (osInit(0) == 0) {
        err = S16(osGetError());
        SETW(R_D4, err);
    } else {
        todo = 0xbe;
        SETW(R_D5, todo);
        tc__atexit(JT_ADDR(FN_osQuit));
    }
    cpu_set(R_D3, 2);
    if ((todo & 2) && err == 0) {
        err = S16(memInit(0));
        SETW(R_D4, err);
        if (err == 0) { todo ^= 2; SETW(R_D5, todo); tc__atexit(JT_ADDR(FN_memQuit)); }
    }
    cpu_set(R_D3, 8);
    if ((todo & 8) && err == 0) {
        err = S16(fileInit(0));
        SETW(R_D4, err);
        if (err == 0) { todo ^= 8; SETW(R_D5, todo); tc__atexit(JT_ADDR(FN_fileQuit)); }
    }
    cpu_set(R_D3, 4);
    if ((todo & 4) && err == 0) {
        err = S16(timerInit());
        SETW(R_D4, err);
        if (err == 0) { todo ^= 4; SETW(R_D5, todo); tc__atexit(JT_ADDR(FN_timerQuit)); }
    }
    cpu_set(R_D3, 0x10);
    if ((todo & 0x10) && err == 0) {
        err = S16(resInit());
        SETW(R_D4, err);
        if (err == 0) { todo ^= 0x10; SETW(R_D5, todo); tc__atexit(JT_ADDR(FN_resQuit)); }
    }
    cpu_set(R_D3, 0x20);
    if ((todo & 0x20) && err == 0) {
        err = S16(configInit());
        SETW(R_D4, err);
        if (err == 0) { todo ^= 0x20; SETW(R_D5, todo); tc__atexit(JT_ADDR(FN_configQuit)); }
    }
    if ((todo & 0x80) && err == 0) {
        maddr dataDir = a6 - 0x418, saved = a6 - 0x310, spare = a6 - 0x208;
        __ct__8fileSpecDPCc(dataDir, fn_address(FN_MohawkInit) + 0x1ea);   /* "Data" */
        __ct__8fileSpecDv(saved);
        __ct__8fileSpecDv(spare);
        err = S16(fileGetError());
        SETW(R_D4, err);
        if (err == 0) {
            fileGetDir(saved);
            err = S16(fileSetDir(dataDir));
            SETW(R_D4, err);
            if (err == 0) {
                err = S16(audioInit());
                SETW(R_D4, err);
                if (err == 0) { todo ^= 0x80; SETW(R_D5, todo); tc__atexit(JT_ADDR(FN_audioQuit)); }
            }
            fileSetDir(saved);
        }
        __dt__8fileSpecDv(spare);
        __dt__8fileSpecDv(saved);
        __dt__8fileSpecDv(dataDir);
    }
    if (todo != 0) ErrorHandler((int16_t)(todo | 0x8000), err);
    uint32_t h = PoPNewHandle(0xaf0);
    W32(gSndItems, h);
    cpu_set(R_A2, DEREF(h));
    tc_memset(DEREF(h), 0, 0xaf0);
    SETW(R_D3, 0);
    W16(gSndItemMax, 0);
    W16(gSndCue, 0);
    W16(gMasterVolume, GetMasterVolume());
    seg5_029374();
    cpu_set(R_D3, 0);
    W32(gSndResFile, 0);
    W32(gMidiResFile, 0);
    cpu_set(R_D3, sv_d3); cpu_set(R_D4, sv_d4); cpu_set(R_D5, sv_d5); cpu_set(R_A2, sv_a2);   /* movem.l (SP)+ */
    return 0;
}

/* 0004cf28: open the resource file {long refnum; name} once (Pascal, long result);
 * any error is fatal */
uint32_t OpenMohawkRsrcVerify(uint32_t f) {
    ENTRY_LINK(FN_OpenMohawkRsrcVerify, 0x16c, 8);           /* link -0x16c; movem D3/A2 */
    if ((int32_t)R32(f) <= 0) {
        maddr spec = a6 - 0x16c;
        __ct__8fileSpecDPCc(spec, f + 4);
        int16_t err = S16(fileGetError());
        if (err == 0) {
            W32(f, resOpenFile(spec));
            err = S16(resGetError());
        }
        if ((int32_t)R32(f) <= 0 || err != 0) {
            call_ptr(JT_ADDR(FN_tc_sprintf), "lll", a6 - 0x64,
                     fn_address(FN_OpenMohawkRsrcVerify) + 0xaa, (uint32_t)(int32_t)err);
            Terminate(1, a6 - 0x64);
        }
        __dt__8fileSpecDv(spec);
    }
    return R32(f);
}

/* 0004cffa: close the resource file opened by OpenMohawkRsrcVerify (Pascal) */
uint32_t CloseMohawkRsrc(uint32_t f) {
    ENTRY_LINK(FN_CloseMohawkRsrc, 4, 4);                    /* link -4; A2 */
    if ((int32_t)R32(f) > 0) resCloseFile(0, R32(f));
    W32(f, 0);
    return 0;
}

/* 0004d03a: the application goes to the background */
uint32_t SwitchOut(void) {
    ENTRY_LINK(FN_SwitchOut, 4, 4);                          /* link -4; D3 */
    e2MAC_ShowMenuBar();
    TakeBackReleaseColors(R32(gMainWindow), 0);
    W16(gAppActive, 0);
    ResetSoundItem(0);
    SetTimer(1, 0, 10);
    WaitTimerNoPoll(1);
    SetMasterVolume(R16(gMasterVolume));
    PauseAudioSystem();
    return 0;
}

/* 0004d096: the application comes back to the front */
uint32_t SwitchIn(void) {
    ENTRY_LINK(FN_SwitchIn, 0x100, 4);                       /* link -0x100; D3 */
    W16(gMasterVolume, GetMasterVolume());
    RecheckSystem(1 << 8);                                   /* move.b #1 */
    e2MAC_HideMenuBar();
    TakeBackReleaseColors(R32(gMainWindow), 1 << 8);
    _InitCursor();
    W16(gAppActive, 1);
    ResumeAudioSystem();
    seg5_029374();
    return 0;
}

/* ================================ sound items ================================ */

/* 0004d0e2: create (or reuse) the sound item for resource `id` ('snd ' if isSnd, else
 * 'MIDI') in item `item` (-1: a free one). Returns the item. Writes its own argument. */
uint32_t CreateSound(int16_t item, int16_t id, uint32_t isSnd) {
    ENTRY_LINK(FN_CreateSound, 4, 20);                       /* link -4; movem D3/D4/A2-A4 */
    int16_t found = S16(GetSndItemForId(id, (int16_t)isSnd));
    if (found != 0) {
        maddr it = SND_ITEM(DEREF(R32(gSndItems)), found);
        if (R16(it + 0xc) == 0) W16(it + 0xc, R16(it + 0xc) + 1);
        return (uint16_t)found;
    }
    uint32_t res;
    if (isSnd == 0) res = resFind(R32(gMidiResFile), id, 0x4d494449);     /* 'MIDI' */
    else res = resFind(R32(gSndResFile), id, 0x736e6420);                 /* 'snd ' */
    if (res != 0) {
        resSetPurgeable(0, res);
        if (item == -1) {
            item = S16(FUN_0004d446());
            W16(args_, item);                                /* move.w D0w,(0x8,A6) */
        }
        _HLock(R32(gSndItems));
        maddr it = SND_ITEM(DEREF(R32(gSndItems)), item);
        if (R16(it + 8) != 0) DestroySoundItem(item);
        W32(it + 4, res);
        _HUnlock(R32(gSndItems));
        int16_t mh = S16(resLoad(res));
        if (mh == 0 && R16(gBGSetState) != 1) {
            MarkBGSetPurgable(1);
            mh = S16(resLoad(res));
        }
        if (mh != 0) {
            uint32_t ref = audioCreate(mh);
            _HLock(R32(gSndItems));
            it = SND_ITEM(DEREF(R32(gSndItems)), item);
            W32(it, ref);
            W16(it + 8, id);
            W16(it + 0xa, (uint16_t)isSnd);
            W16(it + 0xc, 1);
            if (R16(gSndItemMax) < item) W16(gSndItemMax, item);
        }
    }
    _HUnlock(R32(gSndItems));
    return (uint16_t)item;
}

/* 0004d286: unprepare and destroy an audio reference */
uint32_t DestroyAudioRef(uint32_t ref) {
    ENTRY_LINK(FN_DestroyAudioRef, 0, 4);                    /* link 0; D3 */
    int16_t err = 0;
    if (ref != 0) {
        err = S16(audioGetState(ref));
        if (err & 1) err = S16(audioUnprepare(ref));
        else err = 0;
        if (err == 0) {
            err = S16(audioDestroy(ref));
            if (R32(gMusicDoneRef) == ref) W32(gMusicDoneRef, 0);
            else if (R32(gSndDoneRef) == ref) W32(gSndDoneRef, 0);
        }
    }
    return (uint16_t)err;
}

/* 0004d306 */
uint32_t DestroySoundId(int16_t id, int16_t type) {
    ENTRY_LINK(FN_DestroySoundId, 0, 4);                     /* link 0; D3 */
    if (id != 0) {
        int16_t item = S16(GetSndItemForId(id, type));
        if (item != 0) DestroySoundItem(item);
    }
    return 0;
}

/* 0004d344: free a sound item that is not playing */
uint32_t DestroySoundItem(int16_t item) {
    ENTRY_LINK(FN_DestroySoundItem, 4, 20);                  /* link -4; movem D3-D5/A3/A4 */
    if (item == 0) return 0;
    if (S16(SoundItemOn(item)) != 0) return 0;
    maddr it = SND_ITEM(DEREF(R32(gSndItems)), item);       /* not re-read after the calls */
    if (R16(it + 8) == 0) return 0;
    DestroyAudioRef(R32(it));
    resSetPurgeable(1, R32(it + 4));
    int16_t mh = S16(resLoad(R32(it + 4)));
    if (mh != 0) {
        memUnlock(mh);
        memSetPurgeState(1, mh);
        memPurgeHandle(mh);
    }
    resSetPurgeable(1, R32(it + 4));
    W16(it + 8, 0);
    W16(it + 0xc, 0);
    if (R16(gSndItemMax) == item) {
        int16_t n = item;
        if (item > 0 && R16(it + 8) == 0) {
            maddr p = it + 8;
            for (;;) {
                n--;
                p -= 0xe;
                if (n <= 0) break;
                if (R16(p) != 0) break;
            }
        }
        W16(gSndItemMax, n);
    }
    return 0;
}

/* 0004d446: find an item to use: the first empty one, or the first unused one that is
 * not playing (destroyed); 0 when none below 0xc8 */
uint32_t FUN_0004d446(void) {
    ENTRY_NOLINK(FN_FUN_0004d446, 20);                       /* movem D3/D4/A2-A4 */
    maddr base = DEREF(R32(gSndItems));
    int16_t i = 1;
    if (R16(gSndItemMax) >= 1) {
        maddr cnt = base + 0xc, idp = base + 8;
        do {
            if (R16(idp) == 0) break;
            if (R16(cnt) == 0 && S16(SoundItemOn(i)) == 0) {
                DestroySoundItem(i);
                break;
            }
            i++;
            cnt += 0xe;
            idp += 0xe;
        } while (i <= R16(gSndItemMax));
    }
    if ((uint16_t)i >= 0xc8) i = 0;
    return (uint16_t)i;
}

/* 0004d4a0: item holding resource (id, type), or 0 */
uint32_t GetSndItemForId(int16_t id, int16_t type) {
    ENTRY_LINK(FN_GetSndItemForId, 0, 24);                   /* link 0; movem D3-D5/A2-A4 */
    maddr base = DEREF(R32(gSndItems));
    int16_t i = 1;
    if (R16(gSndItemMax) >= 1) {
        int32_t want = id;
        maddr tp = base + 0xa, idp = base + 8;
        do {
            /* zero-extended item fields against sign-extended arguments */
            if ((int32_t)RU16(idp) == want && (int32_t)RU16(tp) == (int32_t)type) break;
            i++;
            tp += 0xe;
            idp += 0xe;
        } while (i <= R16(gSndItemMax));
    }
    if (i > R16(gSndItemMax)) i = 0;
    return (uint16_t)i;
}

/* 0004d516 */
uint32_t GetSndItemType(int16_t item) {
    ENTRY_LINK(FN_GetSndItemType, 0, 12);                    /* link 0; movem A2/D4/D3 */
    int16_t t = 0;
    if (item > 0) t = R16(SND_ITEM(DEREF(R32(gSndItems)), item) + 0xa);
    return (uint16_t)t;
}

/* 0004d56c: stop the sounds and deactivate the audio hardware */
uint32_t PauseAudioSystem(void) {
    ENTRY_LINK(FN_PauseAudioSystem, 0x64, 8);                /* link -0x64; movem D4/D3 */
    if (R16(gAudioPaused) == 0) {
        W16(gAudioPaused, 1);
        ResetSoundItem(0);
        SetTimer(2, 0, 10);
        WaitTimerNoPoll(2);
        int16_t err = S16(audioActivate(0));
        if (err != 0) {
            call_ptr(JT_ADDR(FN_tc_sprintf), "lll", a6 - 0x64,
                     fn_address(FN_PauseAudioSystem) + 0x80, (uint32_t)(int32_t)err);
            Terminate(1, a6 - 0x64);
        }
    }
    return 0;
}

/* 0004d61e */
uint32_t PlaySound(int16_t item, int16_t id, uint32_t isSnd) {
    ENTRY_LINK(FN_PlaySound, 0, 4);                          /* link 0; D3 */
    int16_t i = S16(CreateSound(item, id, isSnd));
    SetSoundItem(i);
    return 0;
}

/* 0004d654: audio library callback (Pascal): cue messages and end of sound/music */
uint32_t PoPAudioCallBack(uint32_t isMusic, uint32_t msg, uint32_t ref) {
    int16_t op = R8(msg);
    if (op == 0) {
        if (isMusic == 0) W16(gSndCue, R8(R32(msg + 4)));
    } else if (op == 1 || op == 5) {
        if (isMusic == 0) {
            W16(gSndPlaying, 0);
            W16(gSndCue, 0);
            W32(gSndDoneRef, ref);
        } else {
            W16(gMusicPlaying, 0);
            W32(gMusicDoneRef, ref);
        }
    }
    W16(gAudioLastMsg, R8(msg));
    return 0;
}

/* 0004d6e2: destroy every item that is not playing */
uint32_t PurgeSounds(void) {
    ENTRY_LINK(FN_PurgeSounds, 4, 28);                       /* link -4; movem D3-D6/A2-A4 */
    int16_t snd = R16(gSndPlaying), mus = R16(gMusicPlaying);
    maddr base = DEREF(R32(gSndItems));
    int16_t i = 1;
    if (R16(gSndItemMax) >= 1) {
        maddr tp = base + 0xa, idp = base + 8;             /* not re-read after the calls */
        do {
            if (R16(idp) != 0) {
                int16_t t = R16(tp);
                if ((t == 0 && i != snd) || (t == 1 && i != mus)) DestroySoundItem(i);
            }
            i++;
            tp += 0xe;
            idp += 0xe;
        } while (i <= R16(gSndItemMax));
    }
    return 0;
}

/* 0004d750 */
uint32_t ReleaseSoundItem(int16_t item) {
    ENTRY_LINK(FN_ReleaseSoundItem, 0, 4);                   /* link 0; A2 */
    maddr it = SND_ITEM(DEREF(R32(gSndItems)), item);
    if (R16(it + 8) != 0 && S16(SoundItemOn(item)) != 0) ResetSoundItem(item);
    return 0;
}

/* 0004d7ba */
uint32_t ResetSoundId(int16_t id, int16_t type) {
    ENTRY_LINK(FN_ResetSoundId, 0, 4);                       /* link 0; D4 */
    int16_t item;
    if (id == 0) item = 0;
    else {
        item = S16(GetSndItemForId(id, type));
        if (item == 0) item = -1;
    }
    if (item >= 0) ResetSoundItem(item);
    return 0;
}

/* 0004d800: stop the music and/or sound playing (item 0: both) */
uint32_t ResetSoundItem(int16_t item) {
    ENTRY_LINK(FN_ResetSoundItem, 4, 4);                     /* link -4; A2 */
    maddr base = DEREF(R32(gSndItems));
    if (R16(gMusicPlaying) != 0 && (item == 0 || R16(gMusicPlaying) == item)) {
        StopAudioRef(R32(SND_ITEM(base, R16(gMusicPlaying))));
        W16(gMusicPlaying, 0);
    }
    if (R16(gSndPlaying) != 0 && (item == 0 || R16(gSndPlaying) == item)) {
        StopAudioRef(R32(SND_ITEM(base, R16(gSndPlaying))));
        W16(gSndPlaying, 0);
    }
    return 0;
}

/* 0004d8b0: reactivate the audio hardware */
uint32_t ResumeAudioSystem(void) {
    ENTRY_LINK(FN_ResumeAudioSystem, 0x64, 4);               /* link -0x64; D3 */
    if (R16(gAudioPaused) != 0) {
        W16(gAudioPaused, 0);
        int16_t err = S16(audioActivate(1));
        if (err != 0) {
            call_ptr(JT_ADDR(FN_tc_sprintf), "lll", a6 - 0x64,
                     fn_address(FN_ResumeAudioSystem) + 0x62, (uint32_t)(int32_t)err);
            Terminate(1, a6 - 0x64);
        }
    }
    return 0;
}

/* 0004d944: set the master volume (any non-zero level means the saved volume).
 * Writes its own argument. */
uint32_t SetVolumeLevel(int16_t level) {
    ENTRY_LINK(FN_SetVolumeLevel, 0, 0);                     /* link 0 */
    ResetSoundItem(0);
    SetTimer(0, 0, 10);
    WaitTimerNoPoll(0);
    if (level != 0 && R16(gMasterVolume) != -1) {
        level = R16(gMasterVolume);
        W16(args_, level);                                   /* move.w ...,(0x8,A6) */
    }
    return SetMasterVolume(level);
}

/* 0004d996 */
uint32_t SetSoundId(int16_t id, int16_t type) {
    ENTRY_LINK(FN_SetSoundId, 0xc, 4);                       /* link -0xc; D3 */
    int16_t item = S16(GetSndItemForId(id, type));
    if (item > 0) SetSoundItem(item);
    return (uint16_t)item;
}

/* 0004d9cc: start playing an item (stopping the music or sound of the same kind) */
uint32_t SetSoundItem(int16_t item) {
    ENTRY_LINK(FN_SetSoundItem, 8, 28);                      /* link -8; movem D3-D7/A3/A4 */
    _HLock(R32(gSndItems));
    maddr it = SND_ITEM(DEREF(R32(gSndItems)), item);
    if (R16(it + 8) == 0 || R32(it) == 0 || item <= 0 || item > R16(gSndItemMax)) return 0;
    if (R16(it + 0xa) == 0) {
        if (R16(gSndPlaying) != 0) ResetSoundItem(R16(gSndPlaying));
        if (R32(gSndDoneRef) != 0) {
            UnprepareAudioRef(R32(gSndDoneRef));
            W32(gSndDoneRef, 0);
        }
    } else {
        if (R16(gMusicPlaying) != 0) ResetSoundItem(R16(gMusicPlaying));
        if (R32(gMusicDoneRef) != 0) {
            UnprepareAudioRef(R32(gMusicDoneRef));
            W32(gMusicDoneRef, 0);
        }
    }
    uint32_t ref = R32(it);
    int16_t type = R16(it + 0xa);
    int16_t state = S16(audioGetState(ref));
    _HUnlock(R32(gSndItems));
    int16_t err = (state & 1) ? 0 : S16(audioPrepare(-1, ref));
    if (err != 0) return 0;
    if (S16(audioSeek(0, ref)) != 0) return 0;
    uint32_t cb = JT_ADDR(FN_PoPAudioCallBack), lt = (uint32_t)(int32_t)type;
    err = S16(audioStart((int16_t)(lt >> 16), (int16_t)lt, (int16_t)(cb >> 16), (int16_t)cb,
                         (int16_t)(ref >> 16), (int16_t)ref));
    if (err != 0) return 0;
    if (type == 0) W16(gSndPlaying, item);
    else W16(gMusicPlaying, item);
    _HLock(R32(gSndItems));
    it = SND_ITEM(DEREF(R32(gSndItems)), item);
    if (R16(it + 0xc) > 0) W16(it + 0xc, R16(it + 0xc) - 1);
    _HUnlock(R32(gSndItems));
    return 0;
}

/* 0004db48: set an item's use count */
uint32_t SetSoundItemKeepVal(int16_t item, int16_t val) {
    ENTRY_LINK(FN_SetSoundItemKeepVal, 4, 8);                /* link -4; movem A2/D3 */
    if (item <= R16(gSndItemMax)) {
        maddr it = SND_ITEM(DEREF(R32(gSndItems)), item);
        if (R16(it + 8) != 0) W16(it + 0xc, val);
    }
    return 0;
}

/* 0004dbb0 */
uint32_t SoundIdOn(int16_t id, int16_t type) {
    ENTRY_LINK(FN_SoundIdOn, 0, 8);                          /* link 0; movem D5/D4 */
    int16_t item, on = 0;
    if (id == 0) item = 0;
    else {
        item = S16(GetSndItemForId(id, type));
        if (item == 0) { item = -1; on = 0; }
    }
    if (item >= 0) on = S16(SoundItemOn(item));
    return (uint16_t)on;
}

/* 0004dbfe: is the item playing (item 0: anything)? */
uint32_t SoundItemOn(int16_t item) {
    if (item < 0 || item > R16(gSndItemMax)) return 0;
    if (item != 0) return (item == R16(gMusicPlaying) || item == R16(gSndPlaying)) ? 1 : 0;
    return (RU16(gMusicPlaying) | RU16(gSndPlaying)) != 0 ? 1 : 0;
}

/* 0004dc5c */
uint32_t StopAudioRef(uint32_t ref) {
    ENTRY_LINK(FN_StopAudioRef, 0, 4);                       /* link 0; D3 */
    int16_t err = 0;
    if (ref != 0) err = S16(audioStop(ref));
    return (uint16_t)err;
}

/* 0004dc8e */
uint32_t UnprepareAudioRef(uint32_t ref) {
    ENTRY_LINK(FN_UnprepareAudioRef, 4, 4);                  /* link -4; D3 */
    int16_t state = S16(audioGetState(ref));
    if (state & 1) audioUnprepare(ref);
    return 0;
}

/* 0004dcd0: wait until the music reaches cue `cue` (or stops, or a key/click) */
uint32_t WaitCue(int16_t cue) {
    ENTRY_LINK(FN_WaitCue, 0, 4);                            /* link 0; D3 */
    int16_t r;
    for (;;) {
        if (S16(SoundItemOn(0)) == 0) return 0;
        r = S16(CheckWaitFunc());
        if (R16(gSndCue) >= cue) break;
        if (r != 0) break;
    }
    return (uint16_t)r;
}

/* 0004dd0e */
uint32_t WaitSndId(int16_t id, int16_t type) {
    ENTRY_LINK(FN_WaitSndId, 0, 8);                          /* link 0; movem D5/D4 */
    int16_t item, r;
    if (id == 0) item = 0;
    else {
        item = S16(GetSndItemForId(id, type));
        if (item == 0) item = -1;
    }
    if (item >= 0) r = S16(WaitSndItem(item));
    else r = 0;
    return (uint16_t)r;
}

/* 0004dd5e: wait until the item stops (or a key/click) */
uint32_t WaitSndItem(int16_t item) {
    ENTRY_LINK(FN_WaitSndItem, 0, 4);                        /* link 0; D3 */
    int16_t r = 0;
    if (S16(SoundItemOn(item)) != 0) {
        do {
            r = S16(CheckWaitFunc());
            if (S16(SoundItemOn(item)) == 0) break;
        } while (r == 0);
    }
    return (uint16_t)r;
}

/* ================================ misc ================================ */

/* 0004dda4: address of a Toolbox trap, 0 when unimplemented */
uint32_t GetPOPTrap(int16_t trap) {
    ENTRY_LINK(FN_GetPOPTrap, 0, 8);                         /* link 0; movem A2/D3 */
    uint32_t a = tc_NGetTrapAddress(1 << 8, trap);           /* ToolTrap (byte) */
    uint32_t u = tc_NGetTrapAddress(1 << 8, 0x9f);           /* _Unimplemented */
    if (a != u) return tc_NGetTrapAddress(1 << 8, trap);
    return 0;
}

/* 0004ddfa: cache the rectangle traps called through pointers */
uint32_t FUN_0004ddfa(void) {
    ENTRY_NOLINK(FN_FUN_0004ddfa, 0);
    W32(pSectRect, GetPOPTrap((int16_t)0xa8aa));
    W32(pUnionRect, GetPOPTrap((int16_t)0xa8ab));
    W32(pEmptyRect, GetPOPTrap((int16_t)0xa8ae));
    W32(pClipRect, GetPOPTrap((int16_t)0xa87b));
    W32(pTrapGetKeys, GetPOPTrap((int16_t)0xa976));
    return 0;
}

/* 0004de42: debug: show the kid's sequence/position and wait for a space */
uint32_t CheckCheatModes(void) {
    ENTRY_LINK(FN_CheckCheatModes, 0x50, 4);                 /* link -0x50; D3 */
    if (R16(gCheatMode) == 0) return 0;
    FUN_000225d2();
    FUN_0004e31e();
    if (R16(gCheatStep) == 0) return 0;
    if (R16(gCheatStep) == 1) {
        call_ptr(JT_ADDR(FN_tc_sprintf), "llll", a6 - 0x50, fn_address(FN_CheckCheatModes) + 0x7e,
                 (uint32_t)(int32_t)R16(gKidSeq), (uint32_t)RU16(gKidPos));   /* "Seq %d, Pos %d" */
        DrawMessage(a6 - 0x50);
        W16(gMsgTimer, 0x18);
        W16(gMsgTimer2, 0x18);
    }
    while (S16(FUN_0004e40c()) != 0x20) {}
    return 0;
}

/* 0004ded0 */
uint32_t CheckMemTest(void) {
    return 0;
}

/* 0004dee8: debug mark (a small arrow) at x on the character's row */
uint32_t DrawTestMarker(int16_t x, int16_t hi, int16_t color) {
    ENTRY_LINK(FN_DrawTestMarker, 0, 8);                     /* link 0; movem D4/D3 */
    int16_t oldColor = (int16_t)R32(R32(gCurPort) + 0x50);
    int16_t y = (int16_t)(R16(gRowY + (int16_t)(R16(CH_ROW) * 2)) - 0xb);
    SetForeColor(color);
    _MoveTo(x, y);
    _LineTo(x, (int16_t)(y + 5));
    _MoveTo(x, y);
    _LineTo((int16_t)(x + 2), (int16_t)(y + 2));
    _MoveTo(x, y);
    _LineTo((int16_t)(x - 2), (int16_t)(y + 2));
    SetForeColor(oldColor);
    return 0;
}

/* 0004df7e */
uint32_t FUN_0004df7e(void) {
    return 0;
}

/* 0004df80: numeric value of dialog item `item` of the front window, defaulting/clamped
 * to [min, max] (the text is reset to min or def); min - 1 when out of range */
uint32_t GetEditTextNumericValue(int16_t item, int16_t min, int16_t max, int16_t def) {
    ENTRY_LINK(FN_GetEditTextNumericValue, 0x114, 8);        /* link -0x114; movem D5/D3 */
    maddr text = a6 - 0x114;
    _GetDialogItem(_FrontWindow(), item, a6 - 0x14, a6 - 0x10, a6 - 0xc);
    _GetDialogItemText(R32(a6 - 0x10), text);
    tc_PtoCstr(text);
    int16_t v;
    if (tc_strlen(text) != 0) {
        v = S16(tc_strtol(text, a6 - 4, 0));
    } else {
        v = min;
        tc_NumToString(text, (uint32_t)(int32_t)min);
        _SetDialogItemText(R32(a6 - 0x10), text);
    }
    if (v < min || v > max) {
        v = (int16_t)(min - 1);
        tc_NumToString(text, (uint32_t)(int32_t)def);
        _SetDialogItemText(R32(a6 - 0x10), text);
    }
    return (uint16_t)v;
}

/* 0004e04c: ModalDialog filter: only digits and backspace (Pascal, Boolean result) */
uint32_t GetNumberFilter(uint32_t itemHit, uint32_t event, uint32_t dialog) {
    ENTRY_LINK_S(FN_GetNumberFilter, 2, 8, 8);               /* link -8; movem A2/D3 */
    _GetPort(a6 - 4);
    _SetPort(dialog);
    W8(a6 - 8, 0);
    if ((int32_t)R16(event) == 3) {                          /* keyDown */
        uint16_t c = (uint8_t)R32(event + 2);
        int ok = (c >= 0x30 && c <= 0x39) || c == 8;
        W8(a6 - 8, ok ? 0 : 1);
        W16(itemHit, 4);
    }
    _SetPort(R32(a6 - 4));
    return R8(a6 - 8);
}
void glue_GetNumberFilter(void) {
    maddr sp = emu_sp();
    uint32_t r = GetNumberFilter(R32(sp), R32(sp + 4), R32(sp + 8));
    W8(sp + 12, r);                                          /* move.b D0b,(0x14,A6) */
    emu_set_sp(sp + 12);
    emu_set_d0(r);
}

/* 0004e0d6: dialog 0x721 asking for a number (prompt, initial value); *result is the
 * value (or init on Cancel). Pascal, Boolean result: OK clicked. */
uint32_t GetNumber(uint32_t result, int16_t init, int16_t max, int16_t min, uint32_t prompt) {
    ENTRY_LINK_S(FN_GetNumber, 2, 0x80, 16);                 /* link -0x80; movem D3-D5/A2 */
    maddr text = a6 - 0x68;
    _GetPort(a6 - 4);
    uint32_t dlg = _GetNewDialog(0x721, 0, 0xffffffffu);
    _SetPort(dlg);
    CenterWindow(dlg, 0);
    _ShowWindow(dlg);
    FrameDefault(dlg, 1);
    _GetDialogItem(dlg, 4, a6 - 0x7c, a6 - 0x78, a6 - 0x74);
    tc_strcpy(text, prompt);
    tc_CtoPstr(text);
    _SetDialogItemText(R32(a6 - 0x78), text);
    _GetDialogItem(dlg, 3, a6 - 0x7c, a6 - 0x78, a6 - 0x74);
    call_ptr(JT_ADDR(FN_tc_sprintf), "lll", text, fn_address(FN_GetNumber) + 0x144,
             (uint32_t)(int32_t)init);                       /* "%d" */
    tc_CtoPstr(text);
    _SetDialogItemText(R32(a6 - 0x78), text);
    _SelectDialogItemText(dlg, 3, 0, 0x7fff);
    int16_t hit;
    do {
        _ModalDialog(0, a6 - 0x7e);
        hit = R16(a6 - 0x7e);
    } while (hit != 1 && hit != 2);
    uint8_t ok = hit == 1;
    W8(a6 - 0x80, ok);
    if (ok) W16(result, GetEditTextNumericValue(3, min, max, init));
    else W16(result, init);
    _SetPort(R32(a6 - 4));
    _DisposeDialog(dlg);
    return R8(a6 - 0x80);
}
void glue_GetNumber(void) {
    maddr sp = emu_sp();
    uint32_t r = GetNumber(R32(sp), (int16_t)RU16(sp + 4), (int16_t)RU16(sp + 6), (int16_t)RU16(sp + 8), R32(sp + 10));
    W8(sp + 14, r);                                          /* move.b D0b,(0x16,A6) */
    emu_set_sp(sp + 14);
    emu_set_d0(r);
}

/* 0004e21e: debug: ask for a level number and jump to it */
uint32_t GoToLevelDialog(void) {
    ENTRY_LINK(FN_GoToLevelDialog, 4, 4);                    /* link -4; D3 */
    ResetSoundItem(0);
    uint8_t ok = (uint8_t)GetNumber(a6 - 4, R16(R32(gGameState) + 0x218a), 0xe, 1,
                                    fn_address(FN_GoToLevelDialog) + 0x6e);   /* "Enter level number..." */
    if (ok != 0 && R16(a6 - 4) != 0) {
        int16_t lv = R16(a6 - 4);
        W16(gNISLevel, lv);
        W16(gLevelLoaded, lv);
        W16(gLevelJump, 1);
    } else {
        W16(a6 - 4, 0);
    }
    return RU16(a6 - 4);
}

/* 0004e2a2 */
uint32_t CheckHeap(void) {
    return 0;
}

/* 0004e2b8: clr.w D0 */
uint32_t FUN_0004e2b8(void) {
    return cpu_get(R_D0) & 0xffff0000u;
}

/* 0004e2bc: clear the message shown */
uint32_t seg16_04e2bc(void) {
    ENTRY_NOLINK(FN_seg16_04e2bc, 0);
    if (R16(gMsgShown) > 0) {
        W16(gMsg70, 0x14);
        W16(gMsgShown, -1);
        ClrMessage(1);
        if (R16(gLevelJump) != 0) W16(gLevelJump, 0);
    }
    return 0;
}

/* 0004e2e8 */
uint32_t seg16_04e2e8(void) {
    ENTRY_NOLINK(FN_seg16_04e2e8, 0);
    if (R16(gLevelReq) >= 3) AddSong(0x18);
    else AddSound(0x12c);
    FUN_00015828();
    mcopy(gKid, gChar, 0x3e);
    return 0;
}

/* 0004e31e */
uint32_t FUN_0004e31e(void) {
    return 0;
}

/* 0004e320: debug: mark the foot position of character record `ch` */
uint32_t DrawFootmark(uint32_t ch, int16_t on) {
    ENTRY_LINK(FN_DrawFootmark, 0x4c, 8);                    /* link -0x4c; movem D4/D3 */
    mcopy(a6 - 0x4c, gChar, 0x3e);
    mcopy(a6 - 0xc, gChar_ee, 10);
    mcopy(gChar, ch, 0x3e);
    seg4_02055e();
    int16_t x = (int16_t)(S16(seg4_021542()) - 0xcf);
    int16_t color = on != 0 ? 2 : 0;
    DrawTestMarker(x, 0, color);
    mcopy(gChar_ee, a6 - 0xc, 10);
    mcopy(gChar, a6 - 0x4c, 0x3e);
    return 0;
}

/* 0004e3be: debug: mark the collision x of character record `ch` */
uint32_t DrawCollideMark(uint32_t ch, int16_t on) {
    ENTRY_LINK(FN_DrawCollideMark, 0, 8);                    /* link 0; movem D4/D3 */
    int16_t x = (int16_t)(R16(ch + 4) - 0xcf);
    int16_t color = on != 0 ? 4 : 0;
    DrawTestMarker(x, 0, color);
    return 0;
}

/* 0004e40c: wait for a key (FUN_0005165e), returns it */
uint32_t FUN_0004e40c(void) {
    ENTRY_NOLINK(FN_FUN_0004e40c, 12);                       /* movem D5/D4/D3 */
    int16_t k;
    for (;;) {
        k = S16(FUN_0005165e());
        if (k == 0) continue;
        int32_t z = (uint16_t)k;                             /* zero-extended: never -1/-2 */
        if (z == -1 || z == -2) continue;
        break;
    }
    return (uint16_t)k;
}
