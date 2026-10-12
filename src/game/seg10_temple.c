/* CODE 10 "Temple" (0x3a4a4-0x3d4dc): level-type 2 (temple) code.
 *  - piece draw procedures for the temple tile set (back walls, exits, gates, pressure
 *    plates, posts, torches, sacred flame), called through the piece-proc table;
 *  - the sliding rock (a mob of type 6): animation/drawing, its collision data for the
 *    barrier code, crushing the kid, triggering;
 *  - the trap door (a mob of type 10): triggering and animation/drawing;
 *  - the altar room of level 13 (shadow merge cut scene) and the sacred flame shapes.
 * Ported from the Ghidra decompilation/disassembly of the original 68k code; addresses in
 * comments are Ghidra addresses (see docs/PORTING.md). */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* ---- level ---- */
#define gGameState      GH(0x102fec)   /* long: pointer to the game state block */
#define LEVEL_NUM()     R16(R32(gGameState) + 0x218a)

/* ---- rooms ---- */
#define gCurRoom        GH(0x10303e)   /* word: current room */
#define gRoomLeft       GH(0x103040)   /* words: rooms around the current one */
#define gRoomRight      GH(0x103042)
#define gRoomAbove      GH(0x103044)
#define gRoomBelow      GH(0x103046)

/* ---- drawing: BF items (see seg03a notes) ---- */
#define gCurBFItem      GH(0x103074)   /* long: -> BF item being built (0x26 bytes) */
#define gLevelPieces    GH(0x103462)   /* long: -> 'PIEC' piece table (6-byte shape/x/y entries) */
#define gDrawClip       GH(0x103a00)   /* Rect: clip of the cell being drawn */
#define gDrawRow        GH(0x103a0a)   /* word: row being drawn (-1: row above the room) */
#define gFullClip       GH(0x1037ce)   /* word: drawing with the full-screen clip */
#define gCellPiece      GH(0x1037ec)   /* word: piece id of the cell being drawn */
#define gScreenRect     GH(0x103014)   /* Rect: the playfield */
#define gEmptyRect      GH(0x106694)   /* Rect: empty rect */
#define gRowTop         GH(0x1034b0)   /* word[]: top y of each row (gRowBottom[row-1]) */
#define gRowBottom      GH(0x1034b2)   /* word[]: bottom y of each row */
#define gColLeft        GH(0x103498)   /* word[]: left x of each column */
#define gCustomRoom     GH(0x103362)   /* long: -> locked 'CUST' data of the room (NULL: none) */
#define gClipRectCell   GH(0x103a84)   /* Rect: offset rect used by DrawTempTorch */
#define gEmptyRectProc  GH(0x1060a0)   /* long: -> EmptyRect-like Pascal function (rect): Boolean */
#define gUnionRectProc  GH(0x1060a4)   /* long: -> UnionRect-like Pascal procedure (a, b, dst) */
#define gSectRectProc   GH(0x1060a8)   /* long: -> SectRect-like Pascal function (a, b, dst): Boolean */

/* fixed shape entries {word shape, word x, word y} copied to a BF item's +0x18 */
#define gExitWallShape  GH(0x1054c2)   /* exit: 3 entries 0x1054c2/c8/ce */
#define gBackWall20     GH(0x1054d4)   /* back wall piece 0x14 (second half) */
#define gExitOpenShape  GH(0x1054da)
#define gPlateShapeA    GH(0x1054e0)   /* words: x/y of the raised plate pieces */
#define gBackWallDecor  GH(0x105532)   /* back wall decorations, 6 bytes each */
#define gPostShapesOdd  GH(0x105492)   /* word[16]: post shapes for odd columns */
#define gPostShapesEven GH(0x1054aa)   /* word[16]: post shapes for even columns */

/* ---- objects (filled by LoadObject, see seg03b notes) ---- */
#define gObjCell        GH(0x104b38)
#define gObjFace        GH(0x104b3a)
#define gObjX           GH(0x104b3c)
#define gObjY           GH(0x104b3e)
#define gObjShape       GH(0x104b40)
#define gObjKind        GH(0x104b42)
#define gObjClip        GH(0x104b44)   /* Rect */
#define gObjPiece       GH(0x104b4c)
#define gObjTable       GH(0x10334e)   /* long: -> object table (0x1c-byte entries) */
#define gNumObjs        GH(0x1037da)   /* word */

/* ---- characters (0x3e-byte records) ---- */
#define CHAR_SIZE       0x3e
#define gCharTab0       GH(0x102e00)   /* gCharTab (0x102df8) + 8: type word of entry 0 */
#define gKid            GH(0x102f2e)   /* record: the kid (saved copy) */
#define gKidFace        GH(0x102f30)
#define gKidX           GH(0x102f32)
#define gKidFrame       GH(0x102f38)
#define gKidCol         GH(0x102f3a)
#define gKidRow         GH(0x102f3c)
#define gKidRoom        GH(0x102f44)
#define gKidSeq         GH(0x102f56)
#define gKidField34     GH(0x102f62)
#define gChar           GH(0x102faa)   /* record: the character being processed */
#define gCharFace       GH(0x102fac)   /* 0 right, -1 left */
#define gCharX          GH(0x102fae)
#define gCharY          GH(0x102fb0)
#define gCharType       GH(0x102fb2)
#define gCharFrame      GH(0x102fb4)
#define gCharCol        GH(0x102fb6)
#define gCharRow        GH(0x102fb8)
#define gCharYVel       GH(0x102fbe)
#define gCharRoom       GH(0x102fc0)
#define gCharOnRock     GH(0x102fc4)   /* 1: riding the sliding rock */
#define gCharAlive      GH(0x102fc6)   /* < 0 alive */
#define gCharLife       GH(0x102fc8)
#define gCharMaxLife    GH(0x102fca)
#define gCharSeqLast    GH(0x102fd2)
#define gCharRect       GH(0x102fd4)   /* Rect */
#define gCharField32    GH(0x102fdc)
#define gCharField34    GH(0x102fde)
#define gCharField36    GH(0x102fe0)
#define gCharImgW       GH(0x104a90)   /* word: width of the character image */
#define gCharRightEdge  GH(0x104a9a)
#define gCharLeftEdge   GH(0x104a9c)
#define gColDetBarr     GH(0x104aa2)   /* word[10]: barrier codes of the previous frame's row */
#define gStepDir        GH(0x10346c)   /* word[2] by face */
#define gColRight       GH(0x10347a)   /* word[]: column x used for wall collisions */
#define gColLeftLimit   GH(0x103494)

/* ---- the current piece (set by ReadCell) ---- */
#define gPieceSpec      GH(0x104b52)
#define gPieceCell      GH(0x104b54)
#define gPieceRoom      GH(0x104b56)
#define gPieceCol       GH(0x104b58)
#define gPieceRow       GH(0x104b5a)
#define gRoomTiles      GH(0x1037e0)   /* long: -> piece words of the room selected by CalcBlue */
#define gRoomSpecs      GH(0x1037dc)   /* long: -> spec longs of that room */
#define gTrobState      GH(0x1037e8)   /* word: state of the trob being animated */
#define gTrobDir        GH(0x103c16)   /* word: -1 = trob finished */

/* ---- mobs: the mob being processed (16 bytes) ---- */
#define gMobX           GH(0x103b5a)
#define gMobY           GH(0x103b5c)
#define gMobRoom        GH(0x103b5e)
#define gMobSpeed       GH(0x103b60)   /* -1 = finished */
#define gMobAccel       GH(0x103b62)   /* sliding rock: 0 idle, -2 triggered, 1..8 speed, 0x64 stopped */
#define gMobType        GH(0x103b64)
#define gMobRow         GH(0x103b66)
#define gMobState       GH(0x103b68)   /* sliding rock: x where it stops; trap door: frame */
#define gMobDrawW       GH(0x103af4)   /* words: mob drawing parameters by type */
#define gMobDrawW2      GH(0x103af6)
#define gMobDrawH       GH(0x103afc)
#define gMobDrawX10     GH(0x103b10)
#define gMobDrawW18     GH(0x103b18)
#define gRockPartsX     GH(0x105790)   /* {y, x} offsets of the three rock pieces */
#define gTrapFrames     GH(0x10579a)   /* word[]: trap door frame -> shape entry index */
#define gTrapShapes     GH(0x1057be)   /* 6-byte {shape, dx, dy} entries */

/* ---- sound / misc ---- */
#define gSoundPlaying   GH(0x103c62)
#define gLevelTrigger   GH(0x10302c)
#define gMsgId          GH(0x103034)
#define gMsgTimer       GH(0x103036)
#define gFlameShapes    GH(0x105480)   /* long: -> handles of the flame images 0xfed..0x101a */
#define gFlameTable     GH(0x105484)   /* word[]: flame shapes / custom pieces by frame */
#define gFlamePalette   GH(0x105490)   /* word */
#define gSwordInfo      GH(0x102de8)   /* words: sword frame, dx, dy */

#define ITEM()          R32(gCurBFItem)
#define PIECES()        R32(gLevelPieces)

/* ---- helpers ---- */

/* Call a Pascal routine through a pointer (args pushed left to right, 4 bytes each,
 * optional Boolean result slot above them; the callee pops the arguments). */
static uint8_t pascal_ptr3(maddr fn, int has_result, uint32_t a, uint32_t b, uint32_t c) {
    maddr sp0 = emu_sp();
    maddr sp = sp0 - (has_result ? 2 : 0) - 12;
    W32(sp + 8, a); W32(sp + 4, b); W32(sp, c);
    emu_set_sp(sp);
    call68_addr(fn);
    uint8_t r = has_result ? R8(sp + 12) : 0;
    emu_set_sp(sp0);
    return r;
}
static uint8_t pascal_ptr1(maddr fn, int has_result, uint32_t a) {
    maddr sp0 = emu_sp();
    maddr sp = sp0 - (has_result ? 2 : 0) - 4;
    W32(sp, a);
    emu_set_sp(sp);
    call68_addr(fn);
    uint8_t r = has_result ? R8(sp + 4) : 0;
    emu_set_sp(sp0);
    return r;
}
#define SECTRECT(a, b, dst)   pascal_ptr3(R32(gSectRectProc), 1, (a), (b), (dst))
#define UNIONRECT(a, b, dst)  pascal_ptr3(R32(gUnionRectProc), 0, (a), (b), (dst))
#define EMPTYRECT(r)          pascal_ptr1(R32(gEmptyRectProc), 1, (r))

/* move.l (A0)+,(A1)+; move.w (A0),(A1) */
static void copy6(maddr dst, maddr src) { W32(dst, R32(src)); W16(dst + 4, RU16(src + 4)); }
static void copy8(maddr dst, maddr src) { W32(dst, R32(src)); W32(dst + 4, R32(src + 4)); }
/* gKid = gChar */
static void save_kid(void) { mcopy(gKid, gChar, CHAR_SIZE); }
/* the piece-table entry at offset off into the item's shape/x/y */
static void item_shape(uint32_t off) { copy6(ITEM() + 0x18, PIECES() + off); }
static void item_xy(uint32_t xoff) {
    W16(ITEM() + 0x1a, RU16(PIECES() + xoff));
    W16(ITEM() + 0x1c, RU16(PIECES() + xoff + 2));
}
#define ADD_ITEM(reset) do { if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(reset); } while (0)

/* Some originals write into their own argument slots (in the caller's frame). After
 * ENTRY_LINK, args_ is the address of the first argument: the real slots when entered
 * from 68k code, the reserved (dead) area when called from C. */
#define ARG16(off)        (args_ + (off))

/* 0003a4a4: back wall of a temple cell: piece 0x16 & 0x1f is the wall shape; skipped when
 * the left cell's back-table image already covers the clip; adds the decorations. */
uint32_t DrawTempBackWall(void) {
    ENTRY_LINK(FN_DrawTempBackWall, 0x10, 28);   /* link -0x10; movem D3-D7/A2-A3 */
    uint16_t info = RU16(ITEM() + 0x16);
    uint16_t hi = (uint16_t)((info & 0xf00) >> 8);    /* -6(A6) */
    uint16_t shape = info & 0x1f;                     /* D5 */
    if (shape == 0) return 0x1f;
    int16_t pair;                                     /* -8(A6) */
    if (shape == 0x14) { shape = 0x13; pair = 1; } else pair = 0;
    uint32_t a3;
    int16_t d7;
    if (R16(gFullClip) != 0) {
        d7 = 0; a3 = 0;
    } else {
        uint32_t it = ITEM();
        a3 = GetBackTableImgRow((int16_t)shape, (int16_t)(R16(it + 0xe) - 1), R16(it + 0x10));
        if (a3 == 0 || SECTRECT(a3 + 0x1e, gDrawClip, a6 - 0x10) == 0) {
            a3 = 0; d7 = 1;
        } else {
            d7 = 0;
            UNIONRECT(gDrawClip, a3 + 0x1e, a3 + 0x1e);
            if (pair != 0) {
                it = ITEM();
                a3 = GetBackTableImgRow((int16_t)(shape + 1), (int16_t)(R16(it + 0xe) - 1), R16(it + 0x10));
                if (a3 != 0 && SECTRECT(a3 + 0x1e, gDrawClip, a6 - 0x10) != 0)
                    UNIONRECT(gDrawClip, a3 + 0x1e, a3 + 0x1e);
            }
        }
    }
    if (a3 != 0) return a3;
    if (d7 == 0 && hi != 0) {
        uint32_t it = ITEM();
        if (!(R16(it + 0xe) == -1 && hi <= 3)) {
            if (R16(it + 0x10) != 3) return RU16(it + 0x10);
        }
    }
    d7 = 1;
    int16_t d6 = R16(gDrawClip);
    uint32_t it = ITEM();
    int16_t row = R16(it + 0x10);
    if (row >= 0 && R16(it + 4) >= 0 && S16(VisibleAbovePiece(R16(it + 0xe), row)) == 0) {
        int16_t top = R16(gDrawClip);
        int16_t d3 = R16(gRowTop + (int16_t)(R16(ITEM() + 0x10) * 2));
        if (top < d3) {
            W16(gDrawClip, d6 > d3 ? d6 : d3);
            if (EMPTYRECT(gDrawClip) != 0) d7 = 0;
        }
    }
    uint32_t d0 = 0;
    if (d7 != 0) {
        it = ITEM();
        int16_t saved12 = R16(it + 0x12);
        W16(it + 0x12, 0);
        W16(ITEM() + 6, 0);
        W16(ITEM() + 0x18, shape);
        hi = (uint16_t)(hi + 0x24);
        uint32_t off = (uint32_t)hi * 0x14;
        W16(ITEM() + 0x1a, RU16(PIECES() + off + 4));
        W16(ITEM() + 0x1c, RU16(PIECES() + off + 6));
        ADD_ITEM(0);
        if (pair != 0) {
            copy6(ITEM() + 0x18, gBackWall20);
            ADD_ITEM(0);
        } else {
            uint32_t a2 = ITEM();
            uint16_t deco = RU16(a2 + 0x14) & 0x3000;
            if (deco != 0) {
                deco = (uint16_t)(deco >> 12);
                copy6(a2 + 0x18, gBackWallDecor + (uint32_t)deco * 6);
                ADD_ITEM(0);
            }
        }
        W16(ITEM() + 0x12, saved12);
        W16(ITEM() + 6, 0x10);
        d0 = 0x10;
    }
    W16(gDrawClip, d6);
    return d0;                                   /* "ret": 0: D0 is not used by any caller */
}

/* 0003a766: exit door frame pieces (back: kinds 5/2, floor: kind 1) */
uint32_t FUN_0003a766(void) {
    ENTRY_NOLINK(FN_FUN_0003a766, 20);           /* movem A2/D3-D6 */
    uint32_t a2 = ITEM();
    uint16_t kind = RU16(a2 + 0xa);
    if (kind == 5 || kind == 2) {
        a2 = ITEM();
        int16_t d4 = (int16_t)((R16(a2 + 0x14) & 3) + 0x16);
        if (d4 != 0x16 && d4 <= 0x17) return 0;
        int16_t d3 = S16(PieceRightId(R16(a2 + 0xe), R16(a2 + 0x10)));
        if (S16(IsWall(d3)) != 0) return 0;
        a2 = ITEM();
        d3 = R16(a2 + 0xa);
        W16(a2 + 0x18, 0x15);
        item_xy(0x19a);
        ADD_ITEM(d3);
        a2 = ITEM();
        if ((R16(a2 + 0x14) & 4) == 0) return 0;
        W16(a2 + 0x18, 0x1b);
        item_xy(0x19a);
        AddNextBFItemToTable();
        return 0;
    }
    if (kind != 1) return 0;
    int16_t d4 = (int16_t)((R16(a2 + 0x14) & 3) + 0x16);
    if (d4 > 0x17) {
        if (R16(gFullClip) == 0) {
            W16(a2 + 0x1a, -0x33);
            d4 = (int16_t)(d4 - 2);
        } else {
            d4 = -1;
        }
    } else {
        W16(ITEM() + 0x1a, 0);
    }
    if (d4 != -1) {
        W16(ITEM() + 0x18, d4);
        W16(ITEM() + 0x1c, RU16(PIECES() + 0x1a2));
        ADD_ITEM(1);
    }
    a2 = ITEM();
    d4 = (int16_t)((R16(a2 + 0x14) & 3) + 0x16);
    int16_t d3 = R16(a2 + 0x10);
    if (d3 < 0) d3 = (int16_t)(d3 + 3);
    int16_t d5 = S16(PieceLeftId(R16(ITEM() + 0xe), d3));
    if (d4 <= 0x17 && S16(IsSpace(d5)) == 0 && d5 != 0x1a && d5 != 0xb) {
        W16(ITEM() + 0x18, 0x18);
        item_xy(0x1a0);
        if (S16(AddNextBFItemToTable()) != 0) {
            ResetNextBFItemWithPrevInfo(1);
            W16(ITEM() + 6, 0x10);
        }
    }
    d4 = S16(PieceBelowId(R16(ITEM() + 0xe), d3));
    if (S16(IsWall(d4)) == 0) return 0;
    W16(ITEM() + 6, 0);
    d4 = (S16(IsWall(d5)) != 0) ? 0x1a : 0x19;
    W16(ITEM() + 0x18, d4);
    item_xy(0x1a0);
    if (S16(AddNextBFItemToTable()) == 0) W16(ITEM() + 6, 0x10);
    return 0;                                    /* "ret": 0 */
}

/* 0003a9ac: piece proc: exit (left part) */
uint32_t DrawTempExit1(void) {
    ENTRY_LINK(FN_DrawTempExit1, 4, 0);          /* link -4 */
    uint16_t kind = RU16(ITEM() + 0xa);
    if (kind == 0) {
        if (R16(gFullClip) != 0) DrawTempBackWall();
    } else if (kind == 5 || kind == 2 || kind == 1) {
        uint32_t it = ITEM();
        W16(it + 0x14, RU16(it + 0x14) & 0xfffc);
        FUN_0003ac12();
    }
    return 0;                                    /* "ret": 0 */
}

/* 0003aa02: piece proc: exit (right part, with the door) */
uint32_t FUN_0003aa02(void) {
    ENTRY_NOLINK(FN_FUN_0003aa02, 12);           /* movem A2/D3/D4 */
    uint32_t a2 = ITEM();
    int16_t d3 = R16(a2 + 0x14);
    int16_t d4 = R16(a2 + 0xa);
    uint16_t kind = (uint16_t)d4;
    if (kind == 0) {
        if (R16(gFullClip) != 0) DrawTempBackWall();
        item_shape(0x156);
        ADD_ITEM(0);
        int16_t c = S16(FirstInRow(R16(ITEM() + 0x10)));
        c = (int16_t)(c + R16(ITEM() + 0xe));
        a2 = GetTrobRoomCell(R16(gCurRoom), c);
        if (a2 != 0 && R16(a2 + 4) >= 4) {
            copy6(ITEM() + 0x18, gExitOpenShape);
            ADD_ITEM(0);
        }
        W16(ITEM() + 0x14, d3);
        DrawTempExitMovable();
    } else if (kind == 5 || kind == 2 || kind == 1) {
        uint32_t it = ITEM();
        W16(it + 0x14, RU16(it + 0x14) & 0xfffc);
        it = ITEM();
        W16(it + 0x14, RU16(it + 0x14) | 1);
        FUN_0003ac12();
        if (d4 == 5) {
            if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(5);
            uint16_t f = RU16(gKidFrame);
            if (f >= 0xd9 && f <= 0xe2) {
                a2 = ITEM();
                ResetNextBFItem(1);
                TransferBFItemMidFields(a2, ITEM());
            }
            item_shape(0x47c);
            AddNextBFItemToTable();
        }
    }
    return 0;
}

/* 0003ab3a: the exit door, raised by the item's info & 0xff pixels (clipped to its row) */
uint32_t DrawTempExitMovable(void) {
    ENTRY_LINK(FN_DrawTempExitMovable, 0x10, 12);   /* link -0x10; movem D3/D4/A2 */
    uint32_t a2 = ITEM();
    if (R16(a2 + 0xa) != 0) return 0;
    copy8(a6 - 8, gDrawClip);
    copy8(a6 - 0x10, gScreenRect);
    W16(a6 - 0x10, (int16_t)(R16(a2 + 0x10) * 0x78 - 10));
    if (SECTRECT(a6 - 0x10, gDrawClip, gDrawClip) != 0) {
        a2 = ITEM();
        uint16_t d3 = RU16(a2 + 0x14) & 0xff;
        uint16_t d4 = d3 < 0x4d ? d3 : 0x4d;
        W16(a2 + 0x18, RU16(PIECES() + 0x476));
        W16(ITEM() + 0x1a, RU16(PIECES() + 0x478));
        W16(ITEM() + 0x1c, (int16_t)(-0x22 - (int16_t)(d4 + 1)));
        ADD_ITEM(0);
    }
    copy8(gDrawClip, a6 - 8);
    return 0;
}

/* 0003ac12: generic floor piece: back wall (kind 0), floor front (5/2, + FUN_0003ace8),
 * floor (1) */
uint32_t FUN_0003ac12(void) {
    ENTRY_NOLINK(FN_FUN_0003ac12, 8);            /* movem D3/A2 */
    uint32_t a2 = ITEM();
    uint16_t kind = RU16(a2 + 0xa);
    if (kind == 0) {
        DrawTempBackWall();
    } else if (kind == 5 || kind == 2) {
        a2 = ITEM();
        int16_t d3 = R16(a2 + 0xa);
        W16(a2 + 0x18, 0x1c);
        item_xy(0x1e);
        ADD_ITEM(d3);
        if ((R16(ITEM() + 0x14) & 2) != 0) FUN_0003ace8();
    } else if (kind == 1) {
        W16(a2 + 6, 0);
        a2 = ITEM();
        int16_t d3 = R16(a2 + 0x14) & 1;
        W16(a2 + 0x18, d3 + 0x1d);
        item_xy(0x24);
        if (S16(AddNextBFItemToTable()) == 0) W16(ITEM() + 6, 0x10);
    }
    return 0;                                    /* "ret": 0 */
}

/* 0003ace8: wall edge piece in front of the floor (custom rooms of template 0x1f shift it) */
uint32_t FUN_0003ace8(void) {
    ENTRY_NOLINK(FN_FUN_0003ace8, 12);           /* movem D3/D4/A2 */
    uint32_t a2 = ITEM();
    int16_t d3 = R16(a2 + 0xa);
    if (d3 != 5 && d3 != 2) return 0;
    int16_t d4 = d3;
    if (R32(gCustomRoom) != 0 && R16(R32(gCustomRoom)) == 0x1f) {
        copy6(a2 + 0x18, gExitWallShape + 0xc);
        a2 = ITEM();
        if (R16(a2 + 0xe) != 1) W16(a2 + 0x1a, R16(a2 + 0x1a) + 3);
    } else {
        copy6(ITEM() + 0x18, gExitWallShape + 6);
    }
    ADD_ITEM(d4);
    a2 = ITEM();
    if ((R16(a2 + 0x14) & 1) != 0) {
        copy6(a2 + 0x18, gExitWallShape);
        ADD_ITEM(d4);
    }
    return 0;                                    /* "ret": 0 */
}

/* 0003ad86: piece proc: gate */
uint32_t seg10_03ad86(void) {
    ENTRY_NOLINK(FN_seg10_03ad86, 8);            /* movem D3/A2 */
    uint32_t a2 = ITEM();
    uint16_t kind = RU16(a2 + 0xa);
    if (kind == 0) {
        DrawTempBackWall();
    } else if (kind == 5 || kind == 2) {
        a2 = ITEM();
        int16_t d3 = R16(a2 + 0xa);
        copy6(a2 + 0x18, PIECES() + 0x58);
        ADD_ITEM(d3);
        DrawTempGateMovable();
    } else if (kind == 1) {
        copy6(a2 + 0x18, PIECES() + 0x5e);
        ADD_ITEM(1);
        uint32_t it = ITEM();
        W16(it + 0x14, RU16(it + 0x14) & 1);
        FUN_0003ac12();
    }
    return 0;
}

/* 0003ae16: the gate bars, lowered by (info & 0xff) / 2 pixels, clipped to the row */
uint32_t DrawTempGateMovable(void) {
    ENTRY_LINK(FN_DrawTempGateMovable, 0x10, 20);   /* link -0x10; movem D3-D6/A2 */
    uint32_t a2 = ITEM();
    int16_t d3 = R16(a2 + 0xa);
    if (d3 != 5 && d3 != 2) return 0;
    copy8(a6 - 8, gDrawClip);
    copy8(a6 - 0x10, gScreenRect);
    W16(a6 - 0x10, (int16_t)(R16(a2 + 0x10) * 0x78 - 0x13));
    if (SECTRECT(a6 - 0x10, gDrawClip, gDrawClip) != 0) {
        a2 = ITEM();
        int32_t d = (int32_t)(RU16(a2 + 0x14) & 0xff) >> 1;
        if (d >= 0x5c) d = 0x5c;
        int16_t d5 = (int16_t)d;
        W16(a2 + 0x18, RU16(PIECES() + 0x454));
        W16(ITEM() + 0x1a, RU16(PIECES() + 0x456));
        W16(ITEM() + 0x1c, (int16_t)(-10 - d5));
        int16_t d6 = R16(ITEM() + 0xa);
        ADD_ITEM(d6);
    }
    copy8(gDrawClip, a6 - 8);
    return 0;                                    /* "ret": 0 */
}

/* 0003aefe: piece proc: column/post pieces (shape from the info's low nibble) */
uint32_t FUN_0003aefe(void) {
    ENTRY_NOLINK(FN_FUN_0003aefe, 12);           /* movem D4/D5/A2 */
    int16_t d5 = 0;
    uint32_t a2 = ITEM();
    int16_t d4 = R16(a2 + 0x14) & 0xf;
    if (d4 != 0xd) {
        if ((R16(a2 + 0xe) & 1) != 0) d4 = R16(gPostShapesOdd + (int16_t)(d4 * 2));
        else d4 = R16(gPostShapesEven + (int16_t)(d4 * 2));
    }
    a2 = ITEM();
    uint16_t kind = RU16(a2 + 0xa);
    if (kind == 0) {
        DrawTempBackWall();
    } else if (kind == 5 || kind == 2) {
        if (d4 != 0xd) {
            if (d4 == 0) d5 = 1;
            else {
                W16(ITEM() + 0x18, d4 + 0x2c);
                item_xy(0xe6);
                AddNextBFItemToTable();
            }
        }
    } else if (kind == 1) {
        if (d4 != 0xd) {
            if (d4 == 0) d5 = 1;
            else {
                W16(a2 + 0x18, d4 + 0x2e);
                item_xy(0xec);
                AddNextBFItemToTable();
            }
        }
    }
    if (d5 != 0) {
        uint32_t it = ITEM();
        W16(it + 0x14, RU16(it + 0x14) & 0xfff0);
        FUN_0003ac12();
    }
    return 0;                                    /* "ret": 0 */
}

/* 0003b006: piece proc */
uint32_t seg10_03b006(void) {
    ENTRY_NOLINK(FN_seg10_03b006, 4);            /* A2 */
    uint32_t a2 = ITEM();
    uint16_t kind = RU16(a2 + 0xa);
    if (kind == 0) {
        DrawTempBackWall();
    } else if (kind == 5) {
        item_shape(0xa8);
        AddNextBFItemToTable();
    } else if (kind == 1) {
        copy6(a2 + 0x18, PIECES() + 0xae);
        ADD_ITEM(1);
        FUN_0003ac12();
    }
    return 0;
}

/* 0003b06e: piece proc (info & 3 selects a variant) */
uint32_t seg10_03b06e(void) {
    ENTRY_NOLINK(FN_seg10_03b06e, 8);            /* movem D3/A2 */
    uint32_t a2 = ITEM();
    int16_t d3 = R16(a2 + 0x14) & 3;
    uint16_t kind = RU16(a2 + 0xa);
    if (kind == 0) {
        DrawTempBackWall();
    } else if (kind == 5) {
        if (d3 == 0) {
            item_shape(0xbc);
            AddNextBFItemToTable();
        }
    } else if (kind == 1) {
        if (d3 == 0) {
            copy6(a2 + 0x18, PIECES() + 0xc2);
        } else {
            d3 = (int16_t)(d3 + 0x23);
            W16(ITEM() + 0x18, d3);
            if (d3 == 0x25) {
                W16(ITEM() + 0x1a, RU16(gPlateShapeA + 6));
                W16(ITEM() + 0x1c, RU16(gPlateShapeA + 4));
            } else {
                W16(ITEM() + 0x1a, RU16(gPlateShapeA + 2));
                W16(ITEM() + 0x1c, RU16(gPlateShapeA));
            }
        }
        AddNextBFItemToTable();
    }
    return 0;
}

/* shared by DrawTempDownPlate/DrawTempUpPlate: x/y of the plate shape from piece entry
 * 5 or 6 (info bit 0x2000) at +xoff */
static void plate_xy(uint32_t a2, uint32_t xoff) {
    int16_t d4 = (int16_t)(((RU16(a2 + 0x14) & 0x2000) ? 1 : 0) + 5);
    uint32_t off = (uint32_t)(uint16_t)d4 * 0x14;
    W16(a2 + 0x1a, RU16(PIECES() + off + xoff));
    W16(ITEM() + 0x1c, RU16(PIECES() + off + xoff + 2));
}

/* 0003b116: piece proc: pressure plate (down) */
uint32_t DrawTempDownPlate(void) {
    ENTRY_LINK(FN_DrawTempDownPlate, 4, 12);     /* link -4; movem D3/D4/A2 */
    uint32_t a2 = ITEM();
    uint16_t kind = RU16(a2 + 0xa);
    if (kind == 0) {
        uint32_t it = ITEM();
        W16(it + 0x14, RU16(it + 0x14) & 0xdfff);
        DrawTempBackWall();
    } else if (kind == 5 || kind == 2) {
        W16(ITEM() + 0x18, 0x2a);
        plate_xy(ITEM(), 0xa);
        AddNextBFItemToTable();
    } else if (kind == 1) {
        W16(a2 + 0x18, 0x2c);
        W16(ITEM() + 6, 0);
        plate_xy(ITEM(), 0x10);
        if (S16(AddNextBFItemToTable()) == 0) W16(ITEM() + 6, 0x10);
    }
    return 0;
}

/* 0003b248: piece proc: pressure plate (up) */
uint32_t DrawTempUpPlate(void) {
    ENTRY_LINK(FN_DrawTempUpPlate, 4, 12);       /* link -4; movem D3/D4/A2 */
    uint32_t a2 = ITEM();
    uint16_t kind = RU16(a2 + 0xa);
    if (kind == 0) {
        uint32_t it = ITEM();
        W16(it + 0x14, RU16(it + 0x14) & 0xdfff);
        DrawTempBackWall();
    } else if (kind == 5 || kind == 2) {
        W16(ITEM() + 0x18, 0x29);
        plate_xy(ITEM(), 0xa);
        AddNextBFItemToTable();
    } else if (kind == 1) {
        W16(a2 + 6, 0);
        W16(ITEM() + 0x18, 0x2b);
        plate_xy(ITEM(), 0x10);
        if (S16(AddNextBFItemToTable()) == 0) W16(ITEM() + 6, 0x10);
    }
    return 0;
}

/* 0003b378: piece proc: posts */
uint32_t DrawTempPosts(void) {
    ENTRY_LINK(FN_DrawTempPosts, 4, 4);          /* link -4; A2 */
    uint32_t a2 = ITEM();
    uint16_t kind = RU16(a2 + 0xa);
    if (kind == 0) {
        DrawTempBackWall();
    } else if (kind == 5 || kind == 2) {
        item_shape(0x44);
        AddNextBFItemToTable();
    } else if (kind == 1) {
        copy6(a2 + 0x18, PIECES() + 0x4a);
        ADD_ITEM(1);
        FUN_0003ac12();
    }
    return 0;
}

/* 0003b3fe: piece proc: wall edge + seg10_03b452's piece */
uint32_t FUN_0003b3fe(void) {
    ENTRY_NOLINK(FN_FUN_0003b3fe, 8);            /* movem D3/A2 */
    uint16_t kind = RU16(ITEM() + 0xa);
    if (kind == 0) {
        DrawTempBackWall();
    } else if (kind == 5 || kind == 2) {
        uint32_t a2 = ITEM();
        if ((R16(a2 + 0x14) & 2) != 0) {
            int16_t d3 = R16(a2 + 0xa);
            FUN_0003ace8();
            if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(d3);
        }
    }
    seg10_03b452();
    return 0;
}

/* 0003b452: overlay piece (front 0x120 / floor 0x126); returns AddNextBFItemToTable's
 * result (FUN_000299d8 returns it) */
uint32_t seg10_03b452(void) {
    ENTRY_NOLINK(FN_seg10_03b452, 4);            /* A2 */
    uint32_t a2 = ITEM();
    uint16_t kind = RU16(a2 + 0xa);
    if (kind == 5 || kind == 2) {
        item_shape(0x120);
        return AddNextBFItemToTable();
    }
    if (kind == 1) {
        copy6(a2 + 0x18, PIECES() + 0x126);
        return AddNextBFItemToTable();
    }
    return kind;                                 /* D0 = zero-extended kind */
}

/* 0003b4a6: back wall when the cell is in the drawn row (or visible from above) */
uint32_t seg10_03b4a6(void) {
    ENTRY_NOLINK(FN_seg10_03b4a6, 12);           /* movem D3/D4/A2 */
    uint32_t a2 = ITEM();
    if (R16(a2 + 0xa) != 0) return 0;
    int16_t d4 = 0;
    if (R16(a2 + 0x10) == R16(gDrawRow)) {
        d4 = 1;
    } else {
        int32_t r = (int32_t)R16(gDrawRow) + 1;
        a2 = ITEM();
        int16_t d3 = R16(a2 + 0x10);
        if (r == (int32_t)d3) d4 = S16(VisibleAbovePiece(R16(a2 + 0xe), d3));
    }
    if (d4 == 0 && R16(gDrawRow) == -1 && S16(IsSpace(R16(gCellPiece))) != 0) d4 = 1;
    if (d4 != 0) DrawTempBackWall();
    return 0;
}

/* 0003b514: piece proc: exit frame */
uint32_t seg10_03b514(void) {
    ENTRY_NOLINK(FN_seg10_03b514, 4);            /* A2 */
    uint32_t a2 = ITEM();
    uint16_t kind = RU16(a2 + 0xa);
    if (kind == 5) {
        a2 = ITEM();
        if (R16(a2 + 0xe) == 9) return 0;
        W16(a2 + 0x14, RU16(a2 + 0x14) & 0xfffc);
        FUN_0003a766();
        if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(5);
        item_shape(0x30);
        AddNextBFItemToTable();
    } else if (kind == 1) {
        W16(a2 + 0x14, RU16(a2 + 0x14) & 0xfffc);
        FUN_0003a766();
    }
    return 0;
}

/* 0003b580: piece proc: torch */
uint32_t seg10_03b580(void) {
    ENTRY_NOLINK(FN_seg10_03b580, 0);
    DrawTempTorch();
    seg10_03b452();
    return 0;
}

/* 0003b58a: torch: back wall (if the cell is not inside the clip), flame frame (info & 0xf) */
uint32_t DrawTempTorch(void) {
    ENTRY_LINK(FN_DrawTempTorch, 8, 12);         /* link -8; movem D3/D4/A2 */
    int16_t d4 = R16(ITEM() + 0xa);
    uint16_t kind = (uint16_t)d4;
    if (kind == 0) {
        uint32_t a2 = ITEM();
        ComputeOffsetCellRect(a6 - 8, gClipRectCell, R16(a2 + 0xe), R16(a2 + 0x10));
        if (S16(EncloseRect(a6 - 8, gDrawClip)) == 0) {
            W16(ITEM() + 6, 0);
            W16(ITEM() + 0x12, 0);
            item_shape(0x17e);
            ADD_ITEM(0);
            W16(ITEM() + 6, 0x10);
            W16(ITEM() + 0x12, 0x13);
        }
        a2 = ITEM();
        int16_t d3 = R16(a2 + 0x14) & 0xf;
        W16(a2 + 0x18, d3 + 0x60);
        item_xy(0x186);
        AddNextBFItemToTable();
    } else if (kind == 2 || kind == 5) {
        uint32_t a2 = ITEM();
        int16_t d3 = R16(a2 + 0x14);
        W16(a2 + 0x14, RU16(a2 + 0x14) & 0xfffc);
        FUN_0003ac12();
        if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(d4);
        W16(ITEM() + 0x14, d3);
        item_shape(0x18a);
        AddNextBFItemToTable();
    } else if (kind == 1) {
        FUN_0003ac12();
    }
    if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(d4);
    return 0;                                    /* "ret": 0 */
}

/* 0003b6e0: piece proc: floor with a back wall shape in info & 0x1f */
uint32_t FUN_0003b6e0(void) {
    ENTRY_NOLINK(FN_FUN_0003b6e0, 8);            /* movem D3/A2 */
    uint32_t a2 = ITEM();
    int16_t d3 = R16(a2 + 0x14) & 0x1f;
    W16(a2 + 0x14, RU16(a2 + 0x14) & 0xffe0);
    uint16_t kind = RU16(ITEM() + 0xa);
    if (kind == 0) {
        if (d3 == 0) FUN_0003ac12();
        else DrawTempBackWall();
    } else if (kind == 5 || kind == 2 || kind == 1) {
        if (d3 == 0) FUN_0003ac12();
    }
    return 0;
}

/* 0003b736: temple overlay init (LoadCodeOverlays): spike shape tables */
uint32_t seg10_03b736(void) {
    W32(GH(0x103c42), GH(0x1054e8));
    W32(GH(0x103c3e), GH(0x1054f8));
    W16(GH(0x103c3c), 0x68);
    W32(GH(0x103c38), GH(0x105508));
    return 0x68;
}

/* 0003b756: next torch frame: random 0..5, different from the previous one */
uint32_t TempGetTorchFrame(int16_t prev) {
    ENTRY_LINK(FN_TempGetTorchFrame, 0, 8);      /* link 0; movem D3/D4 */
    uint32_t r = Rnd(5);
    int16_t d4 = (int16_t)r;
    if (d4 == prev) d4 = (d4 == 5) ? 0 : (int16_t)(d4 + 1);
    return (r & 0xffff0000u) | (uint16_t)d4;
}

/* 0003b79a: is the torch at (room, cell) visible? A torch in the bottom row of the room
 * above is only visible when the cell below it (and its right neighbour) is not empty.
 * The original rewrites its argument slots (room = current room, cell += 20). */
uint32_t TempVisibleTorch(int16_t room, int16_t cell) {
    ENTRY_LINK(FN_TempVisibleTorch, 0, 20);      /* link 0; movem D3-D7 */
    int16_t d6 = S16(IsVisibleRoomCell(room, cell));
    if (d6 == 0) return 0;
    if (room != R16(gRoomBelow)) return (uint16_t)d6;
    if (cell >= 10) return (uint16_t)d6;
    cell = (int16_t)(cell + 0x14);
    W16(ARG16(2), cell);
    int16_t d3 = R16(gCurRoom);
    W16(ARG16(0), d3);
    uint32_t d5 = (uint32_t)((int32_t)cell * 2);
    uint32_t base = (uint32_t)RU16(gCurRoom) * 0x3c + d5 + R32(gGameState);
    int16_t d7 = R16(base - 0x3c);
    if (S16(IsSpace(d7)) != 0) return (uint16_t)d6;
    d7 = R16((uint32_t)(uint16_t)d3 * 0x3c + d5 + R32(gGameState) - 0x3a);
    if (cell != 0x1d && S16(IsSpace(d7)) != 0) return (uint16_t)d6;
    return 0;
}

/* 0003b86a: add the mob objects of a rock piece spanning several columns (gMobX up to
 * gMobState, one object per 0x24 pixels) */
uint32_t FUN_0003b86a(void) {
    ENTRY_NOLINK(FN_FUN_0003b86a, 16);           /* movem D3/D5-D7 */
    int16_t d5 = R16(gMobState);
    int16_t d6 = R16(gMobRow);
    int16_t d7 = (int16_t)((int32_t)(R16(gMobX) & 7) >> 1);
    if (R16(gMobX) >= R16(gMobState)) return 0;
    do {
        int16_t c = S16(FirstInRow(d6));
        c = (int16_t)(c + (int16_t)(R16(gMobX) / 0x33));
        W16(gObjCell, c);
        if (R16(gMobType) == 8) W16(gObjCell, R16(gObjCell) + 1);
        AddMobObj(R16(gMobY));
        uint32_t obj = (uint32_t)((int32_t)R16(gNumObjs) * 0x1c) + R32(gObjTable);
        W16(obj - 4, d7);
        W16(gMobX, R16(gMobX) + 0x24);
    } while (R16(gMobX) < d5);
    return 0;
}

/* 0003b8f6: ATM proc of the sliding rock: draw its pieces (and the dust while moving) */
uint32_t ATMSlideRock(void) {
    ENTRY_LINK(FN_ATMSlideRock, 8, 20);          /* link -8; movem D3-D7 */
    if (R16(gMobSpeed) == -1) return 0;
    int16_t d7 = S16(GetRow(R16(gMobY)));
    int16_t d4 = (int16_t)(R16(gMobX) / 0x33);
    int16_t cell = S16(IndexCell1(d4, d7));
    W16(gObjCell, cell);
    if (S16(IsVisibleRoomCell(R16(gMobRoom), cell)) == 0) return 0;
    int16_t w2 = R16(gMobDrawW2);                 /* -6(A6) */
    int16_t savedY;                               /* -8(A6) */
    if (S16(IsAllFloorsInDirToRock(-1, 0x52)) == 0) {
        savedY = R16(gMobY);
        W16(gMobY, R16(gMobY) - 0x70);
        int16_t d3 = R16(gMobDrawX10);
        int32_t dist = (int32_t)R16(gMobState) - (int32_t)R16(gMobX);
        int32_t d6 = R16(gMobDrawX10);
        if (dist <= d6) d6 = dist;
        W16(gMobDrawX10, (int16_t)d6);
        W16(gMobX, R16(gMobX) + 8);
        W16(gMobDrawW, 0x13);
        MarkMobRedraw(2);
        W16(gMobY, savedY);
        W16(gMobX, R16(gMobX) - 8);
        W16(gMobDrawX10, d3);
        W16(gMobDrawW, w2);
    } else {
        W16(gMobDrawW, 0x6d);
        W16(gMobDrawW2, 0x6f);
    }
    W16(gMobType, 7);
    AddMobObj(R16(gMobY));
    d4 = R16(gMobX);
    savedY = R16(gMobY);
    W16(gMobX, R16(gMobX) + 0x33);
    if ((int32_t)R16(gMobX) < (int32_t)RU16(gMobState)) {
        int16_t c = S16(FirstInRow(d7));
        c = (int16_t)(c + (int16_t)(R16(gMobX) / 0x33));
        W16(gObjCell, c);
        W16(gMobY, R16(gMobY) - 2);
        W16(gMobType, 6);
        AddMobObj(R16(gMobY));
    }
    static const struct { int16_t xoff, yoff, type; } parts[3] = {
        {0x105792 - 0x105790, 0, 8}, {0x105796 - 0x105790, 4, 8}, {0x10579a - 0x105790, 8, 9} };
    for (int i = 0; i < 3; i++) {
        int16_t x = (int16_t)(d4 + R16(gRockPartsX + parts[i].xoff));
        W16(gMobX, x);
        if ((int32_t)x < (int32_t)RU16(gMobState)) {
            W16(gMobY, (int16_t)(savedY + R16(gRockPartsX + parts[i].yoff)));
            W16(gMobType, parts[i].type);
            FUN_0003b86a();
        }
    }
    W16(gMobX, d4);
    W16(gMobY, savedY);
    W16(gMobType, 6);
    W16(gMobDrawW, w2);
    W16(gMobDrawW2, w2);
    return 0;
}

/* 0003baf6: are the floors under the rock solid both behind and ahead of it? */
uint32_t FUN_0003baf6(void) {
    ENTRY_NOLINK(FN_FUN_0003baf6, 8);            /* movem D3/D4 */
    int16_t d4 = 0;
    uint32_t r = IsAllFloorsInDirToRock(-1, 0x33);
    if (S16(r) != 0) {
        r = IsAllFloorsInDirToRock(0, 0x33);
        d4 = (int16_t)r;
    }
    return (r & 0xffff0000u) | (uint16_t)d4;
}

/* 0003bb22: screen rect of the sliding rock in (room, row); writes 0 into its rect
 * argument slot (and returns 0) when there is none */
uint32_t ComputeSlideRockRect(uint32_t rect, int16_t room, int16_t row) {
    ENTRY_LINK(FN_ComputeSlideRockRect, 0, 8);   /* link 0; movem A2/A3 */
    uint32_t a3 = GetSlideRockOnRoomRow(room, row);
    if (a3 != 0) {
        W16(rect, (int16_t)(R16(a3 + 2) - 0x84));
        W16(rect + 2, R16(a3));
        W16(rect + 4, (int16_t)(R16(a3 + 2) + 1));
        W16(rect + 6, (int16_t)(R16(rect + 2) + 0x52));
    } else {
        rect = 0;
        W32(ARG16(0), 0);
    }
    return rect;
}

/* 0003bb96: mob proc: is the kid under the moving rock? crush him */
uint32_t CheckSlideRockCrush(void) {
    ENTRY_LINK(FN_CheckSlideRockCrush, 4, 12);   /* link -4; movem D3-D5 */
    if (R16(gMobRoom) != R16(gKidRoom)) return RU16(gMobRoom);
    if (R16(gMobRow) != R16(gKidRow)) return RU16(gMobRow);
    int16_t face = R16(gKidFace);
    int16_t d4 = (face == 0) ? R16(gKidX) : (int16_t)(R16(gKidX) + R16(gCharImgW));
    int16_t d5 = (int16_t)(R16(gMobX) + 0xdf);
    if (R16(gKidField34) != 4) {
        if (d5 >= d4) return (uint16_t)face;
        if ((int32_t)d5 + 0x19 <= (int32_t)d4) return (uint16_t)d5;
    }
    FUN_000225d2();
    FUN_0003bfa4();
    save_kid();
    return 0xffff;                               /* D0 after the dbra copy loop */
}

/* 0003bc28: DrawObj proc of the rock pieces (piece 0x86..0x89) */
uint32_t DrawObjSlideRock(int16_t piece) {
    ENTRY_LINK(FN_DrawObjSlideRock, 4, 4);       /* link -4; D4 */
    W16(GH(0x104b4a), R16(gObjShape));
    int16_t d4;
    switch ((uint16_t)piece) {
    case 0x86: d4 = 0x5d; break;
    case 0x87: d4 = 0x54; break;
    case 0x88: d4 = (int16_t)(R16(gObjPiece) + 0x59); break;
    case 0x89: d4 = (int16_t)(R16(gObjPiece) + 0x55); break;
    default:   d4 = -1; break;
    }
    if (d4 != -1) AddMid(4, d4, R16(gObjX), R16(gObjY), 0x10);
    return 0;
}

/* 0003bcae: x of the rock's barrier in (room, row): dir 0 = its right side, -1 = left side
 * (pushed against a wall or the kid). 0 / gColLeftLimit when there is no rock. */
uint32_t GetSlideRockBarrDist(int16_t dir, int16_t room, int16_t row) {
    ENTRY_LINK(FN_GetSlideRockBarrDist, 0, 24);  /* link 0; movem D3-D7/A2 */
    uint32_t a2 = GetSlideRockOnRoomRow(room, row);
    int16_t d5;
    if (a2 != 0) {
        d5 = (int16_t)(R16(a2) + 0xdf);
        if (dir == -1) {
            d5 = (int16_t)(d5 - 5);
            int16_t d7 = S16(GetCharCol((int16_t)(d5 - 0x33)));
            int32_t c = (int32_t)S16(FirstInRow(R16(a2 + 0xc))) + (int32_t)d7;
            uint32_t a1 = (uint32_t)(c << 1);
            uint32_t a0 = (uint32_t)RU16(a2 + 4) * 0x3c + R32(gGameState) - 0x3c;
            int16_t d6 = R16(a1 + a0);
            if (S16(IsWall(d6)) != 0 || d6 == 4) {
                d6 = (int16_t)(R16(gColRight + (int16_t)((int32_t)d7 << 1)) + 0x27);
                if (R16(gCharFace) == 0) d6 = (int16_t)(d6 + (int16_t)(R16(gCharRightEdge) - R16(gCharLeftEdge)));
                d5 = (d5 > d6) ? d5 : d6;
            }
        } else {
            d5 = (int16_t)(d5 + 0x38);
        }
    } else {
        d5 = (dir == -1) ? R16(gColLeftLimit) : 0;
    }
    return (uint16_t)d5;
}

/* 0003bdac: barrier data for the collision code: marks the rock's left/right sides in the
 * columns it covers on the character's row (rooms[col] = rock room, codes[col] |= 0x0f /
 * 0xf0), and updates the previous frame's codes (gColDetBarr) when the rock moved. */
uint32_t GetSlideRockDetData(int16_t row, uint32_t rooms, uint32_t codes) {
    ENTRY_LINK(FN_GetSlideRockDetData, 0x10, 32);   /* link -0x10; movem D3-D7/A2-A4 */
    uint32_t a4 = GetSlideRockOnRoomRow(R16(gCharRoom), row);
    if (a4 == 0) return 0;
    int16_t d7 = (int16_t)(R16(a4) + 0xda);
    if (d7 < R16(gCharRightEdge)) {
        int16_t c = S16(GetCharCol(d7));
        if (c >= 0 && c < 10) {
            int32_t off = (int32_t)c * 2;
            W16(codes + off, RU16(codes + off) | 0xf);
            W16(rooms + off, RU16(a4 + 4));
        }
        if (R16(a4 + 8) != 0x64 && R16(gCharRow) == row) {
            int16_t c2 = S16(GetCharCol((int16_t)(d7 + 2)));
            if (c2 != c) {
                int32_t o2 = (int32_t)c2 * 2;
                maddr a3 = gColDetBarr + (int16_t)o2;
                uint16_t v = RU16(a3) & 0xf;
                maddr dst = gColDetBarr + (int16_t)((int32_t)c * 2);
                W16(dst, RU16(dst) | v);
                if ((RU16(codes + o2) & 0xf) == 0) W16(a3, RU16(a3) & 0xf0);
            }
        }
    }
    int16_t d5 = (int16_t)(d7 + 0x3d);
    if (d5 <= R16(gCharLeftEdge)) return 0;
    int16_t c = S16(GetCharCol(d5));
    if (c >= 0 && c < 10) {
        int32_t off = (int32_t)c * 2;
        W16(codes + off, RU16(codes + off) | 0xf0);
        W16(rooms + off, RU16(a4 + 4));
    }
    if (R16(a4 + 8) != 0x64 && R16(gCharRow) == row) {
        int16_t c2 = S16(GetCharCol((int16_t)(d5 + 2)));
        if (c2 != c) {
            int32_t o2 = (int32_t)c2 * 2;
            maddr a3 = gColDetBarr + (int16_t)o2;
            uint16_t v = RU16(a3) & 0xf0;
            maddr dst = gColDetBarr + (int16_t)((int32_t)c * 2);
            W16(dst, RU16(dst) | v);
            if ((RU16(codes + o2) & 0xf0) == 0) W16(a3, RU16(a3) & 0xf);
        }
    }
    return 0;                                    /* "ret": 0 */
}

/* 0003bf3e: the sliding rock mob (type 6) in (room, row), or NULL */
uint32_t GetSlideRockOnRoomRow(int16_t room, int16_t row) {
    ENTRY_LINK(FN_GetSlideRockOnRoomRow, 0, 12); /* link 0; movem D3/D4/A2 */
    int16_t d4 = 0, d3 = 1;
    uint32_t a2;
    do {
        int16_t n = d3++;
        a2 = GetMobTypeItem(6, n);
        if (a2 == 0) d4 = 1;
        else if (R16(a2 + 4) == room && R16(a2 + 0xc) == row) d4 = 1;
    } while (d4 == 0);
    return a2;
}

/* 0003bfa4: the rock hits the character: push him back against the rock or a wall, and
 * crush him (death sequence 0x47) when he is caught against a wall */
uint32_t FUN_0003bfa4(void) {
    ENTRY_NOLINK(FN_FUN_0003bfa4, 16);           /* movem D3-D6 */
    if (R16(GH(0x102fba)) == 5) return 0;
    int16_t d5 = -1;
    int16_t d3 = R16(gMobX);
    if ((int32_t)R16(gCharX) - 0xe5 > (int32_t)d3) {
        d3 = (int16_t)(d3 + 0xdf);
        W16(gCharX, d3);
        if (R16(gCharFace) == -1) W16(gCharX, R16(gCharX) - R16(gCharImgW));
    }
    d3 = (int16_t)(R16(gCharX) - 0xc);
    if (R16(gCharFace) == 0) d3 = (int16_t)(d3 - R16(gCharImgW));
    int16_t d6 = S16(GetCharCol(d3));
    int16_t d4 = S16(ReadCell(R16(gCharRoom), d6, R16(gCharRow)));
    if (S16(IsWall(d4)) != 0 || d4 == 4) {
        W16(gCharX, (int16_t)(R16(gColRight + (int16_t)((int32_t)d6 * 2)) + 0xc));
        if (R16(gCharFace) == 0) W16(gCharX, R16(gCharX) + (int16_t)(R16(gCharImgW) + 6));
        if (d4 != 2) {
            if (R16(gMobAccel) == 0x64) {
                if (R16(gCharAlive) < 0) {
                    DecLifePts(R16(gCharLife));
                    W16(gCharAlive, 0);
                }
                W16(gCharFrame, 0xb9);
            } else {
                d3 = (int16_t)(R16(gMobX) - 0x12);
                d6 = (int16_t)(d3 / 0x33);
                d4 = S16(ReadCell(R16(gCharRoom), d6, R16(gCharRow)));
                if (S16(IsWall(d4)) != 0 || d4 == 4) {
                    if ((int16_t)(d3 % 0x33) < 0x2e) {
                        if (R16(gCharAlive) < 0) AddSound(0xc1);
                        SetKidDeathMethod(0xd);
                    }
                }
                W16(gCharField34, 4);
                d5 = 0x47;
            }
        }
    } else if (R16(gCharAlive) < 0) {
        if (R16(gCharOnRock) == 1) {
            d5 = 0x5d;
            W16(gCharOnRock, 0);
        } else {
            d5 = (R16(gCharFace) == 0) ? 0x2f : 0x30;
        }
    } else {
        W16(gCharX, R16(gCharX) - 0xc);
    }
    if (d5 != -1) {
        FUN_00020cca();
        W16(gCharYVel, 0);
        JumpSeq(d5);
        AnimChar();
    }
    return 0;
}

/* 0003c144: are all cells from the rock's column to dist pixels further (clipped to
 * where the rock stops) floors, in row gMobRow + drow? The original stores the clipped
 * distance back into its argument slot. */
uint32_t IsAllFloorsInDirToRock(int16_t drow, int16_t dist) {
    ENTRY_LINK(FN_IsAllFloorsInDirToRock, 8, 20);   /* link -8; movem D3-D7 */
    int16_t d5 = 1;
    int32_t x = R16(gMobX);
    int16_t d6 = (int16_t)((x + 2) / 0x33);
    int32_t d3 = (int32_t)(uint32_t)RU16(gMobState) - x;
    int32_t d4 = dist;
    if (d3 <= d4) d4 = d3;
    W16(ARG16(2), (int16_t)d4);
    int32_t q = (x + (int32_t)(int16_t)d4) / 0x33;
    int16_t d7 = (int16_t)q;
    int16_t r = (int16_t)(R16(gMobRow) + drow);
    uint32_t d0 = ((uint32_t)q & 0xffff0000u) | (uint16_t)r;
    if (d6 <= d7) {
        do {
            int16_t c = d6++;
            int16_t id = S16(ReadCell(R16(gMobRoom), c, r));
            d5 = (S16(IsSpace(id)) != 0 || id == 0x19) ? 0 : 1;
            d0 = (uint16_t)d5;
        } while (d6 <= d7 && d5 != 0);
    }
    return (d0 & 0xffff0000u) | (uint16_t)d5;
}

/* 0003c1fc: mob proc of the sliding rock: move it left (accelerating), stop it against
 * walls/raised floors, crush the kid, sounds */
uint32_t FUN_0003c1fc(void) {
    ENTRY_NOLINK(FN_FUN_0003c1fc, 20);           /* movem D3-D7 */
    if (R16(gMobAccel) == 0 || R16(gMobAccel) == 0x64) return 0;
    int16_t d4 = R16(gMobX);
    if (R16(gMobAccel) == -2) {
        d4 = (int16_t)(d4 - 2);
    } else if (S16(FUN_0003baf6()) != 0 && R16(gMobAccel) >= 8) {
        W16(gMobAccel, 0);
        PrinceResetSnd(0xc0);
        if (R16(gSoundPlaying) == 0xc0) W16(gSoundPlaying, -1);
    } else {
        W16(gMobX, R16(gMobX) - 0x10);
        if (S16(FUN_0003baf6()) != 0) AddSound(0xbf);
        W16(gMobX, d4);
        d4 = (int16_t)(d4 - 2);
        if (R16(gMobAccel) <= 8) W16(gMobAccel, R16(gMobAccel) + 1);
    }
    if (d4 < 0) {
        W16(gMobRoom, R16(R32(gGameState) + 0x2080 + (uint32_t)RU16(gMobRoom) * 8 - 8));
        d4 = (int16_t)(d4 + 0x1fe);
    }
    if (R16(gMobRoom) == 0) {
        W16(gMobSpeed, -1);
        PrinceResetSnd(0xc0);
        return 0;
    }
    if (R16(gMobAccel) == 0 || R16(gMobAccel) == 0x64) return 0;
    int16_t d3 = (int16_t)(d4 - 0x1f);
    int16_t d6 = (int16_t)(d3 / 0x33);
    if (d3 <= 0) d6 = (int16_t)(d6 - 1);
    int16_t d7 = S16(ReadCell(R16(gMobRoom), d6, R16(gMobRow)));
    if (R16(gMobSpeed) == 1) {
        W16(gMobSpeed, 0);
        W16(gMobAccel, 0x64);
        W16(gMobY, R16(gMobY) + 1);
        W16(gMobX, (int16_t)(d4 + 3));
        if (d7 == 2) {
            FUN_000225d2();
            seg5_027e6e();
            save_kid();
        }
    } else {
        d3 = (int16_t)(((int32_t)R16(gMobX) - 6) / 0x33);
        int16_t d5 = (int16_t)(((int32_t)d4 - 6) / 0x33);
        if (!(d5 == d3 && d6 == d3)) {
            d3 = S16(ReadCell(R16(gMobRoom), d5, R16(gMobRow)));
            if ((S16(IsWall(d3)) != 0 || d3 == 4 || d7 == 2) && R16(gMobSpeed) == 0) {
                W16(gMobSpeed, 1);
                W16(gMobY, R16(gMobY) - 1);
                if (S16(PrinceSndOn(0xc1)) == 0 && d7 != 2) AddSound(0x12);
            }
        }
        uint16_t f = RU16(gKidFrame);
        if (f >= 0x57 && f <= 0x63) {
            d3 = (int16_t)(((int32_t)d4 - 0xc) / 0x33);
            if ((int32_t)R16(gMobRow) + 1 == (int32_t)R16(gKidRow) && R16(gKidCol) == d3) {
                FUN_000225d2();
                seg6_02c0c4();
                save_kid();
            }
        }
    }
    if (R16(gMobAccel) != 0x64) {
        W16(gMobX, d4);
        if (R16(gMobAccel) != 0) AddSound(0xc0);
    } else {
        PrinceResetSnd(0xc0);
    }
    return 0;                                    /* "ret": 0 */
}

/* 0003c448: is the sliding rock in the cell 1 or 2 columns right of the current piece? */
uint32_t SlideRockNearSpikes(void) {
    ENTRY_LINK(FN_SlideRockNearSpikes, 0x14, 8); /* link -0x14; movem D3/D4 */
    mcopy(a6 - 0x14, GH(0x104b50), 0x14);
    ReadCell(R16(gPieceRoom), (int16_t)(R16(gPieceCol) + 1), R16(gPieceRow));
    int16_t d4 = S16(SlideRockInCell(0x19));
    if (d4 == 0) {
        ReadCell(R16(gPieceRoom), (int16_t)(R16(gPieceCol) + 2), R16(gPieceRow));
        d4 = S16(SlideRockInCell(0x19));
    }
    mcopy(GH(0x104b50), a6 - 0x14, 0x14);
    return (uint16_t)d4;
}

/* 0003c4d6: does the rock overlap the current piece's cell (with margin dx)? */
uint32_t SlideRockInCell(int16_t dx) {
    ENTRY_LINK(FN_SlideRockInCell, 8, 20);       /* link -8; movem D3-D7 */
    int16_t d6 = 0;
    int16_t d7 = S16(GetSlideRockBarrDist(0, R16(gPieceRoom), R16(gPieceRow)));
    if (d7 == 0) return 0;
    int16_t left = (int16_t)(R16(gPieceCol) * 0x33);   /* -8(A6) */
    int16_t right = (int16_t)(left + 0x33);            /* -6(A6) */
    d7 = (int16_t)(d7 - 0xdf);
    int32_t m = 0x33 - (int32_t)dx;                    /* -4(A6) */
    if (!(m + left > (int32_t)d7) && m + right >= (int32_t)d7) return 1;
    d7 = S16(GetSlideRockBarrDist(-1, R16(gPieceRoom), R16(gPieceRow)));
    d7 = (int16_t)(d7 - 0xdf);
    if (!((int32_t)left - dx > (int32_t)d7) && (int32_t)right - dx >= (int32_t)d7) d6 = 1;
    return (uint16_t)d6;
}

/* 0003c59e: x limit of the rock in (room, row) seen from the character (GetSlideRockX) */
uint32_t TempGetSlideRockX(int16_t room, int16_t row) {
    ENTRY_LINK(FN_TempGetSlideRockX, 0, 4);      /* link 0; D3 */
    int16_t d3 = S16(GetSlideRockBarrDist(0, room, row));
    if (d3 != 0 && R16(gCharX) < d3) d3 = S16(GetSlideRockBarrDist(-1, room, row));
    return (uint16_t)d3;
}

/* 0003c5f2: trigger: how 1 = start the rock (create the mob if needed), 2 = re-arm.
 * The original may replace its room argument by the current room in its slot. */
uint32_t TriggerSlideRock(int16_t room, int16_t cell, int16_t how) {
    ENTRY_LINK(FN_TriggerSlideRock, 0, 12);      /* link 0; movem D3/D4/A3 */
    uint32_t a3 = GetSlideRockOnRoomRow(room, (int16_t)(cell / 10));
    if (a3 != 0) {
        if (how == 1) {
            int16_t d3 = R16(a3 + 8);
            if (d3 >= 0 && d3 != 0x64) W16(a3 + 8, -2);
        } else if (how == 2) {
            if (R16(a3 + 8) == -2) W16(a3 + 8, 2);
        }
        return 0;
    }
    if (how != 1) return 0;
    ReadCellWithCell(room, cell);
    W16(gMobAccel, -2);
    W16(gMobSpeed, 0);
    int16_t d4 = R16(gColLeft + (int16_t)((int32_t)R16(gPieceCol) * 2));
    if (d4 > 0x1fe && room == R16(gRoomLeft)) {
        room = R16(gCurRoom);
        W16(ARG16(0), room);
        d4 = (int16_t)(d4 - 0x1fe);
    }
    W16(gMobX, d4);
    W16(gMobState, d4);
    W16(gMobRoom, room);
    int16_t d3 = R16(gPieceRow);
    W16(gMobY, (int16_t)(R16(gRowBottom + (int16_t)((int32_t)d3 * 2)) - 9));
    W16(gMobRow, d3);
    W16(gMobType, 6);
    FUN_00026a94();
    return 0;
}

/* 0003c6fc: ATM proc of the trap door: animate (gMobState & 0x1f = frame), open the floor
 * when done, draw the door */
uint32_t ATMTrapDoor(void) {
    ENTRY_LINK(FN_ATMTrapDoor, 0x1c, 24);        /* link -0x1c; movem D3-D7/A2 */
    int16_t x = R16(gMobX);                      /* -0x1a(A6) */
    int16_t y = R16(gMobY);                      /* -0x18(A6) */
    int16_t d6 = R16(gMobRoom);
    int16_t room = R16(gMobRoom);
    int16_t d3 = R16(gCurRoom);
    if (room != d3) {
        if (room == R16(gRoomBelow)) {
            y = (int16_t)(y + 0x16c); d6 = d3;
        } else if (R16(gMobRoom) == R16(gRoomAbove)) {
            y = (int16_t)(y - 0x16c); d6 = R16(gCurRoom);
        } else if (R16(gMobRoom) == R16(gRoomLeft)) {
            x = (int16_t)(R16(gMobX) - 0x1fe); d6 = R16(gCurRoom);
        } else if (R16(gMobRoom) == R16(gRoomRight)) {
            x = (int16_t)(R16(gMobX) + 0x1fe); d6 = R16(gCurRoom);
        }
    }
    int16_t col = (int16_t)(x / 0x33);           /* -0x1c(A6) */
    d3 = (int16_t)(y / 0x78);
    W16(gObjCell, S16(IndexCell1(col, d3)));
    int16_t d7 = S16(FirstInRow(R16(gMobRow)));
    d7 = (int16_t)(d7 + (int16_t)(R16(gMobX) / 0x33));
    int16_t frame = R16(gMobState) & 0x1f;       /* -0xc(A6) */
    if (frame > 0x10) {
        d3 = -1;
        W16(gMobSpeed, d3);
        uint32_t a2 = (uint32_t)RU16(gMobRoom) * 0x78 + (uint32_t)((int32_t)d7 * 4) + R32(gGameState) + 0x708;
        W16(a2, RU16(a2) & 0xff00);
        W16((uint32_t)RU16(gMobRoom) * 0x3c + (uint32_t)((int32_t)d7 * 2) + R32(gGameState) - 0x3c, 0x1a);
        frame = 0x11;
        PrinceResetSnd(0xc6);
        if (R16(gSoundPlaying) == 0xc6) W16(gSoundPlaying, d3);
        AddSound(0x1b);
    }
    int32_t idx = R16(gTrapFrames + (int16_t)((int32_t)frame * 2));
    uint32_t a2 = (uint32_t)(idx * 6) + gTrapShapes;
    d7 = S16(FirstInRow(R16(gMobRow)));
    d7 = (int16_t)(d7 + col);
    if (S16(IsVisibleRoomCell(d6, d7)) == 0) return 0;
    int16_t d4 = R16(gMobY);
    W16(gMobY, R16(gMobY) + R16(a2 + 4));
    y = (int16_t)(y + R16(a2 + 4));
    int16_t d5 = R16(gMobX);
    W16(gMobX, (int16_t)(x + R16(a2 + 2)));
    if (GetBGSetImgSize((int16_t)(R16(a2) - 1), a6 - 8) != 0) {
        W16(gMobDrawH, (int16_t)(R16(a6 - 8) + 1));
        W16(a6 - 6, R16(a6 - 6) + 1);
        int16_t w = (int16_t)(col * 0x33 + 0x4d - R16(gMobX));
        if (w > R16(a6 - 6)) W16(a6 - 6, w);
        W16(gMobDrawW18, R16(a6 - 6));
    }
    MarkMobRedraw(0);
    if (R16(gMobSpeed) != -1) {
        ComputeMobRect(a6 - 0x14);
        W16(a6 - 0x12, R16(a6 - 0x12) + 0x19);
        SECTRECT(gScreenRect, a6 - 0x14, a6 - 0x14);
        MarkBufsForRect(JT_ADDR(FN_MarkFloor), a6 - 0x14);   /* the original also pushes a word -1 */
        AddMobObj(y);
    }
    W16(gMobX, d5);
    W16(gMobY, d4);
    return 0;
}

/* 0003c99a: mob proc of the trap door: advance its frame up to 0x11, door sound */
uint32_t FUN_0003c99a(void) {
    ENTRY_NOLINK(FN_FUN_0003c99a, 4);            /* D3 */
    if (R16(gMobSpeed) == -1) return 0;
    int16_t d3 = R16(gMobState) & 0x1f;
    if (d3 <= 0x10) W16(gMobState, R16(gMobState) + 1);
    if (S16(PrinceSndOn(0xc5)) == 0 && S16(PrinceSndOn(0xc6)) == 0) AddSound(0xc6);
    return 0;
}

/* 0003c9e0: the character stands on a closed trap door: trigger it */
uint32_t seg10_03c9e0(void) {
    ENTRY_NOLINK(FN_seg10_03c9e0, 0);
    if ((R16(GH(0x102df6)) & 0x40) == 0) return 0;
    if ((int32_t)R16(gCharRow) * 0x78 + 0x6a != (int32_t)R16(gCharY)) return 0;
    if (R16(gCharType) == 1) return 0;
    if (S16(ReadCell(R16(gCharRoom), R16(gCharCol), R16(gCharRow))) != 0x1a) return 0;
    if ((R16(gPieceSpec) & 0x1f) != 0) return 0;
    TriggerTrapDoor(R16(gPieceRoom), R16(gPieceCell));
    return 0;                                    /* "ret": 0 */
}

/* 0003ca4a: shaking floor on a trap door (ShakeEm/Falling): trigger it, marking it 0x40
 * when the character is not in its column */
uint32_t seg10_03ca4a(void) {
    ENTRY_NOLINK(FN_seg10_03ca4a, 4);            /* D3 */
    if ((R16(gPieceSpec) & 0x40) != 0) return 0;
    if (R16(gCharCol) != R16(gPieceCol)) {
        W16(gPieceSpec, R16(gPieceSpec) | 0x40);
        W16((uint32_t)((int32_t)R16(gPieceCell) * 4) + R32(gRoomSpecs), R16(gPieceSpec));
    }
    TriggerTrapDoor(R16(gPieceRoom), R16(gPieceCell));
    return 0;                                    /* "ret": 0 */
}

/* 0003ca88: open the trap door at (room, cell): the cell becomes 0x81 / empty, and a
 * trap door mob (type 10) is created */
uint32_t TriggerTrapDoor(int16_t room, int16_t cell) {
    ENTRY_LINK(FN_TriggerTrapDoor, 4, 20);       /* link -4; movem D3-D6/A2 */
    int32_t c = cell;
    uint32_t a2 = R32(gGameState) + 0x780 + (uint32_t)(uint16_t)room * 0x78 - 0x78 + (uint32_t)(c * 4);
    W16(a2, 0x81);
    W16(R32(gRoomTiles) + (uint32_t)(c * 2), 0);
    W16(gMobAccel, 0);
    W16(gMobSpeed, 0);
    int16_t d6 = (int16_t)(cell % 10);
    W16(gMobX, R16(gColLeft + (int16_t)(d6 * 2)));
    W16(gMobState, (R16(gPieceSpec) & 0x40) ? 0x4b : 1);
    W16(gMobRoom, room);
    W16(gMobY, R16(gRowBottom + (int16_t)((int32_t)R16(gPieceRow) * 2)));
    W16(gMobRow, (int16_t)(cell / 10));
    W16(gMobType, 10);
    FUN_00026a94();
    AddSound(0xc5);
    MarkMobRedraw(0);
    return 0;
}

/* 0003cb8a: DrawObj proc of the trap door (frame in the object's shape & 0x1f) */
uint32_t DrawObjTrapDoor(void) {
    ENTRY_LINK(FN_DrawObjTrapDoor, 4, 8);        /* link -4; movem D3/D4 */
    int16_t d3 = R16(gObjShape) & 0x1f;
    int32_t idx = R16(gTrapFrames + (int16_t)((int32_t)d3 * 2));
    int16_t d4 = R16(gTrapShapes + (int16_t)(idx * 6));
    if (d4 == 0x1c) {
        AddMid(4, d4, (int16_t)(R16(gObjX) + 1), (int16_t)(R16(gObjY) - 1), 0x10);
        AddMid(4, 0x1d, R16(gObjX), (int16_t)(R16(gObjY) - 1), 0);
    } else {
        AddMid(4, d4, R16(gObjX), (int16_t)(R16(gObjY) - 1), 0x10);
    }
    return 0;
}

/* 0003cc46: music when the kid reaches column 9 */
uint32_t FUN_0003cc46(void) {
    ENTRY_NOLINK(FN_FUN_0003cc46, 0);
    if (R16(gKidCol) >= 9) AddSong(0x124);
    return 0;
}

/* 0003cc5a: level 13, room 4 (the altar): per-frame check */
uint32_t seg10_03cc5a(void) {
    ENTRY_NOLINK(FN_seg10_03cc5a, 0);
    if (R16(gCurRoom) == 4 && LEVEL_NUM() == 0xd) FUN_0003cf02();
    return 0;
}

/* 0003cc78: the shadow merges into the kid: back to a living kid with full life */
uint32_t FUN_0003cc78(void) {
    ENTRY_NOLINK(FN_FUN_0003cc78, 4);            /* D3 */
    int16_t d3 = R16(gChar);
    seg4_022516();
    FUN_000225d2();
    JumpSeq(0xe7);
    W16(gCharAlive, -1);
    W16(gCharLife, R16(gCharMaxLife));
    ClrLifeBar();
    W16(gMsgId, 0);
    W16(gMsgTimer, 0);
    DrawKidMeter(R16(gCharLife), R16(gCharMaxLife));
    W16(gCharField34, 0xd);
    W16(gCharOnRock, 0);
    save_kid();
    LoadShad(d3);
    FUN_000223c8();
    AddSong(0xfc);
    return 0;
}

/* 0003ccf0: level 13 altar cut scene: drive the kid/shadow by frame */
uint32_t seg10_03ccf0(void) {
    ENTRY_NOLINK(FN_seg10_03ccf0, 12);           /* movem D3-D5 */
    int16_t d5 = -1;
    if (R16(gCharFrame) == 0xf) {
        if (R16(gCharFace) == -1) {
            if (R16(gCharCol) >= 6) {
                if (R16(gCharField34) == 0xd) d5 = 0x47;
                else if (R16(gCharCol) >= 8) d5 = 1;
                else {
                    DoStepFwd(0);
                    if (R16(gCharSeqLast) == 0x2c) d5 = 3;
                }
            } else if (R16(gCharField34) == 0xd || R16(gCharSeqLast) == 0xe8) {
                d5 = 5;
                AddSong(0xfb);
            } else {
                d5 = 0xe8;
            }
        } else if (R16(gCharField34) != 0xd) {
            d5 = 5;
        } else if (R16(gCharCol) >= 6) {
            int16_t d4 = (int16_t)(R16(gKidX) - (int16_t)(R16(gCharX) - 10));
            int16_t d3 = R16(gCharFace);
            if (d3 == R16(gKidFace))
                d4 = (int16_t)(d4 + R16(gStepDir + (int16_t)((int32_t)d3 * 2)) * 0xc);
            if (d4 > 4) {
                DoStepFwd(d4);
            } else {
                W16(gCharX, AddCharX(d4));
                d5 = (R16(gKidFace) == R16(gCharFace)) ? 0x47 : 5;
            }
        } else {
            DoStepFwd(0);
            if (R16(gCharSeqLast) == 0x2c) d5 = 0xf7;
        }
    } else {
        uint16_t f = RU16(gCharFrame);
        if (((f >= 1 && f <= 0xe) || (f >= 0x31 && f <= 0x38)) && R16(gCharCol) >= 6) {
            DoRunJump(4);
        } else if ((f = RU16(gCharFrame)) >= 0x67 && f <= 0x6a) {
            W16(GH(0x104b80), -1);
            seg4_02129a();
        } else if (R16(gCharFrame) == 0x5b && S16(seg6_02c1be()) == 0) {
            seg6_02bfd8();
            if (R16(gCharField34) == 0xd) W16(gCharY, R16(gCharY) - 2);
            if (R16(gCharFace) == 0) {
                GetCharEdges();
                MarkBufsForRect(JT_ADDR(FN_MarkFloor), gCharRect);   /* the original also pushes gChar.id */
            }
        } else if (R16(gCharFrame) == 0x9a) {
            W16(R32(gGameState) + 0x914, 0x85);
        } else if (R16(gCharFrame) == 0x99 && R16(gCharField34) == 0xd) {
            PoPSetPalette(0xfed, 0x30);
        } else if (R16(gCharField34) == 0xd && R16(gCharFrame) == 0xb9) {
            FUN_0003cc78();
        } else if (R16(gCharFrame) == 0x2c) {
            d5 = 0xd;
            W16(gCharX, AddCharX(-0xc));
        }
    }
    if (d5 != -1) JumpSeq(d5);
    return 0;
}

/* 0003cf02: level 13 altar: the kid falls/dies near the flames, music, the shadow appears */
uint32_t FUN_0003cf02(void) {
    ENTRY_NOLINK(FN_FUN_0003cf02, 0);
    if (R16(gKidRow) != 1) return 0;
    if (R16(gKidSeq) == 0xe6) return 0;
    if (R16(gKidX) <= 0x15b) {
        FUN_000225d2();
        JumpSeq(0xe6);
        W16(gCharX, 0x14f);
        DecLifePts(100);
        AddSound(0xf9);
        SetKidDeathMethod(0x10);
        W16(R32(gGameState) + 0x914, 0x85);
        save_kid();
    } else if (R16(gKidX) <= 0x177 && S16(PrinceSndOn(0xfd)) == 0) {
        AddSong(0xfd);
    } else if (R16(gKidFrame) == 0xb9 && R16(gKidRow) == 1 && R16(gKidField34) != 0xc &&
               R16(gLevelTrigger) == 1) {
        FUN_000225d2();
        FUN_0003cfd2();
        W16(gLevelTrigger, -1);
        save_kid();
    }
    return 0;
}

/* 0003cfd2: level 13 altar: turn the kid into a new opponent record (the shadow) */
uint32_t FUN_0003cfd2(void) {
    ENTRY_NOLINK(FN_FUN_0003cfd2, 12);           /* movem D3/D4/A2 */
    int16_t d3 = R16(gCharRoom);
    if (d3 != 4) return 0;
    if (LEVEL_NUM() != 0xd) return 0;
    save_kid();
    int16_t d4 = 0;
    if (d3 != 0) d4 = R16(R32(gGameState) + 0x21a6 + (uint32_t)(uint16_t)d3 * 0xc0 - 0xc0);
    uint32_t cnt = R32(gGameState) + 0x21a6 + (uint32_t)(uint16_t)d3 * 0xc0 - 0xc0;
    W16(cnt, R16(cnt) + 1);
    uint32_t a2 = GetOppInfoIdx(R16(gCharRoom), d4);
    int16_t c = S16(FirstInRow(R16(gCharRow)));
    W16(a2, (int16_t)(c + R16(gCharCol)));
    W16(a2 + 2, R16(gCharX));
    W16(a2 + 4, R16(gCharFace));
    W16(a2 + 6, 0);
    W16(a2 + 0xc, 0);
    W16(a2 + 0xe, d4);
    W16(a2 + 0x10, 0);
    W16(a2 + 0x14, 0);
    W16(a2 + 0x12, 1);
    W16(gChar, d4);
    W16(gCharX, R16(a2 + 2));
    W16(gCharType, 1);
    W16(gCharLife, 1);
    W16(gCharAlive, -1);
    W16(gCharField36, 0x30);
    FUN_00020cca();
    W16(gCharY, R16(gCharY) - 1);
    seg4_020fb2();
    W16(gCharField34, 0);
    W16(gCharOnRock, 0);
    W16(gCharField32, 0);
    JumpSeq(0xe7);
    AnimChar();
    AddSong(0xfe);
    seg4_022516();
    FUN_000225d2();
    return 0;
}

/* 0003d102: is one of the room's characters of type 1 (the shadow)? */
uint32_t IsAltarShadowActive(void) {
    int16_t d4 = 0;
    int16_t d5 = R16(R32(gGameState) + 0x23e6);
    if (d5 != 0) {
        int16_t d6 = 0;
        if (d5 > 0) {
            maddr a2 = gCharTab0;
            do {
                d4 = (R16(a2) == 1) ? 1 : 0;
                d6++;
                a2 += CHAR_SIZE;
            } while (d6 < d5 && d4 == 0);
        }
    }
    return (uint16_t)d4;
}

/* 0003d162: trob proc of the sacred flame (custom room template 0x20): next flame frame */
uint32_t FUN_0003d162(void) {
    ENTRY_NOLINK(FN_FUN_0003d162, 12);           /* movem D3-D5 */
    if (R32(gCustomRoom) == 0 || R16(R32(gCustomRoom)) != 0x20) {
        W16(gTrobDir, -1);
        return 0;
    }
    int16_t d4 = R16(gTrobState);
    int16_t d5;
    if ((R16(gTrobState) & 0x80) != 0) {
        if ((R16(gTrobState) & 0xf) == 0) {
            d4 = 0;
            d5 = 2;
        } else {
            d4 = (int16_t)(d4 - 1);
            uint16_t d3 = (uint16_t)(d4 & 0xf);
            d5 = R16(gFlameTable + (int16_t)((uint32_t)d3 << 1));
        }
    } else {
        if (RU16(gTrobState) < 0xb) d4 = (int16_t)(d4 + 1);
        else d4 = 0;
        d5 = (int16_t)(d4 + 2);
    }
    MarkBufsForRect(JT_ADDR(FN_MarkMove), ((uint32_t)(uint16_t)d5 << 5) + R32(gCustomRoom) + 0x2e);
    /* the original also pushes a word -1 */
    W16(gTrobState, d4);
    return 0;
}

/* 0003d1fa: piece proc of the sacred flame (kind 0xb): draws the custom room piece
 * (info & 0x80: through the frame table) with the full-screen clip */
uint32_t DrawTempSacredFlame(void) {
    ENTRY_LINK(FN_DrawTempSacredFlame, 8, 20);   /* link -8; movem D3-D6/A2 */
    uint32_t a2 = ITEM();
    if (R16(a2 + 0xa) != 0xb) return 0;
    copy8(a6 - 8, gDrawClip);
    copy8(gDrawClip, gScreenRect);
    uint16_t d5 = RU16(a2 + 0x14);
    if ((d5 & 0x80) != 0) {
        d5 &= 0xff7f;
        int16_t d6 = R16(gFlameTable + (int16_t)((uint32_t)d5 << 1));
        a2 = ((uint32_t)(uint16_t)d6 << 5) + R32(gCustomRoom) + 0x1e;
        W16(a2 + 6, 1);
        DrawCustomRoomImg(d6, 1);
        W16(a2 + 6, 0xb);
    } else {
        int16_t d6 = (int16_t)(d5 + 2);
        DrawCustomRoomImg(d6, 0xb);
    }
    copy8(gDrawClip, a6 - 8);
    return 0;
}

/* 0003d2b0: handle of flame image id (0xfed..0x101a), or NULL */
uint32_t GetFlameImgHnd(int16_t id) {
    if (R32(gFlameShapes) == 0 || id < 0xfed || id > 0x101a) return 0;
    int16_t i = (int16_t)(id - 0xfed);
    return R32((uint32_t)((int32_t)i * 4) + R32(gFlameShapes));
}

/* 0003d308: size {height, width} of flame image id into size; returns size, or 0 (also
 * stored into the original's size argument slot) */
uint32_t GetFlameImgSize(int16_t id, uint32_t size) {
    ENTRY_LINK(FN_GetFlameImgSize, 4, 16);       /* link -4; movem D3/A2-A4 */
    if (R32(gFlameShapes) != 0 && id >= 0xfed && id <= 0x101a) {
        uint32_t a3 = GetFlameImgHnd(id);
        if (a3 != 0) {
            uint32_t a4 = R32(a3);
            W16(size, R16(a4 + 6));
            W16(size + 2, R16(a4 + 4));
            return size;
        }
    }
    W32(ARG16(2), 0);
    return 0;
}

/* 0003d37c: temple overlay: load the flame shapes and palettes */
uint32_t FUN_0003d37c(void) {
    ENTRY_NOLINK(FN_FUN_0003d37c, 4);            /* D3 */
    LoadFlameShapes(1);
    W16(gFlamePalette, 0xe0);
    PoPSetPalette(0xfed, R16(gFlamePalette));
    int16_t d3 = (R16(gKidField34) == 0xd) ? 0x7d1 : 0x7d0;
    PoPSetPalette(d3, 0x30);
    return 0;
}

/* 0003d3be: clip rect of the flame object (gObjClip), from its image size; returns it */
uint32_t MakeFlameFCharClipRect(void) {
    ENTRY_LINK(FN_MakeFlameFCharClipRect, 4, 0); /* link -4 */
    if (GetFlameImgSize(R16(gObjShape), a6 - 4) != 0) {
        W16(gObjClip, (int16_t)(R16(gObjY) - R16(a6 - 4)));
        W16(gObjClip + 4, (int16_t)(R16(gObjY) + 1));
        if (R16(gObjFace) == 0) {
            W16(gObjClip + 6, R16(gObjX));
            W16(gObjClip + 2, (int16_t)(R16(gObjX) - R16(a6 - 2)));
        } else {
            W16(gObjClip + 2, R16(gObjX));
            W16(gObjClip + 6, (int16_t)(R16(gObjX) + R16(a6 - 2)));
        }
        SECTRECT(gScreenRect, gObjClip, gObjClip);
    } else {
        copy8(gObjClip, gEmptyRect);
    }
    return gObjClip;
}

/* 0003d45e: the flame of the flame sword (frames 0x132..0x13e) as a character object */
uint32_t SetupFlame(void) {
    ENTRY_LINK(FN_SetupFlame, 0x10, 4);          /* link -0x10; D3 */
    uint16_t f = RU16(gCharFrame);
    if (f < 0x132 || f > 0x13e) return 0;
    seg4_022442();
    W16(gObjShape, (int16_t)(R16(gSwordInfo) + 0xffc));
    AddFCharX(R16(gSwordInfo + 2));
    W16(gObjY, R16(gObjY) + R16(gSwordInfo + 4));
    W16(gObjKind, 4);
    W16(gObjPiece, R16(gFlamePalette));
    MakeFlameFCharClipRect();
    AddCharObj(5);
    seg4_022454();
    return 0;
}

/* 0003d4c8: temple overlay hook */
uint32_t seg10_03d4c8(void) {
    ENTRY_NOLINK(FN_seg10_03d4c8, 0);
    seg3_01d8ae();
    PoPSetPalette(0xbb8, 0xe0);
    return 0;
}
