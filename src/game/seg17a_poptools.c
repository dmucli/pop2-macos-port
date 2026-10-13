/* CODE 17 "PoP Tools", first part (Ghidra 0x4e444-0x51ede): general tools.
 *
 *  - RLE sprite blitters (BankRLE / BankRLELR, hand-written assembly with a color bank
 *    offset added to every pixel);
 *  - the palette layer: gColorTable (a 256-entry ColorTable handle) and gPalette (a
 *    Palette Manager palette), SetColors (scale/convert a 'CTBL'-like "vCtab" into the color
 *    table), ShowColors (AnimatePalette), fades, venetian-blind transitions, standard colors;
 *  - rectangles, random numbers, tick waits and the 6-byte timer table (SetTimer/GetTimer/
 *    WaitTimer), dialogs (About, system check), text drawing in a rectangle with word wrap;
 *  - error reporting and exit (ErrorHandler, CheckOSErr, Terminate, ShowErrorAndExit),
 *    resource/memory helpers that retry after purging the background sets, LZSS packing.
 *
 * A "vCtab" is a handle: word count, then 4-byte entries {r, g, b, index} (8-bit values).
 *
 * Ported from the Ghidra decompilation, checked instruction by instruction against the
 * disassembly; addresses in comments are Ghidra addresses (see docs/PORTING.md).
 *
 * Functions that write their own argument slots (BankRLE*, FadeIn/OutColors,
 * GiveSystemColors, Randomize, Concactenate, CompareTicks, TextInRectHeight) do it through
 * the ENTRY frame (args_ / a6 + 8...): when entered from 68k code these are the caller's
 * real slots; when called from C they are the argument bytes ENTRY reserves, which the
 * original caller would have pushed. The slot is first (re)written with the argument value
 * so the C-called case behaves like the original too. */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* ---- globals (Ghidra addresses) ---- */
#define gColorTable     GH(0x102cbe)   /* long: CTabHandle, 256 entries (the game's colors) */
#define gPalette        GH(0x102cc2)   /* long: PaletteHandle of the game window */
#define gScreenDepth    GH(0x102ca8)   /* word: wanted screen depth (8 or 1, 0 = any) */
#define gDepthOK        GH(0x102caa)   /* byte: RecheckSystem found the wanted depth */
#define gActive         GH(0x102cac)   /* byte: the application is in front */
#define gPauseMark      GH(0x102db6)   /* word: game menu mark (2 = locked) */
#define gGameWindow     GH(0x102dbe)   /* long: the game window */
#define gOffWorld       GH(0x102dc2)   /* the off-screen world record */
#define gScreenPort     GH(0x102dd6)   /* long: CGrafPtr whose pixmap bounds is the screen rect */
#define gBGSetState     GH(0x103906)   /* word: 1 = background sets already purgeable */
#define gMasterVolume   GH(0x106074)   /* word: volume to restore on exit (<0 = none) */
#define gClipRectProc   GH(0x10609c)   /* long: cached ClipRect trap address */
#define gSectRectProc   GH(0x1060a8)   /* long: cached SectRect trap address */
#define gSHAPColors     GH(0x1060ac)   /* long: 'CTBL' handle of the current shape colors */
#define gStdColors      GH(0x1060b0)   /* RGBColor[8]: standard colors (entries 0..7) */
#define gSavedBkRGB     GH(0x1060e0)   /* RGBColor: saved port rgbBkColor */
#define gSavedFgRGB     GH(0x1060e6)   /* RGBColor: saved port rgbFgColor */
#define gSavedBkIdx     GH(0x1060ec)   /* word: saved port bkColor */
#define gSavedFgIdx     GH(0x1060ee)   /* word: saved port fgColor */
#define gSysReds        GH(0x1060f0)   /* byte[6]: GiveSystemColors red components */
#define gSysGreens      GH(0x1060f6)   /* byte[6]: green */
#define gSysBlues       GH(0x1060fc)   /* byte[6]: blue */
#define gSysErrText     GH(0x106102)   /* Str255: text for the "wrong system" dialog */
#define gHasColorQD     GH(0x106202)   /* byte: SysEnvirons hasColorQD */
#define gReadKeyProc    GH(0x10620c)   /* long: key reader procedure (CheckWaitFunc) */
#define gEmptyStr       GH(0x106210)   /* Str: ParamText filler */
#define gTimerStart     GH(0x106310)   /* timers: 6-byte records {long start ticks, */
#define gTimerDur       GH(0x106314)   /*                          word duration}   */
#define gLZTextBuf      GH(0x106472)   /* long: LZSS ring buffer (0x442 bytes) */
#define gLZRson         GH(0x106476)   /* long: LZSS right sons (0x501 words) */
#define gLZLson         GH(0x10647a)   /* long: LZSS left sons */
#define gLZDad          GH(0x10647e)   /* long: LZSS parents */
#define gLZMatchLen     GH(0x10646e)   /* word: LZSS match length */
#define gLZMatchPos     GH(0x106470)   /* word: LZSS match position */
#define gThePort        GH(0x106782)   /* long: QuickDraw thePort */
#define gScrTop         GH(0x10670e)   /* qd.screenBits.bounds */
#define gScrLeft        GH(0x106710)
#define gScrBottom      GH(0x106712)
#define gScrRight       GH(0x106714)

#define CTBL_TYPE       0x4354424cu    /* 'CTBL' */

/* code-segment constants (strings) of this segment */
#define CS(fn, fnaddr, a)  (fn_address(fn) + (uint32_t)((a) - (fnaddr)))

/* _SwapMMUMode takes its mode in D0 (the generated wrapper does not set it) */
static uint32_t swap_mmu(uint32_t d0) {
    tb_begin();
    cpu_set(R_D0, d0);
    tb_trap(0xA05D);
    uint32_t r = cpu_get(R_D0);
    tb_end();
    return r;
}

static void copy6(maddr dst, maddr src) { W32(dst, R32(src)); W16(dst + 4, RU16(src + 4)); }

/* ======================================================================== */

/* 0004e444: draw one RLE-packed image into a pixmap, mirrored (right to left), adding a
 * color bank to every pixel. shape (stripped) +0xc: rows {word len; data}; index = first
 * row; skip = pixels skipped at the start of each row; width = pixels drawn per row.
 * Control byte: bit 7 = fill run (next byte, 0 = transparent), else literal; count+1. */
uint32_t BankRLELR(uint32_t shape, uint32_t pm, int16_t width, int16_t rows, int16_t skip,
                   int16_t index, int16_t x, int16_t y, int16_t bank) {
    ENTRY_LINK(FN_BankRLELR, 0x12, 0);
    W32(a6 + 8, shape); W16(a6 + 0x12, rows);           /* own slots, written below */
    uint16_t rowBytes = RU16(DEREF(pm) + 4) & 0x7ffe;
    W16(a6 - 0x12, rowBytes);
    uint32_t base = _GetPixBaseAddr(pm);
    base = (uint32_t)((int32_t)y * (int16_t)rowBytes) + base + (uint32_t)(int32_t)x;
    W32(a6 - 0x10, base);
    shape = tc_StripAddress(shape);
    W32(a6 + 8, shape);                                  /* move.l (SP)+,(8,A6) */
    uint32_t p = shape + 0xc;
    int16_t i;
    for (i = 0; i < index; i++) p = p + 2 + (uint32_t)(int32_t)R16(p);
    W32(a6 - 8, p); W16(a6 - 4, i);
    uint32_t data = p + 2;
    W32(a6 - 0xc, data);
    W16(a6 - 2, 1);
    uint32_t d0 = swap_mmu((data & 0xffffff00u) | R8(a6 - 2));   /* reads the high byte: 0 */
    W8(a6 - 2, d0);
    /* register image of the assembly loop */
    uint8_t d2 = (uint8_t)bank;
    uint32_t a3 = p, a0 = data, a4 = base;
    uint16_t oldRows;
    do {
        uint32_t a2 = a4 + (uint32_t)(int32_t)width;
        uint32_t a1 = a0;
        d0 = (d0 & 0xffff0000u) | (uint16_t)width;
        uint16_t d6 = (uint16_t)skip;
        uint32_t d1, d7 = 0;
        if (d6 != 0) {
            for (;;) {
                d7 = R8(a1); a1++;
                d1 = d7 & 0x7f;
                if (d6 <= (uint16_t)d1) break;
                if (!(d7 & 0x80)) a1 += d1;
                a1++;
                d6 = (uint16_t)(d6 - d1 - 1);
            }
            if (!(d7 & 0x80)) a1 += (uint32_t)(int32_t)(int16_t)d6;
            d7 = (d7 & 0xffffff00u) | (uint8_t)(d7 - d6);
            d1 = d7;
        } else {
            d1 = R8(a1); a1++;
        }
        for (;;) {
            uint16_t d7w = (uint16_t)(d1 & 0xff7f);
            if (!((int16_t)d7w < (int16_t)(uint16_t)d0)) {
                uint16_t t = (uint16_t)(d7w - (uint16_t)d0);
                d1 = (d1 & 0xffff0000u) | (uint16_t)((uint16_t)d1 - t - 1);
            }
            int fill = (d1 & 0x80) != 0;
            d1 &= ~0x80u;
            uint16_t d5 = (uint16_t)d1;
            if (fill) {
                uint8_t d3 = R8(a1); a1++;
                if (d3 != 0) {
                    d3 = (uint8_t)(d3 + d2);
                    do { a2--; W8(a2, d3); } while (d5-- != 0);
                } else {
                    do { a2--; } while (d5-- != 0);
                }
            } else {
                do { uint8_t c = (uint8_t)(R8(a1) + d2); a1++; a2--; W8(a2, c); } while (d5-- != 0);
            }
            d1 += 1;
            uint32_t old = d0;
            d0 -= d1;
            if (!(old > d1)) break;                       /* bhi */
            d1 = R8(a1); a1++;
        }
        a4 += (uint32_t)(int32_t)(int16_t)rowBytes;
        a3 += (uint32_t)(int32_t)R16(a3);
        a3 += 2;
        a0 = a3 + 2;
        oldRows = RU16(a6 + 0x12);
        W16(a6 + 0x12, oldRows - 1);                      /* subq.w #1,(0x12,A6) */
    } while (oldRows > 1);
    return swap_mmu((d0 & 0xffffff00u) | R8(a6 - 2));
}

/* 0004e59a: same, drawn left to right; literal and fill runs of 8+ pixels are copied a
 * long at a time, and the bank is a long added to every long (or byte) copied. */
uint32_t BankRLE(uint32_t shape, uint32_t pm, int16_t width, int16_t rows, int16_t skip,
                 int16_t index, int16_t x, int16_t y, uint32_t bank) {
    ENTRY_LINK(FN_BankRLE, 0x12, 0);
    W32(a6 + 8, shape); W16(a6 + 0x12, rows);
    uint16_t rowBytes = RU16(DEREF(pm) + 4) & 0x7ffe;
    W16(a6 - 0x12, rowBytes);
    uint32_t base = _GetPixBaseAddr(pm);
    base = (uint32_t)((int32_t)y * (int16_t)rowBytes) + base + (uint32_t)(int32_t)x;
    W32(a6 - 0x10, base);
    shape = tc_StripAddress(shape);
    W32(a6 + 8, shape);
    uint32_t p = shape + 0xc;
    int16_t i;
    for (i = 0; i < index; i++) p = p + 2 + (uint32_t)(int32_t)R16(p);
    W32(a6 - 8, p); W16(a6 - 4, i);
    uint32_t data = p + 2;
    W32(a6 - 0xc, data);
    W16(a6 - 2, 1);
    uint32_t d0 = swap_mmu((data & 0xffffff00u) | R8(a6 - 2));
    W8(a6 - 2, d0);
    uint32_t d2 = bank, d3 = 0;
    uint32_t a3 = p, a0 = data, a4 = base;
    uint16_t oldRows;
    do {
        uint32_t a2 = a4;
        uint32_t a1 = a0;
        d0 = (d0 & 0xffff0000u) | (uint16_t)width;
        uint16_t d6 = (uint16_t)skip;
        uint32_t d1, d7 = 0;
        if (d6 != 0) {
            for (;;) {
                d7 = R8(a1); a1++;
                d1 = d7 & 0x7f;
                if (d6 <= (uint16_t)d1) break;
                if (!(d7 & 0x80)) a1 += d1;
                a1++;
                d6 = (uint16_t)(d6 - d1 - 1);
            }
            if (!(d7 & 0x80)) a1 += (uint32_t)(int32_t)(int16_t)d6;
            d7 = (d7 & 0xffffff00u) | (uint8_t)(d7 - d6);
            d1 = d7;
        } else {
            d1 = R8(a1); a1++;
        }
        for (;;) {
            uint16_t d7w = (uint16_t)(d1 & 0xff7f);
            if (!((int16_t)d7w < (int16_t)(uint16_t)d0)) {
                uint16_t t = (uint16_t)(d7w - (uint16_t)d0);
                d1 = (d1 & 0xffff0000u) | (uint16_t)((uint16_t)d1 - t - 1);
            }
            int fill = (d1 & 0x80) != 0;
            d1 &= ~0x80u;
            uint16_t d5 = (uint16_t)d1;
            if (fill) {
                d3 = (d3 & 0xffffff00u) | R8(a1); a1++;
                if ((uint8_t)d3 != 0) {
                    d3 = (d3 & 0xffffff00u) | (uint8_t)(d3 + d2);
                    if (d5 >= 8) {
                        d5 = (uint16_t)((d5 >> 2) - 1);
                        uint8_t b = (uint8_t)d3;
                        d3 = (uint32_t)b * 0x01010101u;   /* bfins x2: byte replicated */
                        do { W32(a2, d3); a2 += 4; } while (d5-- != 0);
                        d5 = (uint16_t)(d1 & 3);          /* bfextu D1{30:2},D5 */
                    }
                    do { W8(a2, d3); a2++; } while (d5-- != 0);
                } else {
                    a2 = a2 + 1 + (uint32_t)(int32_t)(int16_t)d1;   /* lea (1,A2,D1.w) */
                }
            } else {
                if (d5 >= 8) {
                    d5 = (uint16_t)((d5 >> 3) - 1);
                    do {
                        d3 = R32(a1) + d2; a1 += 4; W32(a2, d3); a2 += 4;
                        d3 = R32(a1) + d2; a1 += 4; W32(a2, d3); a2 += 4;
                    } while (d5-- != 0);
                    d5 = (uint16_t)(d1 & 7);
                }
                do {
                    d3 = (d3 & 0xffffff00u) | (uint8_t)(R8(a1) + d2); a1++;
                    W8(a2, d3); a2++;
                } while (d5-- != 0);
            }
            d1 += 1;
            uint32_t old = d0;
            d0 -= d1;
            if (!(old > d1)) break;
            d1 = R8(a1); a1++;
        }
        a4 += (uint32_t)(int32_t)(int16_t)rowBytes;
        a3 += (uint32_t)(int32_t)R16(a3);
        a3 += 2;
        a0 = a3 + 2;
        oldRows = RU16(a6 + 0x12);
        W16(a6 + 0x12, oldRows - 1);
    } while (oldRows > 1);
    return swap_mmu((d0 & 0xffffff00u) | R8(a6 - 2));
}

/* 0004e71e: copy a vCtab into gColorTable. flags: 1/2/4 = keep red/green/blue (else 0),
 * 8 = gray (average), 0x10 = invert, 0x20 = scale by num/den, 0x40 = no black when
 * scaling. Returns the handle when nothing is done, else HUnlock's D0. */
uint32_t SetColors(uint32_t h, int16_t flags, int16_t num, int16_t den) {
    ENTRY_LINK(FN_SetColors, 0x3c, 28);                  /* D3-D7/A3-A4 */
    int16_t n = h ? R16(DEREF(h)) : 0;
    uint32_t ct = R32(gColorTable);
    if (ct == 0 || n == 0) return ct;
    _HLock(R32(gColorTable));
    int32_t size = (int32_t)R16(DEREF(R32(gColorTable)) + 6) + 1;
    if (size >= (int32_t)n && num >= 0 && den > 0) {
        uint32_t a3 = DEREF(h) + 2;
        for (int16_t i = 0; i < n; i++, a3 += 4) {
            uint16_t idx = R8(a3 + 3);
            uint32_t off = (uint32_t)((int32_t)(int16_t)idx << 3);
            if (flags & 8) {
                int32_t v = (int32_t)R8(a3) + R8(a3 + 1) + R8(a3 + 2);
                if (flags & 0x20) {
                    if ((flags & 0x40) && v == 0) v = 1;
                    v = (int32_t)((uint32_t)v * (uint32_t)(int32_t)num);
                    v = v / (int32_t)den;
                }
                v = v / 3;
                if (v > 0xff) v = 0xff;
                v = (int32_t)((uint32_t)v * 0x101u);
                if (flags & 0x10) v = -1 - v;
                uint16_t c = (uint16_t)v;
                W16(DEREF(R32(gColorTable)) + off + 0xe, c);
                W16(DEREF(R32(gColorTable)) + off + 0xc, c);
                W16(DEREF(R32(gColorTable)) + off + 0xa, c);
            } else {
                int32_t r = R8(a3), g = R8(a3 + 1), b = R8(a3 + 2);
                uint32_t a4 = off + DEREF(R32(gColorTable)) + 8;
                W16(a4, idx);
                if (flags & 0x20) {
                    if (flags & 0x40) {
                        int32_t avg = (r + g + b) / 3 + 1;
                        if (r == 0) r = avg;
                        if (g == 0) g = avg;
                        if (b == 0) b = avg;
                    }
                    r = (int32_t)((uint32_t)r * (uint32_t)(int32_t)num) / (int32_t)den;
                    g = (int32_t)((uint32_t)g * (uint32_t)(int32_t)num) / (int32_t)den;
                    b = (int32_t)((uint32_t)b * (uint32_t)(int32_t)num) / (int32_t)den;
                    if (r > 0xff) r = 0xff;
                    if (g > 0xff) g = 0xff;
                    if (b > 0xff) b = 0xff;
                }
                r = (int32_t)((uint32_t)r * 0x101u);
                g = (int32_t)((uint32_t)g * 0x101u);
                b = (int32_t)((uint32_t)b * 0x101u);
                if (flags & 0x10) { r = -1 - r; g = -1 - g; b = -1 - b; }
                W16(a4 + 2, (flags & 1) ? (uint16_t)r : 0);
                W16(a4 + 4, (flags & 2) ? (uint16_t)g : 0);
                W16(a4 + 6, (flags & 4) ? (uint16_t)b : 0);
            }
        }
    }
    return _HUnlock(R32(gColorTable));
}

/* 0004ead4: show count entries of gColorTable from start in the window's palette */
uint32_t ShowColors(uint32_t win, int16_t start, int16_t count) {
    ENTRY_LINK(FN_ShowColors, 0, 4);                     /* D3 */
    if (start < 0) return cpu_get(R_D0);                 /* D0 untouched */
    int32_t end = (int32_t)start + count;
    if (end > 0x100) return (uint32_t)end;
    return _AnimatePalette(win, R32(gColorTable), start, start, count);
}

/* 0004eb1a: rotate count entries of gColorTable (from start) by one */
uint32_t CycleColors(int16_t start, int16_t count) {
    uint32_t a3 = (uint32_t)((int32_t)start << 3) + DEREF(R32(gColorTable)) + 8;
    uint32_t s0 = R32(a3 + 2);
    uint16_t s1 = RU16(a3 + 6);
    int32_t d3 = (int32_t)count - 1;
    int16_t d4 = 0;
    if (d3 > 0) {
        do { copy6(a3 + 2, a3 + 10); a3 += 8; d4++; } while (d3 > (int32_t)d4);
    }
    W32(a3 + 2, s0); W16(a3 + 6, s1);
    return 0;
}

/* 0004eb9a */
uint32_t ReactivatePalette(uint32_t win) {
    ENTRY_LINK(FN_ReactivatePalette, 0, 0);
    ShowColors(win, 1, 0xfe);
    return _ActivatePalette(win);
}

/* 0004ebd0: create gPalette and gColorTable for the window */
uint32_t SetUpPalette(uint32_t win, int16_t start, int16_t count) {
    ENTRY_LINK(FN_SetUpPalette, 0, 12);                  /* D3-D5 */
    if (start < 0) return cpu_get(R_D0);
    int32_t end = (int32_t)start + count;
    if (end > 0x100) return (uint32_t)end;
    W32(gPalette, _NewPalette(0x100, 0, 0xc, 0));
    for (int16_t i = 0; i < 8; i++) _SetEntryColor(R32(gPalette), i, gStdColors + (uint32_t)(i * 6));
    uint32_t ct = PoPNewHandle(0x808);
    W32(gColorTable, ct);
    _Palette2CTab(R32(gPalette), ct);
    for (int16_t i = 0; i < start; i++) _SetEntryUsage(R32(gPalette), i, 2, 0);
    _SetEntryUsage(R32(gPalette), 0xff, 2, 0);
    _NSetPalette(win, R32(gPalette), 0x0100);           /* move.b #1 */
    return _AnimatePalette(win, R32(gColorTable), start, start, count);
}

/* 0004eccc: give a window the game palette */
uint32_t AddWindow(uint32_t win) {
    ENTRY_LINK(FN_AddWindow, 0, 0);
    return _NSetPalette(win, R32(gPalette), 0x0100);
}

/* 0004ecf0: rebuild the palette from gColorTable (flag in the high byte: also make
 * entries 1..254 explicit+animated again) */
uint32_t TakeBackReleaseColors(uint32_t win, int16_t flag) {
    ENTRY_LINK(FN_TakeBackReleaseColors, 0, 4);          /* D3 */
    if (R32(gColorTable) == 0) return 0;
    if (((uint16_t)flag >> 8) != 0) {
        _CTab2Palette(R32(gColorTable), R32(gPalette), 2, 0);
        for (int16_t i = 1; i < 0xff; i++) _SetEntryUsage(R32(gPalette), i, 0xc, 0);
    } else {
        _CTab2Palette(R32(gColorTable), R32(gPalette), 2, 0);
    }
    _Palette2CTab(R32(gPalette), R32(gColorTable));
    return _ActivatePalette(win);
}

/* 0004ed72: load 'CTBL' id as the shape colors */
uint32_t GetSHAPColors(int16_t id) {
    ENTRY_LINK(FN_GetSHAPColors, 0, 0);
    if (R32(gSHAPColors) != 0) {
        _HUnlock(R32(gSHAPColors));
        _HPurge(R32(gSHAPColors));
        W32(gSHAPColors, 0);
    }
    uint32_t h = _GetResource(CTBL_TYPE, id);
    W32(gSHAPColors, h);
    return h;
}

/* 0004edb8: copy the RGB of color table entry n (byte argument) */
uint32_t GetNthvRGB(int16_t n, uint32_t rgb) {
    uint32_t src = (uint32_t)(((uint16_t)n >> 8) << 3) + DEREF(R32(gColorTable)) + 8 + 2;
    copy6(rgb, src);
    return 0;
}

/* 0004edee: set the 8-bit RGB of entry n of the shape colors (byte arguments) */
uint32_t SetNthvRGB(int16_t n, int16_t r, int16_t g, int16_t b) {
    ENTRY_LINK(FN_SetNthvRGB, 8, 20);                    /* D3-D6/A2 */
    if (R32(gSHAPColors) == 0) return 0;
    _HLock(R32(gSHAPColors));
    int found = 0;
    int16_t count = R16(DEREF(R32(gSHAPColors)));
    for (int16_t i = 0; !found && i < count; i++) {
        uint32_t off = (uint32_t)((int32_t)i << 2);
        uint32_t a2 = off + DEREF(R32(gSHAPColors));
        if (R8(a2 + 5) == (uint8_t)((uint16_t)n >> 8)) {
            W8(a2 + 2, (uint16_t)r >> 8);
            W8(off + DEREF(R32(gSHAPColors)) + 3, (uint16_t)g >> 8);
            W8(off + DEREF(R32(gSHAPColors)) + 4, (uint16_t)b >> 8);
            found = 1;
        }
    }
    return _HUnlock(R32(gSHAPColors));
}

/* 0004ee92: install 'CTBL' id and show it */
uint32_t SetSHAPColors(uint32_t win, int16_t id) {
    ENTRY_LINK(FN_SetSHAPColors, 0, 8);                  /* D3/A2 */
    uint32_t h = _GetResource(CTBL_TYPE, id);
    if (h == 0) return 0;
    SetColors(h, 7, 1, 1);
    ShowColors(win, 1, 0xfe);
    _HUnlock(h);
    return _HPurge(h);
}

static void free_shap_colors(void) {
    if (R32(gSHAPColors) != 0) {
        _HUnlock(R32(gSHAPColors));
        _HPurge(R32(gSHAPColors));
        W32(gSHAPColors, 0);
    }
}

/* 0004eef6: fade colors (h, or the shape colors) in from black over time. speed: low
 * word = ticks factor; when >= 0xffff, bits 16..18 give the step divisor and the slot
 * keeps only the low 5 bits. */
uint32_t FadeInColors(uint32_t h, uint32_t win, uint32_t speed) {
    ENTRY_LINK(FN_FadeInColors, 8, 20);                  /* D3-D7 */
    W32(a6 + 0x10, speed);
    int16_t d5 = 9;
    if (speed >= 0xffffu) {
        d5 = (int16_t)(((int32_t)speed >> 16) & 7);
        speed &= 0x1f;
        W32(a6 + 0x10, speed);                           /* andi.l #$1f,(0x10,A6) */
    }
    if (d5 == 0) port_divide_by_zero(__func__);
    int16_t d7 = (int16_t)((int32_t)(int16_t)(100 / (int32_t)d5) * (int32_t)R16(a6 + 0x12));
    int32_t start = (int32_t)_TickCount();
    int16_t d6 = -1, d4 = 0;
    while (d4 < 100) {
        if (d4 != d6) {
            d6 = d4;
            SetColors(h ? h : R32(gSHAPColors), 0x27, d4, 100);
            ShowColors(win, 1, 0xfe);
        }
        int32_t el = (int32_t)_TickCount() - start;
        if (d7 == 0) port_divide_by_zero(__func__);
        int16_t q = (int16_t)((int32_t)((uint32_t)el * 100u) / (int32_t)d7);
        d4 = q < 100 ? q : 100;
    }
    SetColors(h ? h : R32(gSHAPColors), 0x27, 1, 1);
    ShowColors(win, 1, 0xfe);
    free_shap_colors();
    return 0;
}

/* 0004f04a: show the shape colors at once */
uint32_t NOFadeSetColors(uint32_t win) {
    ENTRY_LINK(FN_NOFadeSetColors, 0, 4);
    SetColors(R32(gSHAPColors), 0x27, 1, 1);
    ShowColors(win, 1, 0xfe);
    free_shap_colors();
    return 0;
}

/* 0004f0a8: fade the shape colors to white, one timer period per step */
uint32_t FadeToWhite(uint32_t win, uint32_t ticks) {
    ENTRY_LINK(FN_FadeToWhite, 0, 16);                   /* D3-D6 */
    int16_t d6 = 1;
    do {
        SetTimer(1, (int16_t)(ticks >> 16), (int16_t)ticks);
        SetColors(R32(gSHAPColors), 0x67, d6, 1);
        ShowColors(win, 1, 0xfe);
        WaitTimerNoPoll(1);
        d6 = (int16_t)(d6 + d6 + 1);
    } while (d6 < 0x100);
    SetColors(R32(gSHAPColors), 0x67, 0x100, 1);
    ShowColors(win, 1, 0xfe);
    free_shap_colors();
    return 0;
}

/* 0004f14e: fade colors (h, or the shape colors) out to black over time */
uint32_t FadeOutColors(uint32_t h, uint32_t win, uint32_t speed) {
    ENTRY_LINK(FN_FadeOutColors, 8, 20);                 /* D3-D7 */
    W32(a6 + 0x10, speed);
    int16_t d5;
    if (speed >= 0xffffu) {
        d5 = (int16_t)(((int32_t)speed >> 16) & 7);
        speed &= 0x1f;
        W32(a6 + 0x10, speed);
    } else {
        d5 = 9;
    }
    if (d5 == 0) port_divide_by_zero(__func__);
    int16_t d7 = (int16_t)((int32_t)(int16_t)(100 / (int32_t)d5) * (int32_t)R16(a6 + 0x12));
    int32_t start = (int32_t)_TickCount();
    int16_t d6 = -1, d4 = 100;
    do {
        if (d4 != d6) {
            d6 = d4;
            SetColors(h ? h : R32(gSHAPColors), 0x27, d4, 100);
            ShowColors(win, 1, 0xfe);
        }
        int32_t el = (int32_t)_TickCount() - start;
        if (d7 == 0) port_divide_by_zero(__func__);
        int16_t q = (int16_t)((int32_t)((uint32_t)el * 100u) / (int32_t)d7);
        d4 = (int16_t)(100 - q);
        if (d4 <= 0) d4 = 0;
    } while (d4 > 0);
    SetColors(h ? h : R32(gSHAPColors), 0x27, 0, 1);
    ShowColors(win, 1, 0xfe);
    free_shap_colors();
    return 0;
}

/* 0004f2a4: set fore/back colors (flag byte: swapped) */
uint32_t StandardColors(int16_t flag, int16_t a, int16_t b) {
    ENTRY_LINK(FN_StandardColors, 0, 0);
    if (((uint16_t)flag >> 8) != 0) { SetForeColor(a); return SetBackColor(b); }
    SetForeColor(b);
    return SetBackColor(a);
}

/* 0004f2ee: foreground = color table entry idx */
uint32_t SetForeColor(int16_t idx) {
    ENTRY_LINK(FN_SetForeColor, 0, 4);                   /* A2 */
    uint32_t a2 = DEREF(R32(gColorTable));
    _RGBForeColor((uint32_t)((int32_t)idx << 3) + a2 + 8 + 2);
    W32(R32(gThePort) + 0x50, (int32_t)idx);             /* fgColor */
    return (uint32_t)(int32_t)idx;
}

/* 0004f334: set entry n (byte) to an RGBColor and show it */
uint32_t SetAndShowNthRGB(int16_t n, uint32_t rgb, uint32_t win) {
    ENTRY_LINK(FN_SetAndShowNthRGB, 0, 8);               /* D3/A2 */
    if (R32(gColorTable) == 0) return 0;
    _HLock(R32(gColorTable));
    uint32_t a2 = DEREF(R32(gColorTable));
    uint8_t i = (uint8_t)((uint16_t)n >> 8);
    copy6((uint32_t)i * 8 + a2 + 8 + 2, rgb);
    ShowColors(win, i, 1);
    return _HUnlock(R32(gColorTable));
}

/* 0004f3a0: background = color table entry idx */
uint32_t SetBackColor(int16_t idx) {
    ENTRY_LINK(FN_SetBackColor, 0, 0);
    _RGBBackColor((uint32_t)((int32_t)idx << 3) + DEREF(R32(gColorTable)) + 8 + 2);
    W32(R32(gThePort) + 0x54, (int32_t)idx);             /* bkColor */
    return (uint32_t)(int32_t)idx;
}

/* 0004f3e0: venetian-blind erase of rect (in steps of `step` pixels, horizontal strips if
 * the flag byte is set, else vertical), then show the erased off-screen world */
uint32_t VenetianOut(uint32_t r, int16_t step, uint32_t wait, int16_t flag) {
    ENTRY_LINK(FN_VenetianOut, 0x18, 16);                /* D3/D5-D7 */
    int16_t depth = R16(gScreenDepth);
    if (depth != 8 && depth != 1) return 0;
    uint32_t src = DEREF(R32(R32(gScreenPort) + 2)) + 6;
    W32(a6 - 0x18, R32(src)); W32(a6 - 0x14, R32(src + 4));
    int16_t d5 = step < 5 ? 5 : step;
    if ((_SectRect(a6 - 0x18, r, a6 - 0x10) & 0xff) != 0) {
        W32(a6 - 8, R32(a6 - 0x10)); W32(a6 - 4, R32(a6 - 0xc));
        if (((uint16_t)flag >> 8) != 0) {
            int16_t d6 = R16(a6 - 0x10);
            StandardColors(0x0100, 0xff, 0);
            if (!((int32_t)R16(a6 - 0x10) + d5 < (int32_t)d6)) {
                do {
                    W16(a6 - 8, d6);
                    W16(a6 - 4, d6 + 1);
                    while (R16(a6 - 4) <= R16(a6 - 0xc)) {
                        _PaintRect(a6 - 8);
                        W16(a6 - 8, R16(a6 - 8) + d5);
                        W16(a6 - 4, R16(a6 - 4) + d5);
                    }
                    WaitFor(wait);
                    d6++;
                } while (!((int32_t)R16(a6 - 0x10) + d5 < (int32_t)d6));
            }
        } else {
            int16_t d6 = R16(a6 - 0xe);
            StandardColors(0x0100, 0xff, 0);
            if (!((int32_t)R16(a6 - 0xe) + d5 < (int32_t)d6)) {
                do {
                    W16(a6 - 6, d6);
                    W16(a6 - 2, d6 + 1);
                    while (R16(a6 - 2) <= R16(a6 - 0xa)) {
                        _PaintRect(a6 - 8);
                        W16(a6 - 6, R16(a6 - 6) + d5);
                        W16(a6 - 2, R16(a6 - 2) + d5);
                    }
                    WaitFor(wait);
                    d6++;
                } while ((int32_t)R16(a6 - 0xe) + d5 >= (int32_t)d6);
            }
        }
    }
    SaveCurrentWorld(gOffWorld);
    StandardColors(0x0100, 0xff, 0);
    _PaintRect(a6 - 0x10);
    RestoreOriginalWorld(gOffWorld);
    DrawSOffWorldToWindow(gOffWorld, R32(gGameWindow), a6 - 0x10, a6 - 0x10, 0);
    return 0;
}

/* 0004f5a8: venetian-blind reveal of rect from the off-screen world */
uint32_t VenetianIn(uint32_t r, int16_t step, uint32_t wait, int16_t flag) {
    ENTRY_LINK(FN_VenetianIn, 0x28, 16);                 /* D3-D6 */
    int16_t depth = R16(gScreenDepth);
    if (depth != 8 && depth != 1) return 0;
    uint32_t src = DEREF(R32(R32(gScreenPort) + 2)) + 6;
    W32(a6 - 0x28, R32(src)); W32(a6 - 0x24, R32(src + 4));
    int16_t d5 = step < 5 ? 5 : step;
    if ((_SectRect(a6 - 0x28, r, a6 - 0x20) & 0xff) != 0) {
        W32(a6 - 0x18, R32(a6 - 0x20)); W32(a6 - 0x14, R32(a6 - 0x1c));
        if (((uint16_t)flag >> 8) != 0) {
            int16_t d6 = R16(a6 - 0x20);
            int32_t t = R16(a6 - 0x20);
            W32(a6 - 0xc, t);
            int32_t d4 = (int32_t)d5;
            if (!(t + d4 < t)) {
                do {
                    W16(a6 - 0x18, d6);
                    W16(a6 - 0x14, d6 + 1);
                    while (R16(a6 - 0x14) <= R16(a6 - 0x1c)) {
                        DrawSOffWorldToWindow(gOffWorld, R32(gGameWindow), a6 - 0x18, a6 - 0x18, 0);
                        W16(a6 - 0x18, R16(a6 - 0x18) + d5);
                        W16(a6 - 0x14, R16(a6 - 0x14) + d5);
                    }
                    WaitFor(wait);
                    d6++;
                } while (!((int32_t)R16(a6 - 0x20) + d4 < (int32_t)d6));
            }
        } else {
            int16_t d6 = R16(a6 - 0x1e);
            int32_t t = R16(a6 - 0x1e);
            W32(a6 - 4, t);
            int32_t d4 = (int32_t)d5;
            if (!(t + d4 < t)) {
                do {
                    W16(a6 - 0x16, d6);
                    W16(a6 - 0x12, d6 + 1);
                    while (R16(a6 - 0x12) <= R16(a6 - 0x1a)) {
                        DrawSOffWorldToWindow(gOffWorld, R32(gGameWindow), a6 - 0x18, a6 - 0x18, 0);
                        W16(a6 - 0x16, R16(a6 - 0x16) + d5);
                        W16(a6 - 0x12, R16(a6 - 0x12) + d5);
                    }
                    WaitFor(wait);
                    d6++;
                } while ((int32_t)R16(a6 - 0x1e) + d4 >= (int32_t)d6);
            }
        }
    }
    DrawSOffWorldToWindow(gOffWorld, R32(gGameWindow), a6 - 0x20, a6 - 0x20, 0);
    return 0;
}

/* 0004f74e: set entry idx (byte) to an 8-bit RGB (byte arguments), scaled num/den, and
 * show it (the temporary one-entry vCtab is purged, not disposed) */
uint32_t ShowNthvRGB(uint32_t win, int16_t num, int16_t den, int16_t idx, int16_t r, int16_t g, int16_t b) {
    ENTRY_LINK(FN_ShowNthvRGB, 0, 12);                   /* D3-D4/A3 */
    uint32_t h = PoPNewHandle(6);
    if (h == 0) return 0;
    _HLock(h);
    W16(DEREF(h), 1);
    uint8_t i = (uint8_t)((uint16_t)idx >> 8);
    W8(DEREF(h) + 5, i);
    W8(DEREF(h) + 2, (uint16_t)r >> 8);
    W8(DEREF(h) + 3, (uint16_t)g >> 8);
    W8(DEREF(h) + 4, (uint16_t)b >> 8);
    SetColors(h, 0x27, num, den);
    ShowColors(win, i, 1);
    _HUnlock(h);
    return _HPurge(h);
}

/* 0004f7dc: free palette entries for the system (mode 0), or keep a highlight color
 * (1000, or 0x20f..0x214 = one of six system colors) */
uint32_t GiveSystemColors(uint32_t win, int16_t mode) {
    ENTRY_LINK(FN_GiveSystemColors, 0x18, 16);           /* D3-D4/D6-D7 */
    W16(a6 + 0xc, mode);
    copy6(a6 - 0x18, gSysReds);
    copy6(a6 - 0x10, gSysGreens);
    copy6(a6 - 8, gSysBlues);
    int16_t d7 = (int16_t)cpu_get(R_D7);                 /* left unset for other modes: the caller's D7 */
    if (R32(gColorTable) == 0) return 0;
    if (mode == 0) {
        _CTab2Palette(R32(gColorTable), R32(gPalette), 2, 0);
        for (int16_t i = 1; i < 0xff; i++) _SetEntryUsage(R32(gPalette), i, 0xc, 0);
    } else {
        if (mode == 1000) {
            d7 = 2;
            ShowNthvRGB(win, 1, 1, (int16_t)0xfe00, (int16_t)0xff00, (int16_t)0x9400, 0x4700);
            ShowNthvRGB(win, 1, 1, (int16_t)0xfd00, (int16_t)0xff00, (int16_t)0xcc00, (int16_t)0xcc00);
        } else if (mode >= 0x20f && mode <= 0x214) {
            mode = (int16_t)(mode - 0x20f);
            W16(a6 + 0xc, mode);                         /* sub.w D4,(0xc,A6) */
            d7 = 1;
            ShowNthvRGB(win, 1, 1, (int16_t)0xfe00, (int16_t)(R8(a6 - 0x18 + mode) << 8),
                        (int16_t)(R8(a6 - 0x10 + mode) << 8), (int16_t)(R8(a6 - 8 + mode) << 8));
        }
        _CTab2Palette(R32(gColorTable), R32(gPalette), 2, 0);
        for (int16_t i = 1; (int32_t)0xff - d7 > (int32_t)i; i++) _SetEntryUsage(R32(gPalette), i, 0xc, 0);
    }
    _Palette2CTab(R32(gPalette), R32(gColorTable));
    return _ActivatePalette(win);
}

/* 0004f960: merge two vCtabs into a new one: all of h1, then the entries of h2 whose index
 * is used (by h1 or h2), up to the number of distinct indexes */
uint32_t CombineCtabs(uint32_t h1, uint32_t h2) {
    ENTRY_LINK(FN_CombineCtabs, 0x11c, 32);              /* D3-D7/A2-A4 */
    const maddr used = a6 - 0x11c;
    uint32_t a3 = 0;
    tc_memset(used, 0, 0x100);
    int16_t d5, d6, d7;
    if (h1) {
        _HLock(h1);
        uint32_t a2 = DEREF(h1);
        d5 = R16(a2);
        uint32_t p = a2 + 2 + 3;
        for (int16_t i = 0; i < d5; i++, p += 4) W8(used + R8(p), 1);
    } else d5 = 0;
    if (h2) {
        _HLock(h2);
        uint32_t a2 = DEREF(h2);
        d6 = R16(a2);
        uint32_t p = a2 + 2 + 3;
        for (int16_t i = 0; i < d6; i++, p += 4) W8(used + R8(p), 1);
    } else d6 = 0;
    d7 = 0;
    for (int16_t i = 0; i < 0x100; i++) if (R8(used + i) != 0) d7++;
    if (d7 == 0) return 0;
    a3 = PoPNewHandle((uint32_t)((int32_t)d7 * 4 + 2));
    if (a3 == 0) return 0;
    _HLock(a3);
    W16(DEREF(a3), d7);
    if (h1) {
        uint32_t dst = DEREF(a3) + 2, src = DEREF(h1) + 2;
        for (int16_t i = 0; i < d5; i++, dst += 4, src += 4) W32(dst, R32(src));
    }
    if (h2) {
        uint32_t dst = (uint32_t)((int32_t)d5 << 2) + DEREF(a3) + 2;
        uint32_t src = DEREF(h2) + 2;
        for (int16_t i = 0; i < d6 && d5 < d7; i++, src += 4) {
            if (R8(used + R8(src + 3)) != 0) { W32(dst, R32(src)); dst += 4; }
        }
    }
    return a3;
}

/* 0004fb42: convert a ColorTable into a vCtab (low bytes of the 16-bit components) */
uint32_t ColorTableTovCtab(uint32_t ct, uint32_t v) {
    uint32_t a2 = DEREF(ct), a4 = DEREF(v);
    int16_t n = (int16_t)(R16(a2 + 6) + 1);
    W16(a4, n);
    uint32_t a3 = a2 + 8, dst = a4 + 2;
    for (int16_t i = 0; i < n; i++, a3 += 8, dst += 4) {
        W8(dst + 3, (uint8_t)i);
        W8(dst, RU16(a3 + 2));
        W8(dst + 1, RU16(a3 + 4));
        W8(dst + 2, RU16(a3 + 6));
    }
    return 0;
}

/* 0004fbe2: save the port's foreground color */
uint32_t FUN_0004fbe2(void) {
    copy6(gSavedFgRGB, R32(gThePort) + 0x24);
    uint32_t c = R32(R32(gThePort) + 0x50);
    W16(gSavedFgIdx, c);
    return c;
}

/* 0004fc04: save the port's background color */
uint32_t seg17_04fc04(void) {
    copy6(gSavedBkRGB, R32(gThePort) + 0x2a);
    uint32_t c = R32(R32(gThePort) + 0x54);
    W16(gSavedBkIdx, c);
    return c;
}

/* 0004fc26: restore the port's background color */
uint32_t FUN_0004fc26(void) {
    copy6(R32(gThePort) + 0x2a, gSavedBkRGB);
    int32_t c = R16(gSavedBkIdx);
    W32(R32(gThePort) + 0x54, c);
    return (uint32_t)c;
}

/* 0004fc46: restore the port's foreground color */
uint32_t seg17_04fc46(void) {
    copy6(R32(gThePort) + 0x24, gSavedFgRGB);
    int32_t c = R16(gSavedFgIdx);
    W32(R32(gThePort) + 0x50, c);
    return (uint32_t)c;
}

/* 0004fc66: entries 0..6 = standard colors */
uint32_t ResetStandardColors(uint32_t win) {
    ENTRY_LINK(FN_ResetStandardColors, 0xc, 16);         /* D3/A2-A4 */
    uint32_t a3 = DEREF(R32(gColorTable)) + 8;
    uint32_t a4 = gStdColors;
    uint16_t i = 1;
    do {
        W16(a3 + 2, RU16(a4));
        W16(a3 + 4, RU16(a4 + 2));
        W16(a3 + 6, RU16(a4 + 4));
        a3 += 8; a4 += 6; i++;
    } while (i < 8);
    return ShowColors(win, 1, 8);
}

/* 0004fcfa: entries 1..253 = standard color c */
uint32_t SetWholePaletteToColor(uint32_t win, int16_t c) {
    ENTRY_LINK(FN_SetWholePaletteToColor, 8, 20);        /* D3-D4/A2-A4 */
    if (c >= 8) return cpu_get(R_D0);
    uint32_t a2 = DEREF(R32(gColorTable)) + 0x10 + 2;
    uint32_t src = gStdColors + (uint32_t)((int32_t)c * 6);
    W32(a6 - 8, R32(src)); W16(a6 - 4, RU16(src + 4));
    uint16_t i = 1;
    do { copy6(a2, a6 - 8); a2 += 8; i++; } while (i < 0xfe);
    return ShowColors(win, 1, 0xfe);
}

/* 0004fd90: halve every color 15 times, one timer period apart, then black */
uint32_t RapidFadeALL(uint32_t ticks) {
    ENTRY_LINK(FN_RapidFadeALL, 8, 20);                  /* D3-D6/A2 */
    if (R32(gColorTable) == 0) return 0;
    _HLock(R32(gColorTable));
    int16_t d5 = 0xf;
    do {
        SetTimer(1, (int16_t)(ticks >> 16), (int16_t)ticks);
        for (int16_t i = 0xff; i >= 0; i--) {
            uint32_t off = (uint32_t)((int32_t)i << 3);
            for (int k = 0xa; k <= 0xe; k += 2) {
                uint32_t a2 = off + DEREF(R32(gColorTable)) + (uint32_t)k;
                W16(a2, RU16(a2) >> 1);
            }
        }
        ShowColors(R32(gGameWindow), 1, 0xfe);
        WaitTimerNoPoll(1);
        d5--;
    } while (d5 > 0);
    for (int16_t i = 0xff; i >= 0; i--) {
        uint32_t off = (uint32_t)((int32_t)i << 3);
        W16(off + DEREF(R32(gColorTable)) + 0xa, 0);
        W16(off + DEREF(R32(gColorTable)) + 0xc, 0);
        W16(off + DEREF(R32(gColorTable)) + 0xe, 0);
    }
    ShowColors(R32(gGameWindow), 1, 0xfe);
    return _HUnlock(R32(gColorTable));
}

/* 0004feb8: fade the current colors out (copy of gColorTable as a vCtab) */
uint32_t FadeOutAllColors(uint32_t win, uint32_t speed) {
    ENTRY_LINK(FN_FadeOutAllColors, 4, 8);               /* D3/A2 */
    if (R32(gColorTable) == 0) return 0;
    uint32_t size = tc_GetHandleSize(R32(gColorTable));
    uint32_t h = PoPNewHandle(size);
    ColorTableTovCtab(R32(gColorTable), h);
    FadeOutColors(h, win, speed);
    return _HPurge(h);
}

/* 0004ff1a: the 512x384 game rect */
uint32_t SetGameRect(uint32_t r) {
    W16(r + 2, 0);
    W16(r, 0);
    W16(r + 6, 0x1fe);
    W16(r + 4, 0x180);
    return 0;
}

/* 0004ff52: move a rect to (0,0) */
uint32_t NormalizeRect(uint32_t r) {
    W16(r + 6, R16(r + 6) - R16(r + 2));
    W16(r + 2, 0);
    int16_t top = R16(r);
    W16(r + 4, R16(r + 4) - top);
    W16(r, 0);
    return (uint32_t)(uint16_t)top;
}

/* 0004ff8e: move a rect to (dx, dy) */
uint32_t MoveRect(uint32_t r, int16_t dx, int16_t dy) {
    ENTRY_LINK(FN_MoveRect, 0, 4);                       /* D3 */
    NormalizeRect(r);
    W16(r + 2, R16(r + 2) + dx);
    W16(r + 6, R16(r + 6) + dx);
    W16(r, R16(r) + dy);
    W16(r + 4, R16(r + 4) + dy);
    return 0;
}

/* 0004ffca */
uint32_t NotSameRects(uint32_t a, uint32_t b) {
    if (R16(a + 2) != R16(b + 2) || R16(a) != R16(b) || R16(a + 6) != R16(b + 6) ||
        R16(a + 4) != R16(b + 4)) return 1;
    return 0;
}

/* 00050024 */
uint32_t RectW(uint32_t r) { return (uint16_t)(R16(r + 6) - R16(r + 2)); }

/* 00050046 */
uint32_t RectH(uint32_t r) { return (uint16_t)(R16(r + 4) - R16(r)); }

/* 00050066: random number in [0, n) (n = 0 means 2) */
uint32_t Randomize(int16_t n) {
    ENTRY_LINK(FN_Randomize, 0, 8);                      /* D4-D5 */
    if (n == 0) n = 2;
    W16(a6 + 8, n);                                      /* move.w D0,(8,A6) */
    int32_t q;
    int16_t d5;
    do {
        int32_t d4 = (int16_t)_Random();
        if (d4 < 0) d4 = (int32_t)(0u - (uint32_t)d4);
        q = (int32_t)((uint32_t)d4 * (uint32_t)(int32_t)n) / 0x8000;
        d5 = (int16_t)q;
    } while (d5 >= n || d5 < 0);
    return (uint32_t)q;
}

/* 000500dc: wait `ticks` ticks */
uint32_t WaitFor(uint32_t ticks) {
    ENTRY_LINK(FN_WaitFor, 4, 12);                       /* D3-D5 */
    int flag = 1;
    int32_t start = (int32_t)_TickCount();
    while (flag) {
        int32_t now = (int32_t)_TickCount();
        if ((int32_t)(start - ticks) > now || !((int32_t)(start + ticks) > now)) flag = 0;
    }
    return 0;
}

/* 0005012e: wait `ticks` ticks or a click; returns Button() */
uint32_t ButtonOrWaitFor(uint32_t ticks) {
    ENTRY_LINK(FN_ButtonOrWaitFor, 4, 12);               /* D3-D5 */
    int flag = 1;
    int32_t start = (int32_t)_TickCount();
    if ((_Button() & 0xff) == 0) {
        do {
            int32_t now = (int32_t)_TickCount();
            if ((int32_t)(start - ticks) > now || !((int32_t)(start + ticks) > now)) flag = 0;
            if (!flag) break;
        } while ((_Button() & 0xff) == 0);
    }
    return _Button() & 0xff;
}

/* 0005019e: append the printable characters (and CR/TAB) of Pascal string src to dst,
 * then ncr carriage returns; *plen = new length */
uint32_t Concactenate(uint32_t dst, uint32_t src, uint32_t plen, int16_t ncr) {
    ENTRY_LINK(FN_Concactenate, 0xc, 32);                /* D3-D7/A2-A4 */
    W16(a6 + 0x14, ncr);
    int16_t len0 = R16(plen);
    int32_t d5 = len0, d6 = 1, d7 = 0;
    if (len0 == 0) W8(dst, 0);
    uint16_t d4 = R8(src);
    if (d4 != 0) {
        uint32_t a4 = dst + (uint32_t)d5 + 1;
        uint32_t s = src + 1;
        do {
            uint8_t c = R8(s + (uint32_t)d7);
            if (c >= 0x20 || c == 0xd || c == 9) {
                W8(a4, c); a4++;
                s++; d6++; d4--;
                W8(dst, R8(dst) + 1);
            } else {
                d4--; d7++;
            }
        } while (d4 != 0);
    }
    if (ncr != 0) {
        uint32_t a3 = dst + (uint32_t)d5 + (uint32_t)d6;
        do {
            W8(a3, 0xd);
            W8(dst, R8(dst) + 1);
            ncr--;
            W16(a6 + 0x14, ncr);                         /* subq.w #1,(0x14,A6) */
            a3++; d6++;
        } while (ncr != 0);
    }
    W16(plen, (uint16_t)((uint16_t)d5 + (uint16_t)d6 - 1));
    return 0;
}

/* 00050270: center a window on the screen (v > 20: at that height) */
uint32_t CenterWindow(uint32_t win, int16_t v) {
    ENTRY_LINK(FN_CenterWindow, 0, 16);                  /* D3-D5/A2 */
    int32_t dx = ((int32_t)R16(gScrRight) - R16(gScrLeft)) - ((int32_t)R16(win + 0x16) - R16(win + 0x12));
    int16_t h = (int16_t)(dx / 2);
    int32_t dy = ((int32_t)R16(gScrBottom) - R16(gScrTop)) - ((int32_t)R16(win + 0x14) - R16(win + 0x10));
    int16_t vv = (int16_t)(dy / 2);
    if (v > 0x14) vv = v;
    return _MoveWindow(win, h, vv, 0);
}

/* 000502f8: |TickCount - t| >= |d| */
uint32_t CompareTicks(uint32_t t, uint32_t d) {
    ENTRY_LINK(FN_CompareTicks, 0, 4);                   /* D3 */
    W32(a6 + 0xc, d);
    int32_t e = (int32_t)(_TickCount() - t);
    if (e < 0) e = (int32_t)(0u - (uint32_t)e);
    if ((int32_t)d < 0) { d = 0u - d; W32(a6 + 0xc, d); }
    return e >= (int32_t)d ? 1 : 0;
}

/* 0005034e: draw the default-button outline around dialog item `item` */
uint32_t FrameDefault(uint32_t dlg, int16_t item) {
    ENTRY_LINK(FN_FrameDefault, 0x14, 4);                /* D3 */
    _GetPort(a6 - 4);
    _SetPort(dlg);
    _GetDialogItem(dlg, item, a6 - 0x14, a6 - 8, a6 - 0x10);
    _PenSize(3, 3);
    _ForeColor(0x21);
    _BackColor(0x1e);
    _TextFont(0);
    _TextFace(0);
    _TextSize(0xc);
    _TextMode(1);
    _PenMode(8);
    _InsetRect(a6 - 0x10, -4, -4);
    _FrameRoundRect(a6 - 0x10, 0x10, 0x10);
    _PenSize(1, 1);
    return _SetPort(R32(a6 - 4));
}

/* 000503e0: toggle a check box dialog item */
uint32_t ToggleCheckBox(uint32_t dlg, int16_t item) {
    ENTRY_LINK(FN_ToggleCheckBox, 0x10, 4);              /* D3 */
    _GetDialogItem(dlg, item, a6 - 0x10, a6 - 0xc, a6 - 8);
    int16_t v = (int16_t)_GetControlValue(R32(a6 - 0xc));
    return _SetControlValue(R32(a6 - 0xc), v == 0 ? 1 : 0);
}

/* 00050432 */
uint32_t RectValid(uint32_t r) {
    if (R16(r + 2) < R16(r + 6) && R16(r) < R16(r + 4)) return 1;
    return 0;
}

/* 0005046c: fatal error alert (0x8000 | manager bits: "MWHK - No %s. Error = %d", else
 * one of 8 messages), then quit */
uint32_t ErrorHandler(int16_t code, int16_t err) {
    ENTRY_LINK(FN_ErrorHandler, 0x100, 8);               /* D3/A2 */
#define EH(a) CS(FN_ErrorHandler, 0x5046c, a)
    if (code & 0x8000) {
        uint32_t a2 = 0;
        if (code & 1) a2 = EH(0x5063a);                  /* "OS Manager" */
        if (code & 2) a2 = EH(0x50646);                  /* "Memory Manager" */
        if (code & 4) a2 = EH(0x50656);                  /* "Timer Manager" */
        if (code & 8) a2 = EH(0x50664);                  /* "File Manager" */
        if (code & 0x10) a2 = EH(0x50672);               /* "Resource Manager" */
        if (code & 0x20) a2 = EH(0x50684);               /* "Config Manager" */
        if (code & 0x40) a2 = EH(0x50694);               /* "GFX Manager" */
        if (code & 0x80) a2 = EH(0x506a0);               /* "Audio Manager" */
        if (code & 0x20) a2 = EH(0x506ae);               /* "Config Manager" (again) */
        if (a2 != 0) {
            call_ptr(JT_ADDR(FN_tc_sprintf), "llll", a6 - 0x100, EH(0x506be), a2, (uint32_t)(int32_t)err);
            tc_CtoPstr(a6 - 0x100);
            _ParamText(a6 - 0x100, EH(0x506d8), EH(0x506da), EH(0x506dc));
        }
    } else {
        static const uint32_t msg[8][4] = {
            { 0x506de, 0x506ec, 0x506ee, 0x506f0 },      /* "Fatal Error" (also default) */
            { 0x506f2, 0x506fc, 0x506fe, 0x50700 },      /* "NO WIND" */
            { 0x50702, 0x5070c, 0x5070e, 0x50710 },      /* "NO MBAR" */
            { 0x50712, 0x5071c, 0x5071e, 0x50720 },      /* "NO MENU" */
            { 0x50722, 0x50730, 0x50732, 0x50734 },      /* "NO Game File" */
            { 0x50736, 0x50746, 0x50748, 0x5074a },      /* "Out Of Memory" */
            { 0x5074c, 0x5075a, 0x5075c, 0x5075e },      /* "VM on error" */
            { 0x50760, 0x5076c, 0x5076e, 0x50770 },      /* "Disk Full" */
        };
        int c = (code < 0 || code > 7) ? 0 : code;       /* out of range -> case 0 */
        _ParamText(EH(msg[c][0]), EH(msg[c][1]), EH(msg[c][2]), EH(msg[c][3]));
    }
#undef EH
    _StopAlert(0x191, 0);
    Terminate(1, 0);
    return 0;
}

/* 00050772: set up the game window (centered on a black full-screen if `full`, else at
 * the saved position *pt or centered) */
uint32_t GameWindow(uint32_t win, int16_t full, uint32_t pt) {
    ENTRY_LINK(FN_GameWindow, 0x14, 12);                 /* D4-D5/A2 */
    _GetPort(a6 - 4);
    _HideWindow(win);
    SetGameRect(a6 - 0x14);
    W32(a6 - 0xc, R32(a6 - 0x14)); W32(a6 - 8, R32(a6 - 0x10));
    int16_t d4 = 0, d5 = 0;
    if (full != 0) {
        d4 = (int16_t)((((int32_t)R16(gScrRight) - R16(gScrLeft)) - ((int32_t)R16(a6 - 0xe) - R16(a6 - 0x12))) / 2);
        d5 = (int16_t)((((int32_t)R16(gScrBottom) - R16(gScrTop)) - ((int32_t)R16(a6 - 0x10) - R16(a6 - 0x14))) / 2);
        _SetRect(a6 - 0x14, 0, 0, R16(gScrRight), R16(gScrBottom));
    }
    W32(win + 0x10, R32(a6 - 0x14)); W32(win + 0x14, R32(a6 - 0x10));
    if (full != 0 || (R16(pt) == 0 && R16(pt + 2) == 0)) CenterWindow(win, 0);
    else _MoveWindow(win, R16(pt + 2), R16(pt), 0);
    _SetPort(win);
    _SetOrigin((int16_t)-d4, (int16_t)-d5);
    _ClipRect(a6 - 0xc);                                 /* through gClipRectProc */
    _ShowWindow(win);
    if (R8(gActive) == 0) {
        _HiliteWindow(win, 0);
        _SetPort(R32(a6 - 4));
    }
    return 0;
}

/* 0005089e: modal About box (DLOG id) until item 1 */
uint32_t DrawAbout(int16_t id) {
    ENTRY_LINK(FN_DrawAbout, 4, 4);                      /* A2 */
    W8(a6 - 2, 0);
    uint32_t dlg = _GetNewDialog(id, 0, 0xffffffffu);
    CenterWindow(dlg, 0x3c);
    _ShowWindow(dlg);
    FrameDefault(dlg, 1);
    while (R8(a6 - 2) == 0) {
        _ModalDialog(0, a6 - 4);
        if (R16(a6 - 4) == 1) {
            _HideWindow(dlg);
            W8(a6 - 2, 1);
        }
    }
    return _DisposeDialog(dlg);
}

/* 00050918: check the system (SysEnvirons, System 6+, Color QuickDraw, screen depth);
 * *depth = main screen depth. With the flag byte, explains a failure in a dialog.
 * Returns 1 when everything is fine (byte result). */
uint32_t Sys6OrLater(uint32_t pdepth, int16_t flag) {
    ENTRY_LINK(FN_Sys6OrLater, 0x128, 32);               /* D3-D7/A2-A4 */
    W8(a6 - 0x116, 1);
    mcopy(a6 - 0x114, gSysErrText, 0x100);
    int16_t d4 = S16(tc_SysEnvirons(a6 - 0x128, 1));
    W8(a6 - 4, 0);
    W8(gHasColorQD, 0);
    int16_t d6 = R16(a6 - 0x124);                        /* systemVersion */
    if (R8(a6 - 0x11f) == 1) W8(gHasColorQD, 1);         /* hasColorQD */
    uint32_t a2 = tc_NGetTrapAddress(0x0100, (int16_t)0xa1ad);       /* _Gestalt */
    if (a2 != tc_NGetTrapAddress(0x0100, 0x9f)) W8(a6 - 4, 1);       /* _Unimplemented */
    int16_t d5 = 1;
    uint32_t a3 = cpu_get(R_A3);                         /* unset if no GetMainDevice: caller's A3 */
    a2 = tc_NGetTrapAddress(0x0100, (int16_t)0xaa2a);                /* _GetMainDevice */
    if (a2 != tc_NGetTrapAddress(0x0100, 0x9f)) {
        a3 = _GetMainDevice();
        if (a3 != 0) d5 = R16(DEREF(R32(DEREF(a3) + 0x16)) + 0x20);   /* (**gdPMap).pixelSize */
    }
    if (R16(gScreenDepth) != 0 && d5 != R16(gScreenDepth)) W8(a6 - 0x116, 1);
    else if (d5 == 8) W8(a6 - 0x116, 0);
    if (d4 != 0 || d6 < 0x600 || a3 == 0 || R8(a6 - 0x116) != 0) {
        if (((uint16_t)flag >> 8) != 0) {
            uint32_t dlg = _GetNewDialog(R16(gScreenDepth) == 0 ? 0x192 : 0x193, 0, 0xffffffffu);
            if (dlg == 0) ErrorHandler(0, 0);
            CenterWindow(dlg, 0x3c);
            if (R16(gScreenDepth) == 8) AddWindow(dlg);
            _ForeColor(0x21);
            _BackColor(0x1e);
            _ShowWindow(dlg);
            FrameDefault(dlg, 1);
            if (R16(gScreenDepth) == 8) {
                _GetDialogItem(dlg, 6, a6 - 0x14, a6 - 8, a6 - 0x10);
                _SetDialogItemText(R32(a6 - 8), a6 - 0x114);
            }
            _ModalDialog(0, a6 - 0x118);
            _DisposeDialog(dlg);
        }
        return 0;                                        /* clr.b D0 */
    }
    W16(pdepth, d5);
    if (d5 == 8 && R8(gHasColorQD) == 0 && R8(a6 - 4) == 0) W8(a6 - 0x116, 1);
    if (R8(a6 - 0x116) != 0) W16(gPauseMark, 2);
    else if (R16(gPauseMark) == 2) W16(gPauseMark, 0);
    return R8(a6 - 0x116) == 0 ? 1 : 0;
}

/* 00050b0e: re-check the screen depth (after a switch) */
uint32_t RecheckSystem(int16_t flag) {
    ENTRY_LINK(FN_RecheckSystem, 4, 0);
    W8(gDepthOK, 0);
    Sys6OrLater(a6 - 2, flag);
    if (R16(a6 - 2) == R16(gScreenDepth)) W8(gDepthOK, 1);   /* stack garbage on failure, as in the original */
    return 0;
}

/* 00050b4a: compact the heap (always with the flag byte, else when free memory < 128K) */
uint32_t CheckFreeMemory(int16_t flag) {
    ENTRY_LINK(FN_CheckFreeMemory, 4, 4);                /* D3 */
    if (((uint16_t)flag >> 8) != 0) {
        W32(a6 - 4, 0);
        return _MaxMem(a6 - 4);
    }
    int32_t f = (int32_t)_FreeMem();
    if (f < 0x20000) {
        W32(a6 - 4, 0);
        return _MaxMem(a6 - 4);
    }
    return (uint32_t)f;
}

/* 00050b9a: pointer to C string number `item` (byte argument, 1-based) of a 'STR#'-like
 * handle (count byte, then NUL-terminated strings) */
uint32_t StrItem(uint32_t h, int16_t item) {
    uint32_t a2 = DEREF(h);
    uint16_t count = R8(a2);
    a2++;
    uint16_t n = (uint8_t)((uint16_t)item >> 8);
    if (n > count) return a2;
    n--;
    if (n == 0) return a2;
    do {
        while (R8(a2) != 0) a2++;
        a2++;
        n--;
    } while (n != 0);
    return a2;
}

/* 00050be8: draw C string str in rect r (TextInRect, justification p10, alignment p14) */
uint32_t StringRect(uint32_t r, int16_t p8, int16_t just, int16_t p12, int16_t valign, uint32_t str) {
    ENTRY_LINK(FN_StringRect, 0, 4);                     /* D3 */
    (void)p8; (void)p12;
    uint32_t n = tc_strlen(str);
    if (n != 0) TextInRect(str, n, r, just, valign);
    return r;
}

/* 00050c2e: StringWidth of a C string (up to 255 characters) */
uint32_t CStrWidth(uint32_t s) {
    ENTRY_LINK(FN_CStrWidth, 0x100, 12);                 /* D3-D4/A2 */
    W8(a6 - 0x100, 0);
    uint16_t i = 1;
    if (R8(s) != 0) {
        do {
            W8(a6 - 0x100, R8(a6 - 0x100) + 1);
            W8(a6 - 0x100 + i, R8(s + i - 1));
            i++;
        } while (i < 0x100 && R8(s + i - 1) != 0);
    }
    return (uint32_t)(int32_t)(int16_t)_StringWidth(a6 - 0x100);
}

/* 00050c9a: draw text (len bytes) word-wrapped in rect r. just: 0 left, 1 center, 0xff
 * right; valign: -1 top, 0 bottom, else centered (TextInRectHeight). CR breaks lines. */
uint32_t TextInRect(uint32_t str, uint32_t len, uint32_t r, int16_t just, int16_t valign) {
    ENTRY_LINK(FN_TextInRect, 0x12c, 32);                /* D3-D7/A2-A4 */
    const int32_t slen = (int32_t)len;
    W32(a6 + 0xc, len);                                  /* its low byte is read from the slot */
    W8(a6 - 0x11c, 0);                                   /* done */
    int16_t d5 = (int16_t)(R16(r + 6) - R16(r + 2) - 2); /* width */
    int16_t d6 = (int16_t)(R16(r + 4) - R16(r));         /* height */
    _GetFontInfo(a6 - 0x118);
    int16_t d7 = (int16_t)(R16(a6 - 0x118) + R16(a6 - 0x116) + R16(a6 - 0x112));
    if (valign == -1) {
        W16(a6 - 0x122, R16(r));
    } else {
        int16_t h = (int16_t)TextInRectHeight(str, len, d5);
        W16(a6 - 0x120, h);
        if (h > d6) W16(a6 - 0x122, R16(r));
        else if (valign == 0) W16(a6 - 0x122, R16(r + 4) - h);
        else {
            int32_t t = (int32_t)d6 >> 1;
            uint16_t w = (uint16_t)((uint16_t)t + (uint16_t)R16(r));
            W16(a6 - 0x122, (uint16_t)(w - (uint16_t)((int32_t)h >> 1)));
        }
    }
    W16(a6 - 0x12c, 1);                                  /* line */
    W16(a6 - 0x124, 0);                                  /* pos */
    if (d5 < 5 || slen <= 0) return 0;
    uint32_t rgn = _NewRgn();
    _GetClip(rgn);
    _ClipRect(r);                                        /* through gClipRectProc */
    while (R8(a6 - 0x11c) == 0) {
        W16(a6 - 0x12a, 0);                              /* line width */
        W16(a6 - 0x126, 0);                              /* n */
        W8(a6 - 0x11e, 1);                               /* cont */
        W8(a6 - 0x11d, 0);                               /* saw a space */
        uint8_t c = R8(str + (uint32_t)(int32_t)R16(a6 - 0x124));
        if (c == 0x20 || c == 0xd) {
            for (;;) {
                W16(a6 - 0x124, R16(a6 - 0x124) + 1);
                if (R8(str + (uint32_t)(int32_t)R16(a6 - 0x124)) == 0xd) W16(a6 - 0x12c, R16(a6 - 0x12c) + 1);
                if (!((int32_t)R16(a6 - 0x124) < slen)) { W8(a6 - 0x11e, 0); W8(a6 - 0x11c, 1); }
                if (R8(a6 - 0x11e) == 0) break;
                c = R8(str + (uint32_t)(int32_t)R16(a6 - 0x124));
                if (c != 0x20 && c != 0xd) break;
            }
        }
        if (R8(a6 - 0x11e) != 0) {
            do {
                int32_t pos = R16(a6 - 0x124), n = R16(a6 - 0x126);
                if (pos + n >= slen) {
                    W8(a6 - 0x11e, 0);
                    W16(a6 - 0x126, R16(a6 - 0x126) - 1);
                } else {
                    int16_t cw = (int16_t)_CharWidth(R8(str + (uint32_t)pos + (uint32_t)n));
                    W16(a6 - 0x128, cw);
                    pos = R16(a6 - 0x124); n = R16(a6 - 0x126);
                    if (R8(str + (uint32_t)pos + (uint32_t)n) == 0x20) W8(a6 - 0x11d, 1);
                    pos = R16(a6 - 0x124); n = R16(a6 - 0x126);
                    if (R8(str + (uint32_t)pos + (uint32_t)n) == 0xd || pos + n > (int32_t)(int16_t)len) {
                        W8(a6 - 0x11e, 0);
                        W16(a6 - 0x126, R16(a6 - 0x126) - 1);
                    } else if (R8(a6 - 0x11e) != 0) {
                        if ((int32_t)R16(a6 - 0x12a) + cw < (int32_t)d5) {
                            W16(a6 - 0x12a, R16(a6 - 0x12a) + cw);
                        } else if (R8(a6 - 0x11d) != 0) {
                            if (R16(a6 - 0x126) > 1 &&
                                R8(str + (uint32_t)(int32_t)R16(a6 - 0x124) + (uint32_t)(int32_t)R16(a6 - 0x126)) != 0x20) {
                                for (;;) {
                                    W16(a6 - 0x126, R16(a6 - 0x126) - 1);
                                    if (R16(a6 - 0x126) <= 1) break;
                                    if (R8(str + (uint32_t)(int32_t)R16(a6 - 0x124) + (uint32_t)(int32_t)R16(a6 - 0x126)) == 0x20) break;
                                }
                            }
                            W8(a6 - 0x11e, 0);
                        } else {
                            W16(a6 - 0x126, R16(a6 - 0x126) - 1);
                            W8(a6 - 0x11e, 0);
                        }
                    }
                }
                W16(a6 - 0x126, R16(a6 - 0x126) + 1);
            } while (R8(a6 - 0x11e) != 0);
        }
        if (R8(a6 - 0x11c) != 0) {
            _MoveTo((int16_t)(R16(r + 2) + 1),
                    (int16_t)((uint16_t)((int32_t)d7 * R16(a6 - 0x12c)) + (uint16_t)R16(a6 - 0x122) - 4));
            continue;
        }
        if ((int32_t)R16(a6 - 0x124) + (int32_t)RU16(a6 - 0x126) <= slen) W8(a6 - 0x10c, R8(a6 - 0x125));
        else W8(a6 - 0x10c, (uint8_t)(R8(a6 + 0xf) - R8(a6 - 0x123)));   /* low bytes of len and pos */
        uint8_t i = 0;
        W8(a6 - 0x110, 0);
        if (!(i > R8(a6 - 0x10c))) {
            do {
                uint32_t a4 = str + (uint32_t)(int32_t)R16(a6 - 0x124);
                W8(a6 - 0x10b + i, R8(a4 + i));
                i++;
                W8(a6 - 0x110, i);
            } while (i <= R8(a6 - 0x10c));
        }
        int16_t w = (int16_t)_StringWidth(a6 - 0x10c);
        W16(a6 - 0x12a, w);
        W16(a6 - 0x128, 0);
        if (just == 1) W16(a6 - 0x128, (int16_t)(((int32_t)d5 - (int32_t)w) >> 1));
        else if (just == 0xff) W16(a6 - 0x128, d5 - R16(a6 - 0x12a));
        if (R16(a6 - 0x128) < 0) W16(a6 - 0x128, 0);
        _MoveTo((int16_t)(R16(r + 2) + R16(a6 - 0x128) + 1),
                (int16_t)((uint16_t)((int32_t)d7 * R16(a6 - 0x12c)) + (uint16_t)R16(a6 - 0x122) - 4));
        _DrawString(a6 - 0x10c);
        W16(a6 - 0x12c, R16(a6 - 0x12c) + 1);
        W16(a6 - 0x124, R16(a6 - 0x124) + R8(a6 - 0x10c));
        if ((int32_t)((uint32_t)(int32_t)d7 * (uint32_t)(int32_t)R16(a6 - 0x12c)) - 4 > (int32_t)d6) W8(a6 - 0x11c, 1);
        if (!(R16(a6 - 0x124) < (int16_t)len)) W8(a6 - 0x11c, 1);
    }
    _SetClip(rgn);
    _DisposeRgn(rgn);
    return 0;
}

/* 00051094: height TextInRect needs for the text at this width (lines x line height) */
uint32_t TextInRectHeight(uint32_t str, uint32_t len, int16_t width) {
    ENTRY_LINK(FN_TextInRectHeight, 0x120, 20);          /* D3-D7 */
    const int32_t slen = (int32_t)len;
    W32(a6 + 0xc, len);
    W16(a6 + 0x10, width);
    W8(a6 - 0x118, 0);                                   /* done */
    width = (int16_t)(width - 2);
    W16(a6 + 0x10, width);                               /* subq.w #2,(0x10,A6) */
    _GetFontInfo(a6 - 0x114);
    int16_t d5 = (int16_t)(R16(a6 - 0x114) + R16(a6 - 0x112) + R16(a6 - 0x10e));
    int16_t d6 = 0;                                      /* lines */
    W16(a6 - 0x11c, 0);                                  /* pos */
    if (width < 5 || slen <= 0 || R8(a6 - 0x118) != 0) return (uint32_t)((int32_t)d5 * d6);
    do {
        int16_t d7 = 0;                                  /* line width */
        W16(a6 - 0x11e, 0);                              /* n */
        W8(a6 - 0x11a, 1);                               /* cont */
        W8(a6 - 0x119, 0);                               /* saw a space */
        uint8_t c = R8(str + (uint32_t)(int32_t)R16(a6 - 0x11c));
        if (c == 0x20 || c == 0xd) {
            for (;;) {
                W16(a6 - 0x11c, R16(a6 - 0x11c) + 1);
                if (R8(str + (uint32_t)(int32_t)R16(a6 - 0x11c)) == 0xd) d6++;
                if (!((int32_t)R16(a6 - 0x11c) < slen)) { W8(a6 - 0x11a, 0); W8(a6 - 0x118, 1); }
                if (R8(a6 - 0x11a) == 0) break;
                c = R8(str + (uint32_t)(int32_t)R16(a6 - 0x11c));
                if (c != 0x20 && c != 0xd) break;
            }
        }
        if (R8(a6 - 0x11a) != 0) {
            do {
                int32_t pos = R16(a6 - 0x11c), n = R16(a6 - 0x11e);
                if (pos + n >= slen) {
                    W8(a6 - 0x11a, 0);
                    W16(a6 - 0x11e, R16(a6 - 0x11e) - 1);
                } else {
                    int16_t cw = (int16_t)_CharWidth(R8(str + (uint32_t)pos + (uint32_t)n));
                    W16(a6 - 0x120, cw);
                    pos = R16(a6 - 0x11c); n = R16(a6 - 0x11e);
                    if (R8(str + (uint32_t)pos + (uint32_t)n) == 0x20) W8(a6 - 0x119, 1);
                    pos = R16(a6 - 0x11c); n = R16(a6 - 0x11e);
                    if (R8(str + (uint32_t)pos + (uint32_t)n) == 0xd || pos + n > (int32_t)(int16_t)len) {
                        W8(a6 - 0x11a, 0);
                        W16(a6 - 0x11e, R16(a6 - 0x11e) - 1);
                    } else if (R8(a6 - 0x11a) != 0) {
                        if ((int32_t)d7 + cw < (int32_t)R16(a6 + 0x10)) {
                            d7 = (int16_t)(d7 + cw);
                        } else if (R8(a6 - 0x119) != 0) {
                            if (R16(a6 - 0x11e) > 1 &&
                                R8(str + (uint32_t)(int32_t)R16(a6 - 0x11c) + (uint32_t)(int32_t)R16(a6 - 0x11e)) != 0x20) {
                                for (;;) {
                                    W16(a6 - 0x11e, R16(a6 - 0x11e) - 1);
                                    if (R16(a6 - 0x11e) <= 1) break;
                                    if (R8(str + (uint32_t)(int32_t)R16(a6 - 0x11c) + (uint32_t)(int32_t)R16(a6 - 0x11e)) == 0x20) break;
                                }
                            }
                            W8(a6 - 0x11a, 0);
                        } else {
                            W16(a6 - 0x11e, R16(a6 - 0x11e) - 1);
                            W8(a6 - 0x11a, 0);
                        }
                    }
                }
                W16(a6 - 0x11e, R16(a6 - 0x11e) + 1);
            } while (R8(a6 - 0x11a) != 0);
        }
        if (R8(a6 - 0x118) == 0) {
            if ((int32_t)R16(a6 - 0x11c) + (int32_t)RU16(a6 - 0x11e) <= slen) W8(a6 - 0x10c, R8(a6 - 0x11d));
            else W8(a6 - 0x10c, (uint8_t)(R8(a6 + 0xf) - R8(a6 - 0x11b)));
            d6++;
            W16(a6 - 0x11c, R16(a6 - 0x11c) + R8(a6 - 0x10c));
            if (!(R16(a6 - 0x11c) < (int16_t)len)) W8(a6 - 0x118, 1);
        }
    } while (R8(a6 - 0x118) == 0);
    return (uint32_t)((int32_t)d5 * d6);
}

/* 00051300: report an OS error ("%s.  OS Error = %d."): fatal -> Terminate, else ask
 * whether to quit. The message pointer comes as two words. */
uint32_t CheckOSErr(int16_t err, int16_t msgHi, int16_t msgLo, int16_t fatal) {
    ENTRY_LINK(FN_CheckOSErr, 0x100, 0);
    if (err == 0) return 0;
    uint32_t msg = (uint32_t)(uint16_t)msgHi << 16 | (uint16_t)msgLo;
    call_ptr(JT_ADDR(FN_tc_sprintf), "llll", a6 - 0x100, CS(FN_CheckOSErr, 0x51300, 0x5137e), msg,
             (uint32_t)(int32_t)err);
    if (fatal != 0) {
        Terminate(1, a6 - 0x100);
    } else {
        call_ptr(JT_ADDR(FN_tc_sprintf), "lll", a6 - 0x100, CS(FN_CheckOSErr, 0x51300, 0x51392), a6 - 0x100);
        if (S16(AlertYesNo(a6 - 0x100)) != 0) Terminate(1, 0);
    }
    return 0;
}

/* 000513ac: show msg (if any), restore the volume and the menu bar, exit */
uint32_t Terminate(int16_t code, uint32_t msg) {
    ENTRY_LINK(FN_Terminate, 0, 4);                      /* D3 */
    (void)code;
    if (msg != 0) AlertOKOnly(msg);
    int16_t vol = R16(gMasterVolume);
    if (vol >= 0) SetVolumeLevel(vol);
    e2MAC_ShowMenuBar();
    tc_exit(0);
    return 0;
}

/* 000513f0: call the key reader procedure, if any (its word result, zero-extended) */
uint32_t CheckWaitFunc(void) {
    ENTRY_LINK(FN_CheckWaitFunc, 4, 8);                  /* D3/A2 */
    uint32_t p = R32(gReadKeyProc);
    if (p == 0) return 0;
    return call_ptr(p, "") & 0xffff;
}

/* 0005142a (Pascal): alert 0x191 with msg, then quit */
uint32_t ShowErrorAndExit(uint32_t msg) {
    ENTRY_LINK(FN_ShowErrorAndExit, 0, 0);
    _ParamText(msg, gEmptyStr, gEmptyStr, gEmptyStr);
    _StopAlert(0x191, 0);
    Terminate(1, 0);
    return 0;
}

/* 00051472: ticks left on timer n (0 when elapsed). D0's high word is the one of the
 * (possibly negative) difference; A0 is left on the duration table. */
uint32_t GetTimer(int16_t n) {
    ENTRY_LINK(FN_GetTimer, 0, 8);                       /* D3-D4 */
    int32_t now = (int32_t)_TickCount();
    int16_t off = (int16_t)((int32_t)n * 6);
    now -= (int32_t)R32(gTimerStart + (uint32_t)(int32_t)off);
    int32_t d0 = (int32_t)R16(gTimerDur + (uint32_t)(int32_t)off) - now;
    int32_t d4 = d0 < 0 ? 0 : d0;
    cpu_set(R_A0, gTimerDur);
    return ((uint32_t)d0 & 0xffff0000u) | ((uint32_t)d4 & 0xffff);
}

/* 000514ce */
uint32_t PollUser0(void) {
    ENTRY_LINK(FN_PollUser0, 4, 0);
    return FUN_000550b8();
}

/* 000514e8: mark a resource changed and write it (SetResourceSize first when the trap
 * exists). Returns 1 on success. */
uint32_t PoPWriteResource(uint32_t h, uint32_t name) {
    ENTRY_LINK(FN_PoPWriteResource, 0x100, 8);           /* D3-D4 */
    int16_t err;
    if (h != 0) {
        _ChangedResource(h);
        err = (int16_t)_ResError();
        if (err == 0) {
            uint32_t size = tc_GetHandleSize(h);
            _HUnlock(h);
            if (GetPOPTrap((int16_t)0xa822) != 0) _SetResourceSize(h, size);
            err = (int16_t)_ResError();
            if (err == 0) _WriteResource(h);
        } else {
            CheckOSErr(err, (int16_t)(name >> 16), (int16_t)name, 0);
        }
    } else {
        err = -1;
    }
    return err == 0 ? 1 : 0;
}

/* 00051576: start timer n for dur ticks (the middle word is the high word of a long) */
uint32_t SetTimer(int16_t n, int16_t hi, int16_t dur) {
    ENTRY_LINK(FN_SetTimer, 0, 4);                       /* D3 */
    (void)hi;
    uint32_t now = _TickCount();
    int16_t off = (int16_t)((int32_t)n * 6);
    W32(gTimerStart + (uint32_t)(int32_t)off, now);
    W16(gTimerDur + (uint32_t)(int32_t)off, dur);
    cpu_set(R_A0, gTimerDur);
    return now;
}

/* 000515c4: wait for timer n, polling the key reader; returns the key (0 = timer ran out) */
uint32_t WaitTimer(int16_t n) {
    ENTRY_LINK(FN_WaitTimer, 0, 4);                      /* D3 */
    int16_t key;
    uint32_t t;
    do {
        key = (int16_t)CheckWaitFunc();
        t = GetTimer(n);
    } while ((int16_t)t != 0 && key == 0);
    return (t & 0xffff0000u) | (uint16_t)key;
}

/* 000515f8: wait for timer n */
uint32_t WaitTimerNoPoll(int16_t n) {
    ENTRY_LINK(FN_WaitTimerNoPoll, 0, 0);
    uint32_t t = GetTimer(n);
    if ((int16_t)t != 0) {
        do {
            _SystemTask();
            t = GetTimer(n);
        } while ((int16_t)t != 0);
    }
    return t;
}

/* 00051632: wait dur ticks on timer n (returns WaitTimer's key) */
uint32_t Wait(int16_t n, int16_t dur) {
    ENTRY_LINK(FN_Wait, 0, 0);
    SetTimer(n, 0, dur);
    return WaitTimer(n);
}

/* 0005165e: wait for a key */
uint32_t FUN_0005165e(void) {
    ENTRY_NOLINK(FN_FUN_0005165e, 4);                    /* D3 */
    uint32_t r;
    do { r = CheckWaitFunc(); } while ((int16_t)r == 0);
    return r;
}

/* 0005166e: close a resource file {refnum, ...} */
uint32_t CloseRsrcFile(uint32_t rec) {
    ENTRY_LINK(FN_CloseRsrcFile, 0, 4);                  /* D3 */
    int16_t ref = R16(rec);
    if (ref > 0) _CloseResFile(ref);
    W16(rec, 0);
    return 0;
}

/* 000516a0: open a resource file {refnum, Str name}; quit if it cannot be opened */
uint32_t OpenRsrcVerify(uint32_t rec) {
    ENTRY_LINK(FN_OpenRsrcVerify, 0x140, 8);             /* A2-A3 */
    int16_t ref = (int16_t)_OpenResFile(R32(rec + 2));
    W16(rec, ref);
    if (ref <= 0) {
        uint32_t name = R32(rec + 2);
        tc_memcpy(a6 - 0x40, name + 1, R8(name));
        W8(a6 - 0x40 + R8(R32(rec + 2)), 0);
        call_ptr(JT_ADDR(FN_tc_sprintf), "lll", a6 - 0x140, CS(FN_OpenRsrcVerify, 0x516a0, 0x51730), a6 - 0x40);
        Terminate(1, a6 - 0x140);
    }
    return RU16(rec);
}

/* 0005174e: GetResource, retrying after making the background sets purgeable; quits
 * when the resource is missing */
uint32_t PoPGetResource(uint32_t type, int16_t id) {
    ENTRY_LINK(FN_PoPGetResource, 0x6c, 4);              /* A2 */
    W32(a6 + 8, type);
    uint32_t h = _GetResource(type, id);
    if (h != 0) return h;
    if (R16(gBGSetState) != 1) {
        MarkBGSetPurgable(1);
        h = _GetResource(type, id);
    }
    if (h == 0) {
        tc_memcpy(a6 - 8, a6 + 8, 4);                    /* the type, from the argument slot */
        W8(a6 - 4, 0);
        call_ptr(JT_ADDR(FN_tc_sprintf), "llll", a6 - 0x6c, CS(FN_PoPGetResource, 0x5174e, 0x517e8),
                 (uint32_t)(int32_t)id, a6 - 8);
        Terminate(1, a6 - 0x6c);
    }
    return h;
}

/* 00051810: NewHandle, retrying after making the background sets purgeable */
uint32_t PoPNewHandle(uint32_t size) {
    ENTRY_LINK(FN_PoPNewHandle, 0, 4);                   /* A2 */
    uint32_t h = _NewHandle(size);
    if (h != 0) return h;
    if (R16(gBGSetState) != 1) {
        MarkBGSetPurgable(1);
        h = _NewHandle(size);
    }
    if (h == 0) ShowErrorAndExit(CS(FN_PoPNewHandle, 0x51810, 0x51862));
    return h;
}

/* 0005187a: NewPtr, retrying after making the background sets purgeable */
uint32_t PoPNewPtr(uint32_t size) {
    ENTRY_LINK(FN_PoPNewPtr, 0, 4);                      /* A2 */
    uint32_t p = _NewPtr(size);
    if (p != 0) return p;
    if (R16(gBGSetState) != 1) {
        MarkBGSetPurgable(1);
        p = _NewPtr(size);
    }
    if (p == 0) ShowErrorAndExit(CS(FN_PoPNewPtr, 0x5187a, 0x518ca));
    return p;
}

/* 000518e0: a locked block high in the heap (the handle's master pointer) */
uint32_t PoPNewHiPtr(uint32_t size) {
    ENTRY_LINK(FN_PoPNewHiPtr, 0, 4);                    /* A2 */
    uint32_t h = PoPNewHandle(size);
    if (h == 0) {
        if (R16(gBGSetState) != 1) {
            MarkBGSetPurgable(1);
            h = PoPNewHandle(size);
        }
        if (h == 0) Terminate(1, CS(FN_PoPNewHiPtr, 0x518e0, 0x51948));
    }
    _MoveHHi(h);
    _HLock(h);
    return R32(h);
}

/* 00051960: LZ-pack a shape handle in place when that makes it smaller (flag 0x400 in
 * the first word); returns 1 if packed */
uint32_t PackShapeLZ(uint32_t h) {
    ENTRY_LINK(FN_PackShapeLZ, 4, 16);                   /* D3-D4/A2-A3 */
    W8(a6 - 4, 0);
    int32_t size = (int32_t)tc_GetHandleSize(h);
    uint32_t packed = PackLZ(h, (uint32_t)size);
    if (packed != 0) {
        int32_t psize = (int32_t)tc_GetHandleSize(packed);
        if (psize < size) {
            uint32_t a2 = R32(packed);
            W16(a2, RU16(a2) | 0x400);
            _SetHandleSize(h, (uint32_t)psize);
            _BlockMove(R32(packed), R32(h), (uint32_t)psize);
            W8(a6 - 4, 1);
        }
        _HUnlock(packed);
        _HPurge(packed);
    }
    return R8(a6 - 4);
}

/* 000519de */
uint32_t UnpackShapeLZ(uint32_t h) {
    ENTRY_LINK(FN_UnpackShapeLZ, 0, 0);
    return UnpackLZ(h);
}

/* 00051a00: LZSS-pack (N = 1024, F = 66, threshold 2) a handle of `size` bytes: the
 * 12-byte header is copied, then the size (long), then flag bytes + literals / 2-byte
 * (length-3)<<10 | position codes. Returns the new handle, or 0. */
uint32_t PackLZ(uint32_t h, uint32_t size) {
    ENTRY_LINK(FN_PackLZ, 0x54, 32);                     /* D3-D7/A2-A4 */
    uint8_t codeBuf[17];
    uint8_t err = 0;
    uint32_t outMax = size << 1;
    int16_t hdr = 0xc;
    _HLock(h);
    uint32_t src = R32(h);
    W32(gLZTextBuf, PoPNewPtr(0x442));
    W32(gLZDad, PoPNewPtr(0x802));
    W32(gLZLson, PoPNewPtr(0x802));
    W32(gLZRson, PoPNewPtr(0xa02));
    uint32_t out = PoPNewHandle(outMax);
    if (R32(gLZTextBuf) == 0 || R32(gLZDad) == 0 || R32(gLZLson) == 0 || R32(gLZRson) == 0 ||
        out == 0 || src == 0) {
        _HUnlock(h);
        if (out != 0) _HPurge(out);
        return 0;
    }
    _HLock(out);
    uint32_t a4 = R32(out);
    tc_memcpy(a4, src, (uint32_t)(int32_t)hdr);
    W32(R32(out) + 0xc, size);
    int32_t d5 = hdr;                                    /* input position */
    uint32_t outPos = (uint32_t)((int32_t)hdr + 4);
    for (int16_t i = 0x401; i <= 0x500; i++) W16(R32(gLZRson) + (uint32_t)((int32_t)i << 1), 0x400);
    for (int16_t i = 0; i < 0x400; i++) W16(R32(gLZDad) + (uint32_t)((int32_t)i << 1), 0x400);
    uint8_t flags = 0, mask = 1;
    int16_t ptr = 0;
    int16_t s = 0, r = 0x3be;
    for (int16_t i = s; i < r; i++) W8(R32(gLZTextBuf) + (uint32_t)(int32_t)i, 0);
    int16_t len = 0;
    {
        uint32_t cur = (uint32_t)d5 + src, end = size + src;
        for (;;) {
            if (cur == end) break;
            W8(R32(gLZTextBuf) + (uint32_t)(int32_t)r + (uint32_t)(int32_t)len, R8(cur));
            cur++; len++;
            if (!(len < 0x42)) break;
        }
        d5 = (int32_t)(cur - src);
    }
    for (int16_t i = 1; i <= 0x42; i++) InsertNode((int16_t)(r - i));
    InsertNode(r);
    uint32_t end = size + src;
    do {
        if ((int32_t)RU16(gLZMatchLen) > (int32_t)len) W16(gLZMatchLen, len);
        if (RU16(gLZMatchLen) < 3) {
            W16(gLZMatchLen, 1);
            flags |= mask;
            codeBuf[ptr++ & 0xf] = R8(R32(gLZTextBuf) + (uint32_t)(int32_t)r);
        } else {
            uint16_t w = (uint16_t)(((RU16(gLZMatchLen) - 3) << 10) | RU16(gLZMatchPos));
            codeBuf[ptr++ & 0xf] = (uint8_t)(w >> 8);
            codeBuf[ptr++ & 0xf] = (uint8_t)w;
        }
        mask = (uint8_t)(mask << 1);
        if (mask == 0) {
            if (outPos + (uint32_t)(int32_t)ptr + 2 < outMax) {
                W8(outPos + a4, flags); outPos++;
                for (int16_t i = 0; i < ptr; i++) { W8(outPos + a4, codeBuf[i & 0xf]); outPos++; }
                flags = 0; ptr = 0; mask = 1;
            } else {
                err = 1;
                goto cleanup;
            }
        }
        int16_t last = R16(gLZMatchLen);
        int16_t i = 0;
        if (i < last) {
            uint32_t cur = (uint32_t)d5 + src;
            for (;;) {
                if (cur == end) break;
                uint8_t c = R8(cur);
                cur++;
                DeleteNode(s);
                W8(R32(gLZTextBuf) + (uint32_t)(int32_t)s, c);
                if (s < 0x41) W8(R32(gLZTextBuf) + (uint32_t)(int32_t)s + 0x400, c);
                s = (int16_t)((s + 1) & 0x3ff);
                r = (int16_t)((r + 1) & 0x3ff);
                InsertNode(r);
                i++;
                if (!(i < last)) break;
            }
            d5 = (int32_t)(cur - src);
        }
        while (i++ < last) {
            DeleteNode(s);
            s = (int16_t)((s + 1) & 0x3ff);
            r = (int16_t)((r + 1) & 0x3ff);
            if (--len != 0) InsertNode(r);
        }
    } while (len > 0);
    if (ptr != 0) {
        if (outPos + (uint32_t)(int32_t)ptr + 2 < size) {
            W8(outPos + a4, flags); outPos++;
            for (int16_t i = 0; i < ptr; i++) { W8(outPos + a4, codeBuf[i & 0xf]); outPos++; }
        } else {
            _HUnlock(h);
            _HUnlock(out);
            err = 1;
        }
    }
cleanup:
    if (R32(gLZTextBuf) != 0) { _DisposePtr(R32(gLZTextBuf)); W32(gLZTextBuf, 0); }
    if (R32(gLZDad) != 0) { _DisposePtr(R32(gLZDad)); W32(gLZDad, 0); }
    if (R32(gLZLson) != 0) { _DisposePtr(R32(gLZLson)); W32(gLZLson, 0); }
    if (R32(gLZRson) != 0) { _DisposePtr(R32(gLZRson)); W32(gLZRson, 0); }
    _HUnlock(h);
    _HUnlock(out);
    if (err) {
        _HPurge(out);
        return 0;
    }
    _SetHandleSize(out, outPos);
    return out;
}
