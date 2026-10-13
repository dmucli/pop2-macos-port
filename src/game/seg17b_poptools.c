/* CODE 17 "PoP Tools", second half (0x51ee8-0x554d0): LZSS unpacker and the encoder's
 * tree, the "pack all data" utility and its progress window, offscreen worlds, saved
 * screen rectangles, the shape (SHAP) blitters, shape lists (SHPL), colour tables,
 * alerts and small helpers (Rnd, pstrcpy ...).
 * Ported from the original 68k code; addresses in comments are Ghidra addresses
 * (see docs/PORTING.md). Notes: docs/notes/seg17b.md. */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* ---- globals (Ghidra addresses) ---- */
#define gLZBuf          GH(0x106472)   /* long: LZSS ring buffer (text_buf) */
#define gLZRson         GH(0x106476)   /* long: encoder tree, right children (word array) */
#define gLZLson         GH(0x10647a)   /* long: encoder tree, left children (word array) */
#define gLZDad          GH(0x10647e)   /* long: encoder tree, parents (word array) */
#define gLZMatchLen     GH(0x10646e)   /* word: longest match found by InsertNode */
#define gLZMatchPos     GH(0x106470)   /* word: its position */
#define gStopPacking    GH(0x106468)   /* word: user asked to stop packing (escape) */
#define gPackWin        GH(0x10646a)   /* long: packing progress window */
#define gFileBarRect    GH(0x106328)   /* Rect: progress bar 0 (resources of the file) */
#define gFolderBarRect  GH(0x106330)   /* Rect: progress bar 1 (files of the folder) */
#define gBar0Cur        GH(0x106464)   /* word: bar 0 value */
#define gBar0Max        GH(0x106462)   /* word: bar 0 maximum (also "packing in progress") */
#define gBar1Cur        GH(0x106460)   /* word: bar 1 value */
#define gBar1Max        GH(0x10645e)   /* word: bar 1 maximum */
#define gBarRight       GH(0x10632e)   /* word: right edge of the file name box */
#define gPackFileName   GH(0x10635e)   /* Str: name of the file being packed */
#define gPackNamePrefix GH(0x106342)   /* long: Str shown before the file name */
#define gPackNameSuffix GH(0x10634c)   /* long: Str shown after the file name */
#define gDataFolderName GH(0x10635a)   /* long: Str, name of the data folder */
#define gShapeFileType  GH(0x106350)   /* OSType of the files to pack */
#define gEstPackTime    GH(0x102ce4)   /* word: cached estimate of the packing time (minutes) */
#define gBGSetFileNames GH(0x1038de)   /* 7 entries of 6 bytes: +0 Str pointer of a BG set file */
#define gBGSetState     GH(0x103906)   /* word: 0 loaded, 1 purgeable, 2 shapes missing */
#define gGameState      GH(0x102fec)   /* long: pointer to the game state block */
#define gEvtWhat        GH(0x102cae)   /* EventRecord.what */
#define gEvtMsgLo       GH(0x102cb2)   /* low word of EventRecord.message */
#define gEvtModifiers   GH(0x102cbc)   /* EventRecord.modifiers */
#define gFgRGB          GH(0x106482)   /* RGBColor used as foreground for CopyBits */
#define gBgRGB          GH(0x106488)   /* RGBColor used as background for CopyBits */
#define gFlipTablesOK   GH(0x10648e)   /* long: 0x0f0f0f0f once the flip tables are built */
#define gBitRevTable    GH(0x106492)   /* 256 bytes: bit-reversed byte (1-bit shapes) */
#define gNibSwapTable   GH(0x106592)   /* 256 bytes: nibble-swapped byte (4-bit shapes) */
#define pUnionRect      GH(0x1060a4)   /* long: UnionRect trap address */
#define pSectRect       GH(0x1060a8)   /* long: SectRect trap address */
#define gEmptyStr       GH(0x106210)   /* empty Pascal string */
#define gRndSeeded      GH(0x106692)   /* word: randSeed initialised */
#define qdRandSeed      GH(0x106704)   /* long: QuickDraw randSeed */
#define qdThePort       GH(0x106782)   /* long: QuickDraw thePort */
#define lmMemErr        0x220          /* low memory MemErr */
#define lmTime          0x20c          /* low memory Time */

#define SHAP 0x53484150u
#define SHPL 0x5348504cu
#define CTBL 0x4354424cu

/* Constants in the code segment (strings), addressed like the original PC-relative lea. */
static maddr cs(int fn, uint32_t entry, uint32_t ghidra_addr) { return fn_address(fn) + (ghidra_addr - entry); }

/* SwapMMUMode with the mode in D0.b; returns the previous mode (D0) */
static uint32_t swap_mmu(uint8_t mode) {
    cpu_set(R_D0, (cpu_get(R_D0) & 0xffffff00u) | mode);
    return _SwapMMUMode();
}

/* Pascal call through a cached trap address with three pointer arguments (pushed in
 * order a, b, c; the callee pops them). With slot: a Boolean result slot is reserved
 * first (not written, like subq.w #2,SP) and its byte returned. */
static uint32_t pascal_ptr3(maddr fn, uint32_t a, uint32_t b, uint32_t c, int slot) {
    maddr sp0 = emu_sp(), sp = sp0 - (slot ? 2 : 0) - 12;
    W32(sp + 8, a); W32(sp + 4, b); W32(sp, c);
    emu_set_sp(sp);
    uint32_t r = call68_addr(fn);
    if (slot) r = R8(sp0 - 2);
    emu_set_sp(sp0);
    return r;
}

/* ======================================================================== LZSS == */

/* 00051ee8: unpack an LZSS-packed resource handle in place (header: +0 flags word, bit
 * 0x400 = packed, +0xc unpacked size, data at +0x10 of the packed copy). Returns 1. */
uint32_t UnpackLZ(uint32_t h) {
    ENTRY_LINK(FN_UnpackLZ, 0x18, 32);          /* link -0x18; movem D3-D7/A2-A4 */
    W32(a6 - 0xc, h);
    if (tc_HandToHand(a6 - 0xc) != 0) return 0;  /* clr.b D0 ("ret": 1) */
    int32_t size = (int32_t)R32(R32(h) + 0xc);
    _SetHandleSize(h, (uint32_t)size);
    _HLock(h);
    _HLock(R32(a6 - 0xc));
    W32(gLZBuf, PoPNewPtr(0x442));
    /* "move.l A4,D0; bne": A4 is the caller's register here (the shape handle in
     * GetShape -> UnpackShapeLZ) */
    if (R32(gLZBuf) == 0 || cpu_get(R_A4) == 0) {
        _HUnlock(R32(a6 - 0xc));
        _HUnlock(h);
        return 0;
    }
    uint32_t src = R32(R32(a6 - 0xc)) + 0x10;
    uint32_t dst = R32(h);
    int32_t end = (int32_t)(dst + (uint32_t)size);
    dst += 0xc;
    tc_memset(R32(gLZBuf), 0, 0x3be);
    uint16_t r = 0x3be, flags = 0;
    for (;;) {
        flags >>= 1;
        if ((flags & 0x100) == 0) { flags = (uint16_t)(R8(src) | 0xff00); src++; }
        if (flags & 1) {
            W8(dst, R8(src)); dst++;
            if ((int32_t)dst >= end) break;
            uint8_t c = R8(src);
            W8(R32(gLZBuf) + r, c); r++;
            src++;
            r &= 0x3ff;
        } else {
            uint16_t hi = R8(src); src++;
            uint16_t lo = R8(src); src++;
            uint16_t pos = (uint16_t)((uint16_t)((uint16_t)(hi << 8) | lo) << 6) >> 6;
            uint16_t len = (uint16_t)((hi >> 2) + 3);
            int done = 0;
            for (uint16_t k = 0; k < len; k++) {
                uint8_t c = R8(R32(gLZBuf) + (((uint32_t)pos + k) & 0x3ff));
                W8(dst, c); dst++;
                if ((int32_t)dst >= end) { done = 1; break; }
                W8(R32(gLZBuf) + r, c); r++;
                r &= 0x3ff;
            }
            if (done) break;
        }
    }
    if (R32(gLZBuf) != 0) _DisposePtr(R32(gLZBuf));
    _HUnlock(h);
    _HUnlock(R32(a6 - 0xc));
    _HPurge(R32(a6 - 0xc));
    W32(a6 - 0xc, 0);
    uint32_t p = R32(h);
    W16(p, RU16(p) & 0xfbff);
    return 1;
}

#define RSON(i) (R32(gLZRson) + (uint32_t)((int32_t)(int16_t)(i) * 2))
#define LSON(i) (R32(gLZLson) + (uint32_t)((int32_t)(int16_t)(i) * 2))
#define DAD(i)  (R32(gLZDad)  + (uint32_t)((int32_t)(int16_t)(i) * 2))

/* 000520c0: insert string r into the encoder's binary tree; finds the longest match */
uint32_t InsertNode(int16_t r) {
    int16_t cmp = 1;
    uint32_t key = R32(gLZBuf) + (uint32_t)(int32_t)r;
    int16_t p = (int16_t)(R8(key) + 0x401);
    W16(LSON(r), 0x400);
    W16(RSON(r), 0x400);
    W16(gLZMatchLen, 0);
    for (;;) {
        if (cmp >= 0) {
            int16_t q = R16(RSON(p));
            if (q == 0x400) { W16(RSON(p), r); W16(DAD(r), p); return 0; }
            p = q;
        } else {
            int16_t q = R16(LSON(p));
            if (q == 0x400) { W16(LSON(p), r); W16(DAD(r), p); return 0; }
            p = q;
        }
        uint16_t i = 1;
        for (;;) {
            cmp = (int16_t)(R8(key + (uint32_t)(int32_t)(int16_t)i) -
                            R8(R32(gLZBuf) + (uint32_t)(int32_t)p + (uint32_t)(int32_t)(int16_t)i));
            if (cmp != 0) break;
            i++;
            if (i >= 0x42) break;
        }
        if ((int32_t)(int16_t)i <= (int32_t)RU16(gLZMatchLen)) continue;
        W16(gLZMatchPos, p);
        W16(gLZMatchLen, i);
        if (i < 0x42) continue;
        W16(DAD(r), R16(DAD(p)));
        W16(LSON(r), R16(LSON(p)));
        W16(RSON(r), R16(RSON(p)));
        W16(DAD(R16(LSON(p))), r);
        W16(DAD(R16(RSON(p))), r);
        uint32_t a = RSON(R16(DAD(p)));
        if (R16(a) == p) W16(a, r);
        else W16(LSON(R16(DAD(p))), r);
        W16(DAD(p), 0x400);
        return 0;
    }
}

/* 000522ca: remove node p from the encoder's tree */
uint32_t DeleteNode(int16_t p) {
    int16_t q;
    if (R16(DAD(p)) == 0x400) return 0;
    if (R16(RSON(p)) == 0x400) q = R16(LSON(p));
    else if (R16(LSON(p)) == 0x400) q = R16(RSON(p));
    else {
        q = R16(LSON(p));
        if (R16(RSON(q)) != 0x400) {
            do q = R16(RSON(q)); while (R16(RSON(q)) != 0x400);
            W16(RSON(R16(DAD(q))), R16(LSON(q)));
            W16(DAD(R16(LSON(q))), R16(DAD(q)));
            W16(LSON(q), R16(LSON(p)));
            W16(DAD(R16(LSON(p))), q);
        }
        W16(RSON(q), R16(RSON(p)));
        W16(DAD(R16(RSON(p))), q);
    }
    W16(DAD(q), R16(DAD(p)));
    uint32_t a = RSON(R16(DAD(p)));
    if (R16(a) == p) W16(a, q);
    else W16(LSON(R16(DAD(p))), q);
    W16(DAD(p), 0x400);
    return 0;
}

/* ================================================================== packing == */

/* 000524dc: pack every unpacked SHAP resource (> 30 bytes) of one resource file */
uint32_t LZPackDataFile(uint32_t path) {
    ENTRY_LINK(FN_LZPackDataFile, 0x104, 24);   /* link -0x104; movem D3-D6/A3-A4 */
    W8(a6 - 0x104, 0);
    pstrcpy(a6 - 0x100, path);
    int16_t rn = (int16_t)_OpenResFile(a6 - 0x100);
    if (rn > 0) {
        int16_t cur = (int16_t)_CurResFile();
        _UseResFile(rn);
        int16_t n = (int16_t)_Count1Resources(SHAP);
        int16_t i = 1;
        if (i <= n && R16(gStopPacking) == 0) {
            do {
                uint32_t h = _GetIndResource(SHAP, i);
                _HLock(h);
                if ((RU16(R32(h)) & 0x400) == 0 && (int32_t)tc_GetHandleSize(h) > 0x1e &&
                    (PackShapeLZ(h) & 0xff) != 0)
                    PoPWriteResource(h, cs(FN_LZPackDataFile, 0x524dc, 0x525ba)); /* "Error writing resource in LZPackDataFile()." */
                _HUnlock(h);
                _HPurge(h);
                DrawPackingProgress(0, i, n);
                HandleEvent();
                i++;
            } while (i <= n && R16(gStopPacking) == 0);
        }
        _CloseResFile(rn);
        _UseResFile(cur);
        W8(a6 - 0x104, 1);
    }
    return R8(a6 - 0x104);
}

/* 000525e6: pack the SHAP resources of every file of the data folder, with a progress
 * window; the char argument says whether the game was running (unload/reload level) */
uint32_t PackAllData(int16_t fromGame) {
    ENTRY_LINK(FN_PackAllData, 0x2c0, 32);      /* link -0x2c0; movem D3-D7/A2-A4 */
    const uint32_t E = 0x525e6;
    uint8_t flag = (uint8_t)((uint16_t)fromGame >> 8);   /* move.b (8,A6) */
    W16(gStopPacking, 0);
    int16_t err = 0;
    _GetPort(a6 - 0x20);
    FUN_00052fca();
    uint16_t t = (uint16_t)GetEstTimeToPackData();
    uint16_t hours = (uint16_t)(t / 0x3c);
    uint16_t mins = (uint16_t)(t % 0x3c);
    if (hours >= 1) {
        call_ptr(JT_ADDR(FN_tc_sprintf), "lll", a6 - 0x1bc, cs(FN_PackAllData, E, 0x528da), (uint32_t)hours); /* "%d hour" */
        if (hours > 1) tc_strcat(a6 - 0x1bc, cs(FN_PackAllData, E, 0x528e2));   /* "s" */
        tc_strcat(a6 - 0x1bc, cs(FN_PackAllData, E, 0x528e4));                  /* " " */
    } else W8(a6 - 0x1bc, 0);
    if (mins >= 1) {
        call_ptr(JT_ADDR(FN_tc_sprintf), "lll", a6 - 0x1a8, cs(FN_PackAllData, E, 0x528e6), (uint32_t)mins); /* "%d minute" */
        if (mins > 1) tc_strcat(a6 - 0x1a8, cs(FN_PackAllData, E, 0x528f0));   /* "s" */
    } else W8(a6 - 0x1a8, 0);
    call_ptr(JT_ADDR(FN_tc_sprintf), "llll", a6 - 0x2bc, cs(FN_PackAllData, E, 0x528f2), a6 - 0x1bc, a6 - 0x1a8);
    if (R16(gStopPacking) == 0 && S16(AlertYesNo(a6 - 0x2bc)) != 0) {
        HandleEvent();
        if (flag) W16(a6 - 0x14, FUN_00053074());
        W32(a6 - 0x1c, _TickCount());
        const maddr pb = a6 - 0x194;             /* CInfoPBRec */
        W16(pb + 0x1c, 0);
        W32(pb + 0xc, 0);
        pstrcpy(a6 - 0x128, R32(gDataFolderName));
        W32(pb + 0x12, a6 - 0x128);
        W32(pb + 0x30, 0);
        W16(pb + 0x16, 0);
        tc_PBGetCatInfo(0, pb);
        err = R16(pb + 0x10);
        if (err == 0 && (R8(pb + 0x1e) & 0x10) != 0) {        /* a directory */
            W16(a6 - 0x28, R16(pb + 0x34));                    /* number of files */
            W32(a6 - 0x24, R32(pb + 0x30));                    /* its dirID */
            W16(pb + 0x1c, 1);
            W32(a6 - 0x10, pb + 0x30);
            W32(a6 - 0xc, pb + 0x10);
            W32(a6 - 0x8, pb + 0x20);
            W32(a6 - 0x4, pb + 0x1c);
            for (;;) {
                DrawEmptyProgressBar(gFileBarRect);
                W32(R32(a6 - 0x10), R32(a6 - 0x24));
                tc_PBGetCatInfo(0, pb);
                err = R16(R32(a6 - 0xc));
                if (err == 0 && (R8(pb + 0x1e) & 0x10) == 0 &&
                    (int32_t)tc_memcmp(R32(a6 - 0x8), gShapeFileType, 4) == 0 &&
                    S16(IsBGSetFile(a6 - 0x128)) == 0) {
                    SetPackingWindowFilename(a6 - 0x128);
                    pstrinsert(a6 - 0x128, cs(FN_PackAllData, E, 0x52938));
                    pstrinsert(a6 - 0x128, R32(gDataFolderName));
                    pstrinsert(a6 - 0x128, cs(FN_PackAllData, E, 0x5293c));
                    LZPackDataFile(a6 - 0x128);
                }
                maddr idx = R32(a6 - 0x4);
                DrawPackingProgress(1, R16(idx), R16(a6 - 0x28));
                W16(idx, R16(idx) + 1);
                if (err != 0 || R16(gStopPacking) != 0) break;
            }
        } else if (err == -0x2b) {
            AlertOKOnly(cs(FN_PackAllData, E, 0x52940));
        }
        if (err != 0 && err != -0x2b) {
            call_ptr(JT_ADDR(FN_tc_sprintf), "lll", a6 - 0x2bc, cs(FN_PackAllData, E, 0x52978), (uint32_t)(int32_t)err);
            AlertOKOnly(a6 - 0x2bc);
        }
        uint8_t ok;
        if (R16(gStopPacking) != 0) ok = 0;
        else if (err == 0 || err == -0x2b) ok = 1;
        else ok = 0;
        W8(a6 - 0x2c0, ok);
        if (flag) GoBackToGameAfterPacking(R16(a6 - 0x14));
    }
    _DisposeWindow(R32(gPackWin));
    _SetPort(R32(a6 - 0x20));
    W16(gBar0Max, 0);
    return R8(a6 - 0x2c0);                      /* uninitialised when the alert is declined */
}

/* 000529a0: ask, then pack all data at startup */
uint32_t PackDataAtStartUp(void) {
    ENTRY_LINK(FN_PackDataAtStartUp, 4, 0);
    if (S16(AlertYesNo(cs(FN_PackDataAtStartUp, 0x529a0, 0x529e2))) != 0)
        W8(a6 - 4, PackAllData(0));
    else
        W8(a6 - 4, 0);
    return R8(a6 - 4);
}

/* 00052a7a: ask whether to pack all data (from the game menu) */
uint32_t CheckPackDataFromGame(void) {
    ENTRY_LINK(FN_CheckPackDataFromGame, 4, 0);
    return AlertYesNo(cs(FN_CheckPackDataFromGame, 0x52a7a, 0x52aa4));
}

/* 00052b0e: frame a progress bar and fill it with the gray pattern */
uint32_t DrawEmptyProgressBar(uint32_t rect) {
    ENTRY_LINK(FN_DrawEmptyProgressBar, 0x10, 4);
    FUN_0004fbe2();
    SetForeColor(0xff);
    seg20_05b9e0(4, 0, a6 - 0x10);             /* GetIndPattern(&pat, 0, 4) */
    W32(a6 - 8, R32(rect));
    W32(a6 - 4, R32(rect + 4));
    _InsetRect(a6 - 8, -1, -1);
    _FrameRect(a6 - 8);
    _FillRect(rect, a6 - 0x10);
    seg17_04fc46();
    return 0;
}

/* 00052b7a: draw progress bar `which` at cur/max; cur == -1 redraws the stored values
 * (which are then written into the argument slots) */
static uint32_t DrawPackingProgress_(int16_t which, int16_t *cur, int16_t *max) {
    ENTRY_LINK(FN_DrawPackingProgress, 8, 8);
    if (which == 0) {
        W32(a6 - 8, R32(gFileBarRect)); W32(a6 - 4, R32(gFileBarRect + 4));
        if (*cur != -1) { W16(gBar0Cur, *cur); W16(gBar0Max, *max); }
        else { *cur = R16(gBar0Cur); *max = R16(gBar0Max); }
    } else {
        W32(a6 - 8, R32(gFolderBarRect)); W32(a6 - 4, R32(gFolderBarRect + 4));
        if (*cur != -1) { W16(gBar1Cur, *cur); W16(gBar1Max, *max); }
        else { *cur = R16(gBar1Cur); *max = R16(gBar1Max); }
    }
    int32_t w = (int32_t)R16(a6 - 2) - (int32_t)R16(a6 - 6);
    w = (int32_t)((uint32_t)w * (uint32_t)(int32_t)*cur);
    if (*max == 0) port_divide_by_zero(__func__);
    w = w / (int32_t)*max;
    if (w < 0) w = 0;
    int16_t right = (int16_t)(R16(a6 - 6) + (int16_t)w);
    if (right < R16(a6 - 2)) W16(a6 - 2, right);
    FUN_0004fbe2();
    SetForeColor(0xff);
    _PaintRect(a6 - 8);
    seg17_04fc46();
    return 0;
}
uint32_t DrawPackingProgress(int16_t which, int16_t cur, int16_t max) {
    return DrawPackingProgress_(which, &cur, &max);
}
void glue_DrawPackingProgress(void) {
    maddr sp = emu_sp();
    int16_t cur = R16(sp + 2), max = R16(sp + 4);
    uint32_t r = DrawPackingProgress_(R16(sp), &cur, &max);
    W16(sp + 2, cur);                          /* move.w ...,(0xa,A6) */
    W16(sp + 4, max);                          /* move.w ...,(0xc,A6) */
    emu_set_d0(r);
}

/* 00052c60: redraw the packing window's texts and empty bars */
uint32_t FUN_00052c60(void) {
    ENTRY_NOLINK(FN_FUN_00052c60, 0);
    if (R32(gPackWin) == 0) return 0;
    _EraseRect(R32(gPackWin) + 0x10);
    _MoveTo(9, 0xf);
    _DrawString(cs(FN_FUN_00052c60, 0x52c60, 0x52caa));    /* "Press escape to stop." */
    _MoveTo(9, 0x48);
    _DrawString(cs(FN_FUN_00052c60, 0x52c60, 0x52cc2));    /* "Folder progress..." */
    DrawEmptyProgressBar(gFileBarRect);
    DrawEmptyProgressBar(gFolderBarRect);
    return 0;
}

/* 00052cd6: time the packing of one test shape and estimate the total (cached) */
uint32_t GetEstTimeToPackData(void) {
    ENTRY_LINK(FN_GetEstTimeToPackData, 0x208, 24);
    uint16_t est;
    if (R16(gEstPackTime) != 0) {
        est = RU16(gEstPackTime);
        return est;
    }
    _MoveTo(9, 0xf);
    _DrawString(cs(FN_GetEstTimeToPackData, 0x52cd6, 0x52df0));   /* "One moment please..." */
    W16(a6 - 0x208, 0);
    W32(a6 - 0x206, a6 - 0x200);
    pstrcpy(a6 - 0x200, cs(FN_GetEstTimeToPackData, 0x52cd6, 0x52e06));
    OpenRsrcVerify(a6 - 0x208);
    int16_t cur = (int16_t)_CurResFile();
    _UseResFile(R16(a6 - 0x208));
    uint32_t t0 = _TickCount();
    uint32_t h = _GetResource(SHAP, 0x7d01);
    if (h != 0) {
        _HLock(h);
        if ((RU16(R32(h)) & 0x400) == 0 && (PackShapeLZ(h) & 0xff) != 0)
            PoPWriteResource(h, cs(FN_GetEstTimeToPackData, 0x52cd6, 0x52e1c));
        _HUnlock(h);
        _HPurge(h);
        HandleEvent();
    }
    CloseRsrcFile(a6 - 0x208);
    _UseResFile(cur);
    uint32_t t1 = _TickCount();
    int32_t q = (int32_t)((t1 - t0) * 0x7bu) / 0xe10;
    est = (uint16_t)((uint16_t)q + 1);
    if (est > 0x14) est = (uint16_t)(est - (uint16_t)(((int32_t)est - 0x14) / 5));
    W16(gEstPackTime, est);
    return est;
}

/* 00052e4e: escape in the packing window asks to stop, other keys beep */
uint32_t HandleKeyDownInPackingWindow(uint32_t evt) {
    ENTRY_LINK(FN_HandleKeyDownInPackingWindow, 0, 4);
    if (_FrontWindow() != R32(gPackWin)) return 0;
    uint16_t c = (uint16_t)(R32(evt + 2) & 0xff);
    if (c == 0x1b) W16(gStopPacking, AlertYesNo(cs(FN_HandleKeyDownInPackingWindow, 0x52e4e, 0x52eac)));
    else _SysBeep(0);
    return 0;
}

/* 00052edc */
uint32_t HandleMouseDownInPackingWindow(void) {
    ENTRY_LINK(FN_HandleMouseDownInPackingWindow, 0, 0);
    if (_FrontWindow() == R32(gPackWin)) _SysBeep(0);
    return 0;
}

/* 00052f16 */
uint32_t HandleUpdateEvtInPackingWindow(void) {
    ENTRY_LINK(FN_HandleUpdateEvtInPackingWindow, 4, 4);
    if (R32(gPackWin) == 0) return 0;
    _GetPort(a6 - 4);
    _SetPort(R32(gPackWin));
    FUN_00052c60();
    if (R16(gBar0Max) != 0) {
        DrawPackingProgress(0, -1, -1);
        DrawPackingProgress(1, -1, -1);
        SetPackingWindowFilename(0);
    }
    _SetPort(R32(a6 - 4));
    return 0;
}

/* 00052f8a: reload level `level` after packing from the game */
uint32_t GoBackToGameAfterPacking(int16_t level) {
    ENTRY_LINK(FN_GoBackToGameAfterPacking, 0, 0);
    FUN_0001d382();
    W16(R32(gGameState) + 0x218a, level);
    W16(R32(gGameState) + 0x2186, 0);
    ResumeAudioSystem();
    return 0;
}

/* 00052fca: open the packing window (WIND 0x195) */
uint32_t FUN_00052fca(void) {
    ENTRY_NOLINK(FN_FUN_00052fca, 0);
    uint32_t w = _GetNewCWindow(0x195, 0, 0xffffffffu);
    W32(gPackWin, w);
    _SelectWindow(w);
    _SetPort(R32(gPackWin));
    _TextFont(0);
    _TextSize(0xc);
    return 0;
}

/* 00052ff4: is this file name one of the BG set files (compared with the tail of the
 * stored names)? */
uint32_t IsBGSetFile(uint32_t name) {
    ENTRY_LINK(FN_IsBGSetFile, 8, 28);
    int16_t found = 0;
    uint16_t i = 0;
    uint32_t chars = name + 1;
    maddr e = gBGSetFileNames;
    do {
        uint32_t s = R32(e);
        if (s != 0) {
            uint8_t n = R8(name);
            int16_t off = (int16_t)(R8(s) - n + 1);
            if (off > 0)
                found = tc_memcmp(chars, s + (uint32_t)(int32_t)off, n) == 0 ? 1 : 0;
        }
        i++;
        e += 6;
    } while (i < 7 && found == 0);
    return (uint16_t)found;
}

/* 00053074: leave the game for packing; returns the current level number */
uint32_t FUN_00053074(void) {
    ENTRY_NOLINK(FN_FUN_00053074, 4);
    ResetSoundItem(0);
    int16_t level = R16(R32(gGameState) + 0x218a);
    UnloadLevel(0);
    FUN_0001c94a();
    uint32_t r = PauseAudioSystem();
    return (r & 0xffff0000u) | (uint16_t)level;
}

/* 0005309c: show "prefix name suffix" in the packing window (NULL: the stored name;
 * the argument slot then receives the stored name's address) */
static uint32_t SetPackingWindowFilename_(uint32_t *name) {
    ENTRY_LINK(FN_SetPackingWindowFilename, 0x108, 0);
    if (*name == 0) *name = gPackFileName;
    else pstrcpy(gPackFileName, *name);
    pstrcpy(a6 - 0x108, R32(gPackNamePrefix));
    pstrcat(a6 - 0x108, *name);
    pstrcat(a6 - 0x108, R32(gPackNameSuffix));
    _SetRect(a6 - 8, 9, 0x16, R16(gBarRight), 0x26);
    _MoveTo(9, 0x23);
    _EraseRect(a6 - 8);
    _DrawString(a6 - 0x108);
    return 0;
}
uint32_t SetPackingWindowFilename(uint32_t name) { return SetPackingWindowFilename_(&name); }
void glue_SetPackingWindowFilename(void) {
    maddr sp = emu_sp();
    uint32_t name = R32(sp);
    uint32_t r = SetPackingWindowFilename_(&name);
    W32(sp, name);
    emu_set_d0(r);
}

/* ========================================================= offscreen worlds == */
/* Offscreen record (0x22 bytes): +4 saved port, +0xc offscreen GrafPort (BuildOffBitMap),
 * +0x10 saved GDevice, +0x14 GWorld, +0x18 bounds, +0x20 built flag. */

/* 00053134: save the current GWorld and switch to the offscreen one */
uint32_t SaveCurrentWorld(uint32_t w) {
    ENTRY_LINK(FN_SaveCurrentWorld, 0, 4);
    if (R8(w + 0x20) == 0) return 0;
    _GetGWorld(w + 4, w + 0x10);
    _SetGWorld(R32(w + 0x14), 0);
    return 0;
}

/* 0005318a */
uint32_t RestoreOriginalWorld(uint32_t w) {
    ENTRY_LINK(FN_RestoreOriginalWorld, 0, 4);
    if (R8(w + 0x20) != 0) _SetGWorld(R32(w + 4), R32(w + 0x10));
    return 0;
}

/* 000531c8 */
uint32_t DisposeSOffWorld(uint32_t w) {
    ENTRY_LINK(FN_DisposeSOffWorld, 0, 4);
    if (R8(w + 0x20) == 0) return 0;
    uint32_t gw = R32(w + 0x14);
    if (gw != 0) {
        _UnlockPixels(R32(gw + 2));
        _DisposeGWorld(R32(w + 0x14));
    }
    W8(w + 0x20, 0);
    return 0;
}

/* 00053220: 8-bit GWorld for `rect` on the main device; returns the QDErr */
uint32_t BuildOffGWorld(uint32_t w, uint32_t rect) {
    ENTRY_LINK(FN_BuildOffGWorld, 4, 12);
    W32(a6 - 4, 0);
    /* "clr.b -(SP)": a word whose high byte is 0 and whose low byte is what the stack held */
    CheckCurrentFreeMemory((int16_t)R8(emu_sp() - 1));
    uint32_t dev = _GetMainDevice();
    int16_t err = (int16_t)_NewGWorld(a6 - 4, 8, rect, 0, dev, 2);
    if (err == 0) {
        W32(w + 0x14, R32(a6 - 4));
        W8(w + 0x20, 1);
    }
    return (uint16_t)err;
}

/* 0005328c: old-style 1-bit offscreen GrafPort (unused by the game) */
uint32_t BuildOffBitMap(uint32_t w, uint32_t rect) {
    ENTRY_LINK(FN_BuildOffBitMap, 8, 20);
    int16_t err = 0;
    W32(a6 - 8, 0);
    _GetPort(a6 - 8);
    uint32_t port = PoPNewPtr(0x6c);
    if (R16(lmMemErr) != 0) return 1;
    _OpenPort(port);
    W32(port + 0x10, R32(rect)); W32(port + 0x14, R32(rect + 4));
    _RectRgn(R32(port + 0x1c), rect);
    _RectRgn(R32(port + 0x18), rect);
    W32(port + 8, R32(rect)); W32(port + 0xc, R32(rect + 4));
    int32_t wd = (int32_t)R16(rect + 6) - (int32_t)R16(rect + 2) + 0xf;
    uint16_t rb = (uint16_t)((uint16_t)(wd >> 4) << 1);
    W16(port + 6, rb);
    W16(a6 - 4, rb);
    int32_t ht = (int32_t)R16(rect + 4) - (int32_t)R16(rect);
    W32(port + 2, PoPNewPtr((uint32_t)(ht * (int32_t)R16(a6 - 4))));
    if (R16(lmMemErr) != 0) {
        _SetPort(R32(a6 - 8));
        _ClosePort(port);
        _DisposePtr(port);
        err = 2;
    } else {
        _SetPort(R32(a6 - 8));
        W32(w + 0xc, port);
        W8(w + 0x20, 1);
        SaveCurrentWorld(w);
        _EraseRect(rect);
        RestoreOriginalWorld(w);
    }
    return (uint16_t)err;
}

/* 0005339e: (re)build the offscreen world `w` for `rect` and lock its pixels */
uint32_t BuildSOffWorld(uint32_t w, uint32_t rect) {
    ENTRY_LINK(FN_BuildSOffWorld, 0, 12);
    DisposeSOffWorld(w);
    W32(w, 0); W32(w + 4, 0); W32(w + 8, 0); W32(w + 0xc, 0); W32(w + 0x10, 0); W32(w + 0x14, 0);
    W32(w + 0x18, R32(rect)); W32(w + 0x1c, R32(rect + 4));
    W8(w + 0x20, 0);
    int16_t err = (int16_t)BuildOffGWorld(w, rect);
    if (err == 0) _LockPixels(R32(R32(w + 0x14) + 2));
    return (uint16_t)err;
}

/* 00053424: CopyBits from the offscreen world to a window */
uint32_t DrawSOffWorldToWindow(uint32_t w, uint32_t win, uint32_t src, uint32_t dst, int16_t mode) {
    ENTRY_LINK(FN_DrawSOffWorldToWindow, 0, 0);
    seg17_04fc04();
    FUN_0004fbe2();
    _RGBForeColor(gFgRGB);
    _RGBBackColor(gBgRGB);
    _CopyBits(R32(R32(R32(w + 0x14) + 2)), win + 2, src, dst, mode, 0);
    FUN_0004fc26();
    seg17_04fc46();
    return 0;
}

/* 00053488: CopyBits between two offscreen worlds */
uint32_t DrawOffToOff(uint32_t from, uint32_t to, uint32_t src, uint32_t dst, int16_t mode) {
    ENTRY_LINK(FN_DrawOffToOff, 0, 0);
    seg17_04fc04();
    FUN_0004fbe2();
    _RGBForeColor(gFgRGB);
    _RGBBackColor(gBgRGB);
    uint32_t a = R32(R32(R32(from + 0x14) + 2));
    _CopyBits(a, R32(R32(R32(to + 0x14) + 2)), src, dst, mode, 0);
    FUN_0004fc26();
    seg17_04fc46();
    return 0;
}

/* 000534e8: swap the rows of `rect` top to bottom in the offscreen world (the first
 * swapped bottom row is rect.bottom itself, one past the rectangle) */
uint32_t InvertOffWorld(uint32_t w, uint32_t rect) {
    ENTRY_LINK(FN_InvertOffWorld, 0x3c, 32);
    uint32_t base = _GetPixBaseAddr(R32(R32(w + 0x14) + 2));
    base = tc_StripAddress(base);
    uint32_t pm = R32(R32(R32(w + 0x14) + 2));
    int16_t rb = (int16_t)(RU16(pm + 4) & 0x7ffe);
    int16_t btop = R16(pm + 6);
    int16_t top = (int16_t)(R16(rect) - btop);
    uint32_t p = base + (uint32_t)((int32_t)top * (int32_t)rb);
    int16_t bot = (int16_t)(R16(rect + 4) - btop);
    uint32_t q = base + (uint32_t)((int32_t)bot * (int32_t)rb);
    int16_t nlongs = (int16_t)((int32_t)rb >> 2);
    uint8_t mode = (uint8_t)swap_mmu(1);
    while ((int32_t)q > (int32_t)p) {
        if (nlongs != 0) {
            uint32_t a = p, b = q;
            uint16_t n = (uint16_t)nlongs;
            do {
                uint32_t x = R32(a), y = R32(b);
                W32(a, y); a += 4;
                W32(b, x); b += 4;
            } while (--n != 0);
        }
        p += (uint32_t)(int32_t)rb;
        q -= (uint32_t)(int32_t)rb;
    }
    swap_mmu(mode);
    return 0;
}

/* ===================================================== saved screen rects == */
/* A saved rect is a handle to a raw shape (type 0xb000): +2 row bytes, +4 width,
 * +6 height, +8 left, +0xa top, pixels at +0xc. */

/* 0005364a: save `rect` (clipped to thePort) of the current port into *ph (reused when
 * big enough, else replaced); returns the handle */
uint32_t SaveRect(uint32_t rect, uint32_t ph) {
    ENTRY_LINK(FN_SaveRect, 0x3c, 32);
    uint32_t h = ph != 0 ? R32(ph) : 0;
    uint32_t port = R32(qdThePort);
    if ((pascal_ptr3(R32(pSectRect), R32(R32(port + 2)) + 6, rect, rect, 1) & 0xff) == 0) return h;
    int16_t ht = (int16_t)(R16(rect + 4) - R16(rect));
    int16_t wd = (int16_t)(R16(rect + 6) - R16(rect + 2));
    int16_t rbs = (int16_t)((wd + 3) & 0x7ffc);
    int32_t size = (int32_t)((uint32_t)(int32_t)ht * (uint32_t)(int32_t)rbs) + 0xc;
    if (size <= 0xc || size > 0x3000c) return h;
    int32_t old = (ph != 0 && R32(ph) != 0) ? (int32_t)tc_GetHandleSize(R32(ph)) : 0;
    if (size > old) {
        if (ph != 0 && R32(ph) != 0) _DisposeHandle(R32(ph));
        h = PoPNewHandle((uint32_t)size);
    }
    if (h == 0) return 0;
    uint32_t pmh = R32(R32(qdThePort) + 2);
    uint32_t pm = R32(pmh);
    int32_t row0 = (int32_t)R16(rect) - (int32_t)R16(pm + 6);
    int32_t col0 = (int32_t)R16(rect + 2) - (int32_t)R16(pm + 8);
    int32_t rb = (int32_t)(int16_t)(RU16(pm + 4) & 0x7ffe);
    uint32_t base = _GetPixBaseAddr(pmh);
    uint32_t src = base + (uint32_t)(rb * row0) + (uint32_t)col0;
    _HLock(h);
    uint32_t hp = R32(h);
    uint32_t dst = hp + 0xc;
    for (int32_t row = 0; row < (int32_t)ht; row++) {
        uint32_t s = src;
        for (int32_t c = 0; c < (int32_t)rbs; c += 4) {
            W32(dst, R32(s));
            s += 4; dst += 4;
        }
        src += (uint32_t)rb;
    }
    W16(hp, 0xb000);
    W16(hp + 2, rbs);
    W16(hp + 4, wd);
    W16(hp + 6, ht);
    W16(hp + 8, R16(rect + 2));
    W16(hp + 0xa, R16(rect));
    _HUnlock(h);
    return h;
}

/* 00053860 */
uint32_t RestoreRect(uint32_t h) {
    ENTRY_LINK(FN_RestoreRect, 0, 0);
    RestoreRectKeep(h);
    return DeallocMapSave(h);
}

/* 0005388a: draw a saved rect back where it came from */
uint32_t RestoreRectKeep(uint32_t h) {
    ENTRY_LINK(FN_RestoreRectKeep, 0, 8);
    if (h == 0) return 0;
    _HLock(h);
    uint32_t p = R32(h);
    DrawIt(p, 0, R16(p + 8), R16(p + 0xa), 0, 0, 0);
    _HUnlock(h);
    return 0;
}

/* 000538de */
uint32_t DeallocMapSave(uint32_t h) {
    ENTRY_LINK(FN_DeallocMapSave, 0, 0);
    if (h != 0) {
        _HUnlock(h);
        _DisposeHandle(h);
    }
    return 0;
}

/* 0005390a: rectangle covered by a saved rect */
uint32_t GetMapSaveRect(uint32_t rect, uint32_t h) {
    if (h == 0) return 0;
    uint32_t p = R32(h);
    W16(rect, R16(p + 0xa));
    W16(rect + 2, R16(p + 8));
    W16(rect + 4, R16(rect) + R16(p + 6));
    W16(rect + 6, R16(rect + 2) + R16(p + 4));
    return 0;
}

/* ================================================================ blitters == */

/* 00053960: 4-bit shape: each nonzero nibble (low nibble first) is written as
 * (nibble | color); a zero byte skips two pixels. `odd` starts on the high nibble of
 * the first byte. The original masks its own argument slot: (0x1a,A6) &= 1. */
static uint32_t Paint4BitShape_(uint32_t src, uint32_t dst, int16_t srb, int16_t drb, int16_t width,
                                int16_t rows, int16_t color, int16_t *odd) {
    uint16_t cnt = (uint16_t)(width - 1);
    uint16_t r = (uint16_t)(rows - 1);
    uint8_t col = (uint8_t)color;
    *odd = (int16_t)(*odd & 1);
    if (*odd != 0) {
        do {
            uint32_t a2 = src, a3 = dst;
            uint16_t d1 = cnt;
            uint8_t b = R8(a2++);
            for (;;) {
                /* 0x5399a: high nibble of b */
                uint8_t n = (uint8_t)(((uint8_t)(b >> 4 | b << 4)) & 0xf);
                if (n) W8(a3, n | col);
                a3++;
                d1--;
                if (d1 == 0) break;             /* beq rowend; bne next (even when negative) */
                /* 0x539b2: next byte, low nibble */
                for (;;) {
                    b = R8(a2++);
                    if (b != 0) break;
                    a3 += 2;
                    d1 = (uint16_t)(d1 - 2);
                    if (d1 == 0 || (int16_t)d1 < 0) goto row_odd;
                }
                n = b & 0xf;
                if (n) W8(a3, n | col);
                a3++;
                d1--;
                if (d1 == 0 || (int16_t)d1 < 0) break;
            }
        row_odd:
            src += (uint32_t)(int32_t)srb;
            dst += (uint32_t)(int32_t)drb;
        } while (r-- != 0);
    } else {
        do {
            uint32_t a2 = src, a3 = dst;
            uint16_t d1 = cnt;
            for (;;) {
                uint8_t b = R8(a2++);
                if (b == 0) {
                    a3 += 2;
                    d1 = (uint16_t)(d1 - 2);
                    if ((int16_t)d1 >= 0) continue;
                    break;
                }
                uint8_t n = b & 0xf;
                if (n) W8(a3, n | col);
                a3++;
                if (d1-- == 0) break;
                n = (uint8_t)(((uint8_t)(b >> 4 | b << 4)) & 0xf);
                if (n) W8(a3, n | col);
                a3++;
                if (d1-- == 0) break;
            }
            src += (uint32_t)(int32_t)srb;
            dst += (uint32_t)(int32_t)drb;
        } while (r-- != 0);
    }
    return 0;
}
uint32_t Paint4BitShape(uint32_t src, uint32_t dst, int16_t srb, int16_t drb, int16_t width,
                        int16_t rows, int16_t color, int16_t odd) {
    return Paint4BitShape_(src, dst, srb, drb, width, rows, color, &odd);
}
void glue_Paint4BitShape(void) {
    maddr sp = emu_sp();
    int16_t odd = R16(sp + 18);
    uint32_t d0 = cpu_get(R_D0);                /* movem.l D0-D7/A0-A3: D0 is preserved */
    Paint4BitShape_(R32(sp), R32(sp + 4), R16(sp + 8), R16(sp + 10), R16(sp + 12), R16(sp + 14),
                    R16(sp + 16), &odd);
    W16(sp + 18, odd);
    emu_set_d0(d0);
}

/* 00053a3a: opaque copy of a raw shape (16-byte blocks, then the remaining bytes) */
uint32_t PaintCopy(uint32_t src, uint32_t dst, int16_t srb, int16_t drb, int16_t width, int16_t rows) {
    uint32_t w = (uint16_t)width;
    uint16_t r = (uint16_t)(rows - 1);
    do {
        uint32_t a2 = src, a3 = dst;
        uint16_t n16 = (uint16_t)(w >> 4);
        for (uint16_t k = 0; k < n16; k++) {
            for (int j = 0; j < 4; j++) { W32(a3, R32(a2)); a2 += 4; a3 += 4; }
        }
        uint16_t n = (uint16_t)(w & 0xf);
        while (n-- != 0) W8(a3++, R8(a2++));
        src += (uint32_t)(int32_t)srb;
        dst += (uint32_t)(int32_t)drb;
    } while (r-- != 0);
    return 0;
}

/* Common prologue of the RLE painters: destination address in the port's pixmap,
 * the source rows from row `skiprows` on. */
typedef struct { uint32_t dst, rowhdr, data; int16_t rb; } RLESetup;
static RLESetup rle_setup(uint32_t *shape, uint32_t pmh, int16_t skiprows, int16_t dx, int16_t dy) {
    RLESetup s;
    s.rb = (int16_t)(RU16(R32(pmh) + 4) & 0x7ffe);
    uint32_t base = _GetPixBaseAddr(pmh);
    s.dst = (uint32_t)((int32_t)dy * (int32_t)s.rb) + base + (uint32_t)(int32_t)dx;
    *shape = tc_StripAddress(*shape);           /* stored into the argument slot (8,A6) */
    uint32_t row = *shape + 0xc;
    for (int16_t i = 0; i < skiprows; i++) row = row + 2 + (uint32_t)(int32_t)R16(row);
    s.rowhdr = row;
    s.data = row + 2;
    return s;
}

/* Skip `skip` pixels of a packed row (0x53ca0..0x53cd2): returns with d1 = the clipped
 * remainder of the current run byte and a1 past what was skipped. */
static void rle_skip(uint32_t *a1, uint32_t *d1, uint32_t *d6, uint32_t *d7) {
    *d7 = 0; *d1 = 0;
    for (;;) {
        *d7 = (*d7 & 0xffffff00u) | R8((*a1)++);
        *d1 = (*d1 & 0xffffff00u) | (*d7 & 0xff);
        *d1 = (*d1 & 0xffff0000u) | (*d1 & 0x7f);
        if ((uint16_t)*d6 <= (uint16_t)*d1) break;
        if ((*d7 & 0x80) == 0) *a1 += (uint32_t)(int32_t)(int16_t)*d1;
        (*a1)++;
        *d6 = (*d6 & 0xffff0000u) | (uint16_t)((uint16_t)*d6 - (uint16_t)*d1);
        *d6 = (*d6 & 0xffff0000u) | (uint16_t)((uint16_t)*d6 - 1);
    }
    if ((*d7 & 0x80) == 0) *a1 += (uint32_t)(int32_t)(int16_t)*d6;
    *d7 = (*d7 & 0xffffff00u) | (uint8_t)((uint8_t)*d7 - (uint8_t)*d6);
    *d1 = *d7;
}

/* Clip the run in d1 against the pixels left in d0.w (0x53cd8..0x53ce4); returns the
 * run flag (bit 7) and clears it in d1. */
static int rle_clip(uint32_t *d0, uint32_t *d1, uint32_t *d7) {
    *d7 = (*d7 & 0xffff0000u) | (*d1 & 0xffff);
    *d7 &= ~0x80u;
    *d7 = (*d7 & 0xffff0000u) | (uint16_t)((uint16_t)*d7 - (uint16_t)*d0);
    if ((int16_t)*d7 >= 0) {
        *d1 = (*d1 & 0xffff0000u) | (uint16_t)((uint16_t)*d1 - (uint16_t)*d7);
        *d1 = (*d1 & 0xffff0000u) | (uint16_t)((uint16_t)*d1 - 1);
    }
    int run = (*d1 & 0x80) != 0;
    *d1 &= ~0x80u;
    return run;
}

/* Left-to-right RLE row loop shared by PaintRLE (dir 1) and PaintRLETB (dir -1). */
static void rle_rows(RLESetup s, int16_t transparent, int16_t width, int16_t *rows, int16_t skip,
                     uint32_t d0, int dir) {
    uint32_t d1 = 0, d3 = 0, d5 = 0, d6 = 0, d7 = 0;
    uint32_t a0 = s.data, a3 = s.rowhdr, a4 = s.dst;
    uint32_t d2 = (uint16_t)transparent;
    const uint16_t d4 = 8;
    if (dir < 0) {                              /* PaintRLETB: start on the last row */
        d5 = (d5 & 0xffff0000u) | (uint16_t)(*rows - 2);
        if ((int16_t)d5 >= 0) {
            do a4 += (uint32_t)(int32_t)s.rb;
            while ((uint16_t)(d5 = (d5 & 0xffff0000u) | (uint16_t)((uint16_t)d5 - 1)) != 0xffff);
        }
    }
    for (;;) {
        uint32_t a2 = a4, a1 = a0;
        d0 = (d0 & 0xffff0000u) | (uint16_t)width;
        d6 = (d6 & 0xffff0000u) | (uint16_t)skip;
        int have = 0;
        if ((uint16_t)d6 != 0) { rle_skip(&a1, &d1, &d6, &d7); have = 1; }
        for (;;) {
            if (!have) { d1 = R8(a1++); }
            have = 0;
            if (rle_clip(&d0, &d1, &d7)) {
                d5 = (d5 & 0xffff0000u) | (d1 & 0xffff);
                d3 = (d3 & 0xffffff00u) | R8(a1++);
                if ((d3 & 0xff) == 0 && (uint16_t)d2 != 0) {
                    a2 = a2 + 1 + (uint32_t)(int32_t)(int16_t)d1;
                } else {
                    if ((uint16_t)d5 >= d4) {
                        d5 = (d5 & 0xffff0000u) | (uint16_t)((uint16_t)d5 >> 2);
                        d5 = (d5 & 0xffff0000u) | (uint16_t)((uint16_t)d5 - 1);
                        d3 = (d3 & 0xffff00ffu) | ((d3 & 0xff) << 8);
                        d3 = (d3 & 0x0000ffffu) | ((d3 & 0xffff) << 16);
                        do { W32(a2, d3); a2 += 4; }
                        while ((uint16_t)(d5 = (d5 & 0xffff0000u) | (uint16_t)((uint16_t)d5 - 1)) != 0xffff);
                        d5 = d1 & 3;
                    }
                    do { W8(a2, (uint8_t)d3); a2++; }
                    while ((uint16_t)(d5 = (d5 & 0xffff0000u) | (uint16_t)((uint16_t)d5 - 1)) != 0xffff);
                }
            } else {
                d5 = (d5 & 0xffff0000u) | (d1 & 0xffff);
                if ((uint16_t)d5 >= d4) {
                    d5 = (d5 & 0xffff0000u) | (uint16_t)((uint16_t)d5 >> 3);
                    d5 = (d5 & 0xffff0000u) | (uint16_t)((uint16_t)d5 - 1);
                    do {
                        W32(a2, R32(a1)); a1 += 4; a2 += 4;
                        W32(a2, R32(a1)); a1 += 4; a2 += 4;
                    } while ((uint16_t)(d5 = (d5 & 0xffff0000u) | (uint16_t)((uint16_t)d5 - 1)) != 0xffff);
                    d5 = d1 & 7;
                }
                do { W8(a2, R8(a1)); a1++; a2++; }
                while ((uint16_t)(d5 = (d5 & 0xffff0000u) | (uint16_t)((uint16_t)d5 - 1)) != 0xffff);
            }
            d1 += 1;
            uint32_t old = d0;
            d0 -= d1;
            if (!(old > d1)) break;             /* bhi */
        }
        if (dir > 0) a4 += (uint32_t)(int32_t)s.rb;
        else a4 -= (uint32_t)(int32_t)s.rb;
        a3 = a3 + (uint32_t)(int32_t)R16(a3) + 2;
        a0 = a3 + 2;
        uint16_t n = (uint16_t)*rows;
        *rows = (int16_t)(n - 1);
        if (!(n > 1)) break;                    /* subq.w #1,(0x14,A6); bhi */
    }
}

/* 00053aa0: RLE shape drawn mirrored (right to left) */
static uint32_t PaintRLELR_(uint32_t *shape, uint32_t pmh, int16_t transparent, int16_t width, int16_t *rows,
                            int16_t skip, int16_t skiprows, int16_t dx, int16_t dy) {
    ENTRY_LINK(FN_PaintRLELR, 0x12, 0);
    RLESetup s = rle_setup(shape, pmh, skiprows, dx, dy);
    uint32_t d0 = swap_mmu(0);                  /* the byte at (-2,A6) of the word 1 */
    uint8_t mode = (uint8_t)d0;
    uint32_t d1 = 0, d3 = 0, d5 = 0, d6 = 0, d7 = 0;
    uint32_t a0 = s.data, a3 = s.rowhdr, a4 = s.dst;
    uint32_t d2 = (uint16_t)transparent;
    for (;;) {
        uint32_t a2 = a4 + (uint32_t)(int32_t)width, a1 = a0;
        d0 = (d0 & 0xffff0000u) | (uint16_t)width;
        d6 = (d6 & 0xffff0000u) | (uint16_t)skip;
        int have = 0;
        if ((uint16_t)d6 != 0) { rle_skip(&a1, &d1, &d6, &d7); have = 1; }
        for (;;) {
            if (!have) { d1 = R8(a1++); }
            have = 0;
            if (rle_clip(&d0, &d1, &d7)) {
                d5 = (d5 & 0xffff0000u) | (d1 & 0xffff);
                d3 = (d3 & 0xffffff00u) | R8(a1++);
                if ((d3 & 0xff) == 0 && (uint16_t)d2 != 0) {
                    do a2--;
                    while ((uint16_t)(d5 = (d5 & 0xffff0000u) | (uint16_t)((uint16_t)d5 - 1)) != 0xffff);
                } else {
                    do { a2--; W8(a2, (uint8_t)d3); }
                    while ((uint16_t)(d5 = (d5 & 0xffff0000u) | (uint16_t)((uint16_t)d5 - 1)) != 0xffff);
                }
            } else {
                d5 = (d5 & 0xffff0000u) | (d1 & 0xffff);
                do { a2--; W8(a2, R8(a1)); a1++; }
                while ((uint16_t)(d5 = (d5 & 0xffff0000u) | (uint16_t)((uint16_t)d5 - 1)) != 0xffff);
            }
            d1 += 1;
            uint32_t old = d0;
            d0 -= d1;
            if (!(old > d1)) break;
        }
        a4 += (uint32_t)(int32_t)s.rb;
        a3 = a3 + (uint32_t)(int32_t)R16(a3) + 2;
        a0 = a3 + 2;
        uint16_t n = (uint16_t)*rows;
        *rows = (int16_t)(n - 1);
        if (!(n > 1)) break;
    }
    swap_mmu(mode);
    return 0;
}

/* 00053bf6: RLE shape (run byte: bit 7 = repeat the next byte, low 7 bits = count-1;
 * color 0 runs are transparent when `transparent` != 0) */
static uint32_t PaintRLE_(uint32_t *shape, uint32_t pmh, int16_t transparent, int16_t width, int16_t *rows,
                          int16_t skip, int16_t skiprows, int16_t dx, int16_t dy) {
    ENTRY_LINK(FN_PaintRLE, 0x12, 0);
    RLESetup s = rle_setup(shape, pmh, skiprows, dx, dy);
    uint32_t d0 = swap_mmu(0);
    uint8_t mode = (uint8_t)d0;
    rle_rows(s, transparent, width, rows, skip, d0, 1);
    swap_mmu(mode);
    return 0;
}

/* The RLE painters store the stripped shape address into (8,A6) and count the rows
 * down in (0x14,A6): their glue writes both slots back. */
#define RLE_GLUE(Name) \
    void glue_##Name(void) { \
        maddr sp = emu_sp(); \
        uint32_t shape = R32(sp); int16_t rows = R16(sp + 12); \
        uint32_t r = Name##_(&shape, R32(sp + 4), R16(sp + 8), R16(sp + 10), &rows, R16(sp + 14), \
                             R16(sp + 16), R16(sp + 18), R16(sp + 20)); \
        W32(sp, shape); W16(sp + 12, rows); emu_set_d0(r); }
uint32_t PaintRLELR(uint32_t p4, uint32_t p8, int16_t p12, int16_t p14, int16_t p16, int16_t p18,
                    int16_t p20, int16_t p22, int16_t p24) {
    return PaintRLELR_(&p4, p8, p12, p14, &p16, p18, p20, p22, p24);
}
RLE_GLUE(PaintRLELR)
uint32_t PaintRLE(uint32_t p4, uint32_t p8, int16_t p12, int16_t p14, int16_t p16, int16_t p18,
                  int16_t p20, int16_t p22, int16_t p24) {
    return PaintRLE_(&p4, p8, p12, p14, &p16, p18, p20, p22, p24);
}
RLE_GLUE(PaintRLE)

/* 00053d72: build the flip tables (bit-reversed bytes, nibble-swapped bytes) */
uint32_t FUN_00053d72(void) {
    for (int16_t i = 0; i < 0x100; i++) {
        uint16_t bits = (uint16_t)(i & 0xff);
        uint16_t rev = 0;
        for (int16_t k = 0; k < 8; k++) {
            rev = (uint16_t)((int32_t)(int16_t)rev >> 1);
            bits = (uint16_t)(bits + bits);
            if (bits & 0x100) rev |= 0x80;
        }
        W8(gBitRevTable + (uint32_t)i, rev);
        uint8_t lo = (uint8_t)((i & 0xf) << 4);
        uint8_t hi = (uint8_t)(((int32_t)i & 0xf0) >> 4);
        W8(gNibSwapTable + (uint32_t)i, lo | hi);
    }
    W32(gFlipTablesOK, 0x0f0f0f0f);
    return 0;
}

/* 00053dd6: draw shape `shape` (pointer to its data) at (x, y) in thePort, clipped to
 * the visible region. Shape types (word +0 & 0xff80): 0x9000/0x9100 bitmap (+mask),
 * 0xa000 4-bit, 0xb000 raw bytes (saved rect), 0xb100 RLE; bits 0x30 = flipped LR/TB,
 * low nibble = left padding. Without a shape, `bits` (a 1-bit BitMap record) is painted
 * as a region in `color`. The drawn rectangle goes to *outRect. RLE shapes with
 * color > 1 go through the bank painters with color-1 (stored into the argument slot). */
static uint32_t DrawIt_(uint32_t shape, uint32_t bits, int16_t x, int16_t y, int16_t mode, int16_t *color,
                        uint32_t outRect) {
    ENTRY_LINK(FN_DrawIt, 0x76, 28);            /* link -0x76; movem D3-D7/A3-A4 */
    if (shape != 0) {
        W16(a6 - 0x12, 1);
        uint8_t mmu = (uint8_t)swap_mmu(R8(a6 - 0x12));
        W16(a6 - 6, x);
        W16(a6 - 2, (int16_t)(R16(shape + 4) + x));
        W16(a6 - 8, y);
        W16(a6 - 4, (int16_t)(R16(shape + 6) + y));
        uint32_t vis = R32(R32(R32(qdThePort) + 0x1c));
        if ((_SectRect(a6 - 8, vis + 2, a6 - 0x10) & 0xff) != 0) {
            int16_t type = R16(shape);
            int16_t sx = (int16_t)(R16(a6 - 0xe) - R16(a6 - 6) + (type & 0xf));
            int16_t sy = (int16_t)(R16(a6 - 0x10) - R16(a6 - 8));
            W16(a6 - 0x6a, (int16_t)(R16(a6 - 0xc) - R16(a6 - 0x10)));
            int16_t wd = (int16_t)(R16(a6 - 0xa) - R16(a6 - 0xe));
            int32_t kind = (int32_t)type & 0xff80;
            if (kind == 0xb000 || kind == 0xa000) {
                int16_t srb = R16(shape + 2);
                uint32_t pm = R32(R32(R32(qdThePort) + 2));
                int16_t drb = (int16_t)(RU16(pm + 4) & 0x7ffe);
                uint32_t src;
                if (kind == 0xb000)
                    src = shape + 0xc + (uint32_t)((int32_t)srb * (int32_t)sy) + (uint32_t)(int32_t)sx;
                else
                    src = (uint32_t)((int32_t)sx >> 1) + shape + 0xc + (uint32_t)((int32_t)srb * (int32_t)sy);
                W32(a6 - 0x6e, src);
                uint32_t bnd = R32(R32(R32(qdThePort) + 2)) + 6;
                W16(a6 - 0x70, (int16_t)(R16(a6 - 0x10) - R16(bnd)));
                W16(a6 - 0x72, (int16_t)(R16(a6 - 0xe) - R16(bnd + 2)));
                uint32_t base = _GetPixBaseAddr(R32(R32(qdThePort) + 2));
                uint32_t dst = (uint32_t)((int32_t)drb * (int32_t)R16(a6 - 0x70)) + base + (uint32_t)(int32_t)R16(a6 - 0x72);
                if (kind == 0xb000) {
                    if (mode == 0x10) PaintTrans(R32(a6 - 0x6e), dst, srb, drb, wd, R16(a6 - 0x6a));
                    else PaintCopy(R32(a6 - 0x6e), dst, srb, drb, wd, R16(a6 - 0x6a));
                } else {
                    if ((R16(shape + 2) & 0xf) != 0) wd++;
                    Paint4BitShape(R32(a6 - 0x6e), dst, srb, drb, wd, R16(a6 - 0x6a), (int16_t)(*color & 0xf0), sx);
                }
            } else if (kind == 0xb100) {
                uint32_t bnd = R32(R32(R32(qdThePort) + 2)) + 6;
                W16(a6 - 0x70, (int16_t)(R16(a6 - 0x10) - R16(bnd)));
                W16(a6 - 0x72, (int16_t)(R16(a6 - 0xe) - R16(bnd + 2)));
                if (*color > 1) {
                    *color = (int16_t)(*color - 1);
                    int32_t c = (int32_t)*color;
                    uint32_t pat = (uint32_t)(c << 8) | (uint32_t)c;
                    pat |= pat << 16;
                    if (((int32_t)type & 0x30) == 0x10) {
                        sx = (int16_t)(R16(shape + 4) - (int16_t)(wd + sx));
                        /* the long pattern is pushed; BankRLELR reads its high word */
                        maddr sp_ = emu_sp(); push16((uint16_t)pat);
                        BankRLELR(shape, R32(R32(qdThePort) + 2), wd, R16(a6 - 0x6a), sx, sy,
                                  R16(a6 - 0x72), R16(a6 - 0x70), (int16_t)(pat >> 16));
                        emu_set_sp(sp_);
                    } else {
                        BankRLE(shape, R32(R32(qdThePort) + 2), wd, R16(a6 - 0x6a), sx, sy,
                                R16(a6 - 0x72), R16(a6 - 0x70), pat);
                    }
                } else {
                    int32_t f = (int32_t)type & 0x30;
                    if (f == 0x20) {
                        PaintRLETB(shape, R32(R32(qdThePort) + 2), mode, wd, R16(a6 - 0x6a), sx, sy,
                                   R16(a6 - 0x72), R16(a6 - 0x70));
                    } else if (f == 0x10) {
                        sx = (int16_t)(R16(shape + 4) - (int16_t)(wd + sx));
                        PaintRLELR(shape, R32(R32(qdThePort) + 2), mode, wd, R16(a6 - 0x6a), sx, sy,
                                   R16(a6 - 0x72), R16(a6 - 0x70));
                    } else {
                        PaintRLE(shape, R32(R32(qdThePort) + 2), mode, wd, R16(a6 - 0x6a), sx, sy,
                                 R16(a6 - 0x72), R16(a6 - 0x70));
                    }
                }
            } else if (kind == 0x9000 || kind == 0x9100) {
                W16(a6 - 0x1a, 0);
                W16(a6 - 0x18, 0);
                W16(a6 - 0x14, R16(shape + 4));
                W16(a6 - 0x16, R16(shape + 6));
                W32(a6 - 0x20, shape + 0xc);
                W16(a6 - 0x1c, R16(shape + 2));
                W16(a6 - 0x66, sx);
                W16(a6 - 0x68, sy);
                W16(a6 - 0x62, (int16_t)(sx + wd));
                W16(a6 - 0x64, (int16_t)(R16(a6 - 0x6a) + sy));
                _ForeColor(0x21);
                _BackColor(0x1e);
                int32_t t = (int16_t)R16(shape);   /* movea.w: never equal to $9000 */
                if (mode != 0x10 || t == 0x9000) {
                    _StdBits(a6 - 0x20, a6 - 0x68, a6 - 0x10, 0, 0);
                } else {
                    W32(a6 - 0x28, R32(a6 - 0x1a));
                    W32(a6 - 0x24, R32(a6 - 0x16));
                    W16(a6 - 0x2a, R16(a6 - 0x1c));
                    W32(a6 - 0x2e, shape + 0xc + (uint32_t)((int32_t)R16(shape + 2) * (int32_t)R16(shape + 6)));
                    _CopyMask(a6 - 0x20, a6 - 0x2e, R32(qdThePort) + 2, a6 - 0x68, a6 - 0x68, a6 - 0x10);
                }
            }
        }
        swap_mmu(mmu);
    } else if (bits != 0 && ((int32_t)(int16_t)R16(bits) & 0x3000) == 0x1000) {
        W16(a6 - 0x1a, 0);
        W16(a6 - 0x18, 0);
        W16(a6 - 0x14, (uint16_t)((RU16(bits + 2) & 0x7ffe) << 3));
        W16(a6 - 0x16, R16(bits + 6));
        W32(a6 - 0x20, bits + 0xc);
        W16(a6 - 0x1c, R16(bits + 2));
        uint32_t rgn = _NewRgn();
        int16_t err = (int16_t)_BitMapToRegion(rgn, a6 - 0x20);
        if (rgn != 0) {
            if (err == 0) {
                FUN_0004fbe2();
                SetForeColor(*color);
                int16_t t = R16(bits);
                if (t & 0x10) x = (int16_t)(x - (t & 0xf));
                _OffsetRgn(rgn, x, y);
                _PaintRgn(rgn);
                uint32_t p = R32(rgn);
                W32(a6 - 0x10, R32(p + 2));
                W32(a6 - 0xc, R32(p + 6));
                seg17_04fc46();
            } else {
                _SetRect(a6 - 0x10, 0, 0, 0, 0);
            }
            _DisposeRgn(rgn);
        } else {
            _SetRect(a6 - 0x10, 0, 0, 0, 0);
        }
    }
    if (outRect != 0) {
        W32(outRect, R32(a6 - 0x10));
        W32(outRect + 4, R32(a6 - 0xc));
    }
    return 0;
}
uint32_t DrawIt(uint32_t shape, uint32_t bits, int16_t x, int16_t y, int16_t mode, int16_t color, uint32_t outRect) {
    return DrawIt_(shape, bits, x, y, mode, &color, outRect);
}
void glue_DrawIt(void) {
    maddr sp = emu_sp();
    int16_t color = R16(sp + 14);
    uint32_t r = DrawIt_(R32(sp), R32(sp + 4), R16(sp + 8), R16(sp + 10), R16(sp + 12), &color, R32(sp + 16));
    W16(sp + 14, color);                       /* subq.w #1,(0x16,A6) */
    emu_set_d0(r);
}

/* 00054302: load SHAP `id` (purging the BG set if memory is short), unpack LZ-packed
 * shapes, and expand byte-RLE-packed ones (flag 0x200) into a new handle */
uint32_t GetShape(int16_t id) {
    ENTRY_LINK(FN_GetShape, 0, 32);
    uint32_t h = _GetResource(SHAP, id);
    if (h == 0 && R16(gBGSetState) != 1) {
        MarkBGSetPurgable(1);
        /* the argument word is popped only after this call */
        maddr sp_ = emu_sp(); push16(1);
        h = _GetResource(SHAP, id);
        emu_set_sp(sp_);
    }
    if (h == 0) return 0;
    if (R8(R32(h)) & 4) {                      /* btst #2,(A0): flag 0x400, LZ-packed */
        uint32_t a4 = cpu_get(R_A4);           /* UnpackLZ tests the caller's A4 (here h) */
        cpu_set(R_A4, h);
        UnpackShapeLZ(h);
        cpu_set(R_A4, a4);
    }
    uint32_t p = R32(h);
    uint16_t flags = RU16(p);
    if ((flags & 0x200) == 0) return h;
    int32_t size = (int32_t)R16(p + 2) * (int32_t)R16(p + 6);
    if (flags & 0x100) size += size;
    size += 0xc;
    uint32_t hsize = tc_GetHandleSize(h);
    uint32_t nh = _NewHandle((uint32_t)size);
    if (nh == 0) return 0;
    _HLock(h);
    _HLock(nh);
    uint32_t a2 = R32(h), a3 = R32(nh);
    uint32_t srcEnd = hsize + a2, dstEnd = (uint32_t)size + a3;
    for (int k = 0; k < 3; k++) { W32(a3, R32(a2)); a2 += 4; a3 += 4; }
    while (srcEnd > a2 && dstEnd > a3) {
        uint8_t n = R8(a2++);
        if (n & 0x80) {
            n = (uint8_t)((n & 0x7f) + 3);
            uint8_t c = R8(a2++);
            while (n != 0) { W8(a3++, c); n--; }
        } else {
            while (n != 0) { W8(a3++, R8(a2++)); n--; }
        }
    }
    _HUnlock(h);
    _HPurge(h);
    _HUnlock(nh);
    uint32_t np = R32(nh);
    W16(np, RU16(np) & 0xf100);
    return nh;
}

/* 00054410: flip a shape to the wanted orientation (bit 0 = LR, bit 1 = TB) from its
 * current one (shape bits 0x10 = LR, 0x20 = TB) */
uint32_t FlipSHAP(uint32_t shape, int16_t want) {
    ENTRY_LINK(FN_FlipSHAP, 0, 8);
    uint16_t sel = (uint16_t)((RU16(shape) & 0x30) | (want & 3));
    switch (sel) {
    case 0x01: case 0x10: case 0x23: case 0x32: FlipSHAPLR(shape); break;
    case 0x02: case 0x13: case 0x20: case 0x31: FlipSHAPTB(shape); break;
    case 0x03: case 0x12: case 0x21: case 0x30: FlipSHAPLR(shape); FlipSHAPTB(shape); break;
    default: break;
    }
    return 0;
}

/* 00054546: mirror a shape left/right in place */
uint32_t FlipSHAPLR(uint32_t shape) {
    ENTRY_LINK(FN_FlipSHAPLR, 0xc, 32);
    if (R32(gFlipTablesOK) == 0) FUN_00053d72();
    int16_t type = R16(shape);
    int32_t rb = (int16_t)R16(shape + 2);
    int32_t odd = (rb & 1) ? 1 : 0;
    int32_t rows = (int16_t)R16(shape + 6);
    int32_t half = rb >> 1;
    uint32_t row = shape + 0xc;
    int32_t kind = (int32_t)type & 0xff80;
    maddr tab, midtab;
    int shift;
    if (kind == 0x9000 || kind == 0x9100) {
        if (kind == 0x9100) rows += rows;
        tab = gBitRevTable; midtab = gBitRevTable; shift = 3;
    } else if (kind == 0xa000) {
        tab = gNibSwapTable; midtab = gBitRevTable; shift = 1;
    } else if (kind == 0xb000) {
        tab = 0; midtab = 0; shift = 0;
    } else {
        if (kind == 0xb100) {
            W16(shape, RU16(shape) & 0xfff0);
            W16(shape, RU16(shape) ^ 0x10);
        }
        return 0;
    }
    for (int32_t r = 0; r < rows; r++) {
        uint32_t a2 = row, a3 = row - 1 + (uint32_t)rb;
        for (int32_t i = 0; i < half; i++) {
            if (tab) {
                uint8_t t = R8(tab + R8(a2));
                W8(a2, R8(tab + R8(a3))); a2++;
                W8(a3, t); a3--;
            } else {
                uint8_t t = R8(a2);
                W8(a2, R8(a3)); a2++;
                W8(a3, t); a3--;
            }
        }
        if (tab && odd) W8(a2, R8(midtab + R8(a2)));
        row += (uint32_t)rb;
    }
    W16(shape, RU16(shape) & 0xfff0);
    W16(shape, RU16(shape) ^ 0x10);
    if (R8(shape + 1) & 0x10)
        W16(shape, RU16(shape) | (((uint16_t)((uint16_t)rb << shift) - RU16(shape + 4)) & 0xf));
    return 0;
}

/* 00054720: flip a shape top/bottom in place (both planes of a masked bitmap) */
uint32_t FlipSHAPTB(uint32_t shape) {
    int16_t type = R16(shape);
    int32_t rb = (int16_t)R16(shape + 2);
    int32_t rows = (int16_t)R16(shape + 6);
    int32_t half = rows >> 1;
    int32_t kind = (int32_t)type & 0xff80;
    if (kind == 0xb100) {
        W16(shape, RU16(shape) & 0xfff0);
        W16(shape, RU16(shape) ^ 0x20);
        return 0;
    }
    if (kind != 0x9000 && kind != 0x9100 && kind != 0xa000 && kind != 0xb000) return 0;
    for (int plane = kind == 0x9100 ? 1 : 0; plane >= 0; plane--) {
        uint32_t a4 = shape + 0xc + (plane ? (uint32_t)rows * (uint32_t)rb : 0);
        uint32_t d7 = (uint32_t)(rows - 1) * (uint32_t)rb + a4;
        for (int32_t r = 0; r < half; r++) {
            uint32_t a2 = a4, a3 = d7;
            int32_t c = 4;
            for (; rb > c; c += 4) {
                uint32_t t = R32(a2);
                W32(a2, R32(a3)); a2 += 4;
                W32(a3, t); a3 += 4;
            }
            for (c -= 4; rb > c; c++) {
                uint8_t t = R8(a2);
                W8(a2, R8(a3)); a2++;
                W8(a3, t); a3++;
            }
            a4 += (uint32_t)rb;
            d7 -= (uint32_t)rb;
        }
    }
    W16(shape, RU16(shape) ^ 0x20);
    return 0;
}

/* 00054866: raw shape with color 0 transparent (longs, then the remaining bytes) */
uint32_t PaintTrans(uint32_t src, uint32_t dst, int16_t srb, int16_t drb, int16_t width, int16_t rows) {
    uint32_t w = (uint16_t)width;
    uint16_t r = (uint16_t)(rows - 1);
    do {
        uint32_t a2 = src, a3 = dst;
        uint16_t n4 = (uint16_t)(w >> 2);
        for (uint16_t k = 0; k < n4; k++) {
            uint32_t s = R32(a2); a2 += 4;
            if (s != 0) {
                uint32_t d = R32(a3);
                for (int b = 24; b >= 0; b -= 8)
                    if ((s >> b) & 0xff) d = (d & ~(0xffu << b)) | (s & (0xffu << b));
                W32(a3, d);
            }
            a3 += 4;
        }
        uint16_t n = (uint16_t)(w & 3);
        while (n-- != 0) {
            uint8_t b = R8(a2++);
            if (b) W8(a3, b);
            a3++;
        }
        src += (uint32_t)(int32_t)srb;
        dst += (uint32_t)(int32_t)drb;
    } while (r-- != 0);
    return 0;
}

/* ============================================================= shape lists == */
/* Shape list (PoPNewPtr): +0 SHPL handle, +4 SHPL id, +6 count, then `count` items of
 * 10 bytes from +8 (+6 = shape handle). SHPL resource: +0 first SHAP id - 1, +2 count. */

/* 00054900 */
uint32_t FreeShpInfo(uint32_t info) {
    ENTRY_LINK(FN_FreeShpInfo, 0, 8);
    uint32_t h = R32(info + 6);
    if (h != 0 && R16(info) != 0) {
        _HPurge(h);
        W32(info + 6, 0);
    }
    return 0;
}

/* 00054940: SHPL `id` (fatal error if missing); maps its colors when asked */
uint32_t hGetNMapShpList(int16_t id, uint32_t colors) {
    ENTRY_LINK(FN_hGetNMapShpList, 0, 8);
    uint32_t h = GetSHPLResource(id);
    if (h == 0) {
        uint32_t buf = PoPNewPtr(0x50);
        call_ptr(JT_ADDR(FN_tc_sprintf), "lll", buf, cs(FN_hGetNMapShpList, 0x54940, 0x549c2), (uint32_t)(int32_t)id);
        Terminate(0, buf);
    }
    _HNoPurge(h);
    if (colors != 0) SetCTBLColors(id, -1, colors);
    return h;
}

/* 000549ec: load the shapes of SHPL `id` into a new shape list */
uint32_t LoadShapeList(int16_t id, uint32_t colors) {
    ENTRY_LINK(FN_LoadShapeList, 0x10, 16);
    uint32_t h = hGetNMapShpList(id, colors);
    uint8_t st = (uint8_t)_HGetState(h);
    _HLock(h);
    uint32_t p = R32(h);
    uint32_t list = PoPNewPtr((uint32_t)((int32_t)R16(p + 2) * 10) + 8);
    W32(list, h);
    W16(list + 6, R16(p + 2));
    W16(list + 4, id);
    _HSetState(h, st);
    tc_memset(list + 8, 0, (uint32_t)((int32_t)R16(list + 6) * 10));
    for (int16_t i = 0; i < R16(list + 6); i++) {
        uint32_t s = GetShape((int16_t)(R16(list + 4) + i + 1));
        if (s != 0) {
            if (R16(R32(s) + 6) != 0) {
                W32(list + (uint32_t)((int32_t)i * 10) + 0xe, s);
                _HNoPurge(s);
            } else {
                _HPurge(s);
            }
        }
    }
    return list;
}

/* 00054b10: shape handle of item n (1-based), or 0 */
uint32_t PrinceGetnShapeItem(uint32_t list, int16_t n) {
    if ((int32_t)n - 1 < (int32_t)R16(list + 6))
        return R32(list + 8 + (uint32_t)((int32_t)n * 10) - 4);
    return 0;
}

/* 00054b7a: size (height, width) of item n into *out; returns out, or 0 (also stored
 * into the argument slot) when the shape is missing */
static uint32_t PrinceGetnShapeItemSize_(uint32_t list, int16_t n, uint32_t *out) {
    uint32_t h = R32(list + 8 + (uint32_t)((int32_t)n * 10) - 4);
    if (h != 0) {
        uint32_t p = R32(h);
        if (p != 0) {
            W16(*out, R16(p + 6));
            W16(*out + 2, R16(p + 4));
            return *out;
        }
    }
    *out = 0;
    return *out;
}
uint32_t PrinceGetnShapeItemSize(uint32_t list, int16_t n, uint32_t out) {
    return PrinceGetnShapeItemSize_(list, n, &out);
}
void glue_PrinceGetnShapeItemSize(void) {
    maddr sp = emu_sp();
    uint32_t out = R32(sp + 6);
    uint32_t r = PrinceGetnShapeItemSize_(R32(sp), R16(sp + 4), &out);
    W32(sp + 6, out);
    emu_set_d0(r);
}

/* 00054c00 */
uint32_t PrinceFreeShp(int16_t id) {
    ENTRY_LINK(FN_PrinceFreeShp, 0, 4);
    uint32_t h = GetShape(id);
    if (h != 0) _HPurge(h);
    return 0;
}

/* 00054c34: draw item idx+1 of a shape list, color 0 transparent */
uint32_t PrincePaintShapeIdx(uint32_t list, int16_t idx, int16_t x, int16_t y, int16_t color) {
    ENTRY_LINK(FN_PrincePaintShapeIdx, 0, 8);
    uint32_t h = PrinceGetnShapeItem(list, (int16_t)(idx + 1));
    if (h != 0) {
        _HLock(h);
        DrawIt(R32(h), 0, x, y, 0x10, color, 0);
        _HUnlock(h);
    }
    return 0;
}

/* 00054c9e */
uint32_t UnloadShapeList(uint32_t list) {
    ENTRY_LINK(FN_UnloadShapeList, 4, 20);
    int16_t i = 0;
    if (R16(list + 6) > 0) {
        uint32_t slot = list + 8 + 6;
        do {
            uint32_t h = R32(slot);
            if (h != 0 && R32(h) != 0) {
                _HUnlock(h);
                _HPurge(R32(slot));
                W32(slot, 0);
            }
            i++;
            slot += 10;
        } while (i < R16(list + 6));
    }
    uint32_t shpl = GetSHPLResource(R16(list + 4));
    if (shpl != 0) _HPurge(shpl);
    _DisposePtr(list);
    return 0;
}

/* 00054d36 */
uint32_t GetSHPLResource(int16_t id) {
    ENTRY_LINK(FN_GetSHPLResource, 0, 0);
    return PoPGetResource(SHPL, id);
}

/* 00054d60: shape n (1-based) of a SHPL handle, made unpurgeable */
uint32_t GetShpListItem(uint32_t shpl, int16_t n) {
    ENTRY_LINK(FN_GetShpListItem, 0, 12);
    uint32_t s = 0;
    uint32_t p = R32(shpl);
    if (R16(p + 2) >= n) {
        s = GetShape((int16_t)(R16(p) + n - 1));
        _HNoPurge(s);
    }
    return s;
}

/* 00054dae */
uint32_t PaintRLEShapeItem(uint32_t shpl, int16_t n, int16_t x, int16_t y, int16_t mode) {
    ENTRY_LINK(FN_PaintRLEShapeItem, 0, 8);
    uint32_t s = GetShpListItem(shpl, n);
    if (s != 0) {
        _HLock(s);
        DrawIt(R32(s), 0, x, y, mode, 0, 0);
        _HUnlock(s);
        _HPurge(s);
    }
    return 0;
}

/* 00054e14: (draws nothing: loads the shape and purges it again) */
uint32_t PaintShapeItem(uint32_t shpl, int16_t n) {
    ENTRY_LINK(FN_PaintShapeItem, 0, 8);
    uint32_t s = GetShpListItem(shpl, n);
    if (s != 0) {
        (void)R32(s);
        _HPurge(s);
    }
    return 0;
}

/* 00054e52: (draws nothing either) */
uint32_t Paint4BitShapeItem(uint32_t shpl, int16_t n) {
    ENTRY_LINK(FN_Paint4BitShapeItem, 4, 4);
    uint32_t s = GetShpListItem(shpl, n);
    if (s != 0) _HPurge(s);
    return 0;
}

/* ================================================================== colors == */

/* 00054e8e */
uint32_t GetCTBLResource(int16_t id) {
    ENTRY_LINK(FN_GetCTBLResource, 4, 0);
    return PoPGetResource(CTBL, id);
}

/* 00054eb8: shift the pixel values (+5 of each 4-byte entry) of a CTBL so that the
 * first one is `base` (low byte of the argument) */
uint32_t RemapCTBL(uint32_t h, int16_t base) {
    if (h == 0) return 0;
    uint32_t p = R32(h);
    uint8_t b = (uint8_t)base;
    if (R8(p + 5) == b) return 0;
    int16_t i = 0;
    if (R16(p) <= 0) return 0;
    do {
        W8(p + (uint32_t)((int32_t)i * 4) + 5, (uint8_t)((uint8_t)i + b));
        i++;
    } while (i < R16(p));
    return 0;
}

/* 00054f1e: install CTBL `id` (remapped to `base` unless negative) and show its colors */
uint32_t SetCTBLColors(int16_t id, int16_t base, uint32_t colors) {
    ENTRY_LINK(FN_SetCTBLColors, 0, 12);
    uint32_t h = GetCTBLResource(id);
    if (h == 0) return 0;
    if (base >= 0) RemapCTBL(h, base);
    SetColors(h, 7, 1, 1);
    uint32_t p = R32(h);
    ShowColors(colors, R8(p + 5), R16(p));
    _HPurge(h);
    return 0;
}

/* ================================================================== alerts == */

/* 00054f92 */
uint32_t AlertOKCancel(uint32_t msg) {
    ENTRY_LINK(FN_AlertOKCancel, 0, 4);
    return S16(MainAlert(0x195, msg)) == 1 ? 1 : 0;
}

/* 00054fce */
uint32_t AlertOKOnly(uint32_t msg) {
    ENTRY_LINK(FN_AlertOKOnly, 0, 0);
    return MainAlert(0x194, msg);
}

/* 00054ff2 */
uint32_t AlertYesNo(uint32_t msg) {
    ENTRY_LINK(FN_AlertYesNo, 0, 4);
    return S16(MainAlert(0x196, msg)) == 1 ? 1 : 0;
}

/* 0005502a: dst = src grown by (left, top, right, bottom) */
uint32_t AlterRect(uint32_t dst, uint32_t src, int16_t dl, int16_t dt, int16_t dr, int16_t db) {
    W32(dst, R32(src));
    W32(dst + 4, R32(src + 4));
    W16(dst + 2, R16(dst + 2) + dl);
    W16(dst, R16(dst) + dt);
    W16(dst + 6, R16(dst + 6) + dr);
    W16(dst + 4, R16(dst + 4) + db);
    return dst;
}

/* 00055074: does `a` enclose `b` (union(a, b) == a)? */
uint32_t EncloseRect(uint32_t a, uint32_t b) {
    ENTRY_LINK(FN_EncloseRect, 8, 4);
    pascal_ptr3(R32(pUnionRect), a, b, a6 - 8, 0);
    return _EqualRect(a, a6 - 8) & 0xff;
}

/* 000550b8: poll events; key down/auto key: the character, or 1 for cmd-Q; else 0 */
uint32_t FUN_000550b8(void) {
    ENTRY_NOLINK(FN_FUN_000550b8, 4);
    HandleEvent();
    int16_t r = R16(gEvtWhat);
    if (r == 3 || r == 5) {
        r = (int16_t)(R16(gEvtMsgLo) & 0xff);
        if ((R16(gEvtModifiers) & 0x100) != 0 && r == 0x71) r = 1;
    } else {
        r = 0;
    }
    return (uint16_t)r;
}

/* 000550f2 */
uint32_t hGetDlog(int16_t id) {
    ENTRY_LINK(FN_hGetDlog, 0, 0);
    return _GetResource(0x444c4f47u, id);
}

/* 00055116 */
uint32_t hGetTxt4(int16_t id) {
    ENTRY_LINK(FN_hGetTxt4, 4, 0);
    return PoPGetResource(0x53545220u, id);
}

/* 00055138: rect of a shape placed at (top, left) */
uint32_t MakeShapeRect(uint32_t rect, int16_t top, int16_t left, uint32_t shape) {
    W16(rect, top);
    W16(rect + 2, left);
    W16(rect + 4, (int16_t)(top + R16(shape + 6)));
    W16(rect + 6, (int16_t)(left + R16(shape + 4)));
    return rect;
}

/* 00055182: stop alert `id` showing the C string `msg`; returns the item hit */
uint32_t MainAlert(int16_t id, uint32_t msg) {
    ENTRY_LINK(FN_MainAlert, 0, 8);
    uint32_t buf = PoPNewPtr(tc_strlen(msg) + 1);
    tc_strcpy(buf, msg);
    tc_CtoPstr(buf);
    _ParamText(buf, gEmptyStr, gEmptyStr, gEmptyStr);
    uint16_t item = (uint16_t)_StopAlert(id, 0);
    uint32_t r = _DisposePtr(buf);
    return (r & 0xffff0000u) | item;
}

/* 000551ec */
uint32_t FUN_000551ec(void) { return 0; }

/* 000551f0: copy a Pascal string */
uint32_t pstrcpy(uint32_t dst, uint32_t src) {
    ENTRY_LINK(FN_pstrcpy, 0, 0);
    return _BlockMove(src, dst, (uint32_t)R8(src) + 1);
}

/* 00055216 */
uint32_t PoPFillRect(uint32_t rect, int16_t color) {
    ENTRY_LINK(FN_PoPFillRect, 0, 0);
    FUN_0004fbe2();
    SetForeColor(color);
    _PaintRect(rect);
    seg17_04fc46();
    return 0;
}

/* 00055246: random number in 0..n */
uint32_t Rnd(int16_t n) {
    ENTRY_LINK(FN_Rnd, 0, 4);
    if (R16(gRndSeeded) == 0) SetRndSeed(0);
    uint32_t r = (uint16_t)_Random();
    return (uint32_t)((int32_t)r % ((int32_t)(uint16_t)n + 1));
}

/* 00055282: seed QuickDraw's Random (0: from the clock) */
uint32_t SetRndSeed(int16_t seed) {
    if (seed == 0) W32(qdRandSeed, R32(lmTime));
    else W32(qdRandSeed, (uint16_t)seed);
    W16(gRndSeeded, 1);
    return 0;
}

/* 000552ba */
uint32_t SetScriptRect(void) { return 0; }

/* 000552d4: set a static text item of a dialog from a C string */
uint32_t SetStatTextCtrl(uint32_t dlg, int16_t item, uint32_t text) {
    ENTRY_LINK(FN_SetStatTextCtrl, 0x114, 0);
    _GetDialogItem(dlg, item, a6 - 0x114, a6 - 0x110, a6 - 0x10c);
    if (R32(a6 - 0x110) == 0) return 0;
    uint32_t n = tc_strlen(text);
    W16(a6 - 4, n);
    W8(a6 - 0x104, R8(a6 - 3));
    _BlockMove(text, a6 - 0x103, (uint32_t)(int32_t)(int16_t)n);
    _SetDialogItemText(R32(a6 - 0x110), a6 - 0x104);
    return 0;
}

/* 0005533a: wait for the mouse button to be released */
uint32_t FUN_0005533a(void) {
    ENTRY_NOLINK(FN_FUN_0005533a, 0);
    uint32_t r = _StillDown();
    if ((r & 0xff) != 0) {
        do {
            CheckWaitFunc();
            r = _StillDown();
        } while ((r & 0xff) != 0);
    }
    return r & 0xff;
}

/* 00055350: RLE shape drawn upside down (bottom row first) */
static uint32_t PaintRLETB_(uint32_t *shape, uint32_t pmh, int16_t transparent, int16_t width, int16_t *rows,
                            int16_t skip, int16_t skiprows, int16_t dx, int16_t dy) {
    ENTRY_LINK(FN_PaintRLETB, 0x12, 0);
    RLESetup s = rle_setup(shape, pmh, skiprows, dx, dy);
    uint32_t d0 = swap_mmu(0);
    uint8_t mode = (uint8_t)d0;
    rle_rows(s, transparent, width, rows, skip, d0, -1);
    swap_mmu(mode);
    return 0;
}

uint32_t PaintRLETB(uint32_t p4, uint32_t p8, int16_t p12, int16_t p14, int16_t p16, int16_t p18,
                    int16_t p20, int16_t p22, int16_t p24) {
    return PaintRLETB_(&p4, p8, p12, p14, &p16, p18, p20, p22, p24);
}
RLE_GLUE(PaintRLETB)
