/* CODE 2 "Main", second half (Ghidra 0x14020-0x17656): application start-up and shut-down,
 * preferences file, the outer game loop (title/attract sequence, level restart loop, per-frame
 * update), keyboard reading, cheat keys, time keeping and the health meters.
 * Ported from the 68k disassembly (see docs/PORTING.md and docs/notes/seg02b.md); addresses in
 * comments are Ghidra addresses. */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* ---- globals (A5 offsets; Ghidra address = 0x108000 + offset) ---- */
#define gGameState      G32(-0x5014)    /* 102fec long: game state block (level data, room table) */
#define gWindow         G32(-0x5242)    /* 102dbe long: the game window */
#define gOffWorld       GA(-0x523e)     /* 102dc2: offscreen world record */
#define gSettings       GA(-0x532a)     /* 102cd6: 16-byte record saved in ":Data:Prince2.opt" */
#define gInitErr        (-0x531a)       /* 102ce6 word: InitGame error flags (0x100 old system, 0x1000 VM on) */
#define gWindowLarge    (-0x5322)       /* 102cde word: window size option (menu item 4) */
#define gWindowPos      (-0x5320)       /* 102ce0 long: saved window position */
#define gDataPacked     (-0x5326)       /* 102cda word: data already packed (menu item 7) */
#define gPackTried      (-0x5324)       /* 102cdc word */
#define gGameMenu       G32(-0x5318)    /* 102ce8 long: game menu handle */
#define gCheatMode      (-0x2824)       /* 1057dc word: cheat mode enabled */
#define gCurRoom        (-0x4fc2)       /* 10303e word: current room */
#define gNextRoom       (-0x4fc4)       /* 10303c word: room to cut to */
#define gLevel          (-0x4fd2)       /* 10302e word: current level */
#define gNextLevel      (-0x4f92)       /* 10306e word: level to go to */
#define gQuit           (-0x4fd0)       /* 103030 word: quit the game */
#define gStartLevel     (-0x4faa)       /* 103056 word: level to start/restart */
#define gMinutesLeft    (-0x4fce)       /* 103032 word */
#define gFramesLeft     (-0x4fc6)       /* 10303a word: frames left in the current minute (0x2cf) */
#define gShowTime       (-0x4fa2)       /* 10305e word: request to display the time left */
#define gMsgId          (-0x4fcc)       /* 103034 word: message being displayed */
#define gMsgTimer       (-0x4fca)       /* 103036 word: message display timer */
#define gResumeFlag     (-0x521c)       /* 102de4 word: a saved game was resumed */
#define gKidHP          (-0x50b4)       /* 102f4c word: kid strength */
#define gKidMaxHP       (-0x50b2)       /* 102f4e word: kid max strength */
#define gKidHPDelta     (-0x50b0)       /* 102f50 word: pending change of kid strength */
#define gStartMaxHP     (-0x4f98)       /* 103068 word: max strength at level start */
#define gOppIndex       (-0x5098)       /* 102f68 word: opponent whose meter is shown */
#define gKeyX           (-0x3498)       /* 104b68 word: joystick x (-1/0/1) */
#define gKeyY           (-0x3496)       /* 104b6a word: joystick y (-1/0/1) */
#define gKeyBtn         (-0x3494)       /* 104b6c word: button (-1 shift/-2 option) */
#define gFrameCount     (-0x349a)       /* 104b66 word */
#define gGetKeysProc    G32(-0x1f68)    /* 106098 long: GetKeys trap address */
#define gGameRect       GA(-0x4fec)     /* 103014: play area rectangle */
#define gScreenRect     GA(-0x1dfc)     /* 106204: whole window rectangle */
#define gAmbient        GA(-0x3738)     /* 1048c8: ambient sound record */

#define KID             GA(-0x5056)     /* 102faa: current character (0x3e bytes) */
#define SAVED_KID       GA(-0x50d2)     /* 102f2e: copy of the kid */
#define OPPS            GA(-0x5208)     /* 102df8: opponents in the room, 0x3e bytes each */
#define OPP_SIZE        0x3e

#define LEVEL_TYPE()    R16(gGameState + 0x2186)
#define LEVEL_NUM()     R16(gGameState + 0x218a)
/* number of opponents in a room (rooms are 0xc0-byte records in the game state) */
#define ROOM_OPPS(room) R16(gGameState + (uint32_t)(uint16_t)(room) * 0xc0 + 0x20e6)

/* Stack frame of the original. The hook has already popped the return address, so on entry
 * the emulated SP is 4 bytes above where the original runs. ENTER(link, regmask) rebuilds what
 * the original's prologue leaves on the stack: return address (still in memory), then for
 * `link A6,#-link` (link >= 0) the caller's A6 and `link` bytes of locals, then the registers
 * saved by `movem.l` (regmask: bit n = Dn, bit 8+n = An), and sets the CPU's A6 to the new frame
 * like `link` does. Callees therefore run at the same stack addresses and see the same frame
 * chain as in the original (some of them store stack addresses in globals or copy uninitialised
 * stack bytes, which the differential checker would otherwise flag). `a6` is the original's
 * frame pointer: locals are at a6 - offset exactly as in the disassembly. Every exit goes
 * through RETURN(), which restores SP and A6. */
typedef struct { maddr sp0, a6; uint32_t old_a6; } OrigFrame;
static OrigFrame enter_frame(int32_t link, uint16_t mask) {
    OrigFrame fr;
    fr.sp0 = emu_sp();
    fr.old_a6 = cpu_get(R_A6);
    maddr sp = fr.sp0 - 4;
    fr.a6 = sp - 4;
    if (link >= 0) {
        sp -= 4;
        W32(sp, fr.old_a6);
        sp -= (uint32_t)link;
    }
    int n = 0;
    for (int r = 0; r < 16; r++) if (mask & (1u << r)) n++;
    sp -= 4u * (uint32_t)n;
    maddr p = sp;
    for (int r = 0; r < 16; r++)
        if (mask & (1u << r)) { W32(p, cpu_get(r)); p += 4; }
    emu_set_sp(sp);
    if (link >= 0) cpu_set(R_A6, fr.a6);
    return fr;
}
static void leave_frame(const OrigFrame *fr) {
    cpu_set(R_A6, fr->old_a6);
    emu_set_sp(fr->sp0);
}
#define ENTER(link, mask) \
    OrigFrame fr__ = enter_frame((link), (mask)); maddr sp0__ = fr__.sp0; maddr a6 = fr__.a6; (void)a6; (void)sp0__
#define RETURN(v)       do { uint32_t r__ = (uint32_t)(v); leave_frame(&fr__); return r__; } while (0)

/* Calls between ported functions of this unit: push the arguments like the original `jsr`
 * does (the callee's ENTER expects them at SP, as after the hook popped the return address). */
static uint32_t callw(uint32_t (*fn)(int16_t), int16_t a) {
    maddr sp = emu_sp(); push16((uint16_t)a); uint32_t r = fn(a); emu_set_sp(sp); return r;
}
static uint32_t calll(uint32_t (*fn)(uint32_t), uint32_t a) {
    maddr sp = emu_sp(); push32(a); uint32_t r = fn(a); emu_set_sp(sp); return r;
}
static uint32_t callll(uint32_t (*fn)(uint32_t, uint32_t), uint32_t a, uint32_t b) {
    maddr sp = emu_sp(); push32(b); push32(a); uint32_t r = fn(a, b); emu_set_sp(sp); return r;
}

/* address of a constant in this code segment (PC-relative data in the original) */
#define CODE2(a)        (fn_address(FN_calcMenuRgn) + ((uint32_t)(a) - 0x14020))

/* save the current character into SAVED_KID (15 longs + 1 word) */
static void save_kid(void) {
    maddr s = KID, d = SAVED_KID;
    for (int i = 0; i < 15; i++) { W32(d, R32(s)); s += 4; d += 4; }
    W16(d, RU16(s));
}

static uint32_t sprintf1(maddr buf, maddr fmt, uint32_t v) {
    return call_ptr(JT_ADDR(FN_tc_sprintf), "lll", buf, fmt, v);
}

/* _MaxMem: D0 = largest block, A0 = grow */
static uint32_t max_mem(uint32_t *grow) {
    tb_begin();
    tb_trap(0xA11D);
    uint32_t r = cpu_get(R_D0);
    *grow = cpu_get(R_A0);
    tb_end();
    return r;
}

/* 00014020: region covering the menu bar: screen bounds with bottom = top + height */
uint32_t calcMenuRgn(int16_t height) {
    ENTER(0x8, 0x0400);                 /* movem A2 */
    maddr f = a6 - 8;
    maddr src = GA(-0x18f2);                /* screenBits.bounds */
    W32(f, R32(src)); W32(f + 4, R32(src + 4));
    W16(f + 4, R16(f) + height);
    uint32_t rgn = _NewRgn();
    _RectRgn(rgn, f);
    RETURN(rgn);
}

/* 00014064: toggle the window size option */
uint32_t FUN_00014064(void) {
    ENTER(-1, 0x0000);
    int16_t v = G16(gWindowLarge) == 0 ? 1 : 0;
    SG16(gWindowLarge, v);
    GameWindow(gWindow, v, GA(gWindowPos));
    _SelectWindow(gWindow);
    SetGameMenuItemMark(4, G16(gWindowLarge));
    RETURN(0);
}

/* 0001409c: initialise the Toolbox, menus, window and offscreen world */
uint32_t MRInit(void) {
    ENTER(0xc, 0x0438);                 /* movem D3-D5/A2 */
    maddr f = a6 - 0xc;                          /* rect at A6-0xc */
    SG32(-0x5242, 0);
    SG8(-0x521e, 0);
    FUN_000142bc();
    FUN_000142de();
    FUN_0004ddfa();
    if (S16(InitGame()) != 0) { RETURN(0); }
    if (G16(-0x1dfe) != 0) SG32(-0x5242, _GetNewCWindow(400, 0, 0xffffffff));
    else SG32(-0x5242, _GetNewWindow(400, 0, 0xffffffff));
    if (gWindow == 0) ErrorHandler(1, 0);
    SetUpPalette(gWindow, 1, 0xfe);
    if (G16(gDataPacked) == 0 && G16(gPackTried) == 0) {
        SG16(gPackTried, 1);
        uint32_t r = PackDataAtStartUp();
        SG16(gDataPacked, r & 0xff);
    }
    if (G16(gDataPacked) != 0) _DisableItem(gGameMenu, 7);
    SetGameMenuItemMark(7, G16(gDataPacked));
    callw(GetSetDefaultSettings, 1);
    GameWindow(gWindow, G16(gWindowLarge), GA(gWindowPos));
    SetGameMenuItemMark(4, G16(gWindowLarge));
    _SelectWindow(gWindow);
    _SetPort(gWindow);
    SetGameRect(f);
    W16(f + 6, R16(f + 6) + 2);
    if (S16(BuildSOffWorld(gOffWorld, f)) != 0) ErrorHandler(5, 0);
    SaveCurrentWorld(gOffWorld);
    _TextMode(1);
    _PenNormal();
    SetBackColor(0xff);
    SetForeColor(0);
    _EraseRect(f);
    RestoreOriginalWorld(gOffWorld);
    _TextMode(1);
    _BackColor(0x21);
    SetForeColor(0);
    uint32_t a = tc_NGetTrapAddress(0x0100, 0x60);
    uint32_t b = tc_NGetTrapAddress(0x0100, 0x9f);
    SG16(-0x5308, a != b ? 1 : 0);
    SetCheatMode();
    RETURN(1);
}

/* 00014258: shut down and quit */
uint32_t FUN_00014258(void) {
    ENTER(-1, 0x0000);
    SetWindowPos();
    if (gWindow != 0) _DisposeWindow(gWindow);
    DisposeSOffWorld(gOffWorld);
    ExitGame();
    Terminate(1, 0);
    RETURN(0);
}

/* 00014284: remember the window position (small window only) */
uint32_t SetWindowPos(void) {
    ENTER(0x4, 0x0008);                 /* movem D3 */
    if (G16(gWindowLarge) != 0) RETURN(0);
    maddr f = a6 - 4;
    W16(f, 0); W16(f + 2, 0);
    _LocalToGlobal(f);
    SG32(gWindowPos, R32(f));
    RETURN(0);
}

/* 000142bc: Toolbox managers */
uint32_t FUN_000142bc(void) {
    ENTER(-1, 0x0000);
    _InitGraf(GA(-0x187e));
    _InitFonts();
    _InitWindows();
    _InitMenus();
    _TEInit();
    _InitDialogs(0);
    _InitCursor();
    _MaxApplZone();
    tb_begin();                             /* FlushEvents(everyEvent, 0) */
    cpu_set(R_D0, 0x0000ffff);
    tb_trap(0xA032);
    tb_end();
    RETURN(0);
}

/* 000142de: menu bar */
uint32_t FUN_000142de(void) {
    ENTER(-1, 0x0c00);                 /* movem A2-A3 */
    uint32_t h = PoPGetResource(0x4d424152, 400);      /* 'MBAR' */
    W16(R32(h), 4);
    uint32_t mbar = _GetNewMBar(400);
    if (mbar == 0) ErrorHandler(2, 0);
    _SetMenuBar(mbar);
    SG32(-0x530c, _GetMenuHandle(400));
    SG32(-0x5310, _GetMenuHandle(0x191));
    SG32(-0x5314, _GetMenuHandle(0x192));
    uint32_t m = _GetMenuHandle(0x193);
    SG32(-0x5318, m);
    if (G32(-0x530c) == 0 || G32(-0x5310) == 0 || G32(-0x5314) == 0 || m == 0) ErrorHandler(3, 0);
    _AppendResMenu(G32(-0x530c), 0x44525652);         /* 'DRVR' */
    _DrawMenuBar();
    RETURN(0);
}

/* 00014388: check the system, load settings, allocate the main data blocks */
uint32_t InitGame(void) {
    ENTER(0x10, 0x0418);                 /* movem D3-D4/A2 */
    maddr f = a6 - 0x10;                         /* f+2: system version, f+4: Gestalt response, f+8: rect */
    SG16(gInitErr, 0);
    uint32_t a = tc_NGetTrapAddress(0x0100, (int16_t)0xa1ad);   /* _Gestalt */
    uint32_t b = tc_NGetTrapAddress(0x0100, 0x9f);              /* _Unimplemented */
    if (a != b) {
        tc_Gestalt(f + 4, 0x766d2020);                          /* 'vm  ' */
        if (R32(f + 4) != 0) {
            tc_Gestalt(f + 4, 0x766d6273);                      /* 'vmbs' */
            if (R32(f + 4) != 0) SG16(gInitErr, G16(gInitErr) | 0x1000);
        }
    }
    uint32_t ok = Sys6OrLater(f + 2, 0x0100);
    if ((ok & 0xff) == 0) {
        SG16(gInitErr, G16(gInitErr) | 0x100);
    } else {
        SG16(-0x5356, 1);
        SG16(-0x5358, R16(f + 2));
        callw(GetSetDefaultSettings, 0);
        FUN_00013e22();
        SG16(-0x5354, 1);
        SG16(-0x4fee, 1);
        if (G8(-0x521e) != 0) {
            SaveCurrentWorld(gOffWorld);
            _PenNormal();
            SetGameRect(f + 8);
            _ForeColor(0x21);
            _PaintRect(f + 8);
            RestoreOriginalWorld(gOffWorld);
        }
        SG32(-0x5014, PoPNewHiPtr(0x49ca));
        SG32(-0x4ca2, PoPNewHiPtr(0x134c));
        SG32(-0x4ca6, PoPNewHiPtr(0x11d0));
        SG32(-0x4caa, PoPNewHiPtr(0x2d0));
        SG32(-0x4cae, PoPNewHiPtr(0x78));
        SG32(-0x4cb2, PoPNewHiPtr(0x348));
        SG32(-0x43cc, PoPNewHiPtr(0x3c));
        SG32(-0x43d0, PoPNewHiPtr(0x12c));
        SG32(-0x43d4, PoPNewHiPtr(0x12c));
        SG32(-0x43d8, PoPNewHiPtr(0x12c));
        SG32(-0x43dc, PoPNewHiPtr(0x3c));
        SG32(-0x43e0, PoPNewHiPtr(0x3c));
        SG32(-0x43e4, PoPNewHiPtr(100));
        SG32(-0x4496, PoPNewHiPtr(0x1e0));
        InitAmbient(gAmbient);
        SG32(-0x1df4, JT_ADDR(FN_FUN_00015a02));          /* input poll procedure */
    }
    uint32_t r = GU16(gInitErr);
    RETURN(r);
}

/* 00014562: save settings, show the "about" alert for VM / error conditions */
uint32_t ExitGame(void) {
    ENTER(0x4, 0x0000);
    if (G16(gInitErr) == 0) callw(GetSetDefaultSettings, 1);
    if (G16(gInitErr) & 0x10) DrawAbout(0x25b);
    else if (G16(gInitErr) & 0x1000) DrawAbout(0x25a);
    RETURN(0);
}

/* 000145b0: read (0) or write (1) the two settings words in ":Data:Prince2.opt" */
uint32_t GetSetDefaultSettings(int16_t write) {
    ENTER(0xc, 0x0000);
    maddr f = a6 - 0xc;                          /* f: refNum, f+8: count */
    W16(f, 0);
    W32(f + 8, 0);
    int16_t rn = S16(calll(MakeGameFile, CODE2(0x146b4)));
    W16(f, rn);
    if (rn != -1) {
        _CloseResFile(rn);
        if (S16(tc_FSOpen(f, 0, CODE2(0x146c8))) != 0) ErrorHandler(4, 0);
        else if (S16(tc_GetEOF(f + 8, R16(f))) != 0) ErrorHandler(4, 0);
        if (write == 0) {
            W32(f + 8, 0x10);
            if (S16(tc_FSRead(gSettings, f + 8, R16(f))) != 0) ErrorHandler(4, 0);
            SG16(-0x1f78, G16(-0x532a));
            SG16(-0x3738, G16(-0x5328));
        } else {
            SG16(-0x532a, G16(-0x1f78));
            SG16(-0x5328, G16(-0x3738));
            W32(f + 8, 0x10);
            if (S16(tc_FSWrite(gSettings, f + 8, R16(f))) != 0) ErrorHandler(4, 0);
        }
        tc_FSClose(R16(f));
    }
    RETURN(0);
}

/* 000146dc: open (creating it with default settings if needed) the settings/resource file */
uint32_t MakeGameFile(uint32_t name) {
    ENTER(0x120, 0x00f8);                 /* movem D3-D7 */
    maddr f = a6 - 0x120;                        /* f = A6-0x120 */
    maddr vRef = f, refNum = f + 2, finfo = f + 8, volName = f + 0x18, freeBytes = f + 0x118, eof = f + 0x11c;
    W16(refNum, 0);
    W32(eof, 0);
    int16_t err = S16(tc_GetVol(vRef, volName));
    int16_t verr = S16(tc_GetVInfo(freeBytes, vRef, volName, 0));
    W16(f + 4, verr);
    if (err != 0 || verr != 0) ErrorHandler(0, 0);
    int16_t rf = S16(_OpenResFile(name));
    if (rf == -1) {
        _CreateResFile(name);
        rf = S16(_OpenResFile(name));
        if (rf != -1) {
            tc_GetVol(vRef, volName);
            tc_GetFInfo(finfo, R16(vRef), name);
            W32(finfo, 0x72737263);         /* 'rsrc' */
            W32(finfo + 4, 0x50524e63);     /* 'PRNc' */
            err = S16(tc_SetFInfo(finfo, R16(vRef), name));
        }
        _CloseResFile(rf);
        if (S16(tc_FSOpen(refNum, 0, CODE2(0x1489e))) != 0) ErrorHandler(4, 0);
        else if (S16(tc_GetEOF(eof, R16(refNum))) != 0) ErrorHandler(4, 0);
        SG16(-0x532a, 1);
        SG16(-0x5328, 1);
        SG16(-0x5326, 0);
        SG16(-0x5324, 0);
        SG16(-0x5320, 0);
        SG16(-0x531e, 0);
        SG16(-0x531c, 0);
        SG16(-0x5322, 1);
        W32(eof, 0x10);
        if (S16(tc_FSWrite(gSettings, eof, R16(refNum))) != 0) ErrorHandler(4, 0);
        tc_FSClose(R16(refNum));
        rf = S16(_OpenResFile(name));
    }
    if (rf == -1 || err != 0 || S16(_ResError()) != 0) {
        if (R16(f + 4) == 0 && S32(R32(freeBytes)) < 10000) ErrorHandler(7, 0);
        else ErrorHandler(4, 0);
    }
    RETURN((uint16_t)rf);
}

/* 000148b2: cheat mode is on when the keys of KeyMap long 0 == 0x1c00 are held at start-up */
uint32_t SetCheatMode(void) {
    ENTER(0x10, 0x0000);
    maddr f = a6 - 0x10;
    maddr src = GA(-0x533a);
    for (int i = 0; i < 16; i += 4) W32(f + i, R32(src + i));
    call_ptr(gGetKeysProc, "l", f);
    SG16(gCheatMode, R32(f) == 0x1c00 ? 1 : 0);
    RETURN(0);
}

/* 000148fa: main */
uint32_t main_(void) {
    ENTER(0x0, 0x0008);                 /* movem D3 */
    int16_t ok = S16(MRInit());
    callll(CheckAvailableMemory, 0xf4240, 0x7a120);
    if (ok != 0) {
        MohawkInit();
        SetStartLevel();
        FUN_0001d382();
        Rnd(1);
        W16(gGameState + 0x2186, 0);
        SetMenusForGamePlayMode(0);
        ShowCheatMode();
        ResetGame();
        ResetSoundItem(0);
        FUN_0001c94a();
    }
    FUN_00014258();
    RETURN(0);
}

/* 0001496a: outer loop: attract mode / start or restart levels until quit */
uint32_t ResetGame(void) {
    ENTER(0x4, 0x0018);                 /* movem D3-D4 */
    int16_t lvl = G16(gStartLevel);
    if (G16(gQuit) == 0) {
        do {
            FUN_000550b8();
            FUN_00015e78();
            if (G16(gResumeFlag) != 0) {
                if (G16(gResumeFlag) == 2) CopyProtectionDialog();
                lvl = G16(gStartLevel);
            }
            if (G16(gStartLevel) == 0) {
                lvl = S16(FUN_00014b48());
            } else {
                lvl = S16(callw(Start, lvl));
                if (G16(-0x4ffe) != 0) {
                    lvl = 0;
                    W16(gGameState + 0x218a, 0);
                }
                if (G16(gMinutesLeft) == 0) {
                    lvl = 0;
                    SG16(gStartLevel, 0);
                } else if (lvl > 0xe) {
                    SG16(gStartLevel, -2);
                    PlayNIS(0xb);
                    HallOfFameDialog(G16(gMinutesLeft));
                    if (G16(gStartLevel) == -2) {
                        lvl = 0;
                        SG16(gStartLevel, 0);
                    } else {
                        lvl = G16(gStartLevel);
                    }
                    SG16(-0x27d2, -1);
                } else if (G16(gStartLevel) != 0 && lvl == 0) {
                    lvl = LEVEL_NUM();
                    SG16(gStartLevel, lvl);
                }
            }
        } while (G16(gQuit) == 0);
    }
    RETURN(GU16(gQuit));
}

/* 00014a44: Finder start-up: open a saved game document, or refuse to print */
uint32_t SetStartLevel(void) {
    ENTER(0x20c, 0x0038);                 /* movem D3-D5 */
    maddr f = a6 - 0x20c;                        /* f: message, f+2: count, f+4: AppFile */
    SG16(gStartLevel, 0);
    tc_CountAppFiles(f + 2, f);
    int16_t count = R16(f + 2);
    if (count > 0) {
        if (R16(f) == 0) {
            int16_t i = 1;
            if (count >= 1 && G16(gResumeFlag) == 0) {
                do {
                    tc_GetAppFiles(f + 4, i);
                    if (tc_memcmp(f + 6, GA(-0x56f0), 4) == 0) {
                        tc_PtoCstr(f + 0xc);
                        ResumeGame(f + 0xc, R16(f + 4));
                    }
                    i++;
                } while (i <= R16(f + 2) && G16(gResumeFlag) == 0);
            }
            if (G16(gResumeFlag) != 0) {
                int16_t l = G16(gLevel);
                SG16(gNextLevel, l);
                SG16(gStartLevel, l);
                SG16(gResumeFlag, 2);
            }
        } else if (R16(f) == 1) {
            AlertOKOnly(CODE2(0x14b14));
            Terminate(0, 0);
        }
    }
    RETURN(0);
}

/* 00014b48: attract sequence (credits, hall of fame, intro cut scenes) -> level to start */
uint32_t FUN_00014b48(void) {
    ENTER(-1, 0x0008);                 /* movem D3 */
    int16_t r = 1;
    if (G16(-0x524c) == 0) {
        if (G16(-0x4ffc) != 0) r = 2;
        else if (S16(DoCreditsDialog()) != 0) r = 2;
        else if (S16(ShowHallOfFame(0)) != 0) r = 2;
    } else {
        SG16(-0x524c, 0);
    }
    if (r != 2) r = S16(PlayNIS(7));
    if (r != 2) r = S16(PlayNIS(4));
    if (r != 2) r = S16(PlayNIS(8));
    if (r == 2) {
        SG16(gStartLevel, 1);
        SG16(-0x524c, 1);
        seg5_02a5c4();
    } else {
        seg5_02a5e4();
    }
    r = G16(gStartLevel);
    FUN_00016112();
    if (G16(-0x27d0) != 0) r = S16(FUN_00014bea());
    RETURN((uint16_t)r);
}

/* 00014bea: "resume game" dialog requested from the menu */
uint32_t FUN_00014bea(void) {
    ENTER(-1, 0x0018);                 /* movem D3-D4 */
    int16_t r = G16(gStartLevel);
    if (S16(ResumeGameDialog()) > 0) {
        r = G16(gLevel);
        SG16(gStartLevel, r);
        SG16(-0x4fac, 0);
    }
    SG16(-0x27d0, 0);
    RETURN((uint16_t)r);
}

/* 00014c16: poll the user: keyboard commands (esc, space, v, l) and cheat keys */
uint32_t FUN_00014c16(void) {
    ENTER(-1, 0x0430);                 /* movem D4-D5/A2 */
    maddr msg = 0;
    int16_t restart = 0;
    int16_t key = S16(PollUser0());
    if ((G16(gKeyBtn) | key) != 0) {
        if ((G16(gMinutesLeft) != 0 && G16(-0x50b6) > 6) || G16(-0x4ffe) != 0) {
            restart = 1;
            if (G16(-0x4ffe) != 0) SG16(-0x4ffc, 1);
        }
    }
    if (key != 0) {
        switch ((uint16_t)key) {
        case 0x1b:
            SG16(-0x524a, 1);
            break;
        case 0x20:
            if (G16(-0x27d2) >= 0) SG16(gShowTime, 1);
            break;
        case 0x76:
            if (G16(-0x5344) & 0x100) msg = GA(-0x5306);
            break;
        case 0x6c:
            if (G16(-0x5344) & 0x100) {
                if (G16(gLevel) <= 3 || G16(gCheatMode) != 0) {
                    if (G16(gLevel) == 0xe && G16(gCheatMode) != 0) {
                        SG16(gNextLevel, 1);
                    } else {
                        SG16(gNextLevel, G16(gLevel) + 1);
                        if (G16(gCheatMode) == 0 && G16(gMinutesLeft) > 0xf) {
                            SG16(gMinutesLeft, 0xf);
                            SG16(gFramesLeft, 0x2cf);
                        }
                    }
                    ResetSoundItem(0);
                }
            }
            break;
        }
        if (msg != 0) {
            DrawMessage(msg);
            SG16(gMsgId, 0x18);
            SG16(gMsgTimer, 0x18);
        }
    }
    if (G16(gCheatMode) != 0) callw(CheckCheatKeys, key);
    if (restart != 0 && G16(-0x4f90) == 0) {
        if (!(G16(gCurRoom) == 4 && LEVEL_NUM() == 0xd && S16(IsAltarShadowActive()) != 0)) {
            SG16(-0x4fac, 1);
            ResetSoundItem(0);
        }
    }
    RETURN((uint16_t)key);
}

/* 00014d5c: cheat keys */
uint32_t CheckCheatKeys(int16_t key) {
    ENTER(0x54, 0x0418);                 /* movem D3-D4/A2 */
    if (key == 0) RETURN(0);
    maddr f = a6 - 0x54;                         /* f: text buffer, f+0x50: number for GetNumber */
    maddr buf = f, num = f + 0x50;
    int16_t d3, d4;
    switch ((uint16_t)key) {
    case 0x2d:                              /* '-': one minute less */
        if (G16(gMinutesLeft) > 1) SG16(gMinutesLeft, G16(gMinutesLeft) - 1);
        SG16(gMsgId, 0);
        SG16(gMsgTimer, 0);
        SG16(gShowTime, 1);
        break;
    case 0x2b:                              /* '+': one minute more */
        SG16(gMinutesLeft, G16(gMinutesLeft) + 1);
        SG16(gMsgId, 0);
        SG16(gMsgTimer, 0);
        SG16(gShowTime, 1);
        break;
    case 0x42: {                            /* 'B': blind mode */
        int16_t v = G16(-0x5246) == 0 ? 1 : 0;
        SG16(-0x5246, v);
        if (v != 0) {
            _EraseRect(gGameRect);
        } else {
            SG16(-0x5244, 1);
            LoadCustomRoomData(G16(gCurRoom));
        }
        break;
    }
    case 0x64:                              /* 'd': set the debug number, then show it */
        if (G16(-0x5344) & 0x100) {
            W16(num, 0);
            uint32_t r = GetNumber(num, G16(-0x36a0), 0x63, 0, CODE2(0x151fc));
            if ((uint8_t)(r >> 8) != 0) {
                SG16(-0x36a0, R16(num));
                W16(num, 1);
            }
        }
        /* fall through */
    case 0x44:                              /* 'D' */
        sprintf1(buf, CODE2(0x1521a), (uint32_t)(int32_t)G16(-0x36a0));
        DrawMessage(buf);
        SG16(gMsgId, 0x18);
        SG16(gMsgTimer, 0x18);
        break;
    case 0x67:                              /* 'g' */
        if (G16(-0x5344) & 0x100) {
            GoToLevelDialog();
        } else {
            save_kid();
            d3 = G16(-0x501c);
            if (d3 != -1) {
                LoadShad(d3);
                SG16(-0x5036, G16(-0x5036) + 1);
                SG16(-0x5034, 1);
                seg4_022516();
                ChangeMeters();
                seg2_01585a();
            }
            FUN_000225d2();
        }
        break;
    case 0x49:                              /* 'I' */
        FUN_00015e26();
        break;
    case 0x52:                              /* 'R': show the room number */
        sprintf1(buf, CODE2(0x15226), (uint32_t)GU16(gCurRoom));
        DrawMessage(buf);
        SG16(gMsgId, 0x18);
        SG16(gMsgTimer, 0x18);
        break;
    case 0x53:                              /* 'S' */
        d3 = LEVEL_TYPE();
        if (d3 == 2 || d3 == 6) {
            SG16(-0x5004, 7);
            SG16(-0x502e, 5);
            seg4_01fa82();
            if (LEVEL_TYPE() == 6) PoPSetPalette(0x7d1, 0x30);
            else PoPSetPalette(0x7d0, 0x30);
        }
        break;
    case 0x74:                              /* 't' */
        if (G16(-0x5344) & 0x100) seg16_04e2e8();
        break;
    case 0x61:                              /* 'a' */
        if (G16(-0x5344) & 0x100) seg16_04e2bc();
        break;
    case 0x57:                              /* 'W' */
        seg2_015e56();
        break;
    case 0x46:                              /* 'F' */
        SG16(-0x2812, G16(-0x2812) == 0 ? 1 : 0);
        break;
    case 0x6b: {                            /* 'k': kill the opponents in the room */
        SG16(-0x501c, -1);
        save_kid();
        d3 = G16(gCurRoom);
        d4 = d3 != 0 ? R16(gGameState + (uint32_t)(uint16_t)d3 * 0xc0 + 0x20e6) : 0;
        int16_t i = 0;
        if (d4 > 0) {
            do {
                LoadShad(i);
                if (G16(-0x503a) < 0) {
                    SG16(-0x503a, 0);
                    SG16(-0x5034, -G16(-0x5038));
                    if (R16(gGameState + 0x21a4) == 2) {
                        seg4_01f1de();
                    } else if (G16(-0x504e) == 10) {
                        SetSkelTimer(i, 0x1e0);
                        JumpSeq(0x6b);
                        SG16(-0x503e, 1);
                    } else {
                        d3 = R16(gGameState + 0x21a4);
                        if (d3 == 5 || d3 == 6) {
                            SG16(-0x5054, ~G16(-0x50d0));
                            SG16(-0x5038, 0);
                            JumpSeq(0x9a);
                        } else {
                            JumpSeq(0x55);
                        }
                    }
                    seg4_022516();
                }
                i++;
            } while (i < d4);
        }
        FUN_000225d2();
        break;
    }
    case 0x4b:                              /* 'K': kill the kid */
        SG16(-0x5034, -1);
        save_kid();
        ChangeMeters();
        seg2_01585a();
        FUN_000225d2();
        if (G16(-0x5038) == 0) {
            JumpSeq(0x47);
            SetKidDeathMethod(3);
        }
        SG16(-0x5034, 0);
        d3 = LEVEL_NUM();
        if (d3 >= 3 && d3 <= 0xd) AddSound(0x1d);
        else AddSound(0x12c);
        break;
    }
    switch ((uint16_t)(G32(-0x5350) & 0xff00)) {   /* modifier/key word of the last event */
    case 0x7a00:
        SG16(-0x2812, G16(-0x2812) != 0 ? 0 : 2);
        break;
    case 0x6100:
        SG16(-0x5282, G16(-0x5282) == 0 ? 1 : 0);
        break;
    case 0x6200:
        SG16(-0x2818, G16(-0x2818) == 0 ? 1 : 0);
        break;
    case 0x6400:
        SG16(-0x2816, G16(-0x2816) == 0 ? 1 : 0);
        break;
    }
    /* (the original then draws a message if a local pointer is set; it is always NULL) */
    RETURN(0);
}

/* 0001522e: quit with a message if there is not enough memory */
uint32_t CheckAvailableMemory(uint32_t need, uint32_t needFree) {
    ENTER(0x100, 0x0008);                 /* movem D3 */
    maddr f = a6 - 0x100;
    uint32_t missing = callll(AvailableMemory, need, needFree);
    if (missing != 0) {
        sprintf1(f, GA(-0x52ee), missing + 0x400);
        Terminate(1, f);
    }
    RETURN(0);
}

/* 0001528c: bytes missing to have `need` in the application zone and `needFree` free */
uint32_t AvailableMemory(uint32_t need, uint32_t needFree) {
    ENTER(0x100, 0x0c78);                 /* movem D3-D6/A2-A3 */
    int32_t limit = S32(R32(0x130));        /* ApplLimit */
    int32_t zone = S32(R32(0x2aa));         /* ApplZone */
    int32_t have = limit - zone;
    int32_t want = S32(need);
    int32_t missing = have >= want ? 0 : want - have;
    if (missing == 0) {
        int32_t fr = S32(GetFreeMemory());
        if (fr < S32(needFree)) {
            if (_UnloadScrap() != 0) {
                missing = S32(needFree) - fr;
            } else {
                fr = S32(GetFreeMemory());
                want = S32(needFree);
                if (fr < want) missing = want - fr;
            }
        }
    }
    RETURN((uint32_t)missing);
}

/* 00015308: total purgeable free memory */
uint32_t GetFreeMemory(void) {
    ENTER(0x8, 0x0000);
    maddr f = a6 - 8;                            /* f: total, f+4: contig */
    tc_PurgeSpace(f + 4, f);
    uint32_t r = R32(f);
    RETURN(r);
}

/* 00015332: "cheat mode" splash */
uint32_t ShowCheatMode(void) {
    ENTER(0x4, 0x0008);                 /* movem D3 */
    if (G16(gCheatMode) == 0) RETURN(0);
    HandleEvent();
    HandleEvent();
    HandleEvent();
    HandleEvent();
    PoPSetPalette(0x1f40, 0x10);
    maddr f = a6 - 4;
    W16(f + 2, 1);
    W16(f, 1);
    DialogDrawStr(CODE2(0x153d8), 0, gGameRect, f);
    PlaySound(-1, 0x271d, 1);
    SetTimer(0, 0, 0xb4);
    do {
        if (S16(FUN_000550b8()) == 0x1b) break;
    } while (S16(GetTimer(0)) != 0);
    SetTimer(0, 0, 0);
    RETURN(0);
}

/* 00015400: trigger the animated objects of the current room and its neighbours */
uint32_t FUN_00015400(void) {
    ENTER(-1, 0x00f8);                 /* movem D3-D7 */
    seg5_024cd0();
    int16_t room = G16(gCurRoom);
    int16_t i = 0, cell = 0, obj = 0;
    do {
        if (i < 0x1e) {
            obj = S16(GetOS(i));
        } else {
            if (i == 0x1e) {
                if (G16(-0x4fc0) != 0) {
                    room = G16(-0x4fc0);
                    CalcBlue(G16(-0x4fc0));
                } else {
                    i += 3;
                }
            }
            if (i == 0x21) {
                if (G16(-0x4fba) == 0) break;
                room = G16(-0x4fba);
                CalcBlue(G16(-0x4fba));
            }
            if (i < 0x21) {
                cell = (int16_t)(S16(FirstInRow((int16_t)(i - 0x1e))) + 9);
                obj = S16(GetOS(cell));
            } else {
                cell = (int16_t)(i - 0x21);
                obj = S16(GetOS(cell));
            }
        }
        switch ((uint16_t)obj) {
        case 0x0a:
            if (i < 0x21) TriggerFlask(room, cell);
            break;
        case 0x17:
            if (i < 0x21) TriggerLava(room, cell);
            break;
        case 0x13:
        case 0x20:
            TriggerTorch(room, cell);
            break;
        case 0x02:
            CheckSpikesJammed(room, cell);
            break;
        case 0x1e:
            TriggerStone(room, cell, 3);
            break;
        case 0x2c:
            TriggerBrgFloor(room, cell);
            break;
        case 0x2f:
            TriggerRoofPillar(room, cell);
            break;
        case 0x31:
            TriggerGlass(room, cell);
            break;
        case 0x30:
            TriggerRoofShip(room, cell);
            break;
        case 0x2b:
        case 0x1c:
        case 0x1d:
        case 0x1f:
        case 0x28:
            AddTrob(room, cell, 1, obj);
            break;
        default:
            if (room == 6 && LEVEL_TYPE() == 6 && (G16(-0x4818) & 0x1000)) {
                TriggerFinlCrystal(room, cell);
            } else if ((room == 7 || room == 8) && LEVEL_TYPE() == 6 && (G16(-0x4818) & 0x1000)) {
                TriggerFinlGargoyle(room, cell);
            }
            break;
        }
        i++;
        cell++;
    } while (i < 0x2b);
    CalcBlue(G16(gCurRoom));
    RETURN(0);
}

/* 00015624: count down the time left and show it when needed */
uint32_t KeepTime(void) {
    ENTER(0x28, 0x0018);                 /* movem D3-D4 */
    if (G16(-0x27d2) >= 0 && G16(-0x50b6) < 0 && G16(gMinutesLeft) != 0) {
        int16_t v = (int16_t)(G16(gFramesLeft) - 1);
        SG16(gFramesLeft, v);
        if (v == 0) {
            SG16(gFramesLeft, 0x2cf);
            v = (int16_t)(G16(gMinutesLeft) - 1);
            SG16(gMinutesLeft, v);
            if (v != 0) {
                int16_t m = G16(gMinutesLeft);
                if (m < 5 || m % 5 == 0) SG16(gShowTime, 1);
            }
        } else if (G16(gMinutesLeft) == 1) {
            if (GU16(gFramesLeft) % 12 == 0) {
                SG16(gShowTime, 1);
                SG16(gMsgId, 0);
                SG16(gMsgTimer, 0);
            }
        }
    }
    if (G16(gMsgTimer) != 0) RETURN(0);
    if (G16(gResumeFlag) != 0) {
        SG16(gResumeFlag, 0);
        RETURN(0);
    }
    if (G16(gShowTime) == 0) RETURN(0);
    maddr f = a6 - 0x28;
    int16_t m = G16(gMinutesLeft);
    if (m > 0) {
        if (m == 1) {
            int16_t secs = (int16_t)(((int32_t)GU16(gFramesLeft) + 1) / 12);
            if (secs == 1) tc_strcpy(f, GA(-0x55aa));
            else sprintf1(f, GA(-0x559c), (uint32_t)(int32_t)secs);
        } else {
            sprintf1(f, GA(-0x558c), (uint32_t)(int32_t)G16(gMinutesLeft));
        }
        DrawMessage(f);
    } else {
        DrawMessage(GA(-0x557c));
    }
    SG16(gMsgId, 0x18);
    SG16(gMsgTimer, 0x18);
    SG16(gShowTime, 0);
    RETURN(0);
}

/* 0001575c: switch to the next room (gNextRoom) if it changed; returns -2 */
uint32_t PrepCut(int16_t keepPalette) {
    ENTER(0x4, 0x0000);
    if (G16(gNextRoom) != 0 && G16(gNextRoom) != G16(gCurRoom)) {
        SG16(gCurRoom, G16(gNextRoom));
        seg3_019120();
        if (keepPalette == 0 || S16(FUN_00010518()) == 0) SaveGamePlayPalette();
        LoadCustomRoomData(G16(gCurRoom));
        SG16(-0x4fa4, 1);
        FUN_000225d2();
        FUN_00015400();
        tc_memset(GA(-0x4f88), 0, 4);
        PreloadRoomData(G16(gCurRoom));
        FUN_0002d1be();
        int16_t room = G16(gCurRoom);
        if (room != 0 && ROOM_OPPS(G16(gCurRoom)) != 0) {
            SG16(gOppIndex, GetOppIdxClosestToKid(1));
        } else {
            DrawOppMeter(-1, 0);
        }
    }
    RETURN(0xfffffffe);
}

/* 00015828: kid gains one strength point (max 12) */
uint32_t FUN_00015828(void) {
    ENTER(-1, 0x0018);                 /* movem D3-D4 */
    int32_t v = (int32_t)G16(-0x5036) + 1;
    int32_t m = v <= 12 ? v : 12;
    SG16(-0x5036, m);
    FUN_0001584c();
    RETURN(0);
}

/* 0001584c */
uint32_t FUN_0001584c(void) {
    ENTER(-1, 0x0000);
    SG16(-0x5034, G16(-0x5036) - G16(-0x5038));
    RETURN(0);
}

/* 0001585a: redraw the strength meters */
uint32_t seg2_01585a(void) {
    ENTER(-1, 0x0408);                 /* movem D3/A2 */
    if (G16(gKidHPDelta) != 0) DrawKidMeter(G16(gKidHP), G16(gKidMaxHP));
    if (G16(gKidHP) == 1) {
        if (G16(gFrameCount) & 1) DrawKidMeter(1, 0);
        else DrawKidMeter(0, 1);
    }
    int16_t idx = G16(gOppIndex);
    if (idx != -1) {
        maddr o = OPPS + (uint32_t)((int32_t)idx * OPP_SIZE);
        if (R16(o + 0x22) != 0 || R16(o + 0x1e) != G16(-0x4aa0)) {
            int16_t t = R16(o + 8);
            if (!((t == 7 || t == 8) && R16(o + 0x34) == 2)) DrawOppMeter(R16(o), R16(o + 0x1e));
        }
    }
    RETURN(0);
}

/* 00015906: apply pending strength changes of the kid and the opponents */
uint32_t ChangeMeters(void) {
    ENTER(0x8, 0x1cf8);                 /* movem D3-D7/A2-A4 */
    if (G16(gKidHPDelta) != 0) {
        if (G16(-0x50ca) == 1 && LEVEL_TYPE() == 6) seg4_01fbda();
        int32_t v = (int32_t)G16(gKidHP) + (int32_t)G16(gKidHPDelta);
        int32_t c = v >= 0 ? v : 0;
        int32_t mx = (int32_t)G16(gKidMaxHP);
        SG16(gKidHP, c >= mx ? mx : (v >= 0 ? v : 0));
    }
    int16_t room = G16(gCurRoom);
    int16_t n = room != 0 ? R16(gGameState + (uint32_t)(uint16_t)room * 0xc0 + 0x20e6) : 0;
    int16_t i = 0;
    if (n > 0) {
        maddr maxp = GA(-0x51e8), dp = GA(-0x51e6), hp = GA(-0x51ea);
        do {
            int16_t d = R16(dp);
            if (d != 0) {
                int32_t v = (int32_t)R16(hp) + (int32_t)d;
                int32_t c = v >= 0 ? v : 0;
                int32_t mx = (int32_t)R16(maxp);
                W16(hp, c >= mx ? mx : (v >= 0 ? v : 0));
            }
            i++;
            maxp += OPP_SIZE; dp += OPP_SIZE; hp += OPP_SIZE;
        } while (i < n);
    }
    RETURN(0);
}

/* 00015a02: input poll procedure: read keyboard and commands; returns the key */
uint32_t FUN_00015a02(void) {
    ENTER(-1, 0x0008);                 /* movem D3 */
    SG16(gNextRoom, 0);
    tc_memset(GA(gKeyX), 0, 6);
    FUN_00015a3e();
    int16_t key = S16(FUN_00014c16());
    if (G16(-0x524a) != 0) FUN_00015d20();
    if (key == 0) key = G16(gKeyBtn);
    RETURN((uint16_t)key);
}

/* 00015a3e */
uint32_t FUN_00015a3e(void) {
    ENTER(-1, 0x0000);
    ReadKeyboard();
    RETURN(0);
}

/* 00015a44: joystick emulation from the key map */
uint32_t ReadKeyboard(void) {
    ENTER(0x10, 0x0000);
    maddr f = a6 - 0x10;
    call_ptr(gGetKeysProc, "l", f);
    if ((R32(f + 4) & 0x8000) == 0) {       /* command key not down */
        if (S16(calll(IsUpKeyPressed, f)) != 0) SG16(gKeyY, -1);
        else if (S16(calll(IsDownKeyPressed, f)) != 0) SG16(gKeyY, 1);
        if (S16(calll(IsLeftKeyPressed, f)) != 0) SG16(gKeyX, -1);
        else if (S16(calll(IsRightKeyPressed, f)) != 0) SG16(gKeyX, 1);
        if ((R32(f + 4) & 1) != 0 || (R32(f + 8) & 0x2000000) != 0) SG16(gKeyBtn, -1);
        else if ((R32(f + 4) & 8) != 0 || (R32(f + 8) & 0x400) != 0) SG16(gKeyBtn, -2);
    }
    RETURN(0);
}

/* Shared shape of the four direction tests: two key-map masks that only count outside cheat
 * mode, then two masks (16-bit tests on longs 2 and 3) that always count. */
static uint16_t key_test(maddr km, uint32_t m0, uint32_t m1, uint16_t m2, uint16_t m3) {
    uint16_t r = (G16(gCheatMode) == 0 && (R32(km) & m0) != 0) ? 1 : 0;
    if (r != 0) return r;
    r = (G16(gCheatMode) == 0 && (R32(km + 4) & m1) != 0) ? 1 : 0;
    if (r != 0) return r;
    r = (uint16_t)R32(km + 8) & m2;
    if (r != 0) return r;
    return (uint16_t)R32(km + 0xc) & m3;
}

/* 00015b02 */
uint32_t IsUpKeyPressed(uint32_t km) {
    ENTER(0x0, 0x0408);                 /* movem D3/A2 */
    RETURN(key_test(km, 0xe00080, 0x5000000, 0x1a, 0x40));
}

/* 00015b86 */
uint32_t IsDownKeyPressed(uint32_t km) {
    ENTER(0x0, 0x0408);                 /* movem D3/A2 */
    RETURN(key_test(km, 0x84030000, 0xc90000, 0xb800, 0x20));
}

/* 00015c0e */
uint32_t IsLeftKeyPressed(uint32_t km) {
    ENTER(0x0, 0x0408);                 /* movem D3/A2 */
    RETURN(key_test(km, 0x82200000, 0x41400000, 0x4802, 0x08));
}

/* 00015c96 */
uint32_t IsRightKeyPressed(uint32_t km) {
    ENTER(0x0, 0x0408);                 /* movem D3/A2 */
    RETURN(key_test(km, 0x8820080, 0x20800000, 0x2011, 0x10));
}

/* 00015d20: pause until resumed (esc) */
uint32_t FUN_00015d20(void) {
    ENTER(-1, 0x0008);                 /* movem D3 */
    DrawMessage(GA(-0x551c));
    ResetSoundItem(0);
    SetGameMenuItemMark(1, G16(-0x524a));
    for (;;) {
        HandleEvent();
        if (G16(-0x524a) == 0) break;
        int16_t m = G16(-0x5352);
        if (m == 3 || m == 5) break;
    }
    SG16(-0x524a, 0);
    SetGameMenuItemMark(1, 0);
    ClrMessage(1);
    RETURN(0);
}

/* 00015d7a: copy a rectangle of the offscreen world to the window (mirrored when flipped) */
uint32_t PrinceCopy(uint32_t rect) {
    ENTER(0x8, 0x0c08);                 /* movem D3/A2-A3 */
    maddr f = a6 - 8;
    maddr src;
    if (G16(-0x5000) == -1 && G16(-0x4fa8) != 0) {
        src = f;
        W32(f, R32(rect)); W32(f + 4, R32(rect + 4));
        W16(f, (int16_t)(0x16d - R16(rect + 4)));
        W16(f + 4, (int16_t)(0x16d - R16(rect)));
        W16(rect, R16(rect) - 1);
        W16(rect + 4, R16(rect + 4) - 1);
        InvertOffWorld(gOffWorld, rect);
    } else {
        src = rect;
    }
    DrawSOffWorldToWindow(gOffWorld, gWindow, rect, src, 0);
    if (G16(-0x5000) == -1 && G16(-0x4fa8) != 0) {
        InvertOffWorld(gOffWorld, rect);
        W16(rect, R16(rect) + 1);
        W16(rect + 4, R16(rect + 4) + 1);
    }
    RETURN(0);
}

/* 00015e26: toggle the flipped screen */
uint32_t FUN_00015e26(void) {
    ENTER(-1, 0x0000);
    if (G16(-0x4fa8) != 0) {
        SG16(-0x4fa8, 0);
        AddSong(0xb1);
    } else {
        SG16(-0x4fa8, 0x438);
        AddSong(0xb0);
    }
    SG16(-0x5248, 1);
    RETURN(0);
}

/* 00015e56 */
uint32_t seg2_015e56(void) {
    ENTER(-1, 0x0000);
    SG16(-0x4fa6, 0xe4);
    AddSong(0x6f);
    PrinceResetSnd(G16(-0x439a));
    SG16(-0x439e, -1);
    RETURN(0);
}

/* 00015e78 */
uint32_t FUN_00015e78(void) {
    ENTER(-1, 0x0000);
    _SetPort(gWindow);
    SG16(gLevel, -1);
    RETURN(0);
}

/* 00015e86: start a new game at level `lvl` */
uint32_t Start(int16_t lvl) {
    ENTER(0x0, 0x0038);                 /* movem D3-D5 */
    FUN_00016112();
    SG16(-0x4f90, 0);
    SG16(gMsgId, 0);
    SG16(gMsgTimer, 0);
    SG16(gShowTime, 0);
    if (G16(gResumeFlag) == 0) {
        SG16(gMinutesLeft, 0x4b);
        SG16(gFramesLeft, 0x2cf);
        SG16(gStartMaxHP, 3);
        if (G16(gCheatMode) != 0) {
            int16_t l = LEVEL_NUM();
            if (l > 0 && l <= 0xe) {
                int32_t v = l > 3 ? (int32_t)l : 3;
                SG16(gStartMaxHP, v >= 12 ? 12 : v);
            }
        }
    }
    RETURN(callw(RestartGame, lvl));
}

/* 00015f1a: level loop: load and play levels until the game ends */
uint32_t RestartGame(int16_t lvl) {
    ENTER(0x0, 0x00f8);                 /* movem D3-D7 */
    if (lvl == 0 || lvl == -1 || lvl > 0xe || G16(gQuit) != 0) RETURN((uint16_t)lvl);
    for (;;) {
        int16_t full;
        if (G16(gResumeFlag) != 0) {
            full = 1;
        } else if (G16(-0x1b9a) != 0) {
            _EraseRect(gScreenRect);
            SG16(-0x1b9a, 0);
            if ((uint8_t)PackAllData(0x0100) != 0) {
                SG16(gDataPacked, 1);
                SetGameMenuItemMark(7, 1);
                _DisableItem(gGameMenu, 7);
            }
            full = 1;
            lvl = G16(gLevel);
            W16(sp0__, lvl);                /* the original keeps lvl in its argument slot */
        } else {
            full = S16(CheckPlayNIS(G16(gLevel), lvl));
            if (G16(gQuit) != 0) RETURN(0xffffffff);
            if (G16(-0x27d0) != 0) {
                int16_t saved = G16(gStartLevel);
                SG16(gStartLevel, lvl);
                lvl = S16(FUN_00014bea());
                W16(sp0__, lvl);
                SG16(gStartLevel, saved);
                full = 1;
            }
            if (lvl != G16(gLevel)) full = 1;
        }
        if (full != 0) {
            FUN_00016112();
            LoadLevel(lvl);
            SetMenusForGamePlayMode(1);
        } else {
            LoadLevelMinimum(G16(gLevel));
        }
        FUN_000171e0();
        seg4_023550();
        seg4_021940();
        FUN_0001723a();
        SG16(-0x43e6, 0);
        SG16(-0x4490, 0);
        SG16(gCurRoom, 0);
        SG16(-0x4f96, 0);
        SG16(-0x4fac, 0);
        SG16(-0x4fd4, 0);
        SG16(-0x439e, -1);
        SG16(-0x439c, -1);
        SG16(gKidHPDelta, 0);
        SG16(-0x5206, 0x56);
        FUN_00016124();
        seg2_016366();
        if (G16(-0x4ffe) != 0) {
            int16_t l = LEVEL_NUM();
            int32_t v = l > 3 ? (int32_t)l : 3;
            if (v >= 12) v = 12;
            SG16(gStartMaxHP, v);
            SG16(gKidHP, v);
            SG16(gKidMaxHP, v);
        }
        SG16(-0x5000, -1);
        lvl = S16(callw(FirstFrame, lvl));
        W16(sp0__, lvl);
        if (lvl != -1) {
            if (G16(-0x4ffe) != 0) StartPlayingRecordedGame();
            lvl = S16(FUN_0001652c());
            W16(sp0__, lvl);
            if (lvl != -1) FUN_00017cfe();
            if (G16(-0x4ffe) != 0) {
                seg5_02a314();
                SG16(gLevel, 0);
                lvl = 0;
                W16(sp0__, lvl);
                SG16(gNextLevel, 0);
            }
        }
        if (lvl == 0 || lvl == -1 || lvl > 0xe || G16(gQuit) != 0) break;
    }
    RETURN((uint16_t)lvl);
}

/* 00016112 */
uint32_t FUN_00016112(void) {
    ENTER(-1, 0x0000);
    FUN_0002a644();
    seg6_02d11e();
    FUN_000201b0();
    seg4_02417e();
    RETURN(0);
}

/* 00016124: place the kid at the level start position */
uint32_t FUN_00016124(void) {
    ENTER(-1, 0x0438);                 /* movem D3-D5/A2 */
    SG16(-0x504e, 0);
    if (LEVEL_NUM() == 7) FUN_0001630a();
    maddr gs = gGameState;
    int16_t v = R16(gs + 0x2198);
    SG16(-0x5040, v);
    SG16(gNextRoom, v);
    SG16(-0x504a, (int16_t)(R16(gs + 0x219a) % 10));
    SG16(-0x5048, (int16_t)(R16(gs + 0x219a) / 10));
    SG16(-0x5054, ~R16(gs + 0x219c));
    int16_t seq = S16(FUN_00011dea());
    SG16(-0x5052, R16(GA(-0x4b88) + (int16_t)(G16(-0x504a) * 2)) + 0x16);
    if (seq != -1) SG16(-0x5052, AddCharX(-10));
    v = G16(gStartMaxHP);
    SG16(-0x5038, v);
    SG16(-0x5036, v);
    int16_t anims = 0;
    if (seq == -1) {
        gs = gGameState;
        if (R16(gs + 0x2186) == 5 && R16(gs + 0x2198) == 4) {
            seq = 4;
            anims = 9;
            SG16(-0x5052, G16(-0x5052) - 8);
        } else if (LEVEL_TYPE() == 1) {
            seq = 0x7c;
        } else if (R16(gGameState + 0x2198) == 0x1b && R16(gGameState + 0x218a) == 6) {
            seq = 2;
            SG16(-0x5052, 0x176);
        } else if (R16(gGameState + 0x2198) == 0x16 && R16(gGameState + 0x218a) == 10) {
            seq = 2;
            SG16(-0x5052, 0x1ae);
        } else if (LEVEL_NUM() == 0xe && G16(-0x5040) == 1) {
            seq = 2;
            SG16(-0x5052, 0x1f2);
        } else {
            seq = 5;
        }
    }
    JumpSeq(seq);
    if (anims != 0) {
        do { AnimChar(); } while (--anims != 0);
    }
    seg2_016296();
    SG16(-0x5008, -1);
    SG16(-0x5004, 0);
    SG16(-0x5010, 1);
    RETURN(0);
}

/* 00016296: reset the kid's state and save it */
uint32_t seg2_016296(void) {
    ENTER(-1, 0x0018);                 /* movem D3-D4 */
    SG16(-0x5056, 10);
    SG16(-0x504e, 0);
    SG16(-0x503a, -1);
    SG16(-0x5024, 3);
    SG16(-0x5022, 0);
    SG16(-0x5020, 0);
    SG16(-0x501c, -1);
    FUN_00020cca();
    seg4_020fb2();
    SG16(-0x4f8e, 0);
    SG16(-0x4fa6, 0);
    SG16(-0x4fa0, 0);
    SG16(-0x4f9c, 0);
    if (G16(-0x500a) == -1) SG16(-0x503c, -1);
    else SG16(-0x503c, 0);
    AnimChar();
    save_kid();
    RETURN(0);
}

/* 0001630a: level 7 start position */
uint32_t FUN_0001630a(void) {
    ENTER(-1, 0x0408);                 /* movem D3/A2 */
    if (G16(-0x500c) == 2) {
        W16(gGameState + 0x2198, 4);
        W16(gGameState + 0x219a, 0x19);
        maddr gs = gGameState;
        W16(gs + 0x1668, RU16(gs + 0x1668) & 0xff00);
    } else {
        W16(gGameState + 0x2198, 0x20);
        W16(gGameState + 0x219a, 0x18);
        maddr gs = gGameState;
        W16(gs + 0x94c, RU16(gs + 0x94c) & 0xff00);
    }
    RETURN(0);
}

/* 00016366: close the exit door of the start room */
uint32_t seg2_016366(void) {
    ENTER(-1, 0x0418);                 /* movem D3-D4/A2 */
    int16_t room = G16(-0x50bc);
    maddr gs = gGameState;
    int16_t l;
    if (room == R16(gs + 0x2198) || (l = R16(gs + 0x218a)) == 7 || l == 6) {
        CalcBlue(room);
        uint16_t i = 0;
        do {
            int16_t cell = (int16_t)i;
            if (R16(G32(-0x4820) + (int32_t)cell * 2) == 0x11) {
                CloseExit(G16(-0x50bc), cell);
                break;
            }
            i++;
        } while (i < 0x1e);
    }
    RETURN(0);
}

/* 000163c6: first frame of a level */
uint32_t FirstFrame(int16_t lvl) {
    ENTER(0x0, 0x0018);                 /* movem D3-D4 */
    SG16(gNextRoom, G16(-0x50bc));
    int16_t r = S16(callw(PrepCut, lvl == -2 ? 0 : 1));
    if (r == -2) {
        if (S16(FUN_00010518()) == 0) PoPEraseRectInBothPorts(gScreenRect);
        else ClrMessage(1);
        if (LEVEL_TYPE() == 1 && S16(SoundIdOn(0x6dc5, 0)) != 0 && S16(WaitSndId(0x6dc5, 0)) == 0)
            Wait(0, 0x1e);
        SG16(gMsgId, 0);
        SG16(gMsgTimer, 0);
        DrawKidMeter(G16(gKidHP), G16(gKidMaxHP));
        FUN_000292a2();
        PlayAmbient(gAmbient);
        DoCleanCut();
        SetTimer(1, 0, 5);
    }
    RETURN((uint16_t)r);
}

/* 0001649e: redraw the whole room */
uint32_t DoCleanCut(void) {
    ENTER(0x8, 0x0008);                 /* movem D3 */
    SG16(-0x4fa4, 0);
    if (G16(-0x5246) != 0) {
        PoPEraseRectInBothPorts(gGameRect);
    } else {
        SG16(-0x4832, 1);
        SG16(-0x4fae, 1);
        FUN_0001910c();
        FUN_00018640();
        seg3_01a9a0();
        if (G32(-0x4c9e) != 0) UnloadCustomRoomShapes(0);
        SG16(-0x4832, 0);
        SG16(-0x5246, 0);
        FUN_00017d9e();
        FUN_0001a8be();
        seg3_01a9a0();
        _SystemTask();
        callw(CheckCurrentFreeMemory, 0);
        FUN_0002e81c();
        calll(PrinceCopy, gGameRect);
        FUN_0001c5a0();
    }
    SG16(-0x4fc8, 2);
    RETURN(0);
}

/* 0001652c: frame loop of a level; returns the next level (or -1) */
uint32_t FUN_0001652c(void) {
    ENTER(-1, 0x0018);                 /* movem D3-D4 */
    int16_t r;
    do {
        if (G16(-0x4fee) != 0) {
            ResetFrameVars();
            FUN_00011a00();
        }
        r = S16(FUN_00016654());
        if (G16(-0x4fee) != 0 && r == -2) {
            if (G16(-0x4fac) != 0) {
                r = S16(FUN_00017578());
            } else {
                int frame = 1;
                if (G16(gNextLevel) != G16(gLevel) && S16(FUN_00028ff6()) == 0) {
                    if (S16(PrinceSndOn(G16(-0x439a))) == 0) {
                        frame = 0;
                    } else {
                        int16_t s = G16(-0x439a);
                        if (s == 0x110 || s == 0x11 || s == 0xc0) frame = 0;
                    }
                }
                if (frame) {
                    r = S16(FrameAdv());
                    if (r == -2) {
                        if (((uint16_t)GetTimer(1) | GU16(-0x4fae)) != 0) {
                            if (G16(-0x5002) != 0 && G16(-0x4fae) == 0) SG16(-0x5002, G16(-0x5002) - 1);
                            SG16(-0x4fae, 0);
                            WaitTimerNoPoll(1);
                        } else if (G16(-0x5002) < 0x14 && G16(-0x5246) == 0 && S16(FUN_0002a4be()) == 0 &&
                                   S16(seg5_02a4da()) != 0) {
                            SG16(-0x5002, G16(-0x5002) + 1);
                        }
                    }
                } else {
                    int16_t l;
                    if (G16(gNextLevel) == -1 || ((l = LEVEL_NUM()) != 9 && l != 5)) ResetSoundItem(0);
                    SG16(gStartMaxHP, G16(gKidMaxHP));
                    r = G16(gNextLevel);
                    FUN_00011bce();
                }
            }
        }
        CheckCursorVisibility();
        CheckCheatModes();
    } while (r == -2 && G16(gQuit) == 0);
    SG16(-0x5000, 0);
    RETURN((uint16_t)r);
}

/* 00016654: one game frame (kid, opponents, objects, room change) */
uint32_t FUN_00016654(void) {
    ENTER(-1, 0x0008);                 /* movem D3 */
    if (G16(-0x4fee) != 0) {
        FUN_00026b68();
        AnimTrobs();
        if (R16(gGameState + 0x21a4) == 2 || G16(-0x5200) == 10) FUN_0001ef96();
        CheckOppGenPts(G16(gCurRoom));
        CheckAlert();
    }
    int16_t r = S16(FUN_00016716());
    if (G16(-0x4fee) != 0) {
        if (r == 0) {
            FUN_000168bc();
            if (G16(-0x4f90) == 0 && G16(gCurRoom) != 0) {
                seg6_02fdf2();
                CheckStab();
            }
            AddSFX();
            FUN_00016f76();
            FUN_00017554();
            ChangeMeters();
            FUN_0002e9c2();
            r = S16(callw(PrepCut, 0));
            if (r == -2) {
                CutOpponent();
                KeepTime();
                if (G16(gMinutesLeft) == 0) {
                    seg2_0175c4();
                    SG16(gStartLevel, 0);
                    r = -1;
                }
            }
        } else if (r != -1) {
            r = -2;
            FUN_000168bc();
            FUN_00017554();
            ChangeMeters();
            callw(PrepCut, 0);
        }
    }
    RETURN((uint16_t)r);
}

/* 00016716: move the kid */
uint32_t FUN_00016716(void) {
    ENTER(-1, 0x0078);                 /* movem D3-D6 */
    int16_t r;
    FUN_000225d2();
    if (S16(seg4_021258()) != 0) {
        r = S16(seg4_021658());
        seg4_021158();
    } else {
        int16_t opp = G16(-0x501c);
        int need = 1;
        if (G16(-0x501c) != -1) {
            int32_t off = (int32_t)G16(-0x501c) * OPP_SIZE;
            if (G16(-0x5054) != R16(GA(-0x5206) + (int16_t)off)) need = 0;
        }
        if (need) {
            opp = S16(GetOppIdxClosestToKid(1));
            if (opp == -1) opp = 0;
        }
        LoadShadAsOpp(opp);
        FUN_00020542();
        FUN_00018138();
        GetCharEdges();
        r = S16(seg4_021658());
        if (r != -1 && G16(-0x4fee) != 0) {
            if (G16(-0x4fac) != 0) {
                r = 1;
            } else {
                AnimChar();
                if (G16(-0x504c) != 0) {
                    FUN_00020eea();
                    seg4_020f34();
                    FUN_00020542();
                    FUN_00018138();
                    GetCharEdges();
                    FUN_00017094();
                    CheckBarr();
                    FUN_00022faa();
                    CheckGateForKid();
                    seg5_025382();
                    CheckPress();
                    switch (RU16(gGameState + 0x2186)) {
                    case 3:
                        seg5_027dc2();
                        if (G16(-0x4f90) == 0) {
                            CheckDartHit();
                            FUN_00027c78();
                            CheckLavaBoil();
                        }
                        break;
                    case 2:
                        seg5_027dc2();
                        seg10_03c9e0();
                        if (G16(-0x4f90) == 0) FUN_00027c78();
                        /* fall through */
                    case 4:
                        FUN_0002993c();
                        if (G16(-0x4f90) == 0) CheckScytheHit();
                        break;
                    }
                    FUN_0001704c();
                }
                r = 0;
            }
        }
        int16_t t = G16(-0x5008);
        if (t >= 0) {
            int16_t a;
            if (t == 0 && G16(-0x50b6) < 0 && (a = G16(-0x50c2)) != 3 && a != 4) {
                AddSound(0x6e);
                SG16(-0x5008, -2);
            } else {
                SG16(-0x5008, G16(-0x5008) - 1);
            }
        }
        if (G16(-0x4fee) != 0) {
            SG16(-0x356e, G16(-0x3560));
            SG16(-0x3570, G16(-0x3562));
            SG16(-0x3572, G16(-0x3568));
            SG16(-0x3574, G16(-0x356a));
        }
    }
    save_kid();
    RETURN((uint16_t)r);
}

/* 000168bc: move the opponents of the current room */
uint32_t FUN_000168bc(void) {
    ENTER(-1, 0x04e8);                 /* movem D3/D5-D7/A2 */
    if (G16(gCurRoom) == 0) RETURN(0);
    int16_t room = G16(gCurRoom);
    int16_t n = room != 0 ? R16(gGameState + (uint32_t)(uint16_t)room * 0xc0 + 0x20e6) : 0;
    if (n != 0) FUN_00020d42();
    int16_t i = 0;
    if (n > 0) {
        do {
            LoadShad(i);
            int16_t saveRoom;
            if (G16(-0x5054) == 0x56 || S16(seg4_021258()) != 0) goto skip;
            if (G16(-0x5050) > 0x1e4 &&
                R16(gGameState + (uint32_t)GU16(-0x5040) * 8 + 0x207e) == 0) goto skip;
            FUN_00020542();
            GetCharEdges();
            FUN_00018138();
            FUN_000217f4();
            saveRoom = G16(gCurRoom);
            {
                int16_t r = G16(-0x5040);
                if (r != G16(gCurRoom)) {
                    SG16(gCurRoom, r);
                    seg3_019120();
                }
            }
            AnimChar();
            if (G16(-0x504e) == 2 && G16(-0x504c) == 0xb9) FUN_0001db44();
            {
                int16_t x = G16(-0x5052);
                maddr gs;
                int16_t l;
                if (x >= 0x36 && x <= 0x365) {
                    if (G16(-0x5050) <= 0x2da) goto active;
                    if (R16(gGameState + (uint32_t)GU16(-0x5040) * 8 + 0x207e) != 0) goto active;
                }
                gs = gGameState;
                l = R16(gs + 0x218a);
                if ((l == 0xd || l == 0xb) && G16(-0x5050) < 0x2da) goto active;
                if (G16(-0x5048) != 0 && l == 5) {
                    int16_t c = G16(-0x5040);
                    if (c == 10 || c == 7 || c == 0xc) goto active;
                }
                {
                    int16_t c = G16(-0x5040);
                    if ((c == 7 || c == 8) && R16(gs + 0x2186) == 6) goto active;
                }
                goto done;
            }
        active:
            FUN_00020eea();
            seg4_020f34();
            FUN_00020542();
            FUN_00018138();
            GetCharEdges();
            if (G16(-0x5046) == 9) {
                SG16(-0x5048, GetRow(G16(-0x5050)));
            } else {
                EnemyCollide();
                seg4_023acc();
                seg5_025382();
                CheckPress();
                switch (RU16(gGameState + 0x2186)) {
                case 3:
                    seg5_027dc2();
                    CheckDartHit();
                    FUN_00027c78();
                    CheckLavaBoil();
                    break;
                case 2:
                    seg5_027dc2();
                    seg10_03c9e0();
                    FUN_00027c78();
                    /* fall through */
                case 4:
                    FUN_0002993c();
                    CheckScytheHit();
                    break;
                case 6:
                    FUN_0003a09e();
                    break;
                }
            }
        done:
            if (G16(-0x504e) == 0xc) seg4_01f834();
            if (G16(-0x5040) != saveRoom) {
                SG16(gCurRoom, saveRoom);
                seg3_019120();
            }
            goto next;
        skip:
            seg4_021158();
        next: {
                maddr gs = gGameState;
                if (R16(gs + 0x2186) == 6) {
                    int16_t c = G16(gCurRoom);
                    int32_t cnt = c != 0 ? (int32_t)R16(gs + (uint32_t)(uint16_t)c * 0xc0 + 0x20e6) : 0;
                    if (cnt != (int32_t)n) {
                        n--;
                        i--;
                        goto inc;
                    }
                }
                seg4_022516();
            }
        inc:
            i++;
        } while (i < n);
    }
    /* remove the dead opponents (state 0x56) */
    maddr o = OPPS;
    i = 0;
    if (n > 0) {
        do {
            if (R16(o + 2) == 0x56) {
                LoadShad(i);
                RemoveOppFromRoom(G16(gCurRoom), i);
                if (G16(gOppIndex) > G16(-0x5056)) {
                    SG16(gOppIndex, G16(gOppIndex) - 1);
                } else if (G16(gOppIndex) == G16(-0x5056)) {
                    SG16(gOppIndex, GetOppIdxClosestToKid(G16(-0x5054)));
                }
                i--;
                n--;
                o -= OPP_SIZE;
            }
            i++;
            o += OPP_SIZE;
        } while (i < n);
    }
    SG16(-0x4f9e, 0);
    RETURN(0);
}

/* 00016b7c: draw the frame, message and restart prompt */
uint32_t FrameAdv(void) {
    ENTER(0x8, 0x0038);                 /* movem D3-D5 */
    SG16(gFrameCount, G16(gFrameCount) + 1);
    int16_t r = -2;
    if (G16(-0x5244) != 0) {
        DoCleanCut();
        SG16(-0x5244, 0);
    } else if (G16(-0x4fa4) != 0) {
        SG16(gCurRoom, G16(gNextRoom));
        DoCleanCut();
    } else if (G16(-0x5248) != 0) {
        SG16(-0x5248, 0);
        LoadCustomRoomData(G16(gCurRoom));
        DoCleanCut();
        FUN_0001c0ac();
    } else {
        FUN_00017d9e();
        FUN_0001a8be();
        seg3_01a9a0();
        if (G16(-0x5246) != 0) _EraseRect(gGameRect);
        FUN_0001bb60();
        if (G16(-0x4fa8) != 0) {
            if (G16(-0x4fa8) == 1) FUN_00015e26();
            else if (G16(-0x50b6) < 0) SG16(-0x4fa8, G16(-0x4fa8) - 1);
        }
    }
    FUN_000292a2();
    PlayAmbient(gAmbient);
    if (G16(gMsgTimer) == 1) {
        int16_t m = G16(gMsgId);
        if (m == 0x24 || m == 0x258) {
            SG16(gStartLevel, 0);
            r = -1;
        } else {
            ClrMessage(1);
        }
    } else if (G16(gMsgTimer) != 0 && G16(gMsgId) != 0x4a4) {
        if (G16(-0x4fd4) != -1) SG16(gMsgTimer, G16(gMsgTimer) - 1);
        if (G16(gMsgId) == 0x258) {
            if (G16(gCurRoom) == 4 && LEVEL_NUM() == 0xd && G16(-0x4fd4) == 0) SG16(-0x4fd4, 1);
            uint16_t t = GU16(gMsgTimer);
            if (t < 0x78) {
                int16_t ph = (int16_t)(t % 12);
                if (ph > 3) {
                    ClrMessage(0);
                } else if (ph == 3) {
                    DrawRestartMessage();
                    PlaySound(-1, 0x2328, 1);
                }
            }
        }
    }
    RETURN((uint16_t)r);
}

/* 00016cf0: per-frame reset */
uint32_t ResetFrameVars(void) {
    ENTER(0xc, 0x1838);                 /* movem D3-D5/A3-A4 */
    int16_t t = G16(-0x50b8) == 1 ? 6 : 5;
    SetTimer(1, 0, t);
    SG16(gKidHPDelta, 0);
    if (G16(-0x509e) == 8 && (G16(-0x50ac) != 0x34 || G16(-0x50ae) != 0)) SG16(-0x509e, 0);
    int16_t room = G16(gCurRoom);
    int16_t n = room != 0 ? R16(gGameState + (uint32_t)(uint16_t)room * 0xc0 + 0x20e6) : 0;
    int16_t i = 0;
    if (n > 0) {
        maddr a3 = GA(-0x51d4), a4 = GA(-0x51e6);
        do {
            W16(a4, 0);
            if (R16(a3) == 8) W16(a3, 0);
            i++;
            a3 += OPP_SIZE;
            a4 += OPP_SIZE;
        } while (i < n);
    }
    FUN_0001707c();
    RETURN(0);
}

/* 00016dae: mark the floor cells under the kid to be redrawn */
uint32_t QuickFloor(void) {
    ENTER(0xc, 0x0078);                 /* movem D3-D6 */
    maddr f = a6 - 0xc;                          /* rect at A6-8 = f+4 */
    maddr rect = f + 4;
    int16_t action = G16(-0x5046);
    maddr src = GA(-0x502c);
    W32(rect, R32(src)); W32(rect + 4, R32(src + 4));
    if (G16(-0x521a) == 1) {
        if (G16(-0x5054) == 0) W16(rect + 6, R16(rect + 6) + G16(-0x5216));
        else W16(rect + 2, R16(rect + 2) - G16(-0x5216));
    }
    uint16_t fr = GU16(-0x504c);
    if (fr >= 0x87 && fr < 0x91 && G16(-0x5054) == 0) {
        MarkHalf(IndexCell(G16(-0x34a4), G16(-0x34a0)));
        RETURN(0);
    }
    int16_t a = G16(-0x5046);
    int16_t col;
    if (a == 2 || a == 3 || a == 4 || a == 6 || (a == 5 && G16(-0x34a0) != G16(-0x349e)) ||
        G16(-0x504c) == 0x4f || G16(-0x504c) == 0x51) {
        col = G16(-0x34a2);
        if (G16(-0x34a2) >= G16(-0x34a4)) {
            for (;;) {
                /* the original tests (action == 2 && action == 5) here, which is never true */
                (void)action;
                {
                    int32_t y = (int32_t)G16(-0x349e) * 0x78 + 0x6a;
                    if (y < (int32_t)R16(rect + 4))
                        MarkFloor(IndexCell(col, G16(-0x349e)), rect);
                }
                int16_t row = G16(-0x34a0);
                if (row != G16(-0x349e)) {
                    int16_t c;
                    if (!(G16(-0x502e) == 0x1c && ((c = G16(-0x5040)) == 7 || c == 8) && LEVEL_TYPE() == 6))
                        MarkFloor(IndexCell(col, row), rect);
                }
                col--;
                if (col < G16(-0x34a4)) break;
            }
        }
    } else {
        int16_t s = G16(-0x504e);
        if (s == 7 || s == 8) {
            s = G16(-0x502e);
            if (s == 0x89 || s == 0x8a || s == 0x8f || s <= 0x90 || s == 0x90) {
                col = G16(-0x34a2);
                if (G16(-0x34a2) >= G16(-0x34a4)) {
                    do {
                        MarkFloor(IndexCell(col, G16(-0x34a0)), rect);
                        col--;
                    } while (col >= G16(-0x34a4));
                }
            }
        }
    }
    RETURN(0);
}

/* 00016f76: remember a reached rebirth point */
uint32_t FUN_00016f76(void) {
    ENTER(-1, 0x0818);                 /* movem D3-D4/A3 */
    if (G16(-0x50bc) == 0 || G16(-0x50c2) == 4 || G16(-0x50b6) >= 0) RETURN(0);
    int16_t d = G16(gKidHPDelta);
    int32_t ad = d < 0 ? -(int32_t)d : (int32_t)d;
    if (ad >= (int32_t)G16(gKidHP)) RETURN(0);
    if (G16(-0x50ca) == 1) RETURN(0);
    if (G16(-0x50bc) == 9 && LEVEL_NUM() == 8 && G16(-0x500a) != 1) RETURN(0);
    maddr p = gGameState + 0x39a6;
    uint16_t i = 0;
    do {
        if (R16(p) == G16(-0x50bc)) {
            int32_t cell = (int32_t)S16(FirstInRow(G16(-0x50c4))) + (int32_t)G16(-0x50c6);
            if (cell == (int32_t)R16(p + 2)) {
                if (G32(-0x5018) == 0 || (int32_t)(int16_t)i + 1 != (int32_t)R16(G32(-0x5018) + 4))
                    SaveRebirthPtStatus((int16_t)(i + 1));
            }
        }
        i++;
        p += 4;
    } while (i < 2);
    RETURN(0);
}

/* 0001704c: pending screen shake */
uint32_t FUN_0001704c(void) {
    ENTER(-1, 0x0000);
    if (G16(-0x4f8e) != 0) {
        int16_t v = (int16_t)(G16(-0x5048) - (G16(-0x4f8e) >= 1 ? 1 : 0));
        ShakeEm(G16(-0x5040), v);
        SG16(-0x4f8e, 0);
    }
    RETURN(0);
}

/* 0001707c: count down two timers */
uint32_t FUN_0001707c(void) {
    ENTER(-1, 0x0000);
    if (G16(-0x4f90) > 0) SG16(-0x4f90, G16(-0x4f90) - 1);
    uint16_t v = GU16(-0x4fa6);
    if (v != 0) SG16(-0x4fa6, v - 1);
    RETURN(v);
}

/* 00017094: the kid bumps into an opponent */
uint32_t FUN_00017094(void) {
    ENTER(-1, 0x0038);                 /* movem D3-D5 */
    int16_t s = G16(-0x5092);
    if (s == 0x56) RETURN(0);
    if (G16(-0x5062) < 2) RETURN(0);
    if (G16(-0x503c) == 1) RETURN(0);
    if (G16(-0x5054) == s) RETURN(0);
    if (G16(-0x504e) == 1) RETURN(0);
    if (G16(-0x5042) >= 0x3b) RETURN(0);
    if (G16(-0x5084) >= 2) RETURN(0);
    s = G16(-0x508c);
    if (s == 6 || s == 0xb || s == 7 || s == 8) RETURN(0);
    if (s == 0xc) {
        uint16_t fr = GU16(-0x508a);
        if (fr >= 0xb3 && fr <= 0xeb) RETURN(0);
    }
    if (s == 4 && G16(-0x508a) == 0xce) RETURN(0);
    if (s == 2 && G16(-0x506c) == 0x6e) RETURN(0);
    if (G16(-0x503a) >= 0) RETURN(0);
    int16_t dist = S16(seg4_0224be());
    dist = (int16_t)(dist < 0 ? -(int32_t)dist : (int32_t)dist);
    if (dist > 0x18) {
        if (G16(-0x502e) != 4) RETURN(0);
        if (dist >= 0x3f) RETURN(0);
    }
    FUN_00020cca();
    SG16(-0x5042, 0);
    int16_t dx = -10;
    uint16_t fr = GU16(-0x504c);
    if (!(fr >= 0xf6 && fr <= 0x105)) {
        int16_t seq;
        if (fr == 0x18 || fr == 0x19 || (fr >= 0x28 && fr < 0x2b) || (fr >= 0x66 && fr <= 0x6a)) {
            seq = 0x2e;
            dx = 10;
        } else {
            seq = 0x2f;
        }
        JumpSeq(seq);
        AnimChar();
    }
    SG16(-0x5052, AddCharX(dx));
    RETURN(0);
}

/* 000171e0: reset the opponents of the level (unless restarting from a rebirth point) */
uint32_t FUN_000171e0(void) {
    ENTER(-1, 0x0418);                 /* movem D3-D4/A2 */
    uint32_t rp = G32(-0x5018);
    if (rp != 0) RETURN(rp);
    uint16_t room = 1;
    uint16_t n = RU16(gGameState + 0x2180);
    if (n < 1) RETURN(n);
    do {
        int16_t j = 0;
        uint32_t o;
        do {
            o = GetOppInfoIdx(room, j);
            j++;
            if (o != 0 && R16(o + 0x18) != 8) W16(o + 0xa, 0);
        } while (o != 0);
        room++;
        n = RU16(gGameState + 0x2180);
    } while (room <= n);
    RETURN(n);
}

/* 0001723a: per-level-type reset */
uint32_t FUN_0001723a(void) {
    ENTER(-1, 0x0008);                 /* movem D3 */
    if (LEVEL_TYPE() == 1) {
        SG16(-0x345e, 0);
    } else if (LEVEL_TYPE() == 3) {
        FUN_000376a4();
        if (LEVEL_NUM() == 5) seg8_037b8e();
    } else if (LEVEL_TYPE() == 4) {
        SG16(-0x4ff8, 0);
        SG16(-0x4ff6, 0);
        SG16(-0x4ff4, 0);
    } else if (LEVEL_TYPE() == 6) {
        SG16(-0x4ff2, 0);
    }
    RETURN(0);
}

/* 000172ae: opponents notice the kid (line of sight along the row) */
uint32_t CheckAlert(void) {
    ENTER(0xc, 0x00f8);                 /* movem D3-D7 */
    if (G16(-0x50bc) == 0) RETURN(0);
    int16_t frame = G16(-0x50c8);
    int16_t opp0 = S16(GetOppIdxClosestToKid(0));
    int16_t opp1 = S16(GetOppIdxClosestToKid(-1));
    int16_t room = G16(-0x50bc);
    int16_t n = room != 0 ? R16(gGameState + (uint32_t)(uint16_t)room * 0xc0 + 0x20e6) : 0;
    int16_t i = 0;
    if (n <= 0) RETURN(0);
    do {
        int32_t noFrame = frame == 0 ? 1 : 0;
        LoadShad(i);
        int zero = 0;
        if (noFrame != 0) zero = 1;
        else if ((uint16_t)frame >= 0xd9 && (uint16_t)frame < 0xe2) zero = 1;
        else if (G16(-0x50b6) >= 0) zero = 1;
        else if (G16(-0x503a) >= 0) zero = 1;
        else if (G16(-0x5054) == 0x56) zero = 1;
        else if (G16(-0x50c4) != G16(-0x5048) && G16(-0x504e) != 7 && G16(-0x504e) != 8) zero = 1;
        else if (G16(-0x504e) == 0xb && G16(-0x5022) == 6) zero = 1;
        else if (G16(-0x50ca) == 1 && LEVEL_TYPE() == 2) zero = 1;
        if (zero) {
            SG16(-0x5024, 0);
        } else {
            SG16(-0x5024, 3);
            int16_t col = G16(-0x50c6);
            if (G16(-0x50c2) == 6) col = (int16_t)(col + R16(GA(-0x4b94) + (int16_t)(G16(-0x50d0) * 2)));
            int16_t last = G16(-0x504a);
            if (col > G16(-0x504a)) {
                int16_t t = col;
                col = G16(-0x504a);
                last = t;
            }
            int16_t cell = S16(ReadCell(G16(-0x50bc), last, G16(-0x50c4)));
            if (cell == 4) last--;
            if (last > col && !(col > last) && G16(-0x5024) == 3) {
                do {
                    cell = S16(ReadCell(G16(-0x50bc), col, G16(-0x50c4)));
                    if (S16(IsWall(cell)) != 0 &&
                        !(G16(-0x504e) == 0xb && cell == 7 && (G16(-0x34ae) & 0x80) != 0)) {
                        SG16(-0x5024, 0);
                        goto inc;
                    }
                    if (cell == 4 && (uint16_t)(G16(-0x34ae) & 0xff) < 0x68) goto block;
                    if (S16(IsImpassable(cell)) != 0 && cell != 0x17 && G16(-0x504e) != 10) goto block;
                    if (S16(IsSpace(cell)) == 0) goto inc;
                    {
                        int16_t s = G16(-0x504e);
                        if (s == 10 || s == 7 || s == 8) goto inc;
                    }
                block:
                    if (LEVEL_TYPE() == 3 && cell == 0x18 && G16(-0x50c6) >= G16(-0x34a8) &&
                        G16(-0x504a) >= G16(-0x34a8)) goto inc;
                    if (G16(-0x504e) == 0xb && (cell == 0xc || cell == 0xd)) goto inc;
                    SG16(-0x5024, 1);
                inc:
                    col++;
                } while (col <= last && G16(-0x5024) == 3);
            }
            if (G16(-0x5024) == 3 && G16(-0x504e) != 0xb) {
                if (i != opp1 && i != opp0) SG16(-0x5024, 2);
                else if (G16(-0x504e) == 10) SG16(-0x503c, 1);
            }
        }
        seg4_022516();
        i++;
    } while (i < n);
    RETURN(0);
}

/* 00017554: per-level-type frame hook */
uint32_t FUN_00017554(void) {
    ENTER(-1, 0x0400);                 /* movem A2 */
    maddr fn = R32(GA(-0x46f8) + (int16_t)(RU16(gGameState + 0x2186) * 4));
    if (fn != 0) call_ptr(fn, "");
    RETURN(0);
}

/* 00017578: end of level: returns the next level */
uint32_t FUN_00017578(void) {
    ENTER(-1, 0x0008);                 /* movem D3 */
    int16_t r;
    SG16(-0x4fac, 0);
    if (G16(gNextLevel) == G16(gLevel)) {
        SaveGamePlayPalette();
        call_ptr(G32(-0x1f64), "l", gScreenRect);
        _EraseRect(gScreenRect);
        ResetSoundItem(0);
        r = G16(gStartLevel) != 0 ? G16(gLevel) : 0;
    } else {
        r = G16(gNextLevel);
    }
    seg5_029374();
    RETURN((uint16_t)r);
}

/* 000175c4: time is up */
uint32_t seg2_0175c4(void) {
    ENTER(-1, 0x0008);                 /* movem D3 */
    ResetSoundItem(0);
    int16_t r = S16(PlayNIS(0x1c));
    SG16(gMinutesLeft, 0);
    SG16(-0x27d2, -1);
    if (r == 2) FUN_00016112();
    RETURN(0);
}

/* 000175f4: make sure some memory is free (purge background shapes if needed) */
uint32_t CheckCurrentFreeMemory(int16_t onlyCompact) {
    ENTER(0x4, 0x0018);                 /* movem D3-D4 */
    uint32_t grow;
    if (onlyCompact != 0) {
        max_mem(&grow);
        RETURN(0);
    }
    int32_t fr = S32(_FreeMem());
    if (fr >= 0x1f400) RETURN(0);
    fr = S32(max_mem(&grow));
    if (fr >= 0x1f400) RETURN(0);
    if (G16(-0x46fa) == 1) RETURN(0);
    MarkBGSetPurgable(1);
    _ReserveMem(0x1f400);
    RETURN(0);
}
