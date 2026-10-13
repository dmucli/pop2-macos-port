/* CODE 23 "Rooftops" — level-specific code for the rooftop levels.
 * Ported from the Ghidra decompilation of the original 68k code; addresses in
 * comments are Ghidra addresses (see docs/PORTING.md). */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* Globals (Ghidra addresses) */
#define gGameState      GH(0x102fec)   /* long: pointer to the game state block */
#define gCurBFItem      GH(0x103074)   /* long: pointer to the background/foreground item being built */
#define gLevelPieces    GH(0x103462)   /* long: pointer to the level's piece table */
#define gCurRoom        GH(0x10303e)   /* word: current room */
#define gRoofPalette    GH(0x104e56)   /* word: which roof palette is active (0/1) */

#define LEVEL_TYPE()    R16(R32(gGameState) + 0x2186)

#define gCellRect       GH(0x103a00)   /* Rect: clip of the cell being built */
#define gColX           GH(0x10349a)   /* word[]: x of each column */
#define gDrawRow        GH(0x103a0a)   /* word: row being drawn (-1: the row above the room) */
#define gCellPiece      GH(0x1037ec)   /* word: piece of the cell above */
#define gScreenRect2    GH(0x103014)   /* Rect: the whole room */
#define gWholeRect      GH(0x106694)   /* Rect: the screen */
/* kid record (gKid, 0x3e bytes) and current character record (gChar) */
#define gKid            GH(0x102f2e)
#define gKidSeqFrame    GH(0x102f38)   /* word: kid frame */
#define gKidRow         GH(0x102f3c)   /* word: kid row */
#define gKidAction      GH(0x102f3e)   /* word: kid action */
#define gKidRoom        GH(0x102f44)   /* word: kid room */
#define gKidId          GH(0x102f56)   /* word: kid character id (0x44 ...) */
#define gLedgeClip      GH(0x102f58)   /* Rect */
#define gKidY2e         GH(0x102f5c)   /* word: kid record +2e (compared with a row's floor y) */
#define gChar           GH(0x102faa)   /* word +0: character number */
#define gCharFace       GH(0x102fac)
#define gCharX          GH(0x102fae)
#define gCharY          GH(0x102fb0)
#define gCharFrame      GH(0x102fb4)
#define CH_ROW          GH(0x102fb8)
#define gCharAction     GH(0x102fba)
#define gOppRoom        GH(0x102fc0)   /* word: room of the current character */
#define CH_LIFE         GH(0x102fc8)
#define gCharId         GH(0x102fd2)
#define gCharEnts_room  GH(0x102e0e)   /* opponent records (0x3e bytes), +0 room */
#define gCharImgW       GH(0x104a9e)
/* water splash below the roofs (rooms 0x13/0x10) */
#define gSplashChar     GH(0x104e58)   /* word: character that fell in the water (-1: none) */
#define gSplashFrame    GH(0x104e5a)   /* word: splash frame (0 none, 1..6) */
#define gSplashX        GH(0x104e5c)   /* word */
#define gSplashHigh     GH(0x104e5e)   /* word: 1 when the character fell from action 9 */
#define gSplashShapes   GH(0x104d72)   /* word[]: splash shape per frame */
#define gSplashYs       GH(0x104d80)   /* word[]: splash y per frame */
/* flying object (FChar) */
#define gFChar_kind     GH(0x104b38)
#define gObjA           GH(0x104b3a)
#define gObjX           GH(0x104b3c)
#define gFChar_y        GH(0x104b3e)
#define gObjShape       GH(0x104b40)
#define gFChar_set      GH(0x104b42)
#define gObjRect        GH(0x104b44)   /* Rect */
/* trobs and mobs */
#define gTrobCells      GH(0x1037dc)   /* long: trob cell specs */
#define gCellSpec       GH(0x1037e8)   /* word: spec of the trob being animated */
#define gTrobRoom       GH(0x103c14)
#define gTrobPhase      GH(0x103c16)
#define gPeelTab        GH(0x10307e)   /* peel records of 0x12 bytes */
#define gMob            GH(0x103b5a)   /* word: mob x */
#define gMobY           GH(0x103b5c)
#define gMobRoom        GH(0x103b5e)
#define gMobSpeed       GH(0x103b60)
#define gMobAccel       GH(0x103b62)
#define gMobType        GH(0x103b64)
#define gMobRow         GH(0x103b66)
#define gMobLanded      GH(0x103b68)   /* word: frame of the falling glass */
#define gMobRectW       GH(0x103b02)
#define gMobRectH       GH(0x103b1e)
#define gMidTable       GH(0x103356)   /* long: mid records of 0x18 bytes */
#define gMidCount       GH(0x1037d8)
/* rooftop object tables */
#define gGlassRect      GH(0x104c42)   /* trob move rect of the glass */
#define gPillarRect1    GH(0x104c22)
#define gPillarRect2    GH(0x104c2a)
#define gPillarRect4    GH(0x104c32)   /* offset rect of the pillar in front */
#define gPillarRect3    GH(0x104c3a)   /* offset rect of the pillar behind */
#define gGlassFrames    GH(0x104d00)   /* word[]: glass piece per trob frame */
#define gGlassOffs      GH(0x104d1e)   /* {word dy, word dx}[] per glass piece */
#define gGlassPath      GH(0x104d42)   /* {word y, word x}[] of the falling glass */
#define pUnionRect      GH(0x1060a4)
#define pEmptyRect      GH(0x1060a0)
#define pSectRect       GH(0x1060a8)

#define NORMAL_ROOF_ROOM(room)  (LEVEL_TYPE() == 5 && (room) != 0x13 && (room) != 0x10 && (room) != 0xf)

/* The original pushes an extra word (-1) for MarkBufsForRect: keep it on the stack. */
#define WITH_WORD(w, call) do { maddr xw_ = emu_sp() - 2; emu_set_sp(xw_); W16(xw_, (w)); call; emu_set_sp(xw_ + 2); } while (0)

/* Pascal rectangle routines through the trap pointers cached by FUN_0004ddfa */
static uint8_t sect_rect(maddr a, maddr b, maddr dst) {
    maddr sp0 = emu_sp();
    emu_set_sp(sp0 - 2);
    push32(a); push32(b); push32(dst);
    call68_addr(R32(pSectRect));
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

/* 0006de24 */
uint32_t DrawRoofBackWall(void) {
    ENTRY_LINK(FN_DrawRoofBackWall, 4, 16);      /* link -4; movem A2/D3-D5 */
    uint32_t item = R32(gCurBFItem);
    uint16_t shape = RU16(item + 0x16) & 0x3f;
    if (shape == 0) return 0;
    int16_t saved12 = R16(item + 0x12);
    W16(item + 0x12, 0);
    W16(item + 6, 0);
    int16_t room = R16(gCurRoom);
    if (LEVEL_TYPE() != 5 || room == 0x13 || room == 0x10 || room == 0xf) shape += 0x6c;
    uint32_t pieces = R32(gLevelPieces);
    W32(item + 0x18, R32(pieces + 0x2d2));
    W16(item + 0x1c, RU16(pieces + 0x2d6));
    room = R16(gCurRoom);
    if (LEVEL_TYPE() == 5 && room != 0x13 && room != 0x10 && room != 0xf &&
        R16(GH(0x104c4a) + (int16_t)shape * 2) != 0) {
        W16(R32(gCurBFItem) + 0x18, RU16(GH(0x104c4a) + (int16_t)shape * 2));
        if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(0);
        W16(R32(gCurBFItem) + 6, 0x10);
        item = R32(gCurBFItem); pieces = R32(gLevelPieces);
        W32(item + 0x18, R32(pieces + 0x2d2));
        W16(item + 0x1c, RU16(pieces + 0x2d6));
    }
    item = R32(gCurBFItem); pieces = R32(gLevelPieces);
    W32(item + 0x18, R32(pieces + 0x2d2));
    W16(item + 0x1c, RU16(pieces + 0x2d6));
    W16(R32(gCurBFItem) + 0x18, shape);
    if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(0);
    W16(R32(gCurBFItem) + 0x12, saved12);
    W16(R32(gCurBFItem) + 6, 0x10);
    return 0;
}

/* 0006dfae: add the floor-edge piece that matches the neighbouring cell */
uint32_t FUN_0006dfae(void) {
    ENTRY_NOLINK(FN_FUN_0006dfae, 12);           /* movem A2/D3/D4 */
    uint32_t item = R32(gCurBFItem);
    int16_t idx = (int16_t)(((RU16(item + 0x14) & 0xf00) >> 8) - 1);
    if (idx < 0) return 0;
    int16_t kind = R16(item + 10);
    int16_t want = R16(GH(0x104cea) + idx * 2);
    if ((kind == 5 && want == 5) || (kind == 1 && want == 1)) {
        W32(item + 0x18, R32(GH(0x104ca8) + idx * 6));
        W16(item + 0x1c, RU16(GH(0x104cac) + idx * 6));
        AddNextBFItemToTable();
    } else if (idx == 1 && R16(R32(gCurBFItem) + 10) == 1) {
        item = R32(gCurBFItem);
        W32(item + 0x18, R32(GH(0x104ca8)));
        W16(item + 0x1c, RU16(GH(0x104cac)));
        AddNextBFItemToTable();
    }
    return 0;
}

/* 0006df5e */
uint32_t FUN_0006df5e(void) {
    ENTRY_NOLINK(FN_FUN_0006df5e, 8);            /* movem A2/D3 */
    uint32_t item = R32(gCurBFItem);
    if (R16(item + 10) != 1) return 0;
    uint32_t pieces = R32(gLevelPieces);
    W32(item + 0x18, R32(pieces + 0x2de));
    W16(item + 0x1c, RU16(pieces + 0x2e2));
    item = R32(gCurBFItem);
    W16(item + 0x18, (RU16(item + 0x14) & 0x1f) + 0x45);
    if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(1);
    FUN_0006dfae();
    return 0;
}

/* 0006e068: piece proc of the rooftop floor (kind 0 back wall, 1 ledge, 5/2 floor) */
uint32_t DrawRoofFloor(void) {
    ENTRY_LINK(FN_DrawRoofFloor, 8, 24);                     /* link -8; movem D3-D6/A2/A3 */
    int16_t add = 0, shape = 0;
    W32(a6 - 8, R32(gCellRect));
    W32(a6 - 4, R32(gCellRect + 4));
    uint32_t item = R32(gCurBFItem);
    uint16_t kind = RU16(item + 0xa);
    if (kind == 0) {
        DrawRoofBackWall();
        if (!NORMAL_ROOF_ROOM(R16(gCurRoom))) {
            item = R32(gCurBFItem);
            if (RU16(item + 0x14) & 0xf) {
                mcopy(item + 0x18, R32(gLevelPieces) + 0x16, 6);
                W16(R32(gCurBFItem) + 0x18, 0x72);
                add = 1;
            }
        }
        goto done;
    }
    if (kind == 1) {
        if (NORMAL_ROOF_ROOM(R16(gCurRoom))) {
            mcopy(item + 0x18, R32(gLevelPieces) + 0x22, 6);
            shape = (int16_t)((RU16(R32(gCurBFItem) + 0x14) & 0xf) + 0x33);
            add = 1;
        } else {
            item = R32(gCurBFItem);
            if (RU16(item + 0x14) & 0xf) {
                shape = 0x71;
                mcopy(item + 0x18, R32(gLevelPieces) + 0x22, 6);
                add = 1;
            }
        }
        if (add) {
            W16(R32(gCurBFItem) + 0x18, shape);
            if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(1);
            add = 0;
        }
        if (!NORMAL_ROOF_ROOM(R16(gCurRoom))) goto done;
        item = R32(gCurBFItem);
        int16_t right = S16(PieceRightId(R16(item + 0xe), R16(item + 0x10)));
        if (S16(IsSpace(right)) == 0) goto done;
        item = R32(gCurBFItem);
        if (S16(DrawRoofLedgeInBack(shape, R16(item + 0xe), R16(item + 0x10))) != 0) goto done;
        W16(R32(gCurBFItem) + 0xa, 2);
    } else if (kind != 5 && kind != 2) {
        goto done;
    }
    /* kinds 5 and 2 (and a ledge without anything behind) */
    if (NORMAL_ROOF_ROOM(R16(gCurRoom))) {
        item = R32(gCurBFItem);
        int16_t right = S16(PieceRightId(R16(item + 0xe), R16(item + 0x10)));
        if (S16(IsSpace(right)) != 0) {
            W16(R32(gLevelPieces) + 0x1e, 0x33);
            shape = (int16_t)((RU16(R32(gCurBFItem) + 0x14) & 0xf) + 0x2f);
            if (shape > 0x32) shape = 0x32;
            mcopy(R32(gCurBFItem) + 0x18, R32(gLevelPieces) + 0x1c, 6);
            W16(R32(gCurBFItem) + 0x18, shape);
            add = 1;
        } else {
            FUN_0006dfae();
        }
    } else {
        W16(R32(gLevelPieces) + 0x1e, 0);
        mcopy(R32(gCurBFItem) + 0x18, R32(gLevelPieces) + 0x1c, 6);
        W16(R32(gCurBFItem) + 0x18, 0x70);
        add = 1;
    }
    if (add == 0) goto done;
    item = R32(gCurBFItem);
    if (R16(item + 0xa) == 5) {
        maddr prev = item;
        ResetNextBFItem(5);
        TransferBFItemMidFields(prev, R32(gCurBFItem));
        goto done;
    }
    shape = (int16_t)(R16(gColX + (int16_t)(R16(R32(gCurBFItem) + 0xe) * 2)) + 0x16);
    if (R16(gCurRoom) == 4) {
        shape -= 2;
    } else if (R16(gCurRoom) == 1 || R16(gCurRoom) == 3) {
        shape -= 3;
    } else if (R16(gCurRoom) == 0xa || R16(gCurRoom) == 0xb) {
        int16_t row = R16(R32(gCurBFItem) + 0x10);
        if (R16(gKidRow) != row || (int32_t)row * 0x78 + 0x6d < (int32_t)R16(gKidY2e)) shape -= 5;
    }
    if (R16(gCellRect + 6) > shape) {
        W16(gCellRect + 6, shape);
        if (empty_rect(gCellRect)) add = 0;
    }
done:
    if (add != 0) {
        if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo((int16_t)kind);
        W32(gCellRect, R32(a6 - 8));
        W32(gCellRect + 4, R32(a6 - 4));
        FUN_0006dfae();
    }
    return 0;
}

/* 0006e3c2: back wall behind a gap when the row above shows through */
uint32_t FUN_0006e3c2(void) {
    ENTRY_NOLINK(FN_FUN_0006e3c2, 12);                       /* movem A2/D4/D3 */
    uint32_t item = R32(gCurBFItem);
    if (R16(item + 0xa) == 0) {
        int16_t draw = 0;
        if (R16(item + 0x10) == R16(gDrawRow)) {
            draw = 1;
        } else if ((int32_t)R16(gDrawRow) + 1 == (int32_t)R16(R32(gCurBFItem) + 0x10)) {
            item = R32(gCurBFItem);
            draw = S16(VisibleAbovePiece(R16(item + 0xe), R16(item + 0x10)));
        }
        if (draw == 0 && R16(gDrawRow) == -1 && S16(IsSpace(R16(gCellPiece))) != 0) draw = 1;
        if (draw != 0) DrawRoofBackWall();
    }
    FUN_0006dfae();
    return 0;
}

/* 0006e434: is the ledge piece `piece` at (col, row + 1) inside gLedgeClip (only for
 * some kid positions)? */
uint32_t DrawRoofLedgeInBack(int16_t piece, int16_t col, int16_t row) {
    ENTRY_LINK(FN_DrawRoofLedgeInBack, 0xc, 12);             /* link -0xc; movem A2/D4/D3 */
    int16_t r = 0;
    if (R16(gKidId) != 0x44) {
        int16_t a = R16(gKidAction), p = R16(gKidSeqFrame);
        if (a == 3 || a == 4 || a == 6 || p == 0x5b || p == 0x50 || p == 0x51 || p == 0x88) {
            if (GetBGSetImgSize(piece, a6 - 4) != 0) {
                uint32_t pc = R32(gLevelPieces);
                int16_t y = R16(pc + 0x20);
                W16(a6 - 8, y);
                W16(a6 - 0xc, y - R16(a6 - 4));
                int16_t x = R16(pc + 0x1e);
                W16(a6 - 0xa, x);
                W16(a6 - 6, x + R16(a6 - 2));
                ComputeOffsetCellRect(a6 - 0xc, a6 - 0xc, col, (int16_t)(row + 1));
                r = sect_rect(gLedgeClip, a6 - 0xc, a6 - 0xc);
            }
        }
    }
    return (uint16_t)r;
}

/* 0006e508: reset the roof palette and the splash state */
uint32_t FUN_0006e508(void) {
    W16(gRoofPalette, -1);
    W16(gSplashFrame, 0);
    W16(gSplashChar, -1);
    return 0;
}

/* 0006e51e: a character falling into the water: start the splash, then after 7 frames
 * kill the kid if he is low enough */
uint32_t FUN_0006e51e(void) {
    ENTRY_NOLINK(FN_FUN_0006e51e, 8);                        /* movem D4/D3 */
    if (R16(gSplashChar) == -1) {
        int16_t lim = R16(gCharAction) == 9 ? 0x162 : 0x147;
        int16_t id = R16(gCharId);
        if (R16(gCharY) >= lim && id != 0x44 && id != 0xf && id != 0x3b && id != 0xa &&
            R16(gCharAction) != 2)
            FUN_0006e806();
    } else if (R16(gSplashChar) == R16(gChar)) {
        if (R16(gSplashFrame) <= 6) {
            W16(gSplashFrame, R16(gSplashFrame) + 1);
        } else {
            W16(gSplashFrame, 0);
            W16(gSplashChar, -1);
            if (R16(CH_ROW) >= 2) {
                W16(gCharFrame, 0xb9);
                DecLifePts(100);
                SetKidDeathMethod(0xf);
            }
        }
    }
    return 0;
}

/* 0006e5b8: switch between the two rooftop palettes depending on the room */
uint32_t RoofCheckPalette(int16_t room) {
    ENTRY_LINK(FN_RoofCheckPalette, 0, 8);       /* link 0; movem D3/D4 */
    int16_t sel = -1;
    if (LEVEL_TYPE() == 5 && room != 0x13 && room != 0x10 && room != 0xf) {
        if (R16(gRoofPalette) != 0) { W16(gRoofPalette, 0); sel = 0; }
    } else if (R16(gRoofPalette) != 1) {
        W16(gRoofPalette, 1); sel = 1;
    }
    if (sel != -1) PoPSetPalette((int16_t)(sel + 0xdac), 0x40);
    return 0;
}

/* 0006e63a */
uint32_t FUN_0006e63a(void) {
    ENTRY_NOLINK(FN_FUN_0006e63a, 0);
    RoofCheckPalette(R16(gCurRoom));
    return 0;
}

/* 0006e646: clip the splash object to the water line */
uint32_t seg23_06e646(void) {
    ENTRY_NOLINK(FN_seg23_06e646, 8);                        /* movem D5/D3 */
    int16_t room = R16(gOppRoom);
    if ((room == 0x13 || room == 0x10) && R16(gChar) == R16(gSplashChar)) {
        int16_t lim = R16(gCharAction) == 9 ? 0x162 : 0x147;
        int16_t y = R16(gObjRect + 4);
        if (y >= lim) y = lim;
        W16(gObjRect + 4, y);
        if (y < R16(gObjRect)) W16(gObjRect, y);
    }
    return 0;
}

/* 0006e694 */
uint32_t seg23_06e694(void) {
    ENTRY_NOLINK(FN_seg23_06e694, 0);
    FUN_0006e866();
    RoofWatchOpps();
    return 0;
}

/* 0006e69e: the kid walked past the end of the roof: he falls (draining his life) */
uint32_t seg23_06e69e(void) {
    ENTRY_NOLINK(FN_seg23_06e69e, 8);                        /* movem D4/D3 */
    if (R16(gCharX) <= 0x332) return 0;
    if (S16(PrinceSndOn(0x21)) == 0 && S16(PrinceSndOn(0x22)) == 0) {
        AddSound(0x22);
        return 0;
    }
    WaitSndId(0x263a, 1);
    SetTimer(0, 0, 0x1e);
    WaitTimer(0);
    PrinceSetSnd(0x21);
    int16_t n = R16(CH_LIFE);
    if (R16(CH_LIFE) > 0) {
        do {
            SetTimer(0, 0, 0x30);
            DecLifePts(1);
            mcopy(gKid, gChar, 0x3e);
            ChangeMeters();
            seg2_01585a();
            FUN_000225d2();
            WaitTimer(0);
            n--;
        } while (n > 0);
    }
    FUN_00020cca();
    JumpSeq(0x47);
    PrinceReleaseSnd(0x21);
    WaitSndId(0x2639, 1);
    SetKidDeathMethod(0xe);
    return 0;
}

/* 0006e788: rect of the splash (screen rect when there is none); returns r */
uint32_t RoofMakeWaterTestRect(uint32_t r) {
    if (R16(gSplashFrame) != 0 && R16(gSplashFrame) < 6) {
        W16(r, 0x147);
        if (R16(gSplashHigh) == 1) W16(r, R16(r) + 0x1b);
        W16(r + 2, R16(gSplashX) - 0x19);
        W16(r + 4, R16(r) + 1);
        W16(r + 6, R16(gSplashX) + 0x19);
    } else {
        W32(r, R32(gWholeRect));
        W32(r + 4, R32(gWholeRect + 4));
    }
    return r;
}

/* 0006e806: start a splash where the current character fell in */
uint32_t FUN_0006e806(void) {
    ENTRY_NOLINK(FN_FUN_0006e806, 0);
    if (R16(gSplashFrame) != 0) return 0;
    W16(gSplashChar, R16(gChar));
    W16(gSplashFrame, 0);
    W16(gSplashHigh, R16(gCharAction) == 9 ? 1 : 0);
    W16(gSplashX, R16(gCharX) - 0xcf);
    int16_t half = (int16_t)((int32_t)R16(gCharImgW) >> 1);
    if (R16(gCharFace) == 0) W16(gSplashX, R16(gSplashX) - half);
    else W16(gSplashX, R16(gSplashX) + half);
    AddSound(0x23);
    return 0;
}

/* 0006e866: the kid in the ship rooms (water, climbing into the ship) */
uint32_t FUN_0006e866(void) {
    ENTRY_NOLINK(FN_FUN_0006e866, 8);                        /* movem D4/D3 */
    int16_t room = R16(gKidRoom);
    if (room == 0x13 || room == 0x10 || R16(gSplashChar) == 0xa) {
        FUN_000225d2();
        FUN_0006e51e();
        if (R16(gKidRoom) == 0x13 &&
            (S16(RoofHangingFromShip()) != 0 || S16(RoofClimbingIntoShip()) != 0)) {
            W16(gCharX, R16(gCharX) - 2);
            if (S16(RoofClimbingIntoShip()) != 0 && R16(gCharFrame) == 0x95) FUN_00028cc0();
        }
        mcopy(gKid, gChar, 0x3e);
    }
    return 0;
}

/* 0006e8d2: splash handling for the opponents of the current room */
uint32_t RoofWatchOpps(void) {
    ENTRY_LINK(FN_RoofWatchOpps, 4, 16);                     /* link -4; movem D3-D5/A2 */
    uint16_t room = RU16(gCurRoom);
    int16_t n = room != 0 ? R16(R32(gGameState) + (uint32_t)room * 0xc0 + 0x20e6) : 0;
    if (n != 0) {
        int16_t i = 0;
        if (n > 0) {
            maddr e = gCharEnts_room;
            do {
                int16_t r = R16(e);
                if (r == 0x13 || r == 0x10 || R16(gSplashChar) == i) {
                    LoadShad(i);
                    FUN_0006e51e();
                    seg4_022516();
                }
                i++;
                e += 0x3e;
            } while (i < n);
        }
    }
    return 0;
}

/* 0006e95e: set up the splash object */
uint32_t SetupSplash(void) {
    ENTRY_LINK(FN_SetupSplash, 4, 8);                        /* link -4; movem D4/D3 */
    if (R16(gSplashChar) != R16(gChar) || R16(gSplashFrame) >= 6) return 0;
    if (R16(gOppRoom) != 0x13 && R16(gOppRoom) != 0x10) return 0;
    seg4_022442();
    W16(gFChar_kind, -1);
    W16(gFChar_set, 4);
    int16_t shape = (int16_t)(R16(gSplashShapes + (int16_t)(R16(gSplashFrame) * 2)) - 1);
    W16(gObjShape, shape);
    if (GetBGSetImgSize(shape, a6 - 4) != 0) {
        W16(gFChar_y, R16(gSplashYs + (int16_t)(R16(gSplashFrame) * 2)) + 0x14b);
        if (R16(gCharAction) == 9) W16(gFChar_y, R16(gFChar_y) + 0x1b);
        int16_t w = R16(a6 - 2);
        W16(gObjX, R16(gSplashX) - (int16_t)((int32_t)w >> 1));
        if (R16(gObjA) == 0) W16(gObjX, R16(gObjX) + w);
        MakeFCharClipRect();
        AddCharObj(5);
        if (R16(gCharAction) != 9) WITH_WORD(-1, MarkBufsForRect(JT_ADDR(FN_MarkFRedraw), gObjRect));
    }
    seg4_022454();
    return 0;
}

/* 0006ea50: trob animation of the glass (14 frames) */
uint32_t AnimRoofGlass(void) {
    ENTRY_LINK(FN_AnimRoofGlass, 8, 0);                      /* link -8 */
    if (RU16(gCellSpec) >= 0xe) {
        W16(gTrobPhase, -1);
    } else {
        W16(gCellSpec, R16(gCellSpec) + 1);
        if (ComputeTrobMoveRect(a6 - 8, gGlassRect) != 0)
            WITH_WORD(-1, MarkBufsForRect(JT_ADDR(FN_MarkMove), a6 - 8));
    }
    return 0;
}

/* 0006eaa4: piece proc of the glass */
uint32_t DrawRoofGlass(void) {
    ENTRY_LINK(FN_DrawRoofGlass, 4, 16);                     /* link -4; movem A2/D3-D5 */
    uint32_t item = R32(gCurBFItem);
    if (R16(item + 0xa) == 0) {
        DrawRoofBackWall();
    } else if (R16(item + 0xa) == 5) {
        uint16_t s = RU16(item + 0x14);
        int16_t f;
        if (s < 0xe) f = R16(gGlassFrames + (uint32_t)(s & 0xf) * 2);
        else f = 8;
        int16_t k = (int16_t)(f * 4);
        W16(R32(gCurBFItem) + 0x1a, R16(gGlassOffs + 2 + k));
        W16(R32(gCurBFItem) + 0x1c, R16(gGlassOffs + k));
        W16(R32(gCurBFItem) + 0x18, f + 0x58);
        AddNextBFItemToTable();
    }
    return 0;
}

/* 0006eb3c: trob animation of the pillar (water running down, 4 frames) */
uint32_t AnimRoofPillar(void) {
    ENTRY_LINK(FN_AnimRoofPillar, 0x10, 4);                  /* link -0x10; D3 */
    if (S16(FUN_00025362()) == 0) return 0;
    int16_t f = R16(gCellSpec) & 7;
    f = f == 3 ? 0 : f + 1;
    W16(gCellSpec, R16(gCellSpec) & ~7);
    W16(gCellSpec, R16(gCellSpec) | f);
    RoofMakeWaterTestRect(a6 - 0x10);
    ComputeTrobMoveRect(a6 - 8, gPillarRect1);
    if (R16(gSplashFrame) == 0 || !sect_rect(a6 - 8, a6 - 0x10, a6 - 0x10))
        WITH_WORD(-1, MarkBufsForRect(JT_ADDR(FN_MarkMove), a6 - 8));
    ComputeTrobMoveRect(a6 - 8, gPillarRect2);
    WITH_WORD(-1, MarkBufsForRect(JT_ADDR(FN_MarkMove), a6 - 8));
    return 0;
}

/* 0006ebee: piece proc of the pillar */
uint32_t DrawRoofPillar(void) {
    ENTRY_LINK(FN_DrawRoofPillar, 0x10, 12);                 /* link -0x10; movem A2/D4/D3 */
    int16_t add = 0;
    W32(a6 - 8, R32(gCellRect));
    W32(a6 - 4, R32(gCellRect + 4));
    uint32_t item = R32(gCurBFItem);
    uint16_t kind = RU16(item + 0xa);
    if (kind == 0) {
        DrawRoofBackWall();
        mcopy(R32(gCurBFItem) + 0x18, R32(gLevelPieces) + 0x3ae, 6);
        W16(R32(gCurBFItem) + 0x18, 0x74);
        if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(0);
        item = R32(gCurBFItem);
        ComputeOffsetCellRect(a6 - 0x10, gPillarRect3, R16(item + 0xe), R16(item + 0x10));
        union_rect(a6 - 0x10, gCellRect, gCellRect);
        mcopy(R32(gCurBFItem) + 0x18, R32(gLevelPieces) + 0x39a, 6);
        int16_t s = (int16_t)((R16(R32(gCurBFItem) + 0x14) & 7) + 0x77);
        if (s > 0x78) s -= 4;
        W16(R32(gCurBFItem) + 0x18, s);
        W16(R32(gCurBFItem) + 6, 0);
        add = 1;
    } else if (kind == 5) {
        item = R32(gCurBFItem);
        ComputeOffsetCellRect(a6 - 0x10, gPillarRect4, R16(item + 0xe), R16(item + 0x10));
        union_rect(a6 - 0x10, gCellRect, gCellRect);
        item = R32(gCurBFItem);
        if (S16(DrawPillarInBack(R16(item + 0xe), R16(item + 0x10))) == 0) {
            maddr prev = R32(gCurBFItem);
            ResetNextBFItem(1);
            TransferBFItemMidFields(prev, R32(gCurBFItem));
        }
        mcopy(R32(gCurBFItem) + 0x18, R32(gLevelPieces) + 0x3a0, 6);
        item = R32(gCurBFItem);
        W16(item + 0x18, (R16(item + 0x14) & 7) + 0x75);
        W16(R32(gCurBFItem) + 6, 0);
        add = 1;
    } else if (kind == 1) {
        mcopy(item + 0x18, R32(gLevelPieces) + 0x3ba, 6);
        W16(R32(gCurBFItem) + 0x18, 0x73);
        add = 1;
    }
    if (add != 0) {
        if (S16(AddNextBFItemToTable()) == 0) W16(R32(gCurBFItem) + 6, 0x10);
        W32(gCellRect, R32(a6 - 8));
        W32(gCellRect + 4, R32(a6 - 4));
    }
    return 0;
}

/* 0006edb6: does the pillar at (col, row) touch the splash? */
uint32_t DrawPillarInBack(int16_t col, int16_t row) {
    ENTRY_LINK(FN_DrawPillarInBack, 0x10, 4);                /* link -0x10; D3 */
    int16_t r = 0;
    if (R16(gSplashFrame) != 0 && R16(gSplashHigh) == 1 &&
        ComputeOffsetCellRect(a6 - 8, gPillarRect4, col, row) != 0) {
        RoofMakeWaterTestRect(a6 - 0x10);
        if (sect_rect(a6 - 8, a6 - 0x10, a6 - 0x10)) r = 1;
    }
    return (uint16_t)r;
}

/* 0006ee28: start the pillar animation at a random frame */
uint32_t TriggerRoofPillar(int16_t room, int16_t cell) {
    ENTRY_LINK(FN_TriggerRoofPillar, 4, 12);                 /* link -4; movem A2/D4/D3 */
    int16_t f = S16(Rnd(3));
    maddr c = R32(gTrobCells) + (int32_t)cell * 4;
    W16(c, (int16_t)((R16(c) & ~7) + f));
    AddTrob(room, cell, 1, 0x2f);
    return 0;
}

/* 0006ee86: trob animation of the ship (0x62 frames) */
uint32_t AnimRoofShip(void) {
    ENTRY_LINK(FN_AnimRoofShip, 0xc, 12);                    /* link -0xc; movem A2/D4/D3 */
    W16(gCellSpec, R16(gCellSpec) + 1);
    if (RU16(gCellSpec) > 0x62) {
        W16(gTrobPhase, -1);
        return 0;
    }
    if (R16(gCurRoom) == R16(gTrobRoom)) {
        _SetRect(a6 - 0xc, 0, 6, 1, 7);
        WITH_WORD(-1, MarkBufsForRect(JT_ADDR(FN_MarkMove), a6 - 0xc));
        int16_t k = S16(GetPeelIdxForCharIdx(0x142, 1));
        if (k != -1) {
            maddr p = gPeelTab + (int32_t)k * 0x12;
            W16(p + 0x10, 0);
            if (R16(p + 0xa) > 2) W16(p + 0xa, R16(p + 0xa) - 2);
            else W16(p + 0xe, 0);
        }
    }
    return 0;
}

/* 0006ef40: piece proc of the ship (three parts, moving with the frame) */
uint32_t DrawRoofShip(void) {
    ENTRY_LINK(FN_DrawRoofShip, 0xc, 20);                    /* link -0xc; movem A2/A3/D3-D5 */
    uint32_t item = R32(gCurBFItem);
    if (R16(item + 0xa) == 0) {
        DrawRoofBackWall();
        return 0;
    }
    if (R16(item + 0xa) != 5 || R16(item + 0xe) != 0 || R16(item + 0x10) != 0 || RU16(item + 0x14) > 0x62)
        return 0;
    W32(a6 - 0xc, R32(gCellRect));
    W32(a6 - 8, R32(gCellRect + 4));
    W32(gCellRect, R32(gScreenRect2));
    W32(gCellRect + 4, R32(gScreenRect2 + 4));
    int16_t dx = (int16_t)(-R16(item + 0x14) * 2);
    mcopy(item + 0x18, R32(gLevelPieces) + 0x3c2, 6);
    W16(R32(gCurBFItem) + 0x1a, dx);
    W16(R32(gCurBFItem) + 0x18, 0x7a);
    maddr prev = R32(gCurBFItem);
    AddNextBFItemToTable();
    ResetNextBFItem(1);
    TransferBFItemMidFields(prev, R32(gCurBFItem));
    W16(R32(gCurBFItem) + 0x18, 0x7b);
    W16(R32(gCurBFItem) + 0x1a, dx + 0x48);
    W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + 0x3cc));
    if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(1);
    mcopy(R32(gCurBFItem) + 0x18, R32(gLevelPieces) + 0x3ce, 6);
    W16(R32(gCurBFItem) + 0x1a, dx);
    W16(R32(gCurBFItem) + 0x18, ((-dx) & 3) + 0x7c);
    AddNextBFItemToTable();
    W32(gCellRect, R32(a6 - 0xc));
    W32(gCellRect + 4, R32(a6 - 8));
    return 0;
}

/* 0006f076: start the ship (from frame 6 when the kid is in frames 0x22..0x28) */
uint32_t TriggerRoofShip(int16_t room, int16_t cell) {
    ENTRY_LINK(FN_TriggerRoofShip, 0, 16);                   /* link 0; movem A2/D3-D5 */
    maddr c = R32(gTrobCells) + (int32_t)cell * 4;
    if (RU16(c) < 0x62) {
        uint16_t f = RU16(gKidSeqFrame);
        if (f >= 0x22 && f <= 0x28) W16(c, 6);
        AddTrob(room, cell, 1, 0x30);
    }
    return 0;
}

/* 0006f0da */
uint32_t RoofClimbingIntoShip(void) {
    return (R16(gOppRoom) == 0x13 && R16(gCharId) == 0x3b) ? 1 : 0;
}

/* 0006f110 */
uint32_t FUN_0006f110(void) {
    int16_t r = 0;
    if (R16(gOppRoom) == 0x13 && R16(CH_ROW) < 3) {
        int16_t v = R16(R32(gGameState) + 0xff0);
        r = (R16(gCharX) < 0x129 && v < 0x21) ? 1 : 0;
        return (uint16_t)r;
    }
    return (cpu_get(R_D0) & 0xffff0000u) | (uint16_t)r;
}

/* 0006f14c */
uint32_t RoofHangingFromShip(void) {
    if (R16(gOppRoom) == 0x13 && R16(gCharX) < 0x129) {
        int16_t a = R16(gCharAction);
        if (a == 2 || a == 6 || R16(gCharFrame) == 0x50) return 1;
    }
    return 0;
}

/* 0006f19e: animation of the falling glass mob */
uint32_t ATMGlass(void) {
    ENTRY_LINK(FN_ATMGlass, 4, 12);                          /* link -4; movem D5/D4/D3 */
    if (R16(gMobRoom) != R16(gCurRoom) || RU16(gMobLanded) > 0xb) {
        W16(gMobSpeed, -1);
        return 0;
    }
    int16_t col = (int16_t)((int32_t)R16(gMob) / 0x33);
    int16_t row = S16(GetRow(R16(gMobY)));
    W16(gFChar_kind, IndexCell1(col, row));
    int16_t shape = (int16_t)(R16(gMobLanded) + 0x61);
    if (GetBGSetImgSize(shape, a6 - 4) != 0) {
        W16(gMobRectW, R16(a6 - 4));
        W16(gMobRectH, R16(a6 - 2));
        AddMobObj(R16(gMobY));
        MarkMobRedraw(1);
    }
    return 0;
}

/* 0006f236: draw the glass object */
uint32_t DrawObjGlass(void) {
    ENTRY_LINK(FN_DrawObjGlass, 0, 4);                       /* link 0; D3 */
    int16_t shape = (int16_t)(R16(gObjShape) + 0x61);
    if (S16(AddMid(4, shape, R16(gObjX), R16(gFChar_y), 0x10)) != 0)
        W16(R32(gMidTable) + (int32_t)R16(gMidCount) * 0x18 - 4, 0);
    return 0;
}

/* 0006f2a0: next frame of the falling glass */
uint32_t FUN_0006f2a0(void) {
    if (R16(gMobSpeed) >= 0) {
        W16(gMobLanded, R16(gMobLanded) + 1);
        W16(gMob, R16(gGlassPath + 2 + (int16_t)(RU16(gMobLanded) * 4)));
        W16(gMobY, R16(gGlassPath + (int16_t)(RU16(gMobLanded) * 4)));
    }
    return 0;
}

/* 0006f2e0: the glass falls when the kid is on the top row */
uint32_t TriggerGlass(int16_t room, int16_t cell) {
    ENTRY_LINK(FN_TriggerGlass, 0, 8);                       /* link 0; movem D4/D3 */
    if (R16(gKidRow) != 0) return 0;
    AddTrob(room, cell, 1, 0x31);
    W16(gMob, R16(gGlassPath + 2));
    W16(gMobY, R16(gGlassPath));
    W16(gMobRoom, R16(gCurRoom));
    W16(gMobSpeed, 0);
    W16(gMobAccel, 0);
    W16(gMobType, 0xd);
    W16(gMobRow, GetRow(R16(gMobY)));
    W16(gMobLanded, 0);
    FUN_00026a94();
    AddSound(0x24);
    for (uint16_t id = 0xe04; id <= 0xe18; id++) PoPGetResource(0x53484150, (int16_t)id);   /* 'SHAP' */
    return 0;
}
