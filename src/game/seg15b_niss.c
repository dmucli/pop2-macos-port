/* CODE 15 "NISs", second half (Ghidra 0x480ba-0x4b5e4): the scripts of the cut scenes
 * (NIS = non-interactive sequence): the happy ending (HappyFunc), the opening story
 * (Opening1/Opening2), the temple, throne room, title and tower transitions, and the
 * per-frame "transition functions" (TransFuncN) that PlayTheAnimation calls back.
 * Ported from the Ghidra decompilation / disassembly of the original 68k code; addresses
 * in comments are Ghidra addresses (see docs/PORTING.md and docs/notes/seg15b.md). */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* ---- globals (Ghidra addresses) ---- */
#define gMainWindow     GH(0x102dbe)   /* long: the game window */
#define gOffWorld       GH(0x102dc2)   /* the off-screen world record (Save/RestoreCurrentWorld) */
#define gGameState      GH(0x102fec)   /* long: pointer to the game state block */
#define GS_LEVELTYPE    0x2186         /*   word: level (graphics set) type */
#define gKidFace        GH(0x102f30)   /* word: kid facing (0 = left) */
#define gKidX           GH(0x102f32)   /* word: kid x */
#define gKidY           GH(0x102f34)   /* word: kid y */
#define gPlayRect       GH(0x103014)   /* Rect: the play area */
#define gSavedPalette   GH(0x103564)   /* long: saved game-play palette handle (SetKidPalette) */
#define gSoundRsrc      GH(0x103812)   /* long */
#define gHappyText      GH(0x105822)   /* word: 1 while HappyDialog draws its text */
#define gInThroneNIS    GH(0x105824)   /* word: 1 while the throne room NIS runs */
#define gMapSave1       GH(0x105826)   /* long: SaveRect save of the title / throne backdrop */
#define gMapSave2       GH(0x10582a)   /* long: SaveRect save of the queen area */
#define gNISBoxRect     GH(0x105832)   /* Rect */
#define gNISRect        GH(0x10583a)   /* Rect: the NIS picture frame (moved to 0x40,0x3e) */
#define gCueNext        GH(0x105b9c)   /* word \                                        */
#define gCueParam       GH(0x105b9e)   /* word  | cue settings used by the sound cue    */
#define gCueSnd         GH(0x105ba0)   /* word  | routines of CODE 15 (WaitCue...)       */
#define gCueOn          GH(0x105ba2)   /* word /                                        */
#define gSavedKeyProc   GH(0x105ba8)   /* long: key reader to restore after an NIS */
#define gCTab1          GH(0x105bac)   /* long: first NIS colour table (GetSetFadeCTAB) */
#define gCTab2          GH(0x105bb0)   /* long: second NIS colour table */
#define gHappyWait      GH(0x105cbc)   /* word: HappyFunc: frames before the next dialog */
#define gHappyLine      GH(0x105cbe)   /* word: HappyFunc: next dialog line (100 = done) */
#define gHappyFrameAdj  GH(0x105cc0)   /* word: HappyFunc: frame number adjustment */
#define gOpenFlyId      GH(0x105cc2)   /* word[8]: Opening2 flight: NIS shape id     */
#define gOpenFlyX       GH(0x105cd2)   /* word[8]:                  x                */
#define gOpenFlyY       GH(0x105ce2)   /* word[8]:                  y                */
#define gOpenFlyTime    GH(0x105cf2)   /* word[8]:                  ticks to show    */
#define gOpenFlyStep    GH(0x105d02)   /* word: Opening2 flight step */
#define gQueenRect      GH(0x105d04)   /* Rect: the queen (throne room) */
#define gTitleRect1     GH(0x105d0c)   /* Rect: title, first word art */
#define gTitleRect2     GH(0x105d14)   /* Rect: title, second word art */
#define gAnimShapes     GH(0x105d74)   /* long: handle to the animation's shape handles */
#define gNISClipRect    GH(0x105f58)   /* Rect: area the animation redraws */
#define gAnimSndId      GH(0x106018)   /* word */
#define gAnimSndOn      GH(0x10601a)   /* word */
#define gNoHappyText    GH(0x106084)   /* word: skip the happy ending dialog */
#define gMidiFile       GH(0x10608a)   /* long */
#define gSndFile        GH(0x10608e)   /* long */
#define gAnimFrame      GH(0x106092)   /* word: frame number of the running animation */
#define pClipRect       GH(0x10609c)   /* long: ClipRect procedure pointer */
#define pUnionRect      GH(0x1060a4)   /* long: UnionRect procedure pointer */
#define gGameRect       GH(0x106204)   /* Rect: the game area */
#define gReadKeyProc    GH(0x10620c)   /* long: keyboard reader procedure */
/* resource-file records for OpenRsrcVerify/CloseRsrcFile */
#define gRsrcTemple     GH(0x1038f4)
#define gRsrcLevel      GH(0x103900)
#define gRsrcTitle      GH(0x105944)
#define gRsrcTower      GH(0x10594a)
#define gRsrcThrone     GH(0x105956)
#define gRsrcNIS        GH(0x10595c)
#define gRsrcTempleNIS  GH(0x105962)

#define GS()            R32(gGameState)
#define FADE_SPEED      0x40001        /* the long most fade calls pass (step 4, 1 tick) */
#define FADE_SLOW       0x90001

/* ---- helpers ---- */

/* ClipRect(r) through the pointer cached at A5-0x1f64 (Pascal: the callee pops r) */
static void clip_rect_ptr(maddr r) {
    maddr sp0 = emu_sp();
    emu_set_sp(sp0 - 4);
    W32(sp0 - 4, r);
    call68_addr(R32(pClipRect));
    emu_set_sp(sp0);
}

/* UnionRect(a, b, dst) through the pointer cached at A5-0x1f5c (Pascal) */
static void union_rect_ptr(maddr a, maddr b, maddr dst) {
    maddr sp0 = emu_sp();
    emu_set_sp(sp0 - 12);
    W32(sp0 - 4, a);
    W32(sp0 - 8, b);
    W32(sp0 - 12, dst);
    call68_addr(R32(pUnionRect));
    emu_set_sp(sp0);
}

/* The callers of NISDialog push a fourth word that it never reads: push it too so that
 * its frame is where the original's is. */
static int16_t nis_dialog(int16_t strs, int16_t idx, int16_t snd, int16_t unused) {
    maddr sp = emu_sp() - 2;
    emu_set_sp(sp);
    W16(sp, unused);
    int16_t r = S16(NISDialog(strs, idx, snd));
    emu_set_sp(sp + 2);
    return r;
}

static void copy_rect(maddr dst, maddr src) {
    W32(dst, R32(src));
    W32(dst + 4, R32(src + 4));
}

/* A Boolean pushed with move.b sits in the high byte of its word. */
#define B(x)            ((int16_t)((x) << 8))

/* 000480ba: per-frame function of the happy ending animation: palette fades by frame
 * number and the final dialog lines (STR# 30000).  The original adds the frame
 * adjustment to its own argument slot (glue_HappyFunc). */
static uint32_t happy_func(int16_t n, maddr slot) {
    ENTRY_LINK(FN_HappyFunc, 8, 20);                 /* link -8; movem A2/D3-D6 */
    int16_t res = 0;
    if (n > 0x82 && n < 0x8e && R16(gAnimFrame) >= 0x66 && R16(gHappyFrameAdj) == 0)
        W16(gHappyFrameAdj, 0x8e - n);
    n = (int16_t)(n + R16(gHappyFrameAdj));
    if (slot) W16(slot, n);
    switch (n) {
    case 0:
        SetGameRect(gNISClipRect);
        break;
    case 2:
        FadeInBothCTBLs(FADE_SPEED);
        break;
    case 0x1e:
        GetSetFadeCTAB(0x753c, 0x1000, FADE_SPEED);
        break;
    case 0x20: case 0x22: case 0x24: case 0x26: case 0x28:
        SetNISCTAB(0x4050, (int16_t)(100 - (int16_t)(n * 10 - 0x12c)));
        break;
    case 0x8e: case 0x8f: case 0x90: case 0x91: case 0x92:
    case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        SetNISCTAB(0x51, (int16_t)(100 - (int16_t)(n * 10 - 0x582)));
        break;
    case 0x98: {
        SaveCurrentWorld(gOffWorld);
        uint32_t rgn = _NewRgn();
        _GetClip(rgn);
        SetGameRect(gNISClipRect);
        clip_rect_ptr(gNISClipRect);
        RestoreOriginalWorld(gOffWorld);
        AddNIS(0x6322, 0, 0, 0x10, 0);
        AddNIS(0x6326, 0x103, 0xa1, 0x10, 0);
        SaveCurrentWorld(gOffWorld);
        _SetClip(rgn);
        _DisposeRgn(rgn);
        RestoreOriginalWorld(gOffWorld);
        GetSetFadeCTAB(0x76aa, 0x45, FADE_SPEED);
        break;
    }
    case 0x99:
        GetSetFadeCTAB(0x753c, 2, FADE_SPEED);
        break;
    case 0x9a:
        GetSetFadeCTAB(0x753d, 0x1000, FADE_SPEED);
        break;
    case 0xb6:
        SetSoundId(0x7544, 0);
        SetGameRect(a6 - 8);
        DrawSOffWorldToWindow(gOffWorld, R32(gMainWindow), a6 - 8, a6 - 8, 0);
        break;
    case 0xb7: case 0xb8: case 0xb9: case 0xba: case 0xbb:
    case 0xbc: case 0xbd: case 0xbe: case 0xbf: case 0xc0: {
        int16_t v = (int16_t)(n * 10 - 0x71c);
        SetNISCTAB(0x51, v);
        if ((n & 1) == 0) SetNISCTAB(0x4050, (int16_t)(100 - v));
        break;
    }
    case 0xc1:
        GetSetFadeCTAB(0x753d, 2, FADE_SPEED);
        break;
    }
    if (n > 2 && R16(gNoHappyText) == 0) {
        if (R16(gHappyWait) == 0) {
            int16_t line = R16(gHappyLine);
            if (line <= 4) {
                SetSoundId((int16_t)(line + 0x7534), 1);
                HappyDialog(0x7530, (int16_t)(R16(gHappyLine) + 4));
                W16(gHappyLine, R16(gHappyLine) + 1);
                W16(gHappyWait, 1);
            } else if (R16(gHappyLine) < 100) {
                HappyDialog(0, 0);
                W16(gHappyLine, 100);
            }
        } else {
            W16(gHappyWait, R16(gHappyWait) - 1);
        }
    }
    if (n > 0xb6) {
        if ((uint16_t)SoundIdOn(0x7544, 0) == 0) res = -1;
    }
    return (uint16_t)res;
}
uint32_t HappyFunc(int16_t n) {
    return happy_func(n, 0);
}
void glue_HappyFunc(void) {
    maddr sp = emu_sp();
    emu_set_d0(happy_func(R16(sp), sp));
}

/* 00048450: draw a line of the happy ending text (flag gHappyText while drawing) */
uint32_t HappyDialog(int16_t strs, int16_t idx) {
    ENTRY_LINK(FN_HappyDialog, 0, 0);
    W16(gHappyText, 1);
    NISTextDraw(strs, idx);
    W16(gHappyText, 0);
    return 0;
}

/* 00048482: Jaffar burns (animation 31000) */
uint32_t JaffarBurnBabyBurn(void) {
    ENTRY_LINK(FN_JaffarBurnBabyBurn, 0xc, 4);      /* link -0xc; move.l D3 */
    maddr rect = a6 - 0xc;
    GetSetFadeCTAB(0x7919, 0x45, FADE_SPEED);
    SetGameRect(rect);
    clip_rect_ptr(rect);
    SaveCurrentWorld(gOffWorld);
    clip_rect_ptr(rect);
    RestoreOriginalWorld(gOffWorld);
    AddNIS(0x7a47, 0, 0, 0, rect);
    CopyNIS(rect);
    int16_t r = S16(PlayTheAnimation(0x7918, 1, 1, JT_ADDR(FN_TransFuncSetCTABframe1), B(0), 0,
                                     B(1), 0xe, 0, 0, 0));
    if (r == 0) r = S16(Wait(0, 0x3c));
    if (r == 0) FadeOutColors(R32(gCTab1), R32(gMainWindow), FADE_SPEED);
    return (uint16_t)r;
}

/* 00048562: the opening story, first part (the Sultan, the Prince and Jaffar) */
uint32_t Opening1(void) {
    ENTRY_LINK(FN_Opening1, 0x10, 8);               /* link -0x10; movem D3-D4 */
    maddr full = a6 - 0x10, frame = a6 - 8;
    int16_t d4;
    FUN_00044c22();
    SetGameRect(full);
    ClearNIS(gOffWorld, 1, full);
    copy_rect(frame, gNISRect);
    MoveRect(frame, 0x40, 0x3e);
    PlaySound(-1, 0x61b2, 0);
    GetSetFadeCTAB(0x61a9, 0x45, FADE_SPEED);
    NISTextDraw(0, 0);
    AddNIS(0x61a9, 0, 0, 0, full);
    AddNIS(0x61aa, 0, 0x129, 0, 0);
    ClearNIS(gOffWorld, 0xf, frame);
    CopyNIS(full);
    d4 = S16(FUN_00044bd6());
    if (d4 == 0) {
        FadeInColors(R32(gCTab1), R32(gMainWindow), FADE_SLOW);
        d4 = S16(FUN_00044bd6());
    }
    if (d4 == 0) {
        GetSetFadeCTAB(0x6979, 0x52, FADE_SLOW);
        ClearNIS(gOffWorld, 0xf, frame);
        AddNIS(0x6979, 0xbf, 0x89, 0x10, full);
        d4 = S16(WaitCue(0x61));
        SetTimer(0, 0, 0xa5);
        if (d4 == 0) d4 = S16(DissolveNIS(0x2d));
        if (d4 == 0) d4 = S16(WaitTimer(0));
    }
    if (d4 == 0) {
        ClearNIS(gOffWorld, 0xf, frame);
        if (d4 == 0) d4 = S16(DissolveNIS(0x2d));
    }
    if (d4 == 0) {
        AddNIS(0x697a, 0xa7, 0x98, 0x10, full);
        d4 = S16(WaitCue(0x62));
        SetTimer(0, 0, 0xa5);
        if (d4 == 0) d4 = S16(DissolveNIS(0x2d));
        if (d4 == 0) d4 = S16(WaitTimer(0));
    }
    if (d4 == 0) {
        SetTimer(0, 0, 0x5f);
        ClearNIS(gOffWorld, 0xf, frame);
        if (d4 == 0) d4 = S16(DissolveNIS(0x2d));
        if (d4 == 0) d4 = S16(WaitTimer(0));
    }
    if (d4 == 0 && (d4 = S16(WaitCue(0x63))) == 0) {
        FadeIndex(0xf, 0xa8, 0x11, 2, 3);
        GetSetFadeCTAB(0x697b, 0x46, FADE_SPEED);
        AddNIS(0x697b, 0x40, 0x3e, 0, full);
        if (d4 == 0) {
            CopyNIS(frame);
            CreateSound(-1, 0x6979, 1);
            FadeInColors(R32(gCTab2), R32(gMainWindow), FADE_SPEED);
        }
    }
    if (d4 == 0) {
        SetTimer(0, 0, 0x5a);
        W16(gCueSnd, 0x61ac);
        W16(gCueParam, 0x80);
        W16(gCueNext, -2);
        W16(gCueOn, 1);
        SetSoundId(0x6979, 1);
        NISTextDraw(0x6978, 1);
        WaitTimer(0);
    }
    if (d4 == 0) {
        NISTextDraw(0x6978, 2);
        d4 = S16(WaitCurrentSound(1));
    }
    if (d4 == 0) {
        SetTimer(0, 0, 0x2d);
        CreateSound(-1, 0x697a, 1);
        d4 = S16(WaitTimer(0));
    }
    if (d4 == 0) d4 = nis_dialog(0x6978, 3, 0x697a, 0xb8);
    if (d4 == 0) {
        SetTimer(0, 0, 0x55);
        CreateSound(-1, 0x697b, 1);
        d4 = S16(WaitTimer(0));
    }
    if (d4 == 0) {
        SetSoundId(0x697b, 1);
        SetTimer(0, 0, 0xaa);
        NISTextDraw(0x6978, 4);
        d4 = S16(WaitTimer(0));
    }
    if (d4 == 0) {
        NISTextDraw(0x6978, 5);
        GetSetFadeCTAB(0x697c, 0x66, 0x10001);
        AddNIS(0x697c, 0x40, 0x3e, 0, full);
        CopyNIS(frame);
        d4 = S16(WaitCue(100));
    }
    if (d4 == 0) {
        NISTextDraw(0, 0);
        FadeInColors(R32(gCTab2), R32(gMainWindow), FADE_SPEED);
    }
    if (d4 == 0) {
        CreateSound(-1, 0x697c, 1);
        d4 = S16(WaitCue(0x65));
    }
    if (d4 == 0) d4 = nis_dialog(0x6978, 6, 0x697c, 0xdf);
    if (d4 == 0) {
        SetTimer(0, 0, 0x14);
        CreateSound(-1, 0x697d, 1);
        d4 = S16(WaitTimer(0));
    }
    if (d4 == 0) d4 = nis_dialog(0x6978, 7, 0x697d, 0x87);
    if (d4 == 0) {
        CreateSound(-1, 0x697e, 1);
        d4 = S16(WaitCue(0x66));
    }
    if (d4 == 0) {
        SetTimer(0, 0, 0xa0);
        SetSoundId(0x697e, 1);
        NISTextDraw(0x6978, 8);
        d4 = S16(WaitTimer(0));
    }
    if (d4 == 0) {
        AddNIS(0x697d, 0x40, 0x3e, 0x10, full);
        DissolveNIS(0x2d);
        d4 = S16(WaitCurrentSound(1));
    }
    if (d4 == 0) {
        SetTimer(0, 0, 0x1e);
        CreateSound(-1, 0x697f, 1);
        d4 = S16(WaitTimer(0));
    }
    if (d4 == 0) d4 = nis_dialog(0x6978, 9, 0x697f, 0x83);
    if (d4 == 0) d4 = S16(Wait(0, 0x14));
    if (d4 == 0) {
        NISTextDraw(0, 0);
        AddNIS(0x697e, 0x40, 0x3e, 0, full);
        d4 = S16(DissolveNIS(0x3c));
    }
    if (d4 == 0) {
        SetTimer(0, 0, 0x12);
        CreateSound(-1, 0x6980, 1);
        d4 = S16(WaitTimer(0));
    }
    if (d4 == 0 && (d4 = nis_dialog(0x6978, 10, 0x6980, 0xfa)) == 0) {
        SetTimer(0, 0, 0x14);
        CreateSound(-1, 0x6981, 1);
        d4 = S16(WaitTimer(0));
    }
    if (d4 == 0) {
        SetSoundId(0x6981, 1);
        NISTextDraw(0x6978, 0xb);
        AddNIS(0x697f, 0x44, 0xaa, 0x10, full);
        d4 = S16(DissolveNIS(0x2d));
    }
    if (d4 == 0) {
        CreateSound(-1, 0x61b3, 0);
        CreateSound(-1, 0x6982, 1);
        AddNIS(0x6980, 0x40, 0x3e, 0, full);
        d4 = S16(WaitSndId(0x61b2, 0));
        if (d4 == 0) {
            GetSetFadeCTAB(0x6980, 0x66, FADE_SPEED);
            NISTextDraw(0, 0);
            CopyNIS(frame);
            FadeInColors(R32(gCTab2), R32(gMainWindow), FADE_SPEED);
            SetSoundId(0x61b3, 0);
        }
    }
    if (d4 == 0 && (d4 = nis_dialog(0x6978, 0xc, 0x6982, 0x7c)) == 0) {
        SetTimer(0, 0, 0x14);
        CreateSound(-1, 0x6983, 1);
        d4 = S16(WaitTimer(0));
    }
    if (d4 == 0) d4 = nis_dialog(0x6978, 0xd, 0x6983, 0x75);
    if (d4 == 0) {
        SetTimer(0, 0, 0x14);
        CreateSound(-1, 0x6984, 1);
        d4 = S16(WaitTimer(0));
    }
    if (d4 == 0) d4 = nis_dialog(0x6978, 0xe, 0x6984, 0xb0);
    if (d4 == 0) {
        NISTextDraw(0, 0);
        AddNIS(0x6981, 0x40, 0x3e, 0, full);
        d4 = S16(WaitCue(0x61));
        GetSetFadeCTAB(0x6981, 0x66, 0x40002);
        CopyNIS(frame);
    }
    if (d4 == 0) FadeInColors(R32(gCTab2), R32(gMainWindow), 0x40002);
    if (d4 == 0) {
        CreateSound(-1, 0x6985, 1);
        d4 = S16(WaitCue(0x62));
    }
    if (d4 == 0) d4 = nis_dialog(0x6978, 0xf, 0x6985, 0xd0);
    if (d4 == 0) {
        CreateSound(-1, 0x6986, 1);
        d4 = S16(WaitCue(99));
    }
    if (d4 == 0) d4 = nis_dialog(0x6978, 0x10, 0x6986, 0x91);
    if (d4 == 0) d4 = S16(Wait(0, 0x5a));
    if (d4 == 0) {
        FadeBothCTBLs(FADE_SPEED);
    } else {
        ResetSoundItem(0);
        RapidFadeALL(0);
    }
    W32(gReadKeyProc, R32(gSavedKeyProc));
    return (uint16_t)d4;
}

/* 00048e88: the opening story, second part (the Prince's return, the flight) */
uint32_t Opening2(void) {
    ENTRY_LINK(FN_Opening2, 0x2c, 32);              /* link -0x2c; movem D3-D7/A2-A4 */
    maddr full = a6 - 0x2c, frame = a6 - 0x1c, endrect = a6 - 0x14;
    maddr lh4 = a6 - 0xc;                           /* long: 4th shape handle */
    maddr lw8 = a6 - 8, lw4 = a6 - 4;               /* words spilled by the loop */
    uint32_t h1, h2, h3;                            /* A2, A3, A4 */
    int16_t d6;
    FUN_00044c22();
    SetGameRect(full);
    ClearNIS(gOffWorld, 1, full);
    copy_rect(frame, gNISRect);
    MoveRect(frame, 0x40, 0x3e);
    GetSetFadeCTAB(0x61a9, 0x45, FADE_SPEED);
    NISTextDraw(0, 0);
    AddNIS(0x61a9, 0, 0, 0, full);
    AddNIS(0x61aa, 0, 0x129, 0, 0);
    ClearNIS(gOffWorld, 0xff, frame);
    CopyNIS(full);
    SaveCurrentWorld(gOffWorld);
    clip_rect_ptr(frame);
    RestoreOriginalWorld(gOffWorld);
    GetSetFadeCTAB(0x6985, 0x46, FADE_SPEED);
    CreateSound(-1, 0x61b4, 0);
    d6 = S16(WaitCue(0x6a));
    if (d6 == 0) {
        FadeInColors(R32(gCTab1), R32(gMainWindow), FADE_SPEED);
        AddNIS(0x6985, 0x3e, 0x3e, 0x10, 0);
        AddNIS(0x6986, 0xf9, 0x3e, 0x10, 0);
        AddNIS(0x6987, 0x12e, 0x69, 0x10, 0);
        CopyNIS(frame);
        d6 = S16(WaitSndId(0x61b3, 0));
    }
    if (d6 == 0) {
        SetSoundId(0x61b4, 0);
        FadeInColors(R32(gCTab2), R32(gMainWindow), FADE_SLOW);
        W16(gCueSnd, 0x61ab);
        W16(gCueParam, 0x96);
        W16(gCueNext, -2);
        W16(gCueOn, 1);
        CreateSound(-1, 0x6987, 1);
        d6 = S16(WaitCue(0x61));
    }
    if (d6 == 0) {
        SetTimer(0, 0, 0x46);
        SetSoundId(0x6987, 1);
        NISTextDraw(0x6978, 0x11);
        d6 = S16(WaitTimer(0));
        if (d6 == 0) NISTextDraw(0x6978, 0x12);
    }
    if (d6 == 0) {
        CreateSound(-1, 0x6988, 1);
        ClearNIS(gOffWorld, 0xff, frame);
        AddNIS(0x6985, 0x3e, 0x3e, 0x10, full);
        AddNIS(0x6986, 0xf9, 0x3e, 0x10, full);
        AddNIS(0x6988, 0x130, 0x69, 0x10, full);
        WaitCurrentSound(1);
        d6 = S16(WaitCue(0x62));
    }
    if (d6 == 0) {
        SetSoundId(0x6988, 1);
        NISTextDraw(0x6978, 0x13);
        d6 = S16(DissolveNIS(0x12c));
    }
    if (d6 == 0) {
        CreateSound(-1, 0x7727, 1);
        d6 = S16(WaitSndItem(0));
    }
    if (d6 == 0) {
        NISTextDraw(0, 0);
        h1 = GetShape(0x6989);
        h2 = GetShape(0x6985);
        h3 = GetShape(0x6986);
        uint32_t h4 = GetShape(0x6988);
        W32(lh4, h4);
        if (h1 == 0 || h2 == 0 || h3 == 0 || h4 == 0) return 1;
        _HLock(h1);
        _HLock(h2);
        _HLock(h3);
        _HLock(R32(lh4));
        SetSoundId(0x7727, 1);
        int32_t i = 1;                              /* D5 (long) */
        if (i < 0x1a) {
            int16_t x = (int16_t)((int16_t)i << 3);        /* D7 */
            W16(lw4, x);
            x = (int16_t)(x + 0xf9);
            W16(lw8, R16(lw4));
            do {
                if (d6 == 0) {
                    int32_t t = 0xe - i;
                    int32_t ticks = 0;
                    if (t >= 0) ticks = t;
                    SetTimer(0, (int16_t)((uint32_t)ticks >> 16), (int16_t)ticks);
                    SaveCurrentWorld(gOffWorld);
                    DrawIt(R32(h1), 0, 0x40, 0x3e, 0, 0, 0);
                    DrawIt(R32(h2), 0, (int16_t)(0x3e - R16(lw8)), 0x3e, 0, 0, 0);
                    DrawIt(R32(h3), 0, x, 0x3e, 0, 0, 0);
                    DrawIt(R32(R32(lh4)), 0, 0x130, 0x69, 0x10, 0, 0);
                    RestoreOriginalWorld(gOffWorld);
                    CopyNIS(frame);
                    d6 = S16(WaitTimer(0));
                }
                x = (int16_t)(x + 8);
                W16(lw8, R16(lw8) + 8);
                i++;
            } while (i < 0x1a);
        }
        _HUnlock(h1);
        _HUnlock(h2);
        _HUnlock(h3);
        _HUnlock(R32(lh4));
        _HPurge(h1);
        _HPurge(h2);
        _HPurge(h3);
        _HPurge(R32(lh4));
        if (d6 == 0) {
            CreateSound(-1, 0x6989, 1);
            CreateSound(-1, 0x61b5, 0);
            d6 = S16(WaitCurrentSound(1));
        }
    }
    if (d6 == 0) {
        SetSoundId(0x61b5, 0);
        SetSoundId(0x6989, 1);
        NISTextDraw(0x6978, 0x14);
    }
    if (d6 == 0) {
        ClearNIS(gOffWorld, 0xff, frame);
        AddNIS(0x698a, 0x40, 0x3e, 0, full);
        d6 = S16(DissolveNIS(0x3c));
    }
    if (d6 == 0) d6 = S16(Wait(0, 0x2a));
    if (d6 == 0) {
        SetTimer(0, 0, 0x66);
        NISTextDraw(0x6978, 0x15);
        ClearNIS(gOffWorld, 0xff, frame);
        AddNIS(0x698b, 0x40, 0x3e, 0, full);
        d6 = S16(DissolveNIS(0x3c));
    }
    if (d6 == 0) d6 = S16(WaitTimer(0));
    if (d6 == 0) {
        ClearNIS(gOffWorld, 0xff, frame);
        AddNIS(0x698c, 0x40, 0x3e, 0, full);
        d6 = S16(WaitCurrentSound(1));
    }
    if (d6 == 0) {
        SetTimer(0, 0, 0x6c);
        d6 = S16(DissolveNIS(0x3c));
    }
    if (d6 == 0) d6 = S16(WaitTimer(0));
    if (d6 == 0) {
        SetTimer(0, 0, 0x6c);
        ClearNIS(gOffWorld, 0xff, frame);
        AddNIS(0x698d, 0x40, 0x3e, 0, full);
        AddNIS(0x698e, 0xc0, 0x4c, 0x10, full);
        AddNIS(0x698f, 0x76, 0x95, 0x10, full);
        AddNIS(0x6991, 0x6f, 0x90, 0x10, full);
        AddNIS(0x6992, 0x10d, 0x66, 0x10, full);
        d6 = S16(DissolveNIS(0x3c));
    }
    if (d6 == 0) {
        NISTextDraw(0, 0);
        CreateSound(-1, 0x698a, 1);
        d6 = S16(WaitTimer(0));
    }
    if (d6 == 0) d6 = nis_dialog(0x6978, 0x16, 0x698a, 0xb6);
    if (d6 == 0) d6 = S16(Wait(0, 0x14));
    if (d6 == 0) {
        SetTimer(0, 0, 0x1e);
        NISTextDraw(0, 0);
        ClearNIS(gOffWorld, 0xff, frame);
        AddNIS(0x699b, 0x40, 0x3e, 0, full);
        AddNIS(0x699c, 0x40, 0x3e, 0x10, full);
        d6 = S16(WaitTimer(0));
    }
    if (d6 == 0) d6 = S16(WaitCue(0x61));
    if (d6 == 0) {
        GetSetFadeCTAB(0x699b, 0x46, FADE_SPEED);
        CopyNIS(frame);
        SetNISCTAB(0x52, 100);
        SetTimer(0, 0, 0x5a);
        CreateSound(-1, 0x698b, 1);
        d6 = S16(WaitTimer(0));
    }
    if (d6 == 0) d6 = S16(WaitCue(0x62));
    if (d6 == 0) d6 = nis_dialog(0x6978, 0x17, 0x698b, 0x112);
    if (d6 == 0) {
        SetTimer(0, 0, 0x28);
        CreateSound(-1, 0x698c, 1);
        d6 = S16(WaitTimer(0));
    }
    if (d6 == 0) {
        /* the flight: 8 steps from the gOpenFly* tables */
        h1 = GetShape(0x698d);
        h2 = GetShape(0x698f);
        h3 = GetShape(0x6990);
        uint32_t h4 = GetShape(0x6992);
        W32(lh4, h4);
        if (h1 == 0 || h2 == 0 || h3 == 0 || h4 == 0) return 1;
        _HLock(h1);
        _HLock(h2);
        _HLock(h3);
        _HLock(R32(lh4));
        d6 = S16(WaitCue(0x63));
        if (d6 == 0) {
            GetSetFadeCTAB(0x6985, 0x46, FADE_SPEED);
            ClearNIS(gOffWorld, 0xff, frame);
            NISTextDraw(0, 0);
            W16(gOpenFlyStep, 0);
            if (d6 == 0) {
                do {
                    SaveCurrentWorld(gOffWorld);
                    clip_rect_ptr(frame);
                    DrawIt(R32(h1), 0, 0x40, 0x3e, 0, 0, 0);
                    RestoreOriginalWorld(gOffWorld);
                    int32_t k = (int32_t)R16(gOpenFlyStep) * 2;
                    AddNIS(R16(gOpenFlyId + k), R16(gOpenFlyX + k), R16(gOpenFlyY + k), 0x10, full);
                    if (R16(gOpenFlyStep) == 6) AddNIS(0x6999, 0xd3, 0x6e, 0x10, full);
                    if (R16(gOpenFlyStep) == 7) AddNIS(0x699a, 0xd4, 0x72, 0x10, full);
                    SaveCurrentWorld(gOffWorld);
                    DrawIt(R32(h2), 0, 0x62, 0x94, 0x10, 0, 0);
                    DrawIt(R32(h3), 0, 0x59, 0x8f, 0x10, 0, 0);
                    DrawIt(R32(R32(lh4)), 0, 0x112, 0x67, 0x10, 0, 0);
                    RestoreOriginalWorld(gOffWorld);
                    if (R16(gOpenFlyStep) == 0) {
                        d6 = S16(WaitCue(0x63));
                        if (d6 == 0) {
                            CopyNIS(frame);
                            SetNISCTAB(0x52, 100);
                            d6 = S16(WaitCue(100));
                        }
                    } else {
                        k = (int32_t)R16(gOpenFlyStep) * 2;
                        SetTimer(0, 0, R16(gOpenFlyTime + k));
                        CopyNIS(full);
                        d6 = S16(WaitTimer(0));
                    }
                    W16(gOpenFlyStep, R16(gOpenFlyStep) + 1);
                } while (d6 == 0 && R16(gOpenFlyStep) < 8);
            }
            _HUnlock(h1);
            _HUnlock(h2);
            _HUnlock(h3);
            _HUnlock(R32(lh4));
            _HPurge(h1);
            _HPurge(h2);
            _HPurge(h3);
            _HPurge(R32(lh4));
        }
    }
    if (d6 == 0 && (d6 = nis_dialog(0x6978, 0x18, 0x698c, 0x81)) == 0) {
        SetTimer(0, 0, 0x14);
        CreateSound(-1, 0x61b6, 0);
        CreateSound(-1, 0x698d, 1);
        d6 = S16(WaitTimer(0));
    }
    if (d6 == 0) {
        GetSetFadeCTAB(0x699b, 0x46, FADE_SPEED);
        ClearNIS(gOffWorld, 0xff, frame);
        AddNIS(0x69a0, 0x40, 0x3e, 0x10, full);
        CopyNIS(frame);
        SetNISCTAB(0x52, 100);
        SetSoundId(0x61b6, 0);
        d6 = nis_dialog(0x6978, 0x19, 0x698d, 0xae);
    }
    if (d6 == 0) {
        NISTextDraw(0, 0);
        ClearNIS(gOffWorld, 0xff, frame);
        AddNIS(0x699b, 0x40, 0x3e, 0, 0);
        AddNIS(0x699d, 0x93, 0x46, 0x10, 0);
        CopyNIS(frame);
    }
    if (d6 == 0) {
        SetTimer(0, 0, 0xc);
        ClearNIS(gOffWorld, 0xff, frame);
        AddNIS(0x699b, 0x40, 0x3e, 0, 0);
        AddNIS(0x699e, 0x4a, 0x45, 0x10, 0);
        CreateSound(-1, 0x698e, 1);
        d6 = S16(WaitTimer(0));
        if (d6 == 0) {
            SetSoundId(0x698e, 1);
            NISTextDraw(0x6978, 0x1a);
            CopyNIS(frame);
            d6 = S16(WaitCurrentSound(1));
            if (d6 == 0) d6 = S16(Wait(0, 0x1e));
        }
    }
    if (d6 == 0) {
        SetTimer(0, 0, 0x14);
        NISTextDraw(0, 0);
        ClearNIS(gOffWorld, 0xff, frame);
        AddNIS(0x699f, 0x40, 0x3e, 0, 0);
        CopyNIS(frame);
        CreateSound(-1, 0x698f, 1);
        d6 = S16(WaitTimer(0));
    }
    if (d6 == 0 && (d6 = nis_dialog(0x6978, 0x1b, 0x698f, 0x62)) == 0) {
        SetTimer(0, 0, 0x14);
        CreateSound(-1, 0x6990, 1);
        d6 = S16(WaitTimer(0));
    }
    if (d6 == 0) {
        SetSoundId(0x6990, 1);
        ClearNIS(gOffWorld, 0xff, frame);
        NISTextDraw(0x6978, 0x1c);
        AddNIS(0x699b, 0x40, 0x3e, 0, 0);
        AddNIS(0x69a1, 0x40, 0x3e, 0x10, 0);
        CopyNIS(frame);
        d6 = S16(WaitCurrentSound(1));
    }
    if (d6 == 0) {
        SetTimer(0, 0, 0x12);
        NISTextDraw(0, 0);
        ClearNIS(gOffWorld, 0xff, frame);
        AddNIS(0x69a2, 0x40, 0x3e, 0, 0);
        AddNIS(0x69a3, 0xcc, 0x3e, 0x10, 0);
        CopyNIS(frame);
        CreateSound(-1, 0x6991, 1);
        d6 = S16(WaitTimer(0));
    }
    if (d6 == 0) d6 = nis_dialog(0x6978, 0x1d, 0x6991, 0x61);
    if (d6 == 0) {
        SetTimer(0, 0, 0x12);
        CreateSound(-1, 0x61b7, 0);
        CreateSound(-1, 0x6992, 1);
        d6 = S16(WaitTimer(0));
    }
    if (d6 == 0) {
        ClearNIS(gOffWorld, 0xff, frame);
        AddNIS(0x69a2, 0x40, 0x3e, 0, 0);
        AddNIS(0x69a4, 0xc9, 0x3e, 0x10, 0);
        SetSoundId(0x61b7, 0);
        SetSoundId(0x6992, 1);
        NISTextDraw(0x6978, 0x1e);
        d6 = S16(DissolveNIS(0x4b));
        if (d6 == 0) d6 = S16(WaitCurrentSound(1));
    }
    if (d6 == 0) d6 = S16(Wait(0, 0x1e));
    if (d6 == 0) {
        SetTimer(0, 0, 0x6e);
        ClearNIS(gOffWorld, 0xff, frame);
        AddNIS(0x69a2, 0x40, 0x3e, 0, 0);
        AddNIS(0x69a5, 0xc9, 0x3e, 0x10, 0);
        d6 = S16(DissolveNIS(0x4b));
        if (d6 == 0) d6 = S16(WaitTimer(0));
    }
    if (d6 == 0) {
        GetSetFadeCTAB(0x69a6, 0x46, FADE_SPEED);
        SetTimer(0, 0, 0x30);
        NISTextDraw(0, 0);
        ClearNIS(gOffWorld, 0xff, frame);
        AddNIS(0x69a7, 0x40, 0x3e, 0, full);
        CopyNIS(frame);
        SetNISCTAB(0x52, 100);
        W32(gSndFile, R32(gSoundRsrc));
        CreateSound(-1, 0x263c, 1);
        d6 = S16(WaitTimer(0));
    }
    if (d6 == 0) {
        SetTimer(0, 0, 0x78);
        AddNIS(0x69a6, 0x7d, 0x48, 0x10, full);
        SetSoundId(0x263c, 1);
        d6 = S16(DissolveNIS(0x5a));
    }
    if (d6 == 0) d6 = S16(WaitCurrentSound(0));
    if (d6 == 0) FadeBothCTBLs(FADE_SPEED);
    else RapidFadeALL(FADE_SLOW);
    ResetSoundItem(0);
    PurgeSounds();
    SaveCurrentWorld(gOffWorld);
    SetGameRect(endrect);
    clip_rect_ptr(endrect);
    RestoreOriginalWorld(gOffWorld);
    W32(gReadKeyProc, R32(gSavedKeyProc));
    return (uint16_t)d6;
}

/* 00049f74: transition into the temple (horses, the kid, the temple room) */
uint32_t NISTemple(void) {
    ENTRY_LINK(FN_NISTemple, 0x14, 20);             /* link -0x14; movem D3-D7 */
    maddr r8 = a6 - 8, r10 = a6 - 0x10, res = a6 - 0x14;
    OpenRsrcVerify(gRsrcTemple);
    OpenRsrcVerify(gRsrcNIS);
    OpenRsrcVerify(gRsrcTempleNIS);
    GetSetFadeCTAB(0xdac, 0x45, FADE_SLOW);
    GetSetFadeCTAB(0x6310, 0x46, FADE_SLOW);
    SetKidPalette(4);
    DrawSkyForNIS();
    AddNIS(0x109a, 0, 0x45, 0x10, 0);
    AddNIS(0x109b, 0x5a, 0x139, 0x10, 0);
    SetGameRect(r8);
    CopyNIS(r8);
    W16(res, PlayTheAnimation(0x61a8, 0xb, 1, JT_ADDR(FN_TransFunc4), B(0), 0, B(1), 0xc, 0, 0, 0));
    SetNISCTAB(0x53, 0);
    FUN_00046940();
    SetGameRect(r10);
    ClearNIS(gOffWorld, 0xff, r10);
    CopyNIS(r10);
    if (R16(res) == 0) {
        GetSetFadeCTAB(0x6323, 0x46, FADE_SLOW);
        GetSetFadeCTAB(0x62d7, 0x51, FADE_SLOW);
        SaveCurrentWorld(gOffWorld);
        PoPFillRect(gPlayRect, 0x55);
        RestoreOriginalWorld(gOffWorld);
        AddNIS(0x6329, -1, 0x30, 0x10, 0);
        int16_t r = S16(PlayTheAnimation(0x61a8, 0xc, 1, JT_ADDR(FN_TransFunc5), B(0), 0, B(1), 0x14,
                                         0, 0, 0));
        W16(res, r);
        if (r == -1) W16(res, 0);
        SetNISCTAB(0x52, 0);
        FUN_00046940();
        SetGameRect(r10);
        ClearNIS(gOffWorld, 0xff, r10);
        CopyNIS(r10);
    }
    if (R16(res) == 0) {
        GetSetFadeCTAB(0xdac, 0x51, FADE_SLOW);
        CreateSound(-1, 0x61ae, 0);
        DrawSkyForNIS();
        AddNIS(0x10a8, 0, 0xa, 0x10, 0);
        CopyNIS(r8);
        GetSetFadeCTAB(0x62d7, 0x52, FADE_SLOW);
        int16_t r = S16(PlayTheAnimation(0x61a8, 0xd, 1, JT_ADDR(FN_TransFunc6), B(0), 0, B(1), 0xc,
                                         0, 0, 0));
        W16(res, r);
        if (r == 0) FadeBothCTBLs(FADE_SPEED);
        else RapidFadeALL(1);
        FUN_00046940();
        SetGameRect(r10);
        ClearNIS(gOffWorld, 0xff, r10);
        CopyNIS(r10);
    }
    if (R16(res) == 0) {
        GetSetFadeCTAB(0x631b, 0x45, FADE_SPEED);
        GetSetFadeCTAB(0x62d7, 0x46, 0);
        AddNIS(0x631b, 0, 0, 0, 0);
        AddNIS(0x631c, 0, 0xe8, 0x10, 0);
        CopyNIS(r8);
        FadeInBothCTBLs(FADE_SPEED);
        int16_t r = S16(PlayTheAnimation(0x61a8, 0xe, 1, JT_ADDR(FN_TransFunc7), B(0), 0, B(1), 0x1c,
                                         0, 0, 0));
        W16(res, r);
        if (r == 0) FadeBothCTBLs(FADE_SPEED);
        else RapidFadeALL(1);
        FUN_00046940();
        SetGameRect(r10);
        ClearNIS(gOffWorld, 0xff, r10);
        CopyNIS(r10);
    }
    if (R16(res) == 0) {
        GetSetFadeCTAB(0x62d6, 0x45, FADE_SLOW);
        DrawRoomFromNIS(0xa, 0x16);
        OpenLevelRsrcFile(2, 0);
        W16(gAnimSndOn, 1);
        W16(gAnimSndId, 0x61b8);
        W16(res, PlayTheAnimation(0x61a8, 0xf, 1, JT_ADDR(FN_TransFunc8), B(0), 0, B(0), 0x1c, 0, 0, 0));
        FUN_00046940();
    }
    CloseRsrcFile(gRsrcTempleNIS);
    CloseRsrcFile(gRsrcNIS);
    return RU16(res);
}

/* 0004a3aa: NISTemple animation 0xb: horses, palette changes */
uint32_t TransFunc4(int16_t f) {
    ENTRY_LINK(FN_TransFunc4, 0, 4);                /* link 0; move.l D3 */
    if (f == 0) {
        FlipHorses(1);
        PlaySound(-1, 0x61ac, 0);
    } else if (f == 1) {
        SetNISCTAB(0x53, 100);
        GetSetFadeCTAB(0x6320, 0x51, FADE_SLOW);
        GetSetFadeCTAB(0x6312, 1, FADE_SLOW);
    } else if (f == 6) {
        SetNISCTAB(0x51, 100);
    } else if (f == 7) {
        SetNISCTAB(0x52, 100);
    } else if (f == 8) {
        SetNISCTAB(0x51, 100);
    } else if (f == 9) {
        GetSetFadeCTAB(0x62d7, 0x51, FADE_SLOW);
    }
    return 0;
}

/* 0004a48e: NISTemple animation 0xc; stops the animation after frame 0x60 */
uint32_t TransFunc5(int16_t f) {
    ENTRY_LINK(FN_TransFunc5, 0, 4);                /* link 0; move.l D3 */
    int16_t r = 0;
    if (f == 0) {
        FlipHorses(1);
        copy_rect(gNISClipRect, gPlayRect);
        DrawSOffWorldToWindow(gOffWorld, R32(gMainWindow), gPlayRect, gPlayRect, 0);
    } else if (f == 1) {
        GetSetFadeCTAB(0x6323, 0x52, FADE_SLOW);
    }
    if (R16(gAnimFrame) >= 0x61) r = -1;
    return (uint16_t)r;
}

/* 0004a50a: NISTemple animation 0xd */
uint32_t TransFunc6(int16_t f) {
    ENTRY_LINK(FN_TransFunc6, 0, 0);
    if (f == 0) FlipHorses(1);
    else if (f == 2) PlaySound(-1, 0x61ae, 0);
    return 0;
}

/* 0004a54c: NISTemple animation 0xe */
uint32_t TransFunc7(int16_t f) {
    ENTRY_LINK(FN_TransFunc7, 0, 0);
    if (f == 0) FlipHorses(1);
    return 0;
}

/* 0004a574: NISTemple animation 0xf (the kid in the temple room) */
uint32_t TransFunc8(int16_t f) {
    ENTRY_LINK(FN_TransFunc8, 0, 8);                /* link 0; movem D3/A2 */
    if (f == 0) {
        FlipKids(1, 0x28);
        FlipKid(1);
        SetNISCTAB(0x51, 100);
        uint32_t saved = R32(gSavedPalette);
        W32(gSavedPalette, 0);
        SetKidPalette(2);
        W32(gSavedPalette, saved);
        FlipHorses(0);
    } else if (f == 1) {
        copy_rect(gNISClipRect, gPlayRect);
    } else if (f == 0x2b) {
        GetSetFadeCTAB(0x6312, 0x52, 1);
    } else if (f == 0x2c) {
        GetSetFadeCTAB(0x6311, 0x51, 1);
    } else if (f == 0x2d) {
        SetNISCTAB(0x52, 100);
    } else if (f == 0x2e) {
        SetNISCTAB(0x51, 100);
    }
    return 0;
}

/* 0004a65c: flip the animation's kid shape (shape handle 16) */
uint32_t FlipKid(int16_t how) {
    ENTRY_LINK(FN_FlipKid, 4, 4);                   /* link -4; move.l A2 */
    uint32_t h = R32(R32(R32(gAnimShapes)) + 0x40);
    if (h != 0 && R32(h) != 0) FlipSHAP(R32(h), how);
    return 0;
}

/* The original keeps the throne room result in D5 (and constants in D3/D4); callees
 * save these registers in their frames, and the Mohawk audio code later copies such stack
 * bytes into the synth record (SetSoundId -> BlockMove of a partly uninitialised local).
 * NISThroneRoom therefore keeps the emulated registers as the original has them. */
static void set_dw(int reg, uint16_t v) { cpu_set(reg, (cpu_get(reg) & 0xffff0000u) | v); }
static int16_t d5w(int16_t v) { set_dw(R_D5, (uint16_t)v); return v; }

/* 0004a694: transition in the throne room (the queen, the dead kid, Jaffar) */
uint32_t NISThroneRoom(void) {
    ENTRY_LINK(FN_NISThroneRoom, 0x10, 12);         /* link -0x10; movem D3-D5 */
    maddr full = a6 - 0x10, box = a6 - 8;
    int16_t d5;
    const uint32_t save_d3 = cpu_get(R_D3), save_d4 = cpu_get(R_D4), save_d5 = cpu_get(R_D5);
    W32(a6 - 0x1c, save_d3);                        /* movem.l D3-D5,-(SP) */
    W32(a6 - 0x18, save_d4);
    W32(a6 - 0x14, save_d5);
    W16(gInThroneNIS, 1);
    OpenRsrcVerify(gRsrcThrone);
    if (R32(gMapSave1) != 0) {
        DeallocMapSave(R32(gMapSave1));
        W32(gMapSave1, 0);
    }
    if (R32(gMapSave2) != 0) {
        DeallocMapSave(R32(gMapSave2));
        W32(gMapSave2, 0);
    }
    SetGameRect(full);
    set_dw(R_D3, 1);                                /* move.w D0,D3 (D0 = 1) */
    ShowNthvRGB(R32(gMainWindow), 1, 1, B(1), B(0xff), B(0x5a), B(0));
    ShowNthvRGB(R32(gMainWindow), 1, 1, B(2), B(0xff), B(0xb5), B(0));
    FUN_0004adc4();
    SetNISCTAB(0x51, 100);
    GetSetFadeCTAB(0x7d01, 0x52, FADE_SPEED);
    AddNIS(0x7d01, 0, 0, 0, 0);
    SaveCurrentWorld(gOffWorld);
    clip_rect_ptr(full);
    DrawDeadKid(0);
    RestoreOriginalWorld(gOffWorld);
    CreateSound(-1, 0x792c, 0);
    d5 = d5w(S16(WaitSndId(0x26cd, 0)));
    if (d5 == 0) d5 = d5w(S16(Wait(0, 2)));
    if (d5 == 0) {
        SetSoundId(0x792c, 0);
        d5 = d5w(S16(DissolveNIS(0xf0)));
    }
    if (d5 == 0) {
        /* the original pushes MoveRect's last argument before calling FUN_0004ae9c */
        maddr sp = emu_sp();
        emu_set_sp(sp - 2);
        W16(sp - 2, 0xd2);
        int16_t dh = S16(FUN_0004ae9c());
        emu_set_sp(sp);
        MoveRect(gQueenRect, dh, 0xd2);
        FUN_00046d00();
        InitDissolve(2);
        SaveCurrentWorld(gOffWorld);
        W32(gMapSave2, SaveRect(gQueenRect, 0));
        RestoreOriginalWorld(gOffWorld);
        d5 = d5w(S16(WaitCue(0x61)));
    }
    if (d5 == 0) {
        int16_t v = R16(gQueenRect);
        d5 = d5w(S16(QueenAddNIS(0x7d02, (int16_t)(R16(gQueenRect + 2) + 9), v, 0xb4)));
    }
    if (d5 == 0) {
        cpu_set(R_D3, 1);                           /* moveq #1,D3 */
        cpu_set(R_D4, (uint32_t)-1);                /* moveq #-1,D4 */
        CreateSound(-1, 0x7d01, 1);
        CreateSound(-1, 0x7d02, 1);
        CreateSound(-1, 0x7d03, 1);
        CreateSound(-1, 0x7d04, 1);
        d5 = d5w(S16(WaitCue(0x62)));
    }
    if (d5 == 0) {
        int16_t v;
        cpu_set(R_D3, 10);                          /* moveq #$a,D3 */
        SetTimer(0, 0, 10);
        cpu_set(R_D4, 0x14);                        /* moveq #$14,D4 */
        v = R16(gQueenRect);
        d5 = d5w(S16(QueenAddNIS(0x7d03, (int16_t)(R16(gQueenRect + 2) + 6), v, 0x14)));
        d5 = d5w((int16_t)(d5 | S16(WaitTimer(0))));
        SetTimer(0, 0, 10);
        v = R16(gQueenRect);
        d5 = d5w((int16_t)(d5 | S16(QueenAddNIS(0x7d04, (int16_t)(R16(gQueenRect + 2) + 4), v, 0x14))));
        d5 = d5w((int16_t)(d5 | S16(WaitTimer(0))));
        SetTimer(0, 0, 10);
        v = R16(gQueenRect);
        d5 = d5w((int16_t)(d5 | S16(QueenAddNIS(0x7d05, R16(gQueenRect + 2), v, 0x14))));
        d5 = d5w((int16_t)(d5 | S16(WaitTimer(0))));
    }
    if (d5 == 0) {
        SaveCurrentWorld(gOffWorld);
        copy_rect(box, gNISBoxRect);
        MoveRect(box, 0, 0);
        W32(gMapSave1, SaveRect(box, 0));
        RestoreOriginalWorld(gOffWorld);
    }
    if (d5 == 0) d5 = d5w(nis_dialog(0x7d00, 1, 0x7d01, 0x137));
    if (d5 == 0) d5 = d5w(nis_dialog(0x7d00, 2, 0x7d02, 0x116));
    if (d5 == 0) d5 = d5w(nis_dialog(0x7d00, 3, 0x7d03, 0x79));
    if (d5 == 0) d5 = d5w(nis_dialog(0x7d00, 4, 0x7d04, 0x146));
    if (d5 == 0) {
        SetTimer(0, 0, 0x3c);
        NISTextDraw(0, 0);
        CreateSound(-1, 0x7d06, 1);
        d5 = d5w(S16(WaitTimer(0)));
    }
    if (d5 == 0) d5 = d5w(nis_dialog(0x7d00, 6, 0x7d06, 0xde));
    if (d5 == 0) {
        SetTimer(0, 0, 0x3c);
        NISTextDraw(0, 0);
        CreateSound(-1, 0x7d08, 1);
        d5 = d5w(S16(WaitTimer(0)));
    }
    if (d5 == 0) d5 = d5w(nis_dialog(0x7d00, 8, 0x7d08, 0xe0));
    SetTimer(0, 0, 0x3c);
    W32(gMidiFile, 0);
    CreateSound(-1, 0x26ce, 0);
    if (d5 == 0) {
        d5 = d5w(S16(WaitTimer(0)));
        NISTextDraw(0, 0);
    }
    if (d5 == 0) d5 = d5w(S16(WaitSndId(0x792c, 0)));
    SetSoundId(0x26ce, 0);
    if (d5 == 0) {
        SetTimer(0, 0, 0x3c);
        QueenAddNIS(0, 0, 0, 0xb4);
        d5 = d5w(S16(WaitTimer(0)));
    }
    if (R32(gMapSave2) != 0) {
        DeallocMapSave(R32(gMapSave2));
        W32(gMapSave2, 0);
    }
    if (R32(gMapSave1) != 0) {
        DeallocMapSave(R32(gMapSave1));
        W32(gMapSave1, 0);
    }
    if (d5 == 0) {
        SaveCurrentWorld(gOffWorld);
        FUN_0004ace6();
        RestoreOriginalWorld(gOffWorld);
    }
    if (d5 == 0) FadeOutAllColors(R32(gMainWindow), FADE_SPEED);
    if (d5 == 0) {
        CopyNIS(full);
        FUN_0004adc4();
        SetNISCTAB(0x51, 0);
        GetSetFadeCTAB(0xdad, 0x46, FADE_SPEED);
        FadeInBothCTBLs(FADE_SPEED);
    }
    if (d5 == 0) d5 = d5w(S16(Wait(0, 0x3c)));
    if ((uint16_t)SoundIdOn(0x26ce, 0) == 0) PlaySound(-1, 0x26ce, 0);
    W16(gInThroneNIS, 0);
    FUN_00046940();
    PurgeSounds();
    CloseRsrcFile(gRsrcThrone);
    cpu_set(R_D3, save_d3);                         /* movem.l (SP)+,D3-D5 */
    cpu_set(R_D4, save_d4);
    cpu_set(R_D5, save_d5);
    return (uint16_t)d5;
}

/* 0004ac06: draw the dead kid on the throne room floor (and the shape 0x3f3); with
 * pal != 0 also set palette 0x3ec */
uint32_t DrawDeadKid(int16_t pal) {
    ENTRY_LINK(FN_DrawDeadKid, 0, 20);              /* link 0; movem D3-D5/A2/A4 */
    uint32_t h = GetShape(0x61f9);
    if (h != 0) {
        _HLock(h);
        int16_t x = (int16_t)(R16(gKidX) - 0xd6);
        if (R16(gKidFace) == 0) {
            FlipSHAP(R32(h), 1);
            x = (int16_t)(x - 0x15);
        }
        uint32_t shp = R32(h);
        int16_t height = R16(shp + 6);
        DrawIt(shp, 0, (int16_t)(x - 7), (int16_t)(R16(gKidY) - height + 6), 0x10, 0x10, 0);
        _HUnlock(h);
        _HPurge(h);
    }
    h = GetShape(0x3f3);
    if (h != 0) {
        _HLock(h);
        uint32_t shp = R32(h);
        int16_t height = R16(shp + 6);
        DrawIt(shp, 0, 0x10f, (int16_t)(0x158 - height + 8), 0x10, 0, 0);
        _HUnlock(h);
        _HPurge(h);
    }
    if (pal != 0) PoPSetPalette(0x3ec, 0xf0);
    return 0;
}

/* 0004ace6: draw the throne room backdrop pieces (level type 4) and the dead kid */
uint32_t FUN_0004ace6(void) {
    ENTRY_NOLINK(FN_FUN_0004ace6, 8);               /* movem D3-D4 */
    if (R16(GS() + GS_LEVELTYPE) != 4) return 0;
    DrawSHAPfromID(0x1068, 0, 0, 0, 0);
    DrawSHAPfromID(0x1069, 0, 0x8f, 0x10, 0);
    DrawSHAPfromID(0x106e, 0x1b7, 0xaa, 0x10, 0);
    DrawSHAPfromID(0x106a, 0x1cf, 0x62, 0x10, 0);
    DrawSHAPfromID(0x106c, 0x38, 0x159, 0x10, 0);
    DrawSHAPfromID(0x106d, 0x73, 0x158, 0x10, 0);
    DrawSHAPfromID(0x106f, 0x46, 0x15b, 0x10, 0);
    DrawSHAPfromID(0x106b, 0x38, 0x161, 0x10, 0);
    DrawDeadKid(1);
    return 0;
}

/* 0004adc4: load the level's two colour tables (25000 + type, 1000 + type), remap them
 * and combine them into gCTab1 */
uint32_t FUN_0004adc4(void) {
    ENTRY_NOLINK(FN_FUN_0004adc4, 4);               /* move.l A2 */
    FUN_00046940();
    GetSetFadeCTAB((int16_t)(R16(GS() + GS_LEVELTYPE) + 25000), 1, FADE_SPEED);
    GetSetFadeCTAB((int16_t)(R16(GS() + GS_LEVELTYPE) + 1000), 2, FADE_SPEED);
    RemapCTBL(R32(gCTab1), 0x10);
    RemapCTBL(R32(gCTab2), 0xf0);
    uint32_t c = CombineCtabs(R32(gCTab1), R32(gCTab2));
    FUN_00046940();
    W32(gCTab1, c);
    return 0;
}

/* 0004ae3e: restore the queen area, add NIS shape `id` at (h, v) (none if id == 0) and
 * dissolve it in; returns DissolveNIS's result.  Callers push the dissolve time as a
 * long; only its low word is used. */
uint32_t QueenAddNIS(int16_t id, int16_t h, int16_t v, uint32_t time) {
    ENTRY_LINK(FN_QueenAddNIS, 4, 0);               /* link -4 */
    SaveCurrentWorld(gOffWorld);
    RestoreRectKeep(R32(gMapSave2));
    RestoreOriginalWorld(gOffWorld);
    if (id != 0) AddNIS(id, h, v, 0x10, 0);
    return DissolveNIS((int16_t)time);
}

/* 0004ae9c: x of the dead kid shape relative to the kid (depends on facing) */
uint32_t FUN_0004ae9c(void) {
    int16_t x = (int16_t)(R16(gKidX) - 0xd6);
    uint16_t face = RU16(gKidFace);
    if (face == 0) {
        x = (int16_t)(x - 0x15);
        x = (int16_t)(x - 0x12);
    }
    /* D0: the high word is the caller's (move.w only) */
    return (cpu_get(R_D0) & 0xffff0000u) | (uint16_t)x;
}

/* 0004aeba: the title sequence (animation 0x61a8 / 4 with TitleFunc) */
uint32_t NISTitle(void) {
    ENTRY_LINK(FN_NISTitle, 8, 4);                  /* link -8; move.l D3 */
    maddr rect = a6 - 8;
    OpenRsrcVerify(gRsrcTitle);
    SetGameRect(rect);
    ClearNIS(gOffWorld, 0xff, rect);
    CopyNIS(rect);
    GetSetFadeCTAB(0x61a9, 0x46, FADE_SPEED);
    FUN_0004b11a();
    int16_t r = S16(PlayTheAnimation(0x61a8, 4, 1, JT_ADDR(FN_TitleFunc), B(0), 0, B(1), 0x10c, 0, 0, 0));
    if (r == -1) r = 0;
    FadeOutColors(R32(gCTab2), R32(gMainWindow), FADE_SLOW);
    FUN_00046940();
    if (R32(gMapSave1) != 0) {
        DeallocMapSave(R32(gMapSave1));
        W32(gMapSave1, 0);
    }
    CloseRsrcFile(gRsrcTitle);
    return (uint16_t)r;
}

/* 0004af8c: per-frame function of the title animation (the two title words) */
uint32_t TitleFunc(int16_t f) {
    ENTRY_LINK(FN_TitleFunc, 0xc, 4);               /* link -0xc; move.l D3 */
    int16_t r = 0;
    if (f == 1) {
        copy_rect(gNISClipRect, gGameRect);
    } else if (f == 2) {
        SaveCurrentWorld(gOffWorld);
        union_rect_ptr(gTitleRect2, gTitleRect1, a6 - 0xc);
        W32(gMapSave1, SaveRect(a6 - 0xc, 0));
        RestoreOriginalWorld(gOffWorld);
        FadeInColors(R32(gCTab2), R32(gMainWindow), FADE_SPEED);
    } else if (f >= 3) {
        if ((uint16_t)SoundItemOn(0) == 0) {
            r = -1;
        } else {
            switch (R16(gAnimFrame)) {
            case 0x65:
                copy_rect(gNISClipRect, gTitleRect1);
                W16(gAnimFrame, 0x72);
                /* fall through */
            case 0x72: {
                int16_t v = R16(gTitleRect1);
                AddNIS(0x631d, R16(gTitleRect1 + 2), v, 0x10, 0);
                break;
            }
            case 0x66:
            case 0x68:
                if (R16(gAnimFrame) == 0x66) copy_rect(gNISClipRect, gTitleRect1);
                else copy_rect(gNISClipRect, gTitleRect2);
                SaveCurrentWorld(gOffWorld);
                RestoreRectKeep(R32(gMapSave1));
                RestoreOriginalWorld(gOffWorld);
                W16(gAnimFrame, 0x78);
                break;
            case 0x67:
                copy_rect(gNISClipRect, gTitleRect2);
                W16(gAnimFrame, 0x73);
                /* fall through */
            case 0x73: {
                int16_t v = R16(gTitleRect2);
                AddNIS(0x631e, R16(gTitleRect2 + 2), v, 0x10, 0);
                break;
            }
            case 0x69:
                r = -1;
                break;
            }
        }
    }
    return (uint16_t)r;
}

/* 0004b11a: draw the title backdrop (sky colour 0xa0, clouds and stars) */
uint32_t FUN_0004b11a(void) {
    ENTRY_NOLINK(FN_FUN_0004b11a, 4);               /* move.l D3 */
    SaveCurrentWorld(gOffWorld);
    FUN_0004fbe2();
    SetForeColor(0xa0);
    _PaintRect(gGameRect);
    seg17_04fc46();
    RestoreOriginalWorld(gOffWorld);
    AddNIS(0x6315, 0xac, -0x13, 0x10, 0);
    AddNIS(0x6310, -0x15, 0x80, 0x10, 0);
    AddNIS(0x6311, -0x74, 0x3a, 0x10, 0);
    AddNIS(0x6310, 0x11b, 0xc5, 0x10, 0);
    AddNIS(0x6311, 0x60, 0xa3, 0x10, 0);
    return 0;
}

/* 0004b1c6: transition at the tower (three animations) */
uint32_t NISTower(void) {
    ENTRY_LINK(FN_NISTower, 8, 16);                 /* link -8; movem D3-D5/A2 */
    maddr rect = a6 - 8;
    int16_t d5;
    SetGameRect(rect);
    OpenRsrcVerify(gRsrcNIS);
    OpenRsrcVerify(gRsrcTower);
    ClearNIS(gOffWorld, 0xff, rect);
    CopyNIS(rect);
    GetSetFadeCTAB(0x631d, 0x45, FADE_SLOW);
    GetSetFadeCTAB(0x62d6, 0x52, FADE_SLOW);
    AddNIS(0x631d, 0, 0, 0, 0);
    AddNIS(0x631e, 0x1a, 0xa8, 0x10, 0);
    CopyNIS(rect);
    FadeInColors(R32(gCTab1), R32(gMainWindow), FADE_SPEED);
    d5 = S16(PlayTheAnimation(0x61a8, 0x15, 1, JT_ADDR(FN_TransFunc9), B(0), 0, B(1), 0x1c, 0, 0, 0));
    if (d5 == 0) FadeBothCTBLs(FADE_SPEED);
    else RapidFadeALL(1);
    FUN_00046940();
    if (d5 == 0) {
        ClearNIS(gOffWorld, 0x5d, rect);
        W16(rect, R16(rect + 4) - 0x14);
        ClearNIS(gOffWorld, 0xff, rect);
        GetSetFadeCTAB(0x631e, 0x45, FADE_SPEED);
        GetSetFadeCTAB(0x62d8, 0x52, FADE_SPEED);
        SaveCurrentWorld(gOffWorld);
        PoPFillRect(gPlayRect, 0x5d);
        RestoreOriginalWorld(gOffWorld);
        AddNIS(0x631a, 0, 0xf8, 0x10, 0);
        W16(rect, 0);
        CopyNIS(rect);
        d5 = S16(PlayTheAnimation(0x61a8, 0x16, 1, JT_ADDR(FN_TransFunc10), B(0), 0, B(1), 0x14, 0x5d,
                                  0, 0));
        if (d5 == 0) FadeBothCTBLs(FADE_SPEED);
        else RapidFadeALL(1);
        FUN_00046940();
    }
    if (d5 == 0) {
        ClearNIS(gOffWorld, 0xff, rect);
        CopyNIS(rect);
        GetSetFadeCTAB(0x6320, 0x51, FADE_SPEED);
        GetSetFadeCTAB(0x62d8, 0x52, FADE_SPEED);
        OpenRsrcVerify(gRsrcLevel);
        SaveCurrentWorld(gOffWorld);
        clip_rect_ptr(gPlayRect);
        PoPFillRect(gPlayRect, 0x40);
        RestoreOriginalWorld(gOffWorld);
        AddNIS(0xfa0, 0, 0xf8, 0x10, 0);
        AddNIS(0xfa1, 0x4c, 0xc1, 0x10, 0);
        uint32_t saved = R32(gSavedPalette);
        W32(gSavedPalette, 0);
        SetKidPalette(6);
        PoPSetPalette(0xdac, 0x40);
        W32(gSavedPalette, saved);
        CopyNIS(rect);
    }
    if (d5 == 0)
        d5 = S16(PlayTheAnimation(0x61a8, 0x17, 1, JT_ADDR(FN_TransFunc11), B(0), 0, B(0), 0x1c, 0, 0, 0));
    FUN_00046940();
    CloseRsrcFile(gRsrcNIS);
    CloseRsrcFile(gRsrcTower);
    return (uint16_t)d5;
}

/* 0004b4fa: NISTower animation 0x15: on frame 1 play the sound and wait for cue 0x61 */
uint32_t TransFunc9(int16_t f) {
    ENTRY_LINK(FN_TransFunc9, 0, 4);                /* link 0; move.l D4 */
    uint16_t r = 0;
    if (f == 1) {
        PlaySound(-1, 0x61b1, 0);
        r = (uint16_t)WaitCue(0x61);
    }
    return r;
}

/* 0004b53e: NISTower animation 0x16: fade in on frame 2 */
uint32_t TransFunc10(int16_t f) {
    ENTRY_LINK(FN_TransFunc10, 0, 4);               /* link 0; move.l D3 */
    if (f != 1 && f == 2) FadeInColors(R32(gCTab1), R32(gMainWindow), FADE_SPEED);
    return 0;
}

/* 0004b582: NISTower animation 0x17: colour table changes on frames 0x28-0x2a */
uint32_t TransFunc11(int16_t f) {
    ENTRY_LINK(FN_TransFunc11, 0, 0);
    if (f == 0x28) GetSetFadeCTAB(0x6313, 0x52, FADE_SLOW);
    else if (f == 0x29) GetSetFadeCTAB(0x6312, 0x52, FADE_SLOW);
    else if (f == 0x2a) GetSetFadeCTAB(0x6313, 0x52, FADE_SLOW);
    return 0;
}
