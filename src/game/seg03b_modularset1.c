/* CODE 3 "Modular Set 1", second half (0x1a8be-0x1dab6): room drawing helpers (dirty
 * rects, half pieces, floor buffers, the object table and its draw order), status-bar
 * messages and life meters, horse/flask/sword background items, piece classification,
 * palettes, and the level/resource-set loading and unloading code.
 * Ported from the Ghidra decompilation and disassembly of the original 68k code; addresses
 * in comments are Ghidra addresses (see docs/PORTING.md).
 *
 * Stack layout: when a ported function is entered from 68k code the return address has
 * already been popped, so emu_sp() is the original's entry SP + 4. ENTER_LINK(N, R) /
 * ENTER_REGS(R) reserve exactly what the original used (return address, LINK A6,#-N frame,
 * R bytes of saved registers), so the 68k code we call sees the same SP as in the original
 * (some of it stores stack addresses in globals), and `a6 - x` is the original's (-x,A6).
 * The reservation is released automatically on every return.
 *
 * A few originals store into their own argument slots (ComputeHalfRect, DrawOppMeter,
 * PoPSetPalette, GetBGSetImgSize). Those slots belong to the 68k caller's frame and are
 * reproduced by hand-written glue ("conv": "manual"); C callers have no such slots. */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* ---- globals (Ghidra addresses) ---- */
#define gGameState      GH(0x102fec)   /* long: pointer to the level/game state block */
#define LVL_TYPE()      R16(R32(gGameState) + 0x2186)   /* level type (scenery set) */
#define LVL_NUM()       R16(R32(gGameState) + 0x218a)   /* level number */
#define gCurBFItem      GH(0x103074)   /* long: background/foreground item being built */
#define gLevelPieces    GH(0x103462)   /* long: pointer to the level's piece table ('PIEC' 0xdac) */
#define gCurRoom        GH(0x10303e)   /* word: current room */
#define gRoomAbove      GH(0x103044)   /* word: room whose bottom row is redrawn on top */
#define gRoomLinkA      GH(0x103040)   /* word: room link word +0x2078 of the room being set up */
#define gRoomLinkB      GH(0x103042)   /* word: room link word +0x207a */
#define gNoTopRow       GH(0x103050)   /* word: nonzero = no room above / partial redraw mode */
#define gCellCol        GH(0x103a08)   /* word: column (0-9) of the cell being drawn */
#define gCellRow        GH(0x103a0a)   /* word: row (0-2) of the cell being drawn */
#define gCellRect       GH(0x103a00)   /* Rect: clip/dirty rect of the cell being drawn */
#define gCellSpec       GH(0x1037e8)   /* long: spec (modifier) of the current cell */
#define gCellPiece      GH(0x1037ec)   /* word: piece id of the current cell */
#define gLeftSpec       GH(0x10362c)   /* long: spec of the cell to the left */
#define gLeftPiece      GH(0x10362a)   /* word: piece id of the cell to the left */
#define gPieceDrawProcs GH(0x103552)   /* long: table of draw procs indexed by piece id */
#define gPieceProcs2    GH(0x10354e)   /* long: second per-level-type proc table */
#define gCustomRoomImgs GH(0x103362)   /* long: custom room images (+0xc = count) */
#define gNumDRects      GH(0x1037cc)   /* word: number of dirty rects */
#define gDRectsOff      GH(0x1037ce)   /* word: nonzero = do not collect dirty rects */
#define gDRects         GH(0x103664)   /* Rect[45]: dirty rects */
#define gOffWorld       GH(0x102dc2)   /* offscreen world record (Save/RestoreCurrentWorld) */
#define gGameWindow     GH(0x102dbe)   /* long: game window */
#define gPlayRect       GH(0x103014)   /* Rect: play area */
#define gStatusRect     GH(0x103024)   /* Rect: status bar (messages, life meters) */
#define gStatusBottom   GH(0x106208)   /* word: bottom of the status bar */
#define gStatusShown    GH(0x103000)   /* word: -1 when the status bar may be drawn */
#define gMsgTimer       GH(0x103034)   /* word: message timer (600 = message just posted) */
#define gMsgTimer2      GH(0x103036)   /* word */
#define gRestartMsg     GH(0x102a96)   /* C string: "press ... to restart" message */
/* objects */
#define gNumObjs        GH(0x1037da)   /* word: number of objects in the object table */
#define gObjTable       GH(0x10334e)   /* long: object table, 0x1c bytes per object */
#define gNumSorted      GH(0x103630)   /* word: number of entries in gSortedObjs */
#define gSortedObjs     GH(0x103632)   /* word[]: object indices in drawing order */
#define gObjForePrio    GH(0x103b20)   /* word[]: draw priority by object kind */
#define gObjForePrio80  GH(0x103b3e)   /* word[]: draw priority for kinds with bit 0x80 */
#define gObjLink        GH(0x104b38)   /* LoadObject: +0x16 linked object */
#define gObjX           GH(0x104b3c)   /* LoadObject: +0 */
#define gObjXCopy       GH(0x1037e6)   /* LoadObject: +0 (copy) */
#define gObjY           GH(0x104b3e)   /* LoadObject: +2 */
#define gObjShape       GH(0x104b40)   /* LoadObject: +6 */
#define gObjList        GH(0x104b42)   /* LoadObject: +4 */
#define gObjA           GH(0x104b3a)   /* LoadObject: +0xa */
#define gObjRect        GH(0x104b44)   /* LoadObject: Rect at +0xe */
#define gObj18          GH(0x104b4c)   /* LoadObject: +0x18 */
#define gObj1A          GH(0x104b4e)   /* LoadObject: +0x1a */
#define gMidTable       GH(0x103356)   /* long: mid-layer item table (0x18 per item) */
#define gNumMid         GH(0x1037d8)   /* word */
#define gMidTable2      GH(0x103352)   /* long: second mid-layer table (0x18 per item) */
#define gNumMid2        GH(0x1037d0)   /* word */
/* horse */
#define gHorseTable0    GH(0x10335e)   /* long: horse items, 0x26 bytes each */
#define gNumHorse0      GH(0x1037d2)
#define gHorseTable1    GH(0x10335a)
#define gNumHorse1      GH(0x1037d4)
#define gHorseShape     GH(0x10355c)   /* long: shape 0x6310 */
/* meters */
#define gKidHP          GH(0x102f4c)
#define gKidMaxHP       GH(0x102f4e)
#define gKidMeterMax    GH(0x103562)   /* word: max HP drawn in the kid meter */
#define gOppMeterHP     GH(0x103560)   /* word: HP drawn in the opponent meter */
#define gMeterShapes    GH(0x103570)   /* long: shape list of the meter bulbs */
#define gChars          GH(0x102df8)   /* characters, 0x3e bytes each */
#define gCurOpp         GH(0x102f68)   /* word: current opponent index */
#define gOppType        GH(0x102fb2)   /* word */
#define gOppRoom        GH(0x102fc0)   /* word */
#define gSwordType      GH(0x102ff6)   /* word */
/* palettes */
#define gSavedPalette   GH(0x103564)   /* long: handle with the saved game-play palette */
#define gColorTable     GH(0x102cbe)   /* long: color table handle */
/* resource sets */
#define gBGSetTable     GH(0x103578)   /* long: BG set table: 8-byte header (+6 count), 10-byte entries */
#define gBGSetState     GH(0x103906)   /* word: 0 loaded, 1 purgeable, 2 shapes missing */
#define gBGSetBusy      GH(0x1039fc)   /* word: MarkBGSetPurgable re-entrancy guard */
#define gBGSetPurged    GH(0x1039fe)   /* word */
#define gLevelRsrc      GH(0x1038dc)   /* 6-byte records per level type: refnum at +0 */
#define gMainRsrc       GH(0x10385a)   /* 6-byte records [2] */
#define gMainRsrc1Ref   GH(0x103860)   /* refnum of gMainRsrc[1] */
#define gMohawkRsrc     GH(0x1037ee)   /* 0x24-byte records [2] */
#define gOppRsrc        GH(0x1039b6)   /* 6-byte records per opponent set */
#define gOppSetLoaded   GH(0x103924)   /* word: opponent set loaded (-1 none) */
#define gSwordSetLoaded GH(0x103926)   /* word */
#define gShapeLists     GH(0x103568)   /* long[5]: loaded shape lists */
#define gOppShapes      GH(0x103574)   /* long: gShapeLists[3] */
#define gKidAttach      GH(0x104b8a)   /* long: 'AFRM' handle of the kid */
#define gKidFrames      GH(0x104b8e)   /* long: 'FRAM' 0x61a9 */
#define gOppFrames      GH(0x104b86)   /* long: 'FRAM' 0x2ee */
#define gOppAttach      GH(0x104b82)   /* long: 'AFRM' 0x2ee */
#define gOppExtra       GH(0x105376)   /* long: resource 0x2f3 */
#define gFlameShapes    GH(0x105480)   /* long: pointer to 46 shape handles (0xfed-0x101a) */
#define gLevelReq       GH(0x10302e)   /* word: level number to load */

#define ROOM_HAS_CHARS(room) R16(R32(gGameState) + (uint32_t)(uint16_t)(room) * 0xc0 + 0x20e6)
#define LEVEL_RSRC_REF() R16(gLevelRsrc + (int16_t)((uint16_t)LVL_TYPE() * 6))
#define OBJ(i)          (R32(gObjTable) + (int32_t)(int16_t)(i) * 0x1c)
#define PIECE_PROC(p)   R32(R32(gPieceDrawProcs) + (uint32_t)(uint16_t)(p) * 4)

static inline void leave_frame(maddr *sp) { emu_set_sp(*sp); }
#define ENTER(n)          maddr entry_sp __attribute__((cleanup(leave_frame))) = emu_sp(); \
                          emu_set_sp(entry_sp - (uint32_t)(n))
#define ENTER_LINK(n, r)  ENTER(8 + (n) + (r)); maddr a6 = entry_sp - 8; (void)a6
#define ENTER_REGS(r)     ENTER(4 + (r))

/* The original pushes a word argument for these 68k routines although their prototypes
 * (and the routines) take none: push it anyway so they see the original stack. */
static uint32_t call_pushing_word(uint32_t (*fn)(void), int16_t w) {
    maddr sp = emu_sp() - 2;
    emu_set_sp(sp);
    W16(sp, w);
    uint32_t r = fn();
    emu_set_sp(sp + 2);
    return r;
}

/* The game caches Toolbox trap addresses at startup (0004ddf6: NGetTrapAddress into
 * A5-0x1f58 SectRect, -0x1f5c UnionRect, -0x1f60 EmptyRect, -0x1f64 ClipRect) and calls
 * the traps through those pointers; the ports call the traps directly. */

/* 0001a8be: redraw the whole room */
uint32_t FUN_0001a8be(void) {
    ENTER_REGS(0);
    seg5_0271fc();
    FUN_00017792();
    MarkPeelForeBufs();
    FUN_0001a8d0();
    return 0;
}

/* 0001a8d0: redraw every cell (rows 2..0), then the bottom row of the room above */
uint32_t FUN_0001a8d0(void) {
    ENTER_REGS(4);
    seg5_024cd0();
    DrawObjs(0x1e);
    W16(gCellRow, 3);
    W16(gCellRow, R16(gCellRow) - 1);   /* subq.w #1,(mem); the test is on the 3 still in D0 */
    for (;;) {
        GetAboveAry();
        GetBelowAry();
        W16(gCellCol, 0);
        do {
            FUN_00024bcc();
            int16_t first = S16(FirstInRow(R16(gCellRow)));
            RedrawCellFast((int16_t)(first + R16(gCellCol)));
            W16(gCellCol, R16(gCellCol) + 1);
        } while (R16(gCellCol) < 10);
        int16_t old = R16(gCellRow);
        W16(gCellRow, old - 1);
        if (old == 0) break;
    }
    if (R16(gNoTopRow) == 0) {
        int16_t room = R16(gCurRoom);
        W16(gCurRoom, R16(gRoomAbove));
        seg3_019120();
        seg5_024cd0();
        W16(gCellRow, 2);
        GetAboveAry();
        GetBelowAry();
        W16(gCellCol, 0);
        while (R16(gCellCol) < 10) {
            FUN_00024bcc();
            RedrawTopFast(R16(gCellCol));
            W16(gCellCol, R16(gCellCol) + 1);
        }
        W16(gCurRoom, room);
        seg3_019120();
    } else {
        FUN_000188a6();
    }
    DrawObjs(-1);
    return 0;
}

/* 0001a9a0: flush the frame: peel, dump the tables, copy the dirty rects */
uint32_t seg3_01a9a0(void) {
    ENTER_REGS(0);
    W16(gNumDRects, 0);
    SaveCurrentWorld(gOffWorld);
    if (R16(GH(0x102dba)) != 0) {
        FUN_0004fbe2();
        SetForeColor(3);
        _PaintRect(gPlayRect);
        seg17_04fc46();
    }
    Peel();
    DumpTable(0);
    DumpTable(3);
    DumpTable(1);
    FUN_0001a30c();
    RestoreOriginalWorld(gOffWorld);
    return 0;
}

/* 0001a9fe: add a dirty rect unless collecting is off or the rect is empty */
uint32_t CheckAddDRect(uint32_t rect) {
    ENTER_LINK(0, 0);
    uint32_t d0 = (uint16_t)R16(gDRectsOff);
    if (d0 != 0) return d0;
    d0 = _EmptyRect(rect) & 0xff;               /* through A5-0x1f60 */
    if (d0 != 0) return d0;
    return AddDRect(rect);
}

/* 0001aa38: merge a rect into an overlapping dirty rect or append it */
uint32_t AddDRect(uint32_t rect) {
    ENTER_LINK(0x10, 12);
    uint32_t d0;
    W32(a6 - 0x10, R32(rect));
    W32(a6 - 0xc, R32(rect + 4));
    _InsetRect(a6 - 0x10, -1, -1);
    maddr dr = gDRects;
    int16_t i = 0;
    while (i < R16(gNumDRects)) {
        if (_SectRect(dr, a6 - 0x10, a6 - 8) & 0xff) {     /* through A5-0x1f58 */
            d0 = _UnionRect(rect, dr, dr);                  /* through A5-0x1f5c */
            return d0;
        }
        i++;
        dr += 8;
    }
    if (R16(gNumDRects) >= 0x2d) {
        d0 = AlertYesNo(fn_address(FN_AddDRect) + (0x1aad8 - 0x1aa38));  /* "DRects Overflow.  Quit?" */
    } else {
        int16_t n = R16(gNumDRects);
        W16(gNumDRects, n + 1);
        d0 = gDRects + (int32_t)n * 8;
        W32(d0, R32(rect));
        W32(d0 + 4, R32(rect + 4));
    }
    return d0;
}

/* 0001aaf0: draw the half piece (0x87-0x90) of the current cell */
uint32_t DrawHalf(void) {
    ENTER_LINK(0x18, 12);
    uint32_t d0 = ComputeHalfRect(a6 - 0x18);
    if (d0 == 0) return d0;
    if (LVL_TYPE() == 2) {
        if (ComputeSlideRockRect(a6 - 0x10, R16(gCurRoom), R16(gCellRow)) != 0 &&
            (_SectRect(a6 - 0x18, a6 - 0x10, a6 - 0x10) & 0xff) != 0)
            W16(a6 - 0x18, R16(a6 - 0xc) - 1);
    }
    W32(a6 - 8, R32(gCellRect));
    W32(a6 - 4, R32(gCellRect + 4));
    W32(gCellRect, R32(a6 - 0x18));
    W32(gCellRect + 4, R32(a6 - 0x14));
    int16_t d3 = 2;
    ResetNextBFItem(d3);
    W16(R32(gCurBFItem) + 0xe, R16(gCellCol));
    W16(R32(gCurBFItem) + 0x10, R16(gCellRow));
    W16(R32(gCurBFItem) + 0x12, R16(gCellPiece));
    W32(R32(gCurBFItem) + 0x14, R32(gCellSpec));
    int16_t d4 = (int16_t)((RU16(R32(gCurBFItem) + 0x14) & 0xc000) >> 14);
    uint32_t proc = PIECE_PROC(RU16(gCellPiece));
    if (proc != 0 && (R16(gNoTopRow) == 0 || d4 == d3 || d4 == 0)) {
        call_ptr(proc, "");
        d3 = (int16_t)(S16(FirstInRow(R16(gCellRow))) + R16(gCellCol));
        MarkFRedraw(d3, a6 - 0x18);
    }
    d0 = R32(gCustomRoomImgs);
    if (d0 != 0 && R16(R32(gCustomRoomImgs) + 0xc) != 0)
        d0 = DrawCustomRoomImgs(2);
    W32(gCellRect, R32(a6 - 8));
    W32(gCellRect + 4, R32(a6 - 4));
    return d0;
}

/* 0001ac2c: rect of the half piece in the current cell (from the kid or a character in
 * the room); returns rect, or ComputeOffsetCellRect's result */
uint32_t ComputeHalfRect(uint32_t rect) {
    ENTER_LINK(8, 16);
    int16_t d5, d6;
    uint16_t piece = RU16(GH(0x102f38));
    if (piece >= 0x87 && piece < 0x91) {
        d5 = R16(GH(0x102f38));
    } else {
        d5 = 0;
        d6 = 0;
        uint16_t room = RU16(gOppRoom);
        int32_t cnt = room != 0 ? (int32_t)ROOM_HAS_CHARS(room) : 0;
        if (cnt > 0) {
            for (;;) {
                int32_t off = (int32_t)d6 * 0x3e;
                if (R16(GH(0x102e00) + (int16_t)off) == 1) {
                    uint16_t d4 = RU16(GH(0x102e02) + (int16_t)off);
                    if (d4 >= 0x87 && d4 < 0x91)
                        d5 = R16(GH(0x102e02) + (int16_t)off);
                }
                d6++;
                room = RU16(gOppRoom);
                cnt = room != 0 ? (int32_t)ROOM_HAS_CHARS(room) : 0;
                if (cnt <= (int32_t)d6) break;
                if (d5 != 0) break;
            }
        }
    }
    if (d5 != 0) {
        maddr src = GH(0x1034ba) + (int32_t)(int16_t)(d5 - 0x87) * 8;
        W32(a6 - 8, R32(src));
        W32(a6 - 4, R32(src + 4));
        if (R16(gCellPiece) == 6) W16(a6 - 4, R16(a6 - 4) + 1);
        int16_t r = R16(gOppRoom);
        if ((r == 7 || r == 8) && LVL_TYPE() == 6) W16(a6 - 8, R16(a6 - 8) + 5);
        rect = ComputeOffsetCellRect(rect, a6 - 8, R16(gCellCol), R16(gCellRow));
    }
    return rect;
}
void glue_ComputeHalfRect(void) {
    maddr sp = emu_sp();
    uint32_t r = ComputeHalfRect(rd32(sp));
    wr32(sp, r);            /* move.l D0,(8,A6): the slot always ends up equal to the result */
    emu_set_d0(r);
}

/* 0001ad8c: draw the floor pieces of the current cell and the one to its left */
uint32_t DrawFloorBuf(void) {
    ENTER_LINK(0x10, 4);
    uint32_t d0;
    W32(a6 - 8, R32(gCellRect));
    W32(a6 - 4, R32(gCellRect + 4));
    int16_t d3 = (int16_t)(R16(gCellRow) * 0x78 + 0x64);
    if (R16(gCellRect) < d3) W16(gCellRect, d3);
    d3 = 0;
    if (LVL_TYPE() == 2) {
        if (ComputeSlideRockRect(a6 - 0x10, R16(gCurRoom), R16(gCellRow)) != 0 &&
            (_SectRect(gCellRect, a6 - 0x10, a6 - 0x10) & 0xff) != 0) {
            W16(gCellRect, R16(a6 - 0xc) - 1);
            d3 = 1;
        } else if (ComputeSlideRockRect(a6 - 0x10, R16(gCurRoom), (int16_t)(R16(gCellRow) + 1)) != 0 &&
                   (_SectRect(gCellRect, a6 - 0x10, a6 - 0x10) & 0xff) != 0) {
            d3 = 1;
        }
    }
    if (d3 == 0 && (int16_t)(d0 = IsSpace(R16(gLeftPiece))) == 0) {
        /* 0001af84: d3 is always 0 here */
        if (d3 != 0 && (int16_t)(d0 = IsSpace(R16(gCellPiece))) != 0 &&
            (int16_t)(d0 = IsWall(R16(gLeftPiece))) != 0) {
            d3 = (int16_t)((RU16(gLeftSpec) & 0xc000) >> 14);
            d0 = 2;
            if (R16(gNoTopRow) == 0 || d3 == 2) {
                d0 = R32(gPieceDrawProcs) + (uint32_t)RU16(gLeftPiece) * 4;
                if (R32(d0) != 0) {
                    ResetNextBFItem(2);
                    W16(R32(gCurBFItem) + 0xe, R16(gCellCol) - 1);
                    W16(R32(gCurBFItem) + 0x10, R16(gCellRow));
                    W16(R32(gCurBFItem) + 0x12, R16(gLeftPiece));
                    W32(R32(gCurBFItem) + 0x14, R32(gLeftSpec));
                    d0 = call_ptr(PIECE_PROC(RU16(gLeftPiece)), "");
                }
            }
        }
    } else {
        d3 = (int16_t)((RU16(gLeftSpec) & 0xc000) >> 14);
        if ((R16(gNoTopRow) == 0 || d3 == 2 || d3 == 0) && PIECE_PROC(RU16(gLeftPiece)) != 0) {
            ResetNextBFItem(2);
            W16(R32(gCurBFItem) + 0xe, R16(gCellCol) - 1);
            W16(R32(gCurBFItem) + 0x10, R16(gCellRow));
            W16(R32(gCurBFItem) + 0x12, R16(gLeftPiece));
            W32(R32(gCurBFItem) + 0x14, R32(gLeftSpec));
            call_ptr(PIECE_PROC(RU16(gLeftPiece)), "");
        }
        d3 = (int16_t)((RU16(gCellSpec) & 0xc000) >> 14);
        if ((R16(gNoTopRow) == 0 || d3 == 2 || d3 == 0) && PIECE_PROC(RU16(gCellPiece)) != 0) {
            ResetNextBFItem(2);
            W16(R32(gCurBFItem) + 0xe, R16(gCellCol));
            W16(R32(gCurBFItem) + 0x10, R16(gCellRow));
            W16(R32(gCurBFItem) + 0x12, R16(gCellPiece));
            W32(R32(gCurBFItem) + 0x14, R32(gCellSpec));
            call_ptr(PIECE_PROC(RU16(gCellPiece)), "");
            d3 = (int16_t)(S16(FirstInRow(R16(gCellRow))) + R16(gCellCol));
            MarkFRedraw(d3, gCellRect);
        }
        d0 = R32(gCustomRoomImgs);
        if (d0 != 0 && R16(R32(gCustomRoomImgs) + 0xc) != 0)
            d0 = DrawCustomRoomImgs(2);
    }
    W32(gCellRect, R32(a6 - 8));
    W32(gCellRect + 4, R32(a6 - 4));
    return d0;
}

/* 0001b046: with which == -1, sort the objects and draw them (each with its linked
 * object), marking drawn ones with -2 */
uint32_t DrawObjs(int16_t which) {
    ENTER_LINK(0x1c, 16);
    uint32_t d0 = (uint16_t)R16(gNumObjs);
    if (d0 == 0 || which != -1) return d0;
    uint32_t save0 = R32(gCellRect), save1 = R32(gCellRect + 4);
    W16(gNumSorted, 0);
    for (int16_t d5 = (int16_t)(R16(gNumObjs) - 1); d5 >= 0; d5--) {
        W16(gSortedObjs + (int16_t)(R16(gNumSorted) * 2), d5);
        W16(gNumSorted, R16(gNumSorted) + 1);
    }
    d0 = (uint16_t)R16(gNumSorted);
    if (d0 != 0) {
        W32(gCellRect, R32(gPlayRect));
        W32(gCellRect + 4, R32(gPlayRect + 4));
        d0 = SortObjTable();
        int16_t d5 = 0;
        if (R16(gNumSorted) > 0) {
            do {
                maddr a2 = gSortedObjs + (int16_t)(d5 * 2);
                int16_t d3 = R16(a2);
                d0 = (uint16_t)R16(OBJ(d3) + 0x16);
                if ((int16_t)d0 >= 0) {
                    DrawObj(d3);
                    d3 = R16(OBJ(R16(a2)) + 0x16);
                    if (d3 > 0 && d3 != 100) {
                        DrawObj(d3);
                        W16(OBJ(R16(OBJ(R16(a2)) + 0x16)) + 0x16, -2);
                    }
                    W16(OBJ(R16(gSortedObjs + (int16_t)(d5 * 2))) + 0x16, -2);
                    d0 = 0xfffffffe;
                }
                d5++;
            } while (d5 < R16(gNumSorted));
        }
    }
    W32(gCellRect, save0);
    W32(gCellRect + 4, save1);
    return d0;
}

/* 0001b1d2: bubble sort gSortedObjs with CompareObj (at most 1000 passes) */
uint32_t SortObjTable(void) {
    ENTER_LINK(4, 28);
    int16_t last = (int16_t)(R16(gNumSorted) - 1);
    int16_t pass = 0, d0, swapped;
    do {
        int16_t d5 = 0;
        swapped = 0;
        if (last > 0) {
            do {
                if (S16(CompareObj(d5, (int16_t)(d5 + 1))) != 0) {
                    maddr a2 = gSortedObjs + (int16_t)(d5 * 2);
                    maddr a3 = GH(0x103634) + (int16_t)(d5 * 2);
                    int16_t t = R16(a2);
                    W16(a2, R16(a3));
                    W16(a3, t);
                    swapped = 1;
                }
                d5++;
            } while (d5 < last);
        }
        d0 = pass++;
    } while (d0 < 1000 && swapped != 0);
    return (uint16_t)d0;
}

static inline int16_t norm_kind(int16_t k) { return (k == 4 || k == 6) ? 2 : k; }

/* 0001b25c: 1 if sorted object a must be drawn after sorted object b */
uint32_t CompareObj(int16_t a, int16_t b) {
    ENTER_LINK(4, 28);
    maddr a4 = OBJ(R16(gSortedObjs + (int16_t)(a * 2)));
    int16_t d5 = R16(a4 + 8);
    int16_t d6 = norm_kind(R16(a4 + 0xc));
    maddr a3 = OBJ(R16(gSortedObjs + (int16_t)(b * 2)));
    int16_t d7 = R16(a3 + 8);
    int16_t kb = norm_kind(R16(a3 + 0xc));
    if (kb == d6) {
        if (R16(a4 + 2) == R16(a3 + 2)) d5 = R16(a4) > R16(a3);
        else d5 = R16(a4 + 2) > R16(a3 + 2);
    } else if ((kb == 3 || kb == 9 || ((kb == 7 || kb == 8) && (d7 == 0xb5 || d7 == 0xa6))) &&
               !(d6 == 3 || d6 == 9 || d6 == 7 || d6 == 8)) {
        d5 = 1;
    } else if ((d6 == 3 || d6 == 9 || ((d6 == 7 || d6 == 8) && (d5 == 0xb5 || d5 == 0xa6))) &&
               !(kb == 3 || kb == 9 || kb == 7 || kb == 8)) {
        d5 = 0;
    } else {
        uint16_t d3 = (uint16_t)GetObjForePriority(d6);
        uint16_t d4 = (uint16_t)GetObjForePriority(kb);
        if (d3 != d4) {
            d5 = d3 < d4;
        } else {
            int16_t ya = R16(a4 + 2), yb = R16(a3 + 2);
            if (ya == yb) d5 = R16(a4) > R16(a3);
            else if ((d6 & 0x80) != 0 || (kb & 0x80) != 0) d5 = ya < yb;
            else if ((d6 == 0 && kb == 3) || (d6 == 2 && kb == 9)) d5 = 0;
            else if ((kb == 0 && d6 == 3) || (kb == 2 && d6 == 9)) d5 = 1;
            else d5 = R16(a4 + 2) > R16(a3 + 2);
        }
    }
    return (uint16_t)d5;
}

/* 0001b494: draw priority of an object kind */
uint32_t GetObjForePriority(int16_t kind) {
    if (kind & 0x80)
        return RU16(gObjForePrio80 + (int16_t)((uint16_t)(kind & 0xff7f) * 2));
    return RU16(gObjForePrio + (int16_t)((uint16_t)kind * 2));
}

/* 0001b4ec: first object with list p4 (+4) and kind p6 (+0xc), or 0 */
uint32_t GetObjTableType(int16_t list, int16_t kind) {
    uint32_t a3 = 0;
    uint32_t a4 = R32(gObjTable);
    int16_t i = 0;
    if (R16(gNumObjs) > 0) {
        do {
            if (R16(a4 + 4) == list && R16(a4 + 0xc) == kind) a3 = a4;
            i++;
            a4 += 0x1c;
        } while (i < R16(gNumObjs) && a3 == 0);
    }
    return a3;
}

/* 0001b54a: draw one object according to its kind */
uint32_t DrawObj(int16_t idx) {
    ENTER_LINK(4, 8);
    uint32_t d0;
    int16_t d3 = (int16_t)LoadObject(idx);
    d0 = (uint16_t)d3;
    switch ((uint16_t)d3) {
    case 0:
        d3 = R16(gObjShape);
        d0 = 0xffffffff;
        if (d3 == -1) break;
        d0 = AddMid(R16(gObjList), (int16_t)(d3 + 1), R16(gObjXCopy), R16(gObjY), 0x10);
        if ((int16_t)d0 == 0) break;
        d0 = (d0 & 0xffff0000) | RU16(gObj18);
        if ((int16_t)d0 == 0) break;
        d0 = R32(gMidTable) + (int32_t)R16(gNumMid) * 0x18;
        W16(d0 - 6, R16(gObj18));
        break;
    case 0xd:
        d0 = call_pushing_word(DrawObjCarpet, d3);
        break;
    case 0x80: case 0x83:
        d0 = DrawObjLoose(d3);
        break;
    case 0x82:
        d0 = AddMid(R16(gObjList), 0x5e, R16(gObjXCopy), R16(gObjY), 0x10);
        break;
    case 0x86: case 0x87: case 0x88: case 0x89:
        d0 = DrawObjSlideRock(d3);
        break;
    case 0x84: case 0x85:
        d0 = DrawObjSpikes(d3);
        break;
    case 0x8a:
        d0 = call_pushing_word(DrawObjTrapDoor, d3);
        break;
    case 0x8b:
        d0 = call_pushing_word(DrawObjBrgFloor, d3);
        break;
    case 0x8c:
        d0 = call_pushing_word(DrawObjFireball, d3);
        break;
    case 0x8d:
        d0 = call_pushing_word(DrawObjGlass, d3);
        break;
    case 3: case 9: case 5: case 2: case 7: case 8: case 0xa: case 6: case 1: case 0xe: {
        d0 = AddMid(R16(gObjList), (int16_t)(R16(gObjShape) + 1), R16(gObjXCopy), R16(gObjY), 0x10);
        if ((int16_t)d0 == 0) break;
        uint32_t a3;
        if (R16(gObjLink) == 100)
            a3 = R32(gMidTable2) + (int32_t)R16(gNumMid2) * 0x18 - 0x18;
        else
            a3 = R32(gMidTable) + (int32_t)R16(gNumMid) * 0x18 - 0x18;
        d0 = 0xffffffe8;
        W16(a3 + 0x12, R16(gObj18));
        W16(a3 + 0x16, R16(gObj1A));
        break;
    }
    default:
        break;
    }
    return d0;
}

/* 0001b79c: draw a loose floor object (the decompiler missed the word argument) */
uint32_t DrawObjLoose(int16_t idx) {
    ENTER_LINK(0, 16);
    int16_t d5, d6 = 0;
    switch ((uint16_t)LVL_TYPE()) {
    case 3:
        call_pushing_word(DrawObjCaveLoose, idx);
        d5 = 0;
        break;
    case 4: d5 = 0xe; d6 = 0x12; break;
    case 2: d5 = 0x1c; d6 = 0x1d; break;
    default: d5 = 0; break;
    }
    if (d5 != 0) {
        W16(gObjA, -1);
        AddMid(4, d5, R16(gObjXCopy), R16(gObjY), 0x10);
        if (d6 != 0) AddMid(4, d6, R16(gObjXCopy), R16(gObjY), 0x10);
    }
    return 0;
}

/* 0001b836: copy object idx into the gObj* globals; returns its kind (+0xc) */
uint32_t LoadObject(int16_t idx) {
    uint32_t a2 = OBJ(idx);
    W16(gObjLink, R16(a2 + 0x16));
    int16_t x = R16(a2);
    W16(gObjX, x);
    W16(gObjXCopy, x);
    W16(gObjY, R16(a2 + 2));
    W16(gObjShape, R16(a2 + 6));
    W16(gObjList, R16(a2 + 4));
    W16(gObjA, R16(a2 + 0xa));
    W32(gObjRect, R32(a2 + 0xe));
    W32(gObjRect + 4, R32(a2 + 0x12));
    W16(gObj18, R16(a2 + 0x18));
    W16(gObj1A, R16(a2 + 0x1a));
    return (a2 & 0xffff0000) | RU16(a2 + 0xc);
}

/* 0001b8c2: draw a message (C string) centered in the status bar, with a shadow */
uint32_t DrawMessage(uint32_t str) {
    ENTER_LINK(8, 0);
    if (R16(gStatusShown) == -1) {
        ClrMessage(1);
        SaveCurrentWorld(gOffWorld);
        _TextFont(0xd4);
        _TextSize(0xe);
        FUN_0004fbe2();
        _ClipRect(gStatusRect);                     /* through A5-0x1f64 */
        W32(a6 - 8, R32(gStatusRect));
        W32(a6 - 4, R32(gStatusRect + 4));
        SetForeColor(7);
        StringRect(a6 - 8, 0, 1, -1, -1, str);      /* long 1, long -1 */
        _OffsetRect(a6 - 8, -1, 0);
        SetForeColor(6);
        StringRect(a6 - 8, 0, 1, -1, -1, str);
        seg17_04fc46();
        RestoreOriginalWorld(gOffWorld);
        _ClipRect(gStatusRect);
        DrawSOffWorldToWindow(gOffWorld, R32(gGameWindow), gStatusRect, gStatusRect, 0);
        _ClipRect(gPlayRect);
    }
    return 0;
}

/* 0001b9aa: show the restart message in the status bar */
uint32_t DrawRestartMessage(void) {
    ENTER_LINK(8, 0);
    if (R16(GH(0x103054)) == 0 && S16(FUN_0002a4be()) == 0 && R16(gStatusShown) == -1) {
        ClrLifeBar();
        SaveCurrentWorld(gOffWorld);
        _TextFont(0xd4);
        _TextSize(0xe);
        FUN_0004fbe2();
        W32(a6 - 8, R32(gStatusRect));
        W32(a6 - 4, R32(gStatusRect + 4));
        W16(a6 - 6, 1);
        W16(a6 - 2, 0x1fe);
        _ClipRect(a6 - 8);
        SetForeColor(7);
        StringRect(a6 - 8, 0, 1, -1, -1, gRestartMsg);
        _OffsetRect(a6 - 8, -1, 0);
        SetForeColor(6);
        _ClipRect(a6 - 8);
        StringRect(a6 - 8, 0, 1, -1, -1, gRestartMsg);
        seg17_04fc46();
        RestoreOriginalWorld(gOffWorld);
        _ClipRect(a6 - 8);
        DrawSOffWorldToWindow(gOffWorld, R32(gGameWindow), a6 - 8, a6 - 8, 0);
        _ClipRect(gPlayRect);
    }
    return 0;   /* D0 is left over from a trap; no caller uses it (ret override) */
}

/* 0001baba: erase the life-bar part of the status bar */
uint32_t ClrLifeBar(void) {
    ENTER_LINK(8, 0);
    if (R16(gStatusShown) == -1) {
        W32(a6 - 8, R32(gStatusRect));
        W32(a6 - 4, R32(gStatusRect + 4));
        W16(a6 - 6, 0);
        W16(a6 - 2, 0x1fe);
        PoPEraseRectInBothPorts(a6 - 8);
    }
    return 0;
}

/* 0001bafa: erase the message area; reset the message timers if asked */
uint32_t ClrMessage(int16_t reset) {
    ENTER_LINK(8, 0);
    uint32_t r;
    if (R16(gMsgTimer) == 600) {
        W32(a6 - 8, R32(gStatusRect));
        W32(a6 - 4, R32(gStatusRect + 4));
        _InsetRect(a6 - 8, (int16_t)-R16(gStatusRect + 2), 0);
        W16(a6 - 2, 0x1fe);
        r = PoPEraseRectInBothPorts(a6 - 8);
    } else {
        r = PoPEraseRectInBothPorts(gStatusRect);
    }
    if (reset != 0) {
        W16(gMsgTimer, 0);
        W16(gMsgTimer2, 0);
    }
    return r & 0xffff0000;   /* D0.w = reset, or 0 */
}

/* 0001bb60: copy all dirty rects to the screen and empty the list */
uint32_t FUN_0001bb60(void) {
    ENTER_REGS(0);
    int16_t n = R16(gNumDRects);
    W16(gNumDRects, n - 1);
    if (n != 0) {
        do {
            PrinceCopy(gDRects + (int32_t)R16(gNumDRects) * 8);
            n = R16(gNumDRects);
            W16(gNumDRects, n - 1);
        } while (n != 0);
    }
    W16(gNumDRects, 0);
    return 0;
}

/* 0001bb96: load (once) the horse shape */
uint32_t GetHorseShape(void) {
    ENTER_LINK(8, 4);
    if (R32(gHorseShape) == 0) {
        uint32_t h = GetShape(0x6310);
        W32(gHorseShape, h);
        if (h != 0) {
            _HNoPurge(h);
            W16(GH(0x103556), 0);
            W16(GH(0x103558), 0);
            W16(GH(0x10355a), 0);
        }
    }
    return R32(gHorseShape);
}

/* 0001bbe2: the horse's point for this room/plane on the horse levels, or 0 */
uint32_t GetHorsePtInPlane(int16_t room, int16_t plane) {
    uint32_t a2 = 0;
    if (plane == 0) {
        if (room == 2 && LVL_NUM() == 9) a2 = GH(0x10350a);
    } else if (plane == 1 && R16(gStatusShown) == -1) {
        if (room == 1 && LVL_NUM() == 0xe) a2 = GH(0x103512);
        else if (room == 0x16 && LVL_NUM() == 0xa) a2 = GH(0x10350e);
    }
    return a2;
}

/* 0001bc78: add a horse item at point pt to plane table 0 or 1 */
uint32_t DrawHorse(uint32_t pt, int16_t plane) {
    if (pt == 0) return 0;
    uint32_t a4;
    if (plane == 0) {
        a4 = R32(gHorseTable0) + (int32_t)R16(gNumHorse0) * 0x26;
        W16(gNumHorse0, R16(gNumHorse0) + 1);
    } else {
        a4 = R32(gHorseTable1) + (int32_t)R16(gNumHorse1) * 0x26;
        W16(gNumHorse1, R16(gNumHorse1) + 1);
    }
    if (R32(gCurBFItem) != 0) {
        W32(gCurBFItem, 0);
    } else {
        W16(a4 + 0xa, plane);
        W16(a4 + 0xe, R16(gCellCol));
        W16(a4 + 0x10, R16(gCellRow));
        W16(a4 + 0xc, 0);
    }
    W16(a4, 4);
    W16(a4 + 2, R16(gCellCol));
    W16(a4 + 4, R16(gCellRow));
    W16(a4 + 6, 0x10);
    W16(a4 + 8, LVL_NUM() == 9 ? 1 : 0);
    W16(a4 + 0x12, 1);
    W16(a4 + 0x1c, R16(pt));
    uint16_t y = RU16(pt + 2);
    W16(a4 + 0x1a, y);
    W16(a4 + 0x18, 0x6310);
    W32(a4 + 0x1e, R32(gCellRect));
    W32(a4 + 0x22, R32(gCellRect + 4));
    return y;
}

/* 0001bd6c: draw a flask (piece 1): table proc for the background, then the flask and
 * its two bubble items */
uint32_t FUN_0001bd6c(void) {
    ENTER_REGS(24);
    uint32_t a2 = R32(gCurBFItem);
    int16_t d4 = R16(a2 + 0xa);
    int16_t d6 = R16(a2 + 0x14);
    int16_t d3, d5, d7;
    if (R32(R32(gPieceDrawProcs) + 4) != 0 && R32(gCustomRoomImgs) == 0) {
        W16(a2 + 0x14, R16(a2 + 0x14) & 0xff00);
        call_ptr(R32(R32(gPieceDrawProcs) + 4), "");
        if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(d4);
        W16(R32(gCurBFItem) + 0x14, d6);
    }
    d3 = 1;
    if (d4 != d3) return 0;
    d4 = d6 & 0xe0;
    d5 = (d4 < 0xa0 && d4 >= 0x40) ? 1 : 0;
    W16(R32(gCurBFItem), d3);
    {
        uint32_t src = R32(gLevelPieces) + 0xd6, dst = R32(gCurBFItem) + 0x18;
        W32(dst, R32(src));
        W16(dst + 4, R16(src + 4));
    }
    W16(R32(gCurBFItem) + 0x18, d5 + 1);
    if (S16(AddNextBFItemToTable()) != 0) {
        ResetNextBFItemWithPrevInfo(d3);
        W16(R32(gCurBFItem), d3);
    }
    if (d4 != 0) {
        d4 = d6 & 0x1f;
        if (d4 != 0x1c && (d4 < 10 || d4 > 13) && (int32_t)d4 - 14 < 10) {
            d7 = (int16_t)GetFlaskBubbleColor(d6);
            if (d4 > 13) { d4 -= 14; d6 = 1; } else d6 = 0;
            W16(R32(gCurBFItem) + 6, d7);
            W16(R32(gCurBFItem) + 8, d6);
            W16(R32(gCurBFItem) + 0x18, d4 + 3);
            W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0x4c0));
            W16(R32(gCurBFItem) + 0x1c, d5 ? -0x25 : -0x21);
            if (S16(AddNextBFItemToTable()) != 0) {
                d3 = 1;
                ResetNextBFItemWithPrevInfo(d3);
                W16(R32(gCurBFItem), d3);
            }
            W16(R32(gCurBFItem) + 6, d7 + 1);
            W16(R32(gCurBFItem) + 8, d6);
            W16(R32(gCurBFItem) + 0x18, d4 + 0xd);
            W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0x4c0));
            W16(R32(gCurBFItem) + 0x1c, d5 ? -0x25 : -0x21);
            AddNextBFItemToTable();
        }
    }
    if (R32(gCurBFItem) != 0) W16(R32(gCurBFItem), 4);
    return 0;
}

/* 0001bf10: bubble color index of a flask from its spec */
uint32_t GetFlaskBubbleColor(int16_t spec) {
    switch ((uint16_t)(spec & 0xe0)) {
    case 0x20: case 0x40: return 0xe9;
    case 0xa0: return 0xed;
    default: return 0xeb;
    }
}

/* 0001bf72: draw the kid's life meter (hp full bulbs, up to max) */
uint32_t DrawKidMeter(int16_t hp, int16_t max) {
    ENTER_LINK(8, 12);
    SaveCurrentWorld(gOffWorld);
    int16_t d5 = 3, d4;
    W16(a6 - 6, d5);
    W16(a6 - 8, 0x170);
    W16(a6 - 4, R16(gStatusBottom));
    W16(a6 - 2, 3);
    if (max != 0) W16(a6 - 2, R16(a6 - 2) + (int16_t)(max * 12));
    else W16(a6 - 2, R16(a6 - 2) + 12);
    if (max >= 1 && R16(gKidMaxHP) < R16(gKidMeterMax)) {
        W16(a6 - 2, (int16_t)(R16(gKidMeterMax) * 12 + 3));
        W16(gKidMeterMax, R16(gKidMaxHP));
    }
    _ClipRect(a6 - 8);
    _EraseRect(a6 - 8);
    for (d4 = 0; d4 < hp; d4++, d5 += 12)
        PrincePaintShapeIdx(R32(gMeterShapes), 0x12c, d5, 0x170, 0x10);
    d4 = hp;
    if (hp < max) {
        do {
            PrincePaintShapeIdx(R32(gMeterShapes), 0x12b, d5, 0x170, 0x10);
            d4++;
            d5 += 12;
        } while (d4 < max);
    }
    if (max > R16(gKidMeterMax)) W16(gKidMeterMax, max);
    RestoreOriginalWorld(gOffWorld);
    _ClipRect(a6 - 8);
    DrawSOffWorldToWindow(gOffWorld, R32(gGameWindow), a6 - 8, a6 - 8, 0);
    _ClipRect(gPlayRect);
    return 0;
}

/* 0001c0ac: redraw the opponent's and the kid's life meters */
uint32_t FUN_0001c0ac(void) {
    ENTER_REGS(8);
    int16_t d3 = R16(gCurOpp);
    if (d3 >= 0 && d3 < 5) {
        uint32_t a2 = gChars + (int32_t)d3 * 0x3e;
        DrawOppMeter(R16(a2), R16(a2 + 0x1e));
    }
    DrawKidMeter(R16(gKidHP), R16(gKidMaxHP));
    return 0;
}

/* 0001c0fe: draw (or erase) the opponent's life meter */
static uint32_t DrawOppMeter_(int16_t opp, int16_t hp, maddr args) {
    ENTER_LINK(8, 20);
    int16_t d3 = -1, d4, d5 = 0, d6 = 0, d7;
    if (opp == -1) {
        opp = (int16_t)GetOppIdxClosestToKid(1);
        if (args) W16(args, opp);
        if (opp == d3) {
            uint16_t room = RU16(gCurRoom);
            if (R16(gCurRoom) != 0 && ROOM_HAS_CHARS(room) != 0) {
                opp = 0;
                if (args) W16(args, opp);
            }
        }
    }
    d3 = opp;
    if (d3 != -1) {
        LoadShad(d3);
        d5 = R16(gOppType);
        d6 = R16(gOppRoom);
    }
    W16(a6 - 6, 0x19b);
    W16(a6 - 8, 0x170);
    W16(a6 - 2, 0x1fb);
    W16(a6 - 4, R16(gStatusBottom));
    int hide = 0;
    if (hp == 0 || opp == -1) hide = 1;
    else if (R16(gCurRoom) == 0 || ROOM_HAS_CHARS(RU16(gCurRoom)) == 0) hide = 1;
    else if (d5 == 10 || d5 == 11 || d5 == 0) hide = 1;
    else if (d5 == 4 && R16(GH(0x102fb8)) != 0 && LVL_NUM() == 5 && (d6 == 10 || d6 == 7 || d6 == 12)) hide = 1;
    if (hide) {
        if (R16(gOppMeterHP) != 0) {
            W16(gOppMeterHP, 0);
            PoPEraseRectInBothPorts(a6 - 8);
        }
    } else if (R16(gMsgTimer) != 600 && R16(gOppMeterHP) != hp) {
        W16(gOppMeterHP, hp);
        if (d5 == 7 || d5 == 8) {
            d3 = hp & 1;
            hp = (int16_t)(hp >> 1);
            if (args) W16(args + 2, hp);
        } else {
            d3 = 0;
        }
        SaveCurrentWorld(gOffWorld);
        _ClipRect(a6 - 8);
        _EraseRect(a6 - 8);
        d7 = 0x1ef;
        for (d4 = 0; d4 < hp; d4++, d7 -= 12)
            PrincePaintShapeIdx(R32(gMeterShapes), 0x12a, d7, 0x170, 0x10);
        if ((d5 == 7 || d5 == 8) && d3 != 0)
            PrincePaintShapeIdx(R32(gMeterShapes), 0x129, d7, 0x170, 0x10);
        RestoreOriginalWorld(gOffWorld);
        _ClipRect(a6 - 8);
        DrawSOffWorldToWindow(gOffWorld, R32(gGameWindow), a6 - 8, a6 - 8, 0);
        W16(GH(0x102fcc), 0);
    }
    _ClipRect(gPlayRect);
    return 0;
}
uint32_t DrawOppMeter(int16_t opp, int16_t hp) { return DrawOppMeter_(opp, hp, 0); }
void glue_DrawOppMeter(void) {
    maddr sp = emu_sp();
    emu_set_d0(DrawOppMeter_((int16_t)rd16(sp), (int16_t)rd16(sp + 2), sp));
}

/* 0001c342: draw a sword lying on the floor (piece 1) */
uint32_t DrawSword(void) {
    ENTER_LINK(4, 20);
    uint32_t a2 = R32(gCurBFItem);
    int16_t d6 = R16(a2);
    int16_t d3 = R16(a2 + 0xa);
    int16_t d4 = R16(a2 + 0x14);
    if (R32(R32(gPieceDrawProcs) + 4) != 0 && !(R16(GH(0x102f44)) == 9 && LVL_NUM() == 8)) {
        W16(a2 + 0x14, R16(a2 + 0x14) & 0xfffc);
        call_ptr(R32(R32(gPieceDrawProcs) + 4), "");
        if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(d3);
        W16(R32(gCurBFItem) + 0x14, d4);
    }
    if (d3 == 1) {
        uint32_t src = R32(gLevelPieces) + 0x1c6, dst = R32(gCurBFItem) + 0x18;
        W32(dst, R32(src));
        W16(dst + 4, R16(src + 4));
        int16_t d5 = d4 & 3;
        d3 = R16(gSwordType);
        if (d5 == d3 || (d3 == -1 && d5 == 1)) W16(R32(gCurBFItem) + 0x18, 0xb);
        else W16(R32(gCurBFItem) + 0x18, 0x2e);
        W16(R32(gCurBFItem), 0);
        AddNextBFItemToTable();
        if (R32(gCurBFItem) != 0) W16(R32(gCurBFItem), d6);
    }
    return 0;
}

/* 0001c416: piece is neither a wall nor empty space */
uint32_t IsFloor(int16_t piece) {
    ENTER_LINK(0, 0);
    if (S16(IsWall(piece)) != 0 || S16(IsSpace(piece)) != 0) return 0;
    return 1;
}

/* 0001c452 */
uint32_t IsImpassable(int16_t piece) {
    return piece == 0xb || piece == 0xf || piece == 0x1a || piece == 0xc || piece == 0xd ||
           piece == 0x17 || piece == 0x18;
}

/* 0001c4a2 */
uint32_t IsLandOnable(int16_t piece) {
    ENTER_LINK(0, 0);
    if (S16(IsFloor(piece)) == 0) return 0;
    if (S16(IsImpassable(piece)) != 0) return 0;
    if (piece == 6 || piece == 0x22) return 0;
    return 1;
}

/* 0001c4ec */
uint32_t IsMobFloor(int16_t piece) {
    return piece == 0xb || piece == 0x1a || piece == 0xf;
}

/* 0001c522 */
uint32_t IsSpace(int16_t piece) {
    return piece == 0 || piece == 9 || piece == 0x1b || piece == 0x2f || piece == 0x31;
}

/* 0001c562 */
uint32_t IsWall(int16_t piece) {
    return piece == 0x14 || piece == 2 || piece == 7 || piece == 0x19 || piece == 0x2b;
}

/* 0001c5a0: restore the saved game-play palette and free it */
uint32_t FUN_0001c5a0(void) {
    ENTER_REGS(4);
    if (R32(gSavedPalette) != 0) {
        SetColors(R32(gSavedPalette), 7, 1, 1);
        ShowColors(R32(gGameWindow), 1, 0xfe);
        _DisposeHandle(R32(gSavedPalette));
        W32(gSavedPalette, 0);
    }
    return 0;
}

/* 0001c5de: save the game-play palette (once) and fade the window to color 1 */
uint32_t SaveGamePlayPalette(void) {
    ENTER_LINK(0x10, 0);
    if (R32(gSavedPalette) == 0) {
        uint32_t h = PoPNewHandle(0x808);
        W32(gSavedPalette, h);
        ColorTableTovCtab(R32(gColorTable), h);
    }
    SetWholePaletteToColor(R32(gGameWindow), 1);
    return 0;
}

/* 0001c62c: set the whole palette to one color of the color table */
uint32_t SetPaletteToColorIdx(int16_t idx) {
    ENTER_LINK(0x14, 20);
    uint32_t a3 = PoPNewHandle(0x3fa);
    if (a3 != 0) {
        GetNthvRGB((int16_t)((idx & 0xff) << 8), a6 - 8);   /* move.b: the byte goes in the high half */
        W8(a6 - 0xc, R8(a6 - 7));
        W8(a6 - 0xb, R8(a6 - 5));
        W8(a6 - 0xa, R8(a6 - 3));
        uint32_t a4 = R32(a3);
        W16(a4, 0xfe);
        W16(a6 - 2, 0);
        do {
            uint32_t a2 = a4 + (int32_t)R16(a6 - 2) * 4;
            W8(a2 + 2, R8(a6 - 0xc));
            W8(a2 + 3, R8(a6 - 0xb));
            W8(a2 + 4, R8(a6 - 0xa));
            W8(a2 + 5, R8(a6 - 1) + 1);
            W16(a6 - 2, R16(a6 - 2) + 1);
        } while (RU16(a6 - 2) < 0xfe);
        SaveGamePlayPalette();
        SetColors(a3, 7, 1, 1);
        ShowColors(R32(gGameWindow), 1, 0xfe);
        _DisposeHandle(a3);
    }
    return 0;
}

/* 0001c6fa: install a 'CTBL' palette (optionally remapped); while a game-play palette is
 * saved, only patch the saved copy */
static uint32_t PoPSetPalette_(int16_t id, int16_t remap, maddr args) {
    ENTER_LINK(4, 20);
    uint32_t a3 = GetCTBLResource(id);
    _HLock(a3);
    uint32_t a4 = R32(a3);
    if (remap >= 0) RemapCTBL(a3, remap);
    if (R32(gSavedPalette) != 0) {
        int16_t len = (int16_t)(R16(a4) << 2);
        uint32_t a2 = R32(R32(gSavedPalette));
        uint16_t first = R8(a4 + 5);
        if (args) W16(args + 2, first);     /* move.w D0,(0xa,A6) */
        _BlockMove(a4 + 2, a2 + 2 + (int32_t)(int16_t)first * 4, (int32_t)len);
    } else {
        SetColors(a3, 7, 1, 1);
        ShowColors(R32(gGameWindow), remap, R16(a4));
    }
    _HUnlock(a3);
    _HPurge(a3);
    return 1;
}
uint32_t PoPSetPalette(int16_t id, int16_t remap) { return PoPSetPalette_(id, remap, 0); }
void glue_PoPSetPalette(void) {
    maddr sp = emu_sp();
    emu_set_d0(PoPSetPalette_((int16_t)rd16(sp), (int16_t)rd16(sp + 2), sp));
}

/* 0001c7a6 */
uint32_t TweakKidShpList(void) {
    return 0;
}

/* 0001c7c2: 1 if the pieces above (or above-right) let the cell be seen */
uint32_t VisibleAbovePiece(int16_t room, int16_t cell) {
    ENTER_LINK(0, 12);
    int16_t d3 = 0;
    int16_t d4 = (int16_t)PieceAboveId(room, cell);
    if (S16(IsSpace(d4)) != 0 || d4 == 5 || d4 == 6 || (d4 == 3 && LVL_NUM() == 0xd)) {
        d3 = 1;
    } else {
        int16_t d5 = (int16_t)PieceAboveRightId(room, cell);
        if (S16(IsSpace(d5)) != 0 || d5 == 5 || d5 == 6 || (d4 == 3 && LVL_NUM() == 0xd)) d3 = 1;
    }
    return (uint16_t)d3;
}

/* 0001c86a: switch the level-type resource file from old to new */
uint32_t OpenLevelRsrcFile(int16_t newType, int16_t oldType) {
    ENTER_LINK(0, 4);
    if (newType != oldType) {
        if (oldType != 0) CloseRsrcFile(gLevelRsrc + (uint32_t)(uint16_t)oldType * 6);
        OpenRsrcVerify(gLevelRsrc + (uint32_t)(uint16_t)newType * 6);
    }
    return 0;
}

/* 0001c8e8: load opponent set if it is not the loaded one (and the level needs it) */
uint32_t CheckLoadedOppSet(int16_t set) {
    ENTER_LINK(0, 8);
    if (R16(gOppSetLoaded) != set) {
        int16_t d4 = R16(R32(gGameState) + 0x21a4);
        if (d4 != 5 && d4 != 6) {
            d4 = R16(gOppType);
            if (d4 != 0xb && d4 != 0 && d4 != 6) LoadOppSet(set);
        }
    }
    return 0;
}

/* 0001c94a: close the main resource files and the Mohawk archives */
uint32_t FUN_0001c94a(void) {
    ENTER_REGS(4);
    uint16_t i;
    for (i = 0; i < 2; i++) CloseRsrcFile(gMainRsrc + (int32_t)(int16_t)i * 6);
    for (i = 0; i < 2; i++) CloseMohawkRsrc(gMohawkRsrc + (int32_t)(int16_t)i * 0x24);
    return 0;
}

/* 0001c9aa: width/height of BG set image idx into *size (loads it if flagged); returns
 * size, or 0 if the shape is not in memory */
uint32_t GetBGSetImgSize(int16_t idx, uint32_t size) {
    ENTER_LINK(0, 16);
    uint32_t a4 = R32(gBGSetTable) + 8 + (int32_t)idx * 10;
    uint32_t a2;
    if (R16(a4 + 2) != 0) {
        W16(a4 + 2, 0);
        a2 = R32(a4 + 6);
        if (a2 == 0 || R32(a2) == 0) {
            int16_t cur = (int16_t)_CurResFile();
            _UseResFile(LEVEL_RSRC_REF());
            a2 = GetShape((int16_t)(idx + 0xdad));   /* not stored back (sic) */
            _UseResFile(cur);
        }
        _HNoPurge(a2);
        W16(gBGSetState, 2);
    } else {
        a2 = R32(a4 + 6);
    }
    uint32_t a3 = R32(a2);
    if (a3 != 0) {
        W16(size, R16(a3 + 6));
        W16(size + 2, R16(a3 + 4));
    } else {
        size = 0;
    }
    return size;
}
void glue_GetBGSetImgSize(void) {
    maddr sp = emu_sp();
    uint32_t r = GetBGSetImgSize((int16_t)rd16(sp), rd32(sp + 2));
    wr32(sp + 2, r);        /* clr.l (0xa,A6) when the shape is gone: slot == result */
    emu_set_d0(r);
}

/* 0001ca84: shape handle of BG set item idx (1-based), loading it if flagged */
uint32_t GetBGSetShapeItem(int16_t idx) {
    ENTER_LINK(0, 12);
    uint32_t a3 = R32(gBGSetTable) + 8 + (int32_t)idx * 10 - 10;
    if (R16(a3 + 2) != 0) {
        W16(a3 + 2, 0);
        uint32_t a2 = R32(a3 + 6);
        if (a2 == 0 || R32(a2) == 0) {
            int16_t cur = (int16_t)_CurResFile();
            _UseResFile(LEVEL_RSRC_REF());
            W32(a3 + 6, GetShape((int16_t)(idx + 0xdac)));
            _UseResFile(cur);
        }
        _HNoPurge(R32(a3 + 6));
        W16(gBGSetState, 2);
    }
    return R32(a3 + 6);
}

/* 0001cb40: load the level type's background set (and its palette) */
uint32_t LoadBGSet(int16_t setPalette) {
    ENTER_LINK(0, 4);
    int16_t cur = (int16_t)_CurResFile();
    _UseResFile(LEVEL_RSRC_REF());
    LoadSet(4, 0xdac);
    if (setPalette != 0) PoPSetPalette(0xdac, 0x40);
    MarkBGSetPurgable(0);
    _UseResFile(cur);
    if (LVL_TYPE() == 6) LoadFlameShapes(2);
    return 0;
}

/* 0001cbce: load the flame shapes 0xfed-0x101a (bit 0: first half, bit 1: second half) */
uint32_t LoadFlameShapes(int16_t which) {
    ENTER_LINK(0, 20);
    if (R32(gFlameShapes) != 0) return R32(gFlameShapes);
    int16_t cur = (int16_t)_CurResFile();
    _UseResFile(R16(gMainRsrc1Ref));
    int16_t d5 = 0xb8, d3, d4;
    uint32_t a2 = PoPNewPtr((uint32_t)(int32_t)d5);
    W32(gFlameShapes, a2);
    tc_memset(a2, 0, (uint32_t)(int32_t)d5);
    if (which & 1) {
        d3 = 0xfed;
    } else {
        d3 = 0xffc;
        a2 = R32(gFlameShapes) + 0x3c;
    }
    d4 = (which & 2) ? 0x101a : 0x100e;
    for (; d3 <= d4; d3++, a2 += 4) {
        uint32_t h = GetShape(d3);
        W32(a2, h);
        if (h != 0) _HNoPurge(h);
    }
    return _UseResFile(cur);   /* D0 after the trap; no caller uses it (ret override) */
}

/* 0001cc84: load (or unload) the flask shape set */
uint32_t LoadFlaskSet(void) {
    ENTER_LINK(4, 4);
    if (R8(R32(gGameState) + 0x2197) != 0) {
        int16_t cur = (int16_t)_CurResFile();
        _UseResFile(R16(gMainRsrc));
        LoadSet(1, 0xbb8);
        if (LVL_TYPE() != 6) PoPSetPalette(0xbb8, 0xe0);
        _UseResFile(cur);
    } else {
        UnloadSet(1);
    }
    return 0;
}

/* 0001ccf0: load level (or reload the current one for -1/-2) with a watch cursor */
uint32_t LoadLevel(int16_t level) {
    ENTER_LINK(0x44, 24);
    uint32_t d0;
    uint32_t a3 = _GetCursor(4);
    if (a3 != 0) {
        _HLock(a3);
        uint32_t src = R32(a3);
        for (int i = 0; i < 0x44; i += 4) W32(a6 - 0x44 + i, R32(src + i));
        _SetCursor(a6 - 0x44);
    }
    UnloadLevel(3);
    int16_t oldType = LVL_TYPE();
    int16_t oldNum = oldType == 0 ? 0 : LVL_NUM();
    int16_t d3 = level;
    if (d3 != -2 && d3 != -1 && d3 != R16(gLevelReq)) {
        if (R16(GH(0x102de4)) == 0) FUN_00011bce();
        W16(gLevelReq, level);
    }
    d3 = R16(gLevelReq);
    W16(GH(0x10306e), d3);
    LoadLevelMap(d3);
    int16_t d6 = (int16_t)FUN_00010518();
    if (d6 == 0) {
        ResetStandardColors(R32(gGameWindow));
        PoPEraseRectInBothPorts(GH(0x106204));
    }
    uint32_t gs = R32(gGameState);
    LoadCodeOverlays(R16(gs + 0x2186), R16(gs + 0x21a4));
    LoadFlaskSet();
    OpenLevelRsrcFile(LVL_TYPE(), oldType);
    LoadBGSet(d6 == 0 ? 1 : 0);
    LoadKidSet();
    LoadOppSet(R16(R32(gGameState) + 0x21a4));
    FUN_0001d85c();
    LoadSwordSet(R16(gSwordType));
    LoadLevelSounds(LVL_NUM(), oldNum);
    FUN_0001d090();
    d3 = LVL_TYPE();
    if (d3 != oldType) SetKidPalette(d3);
    d3 = level;
    if (d3 != -2 && d3 != -1 && LVL_TYPE() == 1) seg18_05588a();
    SetPrinceLevelSpecPtrs(LVL_TYPE());
    d0 = a3;
    if (a3 != 0) {
        _HUnlock(a3);
        _HPurge(a3);
        d0 = _SetCursor(GH(0x106716));
    }
    return d0;   /* no caller uses it (ret override) */
}

/* 0001ce6a: read 'LEVL' (level + 1999) into the game state block */
uint32_t LoadLevelMap(int16_t level) {
    ENTER_LINK(0, 16);
    int16_t cur = (int16_t)_CurResFile();
    _UseResFile(R16(gMainRsrc));
    uint32_t h = PoPGetResource(0x4c45564c, (int16_t)(level + 0x7cf));   /* 'LEVL' */
    _BlockMove(R32(h), R32(gGameState), 0x49ca);
    _UseResFile(cur);
    FUN_0001d48e();
    FUN_00011d52();
    return 0;
}

/* 0001ceca: minimal level reload (map, sword, sounds) */
uint32_t LoadLevelMinimum(int16_t level) {
    ENTER_LINK(0, 0);
    MarkBGSetPurgable(0);
    LoadLevelMap(level);
    FUN_0001d85c();
    LoadSwordSet(R16(gSwordType));
    LoadLevelSounds(level, 0);
    SaveGamePlayPalette();
    PoPEraseRectInBothPorts(GH(0x106204));
    return 0;
}

/* 0001cf1e: unload the sounds of level oldLvl and load those of newLvl (tables of
 * (id, count) pairs ending with -1, per level 1-14) */
uint32_t LoadLevelSounds(int16_t newLvl, int16_t oldLvl) {
    ENTER_LINK(0, 20);
    uint32_t d0;
    uint32_t a3, a2;
    int16_t d5;
    if (newLvl != oldLvl && oldLvl != 0 && (uint16_t)oldLvl <= 0xe) {
        a3 = R32(GH(0x103e9e) + (int16_t)((uint16_t)oldLvl * 4));
        d5 = 0;
        if (R16(a3) != -1) {
            do {
                a2 = a3 + (int32_t)d5 * 2;
                UnloadSounds(R16(a2), R16(a2 + 2));
                d5 += 2;
            } while (R16(a3 + (int32_t)d5 * 2) != -1);
        }
    }
    d0 = (uint16_t)newLvl;
    if (newLvl != 0 && (uint16_t)newLvl <= 0xe) {
        a3 = R32(GH(0x103e9e) + (int16_t)((uint16_t)newLvl * 4));
        d5 = 0;
        d0 = 0xffff;
        if (R16(a3) != -1) {
            do {
                a2 = a3 + (int32_t)d5 * 2;
                LoadSounds(R16(a2), R16(a2 + 2));
                d5 += 2;
            } while (R16(a3 + (int32_t)d5 * 2) != -1);
        }
    }
    return d0;   /* low word: newLvl if no table was used, else -1 */
}

/* 0001cff6: initialise the level type's code overlay and the opponent code */
uint32_t LoadCodeOverlays(int16_t lvlType, int16_t oppSet) {
    ENTER_LINK(0, 4);
    CheckCurrentFreeMemory(0);
    switch (lvlType) {
    case 1: FUN_0005591c(); break;
    case 2: seg10_03b736(); break;
    case 3: FUN_00036bd8(); break;
    case 4: seg7_0338d8(); break;
    case 5: FUN_0006e508(); break;
    case 6: seg9_037c90(); break;
    }
    CheckCurrentFreeMemory(0);
    if (oppSet == 5 || oppSet == 6 || oppSet == 8) FUN_000305b4();
    else FUN_0001dad4();
    return 0;
}

/* 0001d090: load and lock the level's piece table ('PIEC' 0xdac) */
uint32_t FUN_0001d090(void) {
    ENTER_REGS(4);
    FUN_0001da46();
    uint32_t h = PoPGetResource(0x50494543, 0xdac);   /* 'PIEC' */
    _HNoPurge(h);
    _MoveHHi(h);
    _HLock(h);
    uint32_t p = R32(h);
    W32(gLevelPieces, p);
    return p;
}

/* 0001d0be: load the kid's sword shape set and attach frames */
uint32_t LoadSwordSet(int16_t sword) {
    ENTER_LINK(4, 8);
    uint32_t d0;
    if (LVL_TYPE() != 1) {
        if (sword != R16(gSwordSetLoaded)) {
            if (R32(gKidAttach) != 0) {
                _HPurge(R32(gKidAttach));
                W32(gKidAttach, 0);
            }
            UnloadSet(0);
        }
        if (R32(gKidAttach) == 0) W32(gKidAttach, LoadKidAttachFrames(sword));
        int16_t cur = (int16_t)_CurResFile();
        _UseResFile(R16(gMainRsrc));
        int16_t d3 = 0x3e8;
        if (sword == 2) d3 += 0xc8;
        LoadSet(0, d3);
        if (!(R16(gCurRoom) == 1 && LVL_NUM() == 0xe))
            PoPSetPalette((int16_t)(LVL_TYPE() + 0x3e8), 0xf0);
        d0 = _UseResFile(cur);
        W16(gSwordSetLoaded, sword);
    } else {
        d0 = R32(gKidAttach);
        if (d0 == 0) {
            d0 = LoadKidAttachFrames(sword);
            W32(gKidAttach, d0);
        }
    }
    return d0;   /* no caller uses it (ret override) */
}

/* 0001d19a: 'AFRM' of the kid for this sword type */
uint32_t LoadKidAttachFrames(int16_t sword) {
    ENTER_LINK(0, 8);
    int16_t id = 0x61a9;
    if (sword == 2) id = 0x6271;
    uint32_t h = PoPGetResource(0x4146524d, id);   /* 'AFRM' */
    _HNoPurge(h);
    return h;
}

/* 0001d1e8: mark the BG set shapes purgeable (purge != 0) or make them resident again,
 * reloading purged ones */
uint32_t MarkBGSetPurgable(int16_t purge) {
    ENTER_LINK(0xc, 24);
    if (R16(gBGSetBusy) != 0) return (uint16_t)R16(gBGSetBusy);
    W16(gBGSetBusy, 1);
    if (R32(gBGSetTable) != 0 && LVL_TYPE() != 0 && LEVEL_RSRC_REF() != 0 &&
        ((purge != 0 && R16(gBGSetState) != 1) || (purge == 0 && R16(gBGSetState) != 0))) {
        int16_t cur = (int16_t)_CurResFile();
        _UseResFile(LEVEL_RSRC_REF());
        if (purge != 0) {
            W16(gBGSetState, 1);
            W16(gBGSetPurged, 1);
        } else {
            W16(gBGSetState, 0);
        }
        uint32_t a4 = R32(gBGSetTable);
        uint32_t ent = R32(gBGSetTable) + 8;
        uint16_t d5 = 0;
        if ((int32_t)d5 < (int32_t)R16(R32(gBGSetTable) + 6)) {
            uint32_t pH = ent + 6;      /* (-8,A6): the entry's handle */
            uint32_t pFlag = ent + 2;   /* (-4,A6): the entry's "needs load" flag */
            do {
                if (R32(pH) != 0) {
                    if (purge != 0) {
                        if (R16(pFlag) == 0) {
                            W16(pFlag, 1);
                            _HPurge(R32(pH));
                        }
                    } else if (R16(pFlag) != 0) {
                        W16(pFlag, 0);
                        uint32_t a3 = R32(pH);
                        if (a3 == 0 || R32(a3) == 0) W32(pH, GetShape((int16_t)(d5 + 0xdad)));
                        uint32_t a2 = R32(pH);
                        if (a2 != 0) {
                            _HNoPurge(a2);
                        } else {
                            d5 = RU16(a4 + 6);
                            W16(gBGSetState, 2);
                        }
                    }
                }
                d5++;
                pH += 10;
                pFlag += 10;
                ent += 10;
            } while ((int32_t)d5 < (int32_t)R16(a4 + 6));
        }
        _UseResFile(cur);
    }
    W16(gBGSetBusy, 0);
    return 0;
}

/* 0001d382: open the main resource files and the Mohawk archives */
uint32_t FUN_0001d382(void) {
    ENTER_REGS(4);
    uint16_t i;
    for (i = 0; i < 2; i++) OpenRsrcVerify(gMainRsrc + (int32_t)(int16_t)i * 6);
    for (i = 0; i < 2; i++) OpenMohawkRsrcVerify(gMohawkRsrc + (int32_t)(int16_t)i * 0x24);
    W32(GH(0x10608a), R32(gMohawkRsrc));
    W32(GH(0x10608e), R32(GH(0x103812)));
    return 0;
}

/* 0001d3f2: select the per-level-type piece proc tables */
uint32_t SetPrinceLevelSpecPtrs(int16_t lvlType) {
    int16_t off = (int16_t)((uint16_t)lvlType << 2);
    W32(gPieceDrawProcs, R32(GH(0x103516) + off));
    W32(gPieceProcs2, R32(GH(0x103532) + off));
    return 0;
}

/* 0001d43e: clear bit 7 of cell state i if its type is 7 */
uint32_t InitState(int16_t i) {
    uint32_t a2 = R32(GH(0x1037dc)) + (int32_t)i * 4;
    int16_t type = R16(R32(GH(0x1037e0)) + (int32_t)i * 2);
    if (type == 7) W16(a2, R16(a2) & 0xff7f);
    return 0;
}

/* 0001d48e: per room: CalcBlue, room links, InitState of its 30 cells */
uint32_t FUN_0001d48e(void) {
    ENTER_REGS(20);
    uint16_t room = 1;
    if (RU16(R32(gGameState) + 0x2180) >= 1) {
        do {
            CalcBlue((int16_t)room);
            uint32_t a2 = R32(gGameState) + (uint32_t)room * 8;
            W16(gRoomLinkA, R16(a2 + 0x2078));
            W16(gRoomLinkB, R16(a2 + 0x207a));
            for (uint16_t i = 0; i < 30; i++) InitState((int16_t)i);
            room++;
        } while (room <= RU16(R32(gGameState) + 0x2180));
    }
    return 0;
}

/* 0001d4ee: load shape list id into slot (once) */
uint32_t LoadSet(int16_t slot, int16_t id) {
    ENTER_LINK(0, 4);
    uint32_t a2 = gShapeLists + (int16_t)(slot * 4);
    if (R32(a2) != 0) return (uint32_t)((int32_t)slot * 4);
    uint32_t r = LoadShapeList(id, 0);
    W32(a2, r);
    return r;
}

/* 0001d528: load the kid's shapes and frames */
uint32_t LoadKidSet(void) {
    ENTER_LINK(4, 8);
    int16_t cur = (int16_t)_CurResFile();
    _UseResFile(R16(gMainRsrc1Ref));
    LoadSet(2, 0x61a9);
    uint32_t h = PoPGetResource(0x4652414d, 0x61a9);   /* 'FRAM' */
    W32(gKidFrames, h);
    _HNoPurge(h);
    _UseResFile(cur);
    return 0;
}

/* 0001d58c: kid palette for level types 1-6 */
uint32_t SetKidPalette(int16_t lvlType) {
    ENTER_LINK(0, 0);
    if (lvlType != 0 && (uint16_t)lvlType < 7) return PoPSetPalette((int16_t)(lvlType + 0x61a8), 0x10);
    return (uint16_t)lvlType;   /* upper word left over from the caller (ret override) */
}

/* 0001d5c8: load opponent set (shapes, palette, frames) unless already loaded */
uint32_t LoadOppSet(int16_t set) {
    ENTER_LINK(0, 4);
    int16_t d3 = set;
    if (!(d3 != -1 && d3 == R16(gOppSetLoaded) && R32(gOppShapes) != 0)) {
        UnloadSet(3);
        if (R32(gOppFrames) != 0) { _HPurge(R32(gOppFrames)); W32(gOppFrames, 0); }
        if (R32(gOppAttach) != 0) { _HPurge(R32(gOppAttach)); W32(gOppAttach, 0); }
        if (R32(gOppExtra) != 0) { _HPurge(R32(gOppExtra)); W32(gOppExtra, 0); }
        d3 = R16(gOppSetLoaded);
        if (d3 != -1 && set != d3) CloseRsrcFile(gOppRsrc + (int32_t)d3 * 6);
    }
    d3 = set;
    if (d3 != -1 && R32(GH(0x1039b8) + (int16_t)(d3 * 6)) != 0 &&
        !(d3 == R16(gOppSetLoaded) && R32(gOppShapes) != 0)) {
        OpenRsrcVerify(gOppRsrc + (int32_t)d3 * 6);
        LoadSet(3, 0x2ee);
        PoPSetPalette(0x2ee, 0x20);
        uint32_t h = PoPGetResource(0x4652414d, 0x2ee);   /* 'FRAM' */
        W32(gOppFrames, h);
        _HNoPurge(h);
        h = PoPGetResource(0x4146524d, 0x2ee);            /* 'AFRM' */
        W32(gOppAttach, h);
        _HNoPurge(h);
        d3 = set;
        if ((d3 == 5 || d3 == 6) && R32(gOppExtra) == 0) {
            h = PoPGetResource(0, 0x2f3);
            W32(gOppExtra, h);
            _HNoPurge(h);
        }
    }
    W16(gOppSetLoaded, set);
    return 0;   /* no caller uses D0 (ret override) */
}

/* 0001d756: on level 12, load opponent set 9 if room's cells hold a type-0xc0 piece 10 */
uint32_t PreloadRoomData(int16_t room) {
    ENTER_LINK(0x90, 20);
    if (LVL_NUM() == 0xc) {
        tc_memset(a6 - 0x90, 0, 0x80);
        uint32_t r = (uint32_t)(uint16_t)room;
        uint32_t off3c = r * 0x3c, off78 = r * 0x78;
        for (uint16_t i = 0; i < 30; i++) {
            int32_t ci = (int32_t)(int16_t)i;
            int16_t piece = R16(R32(gGameState) + ci * 2 + off3c - 0x3c);
            if (piece != 10) continue;
            int16_t spec = R16(R32(gGameState) + ci * 4 + off78 + 0x708);
            W16(a6 - 0x90 + (int16_t)((uint16_t)piece * 2), 1);
            if ((spec & 0xe0) == 0xc0) {
                if (R16(gOppSetLoaded) != 9) {
                    LoadOppSet(9);
                    break;
                }
            } else {
                W16(a6 - 0x90 + (int16_t)((uint16_t)piece * 2), 0);
            }
        }
    }
    return 0;
}

/* 0001d85c: sword type for this level */
uint32_t FUN_0001d85c(void) {
    if (R32(GH(0x102fe8)) != 0) {
        W16(gSwordType, R16(R32(GH(0x102fe8)) + 0x56));
    } else if (LVL_NUM() == 6) {
        W16(gSwordType, -1);
    } else {
        int16_t n = LVL_NUM();
        W16(gSwordType, (n == 7 || n == 8) ? 2 : 1);
    }
    return 0;
}

/* 0001d8ae: free the flame shapes */
uint32_t seg3_01d8ae(void) {
    ENTER_REGS(12);
    if (R32(gFlameShapes) != 0) {
        uint32_t a3 = R32(gFlameShapes);
        for (int16_t id = 0xfed; id <= 0x101a; id++, a3 += 4) {
            uint32_t h = R32(a3);
            if (h != 0) _HPurge(h);
        }
        _DisposePtr(R32(gFlameShapes));
        W32(gFlameShapes, 0);
    }
    return 0;
}

/* 0001d8e8: unload shape sets from..4 and level extras */
uint32_t UnloadFrom(int16_t from) {
    ENTER_LINK(0, 4);
    FUN_00017cfe();
    int16_t i = from;
    if (from < 5) {
        do UnloadSet(i++); while (i < 5);
    }
    if (from <= 4) {
        if (R32(gCustomRoomImgs) != 0) FUN_00018f66();
        if (LVL_TYPE() == 6) seg3_01d8ae();
    }
    return 0;
}

/* 0001d946: unload the level down to the given depth (0 = everything) */
uint32_t UnloadLevel(int16_t depth) {
    ENTER_LINK(4, 4);
    UnloadFrom(depth);
    if (depth <= 4) {
        LoadLevelSounds(0, LVL_TYPE());
        PurgeSounds();
    }
    if (depth <= 3 && R32(gOppFrames) != 0) {
        if (R32(gOppFrames) != 0) { _HPurge(R32(gOppFrames)); W32(gOppFrames, 0); }
        if (R32(gOppExtra) != 0) { _HPurge(R32(gOppExtra)); W32(gOppExtra, 0); }
        int16_t d3 = R16(gOppSetLoaded);
        if (d3 != -1) CloseRsrcFile(gOppRsrc + (int32_t)d3 * 6);
        W16(gOppSetLoaded, -1);
    }
    if (depth <= 2 && R32(gKidFrames) != 0) {
        _HPurge(R32(gKidFrames));
        W32(gKidFrames, 0);
    }
    if (depth <= 0 && R32(gKidAttach) != 0) {
        _HPurge(R32(gKidAttach));
        W32(gKidAttach, 0);
        if (R32(gOppAttach) != 0) { _HPurge(R32(gOppAttach)); W32(gOppAttach, 0); }
        W16(gSwordSetLoaded, 0);
    }
    FUN_0001da46();
    FUN_00018f66();
    return 0;
}

/* 0001da46: release the level's piece table */
uint32_t FUN_0001da46(void) {
    ENTER_REGS(4);
    if (R32(gLevelPieces) != 0) {
        uint32_t h = PoPGetResource(0x50494543, 0xdac);   /* 'PIEC' */
        if (h != 0) {
            _HUnlock(h);
            _HPurge(h);
        }
        W32(gLevelPieces, 0);
    }
    return 0;
}

/* 0001da78: unload shape list slot 0-4 */
uint32_t UnloadSet(int16_t slot) {
    ENTER_LINK(0, 12);
    if (slot >= 0 && slot < 5) {
        uint32_t a2 = gShapeLists + (int16_t)(slot * 4);
        uint32_t a3 = R32(a2);
        if (a3 != 0) {
            UnloadShapeList(a3);
            W32(a2, 0);
        }
    }
    return 0;
}
