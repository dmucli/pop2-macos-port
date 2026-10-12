/* CODE 4 "Characters 2" (second half, 0x20e5e-0x24180): character movement helpers,
 * falling/landing, life points, player control glue, cell lookups around the character,
 * clipping of character sprites, and the collision-detection ("barrier") code.
 * Ported from the Ghidra decompilation of the original 68k code; addresses in
 * comments are Ghidra addresses (see docs/PORTING.md). */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* ---- the character records (0x3e bytes each) ----
 * gOpp (0x102f2e) <- gSaved (0x102f6c) <- gChar (0x102faa); gCharTab (0x102df8) holds one
 * record per character index (field +0). The current character ("Char") is at 0x102faa. */
#define CHAR_SIZE       0x3e
#define gCharTab        GH(0x102df8)   /* records indexed by character id */
#define gOpp            GH(0x102f2e)   /* record: the opponent / previous copy */
#define gSaved          GH(0x102f6c)   /* record: saved copy of the current character */
#define gChar           GH(0x102faa)   /* record: the current character */
#define CH_ID           GH(0x102faa)   /* word: character id (index into gCharTab) */
#define CH_FACE         GH(0x102fac)   /* word: facing (-1 = left, 0 = right) */
#define CH_X            GH(0x102fae)   /* word: x */
#define CH_Y            GH(0x102fb0)   /* word: y */
#define CH_TYPE         GH(0x102fb2)   /* word: character type (0 = kid, ...) */
#define CH_FRAME        GH(0x102fb4)   /* word: animation frame */
#define CH_COL          GH(0x102fb6)   /* word: cell column */
#define CH_ROW          GH(0x102fb8)   /* word: cell row */
#define CH_ACTION       GH(0x102fba)   /* word: action */
#define CH_XVEL         GH(0x102fbc)   /* word: x velocity */
#define CH_YVEL         GH(0x102fbe)   /* word: y velocity */
#define CH_ROOM         GH(0x102fc0)   /* word: room */
#define CH_FLAG18       GH(0x102fc2)   /* word */
#define CH_ALIVE        GH(0x102fc6)   /* word: <0 alive, >=0 death counter */
#define CH_LIFE         GH(0x102fc8)   /* word: life points */
#define CH_LIFEDELTA    GH(0x102fcc)   /* word: pending life change (negative = damage) */
#define CH_SEQFRAME     GH(0x102fd2)   /* word */
#define CH_FIELD34      GH(0x102fde)   /* word */

#define gGameState      GH(0x102fec)   /* long: pointer to the game state block */
#define LEVEL_TYPE()    R16(R32(gGameState) + 0x2186)
#define LEVEL_TYPE2()   R16(R32(gGameState) + 0x218a)
#define gCurRoom        GH(0x10303e)   /* word: current room */
#define gOppIdx         GH(0x102f68)   /* word: index of the opponent */
#define gFrameFlags     GH(0x102df6)   /* word: flags of the current frame (low 6 bits: dx) */
#define gFrameDX        GH(0x102df2)   /* word */
#define gCellKind       GH(0x104b50)   /* word: kind of the last cell read (ReadCell) */
#define gCellInfo       GH(0x104b52)   /* word: info of the last cell read */
#define gDistInCell     GH(0x1037e4)   /* word: x position inside the column (set by GetCol) */
#define gDirDX          GH(0x103468)   /* word[2] indexed by face: column step for "in front" */
#define gDirDX2         GH(0x10346c)   /* word[2] indexed by face */

/* the player input state (0x104b6e..0x104b81 saved copy, 0x104b78.. current) */
#define gInSaved        GH(0x104b6e)   /* 10 bytes: saved copy of gInLeft..gInBtn */
#define gInLeft         GH(0x104b78)
#define gInRight        GH(0x104b7a)
#define gInUp           GH(0x104b7c)
#define gInDown         GH(0x104b7e)
#define gInBtn          GH(0x104b80)
#define gJoyX           GH(0x104b68)   /* word: -1/0/1 */
#define gJoyY           GH(0x104b6a)   /* word: -1/0/1 */
#define gJoyBtn         GH(0x104b6c)   /* word */

/* 00020e5e: save the current character record into gOpp (after syncing the guard table) */
uint32_t FUN_00020e5e(void) {
    int16_t id = R16(CH_ID);
    if (id >= 0 && id < 5) seg4_022516();
    mcopy(gOpp, gSaved, CHAR_SIZE);
    return 0;
}

/* 00020e86: store gSaved into gCharTab[gSaved.id] */
uint32_t seg4_020e86(void) {
    int32_t idx = R16(gSaved);
    mcopy(gCharTab + idx * CHAR_SIZE, gSaved, CHAR_SIZE);
    return 0;
}

/* 00020eb6: *out = x < 0 ? x - 1 : x */
uint32_t ConvertX(int16_t x, uint32_t out) {
    int16_t v = (x < 0) ? (int16_t)(x - 1) : x;
    W16(out, v);
    return 0;
}

/* 00020eea: accelerate the fall (y velocity) while falling (action 4 or 9) */
uint32_t FUN_00020eea(void) {
    int16_t act = R16(CH_ACTION);
    if (act != 4 && act != 9) return 0;
    if (G16(-0x4fa6) != 0) {
        W16(CH_YVEL, R16(CH_YVEL) + 2);
        if (R16(CH_YVEL) > 8) W16(CH_YVEL, 8);
    } else {
        if (R16(CH_YVEL) >= 0x3f) return 0;
        W16(CH_YVEL, R16(CH_YVEL) + 6);
        if (R16(CH_YVEL) > 0x3f) W16(CH_YVEL, 0x3f);
    }
    return 0;
}

/* 00020f34: apply the x/y velocities to the character */
uint32_t seg4_020f34(void) {
    if (R16(CH_Y) >= 0xe42) return 0;
    if (R16(CH_YVEL) != 0) {
        W16(CH_Y, R16(CH_Y) + R16(CH_YVEL));
        int16_t t = R16(CH_TYPE);
        if (t == 7 || t == 8) W16(CH_ROW, GetRow(R16(CH_Y)));
    }
    if (R16(CH_ACTION) != 4) {
        int16_t t = R16(CH_TYPE);
        if (t != 7 && t != 8) return 0;
    }
    int16_t dx;
    if (R16(CH_TYPE) == 0 && G16(-0x4fa6) != 0) dx = (int16_t)((int32_t)R16(CH_XVEL) >> 1);
    else dx = R16(CH_XVEL);
    W16(CH_X, AddCharX(dx));
    FUN_00020542();
    return 0;
}

/* 00020fb2: stop the character (x and y velocity = 0) */
uint32_t seg4_020fb2(void) {
    W16(CH_XVEL, 0);
    W16(CH_YVEL, 0);
    return 0;
}

/* 00020fc2: take damage points; returns 1 if the character dies */
uint32_t DecLifePts(int16_t pts) {
    int16_t res = 0;
    if (R16(CH_TYPE) == 4 && R16(CH_ROW) != 0 && LEVEL_TYPE2() == 5) {
        int16_t room = R16(CH_ROOM);
        if (room == 10 || room == 7 || room == 0xc) return (uint16_t)res;
    }
    int16_t life = R16(CH_LIFE);
    if (pts >= life) {
        if ((int32_t)R16(CH_LIFEDELTA) > -(int32_t)life) W16(CH_LIFEDELTA, (int16_t)-life);
        res = 1;
    } else if ((int32_t)pts > -(int32_t)R16(CH_LIFEDELTA)) {
        W16(CH_LIFEDELTA, (int16_t)-pts);
        res = 0;
    } else {
        res = (-(int32_t)R16(CH_LIFEDELTA) == (int32_t)R16(CH_LIFE)) ? 1 : 0;
    }
    return (uint16_t)res;
}

/* 00021076: read the cell the character is in */
uint32_t FUN_00021076(void) {
    return ReadCell(R16(CH_ROOM), R16(CH_COL), R16(CH_ROW));
}

/* 0002108a: check whether the character steps off into empty space and start the fall */
uint32_t seg4_02108a(void) {
    if ((R16(gFrameFlags) & 0x40) == 0) return 0;
    if (R16(CH_TYPE) == 10) return 0;
    int16_t cell = S16(FUN_00021076());
    if (S16(IsWall(cell)) != 0) {
        int call = 1;
        if (cell == 7) {
            uint16_t fr = RU16(CH_FRAME);
            if (fr >= 0xf6 && fr <= 0x105) call = 0;
            else if (R16(CH_TYPE) == 0xb) call = 0;
        }
        if (call) cell = S16(seg4_021452());
    }
    if (S16(IsSpace(cell)) == 0) {
        if (cell != 0x1e) return 0;
        if ((RU16(gCellInfo) & 0xf) >= 2) return 0;
    }
    if (LEVEL_TYPE() == 5 && S16(RoofClimbingIntoShip()) != 0) return 0;
    if (LEVEL_TYPE() == 2 && S16(SlideRockInCell(0x19)) != 0) return 0;
    if (R16(CH_TYPE) == 0xb) {
        int16_t d = S16(seg4_021562());
        W16(CH_X, AddCharX((int16_t)(d - 0x44)));
    } else {
        FUN_000226ee();
    }
    return 0;
}

/* 00021158: per-frame fall step: gravity, velocities, falling off the bottom */
uint32_t seg4_021158(void) {
    FUN_00020eea();
    seg4_020f34();
    if (LEVEL_TYPE() == 5) {
        if (R16(CH_Y) > 0x2da) {
            int16_t room = R16(gCurRoom);
            if (room != 0x13 && room != 0x10) {
                AddSound(7);
                DecLifePts(100);
                W16(CH_FRAME, 0xb9);
                SetKidDeathMethod(3);
            }
            W16(CH_Y, 0x2da);
            W16(CH_YVEL, 0);
            W16(CH_ACTION, 1);
            if (R16(CH_TYPE) != 0 && R16(CH_ID) == R16(gOppIdx))
                W16(gOppIdx, GetOppIdxClosestToKid(R16(CH_FACE)));
        } else if (R16(CH_TYPE) == 2 && R16(CH_FRAME) != 0xb9) {
            if (S16(PrinceSndOn(0x1e)) == 0) AddSound(0x1e);
        }
    } else if (R16(CH_Y) > 0x2da) {
        FUN_0002121a();
    }
    return 0;
}

/* 0002121a: the character fell out of the level: kill it */
uint32_t FUN_0002121a(void) {
    DecLifePts(100);
    W16(CH_FRAME, 0xb9);
    SetKidDeathMethod(3);
    W16(CH_Y, 0x2da);
    W16(CH_YVEL, 0);
    W16(CH_ACTION, 1);
    if (R16(CH_TYPE) == 1) CutToKidRoom();
    return 0;
}

/* 00021258: is the character outside the playable rooms (room 0, or the bottom of some rooms)? */
uint32_t seg4_021258(void) {
    if (R16(CH_ROOM) == 0) return 1;
    if ((R16(CH_ROOM) == 3 && LEVEL_TYPE() == 6) || (R16(CH_ROOM) == 5 && LEVEL_TYPE() == 6)) {
        if (R16(CH_ROW) >= 0xb) return 1;
    }
    return 0;
}

/* 0002129a: while falling with the button held, try to grab a ledge */
uint32_t seg4_02129a(void) {
    if (R16(gInBtn) == 0) return 0;
    if (R16(CH_YVEL) >= 0x3c) return 0;
    if (R16(CH_ALIVE) >= 0) return 0;
    int32_t y = (int32_t)R16(CH_Y) + 0x30;
    int32_t lim = (int32_t)R16(CH_ROW) * 0x78 + 0x6a;
    if (y < lim) return 0;
    int16_t oldx = R16(CH_X);
    W16(CH_X, AddCharX(-0x11));
    if (LEVEL_TYPE2() == 5 && R16(gCurRoom) == 10 && R16(CH_FACE) == -1 &&
        G16(-0x36a2) == 3 && G16(-0x36a0) >= 0x28) {
        W16(CH_X, AddCharX(0x19));
        FUN_0002054c();
        if (R16(CH_COL) <= 2) W16(CH_X, AddCharX(-0x19));
    }
    if (R16(CH_SEQFRAME) == 0x63) W16(CH_X, AddCharX(-7));
    FUN_0002054c();
    if (S16(FUN_0002140e()) == 0) {
        W16(CH_X, oldx);
        FUN_0002054c();
        return 0;
    }
    FUN_00020cca();
    W16(CH_YVEL, 0);
    JumpSeq(0xf);
    AnimChar();
    AddSound(9);
    FUN_00020542();
    GetCharEdges();
    int16_t d = (int16_t)(S16(seg4_021562()) + 6);
    if (S16(IsFloor(S16(FUN_00021ac8()))) != 0) d = (int16_t)(d - 0x33);
    if (R16(CH_FACE) == -1) {
        d = (int16_t)(d + 6);
        if (LEVEL_TYPE() == 5 && R16(CH_ROOM) == 0x13) d = (int16_t)(d - 0x2a);
    } else {
        d = (int16_t)(d + 6);
    }
    W16(CH_X, AddCharX(d));
    SG16(-0x4f96, 0xc);
    SG16(-0x4fa0, 0);
    return 0;
}

/* 0002140e: is there a ledge to grab in front of the character? */
uint32_t FUN_0002140e(void) {
    int16_t above = S16(FUN_00021ac8());
    int16_t info = G16(-0x34ae);
    int16_t front = S16(seg4_021b0e());
    int16_t r = S16(IsLedge(above, front, info, G16(-0x34ae)));
    if (r == 0 && LEVEL_TYPE() == 5) r = S16(FUN_0006f110());
    return (uint16_t)r;
}

/* 00021452: push the character out of a wall; returns the cell it ends up in */
uint32_t seg4_021452(void) {
    if (!(R16(CH_FIELD34) == 4 || R16(CH_TYPE) == 0xb ||
          (R16(CH_ROOM) == 3 && LEVEL_TYPE() == 6 && R16(CH_COL) < 0))) {
        int16_t d = S16(seg4_021562());
        int16_t front = S16(GetCellsInFront(1));
        if (d < 0x12 && S16(IsWall(front)) == 0) d = (int16_t)(d + 0x12);
        else d = (int16_t)(d - 0x33);
        W16(CH_X, AddCharX(d));
        FUN_00020542();
    }
    return FUN_00021076();
}

/* 000214cc: read the cell n columns in front of the character */
uint32_t GetCellsInFront(int16_t n) {
    int16_t step = R16(gDirDX + (int32_t)R16(CH_FACE) * 2);
    int16_t col = (int16_t)(step * n + R16(CH_COL));
    return ReadCell(R16(CH_ROOM), col, R16(CH_ROW));
}

/* 00021518: read the cell next to the character (gDirDX2 step) */
uint32_t FUN_00021518(void) {
    int16_t col = R16(CH_COL);
    col = (int16_t)(col + R16(gDirDX2 + (int32_t)R16(CH_FACE) * 2));
    return ReadCell(R16(CH_ROOM), col, R16(CH_ROW));
}

/* 00021542: x of the character's reference point for the current frame */
uint32_t seg4_021542(void) {
    int16_t fx = (int16_t)(R16(gFrameFlags) & 0x3f);
    int16_t v = (int16_t)(R16(gFrameDX) - fx);
    return AddCharX(v);
}

/* 00021562: distance of the character to the edge of its column, in facing direction */
uint32_t seg4_021562(void) {
    return GetDist1(S16(seg4_021542()));
}

/* 00021570: distance to the column edge for x, in facing direction */
uint32_t GetDist1(int16_t x) {
    GetCharCol(x);
    int16_t d;
    if (R16(CH_FACE) == 0) d = (int16_t)(0x32 - R16(gDistInCell));
    else d = R16(gDistInCell);
    return (uint16_t)d;
}

/* 000215a6: column of a character x */
uint32_t GetCharCol(int16_t x) {
    return GetCol((int16_t)(x - 0x16));
}

/* 000215ca: row of a y coordinate.  The original decrements its argument slot in place;
 * glue_GetRow reproduces that for 68k callers. */
uint32_t GetRow(int16_t y) {
    y = (int16_t)(y - 5);
    int16_t q = (int16_t)(y / 0x78);
    uint32_t hi = (q < 0) ? 0xffff0000u : 0;
    int16_t r = q;
    if (y <= 0) r = (int16_t)(r - 1);
    return hi | (uint16_t)r;
}
void glue_GetRow(void) {
    maddr sp = emu_sp();
    int16_t y = (int16_t)rd16(sp);
    wr16(sp, (uint16_t)(y - 5));
    emu_set_d0(GetRow(y));
}

/* 00021600: y position adjusted for the room's position relative to the current room */
uint32_t FUN_00021600(void) {
    int16_t v = G16(-0x34a8);
    int16_t r = G16(-0x34aa);
    if (r == G16(-0x4fc2) || r == G16(-0x4fba) || r == G16(-0x4fbc)) return (uint16_t)v;
    if (r == G16(-0x4fc0) || r == G16(-0x4fb4) || r == G16(-0x4fb8)) return (uint16_t)(int16_t)(v - 10);
    r = G16(-0x34aa);
    if (r == G16(-0x4fbe) || r == G16(-0x4fb2) || r == G16(-0x4fb6)) v = (int16_t)(v + 10);
    return (uint16_t)v;
}

/* 00021658: player control + death sequence handling; returns PlayerCtrl's result (-1 = game over) */
uint32_t seg4_021658(void) {
    int16_t res = S16(PlayerCtrl());
    if (res != -2) return (uint16_t)res;
    if (G16(-0x4fee) == 0) return (uint16_t)res;
    if (R16(CH_ALIVE) < 0) return (uint16_t)res;
    if (S16(IsDeadPos(R16(CH_FRAME))) == 0) return (uint16_t)res;
    if (G16(-0x4f90) != 0) {
        ResetSoundItem(0);
        SG16(-0x50b0, G16(-0x50b2));
        FUN_000225d2();
        JumpSeq(2);
        W16(CH_X, R16(CH_X) + 0x19);
        AnimChar();
        FUN_00020542();
        seg2_016296();
    }
    if (R16(CH_TYPE) != 0 && R16(CH_TYPE) != 1) return (uint16_t)res;
    if (S16(PrinceSndOn(G16(-0x439a))) != 0) {
        int16_t s = G16(-0x439a);
        if (s != 0x110 && s != 0x11 && s != 0xc0) return (uint16_t)res;
    }
    SG16(-0x4fa2, 0);
    int16_t a = R16(CH_ALIVE);
    if (a >= 0 && a < 6) {
        W16(CH_ALIVE, R16(CH_ALIVE) + 1);
        return (uint16_t)res;
    }
    if (R16(CH_ALIVE) == 6) {
        AddKidDeathSong(G16(-0x500e));
        W16(CH_ALIVE, R16(CH_ALIVE) + 1);
        return (uint16_t)res;
    }
    if (R16(CH_ALIVE) != 7) return (uint16_t)res;
    if (S16(PrinceSndOn(G16(-0x439a))) != 0) return (uint16_t)res;
    if (S16(PrinceSndOn(G16(-0x4398))) != 0) return (uint16_t)res;
    if (G16(-0x4fce) == 0) {
        seg2_0175c4();
        res = -1;
    } else if (G16(-0x4fcc) != 0x258) {
        SG16(-0x4fcc, 0x258);
        SG16(-0x4fca, 0x258);
        DrawRestartMessage();
    }
    W16(CH_ALIVE, R16(CH_ALIVE) + 1);
    return (uint16_t)res;
}

/* 0002177e: one frame of player control */
uint32_t PlayerCtrl(void) {
    if (R16(CH_ALIVE) < 0 && R16(CH_LIFE) == 0) W16(CH_ALIVE, 0);
    if (G16(-0x4f96) != 0) SG16(-0x4f96, G16(-0x4f96) - 1);
    FUN_00021930();
    FUN_00015a02();
    if (G16(-0x4f92) != G16(-0x4fd2) || R16(CH_ROOM) == 0 || R16(CH_SEQFRAME) == 0x3b) {
        W16(gJoyX, 0);
        W16(gJoyY, 0);
    }
    FUN_00021954();
    FUN_00021866();
    int16_t r = S16(FUN_0002a1f2());
    FUN_00021920();
    return (uint16_t)r;
}

/* 000217f4: one frame of control for a computer-controlled character */
uint32_t FUN_000217f4(void) {
    int16_t a = R16(CH_ALIVE);
    if (a < 0) {
        if (R16(CH_LIFE) == 0) W16(CH_ALIVE, 0);
    } else if (a >= 6) {
        FUN_00028d38();
        if (R16(gOppIdx) == R16(CH_ID)) W16(gOppIdx, -1);
    } else if (R16(CH_FLAG18) != 0) {
        if (R16(CH_ALIVE) < 0x14) W16(CH_ALIVE, R16(CH_ALIVE) + 1);
        else W16(CH_FLAG18, 0);
    }
    AutoCtrl();
    if (G16(-0x4ffe) != 0) seg5_02a240();
    int16_t t = R16(CH_TYPE);
    if (t != 1 && t != 0xc) GenCtrl();
    return 0;
}

/* 00021866: run GenCtrl with the input mirrored for a character facing right */
uint32_t FUN_00021866(void) {
    int16_t face = R16(CH_FACE);
    if (R16(CH_FACE) == 0) FUN_000218ce();
    if (G16(-0x4fa8) != 0) FUN_000218ec();
    if (G16(-0x4ffe) != 0) seg5_02a240();
    GenCtrl();
    if (face == 0) FUN_000218ce();
    if (G16(-0x4fa8) != 0) FUN_000218ec();
    if (R16(CH_SEQFRAME) == 0xed && R16(CH_FRAME) == 0xb9 && R16(CH_ROOM) == 9 && LEVEL_TYPE2() == 8)
        seg7_033e7a();
    return 0;
}

/* 000218ce: mirror the input horizontally */
uint32_t FUN_000218ce(void) {
    W16(gJoyX, (int16_t)-R16(gJoyX));
    int16_t t = R16(gInLeft);
    W16(gInLeft, R16(gInRight));
    W16(gInRight, t);
    return 0;
}

/* 000218ec: mirror the input vertically */
uint32_t FUN_000218ec(void) {
    W16(gJoyY, (int16_t)-R16(gJoyY));
    int16_t t = R16(gInUp);
    W16(gInUp, R16(gInDown));
    W16(gInDown, t);
    return 0;
}

/* 0002190a: clear the direction inputs */
uint32_t seg4_02190a(void) {
    W16(gInDown, 0);
    W16(gInUp, 0);
    W16(gInLeft, 0);
    W16(gInRight, 0);
    return 1;
}

/* 00021920: save the input state */
uint32_t FUN_00021920(void) {
    mcopy(gInSaved, gInLeft, 10);
    return 0;
}

/* 00021930: restore the input state */
uint32_t FUN_00021930(void) {
    mcopy(gInLeft, gInSaved, 10);
    return 0;
}

/* 00021940: clear the saved input state */
uint32_t seg4_021940(void) {
    return tc_memset(gInSaved, 0, 10);
}

/* 00021954: turn the joystick state into edge-triggered inputs (-1 = newly pressed) */
uint32_t FUN_00021954(void) {
    int16_t v = R16(gInLeft);
    if (v >= 0) {
        if (R16(gJoyX) == -1) { if (v == 0) W16(gInLeft, -1); }
        else W16(gInLeft, 0);
    }
    v = R16(gInRight);
    if (v >= 0) {
        if (R16(gJoyX) == 1) { if (v == 0) W16(gInRight, -1); }
        else W16(gInRight, 0);
    }
    v = R16(gInUp);
    if (v >= 0) {
        if (R16(gJoyY) == -1) { if (v == 0) W16(gInUp, -1); }
        else W16(gInUp, 0);
    }
    v = R16(gInDown);
    if (v >= 0) {
        if (R16(gJoyY) == 1) { if (v == 0) W16(gInDown, -1); }
        else W16(gInDown, 0);
    }
    if (R16(gJoyBtn) == -1) {
        if (R16(gInBtn) != 1) W16(gInBtn, -1);
    } else if (R16(gJoyBtn) == -2) {
        if (R16(gInBtn) != 2) W16(gInBtn, -2);
    } else {
        W16(gInBtn, 0);
    }
    return 0;
}

/* 00021a14: can the character hang on the ledge formed by `above` (cell above) and `front`? */
uint32_t IsLedge(int16_t above, int16_t front, int16_t info, int16_t info2) {
    int16_t r;
    if (S16(IsSpace(above)) == 0 || (LEVEL_TYPE() == 2 && (info & 0x80) != 0)) {
        r = 0;
    } else if ((front == 0xb || front == 0xf) && (info2 & 0xf) != 0) {
        r = 0;
    } else if (S16(IsFloor(front)) == 0) {
        r = 0;
    } else if (front == 4 && (info2 & 0xff) == 0 && R16(CH_FACE) == -1) {
        r = 0;
    } else if (LEVEL_TYPE() == 5 && R16(CH_ROOM) == 0xf && R16(CH_ROW) != 0) {
        r = 0;
    } else {
        r = 1;
    }
    return (uint16_t)r;
}

/* 00021ac8: read the cell above the character */
uint32_t FUN_00021ac8(void) {
    int16_t row = (int16_t)(R16(CH_ROW) - 1);
    return ReadCell(R16(CH_ROOM), R16(CH_COL), row);
}

/* 00021ae0: read the cell above and next to the character (gDirDX2 step) */
uint32_t seg4_021ae0(void) {
    int16_t col = R16(CH_COL);
    col = (int16_t)(col + R16(gDirDX2 + (int32_t)R16(CH_FACE) * 2));
    int16_t row = (int16_t)(R16(CH_ROW) - 1);
    return ReadCell(R16(CH_ROOM), col, row);
}

/* 00021b0e: read the cell above and in front of the character */
uint32_t seg4_021b0e(void) {
    int16_t col = R16(CH_COL);
    col = (int16_t)(col + R16(gDirDX + (int32_t)R16(CH_FACE) * 2));
    int16_t row = (int16_t)(R16(CH_ROW) - 1);
    return ReadCell(R16(CH_ROOM), col, row);
}

#define gCellIdx        GH(0x104b54)   /* word: index of the last cell read inside its room */
#define gCellRoom       GH(0x104b56)   /* word: room of the last cell read */
#define gCellCol        GH(0x104b58)   /* word: column of the last cell read (0..9, relative) */
#define gCellRow        GH(0x104b5a)   /* word: row of the last cell read */

/* 00021b3c: remove the object in the last cell read (replace it by an empty cell) */
uint32_t RemoveObj(int16_t what) {
    SG16(-0x346e, what);
    W16(gInBtn, 1);
    uint16_t v;
    if (LEVEL_TYPE() == 3) v = RU16(gCellInfo) & 0xf00;
    else if (LEVEL_TYPE() == 4) v = RU16(gCellInfo) & 0xf00;
    else v = 0;
    if (G32(-0x4c9e) != 0) v |= 0xc000;
    int32_t idx = R16(gCellIdx);
    uint32_t room = RU16(gCellRoom);
    W16(R32(gGameState) + room * 0x3c - 0x3c + idx * 2, 1);
    idx = R16(gCellIdx);
    room = RU16(gCellRoom);
    W16(R32(gGameState) + 0x780 + room * 0x78 - 0x78 + idx * 4, v);
    FUN_00025ab6();
    return 0;
}

#define gCharClip       GH(0x104b44)   /* Rect: clip rectangle of the character being drawn */
#define gScreenRect     GH(0x106694)   /* Rect: the whole play area */

/* 00021c16: compute the clip rectangle for drawing the current character */
uint32_t ClipChar(void) {
    MakeFCharClipRect();
    if (S16(IsCharNonViewable()) != 0) {
        mcopy(gCharClip, gScreenRect, 8);
        return 0;
    }
    if (R16(CH_TYPE) == 0) {
        uint16_t fr = RU16(CH_FRAME);
        if (fr >= 0xd9 && fr <= 0xe2 &&
            (int32_t)R16(CH_ROW) * 0x78 + 0x52 > (int32_t)R16(CH_Y)) {
            int16_t cell = S16(ReadCell(R16(CH_ROOM), R16(CH_COL), R16(CH_ROW)));
            int16_t row = R16(CH_ROW);
            int16_t col = (int16_t)((cell == 0x10 ? 1 : 0) + R16(CH_COL));
            uint32_t tbl = R32(GH(0x103ad4) + (int16_t)(RU16(R32(gGameState) + 0x2186) * 4));
            FRAME(f, 0xc);
            maddr r = f + 4;                         /* Rect at A6-8 */
            if (ComputeOffsetCellRect(r, tbl, col, row) != 0) {
                W16(gCharClip, R16(r));
                SG16(-0x34b6, R16(r + 6));
            }
            FRAME_END(f);
            return 0;
        }
    }
    if (R16(CH_ACTION) == 9) {
        if (LEVEL_TYPE() == 5) seg23_06e646();
        return 0;
    }
    int16_t ok = 1;
    int16_t t = R16(CH_TYPE);
    int16_t flying = (t == 7 || t == 8) ? 1 : 0;
    int go = 1;
    if (R16(CH_FACE) == 0) {
        uint16_t fr = RU16(CH_FRAME);
        if (fr >= 0x57 && fr <= 0x63 && fr != 0x88) go = 0;
    }
    if (go) {
        int16_t cell = S16(ReadCell(R16(CH_ROOM), G16(-0x34a4), G16(-0x34a0)));
        int clear = 0;
        if (S16(IsSpace(cell)) != 0) {
            clear = 1;
        } else {
            int16_t c = G16(-0x34aa);
            if (c == 1 && LEVEL_TYPE2() == 0xe) clear = 1;
            else if ((c == 7 || c == 8) && LEVEL_TYPE() == 6 && R16(CH_SEQFRAME) != 0xe) clear = 1;
        }
        if (clear) {
            ok = 0;
        } else if (!(R16(CH_ACTION) == 0 && (R16(CH_FRAME) == 0x4f || R16(CH_FRAME) == 0x51))) {
            cell = S16(ReadCell(R16(CH_ROOM), G16(-0x34a2), G16(-0x34a0)));
            if (S16(IsSpace(cell)) != 0) ok = 0;
        }
    }
    if (ok != 0) {
        uint32_t gs = R32(gGameState);
        int16_t lt = R16(gs + 0x2186);
        int16_t room;
        if (lt == 5 || lt == 1) goto level;
        room = R16(CH_ROOM);
        if (room == 0x10 && R16(gs + 0x218a) == 9) goto level;
        if (room == 0x1b && R16(gs + 0x218a) == 6) goto level;
        if (room == 3 && lt == 6) goto level;
        if (room == 6 && lt == 6) goto level;
        if (room == 1 && R16(gs + 0x218a) == 0xe) goto level;
        {
            int16_t r1 = (int16_t)(R16(CH_ROW) + 1);
            if (r1 != 1) {
                int16_t top = (int16_t)(r1 * 0x78 - 0x73);
                if (top >= G16(-0x34c2)) ok = 0;
                else if ((int32_t)top - 0x1c >= (int32_t)G16(-0x356c)) ok = 0;
            }
            if (ok != 0 && flying == 0 && (int32_t)r1 * 0x78 - 0x73 > (int32_t)R16(gCharClip)) {
                int16_t top = (int16_t)(r1 * 0x78 - 0x73);
                SG16(-0x356c, top);
                W16(gCharClip, top);
            }
        }
    }
level:
    switch (RU16(R32(gGameState) + 0x2186)) {
    case 4: seg7_033b26(); break;
    case 5: seg23_06e646(); break;
    case 1: seg18_055506(); break;
    case 6: seg9_03814e(); break;
    }
    return 0;
}

/* Pascal-convention call through the pointer at A5-0x1f58 (SectRect-like:
 * (src1, src2, dst): Boolean). Pushes a result slot and the three rects, the callee pops
 * the arguments; returns the Boolean result byte. */
static uint8_t sect_rect_ptr(maddr a, maddr b, maddr dst) {
    maddr sp0 = emu_sp();
    maddr sp = sp0 - 2 - 12;
    wr32(sp + 8, a);
    wr32(sp + 4, b);
    wr32(sp, dst);
    emu_set_sp(sp);
    call68_addr(G32(-0x1f58));
    uint8_t r = rd8(sp0 - 2);
    emu_set_sp(sp0);
    return r;
}

/* 00021f02: clip rectangle from the size of the character's current image */
uint32_t MakeFCharClipRect(void) {
    int16_t img = G16(-0x34c0);
    if (img == -1) {
        mcopy(gCharClip, gScreenRect, 8);
        return 0;
    }
    uint32_t set = R32(GH(0x103568) + (int16_t)(RU16(GH(0x104b42)) * 4));
    FRAME(f, 8);                                 /* f+0: height, f+2: width */
    uint32_t r;
    if (set == G32(-0x4a88)) r = GetBGSetImgSize(img, f);
    else r = PrinceGetnShapeItemSize(set, (int16_t)(img + 1), f);
    if (r == 0) {
        mcopy(gCharClip, gScreenRect, 8);
        FRAME_END(f);
        return 0;
    }
    SG16(-0x34bc, G16(-0x34c2) - R16(f));
    SG16(-0x34b8, G16(-0x34c2) + 1);
    if (G16(-0x34c6) == 0) {
        SG16(-0x34b6, G16(-0x34c4));
        SG16(-0x34ba, G16(-0x34c4) - R16(f + 2));
    } else {
        SG16(-0x34ba, G16(-0x34c4));
        SG16(-0x34b6, G16(-0x34c4) + R16(f + 2));
    }
    sect_rect_ptr(GA(-0x4fec), gCharClip, gCharClip);
    FRAME_END(f);
    return 0;
}

/* 00021fe8: snap the character onto the floor when it stands in a cell of kind 6 */
uint32_t FUN_00021fe8(void) {
    int16_t t = R16(CH_TYPE);
    if (t == 0xb || t == 7 || t == 8) return 0;
    if (S16(FUN_00021076()) != 6) return 0;
    if ((R16(gFrameFlags) & 0x40) == 0) return 0;
    W16(CH_Y, (int16_t)(R16(CH_ROW) * 0x78 + 0x6b));
    return 0;
}

/* 00022028: compute the drawing index (cell order) of the character */
uint32_t IndexChar(void) {
    int16_t row, col;
    if (R16(CH_ACTION) == 1) { row = G16(-0x349e); col = G16(-0x34a4); }
    else { row = R16(CH_ROW); col = R16(CH_COL); }
    uint16_t fr = RU16(CH_FRAME);
    int adj = (fr >= 0x87 && fr < 0x95);
    if (!adj) {
        int16_t a = R16(CH_ACTION);
        adj = (a == 2 || a == 3 || a == 4 || a == 6);
    }
    if (adj) col = (int16_t)(col + R16(gDirDX2 + (int32_t)R16(CH_FACE) * 2));
    uint32_t r = IndexCell1(col, row);
    SG16(-0x34c8, r);
    return r;
}

#define gObjEffect      GH(0x104b92)   /* word: pending effect of a picked-up object (RemoveObj) */

/* 000220ac: apply the effect of the object (potion...) the kid just picked up */
uint32_t FUN_000220ac(void) {
    if (R16(gObjEffect) == 0) return 0;
    if (R16(CH_TYPE) != 0) {
        if (R16(CH_TYPE) != 1) return 0;
        if ((int32_t)R16(gObjEffect) - 1 != 0) return 0;
    }
    if (R16(gObjEffect) == -1) {
        W16(GH(0x102fc4), 0);
    } else {
        W16(gObjEffect, R16(gObjEffect) - 1);
        int16_t e = R16(gObjEffect);
        switch (e) {
        case 0:
            if (R16(CH_LIFE) != R16(GH(0x102fca))) W16(CH_LIFEDELTA, 1);
            AddSong(0x18);
            break;
        case 1:
            FUN_00015828();
            AddSong(0x18);
            break;
        case 2:
            seg2_015e56();
            break;
        case 3:
            FUN_00015e26();
            break;
        case 4:
            if (G16(-0x4fa8) != 0) {
                FUN_00015e26();
            } else {
                if (S16(DecLifePts(1)) != 0) {
                    JumpSeq(0x47);
                    SetKidDeathMethod(3);
                }
                AddSound(0xc);
            }
            break;
        case 5:
            AddSound(0xc);
            JumpSeq(0x6f);
            break;
        }
    }
    W16(gObjEffect, 0);
    return 0;
}

/* 0002219e: is the character in a dead or resting position (frame-wise)? */
uint32_t FUN_0002219e(void) {
    int16_t r;
    if (S16(IsDeadPos(R16(CH_FRAME))) != 0) {
        r = 1;
    } else {
        int16_t f = R16(CH_FRAME);
        r = (f == 0 || R16(CH_FRAME) == 0xf || R16(CH_FRAME) == 0x6d || R16(CH_FRAME) == 0xa6 ||
             R16(CH_FRAME) == 0xab || R16(CH_FRAME) == 0xe5) ? 1 : 0;
    }
    return (uint16_t)r;
}

/* 000221ee: is the character currently not to be drawn? */
uint32_t IsCharNonViewable(void) {
    int16_t f = R16(CH_FIELD34);
    if (f == 4 || f == 5 || f == 6 || f == 7 || f == 10 || f == 0xc) return 1;
    if (LEVEL_TYPE() == 5) {
        int16_t room = R16(CH_ROOM);
        if (room != 0x13 && room != 0x10 && room != 0xf && S16(IsDeadPos(R16(CH_FRAME))) != 0) return 1;
    }
    return 0;
}

/* 00022270: is `frame` a dead position for the current character type? */
uint32_t IsDeadPos(int16_t frame) {
    int16_t r;
    if (frame == 0xb9) return 1;
    switch (R16(CH_TYPE)) {
    case 0:
        r = (frame == 0xf2 || frame == 0xf3 || frame == 0xb9 || frame == 0x10f || frame == 0x10a) ? 1 : 0;
        if (r != 0) break;
        if (LEVEL_TYPE2() != 5) break;
        if (R16(CH_ROOM) != 3) break;
        r = (R16(CH_ALIVE) >= 0 && R16(CH_FIELD34) == 10) ? 1 : 0;
        break;
    case 2:
        if (R16(R32(gGameState) + 0x21a4) == 0) r = (frame == 0xe4) ? 1 : 0;
        else r = (frame == 0x100 || frame == 0xf8 || frame == 0xfb || frame == 0xfc || frame == 0xfd ||
                  frame == 0xfe || frame == 0xff) ? 1 : 0;
        break;
    case 10:
        r = (frame == 0xd5) ? 1 : 0;
        break;
    case 4:
        r = (frame == 0xce || frame == 0xcf) ? 1 : 0;
        break;
    case 0xb:
        r = (frame == 0x111) ? 1 : 0;
        break;
    default:
        r = 0;
        break;
    }
    return (uint16_t)r;
}

/* 000223c8: remove the current character from play */
uint32_t FUN_000223c8(void) {
    if (R16(CH_ROOM) == 6 && LEVEL_TYPE() == 6 && R16(CH_SEQFRAME) == 0xf0 && R16(GH(0x102f4a)) < 0)
        FUN_000395aa();
    FUN_00022402();
    W16(CH_ROOM, 0);
    return 0;
}

/* 00022402: reset the current character (dead, no life), pick a new opponent if needed */
uint32_t FUN_00022402(void) {
    DrawOppMeter(R16(CH_ID), 0);
    W16(CH_FACE, 0x56);
    W16(CH_ACTION, 0);
    W16(CH_ALIVE, 0);
    W16(CH_LIFE, 0);
    if (R16(CH_ID) != 0 && R16(gOppIdx) == R16(CH_ID))
        W16(gOppIdx, GetOppIdxClosestToKid(1));
    return 0;
}

#define gDrawInfo       GH(0x104b38)   /* 24 bytes: drawing info of the character (index, clip...) */
#define gDrawInfoSave   GH(0x104b20)   /* 24 bytes: saved copy */

/* 00022442: save the character drawing info */
uint32_t seg4_022442(void) {
    mcopy(gDrawInfoSave, gDrawInfo, 24);
    return 0;
}

/* 00022454: restore the character drawing info */
uint32_t seg4_022454(void) {
    mcopy(gDrawInfo, gDrawInfoSave, 24);
    return 0;
}

/* 00022466: en-garde from the crouch (frame 0xa6) */
uint32_t seg4_022466(void) {
    if (R16(CH_FRAME) != 0xa6) return 0;
    if (R16(gInDown) >= 0) return 0;
    if (R16(gInLeft) < 0) {
        int16_t cell = S16(FUN_00021518());
        if (S16(IsAdvancableId(cell, G16(-0x34ae))) == 0) W16(CH_X, AddCharX(8));
        DoEngarde();
    } else {
        W16(gInDown, 1);
        JumpSeq(0x50);
    }
    return 0;
}

#define SV_FACE         GH(0x102f6e)   /* gSaved fields */
#define SV_X            GH(0x102f70)
#define SV_ROW          GH(0x102f7a)
#define SV_ROOM         GH(0x102f82)
#define SV_LIFE         GH(0x102f8a)

/* 000224be: distance to the opponent (gSaved) in facing direction; 999 if not reachable */
uint32_t seg4_0224be(void) {
    int16_t d;
    if (R16(SV_LIFE) == 0 || R16(CH_ROOM) != R16(SV_ROOM) || R16(CH_ROW) != R16(SV_ROW) || R16(SV_FACE) == 0x56) {
        d = 999;
    } else {
        d = (int16_t)(R16(SV_X) - R16(CH_X));
        if (R16(CH_FACE) != 0) d = (int16_t)-d;
        if (d >= 0 && R16(CH_FACE) != R16(SV_FACE)) d = (int16_t)(d + 0x15);
    }
    return (uint16_t)d;
}

/* 00022516: SaveShad: store the current character into gCharTab[id] */
uint32_t seg4_022516(void) {
    int16_t id = R16(CH_ID);
    if (id < 0 || id >= 5) {
        /* "Error in SaveShad(). Quit?" lives in the code segment right after this function */
        if (S16(AlertYesNo(fn_address(FN_seg4_022516) + 0x5e)) != 0) Terminate(1, 0);
        return 0;
    }
    mcopy(gCharTab + (int32_t)R16(CH_ID) * CHAR_SIZE, gChar, CHAR_SIZE);
    return 0;
}

/* 00022590: LoadShad: load gCharTab[id] into the current character */
uint32_t LoadShad(int16_t id) {
    mcopy(gChar, gCharTab + (int32_t)id * CHAR_SIZE, CHAR_SIZE);
    return 0;
}

/* 000225d2: restore the current character from gOpp */
uint32_t FUN_000225d2(void) {
    mcopy(gChar, gOpp, CHAR_SIZE);
    return 0;
}

/* 000225e6: keep the character out of walls (front and back) and below the floor above */
uint32_t FUN_000225e6(void) {
    int16_t d;
    if (S16(IsWall(S16(GetCellsInFront(1)))) == 0) {
        if (S16(IsSpace(S16(FUN_00021ac8()))) == 0) goto front;
        if (S16(IsSpace(S16(seg4_021b0e()))) != 0) goto back;
    }
front:
    d = S16(seg4_021562());
    d = (int16_t)(d + S16(FUN_0002393e()));
    if (d <= 0x11) W16(CH_X, AddCharX((int16_t)(d - 0x14)));
back:
    if (S16(IsWall(S16(FUN_00021518()))) == 0) {
        if (S16(IsSpace(S16(FUN_00021ac8()))) == 0) goto back2;
        if (S16(IsSpace(S16(seg4_021ae0()))) != 0) goto top;
    }
back2:
    d = S16(seg4_021562());
    d = (int16_t)(d + S16(FUN_0002393e()));
    if (d >= 0x22) W16(CH_X, AddCharX((int16_t)(0x14 - (int16_t)(0x33 - d))));
top:
    if (S16(IsSpace(S16(FUN_00021ac8()))) != 0 && S16(IsSpace(S16(seg4_021b0e()))) != 0) return 0;
    if ((int32_t)R16(CH_ROW) * 0x78 > (int32_t)R16(CH_Y)) W16(CH_Y, (int16_t)(R16(CH_ROW) * 0x78));
    return 0;
}

/* 000226ee: start falling: choose the fall sequence from the current frame */
uint32_t FUN_000226ee(void) {
    if (R16(CH_TYPE) == 6 && R16(CH_SEQFRAME) == 0xe6) return 0;
    uint16_t fr = RU16(CH_FRAME);
    int16_t seq = -1;
    W16(CH_ROW, R16(CH_ROW) + 1);
    if (R16(CH_TYPE) == 1) seg4_01fb80();
    if (R16(CH_TYPE) == 0 && LEVEL_TYPE2() == 5 && R16(CH_ROOM) == 10) {
        if ((R16(CH_FACE) == 0 && R16(gInLeft) != 0) || (R16(CH_FACE) == -1 && R16(gInRight) != 0)) {
            seq = 0xc6;
        } else if (G16(-0x36a2) == 3 && G16(-0x36a0) >= 0x14 && R16(GH(0x102fc4)) == 1 && R16(CH_FACE) == 0) {
            seq = 0xc6;
        }
    }
    if (R16(GH(0x102fc4)) == 1) W16(GH(0x102fc4), 0);
    if (seq == -1) {
        switch (fr) {
        case 9:    seq = 7; break;
        case 0xd:  seq = 0x13; break;
        case 0x1a: seq = 0x12; break;
        case 0x2c: seq = 0x15; break;
        case 0x51: seq = 7; W16(CH_Y, R16(CH_Y) + 0x18); break;
        default:
            if (fr >= 0x51 && fr < 0x56) {
                seq = 0x13;
            } else if (fr >= 0x96 && fr < 0xb4) {
                if (R16(CH_TYPE) == 0) {
                    SG16(-0x4f9c, 1);
                    if (fr == 0x99) seq = 0x5f;
                    else {
                        int16_t s = R16(CH_SEQFRAME);
                        seq = (s == 0x38 || s == 0x5e) ? 0x5f : 0x51;
                    }
                } else {
                    SG16(-0x4f9c, 0);
                    int16_t s = R16(CH_SEQFRAME);
                    seq = (s == 0x56 || s == 0x43 || s == 0x6c || s == 0x5e) ? 0x53 : 0x52;
                }
            } else if (R16(CH_TYPE) == 2 && R16(CH_SEQFRAME) == 100) {
                seq = 0xba;
            } else {
                seq = 7;
            }
            break;
        }
    }
    FUN_000225e6();
    if (LEVEL_TYPE() == 1) {
        seq = 0x1b;
        FUN_000561e6();
        SetKidDeathMethod(8);
    } else if (R16(CH_TYPE) == 0 && G16(-0x4fa6) != 0 && R16(CH_SEQFRAME) != 0xe4) {
        seq = 0xe4;
    }
    JumpSeq(seq);
    AnimChar();
    FUN_00020542();
    int16_t cell = S16(FUN_00021076());
    if (S16(IsWall(cell)) != 0) {
        seg4_021452();
        return 0;
    }
    cell = S16(GetCellsInFront(1));
    if (S16(IsWall(cell)) == 0) return 0;
    if (fr != 0x2c) return 0;
    if (S16(seg4_02397e()) >= 0x16) return 0;
    JumpSeq(0x72);
    AnimChar();
    FUN_00020542();
    return 0;
}

/* ---- collision detection ("barriers") ----
 * For the rows row-1, row, row+1 GetColDetData fills a room per column (word[10]) and a
 * barrier code per column (word[10]); the previous frame's row is kept by InitColDetBufs. */
#define gCDRow          GH(0x104a84)   /* word: row being checked */
#define gCDPrevRow      GH(0x104a82)   /* word */
#define gCDColTo        GH(0x104a7e)   /* word: last column to check */
#define gCDColFrom      GH(0x104a80)   /* word: first column to check */
#define gCDCellX        GH(0x104a04)   /* word: x of the column being checked */
#define gCDHitLeft      GH(0x104a86)   /* word: column where a left-side barrier was entered (-1 none) */
#define gCDHitRight     GH(0x104a88)   /* word: column where a right-side barrier was entered (-1 none) */
#define gCharRightX     GH(0x104a9a)   /* word: right edge of the character image */
#define gCharLeftX      GH(0x104a9c)   /* word: left edge of the character image */
#define gCDRooms        GH(0x104a56)   /* word[10]: room of each column, this row */
#define gCDPrevRooms    GH(0x104a6a)   /* word[10]: previous frame */
#define gCDRoomsBelow   GH(0x104a42)   /* word[10]: row + 1 */
#define gCDRoomsAbove   GH(0x104a2e)   /* word[10]: row - 1 */
#define gCDCodes        GH(0x104ab6)   /* word[10]: barrier code of each column, this row */
#define gCDPrevCodes    GH(0x104aa2)   /* word[10]: previous frame */
#define gCDCodesBelow   GH(0x104a1a)   /* word[10]: row + 1 */
#define gCDCodesAbove   GH(0x104a06)   /* word[10]: row - 1 */

/* the column range around the character and the three rows of barrier data */
static void cd_setup_cols(void) {
    int16_t c4;
    if ((int32_t)S16(GetCharCol(R16(gCharRightX))) + 4 > 0xb) c4 = 0xb;
    else c4 = (int16_t)(S16(GetCharCol(R16(gCharRightX))) + 4);
    W16(gCDColTo, c4);
    W16(gCDColFrom, (int16_t)(S16(GetCharCol(R16(gCharLeftX))) - 2));
}
static void cd_slide_rocks(void) {
    GetSlideRockDetData(R16(gCDRow), gCDRooms, gCDCodes);
    GetSlideRockDetData((int16_t)(R16(gCDRow) + 1), gCDRoomsBelow, gCDCodesBelow);
    GetSlideRockDetData((int16_t)(R16(gCDRow) - 1), gCDRoomsAbove, gCDCodesAbove);
}

/* 00022904: gather the barriers around the character and detect newly entered ones */
uint32_t CheckBarr(void) {
    W16(gCDHitLeft, -1);
    W16(gCDHitRight, -1);
    int full = 1;
    if (R16(CH_ACTION) == 7) full = 0;
    else {
        int16_t s = R16(CH_SEQFRAME);
        if (s == 0x4e || s == 0x46) full = 0;
        else if (R8(R32(gGameState) + 0x2191) != 0 && GetSlideRockOnRoomRow(R16(CH_ROOM), R16(CH_ROW)) != 0 &&
                 R16(CH_FIELD34) == 4) full = 0;
    }
    if (full) {
        W16(gCDRow, R16(CH_ROW));
        InitColDetBufs();
        W16(gCDPrevRow, R16(gCDRow));
        cd_setup_cols();
        GetColDetData(R16(gCDRow), gCDRooms, gCDCodes);
        GetColDetData((int16_t)(R16(gCDRow) + 1), gCDRoomsBelow, gCDCodesBelow);
        GetColDetData((int16_t)(R16(gCDRow) - 1), gCDRoomsAbove, gCDCodesAbove);
        if (R8(R32(gGameState) + 0x2191) != 0) cd_slide_rocks();
        maddr rooms = gCDRooms + 18, prooms = gCDPrevRooms + 18, codes = gCDCodes + 18, pcodes = gCDPrevCodes + 18;
        for (int16_t col = 9; col >= 0; col--) {
            int16_t r = R16(rooms);
            if (r != -1 && R16(prooms) == r) {
                if ((R16(pcodes) & 0xf) == 0 && (R16(codes) & 0xf) != 0) W16(gCDHitRight, col);
                if ((R16(pcodes) & 0xf0) == 0 && (R16(codes) & 0xf0) != 0) W16(gCDHitLeft, col);
            }
            rooms -= 2; prooms -= 2; codes -= 2; pcodes -= 2;
        }
    } else if (R16(CH_ACTION) == 7 && R8(R32(gGameState) + 0x2191) != 0 &&
               GetSlideRockOnRoomRow(R16(CH_ROOM), R16(CH_ROW)) != 0 && R16(CH_FIELD34) != 4) {
        W16(gCDRow, R16(CH_ROW));
        InitColDetBufs();
        W16(gCDPrevRow, R16(gCDRow));
        cd_setup_cols();
        if (R8(R32(gGameState) + 0x2191) != 0) cd_slide_rocks();
    }
    if (R16(CH_TYPE) == 0 && R16(gCDHitRight) == -1 && R16(gCDHitLeft) == -1 && G16(-0x4fa6) != 0 &&
        R16(CH_ACTION) == 4)
        FUN_00022baa();
    return 0;
}

#define gCharImgH       GH(0x104aa0)   /* word: height of the character image (GetCharEdges) */

/* 00022baa: bumping the head on the floor above while jumping up: step back */
uint32_t FUN_00022baa(void) {
    int16_t top = (int16_t)(R16(CH_Y) - R16(gCharImgH));
    if ((int32_t)R16(CH_ROW) * 0x78 - 8 > (int32_t)top) {
        int16_t cell = S16(ReadCell(R16(CH_ROOM), R16(CH_COL), (int16_t)(R16(CH_ROW) - 1)));
        if (S16(IsSpace(cell)) == 0) {
            W16(CH_X, AddCharX(-6));
            W16(CH_XVEL, 0);
            JumpSeq(0xe5);
        }
    }
    return 0;
}

/* 00022c22: keep last frame's barrier data (re-aligned when the row changed) and reset the buffers */
uint32_t InitColDetBufs(void) {
    maddr rooms, codes;
    int16_t row = R16(gCDRow), prev = R16(gCDPrevRow);
    if (row == prev || (int32_t)row + 3 == (int32_t)prev || (int32_t)row - 3 == (int32_t)prev) {
        rooms = gCDRooms; codes = gCDCodes;
    } else {
        row = R16(gCDRow); prev = R16(gCDPrevRow);
        if ((int32_t)row + 1 == (int32_t)prev || (int32_t)row - 2 == (int32_t)prev) {
            rooms = gCDRoomsAbove; codes = gCDCodesAbove;
        } else {
            rooms = gCDRoomsBelow; codes = gCDCodesBelow;
            int16_t col = R16(CH_COL);
            if (col >= 0 && col < 10) {
                maddr c = gCDCodesBelow + (int32_t)R16(CH_COL) * 2;
                if (R16(CH_FACE) == 0) {
                    if (R16(CH_XVEL) > 0) W16(c, RU16(c) & 0xf0);
                    else if (R16(CH_XVEL) < 0) W16(c, RU16(c) & 0xf);
                } else {
                    if (R16(CH_XVEL) < 0) W16(c, RU16(c) & 0xf0);
                    else if (R16(CH_XVEL) > 0) W16(c, RU16(c) & 0xf);
                }
            }
        }
    }
    tc_memcpy(gCDPrevRooms, rooms, 0x14);
    tc_memcpy(gCDPrevCodes, codes, 0x14);
    tc_memset(gCDRooms, 0xffff, 0x14);
    tc_memset(gCDRoomsAbove, 0xffff, 0x14);
    return tc_memset(gCDRoomsBelow, 0xffff, 0x14);
}

/* 00022d8c: barrier code of a cell kind (0 = no barrier) */
uint32_t GetBarrCode(int16_t cell) {
    int16_t r;
    switch ((uint16_t)cell) {
    case 2:
        if ((RU16(gCellInfo) & 0xff) < 3 && G16(-0x36bc) != 0) r = 3;
        else r = 4;
        break;
    case 0x19:
    case 0x14:
        r = 4;
        break;
    case 7:
        r = (R16(gCharImgH) > 0x22) ? 4 : 2;
        break;
    case 4:
        if (LEVEL_TYPE() == 3) r = 5;
        else if (R16(gCurRoom) == 9 && LEVEL_TYPE2() == 8) r = 8;
        else r = 1;
        break;
    case 0xc:
        r = 6;
        break;
    default:
        r = 0;
        break;
    }
    return (uint16_t)r;
}

#define gBarrLeftOfs    GH(0x1049e0)   /* word[]: left offset of each barrier code */
#define gBarrRightOfs   GH(0x1049f2)   /* word[]: right offset of each barrier code */

/* 00022e3a: barrier flags (0x0f: left of the char's left edge, 0xf0: right of its right edge)
 * and rooms for the columns gCDColFrom..gCDColTo of a row */
uint32_t GetColDetData(int16_t row, uint32_t rooms, uint32_t codes) {
    int16_t col = R16(gCDColFrom);
    W16(gCDCellX, (int16_t)(R16(GH(0x103478) + (int32_t)col * 2) + 0x16));
    if (col > R16(gCDColTo)) return 0;
    do {
        int16_t l = S16(GetLeftBarr(R16(CH_ROOM), col, row));
        int16_t r = S16(GetRightBarr(R16(CH_ROOM), col, row));
        uint16_t f = (l < R16(gCharRightX)) ? 0xf : 0;
        if (r > R16(gCharLeftX)) f |= 0xf0;
        W16(codes + (int32_t)R16(gCellCol) * 2, f);
        W16(rooms + (int32_t)R16(gCellCol) * 2, RU16(gCellRoom));
        W16(gCDCellX, R16(gCDCellX) + 0x33);
        col = (int16_t)(col + 1);
    } while (col <= R16(gCDColTo));
    return 0;
}

/* 00022eea: x of the left side of the barrier in a cell (gCDCellX based) */
uint32_t GetLeftBarr(int16_t room, int16_t col, int16_t row) {
    int16_t cell = S16(ReadCell(room, col, row));
    int16_t code = S16(GetBarrCode(cell));
    if (code != 0) return (uint32_t)((int32_t)R16(gCDCellX) + (int32_t)R16(gBarrLeftOfs + (int32_t)code * 2));
    return (uint32_t)(int32_t)R16(GH(0x103494));
}

/* 00022f4c: x of the right side of the barrier in a cell (0 if none) */
uint32_t GetRightBarr(int16_t room, int16_t col, int16_t row) {
    int16_t cell = S16(ReadCell(room, col, row));
    int16_t code = S16(GetBarrCode(cell));
    int16_t r;
    if (code != 0) r = (int16_t)(R16(gCDCellX) + 0x32 - R16(gBarrRightOfs + (int32_t)code * 2));
    else r = 0;
    return (uint16_t)r;
}

/* 00022faa: react to a barrier entered this frame */
uint32_t FUN_00022faa(void) {
    int16_t a = R16(CH_ACTION);
    if (a == 2 || a == 6) return 0;
    uint16_t fr = RU16(CH_FRAME);
    if (fr >= 0x87 && fr < 0x91) return 0;
    if (R16(gCDHitRight) >= 0) FUN_00023056();
    else if (R16(gCDHitLeft) >= 0) FUN_00022fea();
    return 0;
}

/* 00022fea: collision with a barrier on the left side of a column */
uint32_t FUN_00022fea(void) {
    if (R16(GH(0x102fc4)) != 1 && R16(CH_FACE) == 0) return 0;
    if (S16(CheckCollide1(R16(gCDHitLeft))) == 0) return 0;
    int16_t x = S16(GetRightBarr(R16(gCellRoom), R16(gCellCol), R16(gCellRow)));
    if (x <= 0 && R8(R32(gGameState) + 0x2191) != 0)
        x = S16(GetSlideRockBarrDist(0, R16(gCellRoom), R16(gCellRow)));
    x = (int16_t)(x - R16(gCharLeftX));
    Collide(x, 0);
    return 0;
}

/* 00023056: collision with a barrier on the right side of a column */
uint32_t FUN_00023056(void) {
    if (R16(GH(0x102fc4)) != 1 && R16(CH_FACE) == -1) return 0;
    if (S16(CheckCollide1(R16(gCDHitRight))) == 0) return 0;
    int16_t x = S16(GetLeftBarr(R16(gCellRoom), R16(gCellCol), R16(gCellRow)));
    if (x >= R16(GH(0x103494)) && R8(R32(gGameState) + 0x2191) != 0)
        x = S16(GetSlideRockBarrDist(-1, R16(gCellRoom), R16(gCellRow)));
    x = (int16_t)(x - R16(gCharRightX));
    Collide(x, -1);
    return 0;
}

/* 000230c4: does the barrier in column `col` of the current row stop the character? */
uint32_t CheckCollide1(int16_t col) {
    int16_t row = R16(CH_ROW);
    if (R16(CH_ROW) < 0) row = (int16_t)(row + 3);
    if (row >= 3) row = (int16_t)(row - 3);
    int16_t cell = S16(ReadCell(R16(gCDRooms + (int32_t)col * 2), col, row));
    return CheckCollide(cell);
}

/* 00023124: does a barrier of this cell kind stop the character? sets gCDCellX */
uint32_t CheckCollide(int16_t cell) {
    int16_t r;
    switch ((uint16_t)cell) {
    case 7:   r = S16(FUN_000235ae()); break;
    case 10:  r = 0; break;
    case 4:   r = S16(seg4_0235de()); break;
    case 0xc: r = S16(FUN_00023746()); break;
    default:  r = 1; break;
    }
    if (r != 0 && R16(CH_TYPE) == 0xb) r = 0;
    if (r != 0) {
        int16_t x = R16(GH(0x103478) + (int32_t)R16(gCellCol) * 2);
        W16(gCDCellX, (int16_t)(S16(AdjustRoom(R16(gCellRoom), x)) + 0x16));
    }
    return (uint16_t)r;
}

/* 000231b6: x relative to the current room for a cell x in a neighbouring room.
 * The original adjusts its argument slot in place; glue_AdjustRoom reproduces that. */
uint32_t AdjustRoom(int16_t room, int16_t x) {
    if (room != G16(-0x4fc2)) {
        if (room == G16(-0x4fc0) || room == G16(-0x4fb4)) x = (int16_t)(x - 0x1fe);
        else if (room == G16(-0x4fbe) || room == G16(-0x4fb2)) x = (int16_t)(x + 0x1fe);
    }
    return (uint16_t)x;
}
void glue_AdjustRoom(void) {
    maddr sp = emu_sp();
    uint32_t r = AdjustRoom((int16_t)rd16(sp), (int16_t)rd16(sp + 2));
    wr16(sp + 2, (uint16_t)r);
    emu_set_d0(r);
}

/* 00023208: push the character out of a barrier by dx and bump (dir: -1 = left side, 0 = right) */
uint32_t Collide(int16_t dx, int16_t dir) {
    if (R16(CH_ALIVE) >= 0) return 0;
    int16_t fr = R16(CH_FRAME);
    if (fr == 0xf2 || fr == 0xf3) return 0;
    W16(CH_X, R16(CH_X) + dx);
    int16_t cell = R16(gCellKind);
    int16_t col;
    if (dir == -1) {
        if (S16(IsWall(R16(gCellKind))) != 0) {
            if (R16(CH_SEQFRAME) == 0x4b) col = (int16_t)(R16(gCellCol) - 2);
            else col = (int16_t)(R16(gCellCol) - 1);
            cell = S16(ReadCell(R16(gCellRoom), col, R16(gCellRow)));
        }
    } else {
        if (S16(IsWall(R16(gCellKind))) != 0) {
            if (R16(CH_SEQFRAME) == 0x4b) col = (int16_t)(R16(gCellCol) + 2);
            else col = (int16_t)(R16(gCellCol) + 1);
            if (R16(gCellRoom) == 0 && col >= 10) {
                W16(gCellRoom, R16(CH_ROOM));
                col = (int16_t)(col - 10);
            }
            cell = S16(ReadCell(R16(gCellRoom), col, R16(gCellRow)));
        }
    }
    if (S16(IsSpace(cell)) == 0 || (LEVEL_TYPE() == 2 && S16(SlideRockInCell(0xc)) != 0)) GroundBump(dir);
    else FUN_00023338();
    if (cell == 7) {
        uint16_t f = RU16(CH_FRAME);
        if (f >= 0xf6 && f <= 0x105) return 0;
    }
    if (S16(IsWall(cell)) != 0) seg4_021452();
    return 0;
}

/* 00023338: bumped into a barrier while there is empty space: step back and fall */
uint32_t FUN_00023338(void) {
    W16(CH_X, AddCharX(-10));
    if (R16(CH_ACTION) == 4) {
        W16(CH_XVEL, 0);
    } else {
        JumpSeq(0x2d);
        AnimChar();
    }
    FUN_00023540();
    FUN_00020542();
    return 0;
}

/* 0002336e: the character bumped into a barrier (dir: side of the barrier) */
uint32_t GroundBump(int16_t dir) {
    if (R16(GH(0x102fc4)) != 1) {
        int32_t d = (int32_t)R16(CH_ROW) * 0x78 + 0x6a - (int32_t)R16(CH_Y);
        if (d >= 0x1e) {
            FUN_00023338();
            return 0;
        }
    }
    FUN_00020cca();
    if (R16(CH_YVEL) >= 0x32) {
        W16(CH_X, AddCharX(-0x11));
        FUN_00020542();
        return 0;
    }
    W16(CH_YVEL, 0);
    if (R16(CH_ALIVE) == 0) return 0;
    int16_t seq;
    if (R16(GH(0x102fc4)) == 1) {
        int16_t rockx = S16(GetSlideRockX(R16(CH_ROOM), R16(CH_ROW)));
        if (dir == R16(CH_FACE)) {
            if (R16(GH(0x102fd0)) == 0x37 && R16(CH_FRAME) == 0xcf) W16(CH_X, AddCharX(0x11));
            W16(CH_X, AddCharX(6));
            seq = 0x41;
        } else {
            seq = 0x40;
            if (S16(IsStrikingPos(R16(CH_TYPE), R16(CH_FRAME))) != 0) {
                if (R16(gCellKind) == 4 && (RU16(gCellInfo) & 0xff) > 0x3c) seq = -1;
                else W16(CH_X, AddCharX(0x19));
            }
        }
        if (rockx != 0 && R16(CH_X) < rockx) {
            seq = 0x5d;
            W16(GH(0x102fc4), 0);
        }
    } else {
        uint16_t fr = RU16(CH_FRAME);
        if (fr >= 0xf6 && fr <= 0x105) {
            W16(CH_X, AddCharX(-5));
            seq = 0x79;
            W16(CH_FLAG18, 0);
        } else if ((int32_t)R16(CH_LIFE) + (int32_t)R16(CH_LIFEDELTA) <= 0) {
            seq = -1;
        } else {
            fr = RU16(CH_FRAME);
            if (fr == 0x18 || fr == 0x19 || (fr >= 0x28 && fr < 0x2b)) seq = 0x2e;
            else if (fr < 0x66 || fr > 0x6a) seq = 0x2f;
            else seq = 0x2e;
        }
    }
    if (seq != -1) {
        JumpSeq(seq);
        AnimChar();
        FUN_00020542();
    }
    if (seq == -1) return 0;
    if (seq != 0x40) FUN_00023540();
    else if (R16(CH_SEQFRAME) != 0x38) AddSound(10);
    return 0;
}

/* 00023540: flag the bump (not for character type 1) */
uint32_t FUN_00023540(void) {
    if (R16(CH_TYPE) != 1) SG16(-0x4f9e, 1);
    return 0;
}

/* 00023550: reset the collision buffers */
uint32_t seg4_023550(void) {
    tc_memset(gCDPrevRooms, 0xffff, 0x14);
    tc_memset(gCDRooms, 0xffff, 0x14);
    tc_memset(gCDRoomsBelow, 0xffff, 0x14);
    uint32_t r = tc_memset(gCDRoomsAbove, 0xffff, 0x14);
    W16(gCDPrevRow, -1);
    return r;
}

/* 000235ae: does a gate (cell kind 7) stop the character? */
uint32_t FUN_000235ae(void) {
    int16_t t = R16(CH_TYPE);
    if (t == 7 || t == 8 || R16(gCharImgH) > 0x22 || (R16(gCellInfo) & 3) == 3) return 1;
    return 0;
}

/* 000235de: does the barrier of cell kind 4 stop the character? */
uint32_t seg4_0235de(void) {
    int16_t r;
    if (LEVEL_TYPE() == 1) {
        r = ((int16_t)(R16(gCellInfo) & 0x1f) < 0x13) ? 1 : 0;
    } else if (R16(CH_TYPE) == 1 || R16(CH_SEQFRAME) == 0x44) {
        r = 0;
    } else {
        int16_t h = (int16_t)(R16(GH(0x102fd8)) - R16(GH(0x102fd4)));
        if (R16(CH_TYPE) == 7) h = (int16_t)(h + 0x2e);
        else if (R16(CH_TYPE) == 8) h = (int16_t)(h + 0x21);
        int16_t v = (int16_t)((RU16(gCellInfo) & 0xff) >> 2);
        r = ((int32_t)v * 2 + 0xb < (int32_t)h + 0xd) ? 1 : 0;
    }
    return (uint16_t)r;
}

/* 0002367a: is there a gate barrier under/in front of the character that lets it stand up? */
uint32_t GateBarrToStandUp(void) {
    int16_t room = R16(CH_ROOM);
    if (room == 9 && LEVEL_TYPE2() == 8) return 0;
    if (S16(ReadCell(room, R16(CH_COL), R16(CH_ROW))) != 4 && S16(GetCellsInFront(1)) != 4 &&
        S16(FUN_00021518()) != 4)
        return 0;
    uint16_t h = RU16(gCellInfo) & 0xff;
    int16_t x = (int16_t)(S16(AddCharX(-10)) - 0x16);
    if (h == 0 || h >= 0xb8) return 0;
    int32_t i = R16(gCellCol);
    if ((int32_t)R16(GH(0x103478) + i * 2) + 0x13 > (int32_t)x) return 0;
    return ((int32_t)R16(GH(0x10347a) + i * 2) + 6 >= (int32_t)x) ? 1 : 0;
}

/* 00023746: does a (extended) scythe blade stop the character? */
uint32_t FUN_00023746(void) {
    int16_t t = R16(CH_TYPE);
    if (t == 1 || t == 7 || t == 8 || t == 0xb) return 0;
    if (R16(gCharImgH) <= 0x30) return 0;
    return (S16(ScytheBladeExtended(R16(gCellInfo))) != 0) ? 1 : 0;
}

#define gCharImgW       GH(0x104a9e)   /* word: width of the character image (+1) */
#define gCharImgX       GH(0x104a98)   /* word: left x of the character image */
#define gCharImgX2      GH(0x104a96)   /* word: right x of the character image */
#define gCharImgTop     GH(0x104a94)   /* word: top of the character image */
#define gCharRect       GH(0x102fd4)   /* Rect: character rectangle (CH+0x2a) */

/* 0002378c: compute the edges / rectangle / cells covered by the character image */
uint32_t GetCharEdges(void) {
    mcopy(gCharRect, gScreenRect, 8);
    int16_t img = G16(-0x34c0);
    if (img == -1) return 0;
    uint32_t set = R32(GH(0x103568) + (int16_t)(RU16(GH(0x104b42)) * 4));
    FRAME(f, 0x10);                    /* f+0: Rect (A6-0x10), f+8: height, f+0xa: width */
    uint32_t r;
    if (set == G32(-0x4a88)) r = GetBGSetImgSize(img, f + 8);
    else r = PrinceGetnShapeItemSize(set, (int16_t)(img + 1), f + 8);
    if (r == 0) { FRAME_END(f); return 0; }
    W16(gCharImgW, R16(f + 0xa) + 1);
    W16(gCharImgH, R16(f + 8));
    int16_t d = S16(FUN_0002393e());
    W16(gCharImgX, G16(-0x34c4) + 0xcf);
    if (R16(CH_FACE) == 0) W16(gCharImgX, R16(gCharImgX) - R16(gCharImgW));
    else W16(gCharImgX, R16(gCharImgX) - d);
    W16(gCharImgW, R16(gCharImgW) + d);
    int16_t x = R16(gCharImgX);
    W16(gCharLeftX, x);
    x = (int16_t)(x + R16(gCharImgW));
    W16(gCharImgX2, x);
    W16(gCharRightX, x);
    int16_t top = (int16_t)(G16(-0x34c2) - R16(gCharImgH) + 1);
    W16(gCharImgTop, top);
    if (top >= 0x16d) W16(gCharImgTop, 0x16d);
    W16(f + 0, R16(gCharImgTop));
    W16(f + 4, G16(-0x34c2) + 1);
    if (R16(CH_FACE) == -1) {
        x = G16(-0x34c4);
        W16(f + 2, x);
        W16(f + 6, x + R16(f + 0xa));
    } else {
        W16(f + 2, G16(-0x34c4) - R16(f + 0xa));
        W16(f + 6, G16(-0x34c4));
    }
    if (sect_rect_ptr(GA(-0x4fec), f, f) != 0) mcopy(gCharRect, f, 8);
    SG16(-0x34a0, GetRow(R16(gCharImgTop)));
    int16_t row = S16(GetRow(G16(-0x34c2)));
    SG16(-0x349e, row);
    if (row == -1) SG16(-0x349e, 3);
    int16_t c;
    if (S16(GetCol(R16(gCharImgX))) < 0) c = 0;
    else c = S16(GetCol(R16(gCharImgX)));
    SG16(-0x34a4, c);
    if (S16(GetCol(R16(gCharImgX2))) > 9) c = 9;
    else c = S16(GetCol(R16(gCharImgX2)));
    SG16(-0x34a2, c);
    FRAME_END(f);
    return 0;
}

/* 0002393e: extra width while riding a sliding rock */
uint32_t FUN_0002393e(void) {
    int16_t r = 0;
    if (R16(GH(0x102fc4)) == 1) {
        int16_t t = R16(CH_TYPE);
        if (t != 7 && t != 8 && G16(-0x521a) != 0) {
            int32_t v = (int32_t)G16(-0x5216) - 3;
            if (v <= 0) v = 0;
            r = (int16_t)v;
        }
    }
    return (uint16_t)r;
}

#define gBarrSide       GH(0x104a8a)   /* word: 0 = edge, 1 = barrier in front, 2 = none */

/* 0002397e: distance to the barrier or edge in front of the character (0x2a = none) */
uint32_t seg4_02397e(void) {
    int16_t d = 0;
    int16_t cell = S16(FUN_00021076());
    if (S16(GetBarrCode(cell)) != 0) {
        d = S16(GetBarrDistFromFace(cell, R16(CH_ROOM), R16(CH_COL)));
        if (d >= 0) goto barrier;
    }
    if (cell == 0x18) goto c18;
    if (cell == 0x17) goto c17;
    cell = S16(GetCellsInFront(1));
    if (S16(GetBarrCode(cell)) != 0) {
        int16_t col = (int16_t)(R16(CH_COL) + R16(gDirDX + (int32_t)R16(CH_FACE) * 2));
        d = S16(GetBarrDistFromFace(cell, R16(CH_ROOM), col));
        if (d >= 0) goto barrier;
    }
    if ((S16(IsSpace(cell)) != 0 || S16(IsImpassable(cell)) != 0 || cell == 0x1e) && cell != 0x18 && cell != 0x17) {
        W16(gBarrSide, 0);
        d = S16(seg4_021562());
        return (uint16_t)d;
    }
    if (cell == 6 || cell == 0x16 || cell == 2 || cell == 0x22) {
        d = S16(seg4_021562());
        if (d == 0) goto none;
        d = (int16_t)(d - 1);
        W16(gBarrSide, 0);
        return (uint16_t)d;
    }
    if (cell == 0x18) goto c18;
    if (cell == 0x17) goto c17;
none:
    W16(gBarrSide, 2);
    return 0x2a;
c18:
    if (R16(CH_FACE) != -1) goto none;
    d = S16(seg4_021562());
    if (R16(CH_COL) == R16(gCellCol)) d = (int16_t)(d - 0x11);
    else d = (int16_t)(d + 0x22);
    return (uint16_t)d;
c17:
    if (R16(CH_FACE) != 0) goto none;
    d = S16(seg4_021562());
    if (R16(CH_COL) == R16(gCellCol)) d = (int16_t)(d - 0x21);
    else d = (int16_t)(d + 0x12);
    return (uint16_t)d;
barrier:
    if (d >= 0x33) goto none;
    W16(gBarrSide, 1);
    return (uint16_t)d;
}

/* 00023acc: is the character standing inside a closed gate-like barrier (kind 4)? */
uint32_t seg4_023acc(void) {
    int16_t cell = S16(FUN_00021076());
    if (R16(CH_ALIVE) >= 0) return 0;
    if (cell != 4 && S16(FUN_00021518()) != 4 && S16(GetCellsInFront(1)) != 4) return 0;
    if (R16(gCellRoom) == 9 && LEVEL_TYPE2() == 8) return 0;
    if (R16(CH_TYPE) == 0xc) return 0;
    if (S16(seg4_0235de()) == 0) return 0;
    int16_t col = R16(gCellCol);
    W16(gCDCellX, (int16_t)(R16(GH(0x103478) + (int32_t)col * 2) + 0x16));
    int16_t l = S16(GetLeftBarr(R16(gCellRoom), col, R16(gCellRow)));
    int16_t r = S16(GetRightBarr(R16(gCellRoom), R16(gCellCol), R16(gCellRow)));
    int16_t room = R16(gCellRoom);
    if (room != G16(-0x4fc2)) {
        if (room == G16(-0x4fc0)) { l = (int16_t)(l - 0x1fe); r = (int16_t)(r - 0x1fe); }
        else if (R16(gCellRoom) == G16(-0x4fbe)) { l = (int16_t)(l + 0x1fe); r = (int16_t)(r + 0x1fe); }
        else { l = 999; r = 0; }
    }
    if (l < R16(gCharRightX) && r > R16(gCharLeftX)) FUN_00023cd2();
    return 0;
}

/* 00023bb4: the kid runs into a closing/closed gate (cell kind 4) */
uint32_t CheckGateForKid(void) {
    int16_t cell = S16(FUN_00021076());
    if (R16(CH_ALIVE) >= 0) return 0;
    if (R16(CH_SEQFRAME) == 0x76) return 0;
    if (cell != 4 && S16(FUN_00021518()) != 4 && S16(GetCellsInFront(1)) != 4) return 0;
    if (R16(gCellRoom) == 9 && LEVEL_TYPE2() == 8) return 0;
    int32_t i = R16(gCellCol);
    if (((uint32_t)RU16(gCDCodes + i * 2) & (uint32_t)RU16(gCDPrevCodes + i * 2)) != 0xff) return 0;
    if (S16(seg4_0235de()) == 0) return 0;
    if (R16(CH_FRAME) == 0x6d || R16(gCellInfo) == 0 || R16(GH(0x102fc4)) == 1) {
        if (LEVEL_TYPE() == 3 && (RU16(gCellInfo) & 0xff) != 0 && R16(GH(0x102fc4)) != 1) {
            FUN_0002d108();
        } else {
            FUN_00023540();
            FUN_00023cd2();
        }
    } else {
        if (R16(CH_SEQFRAME) == 0x32) return 0;
        JumpSeq(0x32);
        int16_t d = S16(seg4_02397e());
        if (d < 0x19) W16(CH_X, AddCharX((int16_t)(d - 0x19)));
    }
    return 0;
}

/* 00023cd2: push the character out of a gate barrier through the nearer side */
uint32_t FUN_00023cd2(void) {
    int16_t front, back, dx;
    if (S16(GetBarrDistFromFace(4, R16(gCellRoom), R16(gCellCol))) < 0)
        front = (int16_t)-(int32_t)S16(GetBarrDistFromFace(4, R16(gCellRoom), R16(gCellCol)));
    else
        front = S16(GetBarrDistFromFace(4, R16(gCellRoom), R16(gCellCol)));
    if (S16(GetBarrDistFromBack(4, R16(gCellRoom), R16(gCellCol))) < 0)
        back = (int16_t)-(int32_t)S16(GetBarrDistFromBack(4, R16(gCellRoom), R16(gCellCol)));
    else
        back = S16(GetBarrDistFromBack(4, R16(gCellRoom), R16(gCellCol)));
    if (front < back) {
        dx = (int16_t)-(int16_t)(front + 3);
        if (R16(CH_TYPE) == 4) dx = (int16_t)(dx - 8);
    } else {
        dx = back;
    }
    W16(CH_X, AddCharX(dx));
    return 0;
}

static int32_t abs32(int16_t v) { return v < 0 ? -(int32_t)v : (int32_t)v; }

/* 00023d80: collision of an enemy (or flying head) with barriers while riding/fighting */
uint32_t EnemyCollide(void) {
    int16_t t = R16(CH_TYPE);
    int16_t head = (t == 7 || t == 8) ? 1 : 0;
    int16_t walker = (t == 7 || t == 8) ? 0 : 1;
    if ((int32_t)walker != (int32_t)R16(CH_ACTION)) {
        if (t != 7 && t != 8) return 0;
        if (R16(CH_SEQFRAME) == 0xa4) return 0;
    }
    if (R16(CH_ALIVE) >= 0 && head == 0) return 0;
    if (R16(GH(0x102fc4)) != 1) return 0;
    if (R16(CH_SEQFRAME) == 0x3a) return 0;
    int16_t cell = S16(FUN_00021076());
    if (S16(IsWall(cell)) == 0 && !(cell == 4 && S16(seg4_0235de()) != 0)) {
        cell = S16(GetCellsInFront(1));
        if (S16(IsWall(cell)) == 0 && !(cell == 4 && S16(seg4_0235de()) != 0)) cell = 0;
    }
    if (cell == 0) return 0;
    FUN_00018138();
    GetCharEdges();
    if (S16(CheckCollide(cell)) == 0) return 0;
    int16_t seq = -1;
    int16_t back = S16(GetBarrDistFromBack(R16(gCellKind), R16(gCellRoom), R16(gCellCol)));
    int16_t front = S16(GetBarrDistFromFace(R16(gCellKind), R16(gCellRoom), R16(gCellCol)));
    int16_t d = (abs32(front) < abs32(back)) ? front : back;
    if (abs32(d) < 5) {
        if (d < 0) d = -5;
        else if (d > 0) d = 5;
        else {
            int16_t s = (front != 0) ? front : back;
            d = (s >= 0) ? 5 : -5;
        }
        if (S16(IsWall(cell)) != 0 && d < 0 && abs32(front) < 5) d = (int16_t)-d;
    }
    if (head != 0) seq = S16(HeadEnemyCollide(front));
    if (seq == -1 && d < 5 && (int32_t)d >= -(int32_t)R16(gCharImgW)) {
        int16_t c = cell;
        if (c == 4 && d > 0) d = (int16_t)-d;
        else if (S16(IsWall(c)) != 0 && d < 0 && d == front) d = (int16_t)-d;
        W16(CH_X, AddCharX((int16_t)-d));
        if (d >= 0 && cell == 4 && head == 0) {
            seq = 0x40;
        } else {
            int skip = 0;
            if (head != 0) {
                int16_t s = R16(CH_SEQFRAME);
                if (s == 0x91 || s == 0xa4 || R16(CH_FIELD34) == 1) skip = 1;
            }
            if (!skip) {
                int16_t tt = R16(CH_TYPE);
                seq = (tt == 7 || tt == 8) ? 0x9e : 0x41;
            }
        }
    }
    if (seq != -1) {
        JumpSeq(seq);
        AnimChar();
        FUN_00020542();
    }
    return 0;
}

/* 00024032: distance from the character's front edge to the barrier of a cell (-1 = none) */
uint32_t GetBarrDistFromFace(int16_t cell, int16_t room, int16_t col) {
    if (cell == 4 && S16(seg4_0235de()) == 0) return 0xffff;
    int16_t x = (int16_t)(R16(GH(0x103478) + (int32_t)col * 2) + 0x16);
    W16(gCDCellX, AdjustRoom(room, x));
    int16_t code = S16(GetBarrCode(cell));
    int16_t d;
    if (code == 0) {
        d = -1;
    } else if (R16(CH_FACE) != 0) {
        d = R16(gCharLeftX);
        d = (int16_t)(d - (int16_t)(R16(gCDCellX) + 0x32 - R16(gBarrRightOfs + (int32_t)code * 2)));
    } else {
        d = R16(gCDCellX);
        d = (int16_t)(d + R16(gBarrLeftOfs + (int32_t)code * 2));
        d = (int16_t)(d - R16(gCharRightX));
    }
    return (uint16_t)d;
}

/* 000240e0: distance from the character's back edge to the barrier of a cell (200 = none) */
uint32_t GetBarrDistFromBack(int16_t cell, int16_t room, int16_t col) {
    int16_t x = (int16_t)(R16(GH(0x103478) + (int32_t)col * 2) + 0x16);
    W16(gCDCellX, AdjustRoom(room, x));
    int16_t code = S16(GetBarrCode(cell));
    int16_t d;
    if (code == 0) {
        d = 200;
    } else if (R16(CH_FACE) >= 0) {
        d = R16(gCharLeftX);
        d = (int16_t)(d - (int16_t)(R16(gCDCellX) + 0x32 - R16(gBarrRightOfs + (int32_t)code * 2)));
    } else {
        d = R16(gCDCellX);
        d = (int16_t)(d + R16(gBarrLeftOfs + (int32_t)code * 2));
        d = (int16_t)(d - R16(gCharRightX));
    }
    return (uint16_t)d;
}

/* 0002417e: empty */
uint32_t seg4_02417e(void) {
    return 0;
}
