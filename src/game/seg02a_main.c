/* CODE 2 "Main", first half (Ghidra 0x10434-0x1400a): NIS (cut-scene) triggering,
 * the in-game dialog framework (frame, fades, text), save/resume of games and of the
 * rebirth point, copy protection, the Hall of Fame, the credits, and the event loop /
 * menu handling.
 * Ported from the Ghidra decompilation of the original 68k code; addresses in comments
 * are Ghidra addresses (see docs/PORTING.md and docs/notes/seg02a.md). */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* ---- globals (Ghidra addresses) ---- */
#define gGameState      GH(0x102fec)   /* long: pointer to the game state block */
#define GS_LEVELTYPE    0x2186         /*   word: level (graphics set) type */
#define GS_LEVEL        0x218a         /*   word: level number */
#define gSavedGame      GH(0x102fe8)   /* long: saved-game / rebirth-point record (0x3766 bytes) */
#define gResumedGame    GH(0x102de4)   /* word: a saved game was resumed */
#define gNISResult2     GH(0x103010)   /* word: the last NIS returned 2 */
#define gInNIS          GH(0x103000)   /* word: 1 while an NIS / dialog runs (-1 special) */
#define gLevel          GH(0x10302e)   /* word: level to play */
#define gLevelLoaded    GH(0x10306e)   /* word: level whose NIS state is current */
#define gNISLevel       GH(0x103056)   /* word: NIS progression state */
#define gLastTimeNIS    GH(0x10582e)   /* word: last "time running out" NIS shown (-1 none) */
#define gMinutesLeft    GH(0x103032)   /* word: minutes left */
#define gTicksLeft      GH(0x10303a)   /* word: sub-minute counter */
#define gMaxHP          GH(0x103068)   /* word: max hit points */
#define gCopyProtOK     GH(0x102928)   /* word: copy protection already passed */
#define gMainWindow     GH(0x102dbe)   /* long: the game window */
#define gOffWorld       GH(0x102dc2)   /* the off-screen world record */
#define gGameRect       GH(0x106204)   /* Rect: the game area (bottom at +4, right at +6) */
#define pClipRect       GH(0x10609c)   /* long: ClipRect procedure pointer */
#define pSectRect       GH(0x1060a8)   /* long: SectRect function pointer (Pascal Boolean) */
#define pGetKeys        GH(0x106098)   /* long: GetKeys procedure pointer */
#define gReadKeyProc    GH(0x10620c)   /* long: keyboard reader procedure (jump-table address) */
#define gEvent          GH(0x102cae)   /* EventRecord: what */
#define gEvtMessage     GH(0x102cb0)   /*   message */
#define gEvtWhen        GH(0x102cb4)   /*   when */
#define gEvtWhere       GH(0x102cb8)   /*   where */
#define gEvtModifiers   GH(0x102cbc)   /*   modifiers */
#define gQuit           GH(0x103030)   /* word: quit requested */
#define gGameMenu       GH(0x102ce8)   /* long: MenuHandle "Game"/options menu */
#define gEditMenu       GH(0x102cec)   /* long: MenuHandle Edit */
#define gFileMenu       GH(0x102cf0)   /* long: MenuHandle File */
#define gAppleMenu      GH(0x102cf4)   /* long: MenuHandle Apple */
#define gPauseMark      GH(0x102db6)   /* word: game menu item 1 mark (0/1, 2 = locked) */
#define gSavedMBarHeight GH(0x102c5e)  /* word: menu bar height while hidden (0 = shown) */
#define gSoundOn        GH(0x106088)   /* word */
#define gMusicOn        GH(0x1048c8)   /* word */
/* low-memory globals LM_GrayRgn, LM_TEScrpLength, LM_TEScrpHandle, LM_MBarHeight: mac/lowmem.h */

#define GS()            R32(gGameState)

/* ---- stack fidelity ----
 * Deep callees (the file layer in CODE 21, Time Manager / sound callbacks run at trap
 * boundaries) store stack addresses into globals, so the port keeps the original stack
 * depth: every non-leaf function reserves what its 68k prologue used (return address,
 * LINK frame, saved registers), and calls between ported C functions reserve the
 * argument bytes the original pushed.  ENTER gives `f` = A6 - linksize, so the local
 * at A6-k is at f + linksize - k. */
#define ENTER(link, nregs)  FRAME(fr_, 8 + (link) + 4 * (nregs)); const maddr f = fr_ + 4 * (nregs); (void)f
#define ENTER_NOLINK(nregs) FRAME(fr_, 4 + 4 * (nregs))
#define RETURN(v)           do { uint32_t r_ = (uint32_t)(v); FRAME_END(fr_); return r_; } while (0)
#define CALLC(argbytes, call) ({ maddr s_ = emu_sp(); emu_set_sp(s_ - (argbytes)); \
                                 uint32_t c_ = (call); emu_set_sp(s_); c_; })

/* Runtime address of a constant (string) in this code segment, given its Ghidra address. */
static maddr S2(uint32_t ghidra_addr) {
    return fn_address(FN_CheckPlayNIS) + (ghidra_addr - 0x10434);
}

/* sprintf(buf, fmt, arg): the library routine is variadic, call it like the original. */
static void sprintf1(maddr buf, maddr fmt, uint32_t arg) {
    call_ptr(JT_ADDR(FN_tc_sprintf), "lll", buf, fmt, arg);
}

/* CheckOSErr(err, msg, flag): the message pointer occupies two 16-bit slots in the prototype. */
static void check_os_err(int16_t err, maddr msg, int16_t flag) {
    CheckOSErr(err, (int16_t)(msg >> 16), (int16_t)msg, flag);
}

/* Calls a Pascal Boolean function pointer taking three longs; returns the Boolean. */
static int call_pascal_bool3(maddr fn, uint32_t a, uint32_t b, uint32_t c) {
    maddr sp0 = emu_sp();
    push16(0);
    push32(a); push32(b); push32(c);
    call68_addr(fn);
    int r = R8(sp0 - 2);
    emu_set_sp(sp0);
    return r;
}

/* NIS(n) called through its jump-table entry like the original `jsr 0xefa(A5)`: the NIS
 * segment is usually unloaded here (UnloadLevel), and the generated trampoline would load
 * it natively, skipping the _LoadSeg trap (one virtual-time step, and the THINK C LoadSeg
 * patch), which shifts the time measured by osGetTime. */
static uint32_t nis_jt(int16_t n) {
    return call_ptr(JT_ADDR(FN_NIS), "w", n);
}

static int16_t hiword_sign(int16_t v) { return (int16_t)(v < 0 ? -1 : 0); }

/* 00010434: play the NIS (cut scene) that is due, if any */
uint32_t CheckPlayNIS(int16_t p4, int16_t p6) {
    ENTER(0, 2);
    int16_t nis = S16(CALLC(4, IsTimeForNIS(p4, p6)));
    int16_t r;
    if (nis != 0) {
        PurgeSounds();
        r = S16(CALLC(2, PlayNIS(nis)));
    } else {
        r = 0;
    }
    W16(gNISResult2, r == 2 ? 1 : 0);
    RETURN((uint16_t)r);
}

/* 0001048c */
uint32_t EraseScreenAfterNIS(int16_t nis) {
    if (nis == 1 || nis == 100 || nis == 2 || nis == 3 || nis == 6) return 0;
    return 1;
}

/* 000104d8 */
uint32_t EraseScreenBeforeNIS(int16_t nis) {
    if (nis == 100 || nis == 4 || nis == 6) return 0;
    return 1;
}

/* 00010518: is an NIS pending at this level/room (levels 27/22/1 ...) */
uint32_t FUN_00010518(void) {
    int16_t r = 0;
    if (R32(gSavedGame) == 0 && R16(gResumedGame) == 0 && R16(gNISResult2) == 0) {
        uint32_t gs = GS();
        int16_t t = R16(gs + 0x2198);
        if ((t == 0x1b && R16(gs + GS_LEVEL) == 6) ||
            (t == 0x16 && R16(gs + GS_LEVEL) == 10) ||
            (t == 1 && R16(gs + GS_LEVEL) == 0xe) ||
            (R16(GH(0x10303c)) == 9 && R16(gs + GS_LEVEL) == 8))
            r = 1;
    } else if (R32(gSavedGame) != 0 && R16(gResumedGame) == 0 && R16(gNISResult2) == 0 &&
               R16(GH(0x102f44)) == 9 && R16(GS() + GS_LEVEL) == 8) {
        r = 1;
    }
    return (uint16_t)r;
}

/* 000105b0: which NIS to play now (0 = none) */
uint32_t IsTimeForNIS(int16_t p4, int16_t p6) {
    int16_t r = 0;
    if (R16(gLevel) != 0) {
        int16_t loaded = R16(gLevelLoaded);
        if (loaded < R16(gLevel)) return 0;
        int32_t d = (int32_t)R16(gLevel) - (int32_t)loaded;
        if (d < 0) d = -d;
        if (d > 1) return 0;
    }
    if (R16(gNISLevel) > 2 && R16(gCopyProtOK) == 0) return 100;
    if (R16(gNISLevel) == 0) return 0;
    if (p4 == 0 || (p4 != p6 && p6 != -1)) {
        int16_t i = (int16_t)(p4 - 1);
        if (i >= 0 && i <= 12) {
            switch (i + 1) {          /* jump table at 0x10636 */
            case 1: r = 9; break;
            case 2: r = 100; break;
            case 3: r = 10; break;
            case 5: r = 1; break;
            case 9: r = 2; break;
            case 13: r = 3; break;
            default: break;
            }
        }
    }
    if (r != 0) return (uint16_t)r;
    if (p4 < 4) return 0;
    if (R16(gLastTimeNIS) == -1 && p4 >= 4) {
        W16(gLastTimeNIS, 0);
        return 0x14;
    }
    int32_t q = ((int32_t)R16(gMinutesLeft) - 1) / 9;
    int16_t n = (int16_t)(7 - (int16_t)q);
    if (n > R16(gLastTimeNIS)) {
        r = (int16_t)(n + 0x14);
        W16(gLastTimeNIS, n);
    }
    return (uint16_t)r;
}

/* 000106c8: play NIS number nis; returns 2 if it was interrupted, else 1 */
uint32_t PlayNIS(int16_t nis) {
    ENTER(0, 4);
    int16_t res = 1;
    W16(gInNIS, 1);
    int16_t pal = R16(GS() + GS_LEVELTYPE);
    if (S16(CALLC(2, EraseScreenBeforeNIS(nis))) != 0) {
        SetWholePaletteToColor(R32(gMainWindow), 1);
        PoPEraseRectInBothPorts(gGameRect);
    }
    if (nis != 100) {
        UnloadLevel(0);
        if (nis < 0x14 && nis != 6) FUN_00011bce();
    }
    if (nis == 100) {
        CopyProtectionDialog();
    } else if (nis >= 0xc) {
        if (S16(nis_jt((int16_t)(nis - 8))) != 0) res = 2;
    } else if (nis >= 1 && nis <= 6) {
        if (S16(nis_jt(nis)) != 0) res = 2;
        switch (nis) {
        case 1: pal = 4; ResetStandardColors(R32(gMainWindow)); break;
        case 2: pal = 2; break;
        case 3: pal = 6; break;
        }
    } else {
        if (S16(nis_jt(nis)) != 0) res = 2;
    }
    if (S16(CALLC(2, EraseScreenAfterNIS(nis))) != 0 || (res == 2 && pal != 0))
        PoPEraseRectInBothPorts(gGameRect);
    if (pal != 0) SetKidPalette(pal);
    W16(gInNIS, 0);
    RETURN((uint16_t)res);
}

/* 00010804: load level `level` and draw room `room` as the backdrop of an NIS */
uint32_t DrawRoomFromNIS(int16_t level, int16_t room) {
    ENTER(4, 3);
    FUN_00016112();
    int16_t oldType = R16(GS() + GS_LEVELTYPE);
    W16(gLevel, level);
    W16(gLevelLoaded, level);
    W16(GH(0x102f44), 0);
    LoadLevelMap(level);
    OpenLevelRsrcFile(R16(GS() + GS_LEVELTYPE), oldType);
    LoadBGSet(level == 10 ? 0 : 1);
    if (R16(GS() + GS_LEVELTYPE) != oldType) FUN_0001d090();
    uint32_t gs = GS();
    LoadCodeOverlays(R16(gs + GS_LEVELTYPE), R16(gs + 0x21a4));
    SetPrinceLevelSpecPtrs(R16(GS() + GS_LEVELTYPE));
    W16(GH(0x10303e), 0);
    W16(GH(0x10303c), room);
    PrepCut(1);
    DoCleanCut();
    if (level == 10) {
        _EraseRect(GH(0x103014));
        PoPSetPalette(0xdac, 0x40);
    }
    UnloadSet(4);
    UnloadSet(1);
    FUN_0001da46();
    CloseRsrcFile(GH(0x1038dc) + (uint32_t)RU16(GS() + GS_LEVELTYPE) * 6);
    W16(GS() + GS_LEVELTYPE, 0);
    RETURN(0);
}

/* 0001092a: draw a number in the dialog font */
uint32_t DialogDrawNumber(int16_t n, uint32_t pt, uint32_t rect, uint32_t style) {
    ENTER(0xc, 0);
    maddr buf = f;
    sprintf1(buf, S2(0x10972), (uint32_t)(int32_t)n);        /* "%d" */
    RETURN(CALLC(16, DialogDrawStr(buf, pt, rect, style)));
}

/* 00010976: draw a C string in the dialog font with a 3-layer shadow effect, either at
 * a point (rect == 0) or justified in a rect (style = 2 justification words) */
uint32_t DialogDrawStr(uint32_t str, uint32_t pt, uint32_t rect, uint32_t style) {
    ENTER(0x110, 1);
    maddr r = f, p = f + 8, st = f + 0xc, ps = f + 0x10;
    if (style != 0) {
        W32(st, R32(style));
    } else {
        W16(st + 2, 0);
        W16(st, 1);
    }
    _TextFont(0xd5);
    _TextSize(0x14);
    _TextFace(0);
    FUN_0004fbe2();
    SetForeColor(1);
    if (rect != 0) {
        AlterRect(r, rect, 8, 0, 8, 0);
        StringRect(r, hiword_sign(R16(st + 2)), R16(st + 2), hiword_sign(R16(st)), R16(st), str);
    } else {
        W32(p, R32(pt));
        W16(p + 2, R16(p + 2) + 8);
        _MoveTo(R16(p + 2), R16(p));
        tc_strcpy(ps, str);
        tc_CtoPstr(ps);
        _DrawString(ps);
    }
    SetForeColor(0x10);
    if (rect != 0) {
        AlterRect(r, rect, 6, 0, 6, 0);
        StringRect(r, hiword_sign(R16(st + 2)), R16(st + 2), hiword_sign(R16(st)), R16(st), str);
    } else {
        int16_t h = (int16_t)(R16(pt + 2) + 6);
        W16(p + 2, h);
        _MoveTo(h, R16(p));
        _DrawString(ps);
    }
    SetForeColor(0x1b);
    if (rect != 0) {
        AlterRect(r, rect, 5, 0, 5, 0);
        StringRect(r, hiword_sign(R16(st + 2)), R16(st + 2), hiword_sign(R16(st)), R16(st), str);
    } else {
        int16_t h = (int16_t)(R16(pt + 2) + 5);
        W16(p + 2, h);
        _MoveTo(h, R16(p));
        _DrawString(ps);
    }
    RETURN(seg17_04fc46());
}

/* 00010b1c: draw the 'TEXT' resource `id` with DialogDrawStr */
uint32_t DialogDrawStrId(int16_t id, uint32_t pt, uint32_t rect, uint32_t style) {
    ENTER(0, 2);
    uint32_t h = PoPGetResource(0x54455854, id);   /* 'TEXT' */
    _HLock(h);
    uint32_t text = R32(h);
    CALLC(16, DialogDrawStr(text, pt, rect, style));
    _HUnlock(h);
    RETURN(_HPurge(h));
}

/* 00010b76: show the dialog (off-screen world -> window) and fade its colors in;
 * returns 0 if the user pressed a key other than Esc/Space during the fade */
uint32_t DialogFadeIn(void) {
    ENTER(0x10, 3);
    maddr evt = f;
    DrawSOffWorldToWindow(gOffWorld, R32(gMainWindow), gGameRect, gGameRect, 0);
    uint32_t h = GetCTBLResource(0x1f40);
    FadeInColors(h, R32(gMainWindow), 0x40001);
    _HPurge(h);
    int16_t r = (int16_t)(_EventAvail(-1, evt) & 0xff);
    if (r != 0 && R16(evt) == 3) {
        int16_t c = (int16_t)(RU16(evt + 4) & 0xff);
        if (c != 0x1b && c != 0x20) r = 0;
    }
    RETURN((uint16_t)r);
}

/* 00010c0a: fade the dialog colors out (same result convention as DialogFadeIn) */
uint32_t DialogFadeOut(void) {
    ENTER(0x10, 3);
    maddr evt = f;
    uint32_t h = GetCTBLResource(0x1f40);
    FadeOutColors(h, R32(gMainWindow), 0x40001);
    int16_t r = (int16_t)(_EventAvail(-1, evt) & 0xff);
    if (r != 0 && R16(evt) == 3) {
        int16_t c = (int16_t)(RU16(evt + 4) & 0xff);
        if (c != 0x1b && c != 0x20) r = 0;
    }
    RETURN((uint16_t)r);
}

/* 00010c82: erase the dialog in both worlds and return to game play */
uint32_t DialogResetToGamePlay(int16_t restart) {
    ENTER(8, 2);
    maddr rect = f;
    W32(rect, R32(gGameRect));
    W32(rect + 4, R32(gGameRect + 4));
    uint32_t clip1 = _NewRgn();
    _GetClip(clip1);
    call_ptr(R32(pClipRect), "l", rect);
    SaveCurrentWorld(gOffWorld);
    uint32_t clip2 = _NewRgn();
    _GetClip(clip2);
    call_ptr(R32(pClipRect), "l", rect);
    RestoreOriginalWorld(gOffWorld);
    PoPEraseRectInBothPorts(rect);
    _SetClip(clip1);
    _DisposeRgn(clip1);
    SaveCurrentWorld(gOffWorld);
    _SetClip(clip2);
    _DisposeRgn(clip2);
    RestoreOriginalWorld(gOffWorld);
    if (restart != 0 && R16(GH(0x105830)) == 0) {
        PrepCut(0);
        W16(GH(0x102db8), 1);
    }
    FUN_0001c5a0();
    SetKidPalette(R16(GS() + GS_LEVELTYPE));
    W32(gReadKeyProc, JT_ADDR(FN_FUN_00015a02));
    RETURN(0);
}

/* 00010d56: keyboard reader used while a dialog is up: Esc/Space dismiss it */
uint32_t FUN_00010d56(void) {
    ENTER_NOLINK(2);
    int16_t k = S16(FUN_000550b8());
    if (R16(gQuit) != 0) {
        k = 1;
    } else if (k != 0x1b && k != 0x20) {
        if (R16(gEvent) == 1 || R16(GH(0x103054)) != 0) k = 0x1b;
        else k = 0;
    }
    RETURN((uint16_t)k);
}

/* 00010d92: draw the dialog frame (corner/edge shapes) and fill its inside */
uint32_t DrawDialogFrame(void) {
    ENTER(0xc, 5);
    maddr shp = f, rect = f + 4;
    uint32_t hv = GetShape(0x1f41);
    uint32_t hh = GetShape(0x1f40);
    if (hv == 0 || hh == 0) RETURN(hh);
    SaveCurrentWorld(gOffWorld);
    W32(rect, R32(gGameRect));
    W32(rect + 4, R32(gGameRect + 4));
    W16(rect + 6, R16(rect + 6) - 2);
    call_ptr(R32(pClipRect), "l", rect);
    uint32_t sv = R32(hv);
    uint32_t sh = R32(hh);
    W32(shp, sh);
    int16_t w = R16(sh + 4);
    FlipSHAP(sv, 0);
    DrawIt(sv, 0, w, 0, 0, 0, 0);
    FlipSHAP(sv, 2);
    DrawIt(sv, 0, w, (int16_t)(R16(gGameRect + 4) - R16(sv + 6)), 0, 0, 0);
    sh = R32(hh);
    W32(shp, sh);
    FlipSHAP(sh, 0);
    DrawIt(R32(shp), 0, 0, 0, 0, 0, 0);
    FlipSHAP(R32(shp), 1);
    DrawIt(R32(shp), 0, (int16_t)(R16(GH(0x10301a)) - w), 0, 0, 0, 0);
    _InsetRect(rect, 0xe, 0xe);
    PoPFillRect(rect, 0x19);
    RestoreOriginalWorld(gOffWorld);
    call_ptr(R32(pClipRect), "l", gGameRect);
    _HPurge(hv);
    RETURN(_HPurge(hh));
}

/* 00010ee4: prepare the screen for a dialog */
uint32_t InitDialog(void) {
    ENTER(8, 0);
    maddr rect = f;
    ResetSoundItem(0);
    SaveGamePlayPalette();
    W32(gReadKeyProc, JT_ADDR(FN_FUN_00010d56));
    ClrLifeBar();
    PoPEraseRectInBothPorts(gGameRect);
    W32(rect, R32(gGameRect));
    W32(rect + 4, R32(gGameRect + 4));
    W16(rect + 6, R16(rect + 6) - 2);
    call_ptr(R32(pClipRect), "l", rect);
    SaveCurrentWorld(gOffWorld);
    call_ptr(R32(pClipRect), "l", rect);
    RETURN(RestoreOriginalWorld(gOffWorld));
}

/* 00010f52: may the game be saved now (time left and the kid is standing safely) */
uint32_t FUN_00010f52(void) {
    ENTER_NOLINK(6);
    int16_t ok = 1;
    if (R16(gMinutesLeft) == 0 && R16(gTicksLeft) == 0) RETURN(0);
    if (R16(GH(0x103b70)) == 0) RETURN(1);
    uint32_t chr = R32(GH(0x103b6a));
    uint16_t i = 0;
    if (R16(GH(0x103b70)) <= 0) RETURN(1);
    do {
        uint16_t frame = RU16(chr + 10);
        ok = (frame < 6 || frame > 9) ? 0 : 1;
        if (ok == 0 && frame == 4) {
            int16_t col = (int16_t)(R16(chr) / 0x33);
            int16_t cell = (int16_t)(S16(FirstInRow(R16(chr + 0xc))) + col - 1);
            uint32_t a = (uint32_t)RU16(chr + 4) * 0x78 + (uint32_t)((int32_t)cell * 4) + GS();
            if ((RU16(a + 0x708) & 0xff) == 0xf0) ok = 1;
        }
        chr += 0x10;
        i++;
        if (ok == 0) break;
    } while ((int32_t)(uint32_t)i < (int32_t)R16(GH(0x103b70)));
    RETURN((uint16_t)ok);
}

/* 00011024: "Open saved game..."; returns -1 if cancelled, else ResumeGame's result */
uint32_t ResumeGameDialog(void) {
    ENTER(0x4c, 3);
    maddr reply = f;                    /* SFReply: good +0, vRefNum +6, fName +10 */
    int16_t r;
    if (S16(CALLC(6, GetSaveGameLocation(reply, 0))) == 0) {
        r = -1;
    } else {
        r = S16(CALLC(6, ResumeGame(reply + 0xa, R16(reply + 6))));
        if (r != 0) {
            int16_t lvl = R16(gLevel);
            W16(gNISLevel, lvl);
            W16(gLevelLoaded, lvl);
            W16(GH(0x103054), 1);
        }
    }
    if (r > 0) {
        CopyProtectionDialog();
        if (R16(gPauseMark) == 1) {
            W16(gPauseMark, 0);
            CALLC(4, SetGameMenuItemMark(1, 0));
        }
    }
    RETURN((uint16_t)r);
}

/* 000110a4: "Save game as..."; returns -1 if cancelled, else SaveGame's result */
uint32_t SaveGameDialog(void) {
    ENTER(0x4c, 1);
    maddr reply = f;
    int16_t r;
    if (S16(CALLC(6, GetSaveGameLocation(reply, 1))) != 0)
        r = S16(CALLC(6, SaveGame(reply + 0xa, R16(reply + 6))));
    else
        r = -1;
    CALLC(2, DrawSaveGameMsg(r));
    RETURN((uint16_t)r);
}

/* 000110f2: save under the last name used, or ask for one */
uint32_t FUN_000110f2(void) {
    ENTER_NOLINK(1);
    int16_t r;
    if (R16(GH(0x102c4e)) != 0) {
        r = S16(CALLC(6, SaveGame(GH(0x102b4e), R16(GH(0x102c4e)))));
        CALLC(2, DrawSaveGameMsg(r));
    } else {
        r = S16(SaveGameDialog());
    }
    RETURN((uint16_t)r);
}

/* 00011120: filled and framed box */
uint32_t DialogDrawBox(uint32_t rect, int16_t hilite) {
    ENTER(0, 1);
    PoPFillRect(rect, hilite != 0 ? 0x10 : 0x18);
    FUN_0004fbe2();
    SetForeColor(0x12);
    _FrameRect(rect);
    RETURN(seg17_04fc46());
}

/* 0001116e: "game saved" / "game not saved" message */
uint32_t DrawSaveGameMsg(int16_t r) {
    ENTER(0, 1);
    if (r == -1) RETURN(0);
    if (r != 0) DrawMessage(GH(0x102af0));
    else DrawMessage(GH(0x102afc));
    W16(GH(0x103034), 0x18);
    W16(GH(0x103036), 0x18);
    RETURN(0x18);
}

/* 000111bc: Standard File dialog for saving (save=1) or opening a game; returns reply.good */
uint32_t GetSaveGameLocation(uint32_t reply, int16_t save) {
    ENTER(0x114, 3);
    maddr vref = f, name = f + 4, types = f + 0x104;
    int16_t err = S16(tc_GetVol(vref, 0));
    check_os_err(err, S2(0x112f8), 1);                     /* "Error 1 in GetSaveGameLocation()" */
    if (R16(GH(0x102c4e)) != 0) tc_SetVol(R16(GH(0x102c4e)), 0);
    uint32_t gs = GS();
    int16_t lvl;
    if (R16(gs + GS_LEVEL) == 0xe &&
        (R32(gSavedGame) != 0 || (R16(GH(0x102f44)) == 4 && R16(gs + GS_LEVELTYPE) == 6)))
        lvl = 0xf;
    else
        lvl = R16(GS() + GS_LEVEL);
    sprintf1(name, S2(0x1131a), (uint32_t)(int32_t)lvl);   /* "Level %d" */
    tc_CtoPstr(name);
    if (save == 1) {
        _SFPutFile(R32(GH(0x103c46)), S2(0x11324), name, 0, reply);           /* "Save game as..." */
    } else {
        W32(types, R32(GH(0x102910)));
        _SFGetFile(R32(GH(0x103c46)), S2(0x11336), 0, 1, types, 0, reply);    /* "Open saved game..." */
    }
    if (R8(reply) != 0) {
        tc_PtoCstr(reply + 10);
        tc_strcpy(GH(0x102b4e), reply + 10);
        W16(GH(0x102c4e), R16(reply + 6));
    }
    tc_SetVol(R16(vref), 0);
    _SetCursor(GH(0x106716));
    RETURN(R8(reply));
}

/* 0001134a: allocate (if rec == 0) and initialise a saved-game record with random
 * filler and one of three ID strings.  The original stores the new pointer back
 * into its argument slot: glue_InitSavedGameStruct does that. */
uint32_t InitSavedGameStruct(uint32_t size, uint32_t rec) {
    ENTER(0, 1);
    if (rec == 0) {
        rec = PoPNewPtr(size);
        tc_memset(rec, 0, size);
    }
    for (uint16_t i = 0; i < 0xc; i++) {
        int16_t v = S16(Rnd(100));
        W16(rec + (int32_t)(int16_t)i * 2 + 0x2a, v);
    }
    for (uint16_t i = 0; i < 6; i++) {
        int16_t v = S16(Rnd(10));
        W16(rec + (int32_t)(int16_t)i * 2 + 0x46, v);
    }
    switch (S16(Rnd(2))) {
    case 0: tc_strcpy(rec + 6, GH(0x102b10)); break;
    case 1: tc_strcpy(rec + 6, GH(0x102b22)); break;
    case 2: tc_strcpy(rec + 6, GH(0x102b3c)); break;
    }
    RETURN(rec);
}
void glue_InitSavedGameStruct(void) {
    maddr sp = emu_sp();
    uint32_t r = InitSavedGameStruct(rd32(sp), rd32(sp + 4));
    wr32(sp + 4, r);                    /* the original updates its `rec` argument */
    emu_set_d0(r);
}

/* 0001143a: saving allowed (level <= 14 and the kid alive with hit points) */
uint32_t FUN_0001143a(void) {
    int16_t r = 1;
    if (R16(gLevel) > 0xe) r = 0;
    else if (R16(GH(0x102f4a)) >= 0 || R16(GH(0x102f4c)) <= 0) r = 0;
    return (uint16_t)r;
}

/* 00011468: load a saved game file; returns 1 on success, 0 on failure */
uint32_t ResumeGame(uint32_t fname, int16_t vref) {
    ENTER(0x108, 4);
    maddr refnum = f, oldvol = f + 2, count = f + 4, pname = f + 8;
    int16_t r;
    tc_GetVol(oldvol, 0);
    tc_strcpy(pname, fname);
    tc_CtoPstr(pname);
    int16_t err = S16(tc_FSOpen(refnum, vref, pname));
    if (err != 0) {
        check_os_err(err, S2(0x11622), 0);
        tc_SetVol(R16(oldvol), 0);
        RETURN(0);
    }
    err = S16(tc_GetEOF(count, R16(refnum)));
    if (err != 0) {
        check_os_err(err, S2(0x1164c), 0);
        tc_SetVol(R16(oldvol), 0);
        RETURN(0);
    }
    uint32_t buf = PoPNewPtr(R32(count));
    err = S16(tc_FSRead(buf, count, R16(refnum)));
    if (err != 0) {
        check_os_err(err, S2(0x11676), 0);
        tc_SetVol(R16(oldvol), 0);
        RETURN(0);
    }
    tc_FSClose(R16(refnum));
    r = S16(CALLC(6, SetCheckSavedGameCheckSumValues(buf, 1)));
    if (r != 0) {
        if (R16(buf + 4) != 0) {
            if (R32(gSavedGame) == 0) W32(gSavedGame, PoPNewPtr(R32(count)));
            _BlockMove(buf, R32(gSavedGame), R32(count));
        } else {
            FUN_00011bce();
        }
        int16_t v = R16(buf + 0x26);
        W16(gMinutesLeft, v < 0x4b ? v : 0x4b);
        W16(gTicksLeft, R16(buf + 0x28));
        W16(gLevel, R16(buf + 0x42));
        v = R16(buf + 0x44);
        int16_t hp = v < 0xf ? v : 0xf;
        W16(gMaxHP, hp);
        W16(GH(0x102f4c), hp);
        W16(GH(0x102f4e), hp);
        W16(GH(0x102ff4), R16(buf + 0x54));
        W16(GH(0x102ff6), R16(buf + 0x56));
        W16(GH(0x103058), R16(buf + 0x5c));
        W16(gLastTimeNIS, R16(buf + 0x5e));
        W16(gResumedGame, 1);
    }
    _DisposePtr(buf);
    tc_SetVol(R16(oldvol), 0);
    RETURN((uint16_t)r);
}

/* 000116a0: write a saved game file; returns 1 on success, 0 on failure */
uint32_t SaveGame(uint32_t fname, int16_t vref) {
    ENTER(0x108, 4);
    maddr refnum = f, oldvol = f + 2, count = f + 4, pname = f + 8;
    int16_t err = S16(tc_GetVol(oldvol, 0));
    check_os_err(err, S2(0x1187c), 1);
    tc_strcpy(pname, fname);
    tc_CtoPstr(pname);
    err = S16(tc_Create(R32(GH(0x102910)), 0x50524e63, vref, pname));   /* 'PRNc' */
    if (err != 0 && err != -0x30) {
        check_os_err(err, S2(0x11892), 0);
        tc_SetVol(R16(oldvol), 0);
        RETURN(0);
    }
    err = S16(tc_FSOpen(refnum, vref, pname));
    if (err != 0) {
        check_os_err(err, S2(0x118b8), 0);
        tc_SetVol(R16(oldvol), 0);
        RETURN(0);
    }
    uint32_t rec;
    if (R32(gSavedGame) != 0) {
        W32(count, 0x3766);
        rec = PoPNewPtr(0x3766);
        _BlockMove(R32(gSavedGame), rec, R32(count));
        CALLC(8, InitSavedGameStruct(R32(count), rec));
    } else {
        W32(count, 100);
        rec = CALLC(8, InitSavedGameStruct(100, 0));
        W16(rec + 0x42, R16(gLevel));
        W16(rec + 0x44, R16(gMaxHP));
        W16(rec + 0x52, R16(GS() + 0x219c));
    }
    W16(rec + 0x26, R16(gMinutesLeft));
    W16(rec + 0x28, R16(gTicksLeft));
    W16(rec + 0x54, R16(GH(0x102ff4)));
    W16(rec + 0x56, R16(GH(0x102ff6)));
    W16(rec + 0x5c, R16(GH(0x103058)));
    W16(rec + 0x5e, R16(gLastTimeNIS));
    tc_memcpy(rec, R32(GH(0x10290c)), 4);
    CALLC(6, SetCheckSavedGameCheckSumValues(rec, 0));
    uint32_t want = R32(count);
    err = S16(tc_FSWrite(rec, count, R16(refnum)));
    _DisposePtr(rec);
    if (R32(count) != want) {
        AlertOKOnly(S2(0x118de));       /* "The disk is full. ..." */
    } else if (err != 0) {
        check_os_err(err, S2(0x1190e), 0);
        tc_SetVol(R16(oldvol), 0);
        RETURN(0);
    }
    tc_FSClose(R16(refnum));
    tc_SetVol(R16(oldvol), 0);
    RETURN(1);
}

/* 00011934: compute (check=0) or verify (check=1) the two checksums of a saved game */
uint32_t SetCheckSavedGameCheckSumValues(uint32_t rec, int16_t check) {
    ENTER(0x10, 7);
    uint32_t sum1 = 0, sum2 = 0;
    if (check != 0) {
        sum1 = R32(rec + 0x58);
        sum2 = R32(rec + 0x60);
    }
    W32(rec + 0x58, 0);
    W32(rec + 0x60, 0);
    int32_t size = S32(tc_GetPtrSize(rec));
    uint32_t sum = 0;
    uint16_t i = 0;
    uint32_t p = rec;
    if (size > 0) {
        do {
            sum += R8(p);
            i++;
            p++;
        } while ((int32_t)(uint32_t)i < size);
    }
    W32(rec + 0x58, sum - 0x11);
    uint32_t c2 = sum * 3 - 0x119e5;
    W32(rec + 0x60, c2);
    if (check != 0 && (R32(rec + 0x58) != sum1 || c2 != sum2)) FUN_00011a70();
    RETURN(1);
}

/* 00011a00: checksum tamper penalty: when the timer set by FUN_00011a70 expires,
 * take a life point, restart the penalty timer and restore the kid's state */
uint32_t FUN_00011a00(void) {
    ENTER_NOLINK(1);
    if (R32(GH(0x10292a)) == 0) RETURN(0);
    uint32_t t = _TickCount();
    if (t <= R32(GH(0x10292a))) RETURN(t);
    if (R16(GH(0x102f4a)) >= 0) RETURN(t);
    FUN_000225d2();
    if (S16(DecLifePts(1)) != 0) JumpSeq(0x47);
    int16_t lvl = R16(GS() + GS_LEVEL);
    if (lvl >= 3 && lvl <= 0xd) AddSound(0x1d);
    FUN_00011a70();
    for (int i = 0; i < 15; i++) W32(GH(0x102f2e) + i * 4, R32(GH(0x102faa) + i * 4));
    W16(GH(0x102f2e) + 60, R16(GH(0x102faa) + 60));
    RETURN(0xffff);
}

/* 00011a70: arm the tamper penalty timer (15..45 s from now) */
uint32_t FUN_00011a70(void) {
    ENTER_NOLINK(2);
    uint32_t t = _TickCount() + 0x384;
    uint16_t r = (uint16_t)Rnd(0x708);
    uint32_t v = t + r;
    W32(GH(0x10292a), v);
    RETURN(v);
}

/* 00011aa4: save the rebirth point (level map, objects, kid state) into gSavedGame */
uint32_t SaveRebirthPtStatus(int16_t room) {
    ENTER(0, 0);
    if (R32(gSavedGame) == 0) W32(gSavedGame, CALLC(8, InitSavedGameStruct(0x3766, 0)));
    W16(R32(gSavedGame) + 4, room);
    tc_memcpy(R32(gSavedGame) + 0x66, GS(), 0x780);
    tc_memcpy(R32(gSavedGame) + 0x7e6, GS() + 0x780, 0xf00);
    tc_memcpy(R32(gSavedGame) + 0x16e6, GS() + 0x21a6, 0x1800);
    tc_memcpy(R32(gSavedGame) + 0x2ee6, GS() + 0x39ca, 0x880);
    int16_t lvl = R16(GS() + GS_LEVEL);
    W16(R32(gSavedGame) + 0x42, lvl);
    W16(R32(gSavedGame) + 0x44, R16(GH(0x102f4e)));
    W16(R32(gSavedGame) + 0x52, R16(GH(0x102f30)));
    if (R16(GS() + GS_LEVEL) == 7 && R16(GH(0x103058)) != 0)
        W16(R32(gSavedGame) + 0x5c, 0);
    else
        W16(R32(gSavedGame) + 0x5c, R16(GH(0x103058)));
    W16(R32(gSavedGame) + 0x56, R16(GH(0x102ff6)));
    SetRebirthPtInitials();
    RETURN(CALLC(6, SetCheckSavedGameCheckSumValues(R32(gSavedGame), 0)));
}

/* 00011bce: forget the rebirth point */
uint32_t FUN_00011bce(void) {
    ENTER_NOLINK(0);
    if (R32(gSavedGame) != 0) _DisposePtr(R32(gSavedGame));
    W32(gSavedGame, 0);
    RETURN(0);
}

/* 00011be2: reset the saved tiles of a rebirth point to their initial states
 * (loose floors, gates, ...) */
uint32_t SetRebirthPtInitials(void) {
    ENTER(0x14, 6);
    uint16_t room = 0;
    if (R16(GS() + 0x2180) == 0) RETURN(0);
    do {
        uint32_t off3c = (uint32_t)room * 0x3c;
        uint32_t off78 = (uint32_t)room * 0x78;
        int16_t roomNo = (int16_t)(room + 1);
        uint16_t cell = 0;
        do {
            int32_t c = (int16_t)cell;
            uint16_t kind = RU16(R32(gSavedGame) + off3c + c * 2 + 0x66);
            if (kind == 4) {
                uint16_t v = RU16(R32(gSavedGame) + off78 + c * 4 + 0x7e6);
                uint16_t lo = v & 0xff;
                uint32_t tr = GetTrobRoomCell(roomNo, (int16_t)cell);
                if (tr != 0 && R16(tr + 4) == 3) lo = 0xb8;
                else if (tr != 0) lo = 0;
                v = (uint16_t)((v & 0xff00) | lo);
                W16(R32(gSavedGame) + off78 + c * 4 + 0x7e6, v);
            } else if (kind == 0x11) {
                maddr a = R32(gSavedGame) + off78 + c * 4 + 0x7e6;
                uint16_t v = RU16(a);
                if ((v & 0xff) != 0) W16(a, (v & 0xff00) | 0x4f);
            } else if (kind == 0xb) {
                maddr a = R32(gSavedGame) + off78 + c * 4 + 0x7e6;
                W16(a, RU16(a) & 0xffb0);
            }
            cell++;
        } while (cell < 0x1e);
        room++;
    } while (room < RU16(GS() + 0x2180));
    RETURN(0);
}

/* 00011d52: restore the level from the rebirth point */
uint32_t FUN_00011d52(void) {
    ENTER_NOLINK(0);
    if (R32(gSavedGame) == 0) RETURN(0);
    tc_memcpy(GS(), R32(gSavedGame) + 0x66, 0x780);
    tc_memcpy(GS() + 0x780, R32(gSavedGame) + 0x7e6, 0xf00);
    tc_memcpy(GS() + 0x21a6, R32(gSavedGame) + 0x16e6, 0x1800);
    tc_memcpy(GS() + 0x39ca, R32(gSavedGame) + 0x2ee6, 0x880);
    uint16_t v = RU16(R32(gSavedGame) + 0x5c);
    W16(GH(0x103058), v);
    RETURN(v);
}

/* 00011dea: restore the kid's position/state from the rebirth point; -1 if none */
uint32_t FUN_00011dea(void) {
    if (R32(gSavedGame) == 0) return 0xffff;
    uint32_t sv = R32(gSavedGame);
    maddr a = (uint32_t)((int32_t)R16(sv + 4) * 4) + GS() + 0x39a6 - 4;
    int16_t room = R16(a);
    W16(GH(0x102fc0), room);
    W16(GH(0x10303c), room);
    int16_t pos = R16(a + 2);
    W16(GH(0x102fb6), (int16_t)(pos % 10));
    pos = R16(a + 2);
    int16_t q = (int16_t)(pos / 10);
    W16(GH(0x102fb8), q);
    W16(gMaxHP, R16(sv + 0x44));
    W16(GH(0x102fac), R16(sv + 0x52));
    W16(GH(0x102ff6), R16(sv + 0x56));
    return (q < 0 ? 0xffff0000u : 0) | 2;       /* D0 high word: sign of the quotient */
}

/* 00011e66: the copy protection dialog (pick the symbol shown in the manual);
 * terminates the game after 3 failures */
uint32_t CopyProtectionDialog(void) {
    ENTER(0xc, 8);
    maddr tries = f, got = f + 2, entry = f + 4, data = f + 8;
    int16_t ok = 1;
    if (R16(gCopyProtOK) == 0 && R16(GH(0x103002)) == 0) {
        W16(gCopyProtOK, 1);
        InitDialog();
        FUN_00013d98();
        DrawDialogFrame();
        _ShowCursor();
        W16(tries, 0);
        int16_t last = -1;
        ok = 0;
        uint32_t h = PoPGetResource(0x434f5059, 0x1f45);   /* 'COPY' */
        _HLock(h);
        W32(data, R32(h));
        /* The loop re-reads the count through A0, which on later iterations still
         * points at the timer table (A5-0x1CEC) left by GetTimer in
         * GetCopyProtectionSymbol -> WaitTimerNoPoll: an original quirk, kept. */
        maddr countp = R32(data);
        for (;;) {
            int16_t n = R16(countp);
            int16_t i = S16(Rnd((int16_t)(n - 1)));
            if (i == last) {
                int32_t t = (int32_t)i + (uint32_t)(uint16_t)Rnd((int16_t)(n - 2)) + 1;
                i = (int16_t)(t % (int32_t)n);
            }
            last = i;
            int16_t k = (int16_t)(i * 2 + 1);
            _BlockMove(R32(data) + (int32_t)k * 2, entry, 4);
            SaveCurrentWorld(gOffWorld);
            if (R16(tries) != 0) CALLC(4, DrawSymbolBox(last, 0));
            CALLC(4, DrawCopyProtectionDialog(R16(entry), R16(tries)));
            RestoreOriginalWorld(gOffWorld);
            if (R16(tries) == 0) DialogFadeIn();
            else DrawSOffWorldToWindow(gOffWorld, R32(gMainWindow), gGameRect, gGameRect, 0);
            int16_t s = S16(GetCopyProtectionSymbol());
            W16(got, s);
            ok = (s == R16(entry + 2) || s == -2) ? 1 : 0;
            if (ok) break;
            W16(tries, R16(tries) + 1);
            if (R16(tries) == 3) break;
            countp = GH(0x106314);
        }
        _HUnlock(h);
        _HPurge(h);
        DialogFadeOut();
        CALLC(2, DialogResetToGamePlay(0));
    }
    if (ok == 0) RETURN(Terminate(1, S2(0x12014)));    /* "Copy protection failure. ..." */
    CALLC(2, SetMenusForGamePlayMode(1));
    _EnableItem(R32(gGameMenu), 5);
    if (R16(GH(0x102cda)) == 0) _EnableItem(R32(gGameMenu), 7);
    RETURN(0);
}

/* 00012040: top-left of symbol i in the 5x2 symbol grid */
uint32_t ComputeSymbolPt(uint32_t pt, int16_t i) {
    W16(pt + 2, (int16_t)((i % 5) * 0x4c + 0x50));
    W16(pt, (int16_t)((i / 5) * 0x4c + 0x83));
    return pt;
}

/* 0001209e: frame rect of symbol i */
uint32_t ComputeSymbolRect(uint32_t r, int16_t i) {
    ComputeSymbolPt(r, i);
    W16(r, R16(r) - 5);
    W16(r + 4, R16(r) + 0x3c);
    W16(r + 2, R16(r + 2) - 5);
    W16(r + 6, R16(r + 2) + 0x3c);
    return r;
}

/* 000120ee: draw the copy protection question (and, first time, the symbols) */
uint32_t DrawCopyProtectionDialog(int16_t entry, int16_t again) {
    ENTER(0x78, 5);
    maddr shp = f + 8, pt = f + 0xc, style = f + 0x10, text = f + 0x14;
    if (again != 0) PoPFillRect(GH(0x102920), 0x19);
    uint32_t h = PoPGetResource(0x54455854, 0x1f45);       /* 'TEXT' */
    uint32_t fmt = StrItem(h, 0x0100);                      /* item 1 (a byte in the high half) */
    sprintf1(text, fmt, (uint32_t)(int32_t)entry);
    _HPurge(h);
    W16(style + 2, 1);
    W16(style, 1);
    CALLC(16, DialogDrawStr(text, 0, GH(0x102920), style));
    if (again == 0) {
        for (int16_t i = 0; i < 10; i++) {
            CALLC(6, ComputeSymbolPt(pt, i));
            uint32_t sh = GetShape((int16_t)(i + 0x1f45));
            if (sh != 0) {
                uint32_t p = R32(sh);
                W32(shp, p);
                DrawIt(p, 0, R16(pt + 2), R16(pt), 0x10, 0, 0);
                _HPurge(sh);
            }
        }
    }
    RETURN(0);
}

/* 000121e6: frame symbol i (hilite = selected color) */
uint32_t DrawSymbolBox(int16_t i, int16_t hilite) {
    ENTER(8, 0);
    if (i == -1) RETURN(0);
    FUN_0004fbe2();
    if (hilite != 0) SetForeColor(0x1b);
    else SetForeColor(0x19);
    CALLC(6, ComputeSymbolRect(f, i));
    _FrameRect(f);
    RETURN(seg17_04fc46());
}

/* 00012240: track the mouse until a symbol is clicked; returns its index */
uint32_t GetCopyProtectionSymbol(void) {
    ENTER(4, 4);
    maddr pt = f;
    int16_t cur = -1, shown = -1;
    W16(GH(0x102ffe), 0);
    int16_t saved = R16(gInNIS);
    W16(gInNIS, 0);
    do {
        HandleEvent();
        if (R16(gEvent) == 1) {
            W32(pt, R32(gEvtWhere));
            _GlobalToLocal(pt);
            do {
                cur = S16(CALLC(4, PtInSymbolRect(pt)));
                if (cur != shown) {
                    CALLC(4, DrawSymbolBox(shown, 0));
                    CALLC(4, DrawSymbolBox(cur, 1));
                    shown = cur;
                }
                _GetMouse(pt);
            } while (_StillDown() & 0xff);
            W16(gEvent, 0);
        }
        if (R16(gQuit) != 0) Terminate(0, 0);
    } while (cur == -1);
    SetTimer(1, 0, 0x1e);
    WaitTimerNoPoll(1);
    W16(gInNIS, saved);
    RETURN((uint16_t)cur);
}

/* 00012306: index of the symbol containing *pt, or -1 */
uint32_t PtInSymbolRect(uint32_t pt) {
    ENTER(8, 2);
    int16_t r = -1;
    int16_t i = 0;
    do {
        CALLC(6, ComputeSymbolRect(f, i));
        if (_PtInRect(R32(pt), f) & 0xff) r = i;
        i++;
    } while (i < 10 && r == -1);
    RETURN((uint16_t)r);
}

/* 0001235c: Hall of Fame entry dialog after finishing the game with `score` */
uint32_t HallOfFameDialog(int16_t score) {
    ENTER(0xc, 1);
    maddr idx = f, rect = f + 4;
    uint32_t hof = CALLC(6, InitHallOfFame(score, idx));
    if (hof == 0) RETURN(0);
    InitDialog();
    FUN_00013d98();
    _EnableItem(R32(gFileMenu), 1);
    _EnableItem(R32(gFileMenu), 4);
    _DisableItem(R32(gGameMenu), 1);
    CALLC(4, DrawHallOfFameDialog(hof));
    DialogFadeIn();
    if (S16(CALLC(6, GetHallOfFameName(hof, R16(idx)))) != 0) {
        SaveCurrentWorld(gOffWorld);
        CALLC(6, ComputeHallPlayerRect(rect, R16(idx)));
        _InsetRect(rect, -3, 0);
        PoPFillRect(rect, 0x19);
        _InsetRect(rect, 3, 0);
        CALLC(16, DialogDrawStr((uint32_t)((int32_t)R16(idx) * 0x1e) + hof + 2, 0, rect, 0));
        _InsetRect(rect, -3, 0);
        RestoreOriginalWorld(gOffWorld);
        DrawSOffWorldToWindow(gOffWorld, R32(gMainWindow), rect, rect, 0);
        CALLC(4, WriteHallOfFameFile(hof));
        Wait(0, 0x384);
    }
    DialogFadeOut();
    CALLC(2, DialogResetToGamePlay(0));
    CALLC(2, SetMenusForGamePlayMode(1));
    _EnableItem(R32(gGameMenu), 5);
    _EnableItem(R32(gGameMenu), 1);
    if (R16(GH(0x102cda)) == 0) _EnableItem(R32(gGameMenu), 7);
    if (hof != 0) _DisposePtr(hof);
    PrinceResetSnd(0xb4);
    RETURN(0);
}

/* 000124d6: rect of Hall of Fame line i */
uint32_t ComputeHallPlayerRect(uint32_t r, int16_t i) {
    W16(r + 2, 0x2a);
    W16(r + 6, 0x133);
    int16_t top = (int16_t)(i * 0x2f + 0x6e);
    W16(r, top);
    W16(r + 4, (int16_t)(top + 0x20));
    return r;
}

/* 00012524: draw the Hall of Fame table (count word + 5 entries of 0x1e bytes:
 * name at +2, score at +0x1e) */
uint32_t DrawHallOfFameDialog(uint32_t hof) {
    ENTER(0xc, 3);
    maddr rect = f, style = f + 8;
    DrawDialogFrame();
    SaveCurrentWorld(gOffWorld);
    W16(style + 2, 1);
    W16(style, 1);
    CALLC(14, DialogDrawStrId(0x1f49, 0, GH(0x10293e), style));
    CALLC(6, DialogDrawBox(GH(0x102936), 0));
    W16(style + 2, 0);
    CALLC(14, DialogDrawStrId(0x1f4a, GH(0x10292e), 0, style));
    CALLC(14, DialogDrawStrId(0x1f4b, GH(0x102932), 0, style));
    int16_t i = 0;
    if (R16(hof) > 0) {
        do {
            CALLC(6, ComputeHallPlayerRect(rect, i));
            W16(style + 2, 0);
            uint32_t e = (uint32_t)((int32_t)i * 0x1e) + hof;
            CALLC(16, DialogDrawStr(e + 2, 0, rect, style));
            W16(style + 2, 0xff);
            W16(rect + 2, 0x172);
            W16(rect + 6, 0x1a4);
            CALLC(14, DialogDrawNumber(R16(e + 0x1e), 0, rect, style));
            i++;
        } while (i < R16(hof));
    }
    RestoreOriginalWorld(gOffWorld);
    RETURN(0);
}

/* 00012650: let the player type a name into Hall of Fame line idx (TextEdit) */
uint32_t GetHallOfFameName(uint32_t hof, int16_t idx) {
    ENTER(0x220, 8);
    maddr rect = f, upd = f + 8, pt = f + 0x10, name = f + 0x14, maxw = f + 0x114,
          len = f + 0x116, msg = f + 0x118, w1 = f + 0x218, w2 = f + 0x21c;
    _SetEntryUsage(R32(GH(0x102cc2)), 0x1b, 2, 0);
    _SetEntryUsage(R32(GH(0x102cc2)), 0x10, 2, 0);
    _Palette2CTab(R32(GH(0x102cc2)), R32(GH(0x102cbe)));
    _ActivatePalette(R32(gMainWindow));
    int16_t done = 0;
    CALLC(6, ComputeHallPlayerRect(rect, idx));
    _InsetRect(rect, -2, 0);
    SaveCurrentWorld(gOffWorld);
    CALLC(6, DialogDrawBox(rect, 1));
    RestoreOriginalWorld(gOffWorld);
    DrawSOffWorldToWindow(gOffWorld, R32(gMainWindow), rect, rect, 0);
    _InsetRect(rect, 2, 2);
    W16(maxw, (int16_t)(R16(rect + 6) - R16(rect + 2) - 5));
    _TextFont(0xd5);
    _TextSize(0x14);
    _TextFace(0);
    FUN_0004fbe2();
    seg17_04fc04();
    SetForeColor(0x1b);
    SetBackColor(0x10);
    W16(rect, R16(rect) + 4);
    uint32_t te = _TENew(rect, rect);
    _TESetText(msg, 0, te);
    _TECalText(te);
    _TEActivate(te);
    do {
        _TEIdle(te);
        HandleEvent();
        int16_t what = R16(gEvent);
        if (what == 3 || what == 5) {
            uint32_t tep = R32(te);
            _GetDialogItemText(R32(tep + 0x3e), name);
            tc_PtoCstr(name);
            uint16_t ch = RU16(gEvtMessage + 2) & 0xff;
            if (ch == 0xd || ch == 3) {
                sprintf1(msg, S2(0x129fc), name);   /* "Do you want \"%s\" to be the name ..." */
                if (S16(AlertYesNo(msg)) != 0) done = 1;
            } else if (RU16(gEvtModifiers) & 0x100) {
                if (ch == 0x63) {
                    _TECopy(te);
                } else if (ch == 0x78) {
                    _TECut(te);
                } else if (ch == 0x76) {
                    int16_t sel = R16(tep + 0x3c);
                    W16(w1, sel);
                    W16(len, (int16_t)(sel + 1 + R16(LM_TEScrpLength)));
                    int16_t w = S16(_TextWidth(name, 0, R16(w1)));
                    w = (int16_t)(w + S16(_TextWidth(R32(R32(LM_TEScrpHandle)), 0, R16(LM_TEScrpLength))));
                    if (R16(len) < 0x19 && w < R16(maxw)) _TEPaste(te);
                }
            } else if (ch != 0 && ch != 0x3e && ch != 0x3c) {
                int16_t sel = R16(tep + 0x3c);
                W16(w2, sel);
                W16(len, (int16_t)(sel + 2));
                int16_t w = S16(_TextWidth(name, 0, R16(w2)));
                w = (int16_t)(w + S16(_CharWidth((int16_t)ch)));
                if ((R16(len) < 0x19 && w < R16(maxw)) ||
                    ch == 8 || ch == 0x1c || ch == 0x1d || ch == 0x1e || ch == 0x1f)
                    _TEKey((int16_t)ch, te);
            }
        } else if (R16(gEvent) == 1) {
            W32(pt, R32(gEvtWhere));
            _GlobalToLocal(pt);
            _TEClick(R32(pt), 0, te);
        } else if (R16(gEvent) == 6) {
            uint32_t win = R32(gEvtMessage);
            if (win == R32(gMainWindow)) {
                maddr src = R32(R32(win + 0x18)) + 2;
                W32(upd, R32(src));
                W32(upd + 4, R32(src + 4));
                if (call_pascal_bool3(R32(pSectRect), gGameRect, upd, upd)) _TEUpdate(upd, te);
            }
        }
        if (R16(gQuit) != 0) done = 1;
    } while (done == 0);
    uint32_t tep = R32(te);
    _GetDialogItemText(R32(tep + 0x3e), name);
    tc_PtoCstr(name);
    tc_strcpy((uint32_t)((int32_t)idx * 0x1e) + hof + 2, name);
    int16_t r;
    if (R16(gQuit) == 0 && tc_strlen(name) != 0) r = 1;
    else r = 0;
    SetForeColor(0);
    SetBackColor(0xff);
    _TEDeactivate(te);
    _TEDispose(te);
    _SetEntryUsage(R32(GH(0x102cc2)), 0x1b, 0xc, 0);
    _SetEntryUsage(R32(GH(0x102cc2)), 0x10, 0xc, 0);
    _Palette2CTab(R32(GH(0x102cc2)), R32(GH(0x102cbe)));
    _ActivatePalette(R32(gMainWindow));
    RETURN((uint32_t)r);
}

/* 00012a3c: insert a new score into the Hall of Fame table (grown by one entry);
 * returns the (new) table, *outIdx = line of the new entry.  The original stores the
 * new pointer into its `hof` argument slot: glue_HOFAddPlayer does that. */
uint32_t HOFAddPlayer(uint32_t hof, int16_t score, uint32_t outIdx) {
    ENTER(0, 6);
    if (hof != 0) {
        uint32_t size = tc_GetPtrSize(hof);
        int32_t n = (int16_t)((int16_t)size + 0x1e);
        uint32_t old = hof;
        hof = PoPNewPtr((uint32_t)n);
        _BlockMove(old, hof, (uint32_t)(n - 0x1e));
        _DisposePtr(old);
    } else {
        hof = PoPNewPtr(0x3e);
        W16(hof, 0);
    }
    int16_t i = 0;
    if (i < R16(hof)) {
        do {
            if (score >= R16((uint32_t)((int32_t)i * 0x1e) + hof + 0x1e)) break;
            i++;
        } while (i < R16(hof));
    }
    int16_t cnt = R16(hof);
    if (i < cnt && i != 4) {
        int16_t bytes = (int16_t)((cnt - i) * 0x1e);
        uint32_t e = (uint32_t)((int32_t)i * 0x1e) + hof;
        _BlockMove(e + 2, e + 0x20, (uint32_t)(int32_t)bytes);
    }
    if (R16(hof) < 5) W16(hof, R16(hof) + 1);
    else if (i == 5) i = 4;
    uint32_t e = (uint32_t)((int32_t)i * 0x1e) + hof;
    W8(e + 2, 0);
    W16(e + 0x1e, score);
    W16(outIdx, i);
    RETURN(hof);
}
void glue_HOFAddPlayer(void) {
    maddr sp = emu_sp();
    uint32_t r = HOFAddPlayer(rd32(sp), (int16_t)rd16(sp + 4), rd32(sp + 6));
    wr32(sp, r);                        /* the original updates its `hof` argument */
    emu_set_d0(r);
}

/* 00012b8c: read the Hall of Fame file (and add `score` if > 0) */
uint32_t InitHallOfFame(int16_t score, uint32_t outIdx) {
    ENTER(8, 3);
    maddr eof = f, vref = f + 4, refnum = f + 6;
    uint32_t hof = 0;
    int16_t err = S16(tc_GetVol(vref, 0));
    if (err != 0) {
        check_os_err(err, S2(0x12c98), 0);
        RETURN(0);
    }
    if (S16(tc_FSOpen(refnum, R16(vref), GH(0x102946))) == 0) {
        err = S16(tc_GetEOF(eof, R16(refnum)));
        if (err != 0) {
            check_os_err(err, S2(0x12cb6), 0);
            RETURN(0);
        }
        hof = PoPNewPtr(R32(eof));
        err = S16(tc_FSRead(hof, eof, R16(refnum)));
        if (err == 0) {
            int16_t n = R16(hof);
            if (n < 0 || n > 5) {
                _DisposePtr(hof);
                hof = 0;
            }
        } else {
            check_os_err(err, S2(0x12cd4), 0);
            _DisposePtr(hof);
            hof = 0;
        }
        tc_FSClose(R16(refnum));
    }
    if (score > 0) {
        hof = CALLC(10, HOFAddPlayer(hof, score, outIdx));
    } else if (hof == 0) {
        hof = PoPNewPtr(0x3e);
        W16(hof, 0);
    }
    RETURN(hof);
}

/* 00012cf2: show the Hall of Fame (fromMenu != 0: from the menu, wait for a key) */
uint32_t ShowHallOfFame(int16_t fromMenu) {
    ENTER(4, 3);
    int16_t r = 0;
    uint32_t hof = CALLC(6, InitHallOfFame(0, f));
    if (hof != 0 && (R16(hof) | fromMenu) != 0) {
        InitDialog();
        PlaySound(-1, 0x26cc, 0);
        FUN_00013d98();
        _DisableItem(R32(gGameMenu), 1);
        _HiliteMenu(0);
        CALLC(4, DrawHallOfFameDialog(hof));
        r = S16(DialogFadeIn());
        if (r == 0) {
            if (fromMenu != 0) r = S16(FUN_0005165e());
            else r = S16(Wait(0, 0x384));
        }
        if (r == 0 || fromMenu != 0) r = S16(DialogFadeOut());
        else PoPFillRect(gGameRect, 0xff);
        CALLC(2, DialogResetToGamePlay(fromMenu));
        CALLC(2, SetMenusForGamePlayMode(1));
        _EnableItem(R32(gGameMenu), 5);
        _EnableItem(R32(gGameMenu), 1);
        if (R16(GH(0x102cda)) == 0) _EnableItem(R32(gGameMenu), 7);
        PrinceResetSnd(0xb4);
    }
    if (hof != 0) _DisposePtr(hof);
    if (R16(gPauseMark) == 1) {
        W16(gPauseMark, 0);
        CALLC(4, SetGameMenuItemMark(1, 0));
    }
    RETURN((uint16_t)r);
}

/* 00012e0e: write the Hall of Fame file; returns 1 on success */
uint32_t WriteHallOfFameFile(uint32_t hof) {
    ENTER(8, 4);
    maddr refnum = f, vref = f + 2, count = f + 4;
    int16_t r = 0;
    int16_t err = S16(tc_GetVol(vref, 0));
    if (err == 0) {
        err = S16(tc_Create(R32(GH(0x102914)), 0x50524e63, R16(vref), GH(0x102946)));  /* 'PRNc' */
        if (err == 0 || err == -0x30) {
            err = S16(tc_FSOpen(refnum, R16(vref), GH(0x102946)));
            if (err == 0) {
                uint32_t n = (uint32_t)((int32_t)R16(hof) * 0x1e) + 0x20;
                W32(count, n);
                err = S16(tc_FSWrite(hof, count, R16(refnum)));
                if (n != R32(count)) AlertOKOnly(S2(0x12f00));   /* "The disk is full. ..." */
                else if (err != 0) check_os_err(err, S2(0x12f38), 0);
                else r = 1;
                tc_FSClose(R16(refnum));
            } else {
                check_os_err(err, S2(0x12f6c), 0);
            }
        }
    }
    RETURN((uint16_t)r);
}

/* 00012fa0: the credits (4 pages); returns non-zero if interrupted */
uint32_t DoCreditsDialog(void) {
    ENTER(0x10, 5);
    maddr rect = f, blink = f + 8;
    InitDialog();
    InitDissolve(1);
    PlaySound(-1, 0x2723, 0);
    DrawDialogFrame();
    W32(rect, R32(gGameRect));
    W32(rect + 4, R32(gGameRect + 4));
    W16(rect + 6, R16(rect + 6) - 2);
    _InsetRect(rect, 0xe, 0xe);
    int16_t r = 0;
    int16_t page = 0;
    do {
        SaveCurrentWorld(gOffWorld);
        PoPFillRect(rect, 0x19);
        CALLC(4, DrawCreditsPage(R16(GH(0x102a46) + page * 2), R16(GH(0x102a4e) + page * 2)));
        RestoreOriginalWorld(gOffWorld);
        if (page != 0) {
            /* the original pushes a long 0x5a that DoDissolve does not use */
            maddr s = emu_sp();
            push32(0x5a);
            r = S16(DoDissolve());
            emu_set_sp(s);
        } else {
            r = S16(DialogFadeIn());
        }
        if (r == 0) {
            if (CALLC(6, GetRectToBlink(blink, page)) == 0) {
                r = S16(Wait(0, 0x1d1));
            } else {
                SetTimer(0, 0, 0x1d1);
                uint32_t save = SaveRect(blink, 0);
                if (r == 0) {
                    while (S16(GetTimer(0)) != 0) {
                        RestoreRectKeep(save);
                        r = S16(Wait(1, 0x14));
                        if (r == 0) {
                            PoPFillRect(blink, 0x19);
                            r = S16(Wait(1, 0x14));
                        }
                        if (r != 0) break;
                    }
                }
                DeallocMapSave(save);
            }
        }
        page++;
    } while (page < 4 && r == 0);
    SetTimer(2, 0, 0x14);
    DialogFadeOut();
    CALLC(2, DialogResetToGamePlay(0));
    WaitTimerNoPoll(2);
    ResetSoundItem(0);
    PurgeSounds();
    seg5_029374();
    FUN_00046d00();
    RETURN((uint16_t)r);
}

/* 00013150: draw credit categories first..last */
uint32_t DrawCreditsPage(int16_t first, int16_t last) {
    ENTER(0, 1);
    _MoveTo(0, 0x45);
    if (first == R16(GH(0x102a48))) _Move(0, 4);
    int16_t i = first;
    if (first <= last) {
        do {
            CALLC(2, DrawCreditCategory(i));
            _Move(0, 0xd);
            i++;
        } while (i <= last);
    }
    RETURN(0);
}

/* 000131b0: draw one credits category ('TEXT' resource: title, then names) at the pen */
uint32_t DrawCreditCategory(int16_t id) {
    ENTER(0x24, 4);
    maddr item = f, style = f + 4, rect = f + 8, pen = f + 0x10;
    uint32_t h = PoPGetResource(0x54455854, id);   /* 'TEXT' */
    if (h == 0) RETURN(0);
    _GetPenState(pen);
    W16(rect + 4, R16(pen));
    W16(rect + 2, 0x1c);
    W16(rect, (int16_t)(R16(rect + 4) - 0x23));
    W16(rect + 6, (int16_t)(R16(gGameRect + 6) - 0x23));
    _HLock(h);
    uint32_t s = StrItem(h, 0x0100);
    uint32_t p = R32(h);
    if (R8(p) == 1) {
        W16(style + 2, 1);
        if (id == R16(GH(0x102a4c)) || id == R16(GH(0x102a4a)) || (id >= 0x3fc && id <= 0x400)) {
            W32(rect, R32(gGameRect));
            W32(rect + 4, R32(gGameRect + 4));
            W16(rect + 6, R16(rect + 6) - 2);
            _InsetRect(rect, 0x1a, 0x1a);
        }
    } else {
        W16(style + 2, 0);
    }
    W16(style, 1);
    CALLC(16, DialogDrawStr(s, 0, rect, style));
    if (R8(p) > 1) {
        W16(style + 2, 0xff);
        W16(item, 2);
        if (2 <= R8(p)) {
            do {
                s = StrItem(h, (int16_t)(R8(item + 1) << 8));
                CALLC(16, DialogDrawStr(s, 0, rect, style));
                _OffsetRect(rect, 0, 0x23);
                _Move(0, 0x23);
                W16(item, R16(item) + 1);
            } while (RU16(item) <= R8(p));
        }
        _Move(0, 2);
    } else {
        _Move(0, 0x25);
    }
    _HUnlock(h);
    RETURN(_HPurge(h));
}

/* 00013312: rect to blink on credits page `page` for the key currently held
 * (keyboard help page), or 0 */
uint32_t GetRectToBlink(uint32_t r, int16_t page) {
    ENTER(0x10, 4);
    maddr keys = f;
    uint32_t res = 0;
    if (R16(GH(0x1057dc)) == 0) RETURN(0);
    call_ptr(R32(pGetKeys), "l", keys);
    if ((R32(keys + 4) & 4) != 0) {
        int16_t line = -1;
        switch (R32(keys)) {
        case 0x20:   if (page == 0) line = 0; break;
        case 0x400:  if (page == 0) line = 1; break;
        case 0x800:  if (page == 0) line = 2; break;
        case 0x1000: if (page == 0) line = 3; break;
        case 0x2000: if (page == 0) line = 4; break;
        case 0x8000: if (page == 0) line = 5; break;
        case 0x4000: if (page == 0) line = 6; break;
        case 4:      if (page == 0) line = 7; break;
        case 0x10:
            if (page == 1) {
                W32(r, R32(GH(0x102918)));
                W32(r + 4, R32(GH(0x10291c)));
                res = r;
            }
            break;
        }
        if (line != -1) {
            res = r;
            int16_t top = (int16_t)(line * 0x28 + 0x2b);
            W16(r, top);
            W16(r + 2, 0x19);
            W16(r + 4, (int16_t)(R16(r) + 0x19));
            W16(r + 6, 0x1e5);
        }
    }
    RETURN(res);
}

/* 00013438: fetch and dispatch the pending events; returns non-zero if something
 * happened that interrupts the game (menu command, suspend, ...) */
uint32_t HandleEvent(void) {
    ENTER(0x9c, 7);
    maddr where = f, upd = f + 4, osMsg = f + 0x14, repost = f + 0x18, sndState = f + 0x1a,
          key = f + 0x1c, text = f + 0x20, qel = f + 0x84, last = f + 0x88, port = f + 0x98;
    int16_t res = 0;
    if (R16(gInNIS) != -1) W16(GH(0x102ffe), 0);
    if (R16(GH(0x102ffe)) != 0) {
        uint32_t t = _TickCount();
        if (t <= R32(GH(0x102c5a)) + 0x3c) {
            W16(gEvent, 0);
            RETURN(0);
        }
    }
    W32(GH(0x102c5a), _TickCount());
    W16(last, 0);
    int16_t mask = R16(GH(0x102c50));
    {   /* FlushEvents(everyEvent, stopMask = event mask): the original builds D0 on the stack */
        maddr s = emu_sp();
        push16(0xffff);
        push16(RU16(GH(0x102c50)));
        uint32_t d0 = R32(emu_sp());
        emu_set_sp(s);
        tb_begin();
        cpu_set(R_D0, d0);
        tb_trap(0xA032);
        tb_end();
    }
    do {
        W16(repost, 0);
        if (R16(GH(0x102cf8)) != 0) {
            _WaitNextEvent(mask, gEvent, 0, 0);
        } else {
            _SystemTask();
            _GetNextEvent(mask, gEvent);
        }
        switch (R16(gEvent)) {             /* jump table at 0x134d4 */
        case 1:                                     /* mouseDown */
            if (_FrontWindow() == R32(gMainWindow)) {
                if (R16(gInNIS) == 1) {
                    W16(repost, 1);
                    mask &= ~2;
                } else {
                    res |= S16(HandleMouseDown());
                }
            } else if (_FrontWindow() == R32(GH(0x10646a))) {
                maddr s = emu_sp();
                push32(gEvent);                     /* pushed but unused by the callee */
                HandleMouseDownInPackingWindow();
                emu_set_sp(s);
            }
            break;
        case 3: case 5:                             /* keyDown, autoKey */
            if (_FrontWindow() == R32(gMainWindow)) {
                W8(key, R8(gEvtMessage + 3));
                if (RU16(gEvtModifiers) & 0x100) {
                    W16(sndState, R16(gSoundOn));
                    PauseAudioSystem();
                    FUN_00013908();
                    uint32_t mc = _MenuKey((int16_t)R8(key));
                    res |= S16(CALLC(4, HandleMenuChoice(mc)));
                    ResumeAudioSystem();
                    CALLC(2, CheckSoundChanges(R16(sndState)));
                }
            } else if (_FrontWindow() == R32(GH(0x10646a))) {
                HandleKeyDownInPackingWindow(gEvent);
            }
            break;
        case 6: {                                   /* updateEvt */
            _GetPort(port);
            _SetPort(R32(gMainWindow));
            _BeginUpdate(R32(gEvtMessage));
            if (R16(GH(0x102cac)) == 0) RecheckSystem(0);
            uint32_t win = R32(gEvtMessage);
            maddr src = R32(R32(win + 0x18)) + 2;
            W32(upd, R32(src));
            W32(upd + 4, R32(src + 4));
            if (R16(GH(0x102caa)) != 0 && R16(GH(0x102cac)) != 0) {
                if (win == R32(gMainWindow)) {
                    if (call_pascal_bool3(R32(pSectRect), upd, gGameRect, upd)) {
                        if (R16(GH(0x103058)) != 0) {
                            int16_t top = R16(upd);
                            W16(upd, (int16_t)(0x16d - R16(upd + 4)));
                            W16(upd + 4, (int16_t)(0x16d - top));
                        }
                        uint32_t rgn = _NewRgn();
                        _GetClip(rgn);
                        _ClipRect(gGameRect);
                        PrinceCopy(upd);
                        _SetClip(rgn);
                        _DisposeRgn(rgn);
                    }
                } else if (R32(gEvtMessage) == R32(GH(0x10646a))) {
                    HandleUpdateEvtInPackingWindow();
                }
            }
            _EndUpdate(R32(gEvtMessage));
            _SetPort(R32(port));
            break;
        }
        case 7:                                     /* diskEvt */
            if (R32(gEvtMessage) & 0xffff0000u) {
                SwitchOut();
                W16(where + 2, 100);
                W16(where, 100);
                int16_t err = S16(tc_DIBadMount(R32(gEvtMessage), R32(where)));
                if (err < 0) {
                    sprintf1(text, S2(0x137f8), (uint32_t)(int32_t)err);   /* "System error %d. ..." */
                    AlertOKOnly(text);
                }
                SwitchIn();
            }
            break;
        case 8: {                                   /* activateEvt */
            uint32_t win = R32(gEvtMessage);
            uint32_t w = R32(gMainWindow);
            if (win == w) {
                _SetPort(w);
                ReactivatePalette(R32(gMainWindow));
            } else {
                w = R32(GH(0x10646a));
                if (win == w) _SetPort(w);
            }
            break;
        }
        case 15:                                    /* osEvt */
            if (_FrontWindow() == R32(gMainWindow)) {
                uint32_t m = R32(gEvtMessage) & 0xff000000u;
                W32(osMsg, m);
                if (m & 0x01000000u) {              /* suspend/resume */
                    uint8_t resume = R8(gEvtMessage + 3) & 1;
                    W8(key, resume);
                    W16(GH(0x102cac), resume);
                    if (resume == 0) {
                        SwitchOut();
                        res |= 1;
                    } else {
                        SwitchIn();
                    }
                }
            }
            break;
        default:
            break;
        }
        if (R16(repost) != 0) {
            /* PPostEvent(what, message) and copy the rest of the event into the queue element */
            tb_begin();
            cpu_set(R_A0, (uint32_t)(int32_t)R16(gEvent));
            cpu_set(R_D0, R32(gEvtMessage));
            tb_trap(0xA12F);
            uint32_t err = cpu_get(R_D0);
            uint32_t q = cpu_get(R_A0);
            tb_end();
            W32(qel, q);
            if ((uint16_t)err == 0) {
                W16(R32(qel) + 6, R16(gEvent));
                W32(R32(qel) + 8, R32(gEvtMessage));
                W32(R32(qel) + 0xc, R32(gEvtWhen));
                W32(R32(qel) + 0x10, R32(gEvtWhere));
                W16(R32(qel) + 0x14, R16(gEvtModifiers));
                res = 1;
                W32(GH(0x102c5a), 0);
            }
        }
        if (R16(gEvent) != 0) {
            for (int i = 0; i < 16; i += 4) W32(last + i, R32(gEvent + i));
        }
    } while (R16(gEvent) != 0);
    if (res != 0 && R16(GH(0x103002)) != 0) W16(GH(0x103004), 1);
    if (R16(last) != 0) {
        for (int i = 0; i < 16; i += 4) W32(gEvent + i, R32(last + i));
    }
    RETURN((uint16_t)res);
}

/* 00013820: mouse down in the game window */
uint32_t HandleMouseDown(void) {
    ENTER(0xc, 4);
    maddr win = f, bounds = f + 4;
    if (_Button() & 0xff) e2MAC_ShowMenuBar();
    int16_t res = 0;
    int16_t part = S16(_FindWindow(R32(gEvtWhere), win));
    switch (part) {
    case 1: {                                       /* inMenuBar */
        int16_t snd = R16(gSoundOn);
        PauseAudioSystem();
        FUN_00013908();
        uint32_t mc = _MenuSelect(R32(gEvtWhere));
        res = S16(CALLC(4, HandleMenuChoice(mc)));
        ResumeAudioSystem();
        CALLC(2, CheckSoundChanges(snd));
        break;
    }
    case 2:                                         /* inSysWindow */
        PauseAudioSystem();
        _SystemClick(gEvent, R32(win));
        ResumeAudioSystem();
        break;
    case 4:                                         /* inDrag */
        PauseAudioSystem();
        _SetRect(bounds, -1000, -1000, 1000, 1000);
        _DragWindow(R32(win), R32(gEvtWhere), bounds);
        ResumeAudioSystem();
        SetWindowPos();
        break;
    case 3:                                         /* inContent */
        _SelectWindow(R32(win));
        break;
    }
    if ((_Button() & 0xff) == 0) e2MAC_HideMenuBar();
    RETURN((uint16_t)res);
}

/* 00013908: enable the Edit menu items only when a desk accessory is in front */
uint32_t FUN_00013908(void) {
    ENTER_NOLINK(1);
    uint32_t w = _FrontWindow();
    if (S16(CALLC(4, IsDAWindow(w))) != 0) {
        for (int16_t i = 1; i <= 6; i++) _EnableItem(R32(gEditMenu), i);
    } else {
        for (int16_t i = 1; i <= 6; i++) _DisableItem(R32(gEditMenu), i);
    }
    RETURN(0);
}

/* 00013952 */
uint32_t IsDAWindow(uint32_t w) {
    if (w == 0) return 0;
    return R16(w + 0x6c) < 0 ? 1 : 0;
}

/* 00013984: dispatch a menu selection (menu id << 16 | item) */
uint32_t HandleMenuChoice(uint32_t choice) {
    ENTER(0, 3);
    int16_t res = 0;
    if (choice == 0) RETURN(0);
    int16_t menu = (int16_t)(choice >> 16);
    int16_t item = (int16_t)choice;
    switch (menu) {
    case 0x190: CALLC(2, HandleAppleChoice(item)); break;
    case 0x191: res = S16(CALLC(2, HandleFileChoice(item))); break;
    case 0x192: CALLC(2, HandleEditChoice(item)); res = R16(gQuit); break;
    case 0x193: CALLC(2, HandleGameMenuChoice(item)); res = R16(gQuit); break;
    }
    _HiliteMenu(0);
    RETURN((uint16_t)res);
}

/* 00013a22: About box or desk accessory */
uint32_t HandleAppleChoice(int16_t item) {
    ENTER(0x104, 0);
    if (item == 1) {
        DrawAbout(0x258);
        RETURN(0);
    }
    _GetMenuItemText(R32(gAppleMenu), item, f);
    _OpenDeskAcc(f);
    RETURN(0);
}

/* 00013a72: File menu */
uint32_t HandleFileChoice(int16_t item) {
    ENTER(0, 3);
    int16_t res = 0;
    switch (item) {                       /* jump table at 0x13a9a */
    case 1:                                         /* New game */
        if (S16(FUN_00028ff6()) == 0) {
            W16(gNISLevel, 1);
            W16(gLevelLoaded, 1);
            W16(gLastTimeNIS, -1);
            W16(GH(0x103054), 1);
            W16(gMinutesLeft, 0x4b);
            W16(gTicksLeft, 0x2cf);
            W16(gMaxHP, 3);
            res = 1;
            FUN_00011bce();
            if (R16(gPauseMark) == 1) {
                W16(gPauseMark, 0);
                CALLC(4, SetGameMenuItemMark(1, 0));
            }
        }
        break;
    case 2:
        if (S16(FUN_00028ff6()) == 0) {
            W16(gNISLevel, 0);
            W16(gLastTimeNIS, -1);
            W16(GH(0x103054), 1);
            W16(gPauseMark, 0);
            res = 1;
        }
        break;
    case 4:                                         /* Open saved game */
        if (R16(gInNIS) == 1) {
            W16(GH(0x105830), 1);
            res = 1;
        } else if (R16(GH(0x103002)) == 0) {
            ResumeGameDialog();
            FUN_000225d2();
        } else {
            W16(GH(0x105830), 1);
        }
        break;
    case 5:                                         /* Save game */
        if (S16(FUN_00010f52()) != 0 && S16(FUN_0001143a()) != 0) {
            SaveGameDialog();
            FUN_000225d2();
        } else {
            AlertOKOnly(GH(0x102c60));
        }
        break;
    case 7:                                         /* Quit */
        W16(gQuit, 1);
        W16(gPauseMark, 0);
        res = 1;
        break;
    }
    RETURN((uint16_t)res);
}

/* 00013b9c: Edit menu (desk accessories only) */
uint32_t HandleEditChoice(int16_t item) {
    ENTER(0, 0);
    RETURN(_SystemEdit((int16_t)(item - 1)));
}

/* 00013bc6: Game menu */
uint32_t HandleGameMenuChoice(int16_t item) {
    ENTER(0, 2);
    uint32_t msg = 0;
    switch (item) {                       /* jump table at 0x13bf0 */
    case 1: {
        int16_t m = R16(gPauseMark);
        if (m != 2) {
            m = (m == 0) ? 1 : 0;
            W16(gPauseMark, m);
            CALLC(4, SetGameMenuItemMark(1, m));
        }
        break;
    }
    case 2:
        if (S16(seg5_02988a()) != 0) {
            _SetItemMark(R32(gGameMenu), 2, 0x12);
            msg = GH(0x102aac);
        } else {
            _SetItemMark(R32(gGameMenu), 2, 0);
            msg = GH(0x102ab6);
        }
        break;
    case 3:
        if (S16(FUN_00029860()) != 0) {
            _SetItemMark(R32(gGameMenu), 3, 0x12);
            msg = GH(0x102ac0);
        } else {
            _SetItemMark(R32(gGameMenu), 3, 0);
            msg = GH(0x102ad2);
        }
        break;
    case 4:
        FUN_00014064();
        break;
    case 5:
        ResumeAudioSystem();
        seg5_029374();
        CALLC(2, ShowHallOfFame(1));
        SetMasterVolume(R16(GH(0x106074)));
        PauseAudioSystem();
        break;
    case 7:
        if ((uint8_t)CheckPackDataFromGame() != 0) {
            W16(GH(0x106466), 1);
            W16(GH(0x103054), 1);
        }
        break;
    case 9:
        W16(GH(0x103054), 1);
        break;
    }
    if (msg != 0) {
        DrawMessage(msg);
        W16(GH(0x103034), 0x18);
        W16(GH(0x103036), 0x18);
    }
    RETURN(0);
}

/* 00013d02: enable the menu items usable while playing (on) or in a dialog (off) */
uint32_t SetMenusForGamePlayMode(int16_t on) {
    ENTER(0, 0);
    if (on != 0) {
        _EnableItem(R32(gFileMenu), 1);
        _EnableItem(R32(gFileMenu), 2);
        _EnableItem(R32(gFileMenu), 4);
        _EnableItem(R32(gFileMenu), 5);
        _EnableItem(R32(gGameMenu), 1);
        _EnableItem(R32(gGameMenu), 9);
    } else {
        _DisableItem(R32(gFileMenu), 2);
        _DisableItem(R32(gFileMenu), 5);
        _DisableItem(R32(gGameMenu), 1);
        _DisableItem(R32(gGameMenu), 9);
    }
    _DrawMenuBar();
    RETURN(e2MAC_HideMenuBar());
}

/* 00013d98: disable the menu items unusable in a dialog */
uint32_t FUN_00013d98(void) {
    ENTER_NOLINK(0);
    _DisableItem(R32(gFileMenu), 1);
    _DisableItem(R32(gFileMenu), 2);
    _DisableItem(R32(gFileMenu), 4);
    _DisableItem(R32(gFileMenu), 5);
    _DisableItem(R32(gGameMenu), 5);
    _DisableItem(R32(gGameMenu), 7);
    RETURN(_DisableItem(R32(gGameMenu), 9));
}

/* 00013de0 */
uint32_t SetGameMenuItemMark(int16_t item, int16_t on) {
    ENTER(0, 0);
    if (on != 0) RETURN(_SetItemMark(R32(gGameMenu), item, 0x12));
    RETURN(_SetItemMark(R32(gGameMenu), item, 0));
}

/* 00013e22: check marks of the sound / music items */
uint32_t FUN_00013e22(void) {
    ENTER_NOLINK(0);
    if (R16(gSoundOn) != 0) _SetItemMark(R32(gGameMenu), 2, 0x12);
    else _SetItemMark(R32(gGameMenu), 2, 0);
    if (R16(gMusicOn) != 0) RETURN(_SetItemMark(R32(gGameMenu), 3, 0x12));
    RETURN(_SetItemMark(R32(gGameMenu), 3, 0));
}

/* 00013e68: hide the cursor after 2 s without mouse movement */
uint32_t CheckCursorVisibility(void) {
    ENTER(4, 1);
    maddr pt = f;
    _GetMouse(pt);
    uint32_t now = _TickCount();
    if (R16(GH(0x102c52)) != R16(pt) || R16(GH(0x102c54)) != R16(pt + 2)) {
        W32(GH(0x102c56), _TickCount());
        W32(GH(0x102c52), R32(pt));
    } else if ((int32_t)(R32(GH(0x102c56)) + 0x78) < (int32_t)now) {
        _ObscureCursor();
    }
    RETURN(0);
}

/* 00013ece: apply a sound on/off change made from a menu */
uint32_t CheckSoundChanges(int16_t oldSound) {
    ENTER(0, 0);
    if (oldSound != R16(gSoundOn)) seg5_029374();
    if (R16(gMusicOn) == 0) {
        if (S16(PrinceSndOn(R16(GH(0x1048cc)))) != 0) PrinceResetSnd(R16(GH(0x1048cc)));
    }
    RETURN(0);
}

/* 00013f18: hide the menu bar (make its area part of the desktop) */
uint32_t e2MAC_HideMenuBar(void) {
    ENTER(0xc, 3);
    maddr port = f;
    if (R16(gSavedMBarHeight) != 0 || R16(GH(0x102cde)) == 0) RETURN(0);
    _GetPort(port);
    uint32_t gray = R32(LM_GrayRgn);
    W16(gSavedMBarHeight, R16(LM_MBarHeight));
    W16(LM_MBarHeight, 0);
    uint32_t rgn = calcMenuRgn(R16(gSavedMBarHeight));
    _UnionRgn(gray, rgn, gray);
    uint32_t w = _FrontWindow();
    W32(f + 4, w);
    _PaintBehind(w, rgn);
    w = _FrontWindow();
    W32(f + 8, w);
    _CalcVisBehind(w, rgn);
    _DisposeRgn(rgn);
    _SetPort(R32(port));
    RETURN(0);
}

/* 00013fa6: show the menu bar again */
uint32_t e2MAC_ShowMenuBar(void) {
    ENTER(4, 3);
    maddr port = f;
    if (R16(gSavedMBarHeight) == 0) RETURN(0);
    _GetPort(port);
    W16(LM_MBarHeight, R16(gSavedMBarHeight));
    uint32_t rgn = calcMenuRgn(R16(gSavedMBarHeight));
    W16(gSavedMBarHeight, 0);
    uint32_t gray = R32(LM_GrayRgn);
    _DiffRgn(gray, rgn, gray);
    uint32_t w = _FrontWindow();
    _CalcVisBehind(w, rgn);
    _DisposeRgn(rgn);
    _HiliteMenu(0);
    _DrawMenuBar();
    _SetPort(R32(port));
    RETURN(0);
}
