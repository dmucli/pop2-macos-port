/* CODE 15 "NISs", first half (0x43fa4-0x480aa): the NIS (cut-scene) player, the
 * "dissolve" screen transition and the colour-table helpers used by the cut scenes.
 * Ported from the Ghidra decompilation / disassembly of the original 68k code; addresses
 * in comments are Ghidra addresses (see docs/PORTING.md and docs/notes/seg15a.md). */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* ---- globals (Ghidra addresses) ---- */
#define gOffWorld       GH(0x102dc2)   /* the off-screen world record (Save/RestoreCurrentWorld) */
#define gGameWindow     GH(0x102dbe)   /* long: the game window */
#define gColorOK        GH(0x102de2)   /* byte: the cut scenes can be shown (colour screen) */
#define gPlayRect       GH(0x103014)   /* Rect: play area */
#define gQuit           GH(0x103030)   /* word: quit requested */
#define gEvent          GH(0x102cae)   /* EventRecord: what */
#define gKeyPending     GH(0x103054)   /* word */
#define gNISAbort       GH(0x105830)   /* word: non-zero aborts the cut scene */
#define gGameRect       GH(0x106204)   /* Rect */
#define gReadKeyProc    GH(0x10620c)   /* long: keyboard reader procedure (jump-table address) */
#define gSavedKeyProc   GH(0x105ba8)   /* long: reader saved while a NIS runs */
#define gNISKey         GH(0x105b9a)   /* word: cleared by the NIS key reader */
#define gNISSub         GH(0x105ba4)   /* word: sub-scene (TreeCheck: 1..9) */
#define gJafSndStarted  GH(0x105ba6)   /* word: TransFuncJaffarMagic started sound 0x7724 */
#define gJafMagicLevel  GH(0x105b8a)   /* word[8]: SetNISCTAB levels for frames 0x2f..0x35 (index 1..7) */
#define gBadgeDy        GH(0x105b9c)   /* word: NISTextDraw: y offset of the speaker badge */
#define gBadgeDx        GH(0x105b9e)   /* word: x offset of the badge */
#define gBadgeShape     GH(0x105ba0)   /* word: SHAP id of the badge */
#define gBadgePending   GH(0x105ba2)   /* word: draw the badge with the next text */
#define gTextMode1      GH(0x105822)   /* word: NISTextDraw background: copy from the off-screen world */
#define gTextMode2      GH(0x105824)   /* word: NISTextDraw background: saved rect gTextSave */
#define gTextSave       GH(0x105826)   /* long: RestoreRectKeep handle */
#define gTextRect       GH(0x105832)   /* Rect: the text box */
#define gNISRect        GH(0x10583a)   /* Rect: the picture area (moved by 0x40,0x3e) */
#define gCTab1          GH(0x105bac)   /* long: CTBL handle, slot 1 (GetSetFadeCTAB flag 1) */
#define gCTab2          GH(0x105bb0)   /* long: CTBL handle, slot 2 (flag 2) */
#define gCTab3          GH(0x105bb4)   /* long: CTBL handle, slot 3 (flag 0x1000, blend target) */
#define gDisData        GH(0x105bb8)   /* long: dissolve data handle (MakeDisolveData) */
#define gDisResName     GH(0x105bbc)   /* Str255: name given to the saved 'DSLV' resource */
#define gScreenGD       GH(0x106782)   /* long: screen GDevice record (+2: its PixMap handle) */
#define gShapeTable     GH(0x105d74)   /* long: handle to the table of SHAP handles */
#define gCurCue         GH(0x106092)   /* word: last music cue reached */
#define gSndItemA       GH(0x106084)   /* word: sound item, channel 1 */
#define gSndItemB       GH(0x106086)   /* word: sound item, channel 0 */
#define gSndFile        GH(0x10608e)   /* long: Mohawk sound file */
#define gMidiFile       GH(0x10608a)   /* long: Mohawk music file */
#define gSndFileAlt     GH(0x103812)   /* long */
#define gKidPalPtr      GH(0x103564)   /* long: cleared around SetKidPalette in NISCarpet */
#define gDisRect2       GH(0x105d04)   /* Rect: dissolve area for InitDissolve(2) */
#define gCarpetRect     GH(0x105f58)   /* Rect: copy of the play rect (TransFunc1) */
#define pClipRect       GH(0x10609c)   /* long: ClipRect (Pascal, called through the pointer) */
/* resource file records (OpenRsrcVerify / CloseRsrcFile; the first word is the refNum) */
#define rfNIS           GH(0x10591a)
#define rfTree          GH(0x10592c)
#define rfCarpet1       GH(0x105932)
#define rfCarpet2       GH(0x105938)
#define rfCarpet3       GH(0x10593e)
#define rfPrincess      GH(0x105950)
#define rfLevelA        GH(0x1038ee)
#define rfLevelB        GH(0x1038f4)
/* Mohawk resource files (OpenMohawkRsrcVerify) */
#define mfNISSnd        GH(0x1059b6)
#define mfNISMidi       GH(0x105b66)

#define CTBL 0x4354424c
#define DSLV 0x44534c56
#define TEXT 0x54455854

/* push a value the callee does not declare (the original pushes it anyway) */
#define WITH_PUSH32(v, call) do { maddr sp_ = emu_sp(); push32(v); call; emu_set_sp(sp_); } while (0)
#define WITH_PUSH16(v, call) do { maddr sp_ = emu_sp(); push16(v); call; emu_set_sp(sp_); } while (0)

/* ClipRect through the trap address cached at A5-0x1f64 (Pascal: the callee pops) */
static void clip_rect(maddr r) {
    maddr sp0 = emu_sp();
    push32(r);
    call68_addr(R32(pClipRect));
    emu_set_sp(sp0);
}

/* NISDialog(id, item, snd) with the fourth word most callers push (NISDialog ignores it) */
static int16_t nis_dialog(int16_t id, int16_t item, int16_t snd, int16_t extra) {
    uint32_t r = 0;
    WITH_PUSH16((uint16_t)extra, r = NISDialog(id, item, snd));
    return (int16_t)r;
}

/* _SwapMMUMode takes and returns the mode in D0.b (the generated wrapper does not set D0) */
static uint8_t swap_mmu_mode(uint8_t mode) {
    tb_begin();
    cpu_set(R_D0, (cpu_get(R_D0) & ~0xffu) | mode);
    tb_trap(0xA05D);
    uint8_t r = (uint8_t)cpu_get(R_D0);
    tb_end();
    return r;
}

/* FadeOutAllColors(window, speed): the index lists one argument but the original pushes
 * and reads two; called through its jump-table entry with both */
static void FadeOutAllColors_(uint32_t win, uint32_t speed) {
    call_ptr(JT_ADDR(FN_FadeOutAllColors), "ll", win, speed);
}

/* 00043fa4: play NIS n (1..20): open the NIS resource files, run the scene, restore.
 * Returns non-zero if the scene was interrupted. */
uint32_t NIS(int16_t n) {
    ENTRY_LINK(FN_NIS, 8, 16);                 /* link -8; movem D3/D4/A2/A3 */
    W16(GH(0x10601a), 0);
    W16(GH(0x106018), 0);
    int16_t r = 0;                              /* D4 */
    OpenRsrcVerify(rfNIS);
    uint32_t oldSnd = R32(gSndFile);            /* A2 */
    W32(gSndFile, OpenMohawkRsrcVerify(mfNISSnd));
    uint32_t oldMidi = R32(gMidiFile);          /* A3 */
    W32(gMidiFile, OpenMohawkRsrcVerify(mfNISMidi));
    if (n == 6) InitDissolve(1);
    else if (n == 7 || n == 8 || n == 10) InitDissolve(0);
    FUN_00044c22();
    W16(gNISSub, 0);
    switch (n) {                                /* jump table at 0x44044 */
    case 1: case 2: case 3: case 4: case 5: case 6:
        r = S16(Transition(n)); break;
    case 7:  r = S16(Opening1()); break;
    case 8:  r = S16(Opening2()); break;
    case 9:  r = S16(AboardShip()); break;
    case 10: r = S16(Discovery()); break;
    case 11: r = S16(HappyEnding()); break;
    case 12: case 13: case 14: case 15: case 16: case 17: case 18: case 19: case 20:
        TreeCheck((int16_t)(n - 0xb)); break;
    default: break;
    }
    if (r != 0) ResetSoundItem(0);
    FUN_00046940();
    W32(gReadKeyProc, R32(gSavedKeyProc));
    CloseRsrcFile(rfNIS);
    FUN_00046d00();
    PurgeSounds();
    W32(gSndFile, oldSnd);
    W32(gMidiFile, oldMidi);
    if ((uint16_t)SoundItemOn(0) == 0) {
        CloseMohawkRsrc(mfNISMidi);
        CloseMohawkRsrc(mfNISSnd);
    }
    return (uint16_t)r;
}

/* 00044110: draw item `item` of 'TEXT' resource `id` (item 0: an empty text) in the text
 * box under the picture: background, optional speaker badge, then the string three times
 * (shadow, outline, face) with font 0xd5 size 20, and copy the box to the window. */
uint32_t NISTextDraw(int16_t id, int16_t item) {
    ENTRY_LINK(FN_NISTextDraw, 0x140, 32);     /* link -0x140; movem D3-D7/A2-A4 */
    const maddr world = a6 - 0x24, str = a6 - 0x128, len = a6 - 0x28;
    const maddr box = a6 - 0x130, dst = a6 - 0x138, rect = a6 - 0x140;
    W16(len, 0);
    W8(a6 - 4, 0);
    if (item == 0) {
        W8(str, 0);
    } else {
        uint32_t h = PoPGetResource(TEXT, id);  /* A3 */
        _HLock(h);
        uint32_t p = StrItem(h, (int16_t)(((uint16_t)item & 0xff) << 8));   /* move.b (0xb,A6) */
        if (R8(p) != 0) {
            do {
                uint8_t c = R8(p); p++;
                W8(str + R16(len), c);
                W16(len, R16(len) + 1);
                W8(str + R16(len), 0);
            } while (R8(p) != 0);
        }
        _HUnlock(h);
        _HPurge(h);
    }
    mcopy(box, gTextRect, 8);
    if (S16(BuildSOffWorld(world, box)) != 0) return 0;
    SaveCurrentWorld(world);
    int16_t c1, c2;                             /* D5, D6 */
    if (R16(gTextMode1) != 0) {
        c1 = 0xfd; c2 = 0xfe;
        mcopy(dst, box, 8);
        MoveRect(dst, 0, 0x129);
        RestoreOriginalWorld(world);
        DrawOffToOff(gOffWorld, world, dst, box, 0);
        SaveCurrentWorld(world);
    } else if (R16(gTextMode2) != 0) {
        W16(R32(R32(gTextSave)) + 10, 0);
        c1 = 1; c2 = 2;
        RestoreRectKeep(R32(gTextSave));
    } else {
        c1 = 3; c2 = 0xe;
        DrawSHAPfromID(0x61aa, 0, 0, 0, 0);
    }
    mcopy(rect, AlterRect(rect, box, 8, 0, 8, 0), 8);
    if (R16(gBadgePending) != 0) {
        int16_t y = (int16_t)(R16(rect) + R16(gBadgeDy) + 0x1e);
        int16_t x = (int16_t)(R16(rect + 2) + R16(gBadgeDx));
        DrawSHAPfromID(R16(gBadgeShape), x, y, 0x10, 0);
        W16(gBadgePending, 0);
    }
    _TextFont(0xd5);
    _TextSize(0x14);
    _TextFace(0);
    SetForeColor(0xff);
    StringRect(rect, 0, 1, 0, 1, str);
    SetForeColor(c1);
    mcopy(rect, AlterRect(rect, box, 6, 0, 6, 0), 8);
    StringRect(rect, 0, 1, 0, 1, str);
    SetForeColor(c2);
    mcopy(rect, AlterRect(rect, box, 5, 0, 5, 0), 8);
    StringRect(rect, 0, 1, 0, 1, str);
    RestoreOriginalWorld(world);
    mcopy(dst, box, 8);
    if (R16(gTextMode2) != 0) MoveRect(dst, 0, 0);
    else MoveRect(dst, 0, 0x129);
    DrawSOffWorldToWindow(world, R32(gGameWindow), box, dst, 0);
    DisposeSOffWorld(world);
    return 0;
}

/* 000443ca: flip the horse shapes (SHAP 0x12d..0x168) horizontally */
uint32_t FlipHorses(int16_t how) {
    ENTRY_LINK(FN_FlipHorses, 0, 8);           /* movem A2/D3 */
    uint16_t i = 0x12d;
    do {
        uint32_t h = R32(R32(R32(gShapeTable)) + (int32_t)(int16_t)i * 4);
        if (h != 0 && R32(h) != 0) FlipSHAP(R32(h), how);
        i++;
    } while (i <= 0x168);
    return 0;
}

/* 0004441a: flip the kid shapes first..0x106 */
uint32_t FlipKids(int16_t how, int16_t first) {
    ENTRY_LINK(FN_FlipKids, 0, 8);             /* movem A2/D4 */
    int16_t i = first;
    if (first > 0x106) return 0;
    do {
        uint32_t h = R32(R32(R32(gShapeTable)) + (int32_t)i * 4);
        if (h != 0 && R32(h) != 0) FlipSHAP(R32(h), how);
        i++;
    } while (i <= 0x106);
    return 0;
}

/* 00044470: PlayTheAnimation frame hook of the tree NISs */
uint32_t TreeFunc(int16_t frame) {
    ENTRY_LINK(FN_TreeFunc, 4, 8);             /* link -4; movem D3/D4 */
    int16_t r = 0;
    if (frame == 2) {
        FadeInBothCTBLs(0x40001);
        if (R16(gNISSub) == 1) {
            CreateSound(-1, 0x6592, 1);
            r = nis_dialog(0x6590, 1, 0x6591, 0x45);
            if (r == 0) {
                SetSoundId(0x6592, 1);
                r = S16(WaitSndId(0x6592, 1));
            }
        } else if (R16(gNISSub) == 9) {
            r = S16(WaitCue(0x61));
        }
    }
    return (uint16_t)r;
}

/* 00044508: the tree NISs (sub-scene 1..9: NIS 12..20) */
uint32_t TreeCheck(int16_t sub) {
    ENTRY_LINK(FN_TreeCheck, 0x14, 8);         /* link -0x14; movem D3/D4 */
    const maddr game = a6 - 0x10, pic = a6 - 8;
    W16(gNISSub, sub);
    FUN_00044c22();
    OpenRsrcVerify(rfTree);
    SetGameRect(game);
    ClearNIS(gOffWorld, 1, game);
    mcopy(pic, gNISRect, 8);
    MoveRect(pic, 0x40, 0x3e);
    if (R8(gColorOK) == 0) return 0;
    GetSetFadeCTAB(0x61a9, 0x45, 0x40001);
    AddNIS(0x61a9, 0, 0, 0, game);
    CopyNIS(game);
    if (R16(gNISSub) == 1) NISTextDraw(0, 0);
    GetSetFadeCTAB((int16_t)(((int32_t)R16(gNISSub) + 2) / 3 + 0x6590), 0x46, 0x40001);
    if (R16(gNISSub) == 9) SetSHAPColors(R32(gGameWindow), 0x65ac);
    if (R16(gNISSub) == 1) CreateSound(-1, 0x6594, 1);
    PlaySound(-1, (int16_t)(R16(gNISSub) + 0x659a), 0);
    int16_t r = S16(PlayTheAnimation(0x6590, R16(gNISSub), 1, JT_ADDR(FN_TreeFunc),
                                     0, 0, 0x100, 6, 0, 1, 1));
    if (r == 0) {
        if (R16(gNISSub) == 1) {
            if (r == 0) r = nis_dialog(0x6590, 2, 0x6594, 0x6e);
            if (r == 0) {
                SetTimer(0, 0, 0x3c);
                CreateSound(-1, 0x6595, 1);
                r = S16(WaitTimer(0));
            }
            if (r == 0) {
                SetSoundId(0x6595, 1);
                NISTextDraw(0x6590, 3);
                r = S16(WaitCurrentSound(1));
            }
        } else if (R16(gNISSub) == 9) {
            r = S16(WaitCue(0x62));
            if (r == 0) {
                r = S16(WaitCue(0x63));
                if (r == 0) GetSetFadeCTAB(0x65ac, 0x72, 0x40001);
            }
        }
    }
    if (r == 0) {
        r = S16(WaitCurrentSound(0));
        if (r == 0) Wait(0, 0x78);
    }
    if (r == 0) FadeBothCTBLs(0x10001);
    else RapidFadeALL(1);
    ResetSoundItem(0);
    PurgeSounds();
    CloseRsrcFile(rfTree);
    W32(gReadKeyProc, R32(gSavedKeyProc));
    return 0;
}

/* 00044768: the transition NISs 1..6 */
uint32_t Transition(int16_t n) {
    ENTRY_LINK(FN_Transition, 8, 4);           /* link -8; move.l D3 */
    int16_t r = 0;
    if (n != 6) CloseRsrcFile(rfNIS);
    FUN_00044c22();
    switch (n) {                                /* jump table at 0x447a0 */
    case 1: r = S16(NISCarpet()); break;
    case 2: r = S16(NISTemple()); break;
    case 3: r = S16(NISTower()); break;
    case 4: r = S16(NISTitle()); break;
    case 5: r = S16(TransPrincessRoom()); break;
    case 6: r = S16(NISThroneRoom()); break;
    default: break;
    }
    if (r == 0) FadeBothCTBLs(0x40001);
    else if (n != 4 || r != 0) ResetSoundItem(0);
    FUN_00046940();
    W32(gReadKeyProc, R32(gSavedKeyProc));
    return (uint16_t)r;
}

/* 00044820: NIS 5: Jaffar's magic in the princess's room */
uint32_t TransPrincessRoom(void) {
    ENTRY_LINK(FN_TransPrincessRoom, 8, 8);    /* link -8; movem D3/D4 */
    const maddr game = a6 - 8;
    OpenRsrcVerify(rfPrincess);
    GetSetFadeCTAB(0x7919, 0x46, 0x40001);
    SetGameRect(game);
    clip_rect(game);
    SaveCurrentWorld(gOffWorld);
    clip_rect(game);
    RestoreOriginalWorld(gOffWorld);
    AddNIS(0x7a47, 0, 0, 0, 0);
    CopyNIS(game);
    CreateSound(-1, 0x7937, 0);
    CreateSound(-1, 0x7724, 1);
    W16(gJafSndStarted, 0);
    PlaySound(-1, 0x7936, 0);
    int16_t r = S16(PlayTheAnimation(0x7918, 2, 1, JT_ADDR(FN_TransFuncJaffarMagic),
                                     0, 0, 0x100, 0x1c, 0, 0, 0));
    ClearNIS(gOffWorld, 0xff, game);
    CopyNIS(game);
    FUN_00046940();
    CloseRsrcFile(rfPrincess);
    if (r == 0) r = S16(WaitSndId(0x7937, 0));
    if (r != 0) ResetSoundItem(0);
    return (uint16_t)r;
}

/* 00044960: frame hook: set the NIS colours on frame 1 */
uint32_t TransFuncSetCTABframe1(int16_t frame) {
    ENTRY_LINK(FN_TransFuncSetCTABframe1, 0, 0);
    if (frame == 1) SetNISCTAB(0x51, 100);
    return 0;
}

/* 0004499a: frame hook of TransPrincessRoom */
uint32_t TransFuncJaffarMagic(int16_t frame) {
    ENTRY_LINK(FN_TransFuncJaffarMagic, 0, 8); /* movem D3/D4 */
    int16_t lvl = 0;
    switch (frame) {
    case 0:    FlipKids(1, 0xfa); break;
    case 2:    FadeInColors(R32(gCTab2), R32(gGameWindow), 0x40001); break;
    case 0x2f: lvl = 1; break;
    case 0x30: lvl = 2; break;
    case 0x31: lvl = 3; break;
    case 0x32: lvl = 4; break;
    case 0x33: lvl = 5; break;
    case 0x34: lvl = 6; break;
    case 0x35: lvl = 7; break;
    default: break;
    }
    if (lvl != 0) SetNISCTAB(0x52, R16(gJafMagicLevel + (int32_t)lvl * 2));
    if (frame >= 0x36 && frame <= 0x45) {
        if (frame == 0x36) SetSoundId(0x7937, 0);
        SetNISCTAB(0x52, (int16_t)((int16_t)(0xf - (int16_t)(frame - 0x36)) << 2));
    }
    if (frame > 0x32) {
        if ((uint16_t)SoundIdOn(0x2747, 1) == 0 && R16(gJafSndStarted) == 0) {
            SetSoundId(0x7724, 1);
            W16(gJafSndStarted, 1);
        }
    }
    return 0;
}

/* 00044ad2: paint rect in colour `color` in world (clipped to the world's rect) */
uint32_t ClearNIS(uint32_t world, int16_t color, uint32_t rect) {
    ENTRY_LINK(FN_ClearNIS, 0, 0);
    SaveCurrentWorld(world);
    clip_rect(world + 0x18);
    _PenNormal();
    SetForeColor(color);
    _PaintRect(rect);
    RestoreOriginalWorld(world);
    clip_rect(gGameRect);
    return 0;
}

/* 00044b26: draw the SHAP held by handle h into world */
uint32_t AddShpNIS(uint32_t world, uint32_t h, int16_t x, int16_t y, int16_t mode) {
    ENTRY_LINK(FN_AddShpNIS, 0, 0);
    SaveCurrentWorld(world);
    DrawIt(R32(h), 0, x, y, mode, 0, 0);
    RestoreOriginalWorld(world);
    return 0;
}

/* 00044b6e: dissolve the off-screen world to the window in `time` ticks */
uint32_t DissolveNIS(int16_t time) {
    ENTRY_LINK(FN_DissolveNIS, 4, 0);          /* link -4 */
    uint32_t r = 0;
    WITH_PUSH32((uint32_t)(uint16_t)time, r = DoDissolve());   /* DoDissolve's long argument */
    return (uint16_t)r;
}

/* 00044b94: play sound snd, show text item `item` of 'TEXT' id, wait for the sound */
uint32_t NISDialog(int16_t id, int16_t item, int16_t snd) {
    ENTRY_LINK(FN_NISDialog, 4, 0);            /* link -4 */
    PlaySound(-1, snd, 1);
    NISTextDraw(id, item);
    return (uint16_t)WaitSndId(snd, 1);
}

/* 00044bd6: key reader installed while a NIS runs: 1 quit, 2 abort, 0x1b escape/click */
uint32_t FUN_00044bd6(void) {
    ENTRY_NOLINK(FN_FUN_00044bd6, 8);          /* movem D3/D5 */
    int16_t k = S16(FUN_000550b8());
    if (R16(gQuit) != 0) k = 1;
    else if (R16(gNISAbort) != 0) k = 2;
    else if (k != 0x1b && k != 0x20) {
        if (R16(gEvent) == 1 || R16(gKeyPending) != 0) k = 0x1b;
        else k = 0;
    }
    W16(gNISKey, 0);
    return (uint16_t)k;
}

/* 00044c22: install the NIS key reader (the previous one is kept in gSavedKeyProc) */
uint32_t FUN_00044c22(void) {
    W32(gSavedKeyProc, R32(gReadKeyProc));
    W32(gReadKeyProc, JT_ADDR(FN_FUN_00044bd6));
    W16(gNISKey, 0);
    return 0;
}

/* 00044c38: draw SHAP `id` at (x, y) in the current port */
uint32_t DrawSHAPfromID(int16_t id, int16_t x, int16_t y, int16_t mode, uint32_t rect) {
    ENTRY_LINK(FN_DrawSHAPfromID, 0, 4);       /* move.l A3 */
    uint32_t h = GetShape(id);
    if (h != 0) {
        _HLock(h);
        DrawIt(R32(h), 0, x, y, mode, 0, rect);
        _HUnlock(h);
        _HPurge(h);
    }
    return 0;
}

/* 00044c92: NIS 9: aboard the ship (with the captain's dialogue) */
uint32_t AboardShip(void) {
    ENTRY_LINK(FN_AboardShip, 0x10, 16);       /* link -0x10; movem D3-D6 */
    const maddr game = a6 - 0x10, pic = a6 - 8;
    PlaySound(-1, 0x6dc4, 0);
    SetGameRect(game);
    ClearNIS(gOffWorld, 1, game);
    mcopy(pic, gNISRect, 8);
    MoveRect(pic, 0x40, 0x3e);
    if (R8(gColorOK) == 0) return 1;
    int16_t r;                                  /* D6 */
    GetSetFadeCTAB(0x61a9, 0x45, 0x40001);
    GetSetFadeCTAB(0x6d61, 0x46, 0x40001);
    NISTextDraw(0, 0);
    AddNIS(0x61a9, 0, 0, 0, 0);
    AddNIS(0x61aa, 0, 0x129, 0, 0);
    AddNIS(0x6d61, 0x40, 0x3e, 0, 0);
    CopyNIS(game);
    r = S16(WaitCue(0x61));
    if (r == 0) {
        FadeInBothCTBLs(0x40001);
        CreateSound(-1, 0x6d61, 1);
        r = S16(WaitCue(0x62));
    }
    if (r == 0) {
        W16(gBadgeShape, 0x61ad);
        W16(gBadgeDx, 0x85);
        W16(gBadgeDy, -9);
        W16(gBadgePending, 1);
        r = nis_dialog(0x6d60, 1, 0x6d61, 0xba);
    }
    if (r == 0) r = S16(WaitCue(0x63));
    if (r == 0) {
        GetSetFadeCTAB(0x6d62, 0x66, 0x40001);
        ClearNIS(gOffWorld, 0xff, pic);
        AddNIS(0x6d62, 0x40, 0x3e, 0x10, 0);
        AddNIS(0x6d63, 0x40, 0xc7, 0x10, 0);
        CopyNIS(pic);
        NISTextDraw(0, 0);
        if (r == 0) {
            FadeInColors(R32(gCTab2), R32(gGameWindow), 1);
            r = S16(Wait(0, 0x1e));
        }
    }
    if (r == 0) {
        CreateSound(-1, 0x6d62, 1);
        r = S16(WaitCue(0x64));
    }
    if (r == 0) {
        r = nis_dialog(0x6d60, 2, 0x6d62, 0x83);
        if (r == 0) r = S16(Wait(0, 0x14));
    }
    if (r == 0) {
        CreateSound(-1, 0x6d63, 1);
        r = S16(WaitCue(0x65));
    }
    if (r == 0) {
        r = nis_dialog(0x6d60, 3, 0x6d63, 0xc8);
        if (r == 0) r = S16(Wait(0, 0x14));
    }
    if (r == 0) {
        NISTextDraw(0, 0);
        r = S16(Wait(0, 0x48));
    }
    if (r == 0) {
        CreateSound(-1, 0x6d65, 1);
        r = S16(WaitCue(0x66));
        if (r == 0) {
            SetSoundId(0x6d65, 1);
            FadeOutColors(R32(gCTab2), R32(gGameWindow), 0x10001);
        }
    }
    if (r == 0) r = S16(WaitCurrentSound(1));
    if (r == 0) {
        GetSetFadeCTAB(0x6d64, 0x46, 0x90001);
        ClearNIS(gOffWorld, 0xff, pic);
        AddNIS(0x6d64, 0x40, 0x3e, 0x10, 0);
        CopyNIS(pic);
        r = S16(WaitCue(0x67));
        if (r == 0) FadeInColors(R32(gCTab2), R32(gGameWindow), 0x20002);
    }
    if (r == 0) {
        CreateSound(-1, 0x6595, 1);
        r = S16(WaitCue(0x68));
    }
    if (r == 0) {
        SetSoundId(0x6595, 1);
        NISTextDraw(0x6d60, 4);
        r = S16(WaitCurrentSound(1));
    }
    if (r == 0) {
        int16_t item = S16(CreateSound(-1, 0x7724, 1));
        SetSoundItemKeepVal(item, 2);
        r = S16(WaitCue(0x69));
    }
    if (r == 0) SetSoundId(0x6595, 1);
    if (r == 0) {
        SetTimer(0, 0, 7);
        NISTextDraw(0, 0);
        AddNIS(0x6d65, 0x40, 0x3e, 0x10, 0);
        CopyNIS(pic);
        r = S16(WaitTimer(0));
    }
    if (r == 0) {
        SetTimer(0, 0, 0x18);
        SetNISCTAB(0x52, 0x100);
        SetNISCTAB(0x52, 0xa00);
        SetNISCTAB(0x4a, 0);
        CreateSound(-1, 0x6dc5, 0);
        r = S16(WaitTimer(0));
    }
    if (r == 0) {
        GetSetFadeCTAB(0x6d62, 0x4a, 0x90001);
        ClearNIS(gOffWorld, 0xff, pic);
        AddNIS(0x6d62, 0x40, 0x3e, 0x10, 0);
        AddNIS(0x6d67, 0x40, 0xa3, 0x10, 0);
        SetSoundId(0x6dc5, 0);
        SetTimer(0, 0, 0x12);
        CopyNIS(pic);
        if (r == 0) r = S16(WaitTimer(0));
    }
    if (r == 0) {
        SetNISCTAB(0x52, 0xa00);
        SetNISCTAB(0x52, 0x100);
        SetNISCTAB(0x52, 100);
        r = S16(WaitCue(0x61));
    }
    if (r == 0) {
        GetSetFadeCTAB(0x6d68, 0x66, 0x40001);
        ClearNIS(gOffWorld, 0xff, pic);
        AddNIS(0x6d68, 0x40, 0x3e, 0x10, 0);
        if (r == 0) {
            CopyNIS(pic);
            FadeInColors(R32(gCTab2), R32(gGameWindow), 1);
        }
    }
    if (r == 0) {
        SetTimer(0, 0, 8);
        AddNIS(0x6d68, 0x40, 0x3e, 0x10, 0);
        AddNIS(0x6d69, 0x40, 0x3e, 0x10, 0);
        CopyNIS(pic);
        r = S16(WaitTimer(0));
    }
    if (r == 0) {
        SetTimer(0, 0, 8);
        SetSoundId(0x7724, 1);
        ClearNIS(gOffWorld, 0xff, pic);
        AddNIS(0x6d68, 0x40, 0x3e, 0x10, 0);
        AddNIS(0x6d6a, 0x40, 0x3e, 0x10, 0);
        CopyNIS(pic);
        r = S16(WaitTimer(0));
    }
    if (r == 0) {
        ClearNIS(gOffWorld, 0xff, pic);
        AddNIS(0x6d68, 0x40, 0x3e, 0x10, 0);
        AddNIS(0x6d6b, 0x40, 0x3e, 0x10, 0);
        r = S16(WaitCue(0x62));
    }
    if (r == 0) {
        CopyNIS(pic);
        r = S16(Wait(0, 6));
    }
    if (r == 0) {
        SetNISCTAB(0x52, 0x100);
        SetNISCTAB(0x52, 0xa00);
        SetNISCTAB(0x4a, 0);
        SetTimer(0, 0, 0xc);
        ClearNIS(gOffWorld, 0xff, pic);
        AddNIS(0x6d6d, 0x40, 0x3e, 0x10, 0);
        AddNIS(0x6d6e, 0x6e, 0x4c, 0x10, 0);
        CopyNIS(pic);
        r = S16(WaitTimer(0));
        if (r == 0) {
            SetNISCTAB(0x52, 0xa00);
            SetNISCTAB(0x52, 0x100);
            SetNISCTAB(0x52, 100);
            r = S16(WaitCurrentSound(1));
        }
    }
    if (r == 0) {
        W32(gSndFile, R32(gSndFileAlt));
        CreateSound(-1, 0x271d, 1);
        r = S16(WaitCue(0x63));
    }
    if (r == 0) {
        SetTimer(0, 0, 0xc);
        SetSoundId(0x271d, 1);
        AddNIS(0x6d6d, 0x40, 0x3e, 0x10, 0);
        AddNIS(0x6d6f, 0x6d, 0x4c, 0x10, 0);
        CopyNIS(pic);
        r = S16(WaitTimer(0));
    }
    if (r == 0) {
        SetTimer(0, 0, 0xc);
        AddNIS(0x6d6d, 0x40, 0x3e, 0x10, 0);
        AddNIS(0x6d70, 0x6d, 0x4c, 0x10, 0);
        CopyNIS(pic);
        r = S16(WaitTimer(0));
    }
    if (r == 0) {
        AddNIS(0x6d6d, 0x40, 0x3e, 0x10, 0);
        AddNIS(0x6d71, 0x6d, 0x4c, 0x10, 0);
        CopyNIS(pic);
        r = S16(WaitCue(100));
    }
    if (r == 0) r = S16(WaitCurrentSound(1));
    if (r == 0) {
        FadeBothCTBLs(0x10001);
    } else {
        ResetSoundItem(0);
        RapidFadeALL(1);
    }
    PurgeSounds();
    return (uint16_t)r;
}

/* 000455d0: NIS 1: the flying carpet */
uint32_t NISCarpet(void) {
    ENTRY_LINK(FN_NISCarpet, 0x10, 24);        /* link -0x10; movem D3-D7/A2 */
    const maddr game = a6 - 0x10;
    OpenRsrcVerify(rfLevelA);
    OpenRsrcVerify(rfLevelB);
    OpenRsrcVerify(rfCarpet1);
    GetSetFadeCTAB(0x61a9, 0x46, 0x40001);
    SaveCurrentWorld(gOffWorld);
    clip_rect(gPlayRect);
    PoPFillRect(gPlayRect, 0x48);
    RestoreOriginalWorld(gOffWorld);
    clip_rect(gPlayRect);
    AddNIS(0x6324, 0x6c, -0xb, 0x10, 0);
    AddNIS(0x6313, 1, 0x31, 0x10, 0);
    AddNIS(0x6325, 0, 0xdc, 0x10, 0);
    int16_t r = S16(PlayTheAnimation(0x61a8, 1, 1, JT_ADDR(FN_TransFunc1),
                                     0, 0, 0x100, 0x1c, 0, 0, 0));    /* D6 */
    if (r == 0) FadeOutColors(R32(gCTab2), R32(gGameWindow), 0x40001);
    else RapidFadeALL(1);
    FUN_00046940();
    CloseRsrcFile(rfCarpet1);
    if (r == 0 && R16(gCurCue) < 0x62) {
        SetGameRect(game);
        ClearNIS(gOffWorld, 0xff, game);
        CopyNIS(game);
        OpenRsrcVerify(rfCarpet2);
        GetSetFadeCTAB(0x61a9, 0x46, 0x40001);
        W32(gSndFile, 0);
        SaveCurrentWorld(gOffWorld);
        PoPFillRect(gPlayRect, 0x4e);
        RestoreOriginalWorld(gOffWorld);
        AddNIS(0x6315, 0xbb, 0, 0x10, 0);
        AddNIS(0x6310, 0xd0, 0x71, 0x10, 0);
        AddNIS(0x6311, 0x49, 0x91, 0x10, 0);
        AddNIS(0x6314, -0x16c, 0xba, 0x10, 0);
        r = S16(PlayTheAnimation(0x61a8, 2, 1, JT_ADDR(FN_TransFunc1),
                                 0, 0, 0x100, 0x1c, 0x4e, 0, 0));
        if (r == -1) r = 0;
        if (r == 0) FadeOutColors(R32(gCTab2), R32(gGameWindow), 0x40001);
        else RapidFadeALL(1);
        FUN_00046940();
        CloseRsrcFile(rfCarpet2);
    }
    if (r == 0) {
        ClearNIS(gOffWorld, 0xff, game);
        OpenRsrcVerify(rfCarpet3);
        int16_t cur = S16(_CurResFile());       /* D7 */
        _UseResFile(R16(rfLevelB));
        GetSetFadeCTAB(0xdae, 0x45, 0x40001);
        SaveCurrentWorld(gOffWorld);
        DrawSHAPfromID(0x122a, 0, 0, 0, 0);
        DrawSHAPfromID(0x122b, 0, 0x87, 0x10, 0);
        RestoreOriginalWorld(gOffWorld);
        _UseResFile(cur);
        CopyNIS(game);
        FadeInColors(R32(gCTab1), R32(gGameWindow), 0x40001);
        GetSetFadeCTAB(0x61a9, 0x52, 0x40001);
        uint32_t saved = R32(gKidPalPtr);       /* A2 */
        W32(gKidPalPtr, 0);
        SetKidPalette(4);
        W32(gKidPalPtr, saved);
        r = S16(PlayTheAnimation(0x61a8, 3, 1, JT_ADDR(FN_TransFunc3),
                                 0, 0, 0, 0xc, 0, 0, 0));
        FUN_00046940();
        CloseRsrcFile(rfCarpet3);
    }
    CloseRsrcFile(rfLevelB);
    CloseRsrcFile(rfLevelA);
    return (uint16_t)r;
}

/* 00045962: frame hook of the carpet animations */
uint32_t TransFunc1(int16_t frame) {
    ENTRY_LINK(FN_TransFunc1, 0, 4);           /* move.l D3 */
    int16_t r = 0;
    if (frame == 1) {
        mcopy(gCarpetRect, gPlayRect, 8);
        DrawSOffWorldToWindow(gOffWorld, R32(gGameWindow), gPlayRect, gPlayRect, 0);
    } else if (frame == 2) {
        r = S16(WaitCue(0x61));
        FadeInColors(R32(gCTab2), R32(gGameWindow), 0x40001);
    }
    if (R16(gCurCue) >= 0x62) r = -1;
    return (uint16_t)r;
}

/* 000459e4: frame hook: wait for cue 0x63 on frame 1 */
uint32_t TransFunc3(int16_t frame) {
    ENTRY_LINK(FN_TransFunc3, 0, 4);           /* move.l D3 */
    int16_t r = 0;
    if (frame == 1) r = S16(WaitCue(0x63));
    return (uint16_t)r;
}

/* 00045a16: NIS 10: the discovery */
uint32_t Discovery(void) {
    ENTRY_LINK(FN_Discovery, 0x1c, 8);         /* link -0x1c; movem D3/D4 */
    const maddr game = a6 - 0x18, small = a6 - 0x10, pic = a6 - 8;
    PlaySound(-1, 0x71ac, 0);
    SetTimer(0, 0, 0x168);
    SetGameRect(game);
    ClearNIS(gOffWorld, 1, game);
    mcopy(pic, gNISRect, 8);
    MoveRect(pic, 0x40, 0x3e);
    if (R8(gColorOK) == 0) return 1;
    int16_t r;                                  /* D4 */
    GetSetFadeCTAB(0x61a9, 0x45, 0x40001);
    GetSetFadeCTAB(0x715e, 0x46, 0x40001);
    NISTextDraw(0, 0);
    AddNIS(0x61a9, 0, 0, 0, 0);
    AddNIS(0x61aa, 0, 0x129, 0, 0);
    AddNIS(0x715e, 0x40, 0x3e, 0, 0);
    CopyNIS(game);
    FadeInBothCTBLs(0x40001);
    CreateSound(-1, 0x7149, 1);
    r = S16(WaitTimer(0));
    if (r == 0) {
        SetTimer(0, 0, 0x46);
        SetSoundId(0x7149, 1);
        W16(gBadgeShape, 0x61ae);
        W16(gBadgeDx, 0x8e);
        W16(gBadgeDy, -2);
        W16(gBadgePending, 1);
        NISTextDraw(0x7148, 1);
    }
    if (r == 0) r = S16(WaitTimer(0));
    if (r == 0) {
        NISTextDraw(0x7148, 2);
        CreateSound(-1, 0x714a, 1);
        r = S16(WaitCurrentSound(1));
        SetTimer(0, 0, 0x5a);
        if (r == 0) r = S16(WaitTimer(0));
    }
    if (r == 0) r = nis_dialog(0x7148, 3, 0x714a, 0x9d);
    if (r == 0) {
        SetTimer(0, 0, 0x28);
        CreateSound(-1, 0x714b, 1);
        r = S16(Wait(0, 0x28));
    }
    if (r == 0) r = nis_dialog(0x7148, 4, 0x714b, 0xbb);
    if (r == 0) r = S16(Wait(0, 0x14));
    if (r == 0) {
        NISTextDraw(0, 0);
        ClearNIS(gOffWorld, 0xff, pic);
        AddNIS(0x714d, 0x40, 0x3e, 0, 0);
        AddNIS(0x714a, 0x4d, 0x4e, 0x10, game);
        CreateSound(-1, 0x714c, 1);
        r = S16(WaitCue(0x61));
    }
    if (r == 0) {
        GetSetFadeCTAB(0x714a, 0x66, 0x90001);
        CopyNIS(pic);
        FadeInColors(R32(gCTab2), R32(gGameWindow), 0x40001);
    }
    if (r == 0) r = S16(Wait(0, 0x3c));
    if (r == 0) r = nis_dialog(0x7148, 5, 0x714c, 0xa8);
    if (r == 0) {
        SetTimer(0, 0, 0x1e);
        CreateSound(-1, 0x714d, 1);
        r = S16(WaitTimer(0));
    }
    if (r == 0) r = nis_dialog(0x7148, 6, 0x714d, 0x89);
    if (r == 0) {
        SetTimer(0, 0, 0x1e);
        CreateSound(-1, 0x714e, 1);
        r = S16(WaitTimer(0));
    }
    if (r == 0) r = nis_dialog(0x7148, 7, 0x714e, 0xdc);
    if (r == 0) {
        CreateSound(-1, 0x7725, 1);
        r = S16(WaitCue(0x62));
    }
    if (r == 0) Wait(0, 0xa);
    if (r == 0) SetSoundId(0x7725, 1);
    if (r == 0) {
        CreateSound(-1, 0x714f, 1);
        r = S16(WaitCurrentSound(1));
    }
    if (r == 0) {
        NISTextDraw(0, 0);
        ClearNIS(gOffWorld, 0xff, pic);
        AddNIS(0x714d, 0x40, 0x3e, 0, 0);
        AddNIS(0x714b, 0x4d, 0x3f, 0x10, small);
        SetTimer(0, 0, 0x3c);
        SetSoundId(0x714f, 1);
        r = S16(DissolveNIS(0xf));
    }
    if (r == 0) r = S16(WaitTimer(0));
    if (r == 0) {
        SetTimer(0, 0, 0x5a);
        SetNISCTAB(0x46, 0);
        GetSetFadeCTAB(0x714e, 0x46, 0x90001);
        ClearNIS(gOffWorld, 0xff, pic);
        AddNIS(0x7150, 0x62, 0x3e, 0, 0);
        AddNIS(0x714e, 0x40, 0x3e, 0x10, 0);
        CopyNIS(pic);
        SetNISCTAB(0x52, 100);
        CreateSound(-1, 0x7150, 1);
        r = S16(WaitTimer(0));
    }
    if (r == 0) {
        GetSetFadeCTAB(0x714a, 0x46, 0x90001);
        ClearNIS(gOffWorld, 0xff, pic);
        AddNIS(0x714d, 0x40, 0x3e, 0, 0);
        AddNIS(0x714b, 0x4d, 0x3f, 0x10, game);
        CopyNIS(pic);
        SetNISCTAB(0x52, 100);
        r = nis_dialog(0x7148, 8, 0x7150, 0x42);
    }
    if (r == 0) {
        SetTimer(0, 0, 0x28);
        CreateSound(-1, 0x71ad, 0);
        CreateSound(-1, 0x7724, 1);
        r = S16(WaitTimer(0));
    }
    if (r == 0) {
        SetTimer(0, 0, 6);
        SetSoundId(0x7724, 1);
        ClearNIS(gOffWorld, 0xff, pic);
        AddNIS(0x714d, 0x40, 0x3e, 0, 0);
        AddNIS(0x714c, 0x4d, 0x3e, 0x10, game);
        NISTextDraw(0, 0);
        CopyNIS(pic);
        r = S16(WaitTimer(0));
    }
    if (r == 0) {
        SetTimer(0, 0, 0xa);
        SetNISCTAB(0x52, 0x100);
        SetNISCTAB(0x52, 0xa00);
        SetNISCTAB(0x4a, 0);
        SetSoundId(0x71ad, 0);
        r = S16(WaitTimer(0));
    }
    if (r == 0) {
        SetTimer(0, 0, 0x9c);
        GetSetFadeCTAB(0x714e, 0x4a, 0x90001);
        ClearNIS(gOffWorld, 0xff, pic);
        AddNIS(0x7150, 0x62, 0x3e, 0, 0);
        AddNIS(0x714e, 0x40, 0x3e, 0x10, 0);
        CopyNIS(pic);
        SetNISCTAB(0x52, 0xa00);
        SetNISCTAB(0x52, 0x100);
        SetNISCTAB(0x52, 100);
        r = S16(WaitTimer(0));
    }
    if (r == 0) {
        SetTimer(0, 0, 0xf0);
        AddNIS(0x7150, 0x62, 0x3e, 0, 0);
        AddNIS(0x714f, 0x40, 0x3e, 0x10, 0);
        r = S16(DissolveNIS(0x4b));
    }
    if (r == 0) r = S16(WaitTimer(0));
    if (r == 0) {
        GetSetFadeCTAB(0x7151, 0x66, 0x10001);
        ClearNIS(gOffWorld, 0xff, pic);
        AddNIS(0x715d, 0x40, 0x3e, 0, 0);
        AddNIS(0x7151, 0x132, 0xaa, 0x10, 0);
        CopyNIS(pic);
        CreateSound(-1, 0x71ae, 0);
        r = S16(WaitCurrentSound(0));
    }
    if (r == 0) {
        SetSoundId(0x71ae, 0);
        FadeInColors(R32(gCTab2), R32(gGameWindow), 0x40001);
        SetTimer(0, 0, 0x3c);
        CreateSound(-1, 0x7151, 1);
        r = S16(WaitTimer(0));
    }
    if (r == 0) {
        r = nis_dialog(0x7148, 9, 0x7151, 0x11e);
        if (r == 0) {
            SetTimer(0, 0, 0x19);
            CreateSound(-1, 0x7152, 1);
            r = S16(WaitTimer(0));
        }
    }
    if (r == 0) r = S16(PlayTheAnimation(0x7148, 1, 1, 0, 0, 0, 0x100, 6, 0, 0x3f, 0x3d));
    if (r == 0) r = nis_dialog(0x7148, 0xa, 0x7152, 0x11e);
    if (r == 0) {
        SetTimer(0, 0, 0x19);
        CreateSound(-1, 0x7153, 1);
        r = S16(WaitTimer(0));
    }
    if (r == 0) r = S16(PlayTheAnimation(0x7148, 2, 1, 0, 0, 0, 0x100, 6, 0, 0x3f, 0x3d));
    if (r == 0) r = nis_dialog(0x7148, 0xb, 0x7153, 0xc8);
    if (r == 0) r = S16(Wait(0, 0x78));
    if (r == 0) r = S16(PlayTheAnimation(0x7148, 3, 1, 0, 0, 0, 0x100, 6, 0, 0x3f, 0x3d));
    if (r == 0) FadeBothCTBLs(0x40001);
    else RapidFadeALL(1);
    ResetSoundItem(0);
    PurgeSounds();
    return (uint16_t)r;
}

/* SetColors on a colour table for the three SetNISCTAB flags 4, 8 and 0x10 */
static void nis_set_colors(maddr slot, uint32_t h, uint16_t flags, int16_t which, int16_t level) {
    if (flags & 4) SetColors(slot ? R32(slot) : h, which, 0, 1);
    if (flags & 8) SetColors(slot ? R32(slot) : h, (int16_t)(which | 0x10), 0, 1);
    if (flags & 0x10) SetColors(slot ? R32(slot) : h, which, level, 100);
}

/* 00046440: set the screen colours from the NIS colour tables.
 * flags: 3 both tables combined, 1 table 1, 2 table 2, 0x2000/0x4000 table 1/2 blended
 * with table 3 at `level` percent; 4/8/0x10 which colours; 0x80 the second colour range;
 * 0x40 show the colours. */
uint32_t SetNISCTAB(int16_t flags, int16_t level) {
    ENTRY_LINK(FN_SetNISCTAB, 0, 16);          /* movem D3-D5/A2 */
    uint16_t f = (uint16_t)flags;
    uint32_t h = 0;                             /* A2 */
    int16_t which = (f & 0x80) ? 0x28 : 0x27;  /* D5 */
    if ((f & 3) == 3) {
        h = CombineCtabs(R32(gCTab1), R32(gCTab2));
        if (h != 0) {
            _HLock(h);
            nis_set_colors(0, h, f, which, level);
            _HUnlock(h);
            _HPurge(h);
        }
    } else if (f & 1) {
        if (R32(gCTab1) != 0) nis_set_colors(gCTab1, 0, f, which, level);
    } else if (f & 2) {
        if (R32(gCTab2) != 0) nis_set_colors(gCTab2, 0, f, which, level);
    } else {
        int32_t b2000 = (int16_t)(f & 0x2000);
        if ((b2000 != 0 || (f & 0x4000) != 0) && R32(gCTab3) != 0) {
            if (b2000 != 0 && R32(gCTab1) != 0) h = BlendCtabs(R32(gCTab1), R32(gCTab3), level);
            if ((f & 0x4000) && R32(gCTab2) != 0) h = BlendCtabs(R32(gCTab2), R32(gCTab3), level);
            if (h != 0) {
                if (f & 0x10) SetColors(h, which, 100, 100);
                _HUnlock(h);
                _HPurge(h);
            }
        }
    }
    if (f & 0x40) ShowColors(R32(gGameWindow), 1, 0xfe);
    return 0;
}

/* load 'CTBL' id into one of the three colour table slots (GetSetFadeCTAB) */
static void load_ctab(maddr slot, int16_t id, uint16_t f, uint32_t speed) {
    if (R32(slot) != 0) {
        if (f & 0x20) FadeOutColors(R32(slot), R32(gGameWindow), speed);
        _HUnlock(R32(slot));
        _HPurge(R32(slot));
        W32(slot, 0);
    }
    W32(slot, _GetResource(CTBL, id));
    if (R32(slot) == 0) return;
    _HNoPurge(R32(slot));
    _HLock(R32(slot));
    if (f & 4) SetColors(R32(slot), 0x27, 0, 1);
    if (f & 8) SetColors(R32(slot), 0x37, 0, 1);
    if (f & 0x10) SetColors(R32(slot), 0x27, 1, 1);
    _HUnlock(R32(slot));
}

/* 0004669c: load colour table `id` into slot 1 (flag 1), 2 (flag 2) or 3 (flag 0x1000),
 * fading the old one out first with flag 0x20; 4/8/0x10 set colours, 0x40 shows them */
uint32_t GetSetFadeCTAB(int16_t id, int16_t flags, uint32_t speed) {
    ENTRY_LINK(FN_GetSetFadeCTAB, 0, 4);       /* move.l D3 */
    uint16_t f = (uint16_t)flags;
    if (f & 1) load_ctab(gCTab1, id, f, speed);
    else if (f & 2) load_ctab(gCTab2, id, f, speed);
    else if (f & 0x1000) load_ctab(gCTab3, id, f, speed);
    if (f & 0x40) ShowColors(R32(gGameWindow), 1, 0xfe);
    return 0;
}

/* 00046940: release the three colour tables */
uint32_t FUN_00046940(void) {
    ENTRY_NOLINK(FN_FUN_00046940, 0);
    if (R32(gCTab1) != 0) { _HUnlock(R32(gCTab1)); _HPurge(R32(gCTab1)); W32(gCTab1, 0); }
    if (R32(gCTab2) != 0) { _HUnlock(R32(gCTab2)); _HPurge(R32(gCTab2)); W32(gCTab2, 0); }
    if (R32(gCTab3) != 0) { _HUnlock(R32(gCTab3)); _HPurge(R32(gCTab3)); W32(gCTab3, 0); }
    return 0;
}

/* 0004698a: fade in to tables 1+2 combined */
uint32_t FadeInBothCTBLs(uint32_t speed) {
    ENTRY_LINK(FN_FadeInBothCTBLs, 0, 4);      /* move.l A2 */
    uint32_t h = CombineCtabs(R32(gCTab1), R32(gCTab2));
    if (h != 0) {
        _HLock(h);
        FadeInColors(h, R32(gGameWindow), speed);
        _HUnlock(h);
        _HPurge(h);
    }
    return 0;
}

/* 000469de: fade out from tables 1+2 combined */
uint32_t FadeBothCTBLs(uint32_t speed) {
    ENTRY_LINK(FN_FadeBothCTBLs, 0, 4);        /* move.l A2 */
    uint32_t h = CombineCtabs(R32(gCTab1), R32(gCTab2));
    if (h != 0) {
        _HLock(h);
        FadeOutColors(h, R32(gGameWindow), speed);
        _HUnlock(h);
        _HPurge(h);
    }
    return 0;
}

static int32_t lmul(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }

/* 00046a30: fade colour index `idx` from (r,g,b) to black in 20 steps of `ticks` */
uint32_t FadeIndex(int16_t idx, uint32_t r, uint32_t g, uint32_t b, uint32_t ticks) {
    ENTRY_LINK(FN_FadeIndex, 0x14, 8);         /* link -0x14; movem D3/D4 */
    int16_t pct = 0x5f;                         /* D4 */
    do {
        SetTimer(1, (int16_t)(ticks >> 16), (int16_t)ticks);
        int32_t m = pct;
        W32(a6 - 4, m);
        int32_t rr = lmul((int32_t)r, m) / 100; W32(a6 - 0x14, rr);
        int32_t gg = lmul((int32_t)g, m) / 100; W32(a6 - 0x10, gg);
        int32_t bb = lmul((int32_t)b, m) / 100; W32(a6 - 0xc, bb);
        ShowNthvRGB(R32(gGameWindow), pct, 100, (int16_t)(((uint16_t)idx & 0xff) << 8),
                    (int16_t)((rr & 0xff) << 8), (int16_t)((gg & 0xff) << 8), (int16_t)((bb & 0xff) << 8));
        WaitTimer(1);
        pct = (int16_t)(pct - 5);
    } while (pct >= 0);
    return 0;
}

/* 00046adc: play a sound and wait for it */
uint32_t PlaySoundWait(int16_t snd, uint32_t chan) {
    ENTRY_LINK(FN_PlaySoundWait, 4, 0);        /* link -4 */
    PlaySound(-1, snd, chan);
    return WaitCurrentSound((int16_t)chan);
}

/* 00046b10: wait until the current sound of channel `chan` ends; 1 if a key cut it short */
uint32_t WaitCurrentSound(int16_t chan) {
    ENTRY_LINK(FN_WaitCurrentSound, 0, 8);     /* movem D3/D4 */
    int16_t r = 0;
    int16_t item = chan == 0 ? R16(gSndItemB) : R16(gSndItemA);
    if (item != 0 && (uint16_t)SoundItemOn(item) != 0) {
        do {
            CheckCursorVisibility();
            if ((uint16_t)FUN_00044bd6() != 0) {
                r = 1;
                ResetSoundItem(0);
            }
        } while ((uint16_t)SoundItemOn(item) != 0);
    }
    return (uint16_t)r;
}

/* 00046b7a: show the off-screen world: dissolve it in if dissolve data is ready.
 * The original takes a long (the time) at 8(A6) that the index does not list; DoCreditsDialog
 * (seg02a) and DissolveNIS push it before calling, so it is read from the argument area. */
uint32_t DoDissolve(void) {
    ENTRY_LINK(FN_DoDissolve, 8, 4);           /* link -8; move.l D3 */
    uint32_t time = R32(a6 + 8);
    int16_t r = 0;
    if (R32(gDisData) != 0) {
        _HideCursor();
        r = S16(DrawDisolveData(gOffWorld, R32(gDisData), time));
        _ShowCursor();
        _ObscureCursor();
    } else {
        SetGameRect(a6 - 8);
        DrawSOffWorldToWindow(gOffWorld, R32(gGameWindow), a6 - 8, a6 - 8, 0);
    }
    return (uint16_t)r;
}

/* 00046be0: prepare the dissolve data for area kind 0 (NIS picture), 1 (whole screen) or
 * 2 (gDisRect2): loaded from the options file ('DSLV') or built and saved */
uint32_t InitDissolve(int16_t kind) {
    ENTRY_LINK(FN_InitDissolve, 8, 16);        /* link -8; movem D3-D6 */
    const maddr rect = a6 - 8;
    int16_t left, top;                          /* D5, D6 */
    if (kind == 1) {
        W16(rect, 0); W16(rect + 2, 0); W16(rect + 4, 0x16d); W16(rect + 6, 0x200);
        left = 0; top = 0;
    } else if (kind == 2) {
        mcopy(rect, gDisRect2, 8);
        left = R16(gDisRect2 + 2);
        top = R16(gDisRect2);
    } else {
        W16(rect, 0x3e); W16(rect + 2, 0x40); W16(rect + 4, 0x124); W16(rect + 6, 0x1c0);
        left = 0x40; top = 0x3e;
    }
    if (R32(gDisData) != 0) return 0;
    int16_t rb = (int16_t)(RU16(R32(R32(R32(gScreenGD) + 2)) + 4) & 0x7ffe);   /* D4 */
    if (kind != 2) LoadSaveDisRes(0, rb, kind);
    if (R32(gDisData) == 0) {
        uint32_t d = MakeDisolveData(gOffWorld, left, top, rect);
        W32(gDisData, d);
        if (kind != 2 && d != 0) {
            LoadSaveDisRes(1, rb, kind);
            SetDisolveDelays(gOffWorld, R32(gDisData));
        } else if (R32(gDisData) != 0) {
            SetDisolveDelays(gOffWorld, R32(gDisData));
        }
    } else {
        SetDisolveDelays(gOffWorld, R32(gDisData));
    }
    return 0;
}

/* 00046d00: release the dissolve data */
uint32_t FUN_00046d00(void) {
    ENTRY_NOLINK(FN_FUN_00046d00, 0);
    if (R32(gDisData) != 0) {
        _HUnlock(R32(gDisData));
        _HPurge(R32(gDisData));
        W32(gDisData, 0);
    }
    return 0;
}

/* 00046d1a: build the dissolve data for `rect` of world: a header {top, left, rect,
 * delay base, delay range} then one {source, destination} offset pair per long word of
 * the area, shuffled with RandBig. Returns the (locked) handle or 0. */
uint32_t MakeDisolveData(uint32_t world, int16_t left, int16_t top, uint32_t rect) {
    ENTRY_LINK(FN_MakeDisolveData, 0x68, 32);  /* link -0x68; movem D3-D7/A2-A4 */
    W32(a6 - 0x50, 0);
    if (R8(world + 0x20) == 0) return 0;
    W32(a6 - 0x38, (int32_t)R16(rect + 6) - (int32_t)R16(rect + 2) + 1);
    W32(a6 - 0x3c, (int32_t)R16(rect + 4) - (int32_t)R16(rect));
    uint32_t pmh = R32(R32(world + 0x14) + 2);
    W32(a6 - 0x40, (int32_t)(int16_t)(RU16(R32(pmh) + 4) & 0x7ffe));
    W32(a6 - 0x30, _GetPixBaseAddr(pmh));
    W32(a6 - 0x30, tc_StripAddress(R32(a6 - 0x30)));
    W32(a6 - 0x2c, R32(a6 - 0x30));
    int32_t off = lmul(S32(R32(a6 - 0x40)), R16(rect)) + R16(rect + 2);
    W32(a6 - 0x30, R32(a6 - 0x30) + (uint32_t)off);
    uint32_t spmh = R32(R32(gScreenGD) + 2);    /* A2 */
    uint32_t spm = R32(spmh);                   /* A3 */
    W32(a6 - 0x44, (int32_t)(int16_t)(RU16(spm + 4) & 0x7ffe));
    W32(a6 - 0x4c, (int32_t)top - (int32_t)R16(spm + 6));
    W32(a6 - 0x48, (int32_t)left - (int32_t)R16(spm + 8));
    W32(a6 - 0x34, _GetPixBaseAddr(spmh));
    off = lmul(S32(R32(a6 - 0x44)), S32(R32(a6 - 0x4c))) + S32(R32(a6 - 0x48));
    W32(a6 - 0x34, R32(a6 - 0x34) + (uint32_t)off);
    W32(a6 - 0x28, R32(a6 - 0x34));
    W32(a6 - 0x38, S32(R32(a6 - 0x38)) / 4);
    int32_t width = S32(R32(a6 - 0x38)) << 2;   /* D3: bytes per row, rounded to longs */
    W32(a6 - 0x38, width);
    int32_t n = lmul(S32(R32(a6 - 0x3c)), width) / 4;    /* D5: number of long words */
    uint32_t h = PoPNewHandle((uint32_t)(n * 8 + 0x1c));
    W32(a6 - 0x50, h);
    if (h == 0) return 0;
    _HLock(h);
    h = R32(a6 - 0x50);
    W16(R32(h), top);
    W16(R32(h) + 2, left);
    mcopy(R32(h) + 4, rect, 8);
    uint32_t e = R32(h) + 0x14;                 /* A4 */
    W16(a6 - 0x68, 0);
    if (0 < S32(R32(a6 - 0x3c))) {
        W32(a6 - 0x1c, (uint32_t)-(int32_t)R32(a6 - 0x28));
        W32(a6 - 0x18, (uint32_t)-(int32_t)R32(a6 - 0x2c));
        W32(a6 - 0xc, R32(a6 - 0x34) + R32(a6 - 0x1c));
        W32(a6 - 8, R32(a6 - 0x30) + R32(a6 - 0x18));
        do {
            int16_t x = 0;                      /* D7 */
            if ((int32_t)x < S32(R32(a6 - 0x38))) {
                W32(a6 - 0x24, R32(a6 - 0xc));
                W32(a6 - 0x20, R32(a6 - 8));
                do {
                    W32(e, R32(a6 - 0x20));
                    W32(e + 4, R32(a6 - 0x24));
                    W32(a6 - 0x20, R32(a6 - 0x20) + 4);
                    W32(a6 - 0x24, R32(a6 - 0x24) + 4);
                    e += 8;
                    x = (int16_t)(x + 4);
                } while ((int32_t)x < S32(R32(a6 - 0x38)));
            }
            W32(a6 - 8, R32(a6 - 8) + R32(a6 - 0x40));
            W32(a6 - 0xc, R32(a6 - 0xc) + R32(a6 - 0x44));
            W16(a6 - 0x68, R16(a6 - 0x68) + 1);
        } while ((int32_t)R16(a6 - 0x68) < S32(R32(a6 - 0x3c)));
    }
    e = R32(R32(a6 - 0x50)) + 0x14;
    W32(a6 - 4, e);
    W32(a6 - 0x54, e);
    uint32_t last = e + (uint32_t)(n << 3);     /* entry n: copy of entry 0 */
    W32(last, R32(e));
    W32(last + 4, R32(e + 4));
    for (int32_t i = 0; i < n; i++) {           /* D6 */
        uint32_t ei = e + (uint32_t)(i << 3);
        W32(a6 - 0x14, ei + 0xc);
        W32(a6 - 0x10, ei + 8);
        int32_t j = S32(RandBig((uint32_t)i, (uint32_t)n));
        uint32_t ej = e + (uint32_t)(j << 3);
        uint32_t d = R32(a6 - 0x54);
        W32(d, R32(ej));
        W32(d + 4, R32(ej + 4));
        W32(ej, R32(R32(a6 - 0x10)));
        W32(ej + 4, R32(R32(a6 - 0x14)));
        W32(a6 - 0x54, R32(a6 - 0x54) + 8);
        W32(a6 - 0x14, R32(a6 - 0x14) + 8);
        W32(a6 - 0x10, R32(a6 - 0x10) + 8);
    }
    return R32(a6 - 0x50);
}

/* 00046fdc: dissolve: copy the long words of the area from world to the screen in the
 * shuffled order of the dissolve data (two passes of a word each), adapting the delay
 * between words so that the whole takes about `time` ticks. The body (0x46ff0) is
 * entered through _StripAddress; it runs in 32-bit addressing mode. Returns 0. */
uint32_t DrawDisolveData(uint32_t world, uint32_t h, uint32_t time) {
    ENTRY_LINK(FN_DrawDisolveData, 0xb0, 32);  /* link -0xb0; movem D3-D7/A2-A4 */
    _StripAddress(fn_address(FN_DrawDisolveData) + 0x14);
    W8(a6 - 0xb0, 0);
    W32(a6 - 0x90, _GetPixBaseAddr(R32(R32(world + 0x14) + 2)));
    W32(a6 - 0x90, tc_StripAddress(R32(a6 - 0x90)));
    if (h == 0) return R8(a6 - 0xb0);
    _HLock(h);
    int32_t delay = S32(DisolveDelay(h, time));
    W32(a6 - 0xa8, delay);
    if (S32(R32(a6 - 0xa8)) < 0) W32(a6 - 0xa8, 0);
    if (S32(R32(a6 - 0xa8)) > 100) W32(a6 - 0xa8, 100);
    uint32_t p = R32(h);
    int16_t top = R16(p), left = R16(p + 2);
    W16(a6 - 0x86, top);
    W16(a6 - 0x88, left);
    uint32_t spmh = R32(R32(gScreenGD) + 2);
    uint32_t spm = R32(spmh);
    W32(a6 - 0x9c, (int32_t)(int16_t)(RU16(spm + 4) & 0x7ffe));
    W32(a6 - 0xa4, (int32_t)top - (int32_t)R16(spm + 6));
    W32(a6 - 0xa0, (int32_t)left - (int32_t)R16(spm + 8));
    W32(a6 - 0x98, _GetPixBaseAddr(spmh));
    int32_t off = lmul(S32(R32(a6 - 0x9c)), S32(R32(a6 - 0xa4))) + S32(R32(a6 - 0xa0));
    W32(a6 - 0x98, R32(a6 - 0x98) + (uint32_t)off);
    W32(a6 - 0x8c, R32(a6 - 0x98));
    int32_t n = (S32(tc_GetHandleSize(h)) - 0x14) / 8;  /* D5 */
    W32(a6 - 0x6c, 0);
    W32(a6 - 0x7c, _TickCount());
    if (time == 0) port_divide_by_zero(__func__);
    W32(a6 - 0x78, (n << 1) / S32(time));
    W32(a6 - 0x74, R32(a6 - 0x78));
    W32(a6 - 0x94, R32(h) + 0x14);
    uint32_t e = tc_StripAddress(R32(h) + 0x14);    /* A4 */
    W32(a6 - 0x94, e);
    if (S32(R32(e)) < S32(R32(a6 - 0x90)) && n != 0) {   /* offsets -> addresses (first use) */
        int32_t k = n;
        do {
            W32(e, R32(e) + R32(a6 - 0x90));
            W32(e + 4, R32(e + 4) + R32(a6 - 0x8c));
            e += 8;
        } while (--k != 0);
    }
    W8(a6 - 0xaf, 1);
    W8(a6 - 0xaf, swap_mmu_mode(R8(a6 - 0xaf)));
    for (int32_t pass = 2; pass != 0; pass--) {     /* D6 */
        int32_t d7 = pass;
        e = R32(a6 - 0x94);
        int32_t k = n;                              /* D4 */
        if (k == 0) continue;
        do {
            d7 += 2;
            uint32_t o = (uint32_t)(d7 & 2);
            W32(a6 - 4, o);
            uint16_t w = RU16(o + R32(e));
            W16(a6 - 0x84, w);
            uint32_t dst = R32(a6 - 4) + R32(e + 4);
            W32(a6 - 0x80, dst);
            W16(dst, w);
            W32(a6 - 0xac, 0xffffffffu);            /* the busy-wait counter ends at -1 */
            W32(a6 - 0x74, R32(a6 - 0x74) - 1);
            if (R32(a6 - 0x74) == 0) {
                W32(a6 - 0x6c, R32(a6 - 0x6c) + 1);
                W32(a6 - 0x74, R32(a6 - 0x78));
                uint32_t now = _TickCount();
                W32(a6 - 0x70, now);
                if (S32(R32(a6 - 0x7c) + R32(a6 - 0x6c)) < S32(now)) {
                    if (R32(a6 - 0xa8) != 0) W32(a6 - 0xa8, R32(a6 - 0xa8) - 1);
                } else if (S32(R32(a6 - 0x7c) + R32(a6 - 0x6c)) > S32(R32(a6 - 0x70))) {
                    W32(a6 - 0xa8, R32(a6 - 0xa8) + 1);
                }
            }
            e += 8;
        } while (--k != 0);
    }
    W8(a6 - 0xaf, swap_mmu_mode(R8(a6 - 0xaf)));
    _HUnlock(h);
    return R8(a6 - 0xb0);
}

/* 0004722e: random number in lo..hi */
uint32_t RandBig(uint32_t lo, uint32_t hi) {
    ENTRY_LINK(FN_RandBig, 0, 4);              /* move.l D3 */
    if (S32(lo) < S32(hi)) {
        int32_t v = (int32_t)(int16_t)_Random() + 0x8000;
        v = lmul(v, S32(hi) - S32(lo));
        v = v / 0x10000;
        v += S32(lo);
        if (v <= S32(hi) && v >= S32(lo)) return (uint32_t)v;
    }
    return lo;
}

/* 00047294: copy rect of the off-screen world to the window */
uint32_t CopyNIS(uint32_t rect) {
    ENTRY_LINK(FN_CopyNIS, 0, 0);
    DrawSOffWorldToWindow(gOffWorld, R32(gGameWindow), rect, rect, 0);
    return 0;
}

/* 000472be: draw SHAP `id` into the off-screen world */
uint32_t AddNIS(int16_t id, int16_t x, int16_t y, int16_t mode, uint32_t rect) {
    ENTRY_LINK(FN_AddNIS, 0, 4);               /* move.l A3 */
    uint32_t h = GetShape(id);
    if (h != 0) {
        _HLock(h);
        SaveCurrentWorld(gOffWorld);
        DrawIt(R32(h), 0, x, y, mode, 0, rect);
        RestoreOriginalWorld(gOffWorld);
        _HUnlock(h);
        _HPurge(h);
    }
    return 0;
}

/* 00047324: draw the sky SHAP 0x10a9 and repeat it 4 times to the right */
uint32_t DrawSkyForNIS(void) {
    ENTRY_LINK(FN_DrawSkyForNIS, 0x10, 4);     /* link -0x10; move.l D3 */
    const maddr src = a6 - 0x10, dst = a6 - 8;
    int16_t k = 4;
    AddNIS(0x10a9, 0, 0, 0, src);
    mcopy(dst, src, 8);
    do {
        MoveRect(dst, R16(a6 - 2), 0);
        DrawOffToOff(gOffWorld, gOffWorld, src, dst, 0);
    } while (--k != 0);
    return 0;
}

/* 00047394: new colour table = a * pct% + b * (100 - pct)% (same sizes; else 0) */
uint32_t BlendCtabs(uint32_t a, uint32_t b, int16_t pct) {
    ENTRY_LINK(FN_BlendCtabs, 0x14, 32);       /* link -0x14; movem D3-D7/A2-A4 */
    uint32_t h = 0;                             /* A3 */
    int16_t na, nb;
    if (a != 0) { _HLock(a); na = R16(R32(a)); } else na = 0;
    if (b != 0) { _HLock(b); nb = R16(R32(b)); } else nb = 0;
    if (na == 0 || na != nb) return h;
    h = PoPNewHandle((uint32_t)(((int32_t)na << 2) + 2));
    if (h == 0) return 0;
    _HLock(h);
    W16(R32(h), na);
    int32_t wa = pct;                           /* D7 */
    W32(a6 - 0xc, 100 - wa);
    uint32_t pa = R32(a) + 2;                   /* A4 */
    W32(a6 - 8, R32(b) + 2);
    W32(a6 - 4, R32(h) + 2);
    for (int16_t i = 0; i < na; i++) {
        for (int c = 0; c < 3; c++) {
            uint32_t x = (uint32_t)lmul(R8(pa), wa); pa++;
            uint32_t pb = R32(a6 - 8); W32(a6 - 8, pb + 1);
            uint32_t y = (uint32_t)lmul(R8(pb), S32(R32(a6 - 0xc)));
            W32(a6 - 0x10, (x + y) / 100u);
            uint32_t pd = R32(a6 - 4); W32(a6 - 4, pd + 1);
            W8(pd, R8(a6 - 0xd));
        }
        uint8_t v = R8(pa); pa++;
        uint32_t pd = R32(a6 - 4); W32(a6 - 4, pd + 1);
        W8(pd, v);
        W32(a6 - 8, R32(a6 - 8) + 1);
    }
    _HUnlock(h);
    return h;
}

/* 00047538 (Pascal): unpack a SHAP (RLE format 0xb100) into a new handle in place of
 * *ph and add `add` (high byte of the word) to every non-zero pixel */
uint32_t UnPackAndMapSHAP(int16_t add, uint32_t ph) {
    ENTRY_LINK(FN_UnPackAndMapSHAP, 0x48, 32); /* link -0x48; movem D3-D7/A2-A4 */
    const maddr rows = a6 - 0x2a, width = a6 - 0x28, stride = a6 - 0x26, hdr = a6 - 0x22;
    const maddr src = a6 - 0x38, dst = a6 - 0x34, rowdst = a6 - 0x30, cnt = a6 - 0x2c, x = a6 - 0x24;
    uint32_t h = 0;                             /* A3 */
    if (ph != 0 && R32(ph) != 0) {
        _HLock(R32(ph));
        uint32_t p = R32(R32(ph));
        W16(rows, R16(p + 6));
        W16(width, R16(p + 4));
        W16(stride, R16(p + 2));
        W16(hdr, R16(p));
        if (R16(hdr) != -0x4f00) {
            _HLock(R32(ph));
            h = R32(ph);
        } else {
            h = PoPNewHandle((uint32_t)(lmul(R16(rows), R16(stride)) + 0xc));
            if (h != 0) {
                _HLock(h);
                W32(src, R32(R32(ph)) + 0xc);
                W32(dst, R32(h) + 0xc);
                for (int16_t row = 0; row < R16(rows); row++) {     /* D7 */
                    W32(a6 - 4, (int32_t)R16(stride));
                    uint32_t rowsrc = R32(src);                    /* A4 */
                    int32_t skip = R16(rowsrc);                    /* D6 */
                    W32(src, R32(src) + 2);
                    W32(rowdst, R32(dst));
                    W16(cnt, 0);
                    while (R16(cnt) < R16(stride)) {
                        W8(R32(rowdst), 0);
                        W32(rowdst, R32(rowdst) + 1);
                        W16(cnt, R16(cnt) + 1);
                    }
                    W32(rowdst, R32(dst));
                    W16(x, 0);
                    while (R16(x) < R16(width)) {
                        uint8_t b = R8(R32(src));
                        W8(a6 - 0x20, b);
                        W32(src, R32(src) + 1);
                        if (b & 0x80) {                             /* run of one byte */
                            W8(a6 - 0x1f, R8(R32(src)));
                            W32(src, R32(src) + 1);
                            W8(a6 - 0x20, (uint8_t)((R8(a6 - 0x20) & 0x7f) + 1));
                            W16(cnt, 0);
                            int32_t n = R8(a6 - 0x20);
                            if (0 < n && R16(x) < R16(width)) {
                                W32(a6 - 0xc, n);
                                do {
                                    W8(R32(dst), R8(a6 - 0x1f));
                                    W32(dst, R32(dst) + 1);
                                    W16(x, R16(x) + 1);
                                    W16(cnt, R16(cnt) + 1);
                                } while ((int32_t)R16(cnt) < S32(R32(a6 - 0xc)) && R16(x) < R16(width));
                            }
                        } else {                                    /* literal bytes */
                            W8(a6 - 0x20, (uint8_t)((R8(a6 - 0x20) & 0x7f) + 1));
                            W16(cnt, 0);
                            int32_t n = R8(a6 - 0x20);
                            if (0 < n && R16(x) < R16(width)) {
                                W32(a6 - 8, n);
                                do {
                                    W8(R32(dst), R8(R32(src)));
                                    W32(dst, R32(dst) + 1);
                                    W32(src, R32(src) + 1);
                                    W16(x, R16(x) + 1);
                                    W16(cnt, R16(cnt) + 1);
                                } while ((int32_t)R16(cnt) < S32(R32(a6 - 8)) && R16(x) < R16(width));
                            }
                        }
                    }
                    W32(src, rowsrc + (uint32_t)skip + 2);
                    W32(dst, R32(rowdst) + R32(a6 - 4));
                }
            }
        }
    }
    if (h != 0) {
        _HLock(h);
        W32(a6 - 0x1c, R32(h));
        W32(a6 - 0x10, lmul(R16(rows), R16(stride)));
        W32(a6 - 0x18, R32(a6 - 0x1c) + 0xc);
        if (R32(a6 - 0x10) != 0) {
            do {
                uint32_t q = R32(a6 - 0x18);
                if (R8(q) != 0) W8(q, R8(q) + (uint8_t)((uint16_t)add >> 8));
                W32(a6 - 0x18, q + 1);
                W32(a6 - 0x10, R32(a6 - 0x10) - 1);
            } while (R32(a6 - 0x10) != 0);
        }
        uint32_t p = R32(a6 - 0x1c);
        W16(p, RU16(hdr) & 0xfeff);
        W16(p + 6, R16(rows));
        W16(p + 4, R16(width));
        W16(p + 2, R16(stride));
        W16(p + 8, 0);
        W16(p + 0xa, 0);
        uint32_t old = R32(ph);
        if (old != h) {
            _HUnlock(old);
            _HPurge(R32(ph));
            W32(ph, h);
        }
        _HUnlock(R32(ph));
        _HNoPurge(R32(ph));
    }
    return 0;
}

/* 00047818: save (save != 0) or load the dissolve data as 'DSLV' a*10+b in the options file */
uint32_t LoadSaveDisRes(int16_t save, int16_t a, int16_t b) {
    ENTRY_LINK(FN_LoadSaveDisRes, 0x10c, 8);   /* link -0x10c; movem D3/D4 */
    const maddr name = a6 - 0x104;
    mcopy(name, gDisResName, 0x100);
    int16_t ref = S16(MakeGameFile(fn_address(FN_LoadSaveDisRes) + 0xe0));  /* ":Data:Prince2.opt" */
    if (ref == -1) return 0;
    int16_t id = (int16_t)(a * 10 + b);
    if (save != 0) {
        W32(a6 - 4, _GetResource(DSLV, id));
        if (R32(a6 - 4) != 0) {
            _RemoveResource(R32(a6 - 4));
            _HUnlock(R32(a6 - 4));
            _HPurge(R32(a6 - 4));
        }
        W32(a6 - 4, R32(gDisData));
        if ((uint16_t)tc_HandToHand(a6 - 4) == 0) {
            _AddResource(R32(a6 - 4), DSLV, id, name);
            _WriteResource(R32(a6 - 4));
        }
    } else {
        FUN_00046d00();
        W32(gDisData, _GetResource(DSLV, id));
        if (R32(gDisData) != 0) {
            _GetResInfo(R32(gDisData), a6 - 0x10c, a6 - 0x108, name);
            _DetachResource(R32(gDisData));
        }
    }
    _CloseResFile(ref);
    return 0;
}

/* 0004790c: time (ticks) a dissolve pass with `delay` takes: the drawing loop of
 * DrawDisolveData run against a dummy destination. Also converts the data's offsets to
 * addresses on first use. The clamped delay is written back into the argument slot. */
uint32_t DissolveTime(uint32_t world, uint32_t h, uint32_t delay) {
    ENTRY_LINK(FN_DissolveTime, 0x5c, 32);     /* link -0x5c; movem D3-D7/A2-A4 */
    W32(a6 + 0x10, delay);                      /* the argument slot (written back below) */
    W32(a6 - 0x10, R32(a6 + 0x10));
    W32(a6 - 0x38, a6 - 0x32);
    uint32_t t0 = _TickCount();
    W32(a6 - 0xc, t0);
    W32(a6 - 8, t0);
    W32(a6 - 0x30, _GetPixBaseAddr(R32(R32(world + 0x14) + 2)));
    W32(a6 - 0x30, tc_StripAddress(R32(a6 - 0x30)));
    if (h != 0) {
        _HLock(h);
        if (S32(R32(a6 + 0x10)) < 0) W32(a6 + 0x10, 0);
        if (S32(R32(a6 + 0x10)) > 100) W32(a6 + 0x10, 100);
        uint32_t p = R32(h);
        int16_t top = R16(p), left = R16(p + 2);
        W16(a6 - 0x26, top);
        W16(a6 - 0x28, left);
        uint32_t spmh = R32(R32(gScreenGD) + 2);
        uint32_t spm = R32(spmh);
        W32(a6 - 0x44, (int32_t)(int16_t)(RU16(spm + 4) & 0x7ffe));
        W32(a6 - 0x4c, (int32_t)top - (int32_t)R16(spm + 6));
        W32(a6 - 0x48, (int32_t)left - (int32_t)R16(spm + 8));
        W32(a6 - 0x40, _GetPixBaseAddr(spmh));
        int32_t off = lmul(S32(R32(a6 - 0x44)), S32(R32(a6 - 0x4c))) + S32(R32(a6 - 0x48));
        W32(a6 - 0x40, R32(a6 - 0x40) + (uint32_t)off);
        W32(a6 - 0x2c, R32(a6 - 0x40));
        int32_t n = (S32(tc_GetHandleSize(h)) - 0x14) / 8;  /* D5 */
        W32(a6 - 0x14, 0);
        W32(a6 - 0x24, _TickCount());
        W32(a6 - 0x20, (n << 1) / 0x3c);
        W32(a6 - 0x1c, R32(a6 - 0x20));
        uint32_t e = R32(h) + 0x14;             /* A4 */
        if (S32(R32(e)) < S32(R32(a6 - 0x30)) && n != 0) {
            int32_t k = n;
            do {
                W32(e, R32(e) + R32(a6 - 0x30));
                W32(e + 4, R32(e + 4) + R32(a6 - 0x2c));
                e += 8;
            } while (--k != 0);
        }
        for (int32_t pass = 2; pass != 0; pass--) {     /* D6 */
            int32_t d7 = pass;
            e = R32(h) + 0x14;
            int32_t k = n;
            if (k == 0) continue;
            do {
                d7 += 2;
                uint32_t o = (uint32_t)(d7 & 2);
                W32(a6 - 4, o);
                uint16_t w = RU16(o + R32(e));
                W16(a6 - 0x34, w);
                W32(a6 - 0x3c, R32(a6 - 4) + R32(e + 4));
                W16(R32(a6 - 0x38), w);             /* to the dummy word at -0x32 */
                W32(a6 - 0x50, 0xffffffffu);        /* the busy-wait counter ends at -1 */
                W32(a6 - 0x1c, R32(a6 - 0x1c) - 1);
                if (R32(a6 - 0x1c) == 0) {
                    W32(a6 - 0x14, R32(a6 - 0x14) + 1);
                    W32(a6 - 0x1c, R32(a6 - 0x20));
                    uint32_t now = _TickCount();
                    W32(a6 - 0x18, now);
                    if (S32(R32(a6 - 0x24) + R32(a6 - 0x14)) < S32(now)) {
                        if (R32(a6 - 0x10) != 0) W32(a6 - 0x10, R32(a6 - 0x10) - 1);
                    } else if (S32(R32(a6 - 0x24) + R32(a6 - 0x14)) > S32(R32(a6 - 0x18))) {
                        W32(a6 - 0x10, R32(a6 - 0x10) + 1);
                    }
                }
                e += 8;
            } while (--k != 0);
        }
        _HUnlock(h);
        W32(a6 - 8, _TickCount());
    }
    return R32(a6 - 8) - R32(a6 - 0xc);
}

/* 00047b3a: busy-wait count per word for a dissolve of `time` ticks (0..) from the
 * calibration stored in the data (+0xc base, +0x10 range) */
uint32_t DisolveDelay(uint32_t h, uint32_t time) {
    uint32_t p = R32(h);
    int32_t base = S32(R32(p + 0xc)), lim = S32(R32(p + 0x10));
    int32_t r = 0;
    if (S32(time) > base && lim - base > 0)
        r = (int32_t)((uint32_t)(S32(time) - base) * 0x13u) / (lim - base);
    return (uint32_t)r;
}

/* 00047b9a: calibrate the dissolve (DissolveTime with no delay and with delay 20) */
uint32_t SetDisolveDelays(uint32_t world, uint32_t h) {
    ENTRY_LINK(FN_SetDisolveDelays, 0, 8);     /* movem D3/D4 */
    _HLock(h);
    DissolveTime(world, h, 0);
    int32_t t0 = S32(DissolveTime(world, h, 0));
    int32_t t20 = S32(DissolveTime(world, h, 0x14));
    W32(R32(h) + 0xc, t0);
    W32(R32(h) + 0x10, t20 - t0);
    _HUnlock(h);
    return 0;
}

/* 00047c16: NIS 11: the happy ending */
uint32_t HappyEnding(void) {
    ENTRY_LINK(FN_HappyEnding, 0x20, 12);      /* link -0x20; movem D3-D5 */
    const maddr game = a6 - 0x20, low = a6 - 0x18, pic = a6 - 0x10, rect = a6 - 8;
    int16_t r = 0;                              /* D4 */
    OpenRsrcVerify(rfPrincess);
    W16(GH(0x105cc0), 0);
    JaffarBurnBabyBurn();
    SetGameRect(game);
    SetForeColor(0xff);
    _PaintRect(game);
    mcopy(pic, gNISRect, 8);
    MoveRect(pic, 0x40, 0x3e);
    if (R8(gColorOK) == 0) return 1;
    GetSetFadeCTAB(0x61a9, 0x45, 0x40001);
    GetSetFadeCTAB(0x7531, 0x46, 0x40001);
    NISTextDraw(0, 0);
    AddNIS(0x61a9, 0, 0, 0, 0);
    AddNIS(0x61aa, 0, 0x129, 0, 0);
    AddNIS(0x7531, 0x40, 0x3e, 0, 0);
    AddNIS(0x7535, 0x5d, 0xae, 0x10, 0);
    CopyNIS(game);
    FadeInBothCTBLs(0x40001);
    PlaySound(-1, 0x753a, 0);
    if (r == 0) {
        AddNIS(0x7532, 0x100, 0x55, 0x10, 0);
        r = S16(WaitCue(0x61));
        if (r == 0) r = S16(DissolveNIS(0xf));
        if (r == 0) r = S16(PlayTheAnimation(0x7530, 2, 1, 0, 0, 0, 0x100, 6, 0, 0x3f, 0x3d));
    }
    if (r == 0) {
        CreateSound(-1, 0x7531, 1);
        r = S16(WaitCue(0x62));
    }
    if (r == 0) {
        W16(gBadgeShape, 0x61af);
        W16(gBadgeDx, 0x75);
        W16(gBadgeDy, -0xe);
        W16(gBadgePending, 1);
        SetSoundId(0x7531, 1);
        NISTextDraw(0x7530, 1);
        AddNIS(0x7537, 0x40, 0x3e, 0, 0);
        r = S16(Wait(0, 6));
        if (r == 0) {
            SetTimer(0, 0, 0x46);
            CreateSound(-1, 0x7532, 1);
            r = S16(WaitTimer(0));
        }
        if (r == 0) {
            FadeOutColors(R32(gCTab2), R32(gGameWindow), 0x10001);
            mcopy(low, game, 8);
            W16(low + 4, 0x129);
            CopyNIS(low);
            FadeInColors(R32(gCTab2), R32(gGameWindow), 0x10001);
        }
        if (r == 0) r = S16(WaitCurrentSound(1));
    }
    if (r == 0) r = S16(WaitCue(0x63));
    if (r == 0) {
        SetTimer(0, 0, 0x5f);
        SetSoundId(0x7532, 1);
        NISTextDraw(0x7530, 2);
        if (r == 0) r = S16(WaitTimer(0));
    }
    if (r == 0) {
        NISTextDraw(0x7530, 3);
        r = S16(WaitCurrentSound(1));
    }
    if (r == 0) r = S16(WaitCue(0x64));
    if (r == 0) FadeBothCTBLs(0x10001);
    if (r == 0) {
        GetSetFadeCTAB(0x76ab, 0x45, 0x10001);
        GetSetFadeCTAB(0x753b, 0x46, 0x10001);
        SetGameRect(rect);
        ClearNIS(gOffWorld, 0xff, rect);
        CopyNIS(rect);
        ShowNthvRGB(R32(gGameWindow), 1, 1, (int16_t)0xfd00, 0x7500, 0x0000, 0x4400);
        ShowNthvRGB(R32(gGameWindow), 1, 1, (int16_t)0xfe00, (int16_t)0xff00, (int16_t)0xcc00, (int16_t)0xe800);
        CreateSound(-1, 0x7544, 0);
        for (uint16_t s = 0x7534; s <= 0x7538; s++) CreateSound(-1, (int16_t)s, 1);
        AddNIS(0x6323, 0, 0, 0, 0);
        AddNIS(0x6324, 0xa8, 0x62, 0x10, 0);
        CopyNIS(rect);
        W16(GH(0x105cbe), 0);
        W16(GH(0x105cbc), 0);
        r = S16(PlayTheAnimation(0x61a8, 0x138c, 1, JT_ADDR(FN_HappyFunc), 0, 0, 0x100, 0x1c, 0, 0, 0));
        if (r == -1) r = 0;
    }
    if (r == 0) {
        FadeBothCTBLs(0x10001);
        WaitSndId(0x7544, 0);
    } else {
        FadeOutAllColors_(R32(gGameWindow), 0x90001);
    }
    ResetSoundItem(0);
    CloseRsrcFile(rfPrincess);
    return (uint16_t)r;
}
