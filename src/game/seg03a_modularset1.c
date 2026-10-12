/* CODE 3 "Modular Set 1", first half (Ghidra 0x17674-0x1a8ae).
 *
 * The room renderer's item tables: background/foreground "BF items" built cell by cell
 * (DrawBackImg/DrawForeImg call the per-piece draw routine, which fills the current item
 * and calls AddNextBFItemToTable), "mid" items (objects), character objects, the peel
 * (save-under) table, custom-room data (CUST resources with extra shapes), the fast cell
 * redraw paths and the final DumpTable/DrawBFItem/DrawMidItem drawing.
 *
 * Ported from the Ghidra decompilation and disassembly of the original 68k code;
 * addresses in comments are Ghidra addresses (see docs/PORTING.md). */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* ---- game state ---- */
#define gGameState      GH(0x102fec)   /* long: pointer to the game state block */
#define LEVEL_TYPE()    R16(R32(gGameState) + 0x2186)   /* scenery set (1 desert, 2/4/6 ..., 5 rooftops) */
#define LEVEL_NUM()     R16(R32(gGameState) + 0x218a)   /* level number */
#define ROOM_LINK(r, k) (R32(gGameState) + 0x2080 + (uint32_t)(uint16_t)(r) * 8 - 8 + 2 * (k)) /* k: 0 left 1 right 2 up 3 down */

/* ---- rooms ---- */
#define gCurRoom        GH(0x10303e)   /* word: room being drawn */
#define gRoomLeft       GH(0x103040)   /* word: neighbours of gCurRoom */
#define gRoomRight      GH(0x103042)
#define gRoomUp         GH(0x103044)
#define gRoomDown       GH(0x103046)
#define gRoomUpLeft     GH(0x103048)
#define gRoomUpRight    GH(0x10304a)
#define gRoomDownLeft   GH(0x10304c)
#define gRoomDownRight  GH(0x10304e)
#define gRoomCustomized GH(0x103050)   /* word: IsCustomizedRoom(gCurRoom) */

/* ---- cell being drawn ---- */
#define gDrawRect       GH(0x103a00)   /* rect: current update/clip rectangle */
#define gDrawRect_left  GH(0x103a02)
#define gDrawCol        GH(0x103a08)   /* word: column 0..9 */
#define gDrawRow        GH(0x103a0a)   /* word: row 0..2 (-1 = top line) */
#define gCellInfo       GH(0x1037e8)   /* long: info of the current cell */
#define gCellPiece      GH(0x1037ec)   /* word: piece of the current cell */
#define gLeftPiece      GH(0x10362a)   /* word: piece of the cell to the left */
#define gLeftInfo       GH(0x10362c)   /* long: info of the cell to the left */
#define gBelowAry       GH(0x10357c)   /* 11 x {word piece, long info}: row below, from column -1 */
#define gFullRedraw     GH(0x1037ce)   /* word: whole room is being redrawn */
#define gPieceDrawFns   GH(0x103552)   /* long: table of per-piece draw routines */
#define gScreenRect     GH(0x103014)   /* rect: the game area */

/* ---- BF item tables (0x26-byte items) ----
 * +0 shape set, +2 col, +4 row, +6, +8 flip, +a layer (1/2 = foreground), +c, +e col, +10 row,
 * +12 piece, +14 info, +18 shape, +1a x, +1c y, +1e rect */
#define gCurBFItem      GH(0x103074)   /* long: item being filled (0 = none) */
#define gForeTable      GH(0x10335a)   /* long: foreground items (max 0x78) */
#define gBackTable      GH(0x10335e)   /* long: background items (max 0x82) */
#define gBackCount      GH(0x1037d2)
#define gForeCount      GH(0x1037d4)
#define gColX           GH(0x103498)   /* word[]: x of each column */
#define gRowY           GH(0x1034b2)   /* word[]: y of each row */
#define gShapeSets      GH(0x103568)   /* long[]: shape set per item type */
#define gBGSetShapes    GH(0x103578)   /* long: the background set (served by GetBGSetShapeItem) */

/* ---- mid items (0x18 bytes) and character objects (0x1c bytes) ---- */
#define gMidTable2      GH(0x103352)   /* long: mid items used when gFChar.kind == 100 */
#define gMidTable       GH(0x103356)
#define gMidCount2      GH(0x1037d0)
#define gMidCount       GH(0x1037d8)
#define gCharObjTable   GH(0x10334e)
#define gCharObjCount   GH(0x1037da)   /* max 30 */
/* the character being set up ("FChar") */
#define gFChar_kind     GH(0x104b38)
#define gFChar_b3a      GH(0x104b3a)
#define gFChar_x        GH(0x104b3c)
#define gFChar_y        GH(0x104b3e)
#define gFChar_shape    GH(0x104b40)
#define gFChar_set      GH(0x104b42)
#define gFChar_rect     GH(0x104b44)
#define gFChar_b4c      GH(0x104b4c)
#define gFChar_b4e      GH(0x104b4e)

/* ---- character state ---- */
#define gCharId         GH(0x102faa)
#define gChar_fac       GH(0x102fac)
#define gChar_fb0       GH(0x102fb0)
#define gCharType       GH(0x102fb2)   /* 0 kid, 1 shadow, 7/8 ..., 10 ... */
#define gCharFrame      GH(0x102fb4)
#define gCharAction     GH(0x102fba)
#define gCharRoom       GH(0x102fc0)
#define gCharRect       GH(0x102fd4)
#define gChar_fde       GH(0x102fde)
#define gChar_fe0       GH(0x102fe0)
#define gChar_fe4       GH(0x102fe4)
#define gCharTable      GH(0x102df8)   /* 0x3e-byte entries of the room's characters */
#define gCharEnts_room  GH(0x102e0e)
#define gWeapon         GH(0x102de6)   /* word: 1 sword, 3, 5 flame, 6 ... */
#define gWeapon_shape   GH(0x102de8)
#define gWeapon_dx      GH(0x102dea)
#define gWeapon_dy      GH(0x102dec)
#define gWeapon_ee      GH(0x102dee)

/* ---- peels: save-under rectangles (0x12 bytes: +0 saved bits, +4 rect, +c id, +e kind, +10 keep) ---- */
#define gPeelCount      GH(0x10307c)   /* max 0x28 */
#define gPeelTable      GH(0x10307e)
#define PEEL(i)         (gPeelTable + (int32_t)(i) * 0x12)

/* ---- custom rooms ---- */
#define gCustomRoom     GH(0x103362)   /* long: locked CUST data (0 = none); pieces at +0x1e, 0x20 each */
#define gCustomRoomH    GH(0x103366)   /* long: its handle */
#define gRoomTemplate   GH(0x10345e)   /* long: {load hook, unload hook} of the room template */
#define gTemplates      GH(0x1033d2)   /* long[35] */
#define gDefTemplate    GH(0x1033ca)
#define PIECE(i)        (R32(gCustomRoom) + 0x1e + (int32_t)(i) * 32)

/* ---- fast redraw flags per cell ---- */
#define gRedrawTop      GH(0x103c1c)   /* 10-byte entries {flag, rect} */
#define gRedrawObj      GH(0x103c20)   /* word flags */
#define gRedrawHalf     GH(0x103c24)   /* word flags */
#define gRedrawFloor    GH(0x103c28)   /* 10-byte entries {flag, rect} */
#define gRedrawFore     GH(0x103c2c)   /* 10-byte entries */
#define gRedrawBack     GH(0x103c30)   /* 10-byte entries */
#define gRedrawFull     GH(0x103c34)   /* word flags */

/* QuickDraw entry points cached in globals (Pascal convention, called with jsr (A0)) */
#define pClipRect       GH(0x10609c)   /* (rect) */
#define pEmptyRect      GH(0x1060a0)   /* (rect): Boolean */
#define pUnionRect      GH(0x1060a4)   /* (a, b, dst) */
#define pSectRect       GH(0x1060a8)   /* (a, b, dst): Boolean */

/* Call a Pascal routine through a pointer held in a global: the callee pops the
 * arguments; a Boolean result is the high byte of the word reserved above them. */
static uint32_t pas_call(maddr gptr, int has_result, int n, uint32_t a, uint32_t b, uint32_t c) {
    maddr fn = R32(gptr);
    maddr sp0 = emu_sp();
    if (has_result) emu_set_sp(sp0 - 2);
    if (n > 0) push32(a);
    if (n > 1) push32(b);
    if (n > 2) push32(c);
    call68_addr(fn);
    uint32_t r = has_result ? rd8(sp0 - 2) : 0;
    emu_set_sp(sp0);
    return r;
}
#define SECT_RECT(a, b, d)   pas_call(pSectRect, 1, 3, (a), (b), (d))
#define EMPTY_RECT(r)        pas_call(pEmptyRect, 1, 1, (r), 0, 0)
#define CLIP_RECT(r)         pas_call(pClipRect, 0, 1, (r), 0, 0)
#define UNION_RECT(a, b, d)  pas_call(pUnionRect, 0, 3, (a), (b), (d))

/* (index * 4 as a word index, like `(A0,D0.w)` on an unsigned word) */
#define SET_OF(type)    R32(gShapeSets + (int16_t)((uint16_t)(type) << 2))

/* 00017674: finish the current BF item (position, rect) and keep it if it is visible */
uint32_t AddNextBFItemToTable(void) {
    FRAME(f, 8);
    uint16_t r = 0;
    uint32_t item = R32(gCurBFItem);
    if (item == 0) goto out;
    uint32_t set = SET_OF(RU16(item));
    uint32_t h = (set == R32(gBGSetShapes)) ? GetBGSetShapeItem(R16(item + 0x18))
                                             : PrinceGetnShapeItem(set, R16(item + 0x18));
    uint32_t shp = R32(h);
    if (shp == 0 || R16(shp + 6) == 0) goto out;
    item = R32(gCurBFItem);
    int16_t dy = (int16_t)(R16(gRowY + (int16_t)(R16(item + 0x10) * 2)) - R16(shp + 6));
    W16(item + 0x1c, R16(item + 0x1c) + dy);
    item = R32(gCurBFItem);
    W16(item + 0x1a, R16(item + 0x1a) + R16(gColX + (int16_t)(R16(item + 0xe) * 2)));
    int16_t x = R16(item + 0x1a);
    MakeShapeRect(f, R16(R32(gCurBFItem) + 0x1c), x, shp);
    r = (uint16_t)SECT_RECT(gDrawRect, f, f);
    if (r != 0) {
        mcopy(R32(gCurBFItem) + 0x1e, f, 8);
        int16_t layer = R16(R32(gCurBFItem) + 0xa);
        if (layer == 1 || layer == 2) W16(gForeCount, R16(gForeCount) + 1);
        else W16(gBackCount, R16(gBackCount) + 1);
        W32(gCurBFItem, 0);
    }
out:
    FRAME_END(f);
    return r;
}

/* 00017792: add the room's characters */
uint32_t FUN_00017792(void) {
    FUN_00017db8();
    Opponent1();
    seg2_01585a();
    return 0;
}

/* 000177a0: add a character object from the FChar globals */
uint32_t AddCharObj(int16_t kind) {
    if (R16(gCharObjCount) < 0x1e) {
        int16_t n = R16(gCharObjCount);
        W16(gCharObjCount, n + 1);
        uint32_t o = R32(gCharObjTable) + (int32_t)n * 0x1c;
        W16(o + 0xc, kind);
        ConvertX(R16(gFChar_x), o);
        W16(o + 2, R16(gFChar_y));
        W16(o + 4, R16(gFChar_set));
        W16(o + 6, R16(gFChar_shape));
        W16(o + 0xa, R16(gFChar_b3a));
        mcopy(o + 0xe, gFChar_rect, 8);
        W16(o + 0x18, R16(gFChar_b4c));
        W16(o + 0x16, (kind == 3 || kind == 9) ? -1 : 0);
        W16(o + 8, R16(gCharFrame));
        W16(o + 0x1a, R16(gCharId));
    }
    return 0;
}

/* 00017856: add a mid (object) item for shape `shape` (1-based) of set `type` */
uint32_t AddMid(int16_t type, int16_t shape, int16_t w0, int16_t bottom, int16_t w10) {
    FRAME(f, 8);   /* f: {height, width} */
    uint16_t r = 0;
    if (shape == 0 || R16(gMidCount) >= 0x1e || _EmptyRect(gFChar_rect) != 0) goto out;
    int16_t idx = (int16_t)(shape - 1);
    uint32_t m;
    if (R16(gFChar_kind) == 100) m = R32(gMidTable2) + (int32_t)R16(gMidCount2) * 0x18;
    else m = R32(gMidTable) + (int32_t)R16(gMidCount) * 0x18;
    W16(m, w0);
    if (LEVEL_NUM() == 5 && R16(gCurRoom) == 10 && type == 4 && idx < R16(R32(gCustomRoom) + 0x1c)) {
        GetCustomPieceSize(idx, f);
    } else if (R32(GH(0x105480)) != 0 && idx >= 0xfed && idx <= 0x101a) {
        GetFlameImgSize(idx, f);
    } else {
        uint32_t set = SET_OF(type);
        uint32_t ok;
        if (set == R32(gBGSetShapes)) ok = GetBGSetImgSize(idx, f);
        else ok = PrinceGetnShapeItemSize(set, (int16_t)(idx + 1), f);
        if (ok == 0 || R16(f) == 0) goto out;
    }
    W16(m + 2, bottom - R16(f) + 1);
    W16(m + 4, type);
    W16(m + 6, idx);
    mcopy(m + 8, gFChar_rect, 8);
    W16(m + 0x10, w10);
    W16(m + 0x12, 0);
    W16(m + 0x14, (R16(gFChar_b3a) != 0 || (type == 4 && R16(gFChar_b4e) < 0)) ? 0 : 1);
    W16(m + 0x16, -1);
    if (R16(gFChar_kind) == 100) W16(gMidCount2, R16(gMidCount2) + 1);
    else W16(gMidCount, R16(gMidCount) + 1);
    r = 1;
out:
    FRAME_END(f);
    return r;
}

/* 000179f4: save the screen under `rect` (clipped to the game area) as a peel */
uint32_t AddPeel(uint32_t rect, int16_t id, int16_t kind) {
    if (R16(gPeelCount) >= 0x28) return 0;
    if (SECT_RECT(gScreenRect, rect, rect) == 0) return 0;
    uint32_t e = PEEL(R16(gPeelCount));
    uint32_t h = SaveRect(rect, e);
    W32(e, h);
    if (h == 0) return 0;
    mcopy(e + 4, rect, 8);
    W16(e + 0xc, id);
    W16(e + 0xe, kind);
    W16(e + 0x10, (kind == 2 || kind == 3) ? 1 : 0);
    W16(gPeelCount, R16(gPeelCount) + 1);
    return 0;
}

/* 00017a98: mark the buffers under the peels that will be restored */
uint32_t MarkPeelForeBufs(void) {
    int16_t n = R16(gPeelCount);
    uint32_t e = GH(0x10306c) + (int32_t)R16(gPeelCount) * 0x12;   /* last peel */
    uint32_t rect = e + 4, id = e + 0xc, keep = e + 0x10;
    while (n-- != 0) {
        if (R16(keep) == 0) {
            int16_t mark = 1;
            int16_t c = R16(id);
            if (c >= 0 && c < 5) {
                LoadShad(c);
                if (R16(gCharAction) == 9 || S16(IsCharNonViewable()) != 0) mark = 0;
            }
            /* the original also pushes the peel id; MarkBufsForRect does not read it */
            if (mark) MarkBufsForRect(JT_ADDR(FN_MarkFRedraw), rect);
        }
        rect -= 0x12; id -= 0x12; keep -= 0x12;
    }
    return 0;
}

/* 00017b54: index of the nth (1-based) live peel with this id, or -1 */
uint32_t GetPeelIdxForCharIdx(int16_t id, int16_t nth) {
    int16_t found = -1, seen = 0, i = 0;
    uint32_t e = gPeelTable;
    if (R16(gPeelCount) > 0) {
        do {
            if (R16(e + 0xc) == id && R32(e) != 0) {
                seen++;
                if (seen == nth) found = i;
            }
            i++;
            e += 0x12;
        } while (i < R16(gPeelCount) && found == -1);
    }
    return (uint16_t)found;
}

/* 00017bba: restore the screen under the peels (last first) and compact the table */
uint32_t Peel(void) {
    int16_t keep[0x30];
    uint32_t rgn = _NewRgn();
    _GetClip(rgn);
    int16_t i = R16(gPeelCount);
    uint32_t e = PEEL(R16(gPeelCount));
    while (i-- != 0) {
        e -= 0x12;
        keep[i] = R16(e + 0x10);
        if (R16(e + 0x10) == 0) {
            if (R16(gFullRedraw) == 0) AddDRect(e + 4);
            CLIP_RECT(e + 4);
            RestoreRectKeep(R32(e));
            if (R16(e + 0xe) == 2) {
                W16(e + 0x10, 1);
                keep[i] = 1;
            }
        }
    }
    _SetClip(rgn);
    _DisposeRgn(rgn);
    int16_t n = R16(gPeelCount), kept = 0;
    uint32_t dst = gPeelTable, src = gPeelTable;
    int16_t k = 0;
    if (k < R16(gPeelCount)) {
        do {
            if (keep[k] != 0) {
                if (src != dst) {
                    uint32_t h = R32(dst);
                    if (h != 0) _HPurge(h);
                    mcopy(dst, src, 0x12);
                    W32(src, 0);
                }
                int16_t id = R16(dst + 0xc);
                if (id < 100 || id >= 200) W16(dst + 0x10, 0);
                kept++;
                dst += 0x12;
            }
            k++;
            src += 0x12;
        } while (k < n);
    }
    W16(gPeelCount, kept);
    return 0;
}

/* 00017cfe: free all peels */
uint32_t FUN_00017cfe(void) {
    uint32_t e = gPeelTable;
    uint16_t i = 0;
    do {
        uint32_t h = R32(e);
        if (h != 0) {
            DeallocMapSave(h);
            W32(e, 0);
        }
        i++;
        e += 0x12;
    } while (i < 0x28);
    W16(gPeelCount, 0);
    return 0;
}

/* 00017d34: the background item with this shape at (col, row), or 0 */
uint32_t GetBackTableImgRow(int16_t shape, int16_t col, int16_t row) {
    uint32_t e = R32(gBackTable), found = 0;
    int16_t i = 0;
    if (R16(gBackCount) > 0) {
        do {
            if (R16(e + 0x18) == shape && R16(e + 4) == row && R16(e + 2) == col) found = e;
            i++;
            e += 0x26;
        } while (i < R16(gBackCount) && found == 0);
    }
    return found;
}

/* 00017d9e: clear the item counters */
uint32_t FUN_00017d9e(void) {
    tc_memset(gBackCount, 0, 10);
    W16(gMidCount2, 0);
    return 0;
}

/* 00017db8: add the kid (and what he carries/causes) as character objects */
uint32_t FUN_00017db8(void) {
    FUN_000225d2();
    int16_t opp = R16(gChar_fe4);
    if (opp != -1) {
        uint16_t room = RU16(gCharRoom);
        int32_t n = room ? (int32_t)R16(R32(gGameState) + 0x21a6 + (uint32_t)room * 0xc0 - 0xc0) : 0;
        if (n > (int32_t)opp) LoadShadAsOpp(opp);
    }
    if (R16(gCharFrame) != 0 && R16(gCharRoom) != 0 && R16(gCharRoom) == R16(gCurRoom)) {
        FUN_000183f8();
        uint32_t last = R32(gCharObjTable) + (int32_t)R16(gCharObjCount) * 0x1c - 6;
        if (R16(gCharAction) == 10 || R16(gChar_fde) == 8) {
            FUN_00018214();
            if (R16(gChar_fde) == 8) W16(gChar_fde, 0);
            else W16(gCharAction, 5);
        }
        if (LEVEL_TYPE() == 5) SetupSplash();
        else if (LEVEL_TYPE() == 1) SetupSand();
        else if (LEVEL_NUM() == 5 && R16(gCharRoom) == 3 && R16(gCharFrame) == 0x127) SetupCarpet();
        else {
            uint16_t fr = RU16(gCharFrame);
            if (fr >= 0x132 && fr <= 0x13e) SetupFlame();
            else if (R16(gWeapon) == 5) {
                FUN_0001839a();
                W16(last, R16(gCharObjCount) - 1);
            }
        }
        if (R16(gCharType) == 1 && LEVEL_TYPE() == 6) SetupShadowFlame();
        if (R16(gWeapon) == 1 && R16(gChar_fde) != 5) {
            SetupSword(3);
            W16(last, R16(gCharObjCount) - 1);
        }
        if (R16(gWeapon) == 3) {
            FUN_0001850c();
            W16(last, R16(gCharObjCount) - 1);
        }
    } else if (LEVEL_TYPE() == 5) {
        SetupSplash();
    }
    return 0;
}

/* 00017f56: add the opponents in the current room as character objects */
uint32_t Opponent1(void) {
    uint16_t room = RU16(gCurRoom);
    int16_t n = room ? R16(R32(gGameState) + (uint32_t)room * 0xc0 + 0x20e6) : 0;
    if (n == 0) return 0;
    int16_t i = 0;
    if (n <= 0) return 0;
    uint32_t ent = gCharEnts_room;
    /* `last` lives in A2, which the original only sets on some paths: start from the caller's A2 */
    uint32_t last = cpu_get(R_A2);
    do {
        if (R16(ent) == R16(gCurRoom)) {
            LoadShad(i);
            SetupOpponent();
            if (R16(gCharAction) != 9)
                last = R32(gCharObjTable) + (int32_t)R16(gCharObjCount) * 0x1c - 6;
            else
                W16(R32(gCharObjTable) + (int32_t)R16(gCharObjCount) * 0x1c - 6, 100);
            int16_t act = R16(gCharAction);
            if (act == 10 || R16(gChar_fde) == 8 || (act == 9 && R16(gCharFrame) == 0xd5)) {
                int16_t t = R16(gCharType);
                if (t != 7 && t != 8) {
                    if (act == 10) W16(gCharAction, 5);
                    FUN_00018214();
                }
            }
            int16_t t = R16(gCharType);
            if (t != 10 && t != 1 && R16(gWeapon) == 1 && R16(gChar_fde) != 5) {
                SetupSword(9);
                if (R16(gCharAction) != 9) W16(last, R16(gCharObjCount) - 1);
                else W16(R32(gCharObjTable) + (int32_t)R16(gCharObjCount) * 0x1c - 6, 100);
            }
            if (R16(gWeapon) == 6) {
                FUN_0001843a();
                W16(last, R16(gCharObjCount) - 1);
            } else if (R16(gCharType) == 1) {
                SetupShadowFlame();
            } else if (LEVEL_TYPE() == 5) {
                SetupSplash();
            } else if (R16(gCharType) == 6) {
                uint16_t fr = RU16(gCharFrame);
                if (fr >= 0x132 && fr <= 0x13e) SetupFlame();
            }
        } else if (LEVEL_TYPE() == 5) {
            LoadShad(i);
            SetupSplash();
        }
        i++;
        ent += 0x3e;
    } while (i < n);
    return 0;
}

/* 00018138: set the FChar globals from the current character */
uint32_t FUN_00018138(void) {
    if (R16(gCharFrame) == 0) return 0;
    if (R16(gWeapon_ee) == -1 && R16(gWeapon_shape) == -1) return 0;
    W16(gFChar_b3a, R16(gChar_fac));
    W16(gFChar_b4c, R16(gChar_fe0));
    if (R16(gCharType) == 0 || R16(gCharType) == 6 || R16(gCharType) == 1) {
        W16(gFChar_set, 2);
        W16(gFChar_shape, R16(gWeapon_ee));
    } else {
        W16(gFChar_set, 3);
        if (R16(gCharType) == 10) W16(gFChar_shape, R16(gWeapon_shape));
        else W16(gFChar_shape, R16(gWeapon_ee));
    }
    int16_t t = R16(gCharType);
    if ((t == 7 || t == 8) && R16(gChar_fde) == 1) {
        AlignHeadOnKidNeck();
        return 0;
    }
    W16(gFChar_y, R16(gChar_fb0) + R16(GH(0x102df4)));
    int16_t x = (int16_t)(S16(AddCharX(R16(GH(0x102df2)))) - 0xcf);
    W16(gFChar_x, x);
    if (R16(gCharType) == 10) {
        seg4_01f6b4();
    } else if (R16(gCharType) == 4) {
        uint16_t fr = RU16(gCharFrame);
        if (fr >= 0xce && fr <= 0xd1) {
            W16(gFChar_set, 4);
            W16(gFChar_shape, R16(gFChar_shape) + 0x6d);
        }
    }
    return 0;
}

/* 00018214: add the character's head (FChar) as a character object */
uint32_t FUN_00018214(void) {
    uint16_t fr = RU16(gCharFrame);
    seg4_022442();
    W16(gFChar_kind, -1);
    W16(gFChar_set, 2);
    W16(gFChar_shape, 0xda);
    W16(gFChar_b4c, 0x10);
    if (fr == 0xb9 || (fr >= 0x6a && fr < 0x6f)) {
        W16(gFChar_y, R16(gFChar_y) + 8);
        AddFCharX(0x13);
    } else if (R16(gCharType) == 0xb) {
        W16(gFChar_y, R16(gFChar_y) + 7);
        AddFCharX(-0xc);
        W16(gFChar_set, 3);
        W16(gFChar_shape, 0x7f);
    } else if (R16(gCharType) == 0xc) {
        W16(gFChar_y, R16(gFChar_y) - 0x15);
        AddFCharX(-0xc);
    } else if (R16(gCharType) == 0) {
        if (R16(GH(0x102f74)) == 0xb || R16(gCharFrame) == 0x109) {
            AddFCharX(0xe);
        } else {
            int16_t f2 = R16(gCharFrame);
            if (f2 == 0x8a || f2 == 0x8b || f2 == 0x8c) {
                W16(gFChar_y, R16(gFChar_y) - 0xf);
                AddFCharX(-7);
            } else if (R16(gCharFrame) == 0x152) {
                W16(gFChar_y, R16(gFChar_y) - 0x5a);
                AddFCharX(0x1e);
            } else if (R16(gCharFrame) == 0x154) {
                W16(gFChar_y, R16(gFChar_y) - 0x50);
            } else {
                W16(gFChar_y, R16(gFChar_y) - 0x1e);
                AddFCharX(0xe);
            }
        }
    } else {
        W16(gFChar_y, R16(gFChar_y) - 0x15);
        AddFCharX(0xb);
    }
    MakeFCharClipRect();
    AddCharObj(5);
    if (R16(gCharAction) == 9)
        W16(R32(gCharObjTable) + (int32_t)R16(gCharObjCount) * 0x1c - 6, 100);
    if (R16(gCharAction) != 9) MarkBufsForRect(JT_ADDR(FN_MarkFRedraw), gFChar_rect);
    seg4_022454();
    return 0;
}

/* 0001839a: add the flame (weapon 5) as a character object */
uint32_t FUN_0001839a(void) {
    if (R16(gWeapon) != 5) return 0;
    seg4_022442();
    W16(gFChar_shape, R16(gWeapon_shape) + 0x100f);
    AddFCharX(R16(gWeapon_dx));
    W16(gFChar_y, R16(gFChar_y) + R16(gWeapon_dy));
    W16(gFChar_set, 4);
    W16(gFChar_b4c, R16(GH(0x105490)));
    MakeFlameFCharClipRect();
    AddCharObj(3);
    MarkBufsForRect(JT_ADDR(FN_MarkFRedraw), gFChar_rect);
    seg4_022454();
    return 0;
}

/* 000183f8: set up the kid and add him as a character object */
uint32_t FUN_000183f8(void) {
    if (R16(gCharFrame) == 0) return 0;
    FUN_00020542();
    FUN_00018138();
    FUN_00021fe8();
    GetCharEdges();
    IndexChar();
    QuickFloor();
    ClipChar();
    MarkBufsForRect(JT_ADDR(FN_MarkFRedraw), gFChar_rect);
    AddCharObj(R16(gCharType));
    return 0;
}

/* 0001843a: add weapon 6 as a character object */
uint32_t FUN_0001843a(void) {
    if (R16(gWeapon) != 6) return 0;
    W16(gFChar_shape, R16(gWeapon_shape));
    AddFCharX(R16(gWeapon_dx));
    W16(gFChar_y, R16(gFChar_y) + R16(gWeapon_dy));
    W16(gFChar_set, 3);
    MakeFCharClipRect();
    AddCharObj(9);
    MarkBufsForRect(JT_ADDR(FN_MarkFRedraw), gFChar_rect);
    return 0;
}

/* 00018484: add the sword as a character object */
uint32_t SetupSword(int16_t kind) {
    if (R16(gWeapon) != 1 || R16(gChar_fde) == 5) return 0;
    W16(gFChar_shape, R16(gWeapon_shape));
    AddFCharX(R16(gWeapon_dx));
    W16(gFChar_y, R16(gFChar_y) + R16(gWeapon_dy));
    W16(gFChar_set, 0);
    W16(gFChar_b4c, 0);
    MakeFCharClipRect();
    if (LEVEL_TYPE() == 5) seg23_06e646();
    AddCharObj(kind);
    if (R16(gCharAction) != 9) MarkBufsForRect(JT_ADDR(FN_MarkFRedraw), gFChar_rect);
    return 0;
}

/* 0001850c: add weapon 3 as a character object */
uint32_t FUN_0001850c(void) {
    if (R16(gWeapon) != 3) return 0;
    W16(gFChar_shape, R16(gWeapon_shape));
    W16(gFChar_b4c, 0);
    AddFCharX(R16(gWeapon_dx));
    W16(gFChar_y, R16(gFChar_y) + R16(gWeapon_dy));
    W16(gFChar_set, 1);
    MakeFCharClipRect();
    AddCharObj(3);
    MarkBufsForRect(JT_ADDR(FN_MarkFRedraw), gFChar_rect);
    return 0;
}

/* 0001855c: set up the current opponent and add him as a character object */
uint32_t SetupOpponent(void) {
    FUN_00020542();
    FUN_00018138();
    FUN_00021fe8();
    GetCharEdges();
    IndexChar();
    QuickFloor();
    ClipChar();
    int16_t t = R16(gCharType), kind;
    if (t == 7 || t == 8 || t == 10 || t == 6 || t == 1) {
        kind = R16(gCharType);
        if (R16(gCharType) == 10) seg4_01f79e();
    } else {
        kind = 2;
    }
    if (R16(gCharAction) != 9 && S16(IsCharNonViewable()) == 0)
        MarkBufsForRect(JT_ADDR(FN_MarkFRedraw), gCharRect);
    AddCharObj(kind);
    seg4_022516();
    return 0;
}

/* 00018600: peel the rect of a custom piece (id = piece shape + 100) */
uint32_t AddCustomPiecePeel(uint32_t piece, uint32_t rect) {
    int16_t id = (int16_t)(R16(piece) + 100);
    AddPeel(rect, id, R16(piece + 0x1a));
    return 0;
}

/* 00018640: level-specific palette for the room, release the cached handle */
uint32_t FUN_00018640(void) {
    uint32_t gs = R32(gGameState);
    switch (RU16(gs + 0x2186)) {
    case 5:
        FUN_0006e63a();
        break;
    case 4:
        if (R16(gCurRoom) == 2 && LEVEL_NUM() == 9) PoPSetPalette(0x6310, 1);
        else if (R16(gRoomLeft) == 2 && LEVEL_NUM() == 9) PoPSetPalette(0x14, 1);
        break;
    case 2:
        if (R16(gCurRoom) == 0x16 && LEVEL_NUM() == 10) PoPSetPalette(0x6311, 1);
        else if (R16(gRoomLeft) == 0x16 && LEVEL_NUM() == 10) PoPSetPalette(0x14, 1);
        break;
    case 6:
        if (R16(gCurRoom) == 1 && R16(gs + 0x218a) == 0xe) PoPSetPalette(0x6313, 1);
        else if ((R16(gRoomUp) == 1 && LEVEL_NUM() == 0xe) || (R16(gCurRoom) == 0xb && LEVEL_NUM() == 0xe))
            PoPSetPalette(0x14, 1);
        break;
    }
    if (R32(GH(0x10355c)) != 0) {
        _HPurge(R32(GH(0x10355c)));
        W32(GH(0x10355c), 0);
    }
    return 0;
}

/* 00018788: draw custom piece `idx` at the current item's cell */
uint32_t DrawCustomRoomImgAtInfo(int16_t idx) {
    uint32_t p = R32(gCustomRoom) + 0x1e + ((uint32_t)(uint16_t)idx << 5);
    uint32_t rect0 = R32(p + 0x10), rect1 = R32(p + 0x14);
    uint32_t pos = R32(p + 2);
    uint32_t item = R32(gCurBFItem);
    p = CustomPieceOffsetToColRow(p, R16(item + 0xe), R16(item + 0x10));
    int16_t layer0 = R16(p + 6);
    W16(p + 6, R16(R32(gCurBFItem) + 0xa));
    int16_t layer = R16(R32(gCurBFItem) + 0xa);
    DrawCustomRoomImg(idx, layer);
    if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(layer);
    W32(p + 2, pos);
    W16(p + 6, layer0);
    W32(p + 0x10, rect0);
    W32(p + 0x14, rect1);
    return 0;
}

/* 00018848: move a custom piece (position and rect) to cell (col, row) */
uint32_t CustomPieceOffsetToColRow(uint32_t piece, int16_t col, int16_t row) {
    int16_t dv = (int16_t)(row * 0x78 + 0x78);
    W16(piece + 2, R16(piece + 2) + dv);
    int16_t dh = (int16_t)(col * 0x33);
    W16(piece + 4, R16(piece + 4) + dh);
    _OffsetRect(piece + 0x10, dh, dv);
    return piece;
}

/* 000188a6: reset the 11 entries at 0x1035be */
uint32_t FUN_000188a6(void) {
    uint32_t e = GH(0x1035be);
    uint16_t i = 0;
    do {
        W16(e, 0);
        W16(e + 2, 0xc000);
        W16(e + 4, 0);
        i++;
        e += 6;
    } while (i < 0xb);
    return 0;
}

/* 000188ce: shape handle of custom piece `idx` */
uint32_t GetCustomPieceShape(int16_t idx) {
    if (R32(gCustomRoom) == 0) return 0;
    return R32(PIECE(idx) + 0xc);
}

/* 00018918: size {height, width} of custom piece `idx` into `out` */
uint32_t GetCustomPieceSize(int16_t idx, uint32_t out) {
    uint32_t r = 0;
    if (R32(gCustomRoom) != 0) {
        uint32_t p = R32(gCustomRoom) + 0x1e + ((uint32_t)(uint16_t)idx << 5);
        uint32_t shp = R32(R32(p + 0xc));
        if (shp != 0) {
            W16(out, R16(shp + 6));
            W16(out + 2, R16(shp + 4));
            r = out;
        }
    }
    return r;
}

/* 00018980: does the room have custom pieces (bits 14-15 of a cell info)? */
uint32_t IsCustomizedRoom(int16_t room) {
    uint32_t c = (uint32_t)(uint16_t)room * 0x78 + R32(gGameState) + 0x708;
    uint16_t bits, i = 0;
    do {
        bits = RU16(c) & 0xc000;
        i++;
        c += 4;
    } while (i < 0x1e && bits == 0);
    return bits != 0 ? 1 : 0;
}

/* 000189e8: load the room's CUST resource (or unload the custom data) */
uint32_t LoadCustomRoomData(int16_t room) {
    if (R16(gRoomCustomized) == 0) {
        if (R32(gCustomRoom) == 0) return 0;
        return FUN_00018f66();
    }
    int16_t id = (int16_t)(room * 0x19 + 0xf87);
    uint32_t h = PoPGetResource(0x43555354, id);   /* 'CUST' */
    _HNoPurge(h);
    if (h == 0) return 0;
    _MoveHHi(h);
    _HLock(h);
    uint32_t data = R32(h);
    if (R32(gCustomRoom) == 0 || R16(data) != R16(R32(gCustomRoom))) {
        LoadCustomRoomShapes(data);
        FUN_00018f66();
        W32(gCustomRoom, data);
        W32(gCustomRoomH, h);
        SetCustomRoomTemplate(R16(data));
    } else {
        LoadCustomRoomShapes(data);
    }
    uint32_t hook = R32(R32(gRoomTemplate));
    if (hook != 0) call_ptr(hook, "");
    W16(data + 6, 1);
    return 1;
}

/* 00018a9a: load the shapes of the pieces of `data`, reusing those of the loaded room */
uint32_t LoadCustomRoomShapes(uint32_t data) {
    FRAME(f, 8);
    uint32_t oldArr = 0;
    int16_t oldN = 0;
    if (R32(gCustomRoom) != 0) {
        uint32_t old = R32(gCustomRoom);
        if (R16(old + 2) == R16(data + 2)) {
            int16_t a = R16(old + 4), b = R16(data + 4);
            oldN = (int16_t)((a > b ? a : b) + 1);
            int16_t sz = (int16_t)(oldN << 2);
            oldArr = PoPNewPtr((uint32_t)(int32_t)sz);
            tc_memset(oldArr, 0, (uint32_t)(int32_t)sz);
            uint32_t p = R32(gCustomRoom) + 0x1e;
            int16_t i = 0;
            if (R16(R32(gCustomRoom) + 0x1c) > 0) {
                do {
                    uint32_t h = R32(p + 0xc);
                    if (h != 0) {
                        W32(oldArr + (int32_t)R16(p) * 4, h);
                        W32(p + 0xc, 0);
                    }
                    i++;
                    p += 0x20;
                } while (i < R16(R32(gCustomRoom) + 0x1c));
            }
        }
        if (R16(R32(gCustomRoom)) != R16(data)) FUN_00018f66();
    }
    int16_t base = R16(data + 2);
    int16_t sz = (int16_t)((int16_t)(R16(data + 4) << 2) + 4);
    uint32_t arr = PoPNewPtr((uint32_t)(int32_t)sz);
    tc_memset(arr, 0, (uint32_t)(int32_t)sz);
    uint32_t p = data + 0x1e;
    int16_t i = 0;
    if (R16(data + 0x1c) > 0) {
        do {
            int16_t s = R16(p);
            if (s != -1) {
                int32_t off = (int32_t)s * 4;
                if (R32(arr + off) == 0) {
                    if (oldArr != 0 && R32(off + oldArr) != 0) {
                        W32(arr + off, R32(off + oldArr));
                        W32(oldArr + (int32_t)R16(p) * 4, 0);
                    } else {
                        uint32_t h = GetShape((int16_t)(base + R16(p)));
                        if (h == 0)   /* "Shape not found in LoadCustomRoomShapes()" in the code segment */
                            Terminate(1, fn_address(FN_LoadCustomRoomShapes) + (0x18da6 - 0x18a9a));
                        _HNoPurge(h);
                        W32(arr + (int32_t)R16(p) * 4, h);
                    }
                }
                uint32_t h = R32(arr + (int32_t)R16(p) * 4);
                W32(p + 0xc, h);
                uint32_t shp = R32(h);
                int16_t y = (int16_t)(R16(p + 2) - R16(shp + 6));
                MakeShapeRect(f, y, R16(p + 4), shp);
                if (R16(data + 6) == 0) {
                    if (EMPTY_RECT(p + 0x10) != 0) {
                        mcopy(p + 0x10, f, 8);
                    } else {
                        _OffsetRect(p + 0x10, R16(p + 4), y);
                        SECT_RECT(f, p + 0x10, p + 0x10);
                    }
                }
                W16(p + 0xa, (int16_t)(base + i));
            } else {
                W32(p + 0xc, 0);
                mcopy(p + 0x10, GH(0x106694), 8);
                W16(p + 0xa, -1);
            }
            i++;
            p += 0x20;
        } while (i < R16(data + 0x1c));
    }
    if (oldArr != 0) {
        int16_t j = 0;
        if (j < oldN) {
            do {
                uint32_t h = R32(oldArr + (int32_t)j * 4);
                if (h != 0) _HPurge(h);
                j++;
            } while (j < oldN);
        }
        _DisposePtr(oldArr);
    }
    _DisposePtr(arr);
    FRAME_END(f);
    return 0;
}

/* 00018dd0: is the loaded custom room template 0x1f? (D0's high word is left from the pointer) */
uint32_t FUN_00018dd0(void) {
    uint32_t c = R32(gCustomRoom);
    if (c == 0) return 0;
    int16_t r = (R16(R32(gCustomRoom)) == 0x1f) ? 1 : 0;
    return (c & 0xffff0000u) | (uint16_t)r;
}

/* 00018df0: let the first peel of custom piece `idx` be dropped by Peel() */
uint32_t RestoreKeepCustomPeel(int16_t idx) {
    int16_t id = (int16_t)(idx + 100);
    int16_t pi = S16(GetPeelIdxForCharIdx(id, 1));
    if (pi != -1) W16(GH(0x10308e) + (int16_t)(pi * 0x12), 0);
    return 0xffff0000u | (uint16_t)pi;
}

/* 00018e62: add the peels of the custom pieces that need one */
uint32_t FUN_00018e62(void) {
    if (R32(gCustomRoom) == 0) return 0;
    uint32_t p = R32(gCustomRoom) + 0x1e;
    int16_t i = 0;
    int16_t n = R16(R32(gCustomRoom) + 0x1c);
    if (n <= 0) return (uint16_t)n;
    do {
        if (R16(p + 0x1a) == 2) AddCustomPiecePeel(p, p + 0x10);
        else if (R16(p + 0x1a) == 3) AddDsrtStoneUpPeels(p);
        i++;
        p += 0x20;
        n = R16(R32(gCustomRoom) + 0x1c);
    } while (i < n);
    return (uint16_t)n;
}

/* 00018ec6: recompute the rect of a custom piece from its shape */
uint32_t SetCustomPieceRect(uint32_t piece) {
    uint32_t h = R32(piece + 0xc);
    if (h != 0) {
        uint32_t shp = R32(h);
        int16_t y = (int16_t)(R16(piece + 2) - R16(shp + 6));
        MakeShapeRect(piece + 0x10, y, R16(piece + 4), shp);
    }
    return 0;
}

/* 00018f1a: select the room template (load/unload hooks) for custom room `id` */
uint32_t SetCustomRoomTemplate(int16_t id) {
    if ((uint16_t)id < 0x23) {
        uint32_t t = R32(gTemplates + (int16_t)((uint16_t)id << 2));
        W32(gRoomTemplate, t);
        return t;
    }
    W32(gRoomTemplate, gDefTemplate);
    return 0x23;
}

/* 00018f66: unload the current custom room */
uint32_t FUN_00018f66(void) {
    if (R32(gCustomRoom) == 0) return 0;
    UnloadCustomRoomShapes(1);
    uint32_t c = R32(gCustomRoom);
    UnloadSounds(R16(c + 8), R16(c + 0xa));
    uint32_t hook = R32(R32(gRoomTemplate) + 4);
    if (hook != 0) call_ptr(hook, "");
    _HUnlock(R32(gCustomRoomH));
    _HPurge(R32(gCustomRoomH));
    W32(gCustomRoom, 0);
    W32(gCustomRoomH, 0);
    return SetCustomRoomTemplate(0x24);
}

/* 00018fc8: release the piece shapes (all, or only those of layer 0 without +18) */
uint32_t UnloadCustomRoomShapes(int16_t all) {
    if (R32(gCustomRoom) == 0) return 0;
    int16_t sz = (int16_t)((int16_t)(R16(R32(gCustomRoom) + 4) << 1) + 2);
    uint32_t done = PoPNewPtr((uint32_t)(int32_t)sz);
    tc_memset(done, 0, (uint32_t)(int32_t)sz);
    uint32_t p = R32(gCustomRoom) + 0x1e;
    int16_t i = 0;
    if (R16(R32(gCustomRoom) + 0x1c) > 0) {
        do {
            uint32_t h = R32(p + 0xc);
            if (h != 0 && (all != 0 || (R16(p + 6) == 0 && R16(p + 0x18) == 0))) {
                if (R16(done + (int32_t)R16(p) * 2) == 0) {
                    _HPurge(h);
                    W16(done + (int32_t)R16(p) * 2, 1);
                }
                W32(p + 0xc, 0);
            }
            if (all == 0 && R16(p + 6) == 0xb) {
                uint32_t c = R32(gCustomRoom);
                if (R16(c) == 0x21) {
                    if ((int32_t)R16(c + 0x1c) - 1 == (int32_t)i)
                        MarkBufsForRect(JT_ADDR(FN_MarkMove), GH(0x104e60));
                } else {
                    MarkBufsForRect(JT_ADDR(FN_MarkMove), p + 0x10);
                }
            }
            i++;
            p += 0x20;
        } while (i < R16(R32(gCustomRoom) + 0x1c));
    }
    return _DisposePtr(done);
}

/* 0001910c: clear the tables and draw the whole room */
uint32_t FUN_0001910c(void) {
    W16(gPeelCount, 0);
    FUN_00017d9e();
    FUN_000192d4();
    FUN_00026292();
    return 0;
}

/* 00019120: set the neighbour rooms of gCurRoom */
uint32_t seg3_019120(void) {
    W16(gRoomDownRight, 0);
    W16(gRoomDownLeft, 0);
    W16(gRoomUpRight, 0);
    W16(gRoomUpLeft, 0);
    if (R16(gCurRoom) == 0) {
        W16(gRoomDown, 0);
        W16(gRoomUp, 0);
        W16(gRoomRight, 0);
        W16(gRoomLeft, 0);
        return 0;
    }
    CalcBlue(R16(gCurRoom));
    W16(gRoomLeft, R16(ROOM_LINK(R16(gCurRoom), 0)));
    W16(gRoomRight, R16(ROOM_LINK(R16(gCurRoom), 1)));
    W16(gRoomUp, R16(ROOM_LINK(R16(gCurRoom), 2)));
    W16(gRoomDown, R16(ROOM_LINK(R16(gCurRoom), 3)));
    if (R16(gRoomUp) != 0) {
        W16(gRoomUpLeft, R16(ROOM_LINK(R16(gRoomUp), 0)));
        W16(gRoomUpRight, R16(ROOM_LINK(R16(gRoomUp), 1)));
    } else {
        if (R16(gRoomLeft) != 0) W16(gRoomUpLeft, R16(ROOM_LINK(R16(gRoomLeft), 2)));
        if (R16(gRoomRight) != 0) W16(gRoomUpRight, R16(ROOM_LINK(R16(gRoomRight), 2)));
    }
    if (R16(gRoomDown) != 0) {
        W16(gRoomDownLeft, R16(ROOM_LINK(R16(gRoomDown), 0)));
        W16(gRoomDownRight, R16(ROOM_LINK(R16(gRoomDown), 1)));
    } else {
        if (R16(gRoomLeft) != 0) W16(gRoomDownLeft, R16(ROOM_LINK(R16(gRoomLeft), 3)));
        if (R16(gRoomRight) != 0) W16(gRoomDownRight, R16(ROOM_LINK(R16(gRoomRight), 3)));
    }
    W16(gRoomCustomized, IsCustomizedRoom(R16(gCurRoom)));
    return 0;
}

/* 000192d4: draw the whole room: rows 2..0, then the bottom row of the room above */
uint32_t FUN_000192d4(void) {
    seg5_024cd0();
    W16(gDrawRow, 3);
    int16_t r = 3;
    W16(gDrawRow, R16(gDrawRow) - 1);
    while (r != 0) {
        GetAboveAry();
        GetBelowAry();
        W16(gDrawCol, 0);
        do {
            FUN_00024bcc();
            FUN_00019382();
            W16(gDrawCol, R16(gDrawCol) + 1);
        } while (R16(gDrawCol) < 10);
        r = R16(gDrawRow);
        W16(gDrawRow, R16(gDrawRow) - 1);
    }
    if (R16(gRoomCustomized) != 0) {
        FUN_000188a6();
        return 0;
    }
    int16_t room = R16(gCurRoom);
    W16(gCurRoom, R16(gRoomUp));
    seg3_019120();
    W16(gRoomDown, room);
    seg5_024cd0();
    W16(gDrawRow, 2);
    GetAboveAry();
    GetBelowAry();
    W16(gDrawRow, -1);
    W16(gDrawCol, 0);
    do {
        FUN_00024bcc();
        FUN_00019382();
        W16(gDrawCol, R16(gDrawCol) + 1);
    } while (R16(gDrawCol) < 10);
    W16(gCurRoom, room);
    seg3_019120();
    return 0;
}

/* 00019382: draw the current cell (back layers 0, 5, custom 11, then the foreground) */
uint32_t FUN_00019382(void) {
    DrawBackImg(0);
    DrawBackImg(5);
    if (R32(gCustomRoom) != 0) DrawBackImg(0xb);
    DrawForeImg();
    return 0;
}

/* 000193aa: redraw what is flagged for cell `cell` */
uint32_t RedrawCellFast(int16_t cell) {
    FRAME(f, 8);
    uint32_t a = R32(gRedrawFull) + (int32_t)cell * 2;
    if (R16(a) != 0) {
        W16(a, 0);
        FUN_00019382();
    } else {
        int32_t o = (int32_t)cell * 10;
        a = o + R32(gRedrawBack);
        if (R16(a) != 0) {
            W16(a, 0);
            mcopy(f, gDrawRect, 8);
            if (SECT_RECT(o + R32(gRedrawBack) + 2, gDrawRect, gDrawRect) != 0) {
                DrawBackImg(0);
                DrawBackImg(5);
                if (R32(gCustomRoom) != 0) DrawBackImg(0xb);
                DrawForeImg();
            }
            mcopy(gDrawRect, f, 8);
        }
    }
    int32_t o = (int32_t)cell * 10;
    a = o + R32(gRedrawFloor);
    if (R16(a) != 0) {
        W16(a, 0);
        if (R16(R32(gRedrawHalf) + (int32_t)cell * 2) != 0 && ComputeHalfRect(f) != 0) {
            uint32_t r = o + R32(gRedrawFloor) + 2;
            UNION_RECT(f, r, r);
        }
        mcopy(f, gDrawRect, 8);
        if (SECT_RECT(R32(gRedrawFloor) + (int32_t)cell * 10 + 2, gDrawRect, gDrawRect) != 0) DrawFloorBuf();
        mcopy(gDrawRect, f, 8);
    } else {
        a = R32(gRedrawHalf) + (int32_t)cell * 2;
        if (R16(a) != 0) {
            W16(a, 0);
            DrawHalf();
        }
    }
    if (R16(R32(gRedrawObj) + (int32_t)cell * 2) != 0) {
        DrawObjs(cell);
        W16(R32(gRedrawObj) + (int32_t)cell * 2, 0);
    }
    o = (int32_t)cell * 10;
    a = o + R32(gRedrawFore);
    if (R16(a) != 0) {
        W16(a, 0);
        if (SECT_RECT(o + R32(gRedrawFore) + 2, gDrawRect, gDrawRect) != 0) DrawForeImg();
    }
    FRAME_END(f);
    return 0;
}

/* 000195be: redraw the top line (row -1) of cell `cell` within its flagged rect */
uint32_t RedrawTopFast(int16_t cell) {
    int32_t o = (int32_t)cell * 10;
    uint32_t a = o + R32(gRedrawTop);
    if (R16(a) != 0) {
        W16(a, 0);
        int16_t row = R16(gDrawRow);
        uint32_t s0 = R32(gDrawRect), s1 = R32(gDrawRect + 4);
        mcopy(gDrawRect, o + R32(gRedrawTop) + 2, 8);
        W16(gDrawRow, -1);
        DrawBackImg(0);
        DrawBackImg(5);
        if (R32(gCustomRoom) != 0) DrawBackImg(0xb);
        DrawForeImg();
        W16(gDrawRow, row);
        W32(gDrawRect, s0);
        W32(gDrawRect + 4, s1);
    }
    return 0;
}

/* 00019664: draw the background passes of the current cell into layer `layer` */
uint32_t DrawBackImg(int16_t layer) {
    int16_t pass = S16(GetBackImgStartPass());
    ResetNextBFItem(layer);
    if (R16(gFullRedraw) != 0) mcopy(gDrawRect, gScreenRect, 8);
    do {
        CheckNextBFItem(layer);
        switch ((uint16_t)pass) {
        case 0:   /* the cell below-left */
            W16(R32(gCurBFItem) + 0xe, R16(gDrawCol) - 1);
            W16(R32(gCurBFItem) + 0x10, R16(gDrawRow) + 1);
            W16(R32(gCurBFItem) + 0x12, R16(gBelowAry + (int16_t)((int32_t)R16(gDrawCol) * 6)));
            W32(R32(gCurBFItem) + 0x14, R32(gBelowAry + 2 + (int16_t)((int32_t)R16(gDrawCol) * 6)));
            if (R16(gFullRedraw) != 0 && R16(gDrawCol) != 0 && R16(gDrawCol) != 9) {
                uint32_t gs = R32(gGameState);
                int16_t lt = R16(gs + 0x2186);
                if (lt != 3) {
                    int16_t pc = R16(R32(gCurBFItem) + 0x12);
                    if (pc != 4 && R16(gs + 0x218a) != 0xd) {
                        if (!(lt == 2 && (pc == 0x13 || pc == 9))) pass += 2;
                    }
                }
            }
            break;
        case 1:   /* the cell below */
            W16(R32(gCurBFItem) + 0xe, R16(gDrawCol));
            W16(R32(gCurBFItem) + 0x10, R16(gDrawRow) + 1);
            W16(R32(gCurBFItem) + 0x12, R16(gBelowAry + 6 + (int16_t)((int32_t)R16(gDrawCol) * 6)));
            W32(R32(gCurBFItem) + 0x14, R32(gBelowAry + 8 + (int16_t)((int32_t)R16(gDrawCol) * 6)));
            if (S16(IsWall(R16(R32(gCurBFItem) + 0x12))) == 0) break;
            pass++;
            /* fall through */
        case 2:   /* the cell to the left */
            W16(R32(gCurBFItem) + 0xe, R16(gDrawCol) - 1);
            W16(R32(gCurBFItem) + 0x10, R16(gDrawRow));
            W16(R32(gCurBFItem) + 0x12, R16(gLeftPiece));
            W32(R32(gCurBFItem) + 0x14, R32(gLeftInfo));
            break;
        case 3:   /* the cell itself */
            DrawCustomRoomImgs(layer);
            W16(R32(gCurBFItem) + 0xe, R16(gDrawCol));
            W16(R32(gCurBFItem) + 0x10, R16(gDrawRow));
            W16(R32(gCurBFItem) + 0x12, R16(gCellPiece));
            W32(R32(gCurBFItem) + 0x14, R32(gCellInfo));
            break;
        }
        uint32_t it = R32(gCurBFItem);
        int16_t k = (int16_t)((RU16(it + 0x14) & 0xc000) >> 14);
        int draw;
        if (R32(gCustomRoom) == 0 || k == 0) draw = 1;
        else if (layer == k) draw = 1;
        else draw = (k == 2 && layer == 5);
        if (draw) {
            uint32_t fp = R32(gPieceDrawFns) + (uint32_t)RU16(it + 0x12) * 4;
            if (R32(fp) != 0) call_ptr(R32(R32(gPieceDrawFns) + (uint32_t)RU16(it + 0x12) * 4), "");
        }
        pass++;
    } while ((uint16_t)pass < 4);
    return 0;
}

/* 000198ee: first background pass needed for the current cell (0..3) */
uint32_t GetBackImgStartPass(void) {
    int full;
    if (R16(gFullRedraw) == 0) {
        full = 0;
    } else {
        int16_t pc = R16(gCellPiece);
        if (pc != 7 && S16(IsWall(pc)) != 0 && (pc = R16(gLeftPiece)) != 7 && S16(IsWall(pc)) != 0)
            full = 1;
        else if (R16(gDrawCol) == 0)
            full = 0;
        else
            full = R16(gDrawRow) != 2;
    }
    int16_t r;
    if (full) r = 3;
    else if (R16(gFullRedraw) != 0 && R16(gDrawCol) == 0 && R16(gDrawRow) != 2) r = 2;
    else if (R32(gCustomRoom) != 0 && R16(gDrawRow) == 2 && S16(FUN_00018dd0()) == 0) r = 2;
    else r = 0;
    return (uint16_t)r;
}

/* 00019994: draw the custom pieces (and the horse) of layer `layer` when due */
uint32_t DrawCustomRoomImgs(int16_t layer) {
    int16_t go = 0;
    if ((layer == 0 || layer == 5) && R16(gFullRedraw) != 0 && R16(gDrawCol) == 0 && R16(gDrawRow) == 2) {
        go = 1;
        if (R16(gCurRoom) == 1 && LEVEL_NUM() == 0xe) seg9_0395fc();
    } else if (layer == 0 && R16(gFullRedraw) == 0 && R32(gCustomRoom) != 0 &&
               R16(R32(gCustomRoom) + 0xe) != 0) {
        go = 1;
    } else if (layer == 1 || layer == 2) {
        if ((R16(gDrawCol) == 9 && R16(gDrawRow) == 0) || R16(gFullRedraw) == 0) go = 1;
    }
    if (go) {
        if (R32(gCustomRoom) != 0) {
            int16_t i = 0;
            if (R16(R32(gCustomRoom) + 0x1c) > 0) {
                do {
                    DrawCustomRoomImg(i, layer);
                    i++;
                } while (i < R16(R32(gCustomRoom) + 0x1c));
            }
        }
        if ((R16(gFullRedraw) != 0 && layer == 0) || layer == 1) {
            uint32_t h = GetHorsePtInPlane(R16(gCurRoom), layer);
            if (h != 0) DrawHorse(h, layer);
        }
    }
    return 0;
}

/* 00019aa8: add custom piece `idx` as a BF item of `layer` if it shows in gDrawRect */
uint32_t DrawCustomRoomImg(int16_t idx, int16_t layer) {
    FRAME(f, 8);
    uint32_t p = R32(gCustomRoom) + 0x1e + (int32_t)idx * 32;
    if (R16(p + 0xa) == -1 || R16(p + 6) != layer) goto out;
    if (SECT_RECT(p + 0x10, gDrawRect, f) == 0) goto out;
    uint32_t it;
    if (layer == 0 || layer == 5 || layer == 0xb) {
        if (R32(p + 0xc) == 0) goto out;
        it = R32(gBackTable) + (int32_t)R16(gBackCount) * 0x26;
        W16(gBackCount, R16(gBackCount) + 1);
    } else {
        it = R32(gForeTable) + (int32_t)R16(gForeCount) * 0x26;
        W16(gForeCount, R16(gForeCount) + 1);
    }
    W16(it, 4);
    W16(it + 2, R16(gDrawCol));
    W16(it + 4, R16(gDrawRow));
    W16(it + 6, R16(p + 8));
    W16(it + 8, 0);
    W16(it + 0xc, R16(p + 0x1a));
    W16(it + 0xe, R16(R32(gCurBFItem) + 0xe));
    W16(it + 0x10, R16(R32(gCurBFItem) + 0x10));
    W16(it + 0x12, R16(R32(gCurBFItem) + 0x12));
    W32(it + 0x14, R32(R32(gCurBFItem) + 0x14));
    W16(it + 0x18, R16(p + 0xa));
    W16(it + 0x1a, R16(p + 4));
    uint32_t shp = R32(R32(p + 0xc));
    W16(it + 0x1c, R16(p + 2) - R16(shp + 6));
    mcopy(it + 0x1e, f, 8);
    W32(gCurBFItem, 0);
out:
    FRAME_END(f);
    return 0;
}

/* 00019c08: draw the foreground passes of the current cell */
uint32_t DrawForeImg(void) {
    ResetNextBFItem(1);
    CheckGateInFore();
    int16_t lt = R16(R32(gGameState) + 0x2186);
    int16_t pass;
    uint16_t m;
    if (lt == 5 && R16(gLeftPiece) == 1 && R16(gCellPiece) == 0) pass = 0;
    else if (lt == 4 && (m = RU16(gLeftInfo) & 0xf00) != 0 && m < 0xd00) pass = 0;
    else if (lt == 2 && R16(gLeftPiece) == 8) pass = 0;
    else pass = 1;
    do {
        CheckNextBFItem(1);
        if (pass == 0) {   /* the cell to the left */
            W16(R32(gCurBFItem) + 0xe, R16(gDrawCol) - 1);
            W16(R32(gCurBFItem) + 0x10, R16(gDrawRow));
            W16(R32(gCurBFItem) + 0x12, R16(gLeftPiece));
            W32(R32(gCurBFItem) + 0x14, R32(gLeftInfo));
        } else if (pass == 1) {   /* the cell itself */
            W16(R32(gCurBFItem) + 0xe, R16(gDrawCol));
            W16(R32(gCurBFItem) + 0x10, R16(gDrawRow));
            W16(R32(gCurBFItem) + 0x12, R16(gCellPiece));
            W32(R32(gCurBFItem) + 0x14, R32(gCellInfo));
        }
        uint32_t it = R32(gCurBFItem);
        int16_t k = (int16_t)((RU16(it + 0x14) & 0xc000) >> 14);
        if (R32(gCustomRoom) == 0 || k == 0 || k == 1 || k == 2) {
            uint32_t fp = R32(gPieceDrawFns) + (uint32_t)RU16(it + 0x12) * 4;
            if (R32(fp) != 0) call_ptr(R32(R32(gPieceDrawFns) + (uint32_t)RU16(it + 0x12) * 4), "");
        }
        pass++;
    } while ((uint16_t)pass < 2);
    DrawCustomRoomImgs(1);
    return 0;
}

/* gate test: is the character entry's column just right of the cell and its state not 2/6? */
static int16_t gate_hit(uint32_t colp, uint32_t statep) {
    int32_t c = (int32_t)R16(gDrawCol);
    int32_t oc = (int32_t)R16(colp);
    if (c - 1 == oc || c - 2 == oc) {
        int16_t s = R16(statep);
        if (s != 2 && s != 6) return 1;
    }
    return 0;
}

/* 00019d54: draw a gate in the left cell in front of a character standing under it */
uint32_t CheckGateInFore(void) {
    uint16_t g = RU16(GH(0x102f38));
    if ((g >= 0x108 && g <= 0x10a) || R32(GH(0x10354e)) == 0) {
        if (LEVEL_TYPE() == 3) {
            W16(R32(gCurBFItem) + 0x10, R16(gDrawRow));
            W16(R32(gCurBFItem) + 0x12, 4);
            if (R16(gCellPiece) == 4) {
                W16(R32(gCurBFItem) + 0xe, R16(gDrawCol));
                W32(R32(gCurBFItem) + 0x14, R32(gCellInfo));
                FUN_000360d2();
            } else if (R16(gLeftPiece) == 4) {
                W16(R32(gCurBFItem) + 0xe, R16(gDrawCol) - 1);
                W32(R32(gCurBFItem) + 0x14, R32(gLeftInfo));
                FUN_000360d2();
            }
            if (R32(gCurBFItem) == 0) ResetNextBFItem(1);
        }
        return 0;
    }
    if (R16(gLeftPiece) != 4) return 0;
    uint32_t save0 = R32(gDrawRect), save1 = R32(gDrawRect + 4);
    int16_t hit = 0;
    uint32_t obj = 0;
    if (R16(GH(0x102f3c)) == R16(gDrawRow)) hit = gate_hit(GH(0x102f3a), GH(0x102f3e));
    if (hit != 0) {
        obj = GetObjTableType(2, 0);
    } else {
        uint16_t room = RU16(gCurRoom);
        int16_t n = room ? R16(R32(gGameState) + (uint32_t)room * 0xc0 + 0x20e6) : 0;
        if (n != 0) {
            uint32_t ent = gCharTable;
            int16_t i = 0;
            if (n > 0) {
                uint32_t state = GH(0x102e08), col = GH(0x102e04), row = GH(0x102e06), type = GH(0x102e00);
                do {
                    if (R16(type) != 0xb) {
                        if (R16(gLeftPiece) == 4 && R16(row) == R16(gDrawRow)) hit = gate_hit(col, state);
                        else hit = 0;
                    }
                    i++;
                    state += 0x3e; col += 0x3e; row += 0x3e; type += 0x3e; ent += 0x3e;
                } while (hit == 0 && i < n);
            }
            /* (the entry pointer has already moved past the matching entry here) */
            if (hit != 0) obj = GetObjTableType(3, R16(ent + 8));
        }
    }
    if (hit != 0) {
        W16(R32(gCurBFItem) + 0xe, R16(gDrawCol) - 1);
        W16(R32(gCurBFItem) + 0x10, R16(gDrawRow));
        W16(R32(gCurBFItem) + 0x12, R16(gLeftPiece));
        W32(R32(gCurBFItem) + 0x14, R32(gLeftInfo));
        if (LEVEL_TYPE() == 2) W16(gDrawRect_left, R16(gDrawRect_left) - 1);
        if (R16(gFullRedraw) != 0)
            W16(gDrawRect, (int16_t)(R16(R32(gCurBFItem) + 0x10) * 0x78) + 5);
        else if (obj != 0)
            SECT_RECT(obj + 0xe, gDrawRect, gDrawRect);
        if (EMPTY_RECT(gDrawRect) == 0) {
            W16(R32(gCurBFItem) + 0xa, 2);
            call_ptr(R32(GH(0x10354e)), "");
            if (R32(gCurBFItem) == 0) ResetNextBFItem(2);
            W16(R32(gCurBFItem) + 0xa, 1);
        }
    }
    W32(gDrawRect, save0);
    W32(gDrawRect + 4, save1);
    return 0;
}

/* 0001a026: make sure there is a current BF item */
uint32_t CheckNextBFItem(int16_t layer) {
    uint32_t it = R32(gCurBFItem);
    if (it != 0) return it;
    ResetNextBFItem(layer);
    return 0;
}

/* 0001a052: start a new BF item for the current cell in the table of `layer` */
uint32_t ResetNextBFItem(int16_t layer) {
    if (layer == 1 || layer == 2) {
        if (R16(gForeCount) >= 0x78) W32(gCurBFItem, R32(gForeTable) + 0x11aa);
        else W32(gCurBFItem, R32(gForeTable) + (int32_t)R16(gForeCount) * 0x26);
    } else {
        if (R16(gBackCount) >= 0x82) W32(gCurBFItem, R32(gBackTable) + 0x1326);
        else W32(gCurBFItem, R32(gBackTable) + (int32_t)R16(gBackCount) * 0x26);
    }
    W16(R32(gCurBFItem), 4);
    W16(R32(gCurBFItem) + 2, R16(gDrawCol));
    W16(R32(gCurBFItem) + 4, R16(gDrawRow));
    W16(R32(gCurBFItem) + 6, 0x10);
    W16(R32(gCurBFItem) + 8, 0);
    W16(R32(gCurBFItem) + 0xa, layer);
    W16(R32(gCurBFItem) + 0xc, 0);
    return 0;
}

/* 0001a134: start a new BF item that keeps the cell fields of the previous one */
uint32_t ResetNextBFItemWithPrevInfo(int16_t layer) {
    ResetNextBFItem(layer);
    uint32_t prev;
    if (layer == 1 || layer == 2) prev = R32(gForeTable) + (int32_t)R16(gForeCount) * 0x26 - 0x26;
    else prev = R32(gBackTable) + (int32_t)R16(gBackCount) * 0x26 - 0x26;
    TransferBFItemMidFields(prev, R32(gCurBFItem));
    return 0;
}

/* 0001a1cc: copy col, row, piece and info from one BF item to another */
uint32_t TransferBFItemMidFields(uint32_t src, uint32_t dst) {
    W16(dst + 0xe, R16(src + 0xe));
    W16(dst + 0x10, R16(src + 0x10));
    W16(dst + 0x12, R16(src + 0x12));
    W32(dst + 0x14, R32(src + 0x14));
    return 0;
}

/* 0001a220: draw a table: 0 background, 3 mid items, 1 foreground */
uint32_t DumpTable(int16_t which) {
    switch ((uint16_t)which) {
    case 0: FUN_0001a25e(); break;
    case 3: FUN_0001a2e0(); break;
    case 1: FUN_0001a338(); break;
    }
    return 0;
}

/* 0001a25e: draw the background items (those without +12 first) */
uint32_t FUN_0001a25e(void) {
    int16_t peeled = (R16(gFullRedraw) != 0 && R32(gCustomRoom) != 0) ? 0 : 1;
    int16_t n = R16(gBackCount);
    uint32_t it = R32(gBackTable);
    int16_t i = 0;
    if (R16(gBackCount) > 0) {
        do {
            if (peeled == 0 && R16(it + 0xc) != 0) {
                FUN_00018e62();
                peeled = 1;
            }
            if (R16(it + 0x12) == 0) DrawBFItem(it);
            i++;
            it += 0x26;
        } while (i < n);
    }
    it = R32(gBackTable);
    i = 0;
    if (n > 0) {
        do {
            if (R16(it + 0x12) != 0) DrawBFItem(it);
            i++;
            it += 0x26;
        } while (i < n);
    }
    return 0;
}

/* 0001a2e0: draw the mid items */
uint32_t FUN_0001a2e0(void) {
    uint16_t n = RU16(gMidCount);
    uint32_t it = R32(gMidTable);
    uint16_t i = 0;
    if (R16(gMidCount) == 0) return 0;
    do {
        DrawMidItem(it);
        i++;
        it += 0x18;
    } while (i < n);
    return 0x18;
}

/* 0001a30c: draw the second mid item table */
uint32_t FUN_0001a30c(void) {
    uint16_t n = RU16(gMidCount2);
    uint32_t it = R32(gMidTable2);
    uint16_t i = 0;
    if (R16(gMidCount2) == 0) return 0;
    do {
        DrawMidItem(it);
        i++;
        it += 0x18;
    } while (i < n);
    return 0x18;
}

/* 0001a338: draw the foreground items (special rooms have their own order) */
uint32_t FUN_0001a338(void) {
    int16_t n = R16(gForeCount);
    int16_t room = R16(gCurRoom);
    if ((room == 1 && LEVEL_NUM() == 0xe) || (room == 6 && LEVEL_TYPE() == 6) ||
        ((room == 7 || room == 8) && LEVEL_TYPE() == 6)) {
        FUN_00037e56();
        return 0;
    }
    uint32_t it;
    int16_t i;
    if (R16(gCurRoom) == 0x16 && LEVEL_NUM() == 10) {
        /* everything but the horse first, then the horse */
        it = R32(gForeTable);
        i = 0;
        if (R16(gForeCount) > 0) {
            do {
                if (R16(it + 0x18) != 0x6310) DrawBFItem(it);
                i++;
                it += 0x26;
            } while (i < n);
        }
        it = R32(gForeTable);
        i = 0;
        if (n > 0) {
            do {
                if (R16(it + 0x18) == 0x6310) DrawBFItem(it);
                i++;
                it += 0x26;
            } while (i < n);
        }
    } else {
        it = R32(gForeTable);
        i = 0;
        if (R16(gForeCount) > 0) {
            do {
                DrawBFItem(it);
                i++;
                it += 0x26;
            } while (i < n);
        }
    }
    return 0;
}

/* 0001a41a: draw one BF item (shape from its set, the horse, the carpet or a custom piece) */
uint32_t DrawBFItem(uint32_t item) {
    FRAME(f, 8);
    int16_t extra = 0;
    uint32_t piece = 0, h = 0, shp, mask;
    int16_t shape = R16(item + 0x18);
    uint16_t type = RU16(item);
    uint32_t set = SET_OF(type);
    if (shape <= R16(set + 6)) {
        if (type == 4 && LEVEL_TYPE() == 5 && R16(gFullRedraw) != 0 && shape == 0x7a) {
            h = (set == R32(gBGSetShapes)) ? GetBGSetShapeItem(shape) : PrinceGetnShapeItem(set, shape);
            shp = R32(h);
            int16_t x = R16(item + 0x1a), y = R16(item + 0x1c);
            _SetRect(f, x, y, (int16_t)(x + R16(shp + 4) + 4), (int16_t)(y + R16(shp + 6) + 0x1c));
            AddPeel(f, (int16_t)(R16(item + 0x18) + 0xc8), 2);
        }
        set = SET_OF(RU16(item));
        h = (set == R32(gBGSetShapes)) ? GetBGSetShapeItem(R16(item + 0x18))
                                        : PrinceGetnShapeItem(set, R16(item + 0x18));
        _HLock(h);
        shp = R32(h);
        if (shp == 0) goto out;
    } else {
        if (R32(gCustomRoom) == 0 && R16(item + 0x18) != 0x6310) goto out;
        if (R16(item + 0x18) == 0x6310) {   /* the horse */
            h = GetHorseShape();
            _HLock(h);
            shp = R32(h);
        } else {
            shape = R16(item + 0x18);
            if (shape >= 0x62d6 && shape <= 0x62e7) {   /* the carpet */
                SaveUnderCarpet();
                h = GetCarpetShape((int16_t)(R16(item + 0x18) - 0x62d6));
                _HLock(h);
                shp = R32(h);
                W16(item + 8, 0);
            } else {   /* a custom piece */
                if (R32(gCustomRoom) == 0) goto out;
                if (R32(GH(0x105480)) != 0) {
                    shape = R16(item + 0x18);
                    if (shape >= 0xfed && shape <= 0x101a) extra = R16(GH(0x105490));
                }
                int32_t d = (int32_t)R16(item + 0x18);
                uint32_t c = R32(gCustomRoom);
                d = (d - (int32_t)R16(c + 2)) << 5;
                piece = c + 0x1e + (uint32_t)d;
                if (R16(piece + 0x1a) == 1) AddCustomPiecePeel(piece, item + 0x1e);
                shp = R32(R32(piece + 0xc));
                W16(item + 8, 0);
            }
        }
    }
    FlipSHAP(shp, R16(item + 8) != 0 ? 1 : 0);
    if ((uint16_t)(RU16(shp) & 0xff80) == 0x9000) {
        mask = shp;
        shp = 0;
        extra = R16(item + 6);
    } else {
        mask = 0;
    }
    CLIP_RECT(item + 0x1e);
    DrawIt(shp, mask, R16(item + 0x1a), R16(item + 0x1c), R16(item + 6), extra, f);
    CheckAddDRect(f);
    CLIP_RECT(gScreenRect);
    if (piece != 0 && R16(piece + 6) == 0 && R16(piece + 0x18) == 0) _HNoPurge(R32(piece + 0xc));
    else if (h != 0) _HUnlock(h);
out:
    FRAME_END(f);
    return 0;
}

/* 0001a6ea: draw one mid item */
uint32_t DrawMidItem(uint32_t item) {
    FRAME(f, 8);
    int16_t type = R16(item + 4);
    int16_t shape = R16(item + 6);
    uint32_t h;
    if (shape >= 0x62d6 && shape <= 0x62e7) {
        h = GetCarpetShape((int16_t)(shape - 0x62d8));
    } else if (LEVEL_NUM() == 5 && R16(gCurRoom) == 10 && type == 4 &&
               R16(item + 6) < R16(R32(gCustomRoom) + 0x1c)) {
        h = GetCustomPieceShape(R16(item + 6));
    } else if (R32(GH(0x105480)) != 0 && (shape = R16(item + 6)) >= 0xfed && shape <= 0x101a) {
        h = GetFlameImgHnd(R16(item + 6));
    } else {
        uint32_t set = R32(gShapeSets + (int16_t)((int32_t)type * 4));
        if (set == R32(gBGSetShapes)) h = GetBGSetShapeItem((int16_t)(R16(item + 6) + 1));
        else h = PrinceGetnShapeItem(set, (int16_t)(R16(item + 6) + 1));
    }
    uint8_t state = (uint8_t)_HGetState(h);
    _HLock(h);
    uint32_t shp = R32(h);
    int16_t x = R16(item);
    if (R16(item + 0x14) != 0) x = (int16_t)(x - R16(shp + 4));
    int16_t mode = R16(item + 0x12);
    if (type == 2 && mode == 0) mode = 0x10;
    FlipSHAP(shp, R16(item + 0x14) != 0 ? 1 : 0);
    int16_t w = R16(shp + 4);
    if ((int32_t)x + (int32_t)w < (int32_t)R16(item + 0xe)) W16(item + 0xe, (int16_t)(x + w));
    AddPeel(item + 8, R16(item + 0x16), 0);
    CLIP_RECT(item + 8);
    DrawIt(shp, 0, x, R16(item + 2), R16(item + 0x10), mode, f);
    CheckAddDRect(f);
    CLIP_RECT(gScreenRect);
    _HSetState(h, state);
    FRAME_END(f);
    return 0;
}
