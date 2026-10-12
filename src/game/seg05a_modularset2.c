/* CODE 5 "Modular Set 2", first half (0x24184-0x2746a): room/cell geometry, the
 * neighbour-piece lookups used while drawing, the animated-object list ("trobs":
 * torches, gates, plates, exits, flasks, loose floors...), the redraw-buffer marking
 * (MarkMove/MarkFloor/...), triggers and links (pressure plates -> gates), and the
 * moving-object list ("mobs": falling loose floors).
 * Ported from the Ghidra decompilation/disassembly of the original 68k code; addresses
 * in comments are Ghidra addresses (see docs/PORTING.md). */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* ---- level / rooms ---- */
#define gGameState      GH(0x102fec)   /* long: pointer to the game state block */
#define gCurRoom        GH(0x10303e)   /* word: current room */
#define gRoomLeft       GH(0x103040)   /* word: rooms around the current one */
#define gRoomRight      GH(0x103042)
#define gRoomAbove      GH(0x103044)
#define gRoomBelow      GH(0x103046)
#define gRoomAboveL     GH(0x103048)
#define gRoomAboveR     GH(0x10304a)
#define gRoomBelowL     GH(0x10304c)
#define gRoomBelowR     GH(0x10304e)
#define gTopRowExtra    GH(0x103050)   /* word: when set, row 0 extends 5 pixels higher (see ComputeClipRect) */
#define gExitRoom       GH(0x10303c)   /* word: room compared with the kid's room by TriggerGate */
#define gPressed        GH(0x103062)   /* word: set to 1 when a plate/loose floor was stepped on */

#define gRoomTiles      GH(0x1037e0)   /* long: -> 30 piece-id words of the room selected by CalcBlue */
#define gRoomSpecs      GH(0x1037dc)   /* long: -> 30 spec longs of that room */
#define gObjSpec        GH(0x1037e8)   /* long: spec of the current object (GetOS) */
#define gObjId          GH(0x1037ec)   /* word: piece id of the current object */
#define gLeftObjId      GH(0x10362a)   /* word id + long spec of the cell left of the drawn one */
#define gLeftObjSpec    GH(0x10362c)
#define gRightObjId     GH(0x103624)   /* word id + long spec of the cell right of the drawn one */
#define gRightObjSpec   GH(0x103626)
#define gAboveRow       GH(0x1035be)   /* 11 x {word id, long spec}: row above (col -1, 0..9) */
#define gBelowRow       GH(0x10357c)   /* 11 x {word id, long spec}: row below (col -1, 0..9) */
#define gLeftCol        GH(0x103612)   /* 3 x {word id, long spec}: column 9 of the room on the left */
#define gRightCol       GH(0x103600)   /* 3 x {word id, long spec}: column 0 of the room on the right */
#define g103362         GH(0x103362)   /* long: pointer (word *p == 0x1f tested) */

#define LEVEL_TYPE()    R16(R32(gGameState) + 0x2186)
#define LEVEL_NUM()     R16(R32(gGameState) + 0x218a)
/* links between rooms: game state + 0x2078 + room*8 = {left, right, above, below} */
#define ROOM_LINK(room, off)  R16(R32(gGameState) + 0x2080 + (uint32_t)(uint16_t)(room) * 8 + (int32_t)(off))
/* piece id / spec of (room, cell) */
#define TILE_ADDR(room, cell) (R32(gGameState) + (uint32_t)(uint16_t)(room) * 0x3c - 0x3c + (int32_t)(int16_t)(cell) * 2)
#define SPEC_ADDR(room, cell) (R32(gGameState) + 0x780 + (uint32_t)(uint16_t)(room) * 0x78 - 0x78 + (int32_t)(int16_t)(cell) * 4)
#define LINK_ADDR(link)       (R32(gGameState) + 0x1680 + (int32_t)(int16_t)(link) * 10)

/* ---- drawing ---- */
#define gScreenRect     GH(0x103014)   /* Rect: the playfield */
#define gEmptyRect      GH(0x106694)   /* Rect: empty rect */
#define gClipRectProc   GH(0x10609c)   /* long: -> ClipRect-like Pascal procedure (rect) */
#define gEmptyRectProc  GH(0x1060a0)   /* long: -> EmptyRect-like Pascal function (rect): Boolean */
#define gUnionRectProc  GH(0x1060a4)   /* long: -> UnionRect-like Pascal procedure (a, b, dst) */
#define gSectRectProc   GH(0x1060a8)   /* long: -> SectRect-like Pascal function (a, b, dst): Boolean */
#define gColLeft        GH(0x103498)   /* word[]: left x of each column */
#define gRowBottom      GH(0x1034b2)   /* word[]: bottom y of each row */
#define gDrawClip       GH(0x103a00)   /* Rect: clip of the cell being drawn */
#define gDrawCol        GH(0x103a08)   /* word: column being drawn */
#define gDrawRow        GH(0x103a0a)   /* word: row being drawn (-1: row above the room) */
#define gFullClip       GH(0x1037ce)   /* word: when set, draw with the full screen clip */
#define gWorldSave      GH(0x102dc2)   /* GWorld save area for SaveCurrentWorld/RestoreOriginalWorld */

/* redraw buffers (pointers to per-cell tables) */
#define gMarkTopBuf     GH(0x103c1c)   /* long: 10 x {word flag, Rect} for the row above the room */
#define gMarkBuf20      GH(0x103c20)
#define gHalfBuf        GH(0x103c24)   /* long: word[30] */
#define gFloorBuf       GH(0x103c28)   /* long: 30 x 10 bytes */
#define gFRedrawBuf     GH(0x103c2c)
#define gMoveBuf        GH(0x103c30)
#define gRedrawBuf      GH(0x103c34)   /* long: word[30] */

/* ---- trobs (animated objects): 8 bytes {cell, room, phase, type} ---- */
#define gTrobs          GH(0x103b72)   /* trob[20] */
#define gTrobCount      GH(0x103c1a)
#define gTrob           GH(0x103c12)   /* the trob being processed */
#define gTrobCell       GH(0x103c12)
#define gTrobRoom       GH(0x103c14)
#define gTrobPhase      GH(0x103c16)   /* -1: finished, removed by AnimTrobs */
#define gTrobType       GH(0x103c18)

/* ---- mobs (moving objects): 16 bytes ---- */
#define gMob            GH(0x103b5a)   /* the mob being processed: */
#define gMobX           GH(0x103b5a)   /*   +0 x */
#define gMobY           GH(0x103b5c)   /*   +2 y */
#define gMobRoom        GH(0x103b5e)   /*   +4 room */
#define gMobVel         GH(0x103b60)   /*   +6 speed / state (-1 = dead) */
#define gMobType        GH(0x103b64)   /*   +a type */
#define gMobRow         GH(0x103b66)   /*   +c row */
#define gMobLanded      GH(0x103b68)   /*   +e */
#define gMobs           GH(0x103b6a)   /* long: -> mob[30] */
#define gMobIdx         GH(0x103b6e)   /* word: index of gMob in gMobs */
#define gMobCount       GH(0x103b70)
#define gMobFloorTop    GH(0x103ae8)   /* word[type] */
#define gMobXOff        GH(0x103b04)   /* word[type] */

/* ---- current piece (set by ReadCell) ---- */
#define gPieceId        GH(0x104b50)
#define gPieceSpec      GH(0x104b52)
#define gPieceCell      GH(0x104b54)
#define gPieceRoom      GH(0x104b56)
#define gPieceCol       GH(0x104b58)
#define gPieceRow       GH(0x104b5a)

/* ---- the kid (character record at 0x102faa, 0x3e bytes) ---- */
#define gKid            GH(0x102faa)
#define gKidSaved       GH(0x102f2e)   /* copy of the kid record */
#define KID(off)        (gKid + (off))
#define gKid_02         KID(0x02)
#define gKidY           KID(0x06)
#define gKidChar        KID(0x08)
#define gKidFrame       KID(0x0a)
#define gKidCol         KID(0x0c)
#define gKidRow         KID(0x0e)
#define gKidAction      KID(0x10)
#define gKid_14         KID(0x14)
#define gKidRoom        KID(0x16)
#define gKid_1c         KID(0x1c)
#define gKid_28         KID(0x28)
#define SAVED(off)      (gKidSaved + (off))

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
#define CLIPRECT(r)           pascal_ptr1(R32(gClipRectProc), 0, (r))

/* Some functions modify their own stack arguments; those writes land in the caller's
 * frame and are part of the original's memory effects. Such a function is written as
 * Name_args(ap), where ap is the address of its argument block (first argument at ap);
 * glue_Name ("conv": "manual") passes the real argument slots, and the C entry point
 * copies the arguments onto the emulated stack first. */
#define ARGS_GLUE(Name) void glue_##Name(void) { emu_set_d0(Name##_args(emu_sp())); }

/* 00024184: select the room whose tiles/specs gRoomTiles/gRoomSpecs point to */
uint32_t CalcBlue(int16_t room) {
    uint16_t r = (uint16_t)room;
    if (r == 0) return 0;
    W32(gRoomTiles, R32(gGameState) + (uint32_t)r * 0x3c - 0x3c);
    W32(gRoomSpecs, R32(gGameState) + 0x780 + (uint32_t)r * 0x78 - 0x78);
    return 0xffffff88;
}

/* 000241f4: column of a cell of `room` relative to the current room (-1 / 11 for the
 * neighbouring column, 0x1e when not visible) */
uint32_t CellToCol(int16_t room, int16_t cell) {
    int16_t col = (int16_t)(cell % 10);
    if (room == R16(gCurRoom) || room == R16(gRoomAbove) || room == R16(gRoomBelow))
        return (uint32_t)(int32_t)col;                    /* D0 = ext.l of the remainder */
    if (col == 9 && (room == R16(gRoomLeft) || room == R16(gRoomBelowL) || room == R16(gRoomAboveL)))
        col = -1;
    else if (col == 0 && (room == R16(gRoomRight) || room == R16(gRoomBelowR) || room == R16(gRoomAboveR)))
        col = 0xb;
    else
        col = 0x1e;
    return (uint16_t)col;                                 /* high word of D0 is 0 (moveq #9) */
}

/* 00024278: row of a cell of `room` relative to the current room */
uint32_t CellToRow(int16_t room, int16_t cell) {
    int16_t row = (int16_t)(cell / 10);
    uint32_t hi = row < 0 ? 0xffff0000u : 0;              /* D0 = ext.l of the quotient */
    if (room == R16(gCurRoom) || room == R16(gRoomLeft) || room == R16(gRoomRight))
        return hi | (uint16_t)row;
    if (room == R16(gRoomAbove)) {
        hi = 0;                                           /* moveq #2, D0 */
        if (row == 2) return hi | 0xffff;
    }
    if (room == R16(gRoomBelow) && row == 0) row = 3;
    else row = 0x1e;
    return hi | (uint16_t)row;
}

/* 000242e2: rectangle of cell (col, row) clipped to the playfield */
uint32_t ComputeClipRect(uint32_t rect, int16_t col, int16_t row) {
    int16_t v = R16(gColLeft + (int16_t)((int32_t)col * 2));
    W16(rect + 2, v);
    W16(rect + 6, v + 0x33);
    v = R16(gRowBottom + (int16_t)((int32_t)row * 2));
    W16(rect + 4, v);
    W16(rect, v - 0x78);
    SECTRECT(gScreenRect, rect, rect);
    int16_t extra = R16(gTopRowExtra);
    if (extra != 0 && row == 0) W16(rect, R16(rect) - 5);
    return (uint16_t)extra;
}

/* 00024362: dst = src offset by (colOff, rowOff) cells and clipped; returns dst or 0 */
static uint32_t ComputeOffsetCellRect_args(maddr ap) {
    int16_t col = R16(ap + 8);
    if (col == 0x1e || R16(ap + 10) == 0x1e) {
        W32(ap, 0);
    } else {
        mcopy(R32(ap), R32(ap + 4), 8);
        _OffsetRect(R32(ap), (int16_t)(col * 0x33), (int16_t)(R16(ap + 10) * 0x78));
        if (!SECTRECT(gScreenRect, R32(ap), R32(ap))) W32(ap, 0);
    }
    return R32(ap);
}
uint32_t ComputeOffsetCellRect(uint32_t dst, uint32_t src, int16_t colOff, int16_t rowOff) {
    FRAME(f, 12);
    W32(f, dst); W32(f + 4, src); W16(f + 8, colOff); W16(f + 10, rowOff);
    uint32_t r = ComputeOffsetCellRect_args(f);
    FRAME_END(f);
    return r;
}
ARGS_GLUE(ComputeOffsetCellRect)

/* 000243ea: fetch the pieces of the row above the drawn row into gAboveRow */
uint32_t GetAboveAry(void) {
    int16_t row = R16(gDrawRow);
    if (row == -1) return 0;
    int16_t savedBelow = R16(gRoomBelow);
    int16_t room, leftRoom;
    if (row == 0) {
        room = R16(gRoomAbove);
        leftRoom = R16(gRoomAboveL);
        W16(gRoomBelow, R16(gCurRoom));
    } else {
        room = R16(gCurRoom);
        leftRoom = R16(gRoomLeft);
    }
    int16_t prow = (int16_t)(R16(gDrawRow) - 1);
    int16_t flag;
    if (R16(gTopRowExtra) != 0) flag = 0;
    else {
        int16_t lt = LEVEL_TYPE();
        flag = (lt == 4 || lt == 5) ? 0 : 1;
    }
    CalcBlue(leftRoom);
    W16(gAboveRow, GetObject(leftRoom, 9, prow, gAboveRow + 2, flag));
    if (R32(g103362) != 0 && R16(gDrawRow) == 0)
        W16(gAboveRow + 2, RU16(gAboveRow + 2) | 0xc000);
    CalcBlue(room);
    maddr e = gAboveRow + 6;
    for (uint16_t col = 1; col < 0xb; col++, e += 6) {
        W16(e, GetObject(room, (int16_t)(col - 1), prow, e + 2, flag));
        if (R16(gTopRowExtra) != 0 && R16(gDrawRow) == 0) {
            maddr gs = R32(gGameState);
            if (R16(gs + 0x2186) == 3) {
                int16_t id = R16(gs + (uint32_t)RU16(gCurRoom) * 0x3c + (int32_t)(int16_t)col * 2 - 0x3e);
                if (S16(IsFloor(id)) != 0)
                    W16(e + 2, (RU16(e + 2) & 0x3fff) | 0x4000);
            }
        }
    }
    CalcBlue(R16(gCurRoom));
    W16(gRoomBelow, savedBelow);
    return 0;
}

/* 0002453c: fetch the pieces of the row below the drawn row into gBelowRow */
uint32_t GetBelowAry(void) {
    int16_t room, leftRoom, prow;
    if (R16(gDrawRow) == 2) {
        room = R16(gRoomBelow);
        leftRoom = R16(gRoomBelowL);
        prow = 0;
    } else {
        room = R16(gCurRoom);
        leftRoom = R16(gRoomLeft);
        prow = (int16_t)(R16(gDrawRow) + 1);
    }
    CalcBlue(leftRoom);
    int16_t flag = room != 0 ? 0x14 : 0;
    W16(gBelowRow, GetObject(leftRoom, 9, prow, gBelowRow + 2, flag));
    CalcBlue(room);
    maddr e = gBelowRow + 6;
    for (uint16_t col = 0; col < 10; col++, e += 6)
        W16(e, GetObject(room, (int16_t)col, prow, e + 2, 0));
    CalcBlue(R16(gCurRoom));
    return 0;
}

/* 000245f0: piece id (result) and spec (*specOut) of (room, col, row); col -1/10 come
 * from the neighbour column caches, room 0 gives a default per level type */
static uint32_t GetObject_args(maddr ap) {
    if (R16(ap + 4) == -1) W16(ap + 4, 2);
    int16_t col = R16(ap + 2);
    int16_t row = R16(ap + 4);
    maddr out = R32(ap + 6);
    int16_t id;
    if (col == -1) {
        int16_t i = (int16_t)((int32_t)row * 6);
        id = R16(gLeftCol + i);
        W32(out, R32(gLeftCol + 2 + i));
    } else if (col == 10) {
        int16_t i = (int16_t)((int32_t)row * 6);
        id = R16(gRightCol + i);
        W32(out, R32(gRightCol + 2 + i));
    } else if (R16(ap) != 0) {
        int16_t cell = (int16_t)(S16(FirstInRow(R16(ap + 4))) + R16(ap + 2));
        id = R16(R32(gRoomTiles) + (int32_t)cell * 2);
        W32(R32(ap + 6), R32(R32(gRoomSpecs) + (int32_t)cell * 4));
    } else {
        id = R16(ap + 10);
        int16_t lt = LEVEL_TYPE();
        if (lt == 3) {
            W16(out, 0); W16(out + 2, 0);
        } else if (LEVEL_TYPE() == 2) {
            W16(out, 0); W16(out + 2, 9);
        } else if ((lt = LEVEL_TYPE()) == 4 || lt == 5) {
            W16(out, 0); W16(out + 2, 0);
        } else {
            W16(out, 0);
            W16(out + 2, (int16_t)(((R16(ap + 2) & 1) << 8) + 9));
        }
    }
    return (uint16_t)id;
}
uint32_t GetObject(int16_t room, int16_t col, int16_t row, uint32_t specOut, int16_t dflt) {
    FRAME(f, 12);
    W16(f, room); W16(f + 2, col); W16(f + 4, row); W32(f + 6, specOut); W16(f + 10, dflt);
    uint32_t r = GetObject_args(f);
    FRAME_END(f);
    return r;
}
ARGS_GLUE(GetObject)

/* 00024750: piece id of the cell above (col, row) */
static uint32_t PieceAboveId_args(maddr ap) {
    int16_t room;
    if (R16(ap + 2) > 0) {
        room = R16(gCurRoom);
        W16(ap + 2, R16(ap + 2) - 1);
    } else {
        room = R16(gRoomAbove);
        W16(ap + 2, 2);
    }
    if (R16(ap) < 0) {
        if (room != 0) room = ROOM_LINK(room, -8);
        W16(ap, R16(ap) + 10);
    } else if (R16(ap) >= 10) {
        if (room != 0) room = ROOM_LINK(room, -6);
        W16(ap, R16(ap) - 10);
    }
    return GetRoomID(room, R16(ap + 2), R16(ap), 1);
}
uint32_t PieceAboveId(int16_t col, int16_t row) {
    FRAME(f, 4);
    W16(f, col); W16(f + 2, row);
    uint32_t r = PieceAboveId_args(f);
    FRAME_END(f);
    return r;
}
ARGS_GLUE(PieceAboveId)

/* 000247ea: piece id of (room, row, col), dflt when the room does not exist */
uint32_t GetRoomID(int16_t room, int16_t row, int16_t col, int16_t dflt) {
    int16_t id = dflt;
    if (room != 0 && (uint16_t)room <= (uint16_t)R16(R32(gGameState) + 0x2180)) {
        int16_t cell = (int16_t)(S16(FirstInRow(row)) + col);
        /* the original tests (cell >= 0 || cell < 0x1e), which always holds */
        id = R16(TILE_ADDR(room, cell));
    }
    return (uint16_t)id;
}

/* 0002486e: piece id of the cell above and to the right of (col, row) */
static uint32_t PieceAboveRightId_args(maddr ap) {
    int16_t room;
    if (R16(ap + 2) > 0) {
        room = R16(gCurRoom);
        W16(ap + 2, R16(ap + 2) - 1);
    } else {
        room = R16(gRoomAbove);
        W16(ap + 2, 2);
    }
    /* with no room the original returns whatever its caller had in D3 */
    if (room == 0) return (uint16_t)cpu_get(R_D3);
    if (R16(ap) < -1) {
        room = ROOM_LINK(room, -8);
        W16(ap, R16(ap) + 9);
    } else if (R16(ap) >= 9) {
        room = ROOM_LINK(room, -6);
        W16(ap, R16(ap) - 9);
    } else {
        W16(ap, R16(ap) + 1);
    }
    return (uint16_t)GetRoomID(room, R16(ap + 2), R16(ap), 1);
}
uint32_t PieceAboveRightId(int16_t col, int16_t row) {
    FRAME(f, 4);
    W16(f, col); W16(f + 2, row);
    uint32_t r = PieceAboveRightId_args(f);
    FRAME_END(f);
    return r;
}
ARGS_GLUE(PieceAboveRightId)

/* 00024924: piece id of the cell below (col, row) */
static uint32_t PieceBelowId_args(maddr ap) {
    int16_t row = R16(ap + 2);
    int16_t room;
    if (row < 2) {
        if (row == 0 && R16(gDrawRow) == -1) room = R16(gRoomBelow);
        else room = R16(gCurRoom);
        W16(ap + 2, R16(ap + 2) + 1);
    } else {
        W16(ap + 2, 0);
        room = R16(gRoomBelow);
    }
    if (R16(ap) < 0) {
        room = R16(R32(gGameState) + (uint32_t)(uint16_t)room * 8 + 0x2078);
        W16(ap, R16(ap) + 10);
    } else if (R16(ap) >= 10) {
        room = R16(R32(gGameState) + (uint32_t)(uint16_t)room * 8 + 0x207a);
        W16(ap, R16(ap) - 10);
    }
    return GetRoomID(room, R16(ap + 2), R16(ap), 0x14);
}
uint32_t PieceBelowId(int16_t col, int16_t row) {
    FRAME(f, 4);
    W16(f, col); W16(f + 2, row);
    uint32_t r = PieceBelowId_args(f);
    FRAME_END(f);
    return r;
}
ARGS_GLUE(PieceBelowId)

/* 000249ce: piece id of the cell left of (col, row) */
static uint32_t PieceLeftId_args(maddr ap) {
    int16_t room;
    if (R16(ap) != 0) {
        if (R16(ap + 2) == 0 && R16(gDrawRow) == -1) room = R16(gRoomBelow);
        else room = R16(gCurRoom);
        W16(ap, R16(ap) - 1);
    } else {
        if (R16(ap + 2) == -1 && R16(gDrawRow) == -1) room = R16(gRoomAboveL);
        else room = R16(gRoomLeft);
        W16(ap, 9);
    }
    return GetRoomID(room, R16(ap + 2), R16(ap), 0x14);
}
uint32_t PieceLeftId(int16_t col, int16_t row) {
    FRAME(f, 4);
    W16(f, col); W16(f + 2, row);
    uint32_t r = PieceLeftId_args(f);
    FRAME_END(f);
    return r;
}
ARGS_GLUE(PieceLeftId)

/* 00024a44: piece id of the cell right of (col, row) */
static uint32_t PieceRightId_args(maddr ap) {
    int16_t room;
    if (R16(ap) != 9) {
        if (R16(ap + 2) == 0 && R16(gDrawRow) == -1) {
            room = R16(gRoomBelow);
        } else {
            room = R16(gCurRoom);
            if (R16(ap + 2) == -1 && R16(gDrawRow) == -1) W16(ap + 2, 2);
        }
        W16(ap, R16(ap) + 1);
    } else {
        if (R16(ap + 2) == -1 && R16(gDrawRow) == -1) room = R16(gRoomAboveR);
        else room = R16(gRoomRight);
        W16(ap, 0);
    }
    if (R16(ap + 2) == 3) {
        if (room == R16(gRoomRight)) room = R16(gRoomBelowR);
        else room = R16(gRoomBelow);
        W16(ap + 2, 0);
    }
    int16_t id;
    if (room != 0) {
        int32_t cell = (int32_t)S16(FirstInRow(R16(ap + 2))) + (int32_t)R16(ap);
        id = R16(R32(gGameState) + (uint32_t)(uint16_t)room * 0x3c - 0x3c + cell * 2);
    } else {
        maddr gs = R32(gGameState);
        int16_t cur;
        if (R16(gs + 0x2186) == 4 &&
            (R16(gs + 0x218a) != 7 ||
             ((cur = R16(gCurRoom)) != 7 && cur != 0xf && cur != 0x20)))
            id = 0;
        else
            id = 0x14;
    }
    return (uint16_t)id;
}
uint32_t PieceRightId(int16_t col, int16_t row) {
    FRAME(f, 4);
    W16(f, col); W16(f + 2, row);
    uint32_t r = PieceRightId_args(f);
    FRAME_END(f);
    return r;
}
ARGS_GLUE(PieceRightId)

/* 00024b60: erase a rectangle in the offscreen world and on screen */
uint32_t PoPEraseRectInBothPorts(uint32_t rect) {
    SaveCurrentWorld(gWorldSave);
    CLIPRECT(rect);
    _EraseRect(rect);
    CLIPRECT(gScreenRect);
    RestoreOriginalWorld(gWorldSave);
    CLIPRECT(rect);
    _EraseRect(rect);
    CLIPRECT(gScreenRect);
    return 0;
}

/* 00024bcc: set up the clip and the pieces around the cell being drawn */
uint32_t FUN_00024bcc(void) {
    int16_t row = R16(gDrawRow);
    int16_t flag;
    if ((row == 2 && R16(gFullClip) == 0) || row == -1) {
        maddr gs = R32(gGameState);
        int16_t lt = R16(gs + 0x2186);
        int16_t below;
        if (lt == 4 &&
            (R16(gs + 0x218a) != 7 ||
             ((below = R16(gRoomBelow)) != 7 && below != 0xf && below != 0x20)))
            flag = 0;
        else if (lt == 5)
            flag = 0;
        else
            flag = 1;
    } else {
        flag = 0x14;
    }
    if (R16(gFullClip) == 0) ComputeClipRect(gDrawClip, R16(gDrawCol), R16(gDrawRow));
    else mcopy(gDrawClip, gScreenRect, 8);
    W16(gObjId, GetObject(R16(gCurRoom), R16(gDrawCol), R16(gDrawRow), gObjSpec, flag));
    W16(gLeftObjId, GetObject(R16(gCurRoom), (int16_t)(R16(gDrawCol) - 1), R16(gDrawRow), gLeftObjSpec, flag));
    W16(gRightObjId, GetObject(R16(gCurRoom), (int16_t)(R16(gDrawCol) + 1), R16(gDrawRow), gRightObjSpec, flag));
    if (R16(gDrawRow) == -1 && R32(g103362) != 0) {
        W16(gObjSpec, RU16(gObjSpec) | 0xc000);
        W16(gLeftObjSpec, RU16(gLeftObjSpec) | 0xc000);
        W16(gRightObjSpec, RU16(gRightObjSpec) | 0xc000);
    }
    return 0;
}

/* 00024cd0: fill the caches of the neighbouring columns (left room col 9, right room col 0) */
uint32_t seg5_024cd0(void) {
    int16_t flag;
    if (R16(gCurRoom) != 0) {
        flag = 0x14;
    } else {
        maddr gs = R32(gGameState);
        int16_t below;
        if (R16(gs + 0x2186) == 4 && R16(gs + 0x218a) == 7 &&
            ((below = R16(gRoomBelow)) == 7 || below == 0xf || below == 0x20))
            flag = 0x14;
        else
            flag = 0;
    }
    int16_t room = R16(gRoomLeft);
    CalcBlue(R16(gRoomLeft));
    maddr e = gLeftCol;
    for (uint16_t row = 0; row < 3; row++, e += 6) {
        W16(e, GetObject(room, 9, (int16_t)row, e + 2, flag));
        maddr p = R32(g103362);
        if (p != 0 && (R16(R32(g103362)) != 0x1f || (row != 2 && row != 1)))
            W16(e + 2, RU16(e + 2) | 0xc000);
        else
            W16(e + 2, RU16(e + 2) & 0x3fff);
    }
    room = R16(gRoomRight);
    CalcBlue(R16(gRoomRight));
    e = gRightCol;
    for (uint16_t row = 0; row < 3; row++, e += 6)
        W16(e, GetObject(room, 0, (int16_t)row, e + 2, flag));
    CalcBlue(R16(gCurRoom));
    return 0;
}

/* 00024db8: animate every trob, then drop the finished ones (phase < 0) */
uint32_t AnimTrobs(void) {
    if (R16(gTrobCount) == 0) return 0;
    maddr t = gTrobs;
    int16_t n = R16(gTrobCount);
    int16_t removed = 0;
    int16_t i = 0;
    if (R16(gTrobCount) > 0) {
        do {
            mcopy(gTrob, t, 8);
            AnimTrob(i);
            W16(t + 4, R16(gTrobPhase));
            if (R16(gTrobPhase) < 0) removed = 1;
            i++;
            t += 8;
        } while (i < n);
    }
    if (removed) {
        maddr dst = gTrobs;
        t = gTrobs;
        n = R16(gTrobCount);
        W16(gTrobCount, 0);
        for (i = 0; i < n; i++, t += 8) {
            if (R16(t + 4) < 0) continue;
            if (t != dst) mcopy(dst, t, 8);
            dst += 8;
            W16(gTrobCount, R16(gTrobCount) + 1);
        }
    }
    return 0;
}

/* 00024e56: animate the trob in gTrob according to its type */
uint32_t AnimTrob(int16_t idx) {
    CalcBlue(R16(gTrobRoom));
    GetOS(R16(gTrobCell));
    int16_t t = (int16_t)(RU16(gTrobType) - 4);
    int handled = 1;
    if (t < 0 || t > 0x2d) handled = 0;
    else switch (t + 4) {
        case 4:
            if (LEVEL_TYPE() == 1) FUN_00055b5c();
            else AnimGate();
            break;
        case 5: case 6: case 34: FUN_000266d4(); break;
        case 10: FUN_0002556c(); break;
        case 11: AnimLoose(); break;
        case 13: AnimScythe(); break;
        case 17: FUN_0002573c(); break;
        case 19: case 32: AnimTorch(idx); break;
        case 23: AnimLava(); break;
        case 27: seg8_0354c8(); break;
        case 28: FUN_00055daa(); break;
        case 29: FUN_00055cf8(); break;
        case 30: FUN_00055ecc(); break;
        case 31: seg9_0396ea(); break;
        case 33: FUN_00036daa(); break;
        case 40: AnimFinlSpider(); break;
        case 41: AnimFinlCrystal(); break;
        case 42: AnimFinlGargoyle(); break;
        case 43: FUN_0003d162(); break;
        case 44: seg8_034698(); break;
        case 47: AnimRoofPillar(); break;
        case 48: AnimRoofShip(); break;
        case 49: AnimRoofGlass(); break;
        default: handled = 0; break;
    }
    if (!handled) W16(gTrobPhase, -1);
    W16(R32(gRoomSpecs) + (int32_t)R16(gTrobCell) * 4, RU16(gObjSpec));
    return 0;
}

/* 00024fb8 */
uint32_t RedrawTorch(void) {
    FRAME(f, 8);
    int16_t cave = LEVEL_TYPE() == 2 ? 1 : 0;
    mcopy(f, cave ? GH(0x103a84) : GH(0x103a7c), 8);
    if (ComputeTrobMoveRect(f, f) != 0) MarkBufsForRect(JT_ADDR(FN_MarkMove), f);
    FRAME_END(f);
    return 0;
}

/* 0002502c */
uint32_t RedrawExit(void) {
    FRAME(f, 8);
    maddr src = R32(GH(0x103ac0) + (int16_t)((uint32_t)RU16(R32(gGameState) + 0x2186) * 4));
    if (ComputeTrobMoveRect(f, src) != 0) MarkBufsForRect(JT_ADDR(FN_MarkMove), f);
    FRAME_END(f);
    return 0;
}

/* 0002507a */
uint32_t RedrawFlask(void) {
    FRAME(f, 8);
    if (ComputeTrobMoveRect(f, GH(0x103a5c)) != 0) MarkBufsForRect(JT_ADDR(FN_MarkMove), f);
    FRAME_END(f);
    return 0;
}

/* 000250b8 */
uint32_t RedrawPlate(void) {
    FRAME(f, 8);
    maddr src = LEVEL_TYPE() == 3 ? GH(0x103a8c) : GH(0x103a74);
    if (ComputeTrobMoveRect(f, src) != 0) MarkBufsForRect(JT_ADDR(FN_MarkMove), f);
    FRAME_END(f);
    return 0;
}

/* 00025110 */
uint32_t RedrawLoose(void) {
    FRAME(f, 8);
    maddr src = R32(GH(0x103aac) + (int16_t)((uint32_t)RU16(R32(gGameState) + 0x2186) * 4));
    if (ComputeTrobMoveRect(f, src) != 0) MarkBufsForRect(JT_ADDR(FN_MarkMove), f);
    FRAME_END(f);
    return 0;
}

/* 00025166: mark the gate's rectangle (taller when the cell above is not visible) */
uint32_t RedrawGate(void) {
    FRAME(f, 0x10);                 /* f = rect (A6-0x10), f+8 = source rect (A6-8) */
    maddr src8 = f + 8;
    int16_t lt = LEVEL_TYPE();
    if (lt == 3) mcopy(src8, GH(0x103a2c), 8);
    else if (LEVEL_TYPE() == 4) mcopy(src8, GH(0x103a34), 8);
    else mcopy(src8, GH(0x103a3c), 8);
    int16_t col = (int16_t)(R16(gTrobCell) % 10);
    int16_t room, cell;
    if (R16(gTrobCell) >= 10) {
        room = R16(gTrobRoom);
        cell = (int16_t)(R16(gTrobCell) - 9);
    } else {
        int16_t r = R16(gTrobRoom);
        maddr links = R32(gGameState) + 0x2080 + (uint32_t)(uint16_t)r * 8;
        room = R16(links - 4);
        cell = (int16_t)(R16(gTrobCell) + 0x15);
        if (room == 0 && col == 9) {
            room = R16(links - 6);
            room = R16(R32(gGameState) + 0x2080 + (uint32_t)(uint16_t)room * 8 - 4);
            cell = 0x14;
        }
    }
    if (room == 0) {
        W16(src8, 5);
    } else if (col != 9) {
        int16_t id = R16(TILE_ADDR(room, cell));
        if (S16(IsSpace(id)) == 0) W16(src8, 5);
    } else if (R16(gTrobRoom) != R16(gRoomLeft)) {
        W16(src8, 5);
    }
    if (ComputeTrobMoveRect(f, src8) != 0) MarkBufsForRect(JT_ADDR(FN_MarkMove), f);
    FRAME_END(f);
    return 0;
}

/* 000252c4 */
uint32_t RedrawTrob(uint32_t src) {
    if (src == 0) return 0;
    FRAME(f, 8);
    if (ComputeTrobMoveRect(f, src) != 0) MarkBufsForRect(JT_ADDR(FN_MarkMove), f);
    FRAME_END(f);
    return 0;
}

/* 00025306: dst = src placed at the cell of gTrob, clipped; 0 when not visible */
uint32_t ComputeTrobMoveRect(uint32_t dst, uint32_t src) {
    int16_t col = S16(CellToCol(R16(gTrobRoom), R16(gTrobCell)));
    int16_t row = S16(CellToRow(R16(gTrobRoom), R16(gTrobCell)));
    return ComputeOffsetCellRect(dst, src, col, row);
}

/* 00025362: is gTrob's cell visible? kills the trob when it is not */
uint32_t FUN_00025362(void) {
    int16_t v = S16(IsVisibleRoomCell(R16(gTrobRoom), R16(gTrobCell)));
    if (v == 0) W16(gTrobPhase, -1);
    return (uint16_t)v;
}

/* 00025382 */
uint32_t seg5_025382(void) {
    int16_t ch = R16(gKidChar);
    if (ch == 7 || ch == 8 || R16(gKidAction) == 9) {
        if (R16(gKid_14) != 0) Falling();
        return 0;
    }
    switch (R16(gKidAction)) {
        case 2: break;
        case 3: {
            uint16_t fr = RU16(gKidFrame);
            if (fr >= 0x66 && fr <= 0x6a) seg4_02129a();
            break;
        }
        case 4: Falling(); break;
        case 5:
            if (R16(gKidFrame) == 0x6d) seg4_02108a();
            break;
        case 6: break;
        default: seg4_02108a(); break;
    }
    return 0;
}

/* 000253fe: what the kid is standing on: plates, loose floors */
uint32_t CheckPress(void) {
    int16_t ch = R16(gKidChar);
    if (ch == 0xb || ch == 0xa) return 0;
    if (ch == 0 && R16(GH(0x10305a)) != 0) return 0;
    if (ch == 1) {
        int16_t v = R16(gKid_28);
        if (v >= 0x1d && v <= 0x2a) return 0;
    }
    int16_t frame = R16(gKidFrame);
    int16_t piece;
    uint16_t fr = RU16(gKidFrame);
    if ((fr >= 0x57 && RU16(gKidFrame) <= 0x63) ||
        (RU16(gKidFrame) >= 0x87 && RU16(gKidFrame) < 0x8d)) {
        piece = S16(FUN_00021ac8());
    } else {
        int16_t act = R16(gKidAction);
        if (act < 2 || act == 7 || act == 5 || act == 8) {
            piece = S16(FUN_00021ac8());
            if (frame == 0x4f && piece == 0xb) {
                BreakLoose(0);
                piece = 0;
            } else if (frame == 0x4f && piece == 0xf) {
                seg7_033c50();
                piece = 0;
            } else if ((R16(GH(0x102df6)) & 0x40) == 0) {
                piece = 0;
            } else {
                piece = S16(FUN_00021076());
            }
        } else {
            piece = 0;
        }
    }
    if ((int32_t)R16(gKidRow) * 0x78 + 0x5f > (int32_t)R16(gKidY)) return 0;
    if (piece == 5 || piece == 6 || piece == 0x22) {
        if (R16(gKid_1c) < 0 || R16(gKidChar) == 4) PushPlate(0, -1);
        else FUN_000266ae();
    } else if (piece == 0xb || piece == 0xf) {
        W16(gPressed, 1);
        if (piece == 0xb) BreakLoose(0);
        else if (piece == 0xf) seg7_033c50();
    }
    return 0;
}

/* 0002556c: flask bubbling */
uint32_t FUN_0002556c(void) {
    if (R16(gObjId) != 10) {
        W16(gTrobPhase, -1);
        return 0;
    }
    if (R16(gTrobPhase) < 0) return 0;
    if (S16(FUN_00025362()) == 0) return 0;
    int16_t hi = (int16_t)(R16(gObjSpec) & 0xffe0);
    int16_t f = (int16_t)(R16(gObjSpec) & 0x1f);
    f = f >= 0x1b ? 0 : (int16_t)(f + 1);
    W16(gObjSpec, f | hi);
    RedrawFlask();
    return 0;
}

/* 000255c6: torch flicker */
uint32_t AnimTorch(int16_t idx) {
    if (S16(FUN_00025362()) == 0) return 0;
    int16_t fr = S16(GetTorchFrame((int16_t)(R16(gObjSpec) & 0xf)));
    W16(gObjSpec, (int16_t)((R16(gObjSpec) & 0xfff0) + fr));
    if (R16(GH(0x102ffe)) == 0 ||
        (((int32_t)idx + (uint32_t)RU16(GH(0x104b66))) & 1) != 0)
        RedrawTorch();
    return 0;
}

/* 00025628: a gate stopped moving */
uint32_t StopGate(int16_t arg) {
    W16(gTrobPhase, -1);
    int16_t snd = LEVEL_TYPE() == 3 ? 0x12 : 0x112;
    AddGateSound(snd);
    FUN_000225d2();
    int16_t cell = (int16_t)(S16(FirstInRow((int16_t)(R16(gKidRow) - 1))) + R16(gKidCol));
    if (cell == R16(gTrobCell) && R16(gKid_02) == -1 && R16(gKidAction) == 2 && arg == 0) {
        JumpSeq(0x2f);
        mcopy(gKidSaved, gKid, 0x3e);
    }
    return 0;
}

/* 000256bc: sound when a gate closes on the kid's fingers */
uint32_t CheckSquishedFingers(void) {
    if (R16(gTrobRoom) != R16(SAVED(0x16))) return 0;
    int16_t act = R16(SAVED(0x10));
    if (act != 2 && act != 6) return 0;
    int16_t cell = (int16_t)(S16(FirstInRow(R16(SAVED(0x0e)))) + R16(SAVED(0x0c)));
    int32_t tc = R16(gTrobCell);
    if (tc + 10 == (int32_t)cell || tc + 11 == (int32_t)cell) AddSound(0x1d);
    return 0;
}

/* 0002573c: exit door opening / closing */
uint32_t FUN_0002573c(void) {
    int16_t pos = (int16_t)(R16(gObjSpec) & 0xff);
    int16_t phase = R16(gTrobPhase);
    if (phase >= 0) {
        if (phase >= 4) {
            W16(gTrobPhase, R16(gTrobPhase) + 1);
            pos = (int16_t)(pos - R16(GH(0x103a04) + (int16_t)((int32_t)R16(gTrobPhase) * 2)));
            if (pos < 0) {
                pos = 0;
                W16(gTrobPhase, -1);
                AddSound(0x14);
            } else if (R16(gTrobPhase) == 4) {
                AddSound(0x11);
            }
        } else {
            pos++;
            if (pos >= 0x4f) {
                W16(gTrobPhase, -1);
                if (!(R16(gCurRoom) == 4 && LEVEL_NUM() == 0xd)) {
                    ResetSoundItem(0);
                    AddSound(0x12);
                }
            } else if (!(R16(gCurRoom) == 4 && LEVEL_NUM() == 0xd)) {
                AddSound(0x11);
            }
        }
    }
    W16(gObjSpec, (int16_t)((R16(gObjSpec) & 0xff00) + pos));
    RedrawExit();
    return 0;
}

/* 0002580e: 1 when the current piece's spec (low byte) is >= 0x4f */
uint32_t seg5_02580e(void) {
    return (RU16(gPieceSpec) & 0xff) >= 0x4f ? 1 : 0;
}

/* 0002582a */
uint32_t GetTorchFrame(int16_t f) {
    if (LEVEL_TYPE() == 3) return CaveGetTorchFrame(f);
    return TempGetTorchFrame(f);
}

/* 00025872: mark a cell of the row above the room (cell -10..-1) for redraw */
static uint32_t MarkTop_args(maddr ap) {
    int16_t col = R16(ap);
    if (col > -1 || col < -10) return 0;
    FRAME(f, 8);
    ComputeClipRect(f, (int16_t)-(int16_t)(((int32_t)col + 1) % 10), -1);
    if (R32(ap + 2) != 0) {
        if (!SECTRECT(R32(ap + 2), f, f)) { FRAME_END(f); return 0; }
    }
    col = (int16_t)-(int16_t)(col + 1);
    W16(ap, col);
    maddr e = R32(gMarkTopBuf) + (int32_t)col * 10;
    if (R16(e) != 0) {
        UNIONRECT(f, e + 2, e + 2);
    } else {
        int32_t off = (int32_t)R16(ap) * 10;
        W16(R32(gMarkTopBuf) + off, 1);
        mcopy(R32(gMarkTopBuf) + off + 2, f, 8);
    }
    FRAME_END(f);
    return 0;
}
uint32_t MarkTop(int16_t col, uint32_t rect) {
    FRAME(a, 6);
    W16(a, col); W32(a + 2, rect);
    uint32_t r = MarkTop_args(a);
    FRAME_END(a);
    return r;
}
ARGS_GLUE(MarkTop)

/* 00025962 */
uint32_t MarkMove(int16_t cell, uint32_t rect) {
    if (cell >= 0x1e) return 0;
    if (cell >= 0) MarkRectBuf(R32(gMoveBuf) + (int32_t)cell * 10, cell, rect);
    else MarkTop(cell, rect);
    return 0;
}

/* 000259c6 */
uint32_t MarkFloor(int16_t cell, uint32_t rect) {
    if (cell >= 0x1e) return 0;
    if (cell >= 0) MarkRectBuf(R32(gFloorBuf) + (int32_t)cell * 10, cell, rect);
    else MarkTop(cell, rect);
    return 0;
}

/* 00025a2c */
uint32_t MarkHalf(int16_t cell) {
    if (cell >= 0x1e) return 0;
    if (cell >= 0) W16(R32(gHalfBuf) + (int32_t)cell * 2, 1);
    else MarkTop(cell, 0);
    return 0;
}

/* 00025a70 */
uint32_t MarkRedraw(int16_t cell) {
    if (cell >= 0x1e) return 0;
    if (cell >= 0) W16(R32(gRedrawBuf) + (int32_t)cell * 2, 1);
    else MarkTop(cell, 0);
    return 0;
}

/* 00025ab6: mark the current piece's cell for redraw, relative to the current room */
uint32_t FUN_00025ab6(void) {
    int16_t cell = R16(gPieceCell);
    int16_t room = R16(gPieceRoom);
    if (room == R16(gCurRoom)) {
    } else if (room == R16(gRoomLeft)) {
        cell = (int16_t)(cell - 9);
    } else if (R16(gPieceRoom) == R16(gRoomRight)) {
        cell = (int16_t)(cell + 9);
    } else if (R16(gPieceRoom) == R16(gRoomAbove)) {
        cell = (int16_t)(cell - 0x14);
    } else if (R16(gPieceRoom) == R16(gRoomBelow)) {
        cell = (int16_t)(cell + 0x14);
    }
    MarkRedraw(cell);
    return 0;
}

/* 00025b10 */
uint32_t MarkFRedraw(int16_t cell, uint32_t rect) {
    if (cell >= 0x1e) return 0;
    if (cell >= 0) MarkRectBuf(R32(gFRedrawBuf) + (int32_t)cell * 10, cell, rect);
    else MarkTop(cell, rect);
    return 0;
}

/* 00025b78: call mark(cell, rect) for every cell the rectangle touches. Callers push a
 * third word (-1) that is not used. */
uint32_t MarkBufsForRect(uint32_t mark, uint32_t rect) {
    if (EMPTYRECT(rect)) return 0;
    int16_t top, bottom, left, right, v;
    v = R16(rect);
    if (v >= 5) {
        top = (int16_t)(((int32_t)v - 5) / 0x78);
        if (top > 2) top = 2;
    } else top = -1;
    v = R16(rect + 4);
    if (v >= 5) {
        bottom = (int16_t)(((int32_t)v - 5) / 0x78);
        if (bottom > 2) bottom = 2;
    } else bottom = -1;
    if (R16(rect + 2) <= 0x32) left = 0;
    else if (R16(rect + 2) >= 0x1cc) left = 9;
    else left = (int16_t)(R16(rect + 2) / 0x33);
    if (R16(rect + 6) <= 0x32) right = 0;
    else if (R16(rect + 6) >= 0x1cc) right = 9;
    else right = (int16_t)(R16(rect + 6) / 0x33);
    for (int16_t row = top; row <= bottom; row++)
        for (int16_t col = left; col <= right; col++) {
            int16_t cell = S16(IndexCell(col, row));
            call_ptr(mark, "wl", (uint32_t)(uint16_t)cell, rect);
        }
    return 0;
}

/* 00025cdc: a -= b where the difference is a rectangle; a becomes empty when covered */
uint32_t PoPDiffRect(uint32_t a, uint32_t b) {
    FRAME(f, 8);                     /* the intersection (A6-8): top, left, bottom, right */
    uint8_t r = SECTRECT(a, b, f);
    if (r) {
        r = (uint8_t)_EqualRect(a, f);
        if (r) {
            mcopy(a, gEmptyRect, 8);
        } else {
            int16_t keepV = 0;
            if (R16(f + 2) > R16(a + 2)) {
                if (R16(f + 6) >= R16(a + 6)) W16(a + 6, R16(f + 2));
                else keepV = 1;
            } else if (R16(f + 6) < R16(a + 6)) {
                if (R16(f + 2) > R16(a + 2)) keepV = 1;
                else W16(a + 2, R16(f + 6));
            } else {
                keepV = 1;
            }
            if (keepV) {
                if (R16(f) < R16(a + 4)) W16(a + 4, R16(f));
                else if (R16(f + 4) > R16(a)) W16(a, R16(f + 4));
            }
            r = EMPTYRECT(a);
            if (r) mcopy(a, gEmptyRect, 8);
        }
    }
    FRAME_END(f);
    return r;
}

/* 00025dcc: trigger every object on the link chain starting at `link` */
static uint32_t Trigger_args(maddr ap) {
    int16_t room = S16(GetLinkRoom(R16(ap)));
    if (room == -2 || R16(ap) == -3) return 0;
    do {
        room = S16(GetLinkRoom(R16(ap)));
        CalcBlue(room);
        int16_t cell = S16(GetLinkCell(R16(ap)));
        int16_t id = R16(R32(gRoomTiles) + (int32_t)cell * 2);
        int16_t phase = S16(TriggerObject(id, room, cell, R16(ap + 2)));
        if (phase >= 0) AddTrob(room, cell, phase, id);
        W16(ap, GetLinkNext(R16(ap)));
    } while (R16(ap) != -3);
    return 0;
}
uint32_t Trigger(int16_t link, int16_t how) {
    FRAME(f, 4);
    W16(f, link); W16(f + 2, how);
    uint32_t r = Trigger_args(f);
    FRAME_END(f);
    return r;
}
ARGS_GLUE(Trigger)

/* 00025e68 */
uint32_t TriggerFlask(int16_t room, int16_t cell) {
    int32_t off = (int32_t)cell * 4;
    int16_t v = (int16_t)(R16(R32(gRoomSpecs) + off) & 0xffe0);
    v = (int16_t)(v | (int16_t)(S16(Rnd(0x1a)) + 1));
    W16(R32(gRoomSpecs) + off, v);
    AddTrob(room, cell, 1, 10);
    return 0;
}

/* 00025ed0: start a gate moving; returns the new trob phase or -1 */
uint32_t TriggerGate(int16_t room, int16_t cell, int16_t how) {
    int16_t phase = -1;
    int16_t spec = R16(R32(gRoomSpecs) + (int32_t)cell * 4);
    int16_t pos = (int16_t)(spec & 0xff);
    int16_t hi = (int16_t)(spec & 0xff00);
    switch (how) {
        case 5:
            if (pos == 0xff) break;
            if ((uint16_t)pos >= 0xb8) {
                int32_t off = (int32_t)cell * 4;
                W16(R32(gRoomSpecs) + off, (int16_t)(hi + 0xed));
                if (R16(gKid_1c) < 0) {
                    if (R16(gExitRoom) == 0) break;
                    if (R16(gKidRoom) == R16(gExitRoom)) break;
                }
                W16(R32(gRoomSpecs) + off, (int16_t)(hi + 0xff));
                phase = 3;
            } else {
                W16(R32(gRoomSpecs) + (int32_t)cell * 4, (int16_t)(((pos + 3) & 0xfc) + hi));
                if (R16(gKid_1c) >= 0) phase = 3;
                else if (R16(gExitRoom) == 0) phase = 2;
                else if (R16(gKidRoom) == R16(gExitRoom)) phase = 2;
                else phase = 3;
            }
            break;
        case 0xe:
            if ((uint16_t)pos < 0xb8) phase = 3;
            else W16(R32(gRoomSpecs) + (int32_t)cell * 4, (int16_t)(hi + 0xff));
            break;
        case 6: {
            maddr t = GetTrobRoomCell(room, cell);
            if (S16(IsDeadPos(R16(gKidFrame))) != 0 && R16(gKid_1c) >= 2) break;
            if (t == 0 && pos != 0) phase = 4;
            else if (t != 0 && R16(t + 4) != 4) phase = 4;
            break;
        }
        case 0x22:
            phase = S16(TriggerGateWithRevPlate(room, cell));
            break;
    }
    return (uint16_t)phase;
}

/* 00026048: trigger one object; returns the trob phase to add, or -1 */
uint32_t TriggerObject(int16_t id, int16_t room, int16_t cell, int16_t how) {
    int16_t r;
    switch ((uint16_t)id) {
        case 4:
            r = S16(TriggerGate(room, cell, how));
            break;
        case 0x11: {
            maddr gs = R32(gGameState);
            if (room != R16(gs + 0x2198) && R16(gs + 0x218a) == 8 && R16(GH(0x102ff6)) != 1) {
                r = -1;
                break;
            }
            r = (R16(R32(gRoomSpecs) + (int32_t)cell * 4) & 0xff) != 0 ? -1 : 1;
            if (r == 1 && S16(PrinceSndOn(0x129)) == 0 && LEVEL_NUM() != 0xd) AddSong(0x129);
            break;
        }
        case 0x21:
            if ((R16(gPieceSpec) & 0x2000) == 0) TriggerDart(room, cell);
            r = -1;
            break;
        case 0x1b:
            r = S16(TriggerCaveBars(room, cell));
            break;
        case 0x19:
            r = how == 5 ? 2 : 1;
            TriggerSlideRock(room, cell, r);
            r = -1;
            break;
        default:
            r = -1;
            break;
    }
    return (uint16_t)r;
}

/* 0002617a: light a torch */
uint32_t TriggerTorch(int16_t room, int16_t cell) {
    int32_t off = (int32_t)cell * 4;
    int16_t spec = R16(R32(gRoomSpecs) + off);
    int16_t lt = LEVEL_TYPE();
    if (lt == 3 && (spec & 0x10) != 0) return 0;
    if (lt == 2 && S16(TempVisibleTorch(room, cell)) == 0) return 0;
    int16_t n = LEVEL_TYPE() == 3 ? 8 : 5;
    int16_t v = (int16_t)(S16(Rnd(n)) + (spec & 0xfff0));
    W16(R32(gRoomSpecs) + off, v);
    AddTrob(room, cell, 1, 0x13);
    return 0;
}

/* 00026220: add a trob, or update the phase of the one already at (room, cell) */
uint32_t AddTrob(int16_t room, int16_t cell, int16_t phase, int16_t type) {
    maddr t = GetTrobRoomCell(room, cell);
    if (t != 0) {
        W16(t + 4, phase);
        return 0;
    }
    if (R16(gTrobCount) >= 0x14) return 0;
    W16(gTrobRoom, room);
    W16(gTrobCell, cell);
    W16(gTrobPhase, phase);
    W16(gTrobType, type);
    int16_t i = R16(gTrobCount);
    W16(gTrobCount, i + 1);
    mcopy(gTrobs + (int32_t)i * 8, gTrob, 8);
    return 0;
}

/* 00026292: clear the redraw buffers */
uint32_t FUN_00026292(void) {
    tc_memset(R32(gRedrawBuf), 0, 0x3c);
    tc_memset(R32(gMoveBuf), 0, 0x12c);
    tc_memset(R32(gFRedrawBuf), 0, 0x12c);
    tc_memset(R32(gFloorBuf), 0, 0x12c);
    tc_memset(R32(gHalfBuf), 0, 0x3c);
    tc_memset(R32(gMarkBuf20), 0, 0x3c);
    tc_memset(R32(gMarkTopBuf), 0, 0x64);
    return 0;
}

/* 00026318: step col by dir until a wall (or col leaves -1..10) */
static uint32_t Get1stWallColInDir_args(maddr ap) {
    for (;;) {
        W16(ap + 2, R16(ap + 2) + R16(ap + 6));
        int16_t id = S16(ReadCell(R16(ap), R16(ap + 2), R16(ap + 4)));
        if (S16(IsWall(id)) != 0) break;
        if (R16(ap + 2) < -1 || R16(ap + 2) > 10) break;
    }
    return (uint16_t)R16(ap + 2);
}
uint32_t Get1stWallColInDir(int16_t room, int16_t col, int16_t row, int16_t dir) {
    FRAME(f, 8);
    W16(f, room); W16(f + 2, col); W16(f + 4, row); W16(f + 6, dir);
    uint32_t r = Get1stWallColInDir_args(f);
    FRAME_END(f);
    return r;
}
ARGS_GLUE(Get1stWallColInDir)

/* 00026376 */
uint32_t GetSlideRockX(int16_t a, int16_t b) {
    if (LEVEL_TYPE() == 2) return TempGetSlideRockX(a, b);
    return 0;
}

/* links: game state + 0x1680 + link*10 = {room, cell, timer, before, next} */
/* 000263b8 */
uint32_t GetLinkNext(int16_t link) {
    maddr p = LINK_ADDR(link);
    return (p & 0xffff0000u) | RU16(p + 8);
}

/* 000263f8 */
uint32_t GetLinkTimer(int16_t link) {
    maddr p = LINK_ADDR(link);
    return (p & 0xffff0000u) | RU16(p + 4);
}

/* 00026438 */
uint32_t ChangeLinkTimer(int16_t link, int16_t timer) {
    W16(LINK_ADDR(link) + 4, timer);
    return 0;
}

/* 00026484 */
uint32_t GetLinkCell(int16_t link) {
    maddr p = LINK_ADDR(link);
    return (p & 0xffff0000u) | RU16(p + 2);
}

/* 000264c4 */
uint32_t LinkBefore(int16_t link) {
    return R16(LINK_ADDR(link) + 6) == -3 ? 0 : 1;
}

/* 0002650a */
uint32_t LinkMore(int16_t link) {
    return R16(LINK_ADDR(link) + 8) == -3 ? 0 : 1;
}

/* 0002654e */
uint32_t GetLinkRoom(int16_t link) {
    maddr p = LINK_ADDR(link);
    return (p & 0xffff0000u) | RU16(p);
}

/* 0002658c: a pressure plate was pushed */
static uint32_t PushPlate_args(maddr ap) {
    if (R16(ap) == 0) W16(ap, R16(gPieceId));
    if (R16(ap + 2) == -1) W16(ap + 2, R16(gPieceSpec) & 0xff);
    int16_t timer = S16(GetLinkTimer(R16(ap + 2)));
    int16_t cell = R16(gPieceCell);
    int16_t spec = (int16_t)(R16(gPieceSpec) | 0x2000);
    int16_t room = R16(gPieceRoom);
    if (timer == 0x1f) {
        Trigger(R16(ap + 2), R16(ap));
    } else {
        ChangeLinkTimer(R16(ap + 2), 5);
        if (timer >= 2) {
            Trigger(R16(ap + 2), R16(ap));
        } else {
            AddTrob(R16(gPieceRoom), R16(gPieceCell), 1, R16(ap));
            if (R16(gKidFrame) != 0xb9 && !(R16(gPieceRoom) == 4 && LEVEL_NUM() == 0xd))
                AddSound(0x1b);
            RedrawPlate();
            Trigger(R16(ap + 2), R16(ap));
            W16(gPressed, 1);
        }
    }
    W16(SPEC_ADDR(room, cell), spec);
    return 0;
}
uint32_t PushPlate(int16_t id, int16_t link) {
    FRAME(f, 4);
    W16(f, id); W16(f + 2, link);
    uint32_t r = PushPlate_args(f);
    FRAME_END(f);
    return r;
}
ARGS_GLUE(PushPlate)

/* 000266ae */
uint32_t FUN_000266ae(void) {
    int16_t link = (int16_t)(R16(gPieceSpec) & 0xff);
    ChangeLinkTimer(link, 0x1f);
    PushPlate(R16(gPieceId), link);
    return 0;
}

/* 000266d4: plate coming back up */
uint32_t FUN_000266d4(void) {
    if (R16(gTrobPhase) < 0) return 0;
    int16_t link = (int16_t)(R16(gObjSpec) & 0xff);
    int16_t timer = S16(GetLinkTimer(link));
    if (timer >= 0x1f) {
        W16(gTrobPhase, -1);
        return 0;
    }
    ChangeLinkTimer(link, (int16_t)(timer - 1));
    if (timer < 2) {
        W16(gTrobPhase, -1);
        RedrawPlate();
        W16(gObjSpec, RU16(gObjSpec) & 0xdfff);
    }
    return 0;
}

/* 0002672a */
uint32_t CloseExit(int16_t room, int16_t cell) {
    if (LEVEL_TYPE() == 1) return 0;
    maddr p = R32(gRoomSpecs) + (int32_t)cell * 4;
    W16(p, (int16_t)((R16(p) & 0xff00) + 0x4f));
    AddTrob(room, cell, 4, 0x11);
    return 0;
}

/* 00026786: turn (room, cell) into an empty space */
uint32_t MakeSpace(int16_t room, int16_t cell) {
    W16(TILE_ADDR(room, cell), 0);
    int16_t spec = R16(SPEC_ADDR(room, cell));
    maddr gs = R32(gGameState);
    if (R16(gs + 0x218a) == 5 && room == 3)
        spec = (int16_t)0xc000;
    else if (R16(gs + 0x2186) == 2)
        spec = (int16_t)((spec & 0x3080) + 3);
    else
        spec = (int16_t)((spec & 0xf80) + 3);
    W16(SPEC_ADDR(room, cell), spec);
    return 0;
}

/* 00026890: a loose floor starts shaking (arg != 0: and falling) */
uint32_t BreakLoose(int16_t arg) {
    if (R16(gKidChar) == 1 && arg == 0) {
        FUN_00026934();
        return 0;
    }
    int16_t spec = (int16_t)(R16(gPieceSpec) & 0xffbf);
    if (arg != 0) spec = (int16_t)((spec & 0xfff0) | 0xb);
    W16(SPEC_ADDR(R16(gPieceRoom), R16(gPieceCell)), spec);
    AddTrob(R16(gPieceRoom), R16(gPieceCell), arg, 0xb);
    AddSound(0x1c);
    return 0;
}

/* 00026934: shake a loose floor */
uint32_t FUN_00026934(void) {
    if ((R16(gPieceSpec) & 0xf) != 0) return 0;
    if (GetTrobRoomCell(R16(gPieceRoom), R16(gPieceCell)) != 0) return 0;
    int16_t n = (int16_t)(((uint32_t)(RU16(gPieceSpec) & 0x30) >> 4) + 1);
    int update;
    int near = 0;
    if (R16(gKidCol) == R16(gPieceCol)) {
        int16_t krow = R16(gKidRow);
        int16_t prow = R16(gPieceRow);
        if (krow == prow || (int32_t)prow - 1 == (int32_t)krow || krow == 0 || krow == 2) near = 1;
    }
    if (!near && n != 3) update = 1;
    else update = R16(gKidChar) == 1;
    if (update) {
        W16(gPieceSpec, RU16(gPieceSpec) & 0xffcf);
        W16(gPieceSpec, RU16(gPieceSpec) | (uint16_t)(n << 4));
        W16(gPieceSpec, RU16(gPieceSpec) | 0x40);
        W16(R32(gRoomSpecs) + (int32_t)R16(gPieceCell) * 4, RU16(gPieceSpec) | 0x40);
    }
    AddTrob(R16(gPieceRoom), R16(gPieceCell), 1, 0xb);
    AddSound(0x1c);
    return 0;
}

/* 000269f6: shake the loose floors of a row (something landed) */
uint32_t ShakeEm(int16_t room, int16_t row) {
    uint8_t saved[16];
    for (int i = 0; i < 16; i++) saved[i] = R8(gMob + i);
    for (uint16_t col = 0; col < 10; col++) {
        int16_t id = S16(ReadCell(room, (int16_t)col, row));
        switch ((uint16_t)id) {
            case 0xb: FUN_00026934(); break;
            case 0xf: seg7_033c50(); break;
            case 0x1a: seg10_03ca4a(); break;
        }
    }
    if (R16(R32(gGameState) + 0x21a4) == 2) ShakeBones(room, row);
    for (int i = 0; i < 16; i++) W8(gMob + i, saved[i]);
    return 0;
}

/* 00026a94: append gMob to the mob list */
uint32_t FUN_00026a94(void) {
    if (R16(gMobCount) >= 0x1e) return 0;
    int16_t i = R16(gMobCount);
    W16(gMobCount, i + 1);
    mcopy(R32(gMobs) + (int32_t)i * 16, gMob, 16);
    return 0;
}

/* 00026abc: the n-th (1-based) live mob of a type, or 0 */
uint32_t GetMobTypeItem(int16_t type, int16_t n) {
    maddr found = 0;
    if (R16(gMobCount) == 0) return 0;
    maddr m = R32(gMobs);
    int16_t i = 0, k = 0;
    if (R16(gMobCount) <= 0) return 0;
    for (;;) {
        if (R16(m + 0xa) == type && R16(m + 6) != -1) {
            k++;
            if (k == n) found = m;
        }
        i++;
        m += 16;
        if (i >= R16(gMobCount) || found != 0) break;
    }
    return found;
}

/* 00026b28: load the current object (spec, id) of a cell of the CalcBlue room */
uint32_t GetOS(int16_t cell) {
    W32(gObjSpec, R32(R32(gRoomSpecs) + (int32_t)cell * 4));
    W16(gObjId, R16(R32(gRoomTiles) + (int32_t)cell * 2));
    return 0;
}

/* 00026b68: animate every mob, then drop the dead ones */
uint32_t FUN_00026b68(void) {
    int16_t dead = 0;
    int16_t n = R16(gMobCount);
    maddr m = R32(gMobs);
    W16(gMobIdx, 0);
    if (0 < R16(gMobCount)) {
        do {
            mcopy(gMob, m, 16);
            FUN_00026c14();
            mcopy(m, gMob, 16);
            dead |= R16(m + 6) == -1 ? 1 : 0;
            W16(gMobIdx, R16(gMobIdx) + 1);
            m += 16;
        } while (R16(gMobIdx) < n);
    }
    if (dead) {
        m = R32(gMobs);
        maddr dst = R32(gMobs);
        n = R16(gMobCount);
        W16(gMobCount, 0);
        for (int16_t i = 0; i < n; i++, m += 16) {
            if (R16(m + 6) == -1) continue;
            if (dst != m) mcopy(dst, m, 16);
            dst += 16;
            W16(gMobCount, R16(gMobCount) + 1);
        }
    }
    return 0;
}

/* 00026c14: animate gMob according to its type
 * (a type >= 0x8000 would make the original jump outside its table) */
uint32_t FUN_00026c14(void) {
    switch (RU16(gMobType)) {
        case 0: case 3:
            FUN_00026c90();
            if (R16(gMobVel) <= 0) W16(gMobVel, R16(gMobVel) + 1);
            FUN_000276f6();
            break;
        case 2: MobDart(); break;
        case 4: MobSpikes(); break;
        case 6: FUN_0003c1fc(); CheckSlideRockCrush(); break;
        case 10: FUN_0003c99a(); break;
        case 11: FUN_00034b62(); break;
        case 12: seg9_03a350(); break;
        case 13: FUN_0006f2a0(); break;
    }
    return 0;
}

/* 00026c90: falling loose floor */
uint32_t FUN_00026c90(void) {
    int16_t vel = R16(gMobVel);
    if (vel < 0) return 0;
    if (vel < 0x3c) W16(gMobVel, R16(gMobVel) + 8);
    W16(gMobY, R16(gMobY) + R16(gMobVel));
    if (R16(gMobRoom) == 0) {
        int32_t top = (int32_t)R16(gMobFloorTop + (int16_t)((uint32_t)RU16(gMobType) * 2)) + 0x16d;
        if (top <= (int32_t)R16(gMobY)) W16(gMobVel, -2);
        return 0;
    }
    if ((int32_t)R16(gMobRow) * 0x78 + 0x78 > (int32_t)R16(gMobY)) return 0;
    int16_t id = S16(ReadCell(R16(gMobRoom), (int16_t)(R16(gMobX) / 0x33), R16(gMobRow)));
    if (id == 0xb) FUN_000270c2();
    else if (id == 0xf) seg7_033c50();
    if (id == 0xb || id == 0xf || S16(IsSpace(id)) != 0) {
        FUN_00027156();
        return 0;
    }
    AddSound(0x1a);
    ShakeEm(R16(gMobRoom), R16(gMobRow));
    W16(gMobY, (int16_t)(R16(gMobRow) * 0x78) + 0x78);
    W16(gMobVel, -2);
    W16(gMobLanded, 1);
    W16(R32(gMobs) + (int32_t)R16(gMobIdx) * 16 + 0xe, 1);
    MakeRubble();
    return 0;
}

/* 00026db6: a loose floor landed: turn what it hit into rubble */
uint32_t MakeRubble(void) {
    int16_t newId = 0;
    int16_t col = (int16_t)(R16(gMobX) / 0x33);
    int16_t id = S16(ReadCell(R16(gMobRoom), col, R16(gMobRow)));
    int16_t room = R16(gPieceRoom);
    col = R16(gPieceCol);
    int16_t row = R16(gPieceRow);
    int16_t cell = R16(gPieceCell);
    switch ((uint16_t)id) {
        case 5:
            W16(TILE_ADDR(R16(gPieceRoom), R16(gPieceCell)), 0xe);
            newId = 0xe;
            /* fall through */
        case 0x22:
        case 6:
            PushPlate(newId, -1);
            if (id == 0x22)
                W16(R32(gGameState) + (uint32_t)(uint16_t)room * 0x78 + (int32_t)cell * 4 + 0x708,
                    RU16(gPieceSpec) | 0x3000);
            ReadCell(R16(gMobRoom), (int16_t)(R16(gMobX) / 0x33), R16(gMobRow));
            W16(gPieceSpec, RU16(gPieceSpec) & 0xff00);
            if (id == 5) W16(gPieceSpec, RU16(gPieceSpec) | 4);
            else if (id == 6) W16(gPieceSpec, RU16(gPieceSpec) | 8);
            id = R16(gPieceId);
            /* fall through */
        case 1: case 0x21: case 2: case 0xa:
            if (id == 0xa) {
                maddr t = GetTrobRoomCell(R16(gPieceRoom), R16(gPieceCell));
                if (t != 0) {
                    mcopy(gTrob, t, 8);
                    RedrawFlask();
                }
            }
            /* fall through */
        case 0x13: case 0x20:
            id = S16(ReadCell(room, col, row));
            if (id == 0x13 || id == 0x20) {
                W16(TILE_ADDR(room, cell), 0x20);
            } else {
                if (id == 0xa || id == 1) W16(gPieceSpec, 0);
                W16(TILE_ADDR(room, cell), 0xe);
                maddr gs = R32(gGameState);
                if (R16(gs + 0x2186) == 4) {
                    W16(gs + (uint32_t)(uint16_t)room * 0x78 + (int32_t)cell * 4 + 0x708,
                        RU16(gPieceSpec) | 0x40);
                } else {
                    gs = R32(gGameState);
                    int16_t lt = R16(gs + 0x2186);
                    if ((lt == 3 || lt == 2) && id != 1)
                        W16(gs + (uint32_t)(uint16_t)room * 0x78 + (int32_t)cell * 4 + 0x708, 0);
                }
            }
            MarkMobRedraw(0);
            break;
        case 0xc: case 0xd:
            W16(R32(gGameState) + (uint32_t)RU16(gPieceRoom) * 0x78 + (int32_t)R16(gPieceCell) * 4 + 0x708,
                RU16(gPieceSpec) | 0x20);
            MarkMobRedraw(0);
            break;
    }
    return 0;
}

/* 000270c2: a falling floor hit another loose floor: knock it down too */
uint32_t FUN_000270c2(void) {
    MakeSpace(R16(gPieceRoom), R16(gPieceCell));
    W16(gMobVel, R16(gMobVel) >> 1);
    mcopy(R32(gMobs) + (int32_t)R16(gMobIdx) * 16, gMob, 16);
    W16(gMobY, (int16_t)(R16(gMobRow) * 0x78) + 0x7d);
    MarkMobRedraw(0);
    W16(gMobY, R16(R32(gMobs) + (int32_t)R16(gMobIdx) * 16 + 2) + 6);
    FUN_00027156();
    FUN_00026a94();
    mcopy(gMob, R32(gMobs) + (int32_t)R16(gMobIdx) * 16, 16);
    MarkMobRedraw(0);
    return 0;
}

/* 00027156: gMob moves down one row (into the room below after row 2) */
uint32_t FUN_00027156(void) {
    W16(gMobRow, R16(gMobRow) + 1);
    if (R16(gMobRow) < 3) return 0;
    W16(gMobY, R16(gMobY) - 0x16d);
    W16(gMobRow, 0);
    W16(gMobRoom, ROOM_LINK(R16(gMobRoom), -2));
    return 0;
}

/* 00027190: is a landed loose floor (mob) lying on the current piece? */
uint32_t seg5_027190(void) {
    int16_t type = LEVEL_TYPE() == 3 ? 0 : 3;
    int16_t found = 0;
    int16_t n = 0;
    maddr m;
    do {
        n++;
        m = GetMobTypeItem(type, n);
        if (m != 0 && R16(m + 4) == R16(gPieceRoom) && R16(m + 0xc) == R16(gPieceRow) &&
            (int16_t)(R16(m) / 0x33) == R16(gPieceCol) && R16(m + 0xe) != 0)
            found = 1;
    } while (m != 0 && found == 0);
    return (m & 0xffff0000u) | (uint16_t)found;
}

/* 000271fc: run the per-type "ATM" routine of every mob */
uint32_t seg5_0271fc(void) {
    maddr m = R32(gMobs);
    int16_t i = 0;
    if (R16(gMobCount) <= 0) return 0;
    do {
        mcopy(gMob, m, 16);
        FUN_00027240();
        mcopy(m, gMob, 16);
        i++;
        m += 16;
    } while (i < R16(gMobCount));
    return 0;
}

/* 00027240 (a type >= 0x8000 would make the original jump outside its table) */
uint32_t FUN_00027240(void) {
    switch (RU16(gMobType)) {
        case 0: case 3: ATMLoose(); break;
        case 2: seg8_036dda(); break;
        case 4: ATMSpikes(); break;
        case 6: ATMSlideRock(); break;
        case 10: ATMTrapDoor(); break;
        case 11: ATMBrgFloor(); break;
        case 12: ATMFireball(); break;
        case 13: ATMGlass(); break;
    }
    return 0;
}

/* 000272a8: draw a falling loose floor when it is in (or entering) the current room */
uint32_t ATMLoose(void) {
    FRAME(f, 8);
    int16_t visible = 1;
    int16_t y = R16(gMobY);
    int16_t h = R16(gMobFloorTop + (int16_t)((uint32_t)RU16(gMobType) * 2));
    if (R16(gMobRoom) == R16(gCurRoom)) {
        if (!((int32_t)h + 0x16c > (int32_t)R16(gMobY))) visible = 0;
    } else if (R16(gMobRoom) == R16(gRoomBelow)) {
        if (R16(gMobY) >= h) visible = 0;
        else y = (int16_t)(R16(gMobY) + 0x16c);
    } else if (R16(gMobRoom) == R16(gRoomAbove)) {
        if (0x16c - (int32_t)h > (int32_t)R16(gMobY)) visible = 0;
        else y = (int16_t)(R16(gMobY) - 0x16c);
    } else {
        int16_t left = R16(gRoomLeft);
        if (R16(gMobRoom) == left) {
            int16_t x = R16(gMobX);
            if ((int32_t)R16(gMobX) + (int32_t)R16(gMobXOff + (int16_t)((uint32_t)RU16(gMobType) * 2)) > 0x1fe) {
                W16(gMobX, (int16_t)(x - 0x1fe));
                if (R16(gMobRoom) == left) W16(gMobRoom, R16(gCurRoom));
                else if (R16(gMobRoom) == R16(gRoomAboveL)) W16(gMobRoom, R16(gRoomAbove));
            } else {
                visible = 0;
            }
        } else {
            visible = 0;
        }
    }
    if (visible) {
        int16_t col = (int16_t)(R16(gMobX) / 0x33);
        int16_t row = S16(GetRow(y));
        W16(GH(0x104b38), IndexCell1(col, row));
        MarkMobRedraw(1);
        ComputeMobRect(f);
        SECTRECT(gScreenRect, f, f);
        AddMobObj(y);
        col++;
        int16_t cell = S16(IndexCell(col, row));
        if (R16(gMobRoom) != R16(gRoomBelow)) MarkFloor(cell, f);
        int16_t row2 = S16(GetRow((int16_t)(y - h)));
        if (row2 != row) {
            cell = S16(IndexCell(col, row2));
            MarkFloor(cell, f);
        }
    }
    FRAME_END(f);
    return 0;
}
