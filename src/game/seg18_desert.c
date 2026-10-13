/* CODE 18 "Desert" (Ghidra 0x554e0-0x563b4): level-type code of the desert levels:
 * the sand storm color cycling, the stepping stones (marked by the kid, they sink
 * and come back up), the exit and its music, the planks and rocks, the sand
 * swirling around the kid. Ported from the disassembly of the original 68k code
 * (Ghidra addresses in comments, see docs/PORTING.md). */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* ---- character records (0x3e bytes, layout of docs/notes/seg04a.md) ---- */
#define REC_SIZE    0x3e
#define gChar       GH(0x102faa)   /* the character being processed */
#define gKid        GH(0x102f2e)   /* saved kid record */
#define CH(f)       (gChar + (f))
#define KID(f)      (gKid + (f))
#define F_FACE    0x02
#define F_X       0x04
#define F_FRAME   0x0a
#define F_COL     0x0c
#define F_ROOM    0x16
#define F_ALIVE   0x1c
#define F_SEQ     0x28
#define F_RECT    0x2a
#define F_34      0x34

/* ---- globals (Ghidra addresses) ---- */
#define gGameState    GH(0x102fec)  /* long: pointer to the game state block */
#define GS()          R32(gGameState)
#define gCurRoom      GH(0x10303e)  /* word */
#define gNextLevel    GH(0x10306e)  /* word */
#define gCurBFItem    GH(0x103074)  /* long: BF item being built */
#define gCustRoom     GH(0x103362)  /* long: locked custom-room data (+0 template id,
                                       +2 shape base, pieces of 0x20 bytes from +0x1e) */
#define gTrobState    GH(0x1037e8)  /* word: state of the trob being animated */
#define gTrobCell     GH(0x103c12)  /* word: cell of the trob being animated */
#define gTrobDir      GH(0x103c16)  /* word: direction; -1 ends the trob */
#define gCellRect     GH(0x103a00)  /* Rect: rect of the cell being drawn */
#define gCellCol      GH(0x103a08)  /* word */
#define gGameRect     GH(0x103014)  /* Rect */
#define gWorldPal     GH(0x102dbe)  /* long */
#define gFrameCount   GH(0x104b66)  /* word: frame counter */
#define gNoNewTrobs   GH(0x102ffe)  /* word */
#define gSandSong     GH(0x104ba2)  /* word: the desert song has been started */
#define gStoneSet     GH(0x104b9c)  /* word: stone shape set (-1 = not chosen yet) */
#define gStoneIdx     GH(0x104b9e)  /* word: index of the special stone (1-5) */
#define gExitOpened   GH(0x104ba0)  /* word */
#define gLastMarkCol  GH(0x104c20)  /* word: last stone column marked by the kid (-1) */
#define gBGSetTable   GH(0x103578)  /* long */
#define gSoundReq     GH(0x103c62)  /* word: sound request */
#define gAmbientLast  GH(0x1048cc)  /* word: last ambient sound */
#define gPeelTab      GH(0x10307e)  /* peels, 0x12 bytes each (+0x10 keep flag) */
#define gFCharImg     GH(0x104b48)  /* word: FChar image */
#define gFCharImgMin  GH(0x104b44)  /* word */

#define STONE_STATE(room, cell) \
    (GS() + 0x780 + (uint32_t)(uint16_t)(room) * 0x78 - 0x78 + (int32_t)(cell) * 4)

static void copy_rect(maddr dst, maddr src) { mcopy(dst, src, 8); }

/* Callers of MarkBufsForRect push a third word (-1) that it never reads: push it too so
 * that its frame is where the original's is. */
static void mark_bufs_for_rect(uint32_t fn, uint32_t rect) {
    maddr sp = emu_sp() - 2;
    emu_set_sp(sp);
    W16(sp, -1);
    MarkBufsForRect(fn, rect);
    emu_set_sp(sp + 2);
}

/* SectRect(src1, src2, VAR dst): Boolean through the pointer cached at A5-0x1f58 */
static uint8_t sect_rect_ptr(maddr src1, maddr src2, maddr dst) {
    maddr sp0 = emu_sp();
    push16(0);
    push32(src1);
    push32(src2);
    push32(dst);
    call68_addr(R32(GH(0x1060a8)));
    uint8_t r = R8(sp0 - 2);
    emu_set_sp(sp0);
    return r;
}

/* 000554e4: start the desert song */
uint32_t FUN_000554e4(void) {
    ENTRY_NOLINK(FN_FUN_000554e4, 4);            /* move.l D3 */
    if (S16(PrinceSndOn(0x32)) == 0) AddSong(0x32);
    W16(gSandSong, 1);
    return 0;
}

/* 00055506: clip the FChar image at 0xf0 */
uint32_t seg18_055506(void) {
    int16_t v = R16(gFCharImg);
    int32_t r = v < 0xf0 ? (int32_t)v : 0xf0;
    W16(gFCharImg, r);
    if ((int16_t)r < R16(gFCharImgMin)) W16(gFCharImgMin, r);
    return (uint32_t)r;
}

/* 00055530: the exit of room 1: open it when every stone is down except the special
 * one, with music, sound and the special stone sinking on the way */
uint32_t DsrtCheckExit(void) {
    ENTRY_LINK(FN_DsrtCheckExit, 4, 20);         /* link -4; movem D3-D7 */
    uint16_t st = RU16(GS() + 0x7a8);
    if (st & 0x1f) return 0;
    uint16_t step = (uint16_t)((st & 0x1f00) >> 8);
    if (R16(gExitOpened) != 0) return 0;
    W16(a6 - 4, ((int32_t)R16(gStoneIdx) + 2 == (int32_t)R16(gLastMarkCol)) ? 0 : 1);
    if (R16(a6 - 4) == 0) W16(gExitOpened, 1);
    int16_t i = 0;
    if (R16(a6 - 4) != 0) {
        do {
            uint16_t s = RU16(GS() + (int32_t)i * 4 + 0x7b0) & 0xf;
            if ((s != 0 && i != R16(gStoneIdx)) || (s == 0 && i == R16(gStoneIdx) && step < 10))
                W16(a6 - 4, 0);
            i++;
        } while (R16(a6 - 4) != 0 && (uint16_t)i < 6);
    }
    if (R16(a6 - 4) != 0) {
        if (step >= 0x14) {
            TriggerDsrtExit(1, 10);
        } else {
            if (step == 6) AddSong(0x2e);
            else if (step == 9) AddSound(0x37);
            else if (step == 10) SinkDsrtStone((int16_t)(R16(gStoneIdx) + 0xc));
            step++;
            step = (uint16_t)(step << 8);
            st = (uint16_t)((st & 0xe0ff) | step);
        }
    } else {
        st &= 0xe0ff;
    }
    W16(GS() + 0x7a8, st);
    return 0;
}

/* 00055660: sand storm: cycle the sand colors every third frame */
uint32_t FUN_00055660(void) {
    ENTRY_NOLINK(FN_FUN_00055660, 0);
    if (R16(gNoNewTrobs) != 0) return 0;
    if (R16(gCurRoom) == 1) return 0;
    if (RU16(gFrameCount) % 3 != 0) return 0;
    CycleColors(0xde, 7);
    CycleColors(0xe5, 0xb);
    CycleColors(0xf0, 5);
    ShowColors(R32(gWorldPal), 0xde, 0x17);
    return 0;
}

/* 000556c0: per-frame hook of the desert levels: wind sound, exit, music */
uint32_t seg18_0556c0(void) {
    ENTRY_NOLINK(FN_seg18_0556c0, 4);            /* move.l D3 */
    FUN_00055660();
    FUN_00055760();
    DsrtCheckExit();
    int16_t music;
    if (R16(gCurRoom) != 1) {
        if (S16(PrinceSndOn(0x33)) == 0) AddSound(0x33);
        music = R16(gSandSong);
    } else if (S16(PrinceSndOn(0x32)) != 0) {
        if (S16(PrinceSndOn(0x33)) != 0) PrinceResetSnd(0x33);
        music = 0;
    } else {
        music = 1;
    }
    if (music != 0 && R16(KID(F_34)) != 7 && R16(KID(F_ALIVE)) < 0 &&
        S16(PrinceSndOn(R16(gAmbientLast))) == 0 && GetTrobId(0x1e, 1) == 0 &&
        S16(FUN_00055c96()) == 0)
        FUN_000554e4();
    return 0;
}

/* 00055760: the kid in room 1 (stones, exit to the left) and room 3 (right edge) */
uint32_t FUN_00055760(void) {
    ENTRY_NOLINK(FN_FUN_00055760, 4);            /* move.l D3 */
    FUN_000225d2();
    if (R16(CH(F_ROOM)) == 1) {
        if (R16(CH(F_COL)) != R16(gLastMarkCol) && S16(IsMarkStonePos(R16(CH(F_FRAME)))) != 0) {
            int16_t c = R16(gLastMarkCol);
            if (c >= 2 && c <= 7 && S16(SinkDsrtStone((int16_t)(c + 10))) != 0 &&
                (int32_t)R16(gStoneIdx) + 2 == (int32_t)R16(gLastMarkCol))
                AddSong(0x31);
        }
        if (S16(IsMarkStonePos(R16(CH(F_FRAME)))) != 0) W16(gLastMarkCol, RU16(CH(F_COL)));
        if (R16(CH(F_X)) < 0xcf) {
            if (R16(gNextLevel) == R16(GS() + 0x218a)) {
                W16(gNextLevel, R16(gNextLevel) + 1);
                JumpSeq(2);
            } else {
                JumpSeq(2);
                W16(gSoundReq, -1);
            }
        }
    } else if (R16(CH(F_ROOM)) == 3 && R16(CH(F_X)) >= 0x30b) {
        if ((RU16(GH(0x104b7c)) | RU16(GH(0x104b7e))) != 0 ||
            (R16(CH(F_FACE)) == 0 && R16(GH(0x104b78)) != 0)) {
            JumpSeq(2);
            FUN_00020cca();
        }
        if (S16(PrinceSndOn(0x38)) == 0 && (R16(GH(0x104b80)) < 0 || S16(Rnd(0x14)) == 0))
            AddSound(0x38);
    }
    mcopy(gKid, gChar, REC_SIZE);
    return 0;
}

/* 0005588a: forget the stone set (chosen again at the next level load) */
uint32_t seg18_05588a(void) {
    W16(gStoneSet, -1);
    return 0xffffffffu;
}

/* 00055892: frames in which the kid marks the stone he stands on */
uint32_t IsMarkStonePos(int16_t f) {
    if (f == 0xf || f == 0x6d || f == 7 || f == 0x2e) return 1;
    if (f >= 0xb && f <= 0xe) return 1;
    if (f == 0x26 || f == 0x2c || f == 0x1a) return 1;
    if (f == 0x9e || f == 0xaa || f == 0xab) return 1;
    return 0;
}

/* 0005591c: level load hook: choose the stone set and load its three shapes */
uint32_t FUN_0005591c(void) {
    ENTRY_NOLINK(FN_FUN_0005591c, 16);           /* movem D3-D4/A2-A3 */
    W16(gLastMarkCol, -1);
    W16(gExitOpened, 0);
    if (R32(gCustRoom) == 0) return 0;
    if (R16(R32(gCustRoom)) != 6) return 0;
    if (R16(gStoneSet) == -1) {
        W16(gStoneSet, Rnd(2));
        W16(gStoneIdx, (int16_t)(S16(Rnd(4)) + 1));
    }
    if (R16(gStoneSet) == 1) return 0;
    _HLock(R32(R32(gBGSetTable)));
    uint32_t piece = R32(gCustRoom) + 0x21e;
    _HPurge(R32(piece + 0xc));
    int16_t shape = (int16_t)(R16(R32(gCustRoom) + 2) + R16(gStoneSet) + 0x10);
    uint32_t h = GetShape(shape);
    W32(piece + 0xc, h);
    _HNoPurge(h);
    piece = R32(gCustRoom) + 0x25e;
    _HPurge(R32(piece + 0xc));
    shape += 3;
    h = GetShape(shape);
    W32(piece + 0xc, h);
    _HNoPurge(h);
    piece = R32(gCustRoom) + 0x23e;
    _HPurge(R32(piece + 0xc));
    shape += 3;
    h = GetShape(shape);
    W32(piece + 0xc, h);
    _HNoPurge(h);
    _HUnlock(R32(R32(gBGSetTable)));
    return 0;
}

/* 000559fe: sand swirling around the kid (sequence 0x1b): draw custom piece 4/5 */
uint32_t SetupSand(void) {
    ENTRY_LINK(FN_SetupSand, 0x14, 20);          /* link -0x14; movem D3-D4/A2-A4 */
    if (R16(CH(F_SEQ)) != 0x1b) return 0;
    if (R16(CH(F_RECT)) >= 0xe3) return 0;
    copy_rect(a6 - 0x14, gCellRect);
    int16_t n = (int16_t)((R16(gFrameCount) & 1) + 4);
    uint32_t piece = R32(gCustRoom) + 0x1e + (uint32_t)(uint16_t)n * 32;
    W32(a6 - 0xc, R32(piece + 2));
    copy_rect(a6 - 8, piece + 0x10);
    W16(piece + 2, 0xf3);
    W16(piece + 4, RU16(GH(0x104b3c)));
    if (R16(GH(0x104b3a)) == 0) W16(piece + 4, R16(piece + 4) - R16(GH(0x104a9e)));
    uint32_t shp = R32(R32(piece + 0xc));
    int16_t d = (int16_t)((int16_t)((int32_t)R16(GH(0x104a9e)) >> 1) -
                          (int16_t)((int32_t)R16(shp + 4) >> 1));
    W16(piece + 4, R16(piece + 4) + d);
    int16_t x = R16(piece + 4);
    int16_t y = (int16_t)(R16(piece + 2) - R16(shp + 6));
    uint32_t rect = piece + 0x10;
    MakeShapeRect(rect, y, x, shp);
    mark_bufs_for_rect(JT_ADDR(FN_MarkMove), rect);
    copy_rect(gCellRect, piece + 0x10);
    W16(piece + 6, 1);
    DrawCustomRoomImg(n, 1);
    if (R32(gCurBFItem) == 0) {
        int16_t v = (R16(gFrameCount) & 2) == 0 ? 1 : 0;
        W16(R32(GH(0x10335a)) + (int32_t)R16(GH(0x1037d4)) * 0x26 - 0x1e, v);
    }
    W16(piece + 6, 0xb);
    W32(piece + 2, R32(a6 - 0xc));
    copy_rect(piece + 0x10, a6 - 8);
    copy_rect(gCellRect, a6 - 0x14);
    return 0;
}

/* 00055b5c: animate the exit gate opening (custom piece 2 moves up, 20 steps) */
uint32_t FUN_00055b5c(void) {
    ENTRY_NOLINK(FN_FUN_00055b5c, 12);           /* movem D3/A2-A3 */
    uint16_t n = RU16(gTrobState) & 0x1f;
    if (n == 0x14) {
        W16(gTrobDir, -1);
        PrinceResetSnd(0x11);
    } else {
        n++;
        if (S16(PrinceSndOn(0x37)) == 0) AddSound(0x11);
    }
    W16(gTrobState, RU16(gTrobState) & 0xffe0);
    W16(gTrobState, RU16(gTrobState) | n);
    uint32_t piece = R32(gCustRoom) + 0x5e;
    W16(piece + 0x12, R16(piece + 0x12) - 3);
    uint32_t r = piece + 0x10;
    sect_rect_ptr(gGameRect, r, r);
    mark_bufs_for_rect(JT_ADDR(FN_MarkMove), r);
    RestoreKeepCustomPeel(R16(piece));
    return 0;
}

/* 00055be8: draw the exit (custom pieces 2 and 16, moved up by the opening step) */
uint32_t DrawDsrtExit(void) {
    ENTRY_LINK(FN_DrawDsrtExit, 8, 12);          /* link -8; movem D3-D4/A2 */
    uint32_t it = R32(gCurBFItem);
    if (R16(it + 0xa) != 0xb) return 0;
    if (R16(it + 2) != R16(gCellCol)) return 0;
    int16_t n = R16(it + 0x14) & 0x1f;
    if (n != 0) {
        copy_rect(a6 - 8, gCellRect);
        copy_rect(gCellRect, gGameRect);
    }
    int16_t d = (int16_t)(n * 3);
    W16(R32(gCustRoom) + 0x62, 0xd - d);
    W16(R32(gCustRoom) + 0x222, 0x29 - d);
    W16(R32(gCustRoom) + 0x230, 0);
    DrawCustomRoomImg(2, 0xb);
    DrawCustomRoomImg(0x10, 0xb);
    if (n != 0) copy_rect(gCellRect, a6 - 8);
    return 0;
}

/* 00055c96: exit opening step (0 = closed) */
uint32_t FUN_00055c96(void) {
    return RU16(GS() + 0x7a8) & 0x1f;
}

/* 00055caa: open the exit (trob type 4) unless it is already animated */
uint32_t TriggerDsrtExit(int16_t room, int16_t cell) {
    ENTRY_LINK(FN_TriggerDsrtExit, 0, 0);        /* link 0 */
    if (GetTrobRoomCell(room, cell) != 0) return 0;
    AddTrob(room, cell, 1, 4);
    AddSong(0x2f);
    return 0;
}

/* 00055cf8: animate the planks (6 frames), redraw custom piece 1 */
uint32_t FUN_00055cf8(void) {
    ENTRY_NOLINK(FN_FUN_00055cf8, 4);            /* move.l D3 */
    if (S16(FUN_00025362()) == 0) return 0;
    uint16_t f = RU16(gTrobState) & 0xf;
    if (f == 5) f = 0;
    else f++;
    W16(gTrobState, RU16(gTrobState) & 0xfff0);
    W16(gTrobState, RU16(gTrobState) | f);
    MarkMove(R16(gTrobCell), R32(gCustRoom) + 0x4e);
    return 0;
}

/* 00055d38: draw the planks (frame / 2 + 1) */
uint32_t DrawDsrtPlanks(void) {
    ENTRY_LINK(FN_DrawDsrtPlanks, 8, 16);        /* link -8; movem D3-D5/A2 */
    uint32_t it = R32(gCurBFItem);
    if (R16(it + 0xa) != 0xb) return 0;
    uint16_t f = RU16(it + 0x14) & 0xf;
    int16_t img = (int16_t)((f >> 1) + 1);
    copy_rect(a6 - 8, gCellRect);
    copy_rect(gCellRect, gGameRect);
    DrawCustomRoomImg(img, 0xb);
    copy_rect(gCellRect, a6 - 8);
    return 0;
}

/* 00055daa: animate the rocks (6 frames), redraw custom piece 2 */
uint32_t FUN_00055daa(void) {
    ENTRY_NOLINK(FN_FUN_00055daa, 4);            /* move.l D3 */
    if (S16(FUN_00025362()) == 0) return 0;
    uint16_t f = RU16(gTrobState) & 0xf;
    if (f == 5) f = 0;
    else f++;
    W16(gTrobState, RU16(gTrobState) & 0xfff0);
    W16(gTrobState, RU16(gTrobState) | f);
    MarkMove(R16(gTrobCell), R32(gCustRoom) + 0x6e);
    return 0;
}

/* 00055dea: draw the rocks (frame / 2 + 2) */
uint32_t DrawDsrtRocks(void) {
    ENTRY_LINK(FN_DrawDsrtRocks, 8, 16);         /* link -8; movem D3-D5/A2 */
    uint32_t it = R32(gCurBFItem);
    if (R16(it + 0xa) != 0xb) return 0;
    uint16_t f = RU16(it + 0x14) & 0xf;
    int16_t img = (int16_t)((f >> 1) + 2);
    copy_rect(a6 - 8, gCellRect);
    copy_rect(gCellRect, gGameRect);
    DrawCustomRoomImg(img, 0xb);
    copy_rect(gCellRect, a6 - 8);
    return 0;
}

/* 00055e5c: add the save-under peels of the six stones of a custom piece */
uint32_t AddDsrtStoneUpPeels(uint32_t piece) {
    ENTRY_LINK(FN_AddDsrtStoneUpPeels, 8, 16);   /* link -8; movem D3-D6 */
    int16_t id = (int16_t)(R16(piece) + 0x64);
    int16_t dh = 0x66;
    for (int16_t i = 0; i < 6; i++) {
        copy_rect(a6 - 8, piece + 0x10);
        _OffsetRect(a6 - 8, dh, 0xf0);
        AddPeel(a6 - 8, id, 3);
        dh = (int16_t)(dh + 0x33);
    }
    return 0;
}

/* 00055ecc: animate a stone: wait counter (bits 4-8) while the kid stands on it,
 * then sink (dir 3) / rise (dir 1) through frames 0-13 */
uint32_t FUN_00055ecc(void) {
    ENTRY_NOLINK(FN_FUN_00055ecc, 8);            /* movem D3-D4 */
    int16_t n = (int16_t)((RU16(gTrobState) & 0x1f0) >> 4);
    if (n == 0) {
        uint16_t f = RU16(gTrobState) & 0xf;
        if (R16(gTrobDir) == 3 && f == 0) AddSound(0x34);
        if ((f == 0xd && R16(gTrobDir) == 3) || (f == 0 && R16(gTrobDir) == 1)) {
            if (R16(gTrobDir) == 3 && GetTrobId(0x1e, 2) == 0) FUN_000554e4();
            W16(gTrobDir, -1);
        } else {
            f = (uint16_t)(f + (int16_t)(R16(gTrobDir) - 2));
            W16(gTrobState, RU16(gTrobState) & 0xfff0);
            W16(gTrobState, RU16(gTrobState) | f);
        }
        if (R16(gTrobDir) > 0) RedrawStone();
    } else if (R16(KID(F_SEQ)) != 0x1b && R16(KID(F_ALIVE)) < 0) {
        n--;
        W16(gTrobState, RU16(gTrobState) & 0xfe0f);
        W16(gTrobState, RU16(gTrobState) | (uint16_t)(n << 4));
    }
    return 0;
}

/* 00055f88: draw a stone (and the special stone's extra images) */
uint32_t seg18_055f88(void) {
    ENTRY_NOLINK(FN_seg18_055f88, 12);           /* movem D3-D4/A2 */
    uint32_t it = R32(gCurBFItem);
    int16_t layer = R16(it + 0xa);
    if (layer != 0xb && layer != 2) return 0;
    uint16_t f = RU16(it + 0x14) & 0xf;
    if (f == 0) return 0;
    DrawCustomRoomImgAtInfo((int16_t)(f + 2));
    if ((int32_t)R16(R32(gCurBFItem) + 0xe) - 2 != (int32_t)R16(gStoneIdx)) return 0;
    if (f == 9) {
        DrawCustomRoomImgAtInfo(0x11);
        return 0;
    }
    if (f <= 9) return 0;
    uint32_t piece = R32(gCustRoom) + 0x25e;
    int16_t y = (int16_t)((int16_t)((0xc - f) << 1) - 6);
    W16(piece + 2, y);
    if (y < -6) W16(piece + 2, -6);
    SetCustomPieceRect(piece);
    DrawCustomRoomImgAtInfo(0x12);
    return 0;
}

/* 0005601c: mark the moving stone's rect for redraw */
uint32_t RedrawStone(void) {
    ENTRY_LINK(FN_RedrawStone, 8, 0);            /* link -8 */
    if (ComputeTrobMoveRect(a6 - 8, GH(0x104b94)) != 0)
        mark_bufs_for_rect(JT_ADDR(FN_MarkMove), a6 - 8);
    return 0;
}

/* 0005605a: drop the keep flag of the stone peels (char id 0x73) of column col */
uint32_t RemoveLastStonePeel(int16_t col) {
    ENTRY_LINK(FN_RemoveLastStonePeel, 0xc, 20); /* link -0xc; movem D3-D7 */
    if (R32(gCustRoom) == 0) return 0;
    if (R16(R32(gCustRoom)) != 6) return 0;
    int16_t x = (int16_t)((int16_t)(col * 0x33) + 0x33);
    W16(a6 - 4, 0);
    int16_t n = 1;
    int16_t idx;
    do {
        idx = S16(GetPeelIdxForCharIdx(0x73, n));
        if (idx != -1) {
            int16_t off = (int16_t)((int32_t)idx * 0x12);
            GetMapSaveRect(a6 - 0xc, R32(gPeelTab + off));
            if (R16(a6 - 0xa) == x || R16(a6 - 6) == x) {
                W16(gPeelTab + 0x10 + off, 0);
                W16(a6 - 4, R16(a6 - 4) + 1);
                if (R16(a6 - 4) == 2) idx = -1;
            } else if (R16(a6 - 0xa) < x && R16(a6 - 6) > x && R16(a6 - 0xc) == 0xd2 &&
                       R16(a6 - 8) == 0xf0) {
                W16(gPeelTab + 0x10 + (int16_t)((int32_t)idx * 0x12), 0);
                idx = -1;
            }
        }
        n++;
    } while (idx != -1);
    return 0;
}

/* 0005616c: make the stone at `cell` of room 1 sink; returns 1 if it was started */
uint32_t SinkDsrtStone(int16_t cell) {
    ENTRY_LINK(FN_SinkDsrtStone, 4, 16);         /* link -4; movem D3-D6 */
    int16_t r = S16(TriggerStone(1, cell, 1));
    if (r != 0) {
        uint16_t saved = RU16(gTrobState);
        int32_t off = (int32_t)cell * 4;
        W32(a6 - 4, (uint32_t)off);
        W16(gTrobState, RU16(GS() + 0x780 + off));
        FUN_00055ecc();
        W16(GS() + 0x780 + (int32_t)R32(a6 - 4), RU16(gTrobState));
        W16(gTrobState, saved);
    }
    return (uint16_t)r;
}

/* 000561e6: the kid dies on the stones: sink the marked one and his own */
uint32_t FUN_000561e6(void) {
    ENTRY_NOLINK(FN_FUN_000561e6, 4);            /* move.l D3 */
    int16_t c = R16(gLastMarkCol);
    if (c != -1 && c >= 2 && c <= 7) SinkDsrtStone((int16_t)(c + 10));
    SinkDsrtStone((int16_t)(R16(CH(F_COL)) + 10));
    return 0;
}

/* 0005621e: trigger the stone at (room, cell): how 3 = set a wait counter (or play the
 * song for column 7), 1 = sink. Returns 1 when a trob was added. */
uint32_t TriggerStone(int16_t room, int16_t cell, int16_t how) {
    ENTRY_LINK(FN_TriggerStone, 8, 20);          /* link -8; movem D3-D7 */
    W32(a6 - 8, (uint32_t)(uint16_t)room * 0x78);
    W32(a6 - 4, (uint32_t)((int32_t)cell * 4));
    uint16_t st = RU16(STONE_STATE(room, cell));
    uint16_t f = st & 0xf;
    int16_t add = 1;
    if (how == 3 && f == 0) {
        st &= 0xfe0f;
        int16_t w = (int16_t)(S16(Rnd(0xa)) + 0x15);
        st |= (uint16_t)(w << 4);
        W16(GS() + 0x780 + R32(a6 - 8) - 0x78 + R32(a6 - 4), st);
    } else if (how == 1) {
        if (f != 0) {
            AddSound(0x35);
        } else {
            add = GetTrobRoomCell(room, cell) != 0 ? 1 : 0;
            if (st & 0x1f0) {
                st &= 0xfe0f;
                W16(STONE_STATE(room, cell), st);
            }
        }
    } else {
        add = 0;
        if (how == 3 && (int16_t)(cell % 10) == 7 && GetTrobId(0x1e, 1) == 0) FUN_000554e4();
    }
    if (add) {
        AddTrob(room, cell, how, 0x1e);
        if (how == 1) RemoveLastStonePeel((int16_t)(cell % 10));
    }
    return (uint16_t)add;
}
