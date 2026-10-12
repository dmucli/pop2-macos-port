/* CODE 5 "Modular Set 2" (second half, Ghidra 0x27476-0x2a632): moving objects
 * ("mobs": falling floors, spikes, darts), gates/loose floors/scythes animation,
 * sound/music cueing (ambient sounds, Prince sound slots) and recorded-game playback.
 * Ported from the Ghidra decompilation of the original 68k code, checked against the
 * disassembly; addresses in comments are Ghidra addresses (see docs/PORTING.md).
 *
 * Note: Ghidra shows the 68020 memory-indirect operands of this segment
 * (e.g. `lea ([-0x5014,A5,D1],0x780),A0`) as pre-indexed; the extension words
 * (I/IS = 101/110) say post-indexed, i.e. *(gGameState) + D1 + 0x780. */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* ---- globals (Ghidra addresses) ---- */
#define gGameState      GH(0x102fec)   /* long: pointer to the game state block */
#define GS16(off)       R16(R32(gGameState) + (off))
#define LEVEL_TYPE()    GS16(0x2186)   /* word: level type (2 = ..., 3 = caves, 5 = rooftops) */
#define LEVEL_NUM()     GS16(0x218a)   /* word: level number */

#define gKid            GH(0x102f2e)   /* Char (0x3e bytes): the kid */
#define gKidCol         GH(0x102f3a)
#define gKidRow         GH(0x102f3c)
#define gKidRoom        GH(0x102f44)
#define gKidFrame       GH(0x102f56)
#define gChars          GH(0x102df8)   /* Char[]: guards/opponents in the kid's room, stride 0x3e */
#define CHAR_SIZE       0x3e
#define gChar           GH(0x102faa)   /* Char: the character being animated */
#define gCharFace       GH(0x102fac)
#define gCharX          GH(0x102fae)
#define gCharY          GH(0x102fb0)
#define gCharAction     GH(0x102fb2)
#define gCharFrame      GH(0x102fb4)
#define gCharCol        GH(0x102fb6)
#define gCharRow        GH(0x102fb8)
#define gCharFallY      GH(0x102fbe)
#define gCharRoom       GH(0x102fc0)
#define gCharAlive      GH(0x102fc2)
#define gCharFlag1a     GH(0x102fc4)
#define gCharId         GH(0x102fd2)
#define gKidLifeFlag    GH(0x102ff8)

#define gCurRoom        GH(0x10303e)
#define gRoomLeft       GH(0x103040)
#define gRoomRight      GH(0x103042)
#define gRoomUp         GH(0x103044)
#define gRoomDown       GH(0x103046)
#define gRoomUL         GH(0x103048)
#define gRoomUR         GH(0x10304a)
#define gRoomDL         GH(0x10304c)
#define gRoomDR         GH(0x10304e)
#define gScreenRect     GH(0x103014)   /* Rect */

#define gMobTable       GH(0x10334e)   /* long: pointer to mob display entries (0x1c bytes) */
#define gMobCount       GH(0x1037da)   /* word */
#define gTrobCells      GH(0x1037dc)   /* long: pointer to the room cells being edited (4 bytes per cell) */
#define gColRemainder   GH(0x1037e4)   /* word: x % 51 from GetCol */
#define gMidOffY        GH(0x1037e6)
#define gTrobState      GH(0x1037e8)   /* word: current trob cell state (long-copied in TriggerDart) */

/* the mob being processed (16 bytes, saved/restored by TriggerDart) */
#define gMobX           GH(0x103b5a)
#define gMobY           GH(0x103b5c)
#define gMobRoom        GH(0x103b5e)
#define gMobSpeed       GH(0x103b60)   /* -1 = mob finished */
#define gMobAccel       GH(0x103b62)
#define gMobType        GH(0x103b64)
#define gMobRow         GH(0x103b66)
#define gMobState       GH(0x103b68)
#define gMobWidths      GH(0x103ae8)   /* word[] by mob type */
#define gMobHeights     GH(0x103b04)   /* word[] by mob type */

#define gTrobs          GH(0x103b72)   /* {cell, room, id, idroom} 8-byte entries */
#define gTrobCount      GH(0x103c1a)
#define gTrobCell       GH(0x103c12)   /* word: cell of the trob being animated */
#define gTrobRoom       GH(0x103c14)   /* word: room of the trob being animated */
#define gTrobDir        GH(0x103c16)   /* word: trob direction/state, -1 = done */
#define gGateDownSpeed  GH(0x103a12)   /* word[] */
#define gGateSpeed      GH(0x103a24)   /* word[] */

#define gSpikeShapesA   GH(0x103c42)   /* long: word[] spike shape ids (0x84) */
#define gSpikeShapesB   GH(0x103c3e)   /* long: word[] spike shape ids (0x85) */
#define gShapeOffsets   GH(0x103c38)   /* long: {dx, dy} per shape */
#define gShapeFirst     GH(0x103c3c)   /* word: first shape id of the table */

#define gDirDX          GH(0x103468)   /* word[] by direction */
#define gDirDX2         GH(0x10346c)
#define gRowY           GH(0x10347a)   /* word[]: floor y of each row */
#define gColX           GH(0x103498)   /* word[]: x of each column */
#define gRowTop         GH(0x1034b2)   /* word[] */
#define gMidOffX        GH(0x104b3e)
#define gSpikeIdx       GH(0x104b40)
#define gFloorY         GH(0x104a9c)
#define gFloorY2        GH(0x104a9e)
#define gMobFirstCol    GH(0x104b38)
#define gCellA          GH(0x104b50)   /* word: cell piece read by ReadCell */
#define gCellB          GH(0x104b52)   /* word: cell state read by ReadCell */
#define gCellIdx        GH(0x104b54)   /* word: cell index read by ReadCell */
#define gCellRoom       GH(0x104b56)   /* word: room of that cell */
#define gCellCol        GH(0x104b58)
#define gCellRow        GH(0x104b5a)
#define gScytheA        GH(0x1048e6)   /* word[]: scythe frames */
#define gScytheB        GH(0x1048e8)

#define pUnionRect      GH(0x1060a4)   /* long: address of UnionRect */
#define pSectRect       GH(0x1060a8)   /* long: address of SectRect */
#define gEmptyRect      GH(0x106694)   /* Rect */

/* address of the cell word of `cell` in room `room` (rooms are 0x78 bytes at gs+0x708) */
static uint32_t room_cell(uint16_t room, int16_t cell) {
    return R32(gGameState) + ((uint32_t)room - 1) * 0x78 + 0x780 + (int32_t)cell * 4;
}

/* Pascal SectRect(src1, src2, VAR dst): Boolean through the pointer at pSectRect */
static uint8_t sect_rect_ptr(maddr src1, maddr src2, maddr dst) {
    maddr sp0 = emu_sp();
    push16(0);
    push32(src1);
    push32(src2);
    push32(dst);
    call68_addr(R32(pSectRect));      /* restores SP; the result is in the slot below sp0 */
    uint8_t r = R8(sp0 - 2);
    emu_set_sp(sp0);
    return r;
}

/* ---- stack depth ----
 * The 68k sound item routines read uninitialised locals, so the dead stack below them must sit
 * where it sits in the original (the checker sees the garbage they copy into the heap otherwise).
 * Ported functions on the sound paths therefore keep the original stack depth: WITH_DEPTH(n, ...)
 * lowers SP by n = 4 (return address, already popped by the hook) + the original's link size +
 * saved registers around the body, and C_<Name>() lowers it by the argument bytes the original
 * pushes when one of these functions calls another. */
#define SP_DOWN(name, n)  maddr name##__sp = emu_sp(); emu_set_sp(name##__sp - (n))
#define SP_UP(name)       emu_set_sp(name##__sp)
#define WITH_DEPTH(n, call) do { SP_DOWN(dpt, n); uint32_t r__ = (call); SP_UP(dpt); return r__; } while (0)
#define CALL_A(n, call)   __extension__ ({ SP_DOWN(cla, n); uint32_t r2__ = (call); SP_UP(cla); r2__; })
#define C_PrinceSndOn(a) CALL_A(2, PrinceSndOn(a))
#define C_PrinceSetSnd(a) CALL_A(2, PrinceSetSnd(a))
#define C_PrinceResetSnd(a) CALL_A(2, PrinceResetSnd(a))
#define C_PrinceReleaseSnd(a) CALL_A(2, PrinceReleaseSnd(a))
#define C_SetCueSnd(a, b) CALL_A(4, SetCueSnd(a, b))
#define C_TimeForNewAmbient(a) CALL_A(4, TimeForNewAmbient(a))
#define C_AddGateSound(a) CALL_A(2, AddGateSound(a))

/* 00027476: rectangle covered by the current mob (offset into the current room) */
uint32_t ComputeMobRect(uint32_t r) {
    W16(r, R16(gMobY) - R16(gMobWidths + (int16_t)(RU16(gMobType) << 1)));
    W16(r + 2, R16(gMobX));
    W16(r + 4, R16(gMobY) + 1);
    W16(r + 6, R16(r + 2) + R16(gMobHeights + (int16_t)(RU16(gMobType) << 1)));
    int16_t room = R16(gMobRoom);
    if (room == R16(gCurRoom)) return 0;
    int16_t dh, dv;
    if (room == R16(gRoomLeft) || room == R16(gRoomUL) || room == R16(gRoomDL)) dh = -0x1fe;
    else if (room == R16(gRoomRight) || room == R16(gRoomUR) || room == R16(gRoomDR)) dh = -0x1fe;
    else dh = 0;
    if (room == R16(gRoomUp) || room == R16(gRoomUL) || room == R16(gRoomUR)) dv = -0x16d;
    else if (room == R16(gRoomDown) || room == R16(gRoomDL) || room == R16(gRoomDR)) dv = 0x16d;
    else dv = 0;
    if ((dh | dv) != 0) {
        _OffsetRect(r, dh, dv);
    } else {
        W32(r, R32(gEmptyRect));
        W32(r + 4, R32(gEmptyRect + 4));
    }
    return 0;
}

/* 0002757a: mark the buffers under the mob rectangle for redraw */
uint32_t MarkMobRedraw(int16_t how) {
    FRAME(f, 8);
    maddr rect = f;
    ComputeMobRect(rect);
    sect_rect_ptr(gScreenRect, rect, rect);
    maddr fn;
    if (how == 1) fn = JT_ADDR(FN_MarkFRedraw);
    else if (how == 2) fn = JT_ADDR(FN_MarkFloor);
    else fn = JT_ADDR(FN_MarkMove);
    MarkBufsForRect(fn, rect);
    FRAME_END(f);
    return 0;
}

/* 000275ea: append the current mob to the mob display table */
uint32_t AddMobObj(int16_t y) {
    int16_t n = R16(gMobCount);
    W16(gMobCount, n + 1);
    uint16_t type = RU16(gMobType) | 0x80;
    int32_t off = (int32_t)n * 0x1c;
    W16(R32(gMobTable) + off + 0xc, type);
    W16(R32(gMobTable) + off, R16(gMobX));
    W16(R32(gMobTable) + off + 2, y);
    W16(R32(gMobTable) + off + 4, 4);
    W16(R32(gMobTable) + off + 6, R16(gMobState));
    ComputeMobRect(R32(gMobTable) + off + 0xe);
    uint32_t r = R32(gMobTable) + off + 0xe;
    sect_rect_ptr(gScreenRect, r, r);
    W16(R32(gMobTable) + off + 0x18, 0);
    W16(R32(gMobTable) + off + 8, 0);
    W16(R32(gMobTable) + off + 0x1a, 0xffff);
    W16(R32(gMobTable) + off + 0x16, 0);
    return 0;
}

/* 000276f6: crush the kid / guards standing under a falling mob */
uint32_t FUN_000276f6(void) {
    if (R16(gMobRoom) == 0) return 0;
    if (S16(CharUnderMob(gKid)) != 0) {
        FUN_000225d2();
        CrushChar();
        mcopy(gKid, gChar, 0x3e);
    }
    uint16_t room = RU16(gMobRoom);
    int16_t n;
    if (room != 0) n = R16(R32(gGameState) + (uint32_t)room * 0xc0 + 0x20e6);
    else n = 0;
    if (n == 0) return 0;
    uint32_t p = gChars;
    int16_t i = 0;
    if (n <= 0) return (uint16_t)n;
    do {
        if (R16(gMobRoom) == R16(gKidRoom)) {
            if (S16(CharUnderMob(p)) != 0) {
                LoadShad(R16(p));
                CrushChar();
                seg4_022516();
            }
        } else {
            uint32_t opp = GetOppInfoIdx(R16(gMobRoom), i);
            if (S16(OppInfoUnderMob(opp)) != 0) CrushOppInfo(opp, R16(gMobRoom));
        }
        i++;
        p += CHAR_SIZE;
    } while (i < n);
    return 0x3e;
}

/* 000277c2: is the character struct p under the falling mob? */
uint32_t CharUnderMob(uint32_t p) {
    int16_t res = 0;
    int16_t top;
    if (R16(p + 8) == 0xb) top = R16(p + 6);
    else if (R16(p + 8) == 7 || R16(p + 8) == 8) top = R16(p + 6) - 0x26;
    else top = R16(p + 6) - 0x39;
    if (R16(p + 0x1c) >= 0) return 0;
    if (R16(gMobRoom) != R16(p + 0x16)) return 0;
    if (R16(gMobY) <= top) return 0;
    if (R16(p + 0xe) != R16(gMobRow)) return 0;
    int16_t col = (int16_t)(R16(gMobX) / 0x33);
    res = (col == R16(p + 0xc));
    if (res != 0) return (uint16_t)res;
    if (R16(p + 8) != 0xb) return 0;
    LoadShad(R16(p));
    int16_t fr = S16(seg4_021562());
    if ((int32_t)R16(p + 0xc) + R16(gDirDX2 + (int16_t)(R16(p + 2) * 2)) == (int32_t)col) {
        res = fr > 0xc;
    } else if ((int32_t)R16(p + 0xc) + R16(gDirDX + (int16_t)(R16(p + 2) * 2)) == (int32_t)col) {
        res = fr < 6;
    }
    return (uint16_t)res;
}

/* 000278fe: is the (off-screen) opponent info p under the falling mob? */
uint32_t OppInfoUnderMob(uint32_t p) {
    int16_t res = 0;
    int16_t top = (int16_t)((int16_t)(R16(p) / 10) * 0x78 + 0x6a);
    if (!(GS16(0x21a4) == 5 && R16(p + 0x18) == 8)) top -= 0x39;
    int16_t y = R16(gMobY);
    if (y <= top) return 0;
    int16_t d = (int16_t)(y - top);
    if ((int32_t)R16(gMobSpeed) * 4 < (int32_t)d) return 0;
    int32_t c = ((int32_t)R16(p + 2) - 0xe5) / 0x33;
    res = (c == (int32_t)(int16_t)(R16(gMobX) / 0x33));
    return (uint16_t)res;
}

/* 000279a8: play a gate sound (with the level-5 gate music) */
static uint32_t AddGateSound_body(int16_t snd) {
    AddSound(snd);
    if (snd != 0x14) return 0;
    if (LEVEL_NUM() != 5) return 0;
    if (R16(gTrobRoom) != 7) return 0;
    if (S16(C_PrinceSndOn(0x78)) != 0) return 0;
    if (LEVEL_NUM() == 5 && R16(gKidRoom) == 7 && R16(gKidCol) < 3) return 0;
    AddSong(0x78);
    return 0;
}
uint32_t AddGateSound(int16_t snd) {
    WITH_DEPTH(16, AddGateSound_body(snd));
}

/* 00027a26: trob entry for (room, cell), or 0 */
uint32_t GetTrobRoomCell(int16_t room, int16_t cell) {
    uint32_t found = 0;
    uint32_t p = gTrobs;
    int16_t i = 0;
    if (R16(gTrobCount) <= 0) return 0;
    do {
        if (R16(p + 2) == room && R16(p) == cell) found = p;
        i++;
        p += 8;
    } while (i < R16(gTrobCount) && found == 0);
    return found;
}

/* 00027a80: the n-th active trob entry with id room `room`, or 0 */
uint32_t GetTrobId(int16_t room, int16_t n) {
    int16_t k = 0;
    uint32_t found = 0;
    uint32_t p = gTrobs;
    int16_t i = 0;
    if (R16(gTrobCount) <= 0) return 0;
    do {
        if (R16(p + 6) == room && R16(p + 4) != -1) {
            k++;
            if (k == n) found = p;
        }
        i++;
        p += 8;
    } while (i < R16(gTrobCount) && found == 0);
    return found;
}

/* 00027ae0: is cell of room visible on the current screen? */
uint32_t IsVisibleRoomCell(int16_t room, int16_t cell) {
    if (room == R16(gCurRoom) ||
        (room == R16(gRoomLeft) && cell % 10 == 9) ||
        (room == R16(gRoomDown) && cell < 10) ||
        (room == R16(gRoomDL) && cell == 9))
        return 1;
    return 0;
}

/* 00027b48: spikes mob: retract spikes under a moving mob, add the mob objects */
uint32_t ATMSpikes(void) {
    int16_t col = (int16_t)(R16(gMobX) / 0x33);
    int16_t idx = -1;
    int16_t room = R16(gMobRoom);
    int16_t cur = R16(gCurRoom);
    if (room != cur) {
        if (room == R16(gRoomLeft) && col == 10) {
            W16(gMobRoom, cur);
            col = 0;
            W16(gMobX, R16(gMobX) - 0x1fe);
        } else {
            idx = (int16_t)(S16(FirstInRow(R16(gMobRow))) + col - 1);
            uint32_t a = (uint32_t)RU16(gMobRoom) * 0x78 + R32(gGameState) + 0x780 - 0x78 + (int32_t)idx * 4;
            if ((RU16(a) & 0xff) != 0xf0) {
                W16(gMobSpeed, -1);
                W16(a, 0);
            }
        }
    }
    if (R16(gMobSpeed) == -1) return 0;
    if (idx == -1) W16(gMobFirstCol, S16(FirstInRow(R16(gMobRow))) + col);
    else W16(gMobFirstCol, idx + 1);
    AddMobObj(R16(gMobY));
    W16(gMobType, 5);
    AddMobObj(R16(gMobY));
    W16(gMobType, 4);
    return 0;
}

/* 00027c78: does the animated character step into raised spikes? */
uint32_t FUN_00027c78(void) {
    if (R16(gCharId) == 0x55) return 0;
    int16_t act = R16(gCharAction);
    if (act == 4 || act == 1) return 0;
    if (S16(IsDeadPos(R16(gCharFrame))) != 0) return 0;
    int16_t c = (int16_t)(R16(gCharCol) - 1);
    if (c > R16(gCharCol)) return 0;
    do {
        if (S16(ReadCell(R16(gCharRoom), c, R16(gCharRow))) == 2 && S16(FUN_00028124()) == 2) {
            int16_t y = (int16_t)(R16(gRowY + (int16_t)(c * 2)) + 0x16);
            if (R16(gCharFlag1a) == 0) y += 0x16;
            else y += 0xf;
            int hit = 0;
            if (y > R16(gFloorY) &&
                (int32_t)R16(gCharRow) * 0x78 + 0x38 <= (int32_t)R16(gCharY)) {
                uint16_t fr = RU16(gCharFrame);
                if ((fr < 0x79 || fr > 0x84) && fr != 0xf) hit = 1;
            }
            if (!hit && LEVEL_TYPE() == 2 && S16(SlideRockNearSpikes()) != 0) hit = 1;
            if (hit) {
                seg5_027e6e();
                uint32_t m = GetSpikeMobRoomRow(R16(gCharRoom), R16(gCharRow));
                if (m != 0) {
                    W16(m + 0xe, RU16(m + 0xe) & 0xff00);
                    W16(m + 0xe, RU16(m + 0xe) | 0xf0);
                    uint16_t v = RU16(m + 0xe);
                    W16((uint32_t)RU16(gCellRoom) * 0x78 + (int32_t)R16(gCellIdx) * 4 + R32(gGameState) + 0x708, v);
                }
            }
        }
        c++;
    } while (c <= R16(gCharCol));
    return 0;
}

/* 00027dc2: trigger the spikes in front of the animated character */
uint32_t seg5_027dc2(void) {
    int16_t c = R16(gCharCol);
    if (c < 0) return 0;
    if (R16(gCharAction) == 4) return 0;
    c--;
    if (S16(ReadCell(R16(gCharRoom), c, R16(gCharRow))) != 2) return 0;
    if ((int32_t)R16(gCharRow) * 0x78 + 0x23 > (int32_t)R16(gCharY)) return 0;
    int16_t i = S16(FUN_00021600());
    int16_t x = (int16_t)(R16(gRowY + (int16_t)(i * 2)) + 0x16);
    if (R16(gCharFlag1a) == 0) x += 0x19;
    else x += 3;
    if (R16(gCharFace) == 0) {
        x += R16(gFloorY2);
        x -= S16(FUN_0002393e());
    }
    if (R16(gCharX) > x) return 0;
    TriggerSpikes(R16(gCellRoom), R16(gCellIdx));
    return 0;
}

/* 00027e6e: the character was impaled by spikes */
uint32_t seg5_027e6e(void) {
    uint32_t a = (uint32_t)RU16(gCellRoom) * 0x78 + R32(gGameState) + 0x780 - 0x78 + (int32_t)R16(gCellIdx) * 4;
    if (R16(gCharAction) != 4) W16(a, 0xf0);
    else W16(a, 0x8f);
    uint32_t m = GetMobTypeItem(4, 1);
    if (m != 0 && R16(m + 4) == R16(gCellRoom) && R16(m + 0xc) == R16(gCellRow))
        W16(m + 0xe, RU16(a));
    FUN_00020cca();
    W16(gCharFallY, 0);
    if (R16(gCharAction) == 0 || R16(gCharAction) == 2) {
        W16(gCharAlive, 1);
        int16_t i = S16(FUN_00021600());
        W16(gCharX, R16(gRowY + (int16_t)(i * 2)) + 0x27);
        int16_t seq;
        if (R16(gCharFace) == 0) {
            seq = R16(gCharAction) == 0 ? 0x36 : 0xbd;
            W16(gCharFace, -1);
        } else {
            seq = R16(gCharAction) == 0 ? 0x33 : 0xbc;
        }
        JumpSeq(seq);
        AddSound(0x119);
        DecLifePts(100);
        SetKidDeathMethod(4);
    } else {
        if (R16(gCharAction) != 4) return 0;
        FUN_0001f556();
    }
    AnimChar();
    return 0;
}

/* 00027f92: draw the spikes piece (0x84 = left part, 0x85 = right part); D0 is passed
 * on by DrawObj, so it is reproduced (table address or AddMid's result) */
uint32_t DrawObjSpikes(int16_t kind) {
    uint32_t d0 = 0;
    if (R16(gSpikeIdx) == 0xf0) {
        d0 = 7;
        W16(gSpikeIdx, 7);
    } else if (RU16(gSpikeIdx) & 0x80) {
        d0 = 3;
        W16(gSpikeIdx, 3);
    } else {
        W16(gSpikeIdx, RU16(gSpikeIdx) & 0xff);
    }
    uint32_t tbl;
    if (kind == 0x84) tbl = gSpikeShapesA;
    else if (kind == 0x85) tbl = gSpikeShapesB;
    else return d0;
    d0 = (int32_t)R16(gSpikeIdx) * 2 + R32(tbl);
    int16_t s = R16(d0);
    if (s == 0) return d0;
    uint32_t p = ((int32_t)s - (int32_t)R16(gShapeFirst)) * 4 + R32(gShapeOffsets);
    int16_t v = (int16_t)(R16(gMidOffX) + R16(p));
    int16_t h = (int16_t)(R16(gMidOffY) + R16(p + 2));
    return AddMid(4, s, h, v, 0x10);
}

/* 0002808c: spikes state -> frame index */
uint32_t GetSpikeIdx(int16_t state) {
    if (state & 0x80) return 3;
    return (uint16_t)(state & 7);
}

/* 000280c2: spikes mob for (room, row), or 0 */
uint32_t GetSpikeMobRoomRow(int16_t room, int16_t row) {
    int16_t done = 0;
    int16_t i = 1;
    uint32_t m;
    do {
        m = GetMobTypeItem(4, i++);
        if (m == 0) done = 1;
        else if (R16(m + 4) == room && R16(m + 0xc) == row) done = 1;
    } while (done == 0);
    return m;
}

/* 00028124: classify the spikes state read by ReadCell: 0 none/jammed, 1 retracting, 2 rising */
uint32_t FUN_00028124(void) {
    uint16_t s = RU16(gCellB) & 0xff;
    if (s == 0 || s == 0xf0) return 0;
    if (s & 0x80) return 1;
    if ((int16_t)s < 3) return 2;
    return 0;
}

/* 0002815a: animate the spikes mob */
uint32_t MobSpikes(void) {
    if (R16(gMobSpeed) == -1) return 0;
    int16_t c = (int16_t)((int16_t)(R16(gMobX) / 0x33) - 1);
    ReadCell(R16(gMobRoom), c, R16(gMobRow));
    uint16_t s = RU16(gCellB) & 0xff;
    if (s == 1) AddSound(0x11a);
    if (s == 0xf0) return 0;
    if (s & 0x80) {
        s--;
        if ((s & 0x7f) == 0) {
            s = 4;
            AddSound(0x11c);
        }
    } else {
        s++;
        if (s == 3) s = 0x8f;
        else if (s == 7) {
            s = 0;
            W16(gMobSpeed, -1);
        }
    }
    CalcBlue(R16(gCellRoom));
    W16(R32(gTrobCells) + (int32_t)R16(gCellIdx) * 4, s);
    W16(gMobState, s);
    return 0;
}

/* 00028220: raise the spikes at cell of room */
uint32_t TriggerSpikes(int16_t room, int16_t cell) {
    uint32_t a = (int32_t)cell * 4 + R32(gTrobCells);
    uint16_t s = RU16(a) & 0xff;
    if (s != 0 && (s & 0x80) == 0) return 0;
    if (s == 0) {
        uint32_t gs = R32(gGameState);
        int16_t x0 = R16(gs + 0x2186) == 3 ? 0x3f : 0x42;
        W16(gMobX, (int16_t)((cell % 10) * 0x33) + x0);
        int16_t row = (int16_t)(cell / 10);
        int16_t dy = R16(gs + 0x2186) == 3 ? -0x13 : -0x14;
        W16(gMobY, (int16_t)(row * 0x78) + 0x7d + dy);
        W16(gMobRow, row);
        W16(gMobRoom, room);
        W16(gMobAccel, 0);
        W16(gMobSpeed, 0);
        W16(gMobType, 4);
        uint16_t v = RU16(a) + 1;
        W16(a, v);
        W16(gMobState, v);
        FUN_00026a94();
    } else if (s != 0xf0) {
        W16(R32(gTrobCells) + (int32_t)cell * 4, 0x8f);
        uint32_t m = GetSpikeMobRoomRow(R16(gCharRoom), R16(gCharRow));
        if (m != 0) W16(m + 0xe, 0x8f);
    }
    return 0;
}

/* 0002832a: animate a gate */
static uint32_t AnimGate_body(void) {
    int16_t st = R16(gTrobDir);
    int16_t v = RU16(gTrobState) & 0xff;
    int16_t snd;
    int16_t squish;
    if (st < 0) goto tail;
    if (LEVEL_TYPE() == 3 && R16(gKidFrame) == 0x76) {
        int16_t cell = R16(gTrobCell);
        int16_t row = (int16_t)(cell / 10);
        int16_t col = (int16_t)(cell % 10);
        if (R16(gTrobRoom) != R16(gKidRoom)) {
            if (R16(gTrobRoom) == R16(gRoomLeft)) col -= 10;
            else if (R16(gTrobRoom) == R16(gRoomRight)) col += 10;
        }
        squish = R16(gKidRow) == row &&
                 (R16(gKidCol) == col || (int32_t)col + 1 == (int32_t)R16(gKidCol) ||
                  (int32_t)col - 1 == (int32_t)R16(gKidCol));
    } else {
        squish = 0;
    }
    if (st >= 4) {
        if (st < 8) {
            st++;
            W16(gTrobDir, st);
        }
        v -= R16(gGateDownSpeed + st * 2);
        if (v <= 0) {
            if (squish) {
                v = 0x38;
                snd = 0x14;
            } else {
                v = 0;
                snd = LEVEL_TYPE() == 3 ? 0x14 : 0x113;
                CheckSquishedFingers();
                if (S16(DidKidEscapeOpp()) != 0) W16(gKidLifeFlag, 4);
            }
            C_AddGateSound(snd);
            W16(gTrobDir, -1);
        }
    } else if (v == 0xff) {
        StopGate(0xff);
    } else {
        v += R16(gGateSpeed + st * 2);
        if (st == 0 || st == 1) {
            if (v <= 0) {
                v = squish ? 0x38 : 0;
                StopGate(v);
                CheckSquishedFingers();
            } else if (v < 0xb8) {
                snd = LEVEL_TYPE() == 3 ? 0x11 : 0x110;
                C_AddGateSound(snd);
                W16(gTrobDir, 1);
                if (squish) v = S16(CaveMoveSquishingGate(v));
            } else if (LEVEL_NUM() == 5 && R16(gTrobRoom) == 7) {
                v -= R16(gGateSpeed + st * 2);
            }
        } else if (v < 0xb8) {
            if (LEVEL_TYPE() == 3) C_AddGateSound(0x11);
            else if ((v & 7) == 0) C_AddGateSound(0x110);
        } else if (st < 3) {
            RedrawGate();
            v = 0xed;
            W16(gTrobDir, 0);
            snd = LEVEL_TYPE() == 3 ? 0x12 : 0x111;
            C_AddGateSound(snd);
        } else {
            v = 0xff;
            StopGate(0xff);
        }
    }
    W16(gTrobState, (RU16(gTrobState) & 0xff00) + v);
tail:
    if (v <= 0xb8) RedrawGate();
    return 0;
}
uint32_t AnimGate(void) {
    WITH_DEPTH(16, AnimGate_body());
}

/* 000285ea: animate a loose floor; when it falls, turn it into a mob */
uint32_t AnimLoose(void) {
    W16(gTrobState, RU16(gTrobState) + 1);
    int16_t lo = RU16(gTrobState) & 0xf;
    if (R16(gTrobDir) != -1) {
        if (R8(gTrobState + 1) & 0x40) {
            if (lo >= 4) {
                W16(gTrobDir, -1);
                W16(gTrobState, RU16(gTrobState) & 0xffb0);
            }
        } else if (lo >= 0xc) {
            int16_t type = LEVEL_TYPE() == 3 ? 0 : 3;
            MakeSpace(R16(gTrobRoom), R16(gTrobCell));
            W16(gTrobState, RU16(room_cell(RU16(gTrobRoom), R16(gTrobCell))));
            int16_t cell = R16(gTrobCell);
            int16_t row = (int16_t)(cell / 10);
            int16_t col = (int16_t)(cell % 10);
            W16(gMobX, (uint16_t)(0x33 * (uint16_t)col));
            W16(gMobY, (uint16_t)((uint16_t)(row + 1) * 0x78) + 5);
            W16(gMobRoom, R16(gTrobRoom));
            W16(gMobAccel, 0);
            W16(gMobSpeed, R16(gTrobDir));
            W16(gMobType, type);
            W16(gMobRow, row);
            W16(gMobState, 0);
            if (LEVEL_TYPE() == 3) {
                W16(gMobY, R16(gMobY) + 0x12);
                CaveClearFlawFromPieceBelow(R16(gTrobRoom), R16(gTrobCell));
            }
            FUN_00026a94();
            W16(gTrobDir, -1);
        }
    }
    RedrawLoose();
    return 0;
}

/* 00028704: animate a scythe */
uint32_t AnimScythe(void) {
    int16_t f = RU16(gTrobState) & 0x1f;
    if (f < 0x13) {
        W16(gTrobState, RU16(gTrobState) + 1);
        if (R16(gScytheA + f * 2) != R16(gScytheB + f * 2)) RedrawScythe(0);
        if (f == 0) {
            AddSound(0x115);
        } else if (f == 0xf) {
            AddSound(0x114);
        } else if (f >= 0x10 && (R8(gTrobState + 1) & 0x40) && !(R8(gTrobState + 1) & 0x80)) {
            W16(gTrobState, RU16(gTrobState) | 0x80);
            RedrawScythe(1);
        }
    } else {
        W16(gTrobState, RU16(gTrobState) & 0xffe0);
        W16(gTrobDir, -1);
    }
    int16_t cell = R16(gTrobCell);
    int16_t row = (int16_t)(cell / 10);
    int16_t col = (int16_t)(cell % 10);
    ReadCell(R16(gTrobRoom), col - 1, row);
    W16(gCellB, RU16(gCellB) & 0xff00);
    W16(gCellB, RU16(gCellB) | (RU16(gTrobState) & 0xff));
    W16(room_cell(RU16(gCellRoom), R16(gCellIdx)), RU16(gCellB));
    return 0;
}

/* 00028802: re-create the mob of jammed spikes at cell of room */
uint32_t CheckSpikesJammed(int16_t room, int16_t cell) {
    int16_t row = (int16_t)(cell / 10);
    int16_t col = (int16_t)(cell % 10);
    if (GetSpikeMobRoomRow(room, row) != 0) return 0;
    uint16_t v = RU16(room_cell((uint16_t)room, cell));
    if ((v & 0xff) != 0xf0) return 0;
    int16_t x0 = LEVEL_TYPE() == 3 ? 0x3f : 0x42;
    W16(gMobX, (uint16_t)(0x33 * (uint16_t)col) + x0);
    int16_t dy = LEVEL_TYPE() == 3 ? -0x13 : -0x14;
    W16(gMobY, (uint16_t)((uint16_t)(row + 1) * 0x78) + 5 + dy);
    W16(gMobRow, row);
    W16(gMobRoom, room);
    W16(gMobAccel, 0);
    W16(gMobSpeed, 0);
    W16(gMobType, 4);
    W16(gMobState, v);
    FUN_00026a94();
    return 0;
}

/* 000288e6: did the kid get past the gate before it closed on the guard? */
uint32_t DidKidEscapeOpp(void) {
    int16_t res = 0;
    if (LEVEL_NUM() != 4) return 0;
    if (R16(gKidLifeFlag) == -2) return 0;
    int16_t n;
    if (R16(gTrobRoom) != 0)
        n = R16(R32(gGameState) + ((uint32_t)RU16(gTrobRoom) - 1) * 0xc0 + 0x21a6);
    else
        n = 0;
    int16_t row = (int16_t)(R16(gTrobCell) / 10);
    int16_t col = (int16_t)(R16(gTrobCell) % 10);
    if (n == 0) return 0;
    if (R16(gTrobRoom) != R16(gKidRoom)) return 0;
    if (row != R16(gKidRow)) return 0;
    int32_t d = (int32_t)col - (int32_t)R16(gKidCol);
    if (d < 0) d = -d;
    if (d - 5 >= 0) return 0;
    uint32_t p = gChars;
    for (int16_t i = 0; i < n; i++, p += CHAR_SIZE) {
        if (R16(p + 0xe) == row && R16(p + 0x1c) < 0 && R16(gKidRoom) == R16(p + 0x16)) {
            if ((col < R16(gKidCol) && col < R16(p + 0xc)) || (col >= R16(gKidCol) && col >= R16(p + 0xc)))
                return 0;
            res = 1;
        }
    }
    return (uint16_t)res;
}

/* 00028a30: column of an x coordinate (remainder in gColRemainder); D0 = (q-4):col */
uint32_t GetCol(int16_t x) {
    int32_t dvd = (int32_t)((x < 0 ? 0xffff0000u : 0) | (uint16_t)(x - 4));
    int16_t q = (int16_t)(dvd / 0x33);
    int16_t r = (int16_t)(dvd % 0x33);
    int16_t col = (int16_t)(q - 4);
    W16(gColRemainder, r);
    if (x < 0) col--;
    return ((uint32_t)(uint16_t)(q - 4) << 16) | (uint16_t)col;
}

/* 00028a68: add the clipped rectangle of cell to the {flag, Rect} accumulator at acc */
uint32_t MarkRectBuf(uint32_t acc, int16_t cell, uint32_t clip) {
    FRAME(f, 8);
    maddr rect = f;
    int16_t row = (int16_t)(cell / 10);
    int16_t col = (int16_t)(cell % 10);
    ComputeClipRect(rect, col, row);
    if (sect_rect_ptr(clip, rect, rect)) {
        if (R16(acc) != 0) {
            call_ptr(R32(pUnionRect), "lll", acc + 2, acc + 2, rect);
        } else {
            W16(acc, 1);
            W32(acc + 2, R32(rect));
            W32(acc + 6, R32(rect + 4));
        }
    }
    FRAME_END(f);
    return 0;
}

/* 00028af0: ReadCell with a cell index */
uint32_t ReadCellWithCell(int16_t room, int16_t cell) {
    int16_t row = (int16_t)(cell / 10);
    int16_t col = (int16_t)(cell % 10);
    return ReadCell(room, col, row);
}

/* 00028b36: fire a dart from the shooter at cell of room. The original stores the
 * adjusted room into its own argument slot; glue_TriggerDart writes it back. */
static int16_t s_triggerDartRoom;
uint32_t TriggerDart(int16_t room, int16_t cell) {
    s_triggerDartRoom = room;
    if (RU16(R32(gTrobCells) + (int32_t)cell * 4) & 7) return 0;
    uint8_t saved[16];
    for (int i = 0; i < 16; i++) saved[i] = R8(gMobX + i);
    AddTrob(room, cell, 1, 0x21);
    uint32_t st = R32(gTrobState);
    W32(gTrobState, R32(room_cell((uint16_t)room, cell)));
    FUN_00036daa();
    W32(room_cell((uint16_t)room, cell), R32(gTrobState));
    W32(gTrobState, st);
    AddSound(0x41);
    W16(gMobAccel, 0x1b);
    W16(gMobSpeed, -0x13);
    int16_t tc = R16(gTrobCell);
    W16(gMobRow, (int16_t)(tc / 10));
    int16_t col = (int16_t)(tc % 10);
    int16_t x = (int16_t)(R16(gColX + col * 2) + 0x28);
    if (x > 0x1fe) {
        if (room == R16(gRoomLeft)) {
            room = R16(gCurRoom);
            s_triggerDartRoom = room;
            x -= 0x1fe;
        } else {
            W16(gMobSpeed, -1);
        }
    }
    if (R16(gMobSpeed) != -1) {
        W16(gMobX, x);
        W16(gMobRoom, room);
        W16(gMobY, R16(gRowTop + R16(gMobRow) * 2) - 0x1e);
        W16(gMobType, 2);
        FUN_00026a94();
    }
    for (int i = 0; i < 16; i++) W8(gMobX + i, saved[i]);
    return 0;
}
/* the hook has already popped the return address: the arguments start at SP */
void glue_TriggerDart(void) {
    maddr sp = emu_sp();
    uint32_t r = TriggerDart((int16_t)rd16(sp), (int16_t)rd16(sp + 2));
    wr16(sp, (uint16_t)s_triggerDartRoom);
    emu_set_d0(r);
}

/* ---- sounds ----
 * gSndTable: 8-byte entries per sound id {type, priority, item, kind (0 = MIDI, else 'snd ')} */
#define gSndTable       GH(0x103f60)
#define gSndCur         GH(0x103c62)   /* word: sound requested this frame (-1 = none) */
#define gSongNext       GH(0x103c64)   /* word: song requested this frame (-1 = none) */
#define gSndCue         GH(0x103c66)   /* word: sound currently cued */
#define gSongCur        GH(0x103c68)   /* word: song currently playing */
#define gDeathSongs     GH(0x103c6a)   /* word[] */
#define gSoundsMuted    GH(0x10305a)
#define gAmbient        GH(0x1048c8)   /* {on, last ambient index, last sound} */
#define gAmbientOn      GH(0x1048c8)
#define gAmbientSnd     GH(0x1048cc)
#define gAmbBase        GH(0x103f02)   /* long[] by level type: word[] first ambient sound */
#define gAmbRange       GH(0x103f1e)   /* long[] by level type: word[] number of variants */
#define gAmbFight       GH(0x103f3a)   /* word[] by level type: ambient index while fighting */
#define gSoundOn        GH(0x106088)
#define gMidiFile       GH(0x10608a)
#define gSndFile        GH(0x10608e)
#define gMaxKeepItem    GH(0x106082)

/* 00028c7c: play the kid's death song unless it is already playing */
static uint32_t AddKidDeathSong_body(int16_t which) {
    uint32_t a = gDeathSongs + (int16_t)((int32_t)which << 1);
    if (S16(C_PrinceSndOn(R16(a))) == 0) AddSong(R16(a));
    return 0;
}
uint32_t AddKidDeathSong(int16_t which) {
    WITH_DEPTH(12, AddKidDeathSong_body(which));
}

/* 00028cc0: start the level's song */
uint32_t FUN_00028cc0(void) {
    int16_t s = S16(FUN_00028f8c());
    if (s != -1) AddSong(s);
    return 0;
}

/* 00028cda: the kid screams when falling */
uint32_t AddKidScream(int16_t room) {
    if ((R16(gCharAction) == 0 && LEVEL_TYPE() != 5) ||
        (LEVEL_TYPE() == 5 && room != 0x13 && room != 0x10 && room != 0xf))
        AddSound(8);
    return 0;
}

/* 00028d38: start the "guard alert" music when the character meets an opponent */
static uint32_t FUN_00028d38_body(void) {
    if (R16(gCharAlive) == 0) return 0;
    int16_t go, song;
    switch (R16(gCharAction)) {
    case 2:
        go = R16(GH(0x102fc6)) >= 6;
        song = 0xbd;
        break;
    case 7:
    case 8:
        go = S16(C_PrinceSndOn(0x84)) == 0;
        song = 0x89;
        break;
    case 10:
        go = S16(C_PrinceSndOn(0xf1)) == 0;
        song = 0xee;
        break;
    case 11:
        W16(gCharAlive, 0);
        /* fall through */
    default:
        go = 0;
        if (R16(GH(0x102f4a)) < 0) {
            int16_t id = R16(gCharId);
            if ((id == 0x78 || id == 0x73) && R16(GH(0x102fc6)) > 4 &&
                S16(C_PrinceSndOn(0x3d)) == 0 && S16(C_PrinceSndOn(0x44)) == 0 && S16(C_PrinceSndOn(0x45)) == 0)
                go = 1;
        }
        song = 0x3f;
        break;
    }
    if (go == 0) return 0;
    if (S16(OppToFight()) == 0 && GetOppGenPtOnRow(R16(gCharRoom), R16(gCharRow)) == 0 && LEVEL_TYPE() != 5)
        AddSong(song);
    W16(gCharAlive, 0);
    return 0;
}
uint32_t FUN_00028d38(void) {
    WITH_DEPTH(16, FUN_00028d38_body());
}

/* 00028e5e: sword sound when the kid or the guard he faces strikes */
uint32_t AddSFX(void) {
    if (R16(GH(0x102f68)) == -1) return 0;
    if (R16(GH(0x102f38)) == 0xa7) {
        AddSound(10);
        return 0;
    }
    uint32_t c = (int32_t)R16(GH(0x102f68)) * CHAR_SIZE + gChars;
    if (R16(c + 0xa) != 0xa7) return 0;
    int16_t act = R16(c + 8);
    if (act == 7 || act == 8) return 0;
    AddSound(10);
    return 0;
}

/* 00028ed8: request a song for this frame (first request wins) */
uint32_t AddSong(int16_t song) {
    if (R16(gSongNext) == -1) {
        CheckSnd();
        W16(gSongNext, song);
    }
    return 0;
}

/* 00028f04: request a sound for this frame (highest priority wins); D0 reproduced */
uint32_t AddSound(int16_t snd) {
    uint32_t d0;
    int16_t cur = R16(gSndCur);
    if (cur != -1) {
        d0 = (uint32_t)((int32_t)snd << 3);
        uint16_t ps = RU16(gSndTable + 2 + (int16_t)d0);
        d0 = (d0 & 0xffff0000u) | ps;
        uint16_t pc = RU16(gSndTable + 2 + (int16_t)((int32_t)cur << 3));
        if (ps > pc) return d0;
    } else {
        d0 = 0xffffffffu;
    }
    uint16_t m = RU16(gSoundsMuted);
    d0 = (d0 & 0xffff0000u) | m;
    if (m != 0) return d0;
    if (R16(gCharAction) == 1 && (snd == 0x126 || snd == 0x127 || snd == 0 || snd == 0x12c)) return d0;
    W16(gSndCur, snd);
    return d0;
}

/* 00028f78: empty */
uint32_t CheckSnd(void) {
    return 0;
}

/* 00028f8c: song of the level type (-1 = none) */
uint32_t FUN_00028f8c(void) {
    uint16_t t = RU16(R32(gGameState) + 0x2186);
    int16_t s;
    if (t == 1) s = -1;
    else if (t == 4) s = 0x7c;
    else if (t == 2) s = 0xbb;
    else s = 0x20;
    return (uint16_t)s;
}

/* 00028fc4: reset an ambient-sound record */
uint32_t InitAmbient(uint32_t amb) {
    tc_memset(amb, 0, 6);
    W16(amb, 1);
    return 0;
}

/* 00028ff6: is the level's song playing? */
static uint32_t FUN_00028ff6_body(void) {
    int16_t s = S16(FUN_00028f8c());
    int16_t r;
    if (s != -1) r = S16(C_PrinceSndOn(s));
    else r = 0;
    return (uint16_t)r;
}
uint32_t FUN_00028ff6(void) {
    WITH_DEPTH(12, FUN_00028ff6_body());
}

/* 0002901c: load sound ids first..last into sound items */
static uint32_t LoadSounds_body(int16_t first, int16_t last) {
    if (first == -1) return 0;
    uint32_t e = gSndTable + (int32_t)first * 8;
    int16_t i = first;
    if (first > last) return 0;
    uint32_t item = e + 4;
    uint32_t kind = e + 6;
    int16_t id = (int16_t)(first + 0x2618);
    do {
        uint32_t a2 = kind;
        int16_t si = S16(GetSndItemForId(id, R16(a2)));
        if (si == 0) {
            if (R16(a2) == 0) a2 = resFind(R32(gMidiFile), id, 0x4d494449);   /* 'MIDI' */
            else a2 = resFind(R32(gSndFile), id, 0x736e6420);                  /* 'snd ' */
            if (a2 != 0) {
                si = S16(CreateSound(-1, id, (int32_t)R16(kind)));
                resSetPurgeable(0, a2);
            } else {
                si = 0;
            }
        }
        if (si != 0) {
            W16(item, si);
            SetSoundItemKeepVal(si, -1);
        }
        id++;
        i++;
        item += 8;
        kind += 8;
        e += 8;
    } while (i <= last);
    return 0;
}
uint32_t LoadSounds(int16_t first, int16_t last) {
    WITH_DEPTH(40, LoadSounds_body(first, last));
}

/* 00029112: pick and start a new ambient sound when it is time */
static uint32_t PlayAmbient_body(uint32_t amb) {
    if (R16(gKidRoom) == 0) return 0;
    FUN_000225d2();
    if (S16(C_TimeForNewAmbient(amb)) == 0) return 0;
    uint16_t t = RU16(R32(gGameState) + 0x2186);
    if (R32(gAmbBase + (int16_t)((uint32_t)t << 2)) == 0) return (uint16_t)(t << 2);
    int16_t opp = S16(OppToFight());
    if (opp != 0 && R16(gAmbFight + (int16_t)((uint32_t)RU16(R32(gGameState) + 0x2186) << 1)) == -1) opp = 0;
    int16_t snd = S16(SpecialMusic(opp));
    int16_t idx;
    if (snd == -1) {
        if (opp == 0) {
            int16_t c = S16(IndexCell1(R16(gCharCol), R16(gCharRow)));
            if (c == 0x1e) idx = 0;
            else idx = R16((int32_t)c * 2 + (uint32_t)RU16(gCharRoom) * 0x3c + R32(gGameState) + 0x424a - 0x3c);
        } else {
            idx = R16(gAmbFight + (int16_t)((uint32_t)RU16(R32(gGameState) + 0x2186) << 1));
        }
        int32_t o = (int32_t)idx * 2;
        int16_t ti = (int16_t)((uint32_t)RU16(R32(gGameState) + 0x2186) << 2);
        int16_t base = R16(R32(gAmbBase + ti) + o);
        int16_t range = R16(R32(gAmbRange + ti) + o);
        snd = (int16_t)(S16(Rnd(range)) + base);
        if (snd == R16(amb + 4)) {
            if ((int32_t)base + range != (int32_t)snd) snd++;
            else snd = base;
        }
    } else {
        idx = -1;
    }
    uint32_t r = C_PrinceSetSnd(snd);
    W16(amb + 2, idx);
    W16(amb + 4, snd);
    return r;
}
uint32_t PlayAmbient(uint32_t amb) {
    WITH_DEPTH(40, PlayAmbient_body(amb));
}

/* 000292a2: once per frame: start the requested sound and song */
static uint32_t FUN_000292a2_body(void) {
    if (S16(C_SetCueSnd(R16(gSndCur), R16(gSndCue))) != 0) {
        C_PrinceResetSnd(R16(gSndCue));
        int16_t s = R16(gSndCur);
        W16(gSndCue, s);
        C_PrinceSetSnd(s);
    }
    W16(gSndCur, -1);
    if (R16(gSongNext) == -1) return 0;
    int16_t it = R16(gSndTable + 4 + (int16_t)((int32_t)R16(gSongCur) << 3));
    if (it > 0) ResetSoundItem(it);
    it = R16(gSndTable + 4 + (int16_t)((int32_t)R16(gAmbientSnd) << 3));
    if (it > 0) ResetSoundItem(it);
    int16_t s = R16(gSongNext);
    W16(gSongCur, s);
    it = R16(gSndTable + 4 + (int16_t)((int32_t)s << 3));
    if (it > 0) {
        SetSoundItem(it);
    } else {
        s = R16(gSongNext);
        if (R16(gSndTable + 6 + (int16_t)((int32_t)s << 3)) >= 0) PlaySound(-1, (int16_t)(s + 0x2618), 0);
    }
    UpdateAmbient(gAmbient, R16(gSongNext));
    W16(gSongNext, -1);
    return 0;
}
uint32_t FUN_000292a2(void) {
    WITH_DEPTH(8, FUN_000292a2_body());
}

/* 00029374: apply the sound on/off setting */
static uint32_t seg5_029374_body(void) {
    if (R16(gSoundOn) == 0) SetVolumeLevel(0);
    else SetVolumeLevel(0x100);
    return 0;
}
uint32_t seg5_029374(void) {
    WITH_DEPTH(4, seg5_029374_body());
}

/* 00029390: release the sound item of a sound id; D0 reproduced */
static uint32_t PrinceReleaseSnd_body(int16_t snd) {
    uint32_t e = (int32_t)snd * 8 + gSndTable;
    int16_t it = R16(e + 4);
    if (it > 0) return ReleaseSoundItem(it);
    return e;
}
uint32_t PrinceReleaseSnd(int16_t snd) {
    WITH_DEPTH(16, PrinceReleaseSnd_body(snd));
}

/* 000293d2: stop the sound item of a sound id; D0 reproduced */
static uint32_t PrinceResetSnd_body(int16_t snd) {
    uint32_t e = (int32_t)snd * 8 + gSndTable;
    int16_t it = R16(e + 4);
    if (it > 0) return ResetSoundItem(it);
    return e;
}
uint32_t PrinceResetSnd(int16_t snd) {
    WITH_DEPTH(16, PrinceResetSnd_body(snd));
}

/* 00029412: start the sound item of a sound id; D0 reproduced */
static uint32_t PrinceSetSnd_body(int16_t snd) {
    uint32_t d0 = (uint32_t)((int32_t)snd << 3);
    int16_t it = R16(gSndTable + 4 + (int16_t)d0);
    if (it != 0) return SetSoundItem(it);
    return d0;
}
uint32_t PrinceSetSnd(int16_t snd) {
    WITH_DEPTH(12, PrinceSetSnd_body(snd));
}

/* 00029448: is the sound of a sound id playing? D0 reproduced */
static uint32_t PrinceSndOn_body(int16_t snd) {
    uint32_t d0 = (uint32_t)((int32_t)snd << 3);
    int16_t it = R16(gSndTable + 4 + (int16_t)d0);
    if (it != 0) return SoundItemOn(it);
    return d0 & 0xffff0000u;
}
uint32_t PrinceSndOn(int16_t snd) {
    WITH_DEPTH(16, PrinceSndOn_body(snd));
}

/* 0002948a: may snd replace the cued sound cur? */
static uint32_t SetCueSnd_body(int16_t snd, int16_t cur) {
    int16_t r = 0;
    if (snd == -1) return 0;
    if (S16(C_PrinceSndOn(cur)) == 0) return 1;
    int16_t ty = R16(gSndTable + (int16_t)((int32_t)cur << 3));
    if ((ty == 1 && snd != cur) || ty == 2) {
        if (RU16(gSndTable + 2 + (int16_t)((int32_t)snd << 3)) <= RU16(gSndTable + 2 + (int16_t)((int32_t)cur << 3)))
            r = 1;
    }
    return (uint16_t)r;
}
uint32_t SetCueSnd(int16_t snd, int16_t cur) {
    WITH_DEPTH(32, SetCueSnd_body(snd, cur));
}

/* 00029508: level/room specific music (-1 = none) */
uint32_t SpecialMusic(int16_t fight) {
    if (fight != 0 && LEVEL_NUM() == 5) {
        int16_t c = R16(gCurRoom);
        if (c == 10 || c == 7 || c == 0xc) return 0x77;
    }
    if (fight != 0 && R16(GH(0x102e00)) == 0xc) return 0xf5;
    if (fight != 0 && R16(GH(0x102e00)) == 10) return 0xf6;
    if (R16(GH(0x102f36)) == 1 && R16(GH(0x102f4c)) > 2 && R16(gCurRoom) == 4 && LEVEL_NUM() == 0xd) return 0xfc;
    if (R16(gCurRoom) == 9 && LEVEL_NUM() == 8 && R16(GH(0x102ff6)) != 1) return 0xb4;
    if (LEVEL_NUM() == 0xd) {
        int16_t c = R16(gCurRoom);
        if (c == 4 || c == 0x1d) return 0xfd;
    }
    return 0xffff;
}

/* 000295f8: is some music playing / due that blocks ambient sounds? */
static uint32_t FUN_000295f8_body(void) {
    int16_t r = 0;
    if (R16(GH(0x102f4a)) > 0) r = 1;
    if (S16(FUN_00028ff6()) != 0) return 1;
    if (LEVEL_NUM() == 4 && S16(IsExitChasingSkel(R16(GH(0x102e0e)))) != 0) {
        int16_t f = R16(GH(0x102e20));
        if (f == 0x58 || f == 0x66 || f == 0xd8) return 1;
    }
    if (R16(gKidRoom) == 0x10 && LEVEL_NUM() == 9) return 1;
    if (R16(gKidRoom) == 9 && LEVEL_NUM() == 8 && R16(gKidCol) >= 8) return 1;
    return (uint16_t)r;
}
uint32_t FUN_000295f8(void) {
    WITH_DEPTH(12, FUN_000295f8_body());
}

/* 0002968c: time to start a new ambient sound? */
static uint32_t TimeForNewAmbient_body(uint32_t amb) {
    int16_t r = 0;
    if (R16(amb) == 0) return 0;
    if (R16(GH(0x102fc6)) >= 0) return 0;
    if (S16(FUN_000295f8()) != 0) return 0;
    if (R16(GH(0x102db6)) != 0) return 0;
    if (S16(C_PrinceSndOn(R16(amb + 4))) == 0) return 1;
    if (R16(amb + 2) == -1) {
        if (LEVEL_NUM() != 8) return 0;
        if (S16(C_PrinceSndOn(0xb6)) == 0) return 0;
    }
    int16_t opp = S16(OppToFight());
    if (opp != 0 && R16(gAmbFight + (int16_t)((uint32_t)RU16(R32(gGameState) + 0x2186) << 1)) == -1) opp = 0;
    if (opp != 0) {
        if (R16(amb + 2) < R16(gAmbFight + (int16_t)((uint32_t)RU16(R32(gGameState) + 0x2186) << 1))) r = 1;
        return (uint16_t)r;
    }
    int16_t f = R16(gAmbFight + (int16_t)((uint32_t)RU16(R32(gGameState) + 0x2186) << 1));
    if (R16(amb + 2) < f) return 0;
    if (f == -1) return 0;
    if (GetOppGenPtOnRow(R16(gCharRoom), R16(gCharRow)) != 0) return 0;
    if (LEVEL_NUM() == 8 && S16(C_PrinceSndOn(0xb6)) != 0) return 0;
    r = 1;
    uint16_t room = RU16(gCurRoom);
    int16_t n;
    if (room != 0) n = R16(R32(gGameState) + (uint32_t)room * 0xc0 + 0x20e6);
    else n = 0;
    uint16_t i = 0;
    if (n == 0) return 1;
    uint32_t c = gChars;
    do {
        if (R16(c + 0xe) == R16(gKidRow) && R16(c + 0x18) != 0 && R16(c + 0x1c) < 7 &&
            R16(amb + 2) == R16(gAmbFight + (int16_t)((uint32_t)RU16(R32(gGameState) + 0x2186) << 1)))
            r = 0;
        i++;
        c += CHAR_SIZE;
    } while (r != 0 && i < (uint16_t)n);
    return (uint16_t)r;
}
uint32_t TimeForNewAmbient(uint32_t amb) {
    WITH_DEPTH(40, TimeForNewAmbient_body(amb));
}

/* 00029860: toggle ambient sounds; D0 reproduced */
static uint32_t FUN_00029860_body(void) {
    uint16_t on = R16(gAmbientOn) == 0 ? 1 : 0;
    W16(gAmbientOn, on);
    uint32_t d0 = on;
    if (on == 0 && R16(gAmbientSnd) != 0) d0 = C_PrinceResetSnd(R16(gAmbientSnd));
    return (d0 & 0xffff0000u) | RU16(gAmbientOn);
}
uint32_t FUN_00029860(void) {
    WITH_DEPTH(4, FUN_00029860_body());
}

/* 0002988a: toggle sound on/off */
uint32_t seg5_02988a(void) {
    W16(gSoundOn, R16(gSoundOn) == 0 ? 1 : 0);
    return 0;
}

/* 0002989c: remember the sound started for an ambient record */
uint32_t UpdateAmbient(uint32_t amb, int16_t snd) {
    if (R16(amb) != 0) {
        W16(amb + 4, snd);
        W16(amb + 2, -1);
    }
    return 0;
}

/* 000298ce: let the sound items of ids first..last be purged */
static uint32_t UnloadSounds_body(int16_t first, int16_t last) {
    if (first == -1) return 0;
    uint32_t e = (int32_t)first * 8 + gSndTable;
    int16_t i = first;
    if (first > last) return 0;
    uint32_t item = e + 4;
    do {
        int16_t it = R16(item);
        if (it != 0 && it <= R16(gMaxKeepItem) && S16(C_PrinceSndOn(i)) == 0) SetSoundItemKeepVal(R16(item), 0);
        i++;
        item += 8;
    } while (i <= last);
    return 0;
}
uint32_t UnloadSounds(int16_t first, int16_t last) {
    WITH_DEPTH(24, UnloadSounds_body(first, last));
}

/* ---- scythes ---- */
#define gCurBFItem      GH(0x103074)   /* long: background/foreground item being built */
#define gLevelPieces    GH(0x103462)   /* long: the level's piece table */
#define gColLeft        GH(0x103478)   /* word[]: x of each column */
#define gKidXLeft       GH(0x104a98)
#define gScytheRectA    GH(0x1048d6)   /* Rect: blade hit rect, frame 0x10 */
#define gScytheRectB    GH(0x1048de)   /* Rect: blade hit rect, frame 0x11 */
#define gScytheRedraw   GH(0x1048ce)   /* Rect */
#define gScytheFrames5  GH(0x1047b2)   /* 6-byte {long, word} pieces by frame */
#define gScytheFrames1  GH(0x1047d0)
#define gCharRect       GH(0x102fd4)   /* Rect of the animated character */

/* 0002993c: trigger the scythe the character walks into */
uint32_t FUN_0002993c(void) {
    int16_t act = R16(gCharAction);
    if (act == 7 || act == 8 || act == 0xb) return 0;
    if (R16(GH(0x102fc6)) >= 0) return 0;
    if ((int32_t)R16(gCharRow) * 0x78 + 0x4c > (int32_t)R16(gCharY)) return 0;
    int16_t c = S16(ReadCell(R16(gCharRoom), R16(gCharCol), R16(gCharRow)));
    if (c != 0xc && c != 0xd) return 0;
    if ((int32_t)R16(gColLeft + (int16_t)(R16(gCharCol) * 2)) + 0x1c >= (int32_t)R16(gCharX)) return 0;
    TriggerScythe(R16(gCellRoom), R16(gCellIdx));
    return 0;
}

/* 000299d8: draw the scythe's overlay piece */
uint32_t FUN_000299d8(void) {
    uint32_t it = R32(gCurBFItem);
    if ((RU16(it + 0x14) & 0x20) == 0) return 0;
    if (LEVEL_TYPE() == 4) {
        W16(it + 0x14, 0);
        return FUN_000337ac();
    }
    return seg10_03b452();
}

/* 00029a0a: does a scythe blade hit the animated character? */
uint32_t CheckScytheHit(void) {
    int16_t done = 0;
    if (R16(gCharAction) != 0 && R16(gCharAction) != 2) return 0;
    if (R16(gCharId) == 0x2c) return 0;
    int16_t c = (int16_t)(R16(gCharCol) - 1);
    if ((int32_t)R16(gCharCol) + 1 < (int32_t)c) return 0;
    FRAME(f, 8);
    maddr rect = f;
    do {
        int16_t t = S16(ReadCell(R16(gCharRoom), c, R16(gCharRow)));
        if (t == 0xc || t == 0xd) {
            if (S16(GetScytheHitType(R16(gCellB))) != 0 && S16(ComputeBladeHitRect(rect)) != 0) {
                if (R16(gCharAction) != 0) W16(rect + 6, R16(rect + 6) - 10);
                if (sect_rect_ptr(rect, gCharRect, rect)) {
                    uint16_t fr = RU16(gCharFrame);
                    if (fr < 0x10b || fr > 0x10f) {
                        if (R16(gCharAction) == 0) {
                            JumpSeq(0x71);
                            SetKidDeathMethod(5);
                        } else {
                            JumpSeq(0xbe);
                            W16(GH(0x102fc6), 1);
                        }
                        AnimChar();
                        DecLifePts(100);
                        AddSound(0x116);
                        W16(gCellB, RU16(gCellB) | 0x40);
                        W16(R32(gTrobCells) + (int32_t)R16(gCellIdx) * 4, RU16(gCellB));
                        if (t == 0xc) ReadCell(R16(gCharRoom), c + 1, R16(gCharRow));
                        else ReadCell(R16(gCharRoom), c - 1, R16(gCharRow));
                        W16(gCellB, RU16(gCellB) | 0x40);
                        W16(R32(gTrobCells) + (int32_t)R16(gCellIdx) * 4, RU16(gCellB));
                    }
                }
            }
            done = 1;
        }
        c++;
    } while (done == 0 && (int32_t)R16(gCharCol) + 1 >= (int32_t)c);
    FRAME_END(f);
    return 0;
}

/* 00029b9c: hit rectangle of the scythe blade read by ReadCell, 0 if not dangerous */
uint32_t ComputeBladeHitRect(uint32_t r) {
    uint16_t f = RU16(gCellB) & 0x1f;
    int16_t res = 1;
    if (f == 2) {
        call_ptr(R32(pUnionRect), "lll", r, gScytheRectB, gScytheRectA);
    } else if (f == 0x10) {
        W32(r, R32(gScytheRectA));
        W32(r + 4, R32(gScytheRectA + 4));
    } else if (f == 0x11) {
        W32(r, R32(gScytheRectB));
        W32(r + 4, R32(gScytheRectB + 4));
    } else {
        res = 0;
    }
    if (res != 0) {
        int16_t dh = (int16_t)(R16(gCellCol) * 0x33);
        dh += (int16_t)((R16(gCellA) == 0xd ? 1 : 0) * 0x33);
        _OffsetRect(r, dh, (int16_t)(R16(gCellRow) * 0x78));
    }
    return (uint16_t)res;
}

/* 00029c48: draw a scythe (back part) */
uint32_t FUN_00029c48(void) {
    uint32_t it = R32(gCurBFItem);
    uint16_t kind = RU16(it + 0xa);
    uint16_t fl = RU16(it + 0x14);
    W16(it + 0x14, RU16(it + 0x14) & 0xff00);
    if (LEVEL_TYPE() == 4) seg7_032f0e();
    else FUN_0003ac12();
    if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(kind);
    W16(R32(gCurBFItem) + 6, 0x10);
    W16(R32(gCurBFItem) + 0x14, fl);
    uint16_t flip = fl & 0x40;
    int16_t add = 0;
    if (kind == 0) {
        if (fl & 0x80) {
            W16(R32(gCurBFItem) + 0x18, 0x39);
            W16(R32(gCurBFItem) + 0x1c, -0x42);
        } else {
            W16(R32(gCurBFItem) + 0x18, 0x38);
            W16(R32(gCurBFItem) + 0x1c, -0x45);
        }
        W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0xf4));
        add = 1;
    } else if (kind == 5 || kind == 1) {
        int16_t s = R16(gScytheA + (fl & 0x1f) * 2);
        if (s >= 0x3a && s <= 0x3c) {
            uint32_t src = (int32_t)s * 6 + (kind == 5 ? gScytheFrames5 : gScytheFrames1);
            uint32_t dst = R32(gCurBFItem) + 0x18;
            W32(dst, R32(src));
            W16(dst + 4, RU16(src + 4));
            if (flip) W16(R32(gCurBFItem) + 0x18, R16(R32(gCurBFItem) + 0x18) + 9);
            add = 1;
        }
    }
    if (add && S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(kind);
    FUN_000299d8();
    return 0;
}

/* 00029de0: draw a scythe (front part) */
uint32_t DrawScythe2(void) {
    uint32_t it = R32(gCurBFItem);
    uint16_t kind = RU16(it + 0xa);
    uint16_t fl = RU16(it + 0x14);
    W16(it + 0x14, RU16(it + 0x14) & 0xff00);
    if (LEVEL_TYPE() == 4) W16(R32(gCurBFItem) + 0x14, RU16(R32(gCurBFItem) + 0x14) + 1);
    if (LEVEL_TYPE() == 4) seg7_032f0e();
    else FUN_0003ac12();
    if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(kind);
    W16(R32(gCurBFItem) + 6, 0x10);
    W16(R32(gCurBFItem) + 0x14, fl);
    uint16_t flip = fl & 0x40;
    int16_t add = 0;
    if (kind == 0) {
        if (R16(GH(0x1037ce)) == 0 &&
            (int32_t)R16(GH(0x103a08)) - 1 == (int32_t)R16(R32(gCurBFItem) + 0xe)) {
            if (fl & 0x80) {
                W16(R32(gCurBFItem) + 0x18, 0x39);
                W16(R32(gCurBFItem) + 0x1c, -0x42);
            } else {
                W16(R32(gCurBFItem) + 0x18, 0x38);
                W16(R32(gCurBFItem) + 0x1c, -0x45);
            }
            W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0x108));
            add = 1;
        }
    } else if (kind == 5) {
        int16_t s = R16(gScytheA + (fl & 0x1f) * 2);
        if (s == 0x3d || s == 0x3e) {
            uint32_t src = (int32_t)s * 6 + gScytheFrames5;
            uint32_t dst = R32(gCurBFItem) + 0x18;
            W32(dst, R32(src));
            W16(dst + 4, RU16(src + 4));
            if (flip) W16(R32(gCurBFItem) + 0x18, R16(R32(gCurBFItem) + 0x18) + 9);
            add = 1;
        }
    } else if (kind == 1) {
        int16_t s = R16(gScytheA + (fl & 0x1f) * 2);
        if (s != 0x3e && (s == 0x3d || s != 0)) {
            uint32_t src = (int32_t)s * 6 + gScytheFrames1;
            uint32_t dst = R32(gCurBFItem) + 0x18;
            W32(dst, R32(src));
            W16(dst + 4, RU16(src + 4));
            if (s != 0x3d) W16(R32(gCurBFItem) + 0x1a, R16(R32(gCurBFItem) + 0x1a) - 0x33);
            if (flip) W16(R32(gCurBFItem) + 0x18, R16(R32(gCurBFItem) + 0x18) + 9);
            add = 1;
        }
    }
    if (add && S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(kind);
    FUN_000299d8();
    return 0;
}

/* 00029fec: is the scythe state dangerous? */
uint32_t GetScytheHitType(int16_t state) {
    uint16_t f = (uint16_t)state & 0x1f;
    if (f == 2 || (f >= 0x10 && f <= 0x11)) return 1;
    return 0;
}

/* 0002a02e: mark the scythe area for redraw */
uint32_t RedrawScythe(int16_t extended) {
    if (S16(FUN_00025362()) == 0) {
        W16(gTrobDir, 1);
        return 0;
    }
    FRAME(f, 8);
    maddr rect = f;
    W32(rect, R32(gScytheRedraw));
    W32(rect + 4, R32(gScytheRedraw + 4));
    if (extended != 0) {
        W16(rect + 6, R16(rect + 6) + 10);
        W16(rect, R16(rect) - 2);
    }
    if (ComputeTrobMoveRect(rect, rect) != 0) MarkBufsForRect(JT_ADDR(FN_MarkMove), rect);
    FRAME_END(f);
    return 0;
}

/* 0002a09a: is the character standing in an extended scythe blade? */
uint32_t FUN_0002a09a(void) {
    int16_t r = 0;
    int16_t c = S16(ReadCell(R16(gCharRoom), R16(gCharCol), R16(gCharRow)));
    if (c == 0xc && S16(ScytheBladeExtended(R16(gCellB))) != 0) return 1;
    if (c != 0xd) return 0;
    ReadCell(R16(gCharRoom), R16(gCharCol) - 1, R16(gCharRow));
    if (S16(ScytheBladeExtended(R16(gCellB))) == 0) return 0;
    if ((int32_t)R16(gColLeft + (int16_t)(R16(gCharCol) * 2)) + 0xe >= (int32_t)R16(gKidXLeft)) r = 1;
    return (uint16_t)r;
}

/* 0002a120: is the blade out (states 3..15)? */
uint32_t ScytheBladeExtended(int16_t state) {
    uint16_t f = (uint16_t)state & 0x1f;
    if (f < 3 || f > 0xf) return 0;
    return 1;
}

/* 0002a15e: start the scythe at cell of room. The original stores the adjusted
 * room/cell into its own argument slots; glue_TriggerScythe writes them back. */
static int16_t s_triggerScytheRoom, s_triggerScytheCell;
uint32_t TriggerScythe(int16_t room, int16_t cell) {
    if (R16(gCellA) == 0xc) {
        ReadCell(room, R16(gCellCol) + 1, R16(gCellRow));
        room = R16(gCellRoom);
        cell = R16(gCellIdx);
    }
    s_triggerScytheRoom = room;
    s_triggerScytheCell = cell;
    uint32_t a = (uint32_t)(uint16_t)room * 0x78 + (int32_t)cell * 4 + R32(gGameState);
    if ((RU16(a + 0x708) & 0x1f) == 0) AddTrob(room, cell, 1, 0xd);
    return 0;
}
void glue_TriggerScythe(void) {
    maddr sp = emu_sp();
    uint32_t r = TriggerScythe((int16_t)rd16(sp), (int16_t)rd16(sp + 2));
    wr16(sp, (uint16_t)s_triggerScytheRoom);
    wr16(sp + 2, (uint16_t)s_triggerScytheCell);
    emu_set_d0(r);
}

/* ---- recorded games ---- */
#define gRecMode        GH(0x103c4a)   /* word: 2 = recording?, 3 = playing, 4 = loaded */
#define gRecOffset      GH(0x103c4c)   /* word: offset of the current frame record */
#define gRecIndex       GH(0x103c4e)   /* word: next event in the frame record */
#define gRecDirs        GH(0x103c50)   /* word[6]: last recorded direction word per character */
#define gRecHandle      GH(0x103c5c)   /* long: 'RECG' handle */
#define gRecId          GH(0x103c60)   /* word: 'RECG' resource id (0xfa..0xfc) */
#define gFrameCount     GH(0x104b66)   /* word */
#define gDirs           GH(0x104b78)   /* word[5]: decoded joystick/keys */
#define gRecLoaded      GH(0x103002)
#define gRecFlag        GH(0x103004)

/* 0002a1f2: end the playback when the recording is over */
uint32_t FUN_0002a1f2(void) {
    int16_t r = -2;
    if (R16(gRecMode) != 3) return (uint16_t)r;
    if (R32(gRecHandle) == 0) return (uint16_t)r;
    uint32_t p = R32(R32(gRecHandle));
    if ((int32_t)R16(p + 2) >= (int32_t)RU16(gFrameCount)) return (uint16_t)r;
    W16(gRecMode, 0);
    if (R32(gRecHandle) != 0) {
        _HPurge(R32(gRecHandle));
        W32(gRecHandle, 0);
    }
    return 0xffff;
}

/* 0002a240: feed the recorded input of this frame to the animated character */
uint32_t seg5_02a240(void) {
    if (R16(gRecMode) != 3) return 0;
    if (R32(gRecHandle) == 0) return 0;
    int16_t who = R16(gChar);
    if (who == 10) who = 5;
    uint32_t base = R32(R32(gRecHandle));
    int16_t off = R16(gRecOffset);
    if (R16(base + 4) > off) {
        uint32_t fr = base + 6 + (int32_t)off;
        if ((int32_t)R16(fr) < (int32_t)RU16(gFrameCount)) fr = IncRecordedGameToNextFrame(base);
        if ((int32_t)R16(fr) == (int32_t)RU16(gFrameCount) && R16(fr + 2) > R16(gRecIndex)) {
            uint32_t ev = (int32_t)R16(gRecIndex) * 4 + fr + 4;
            if (R16(ev) == who) {
                ConvertRecDirToDir(ev + 2);
                W16(gRecDirs + (int16_t)(who * 2), R16(ev + 2));
                W16(gRecIndex, R16(gRecIndex) + 1);
            }
        }
    }
    ConvertRecDirToDir((int32_t)who * 2 + gRecDirs);
    return 0;
}

/* 0002a314 */
uint32_t seg5_02a314(void) {
    if (R16(gRecMode) == 4) W16(GH(0x10302e), -1);
    return 0;
}

/* 0002a324: pack the decoded input into a recorded direction word */
uint32_t ConvertDirToRecDir(uint32_t p) {
    uint16_t d0 = (uint16_t)((R16(gDirs) + 1) << 14);
    d0 |= RU16(p) & 0x3fff;
    W16(p, d0);
    uint16_t d1 = (uint16_t)(((R16(gDirs + 2) + 1) & 3) << 12);
    d1 |= d0 & 0xcfff;
    W16(p, d1);
    d0 = (uint16_t)(((R16(gDirs + 4) + 1) & 3) << 10);
    d0 |= d1 & 0xf3ff;
    W16(p, d0);
    d1 = (uint16_t)(((R16(gDirs + 6) + 1) & 3) << 8);
    d1 |= d0 & 0xfcff;
    W16(p, d1);
    d0 = (uint16_t)(((R16(gDirs + 8) + 2) & 0x1f) << 3);
    d0 |= d1 & 0xff07;
    W16(p, d0);
    return 0;
}

/* 0002a3b2: unpack a recorded direction word into the decoded input */
uint32_t ConvertRecDirToDir(uint32_t p) {
    W16(gDirs, (RU16(p) >> 14) - 1);
    W16(gDirs + 2, ((RU16(p) >> 12) & 3) - 1);
    W16(gDirs + 4, ((RU16(p) >> 10) & 3) - 1);
    W16(gDirs + 6, ((RU16(p) >> 8) & 3) - 1);
    W16(gDirs + 8, ((RU16(p) >> 3) & 0x1f) - 2);
    return 0;
}

/* 0002a420: advance to the next frame record of the recording at base */
uint32_t IncRecordedGameToNextFrame(uint32_t base) {
    uint32_t frames = base + 6;
    uint32_t fr = frames + (int32_t)R16(gRecOffset);
    W16(gRecOffset, R16(gRecOffset) + (uint16_t)(RU16(fr + 2) * 4 + 8));
    W16(gRecIndex, 0);
    int16_t off = R16(gRecOffset);
    fr = frames + (int32_t)off;
    if (R16(gRecMode) == 3 && off > R16(base + 4))
        Terminate(0, fn_address(FN_IncRecordedGameToNextFrame) + (0x2a4a2 - 0x2a420));
    return fr;
}

/* 0002a4be: playing back a recording? */
uint32_t FUN_0002a4be(void) {
    int16_t m = R16(gRecMode);
    if (m == 3 || m == 4) return 1;
    return 0;
}

/* 0002a4da: recording? */
uint32_t seg5_02a4da(void) {
    return R16(gRecMode) == 2 ? 1 : 0;
}

/* 0002a4ec: load the 'RECG' resource of the current recording */
uint32_t FUN_0002a4ec(void) {
    uint32_t h = PoPGetResource(0x52454347, R16(gRecId));
    _HNoPurge(h);
    return h;
}

/* 0002a50a: restart the playback of the loaded recording */
uint32_t StartPlayingRecordedGame(void) {
    int16_t m = R16(gRecMode);
    if (m != 4 && m != 3) return 0;
    W16(gRecMode, 3);
    W16(gRecOffset, 0);
    W16(gRecIndex, 0);
    tc_memset(gRecDirs, 0, 0xc);
    if (R32(gRecHandle) != 0) _HPurge(R32(gRecHandle));
    if (R16(gRecLoaded) != 0) W32(gRecHandle, FUN_0002a4ec());
    W16(gFrameCount, 0);
    SetRndSeed(1);
    Rnd(0x11);
    FUN_00011bce();
    return 0;
}

/* 0002a5c4: stop recording/playback */
uint32_t seg5_02a5c4(void) {
    W16(gRecLoaded, 0);
    W16(gRecMode, 0);
    if (R32(gRecHandle) != 0) {
        _HPurge(R32(gRecHandle));
        W32(gRecHandle, 0);
    }
    return 0;
}

/* 0002a5e4: load the next demo recording ('RECG' 0xfa..0xfc) */
uint32_t seg5_02a5e4(void) {
    W16(gRecMode, 4);
    if (R16(gRecId) == 0) {
        W16(gRecId, 0xfa);
    } else {
        W16(gRecId, R16(gRecId) + 1);
        if (R16(gRecId) >= 0xfd) W16(gRecId, 0xfa);
    }
    uint32_t h = FUN_0002a4ec();
    W16(GH(0x103056), R16(R32(h)));
    W16(gRecLoaded, 1);
    W16(gRecFlag, 0);
    _HPurge(h);
    return 0;
}
