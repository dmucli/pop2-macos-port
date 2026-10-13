/* CODE 7 "Ruins" — level-specific code for the ruins levels: the flying heads, the snakes
 * and the drawing routines of the ruins pieces (back wall, crevices, exit, gate, plates...).
 * Ported from the original 68k code (Ghidra range 0x305b4-0x33fb2); addresses in comments
 * are Ghidra addresses (see docs/PORTING.md and docs/notes/seg07.md). */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* Character records (0x3e bytes): the character being processed, its opponent, the kid */
#define CHR(off)        GH(0x102faa + (off))
#define OPP(off)        GH(0x102f6c + (off))
#define KID(off)        GH(0x102f2e + (off))
#define C_ID     0x00
#define C_FACE   0x02
#define C_X      0x04
#define C_Y      0x06
#define C_TYPE   0x08
#define C_FRAME  0x0a
#define C_COL    0x0c
#define C_ROW    0x0e
#define C_ACTION 0x10
#define C_ROOM   0x16
#define C_1A     0x1a
#define C_ALIVE  0x1c
#define C_LIFE   0x1e
#define C_SEQ    0x28
#define C_34     0x34

#define gGameState      GH(0x102fec)   /* long: pointer to the game state block */
#define gCurBFItem      GH(0x103074)   /* long: background/foreground item being built */
#define gLevelPieces    GH(0x103462)   /* long: the level's piece table ('PIEC') */
#define gCurRoom        GH(0x10303e)   /* word: current room */
#define gSkillTicks     GH(0x104960)   /* word: compared with 20/30 to scale the head's bite chances */

static inline int16_t iabs16(int32_t v) { return (int16_t)(v < 0 ? -v : v); }

/* 000305b4: empty */
uint32_t FUN_000305b4(void) { return 0; }

/* 000305b6: place the head object (kind 2 in the object table) at the kid's neck */
uint32_t AlignHeadOnKidNeck(void) {
    ENTRY_LINK(FN_AlignHeadOnKidNeck, 4, 20);    /* link -4; movem D3-D5/A3-A4 */
    uint32_t obj = GetObjTableType(2, 0);
    if (obj == 0) return 0;
    _HLock(R32(GH(0x105376)));
    uint32_t tab = DEREF(R32(GH(0x105376)));
    int16_t d;
    if (R16(CHR(C_FACE)) != R16(KID(C_FACE))) {
        d = R16(tab + (int32_t)R16(obj + 6) * 4 + 2);
    } else {
        PrinceGetnShapeItemSize(R32(GH(0x103574)), (int16_t)(R16(GH(0x104b40)) + 1), a6 - 4);
        int16_t k = R16(CHR(C_TYPE)) == 7 ? 0x11 : 0x19;
        d = (int16_t)(-R16(tab + (int32_t)R16(obj + 6) * 4 + 2));
        d = (int16_t)(d - (int16_t)(R16(a6 - 2) + k));
    }
    if (R16(CHR(C_TYPE)) == 8) d = (int16_t)(d + 10);
    if (R16(CHR(C_FACE)) == 0) d = (int16_t)-d;
    W16(GH(0x104b3c), R16(obj) + d);
    W16(GH(0x104b3e), R16(obj + 2) + R16(tab + (int32_t)R16(obj + 6) * 4));
    _HUnlock(R32(GH(0x105376)));
    W16(CHR(C_X), R16(GH(0x104b3c)) + 0xcf);
    return 0;
}

/* 000306a8: line of sight (Bresenham in half-cell steps) from the character to its
 * opponent. Returns -1 when blocked, 4 when in the same cell or FUN_0002df86 says no,
 * else the direction: 0 left / 1 right (same row), 2 down / 3 up. */
uint32_t CharCanSeeOpp(void) {
    ENTRY_LINK(FN_CharCanSeeOpp, 0x24, 20);      /* link -0x24; movem D3-D7 */
    int16_t flag = 1;
    if (R16(CHR(C_COL)) == R16(OPP(C_COL)) && R16(CHR(C_ROW)) == R16(OPP(C_ROW))) return 4;
    int16_t y1 = (int16_t)(R16(CHR(C_ROW)) << 1);
    int16_t x1 = (int16_t)(R16(CHR(C_COL)) << 1);
    if (((int32_t)R16(CHR(C_X)) + (int32_t)R16(GH(0x102df2)) - 0xc3) % 0x33 > 0x19) x1++;
    int16_t y2 = (int16_t)((R16(OPP(C_ROW)) << 1) + 1);
    int16_t x2 = (int16_t)(R16(OPP(C_COL)) << 1);
    if (((int32_t)R16(OPP(C_X)) - 0xcf) % 0x33 > 0x19) x2++;
    int16_t dx = iabs16((int32_t)x2 - (int32_t)x1);
    int16_t dy = iabs16((int32_t)y2 - (int32_t)y1);
    int16_t sx = x2 > x1 ? 1 : -1;
    int16_t sy = y2 > y1 ? 1 : -1;
    int16_t err, inc1, inc2;
    int16_t cell;
    if (dx > dy) {
        inc1 = (int16_t)(dy << 1);
        err = (int16_t)(inc1 - dx);
        inc2 = (int16_t)((int16_t)(dy - dx) << 1);
        do {
            x1 = (int16_t)(x1 + sx);
            if ((sx > 0 && (x1 & 1) == 0) || (sx < 0 && (x1 & 1) != 0)) {
                cell = S16(ReadCell(R16(CHR(C_ROOM)), (int16_t)(x1 >> 1), (int16_t)(y1 >> 1)));
                flag = S16(IsWall(cell)) == 0 ? 1 : 0;
            }
            if (flag) {
                if (err < 0) err = (int16_t)(err + inc1);
                else {
                    if ((sy > 0 && (y1 & 1) != 0) || (sy < 0 && (y1 & 1) == 0)) {
                        if (sy < 0) cell = S16(ReadCell(R16(CHR(C_ROOM)), (int16_t)(x1 >> 1), (int16_t)(((int32_t)y1 - 1) >> 1)));
                        else cell = S16(ReadCell(R16(CHR(C_ROOM)), (int16_t)(x1 >> 1), (int16_t)(y1 >> 1)));
                        flag = S16(IsSpace(cell));
                    }
                    y1 = (int16_t)(y1 + sy);
                    err = (int16_t)(err + inc2);
                }
            }
            dx = (int16_t)(dx - 1);
        } while (dx != 0 && flag != 0);
    } else {
        inc1 = (int16_t)(dx << 1);
        err = (int16_t)(inc1 - dy);
        inc2 = (int16_t)((int16_t)(dx - dy) << 1);
        do {
            if ((sy > 0 && (y1 & 1) != 0) || (sy < 0 && (y1 & 1) == 0)) {
                if (sy < 0) cell = S16(ReadCell(R16(CHR(C_ROOM)), (int16_t)(x1 >> 1), (int16_t)(((int32_t)y1 - 1) >> 1)));
                else cell = S16(ReadCell(R16(CHR(C_ROOM)), (int16_t)(x1 >> 1), (int16_t)(y1 >> 1)));
                flag = S16(IsSpace(cell));
            }
            if (flag) {
                y1 = (int16_t)(y1 + sy);
                if (err < 0) err = (int16_t)(err + inc1);
                else {
                    x1 = (int16_t)(x1 + sx);
                    if ((sx > 0 && (x1 & 1) == 0) || (sx < 0 && (x1 & 1) != 0)) {
                        cell = S16(ReadCell(R16(CHR(C_ROOM)), (int16_t)(x1 >> 1), (int16_t)(y1 >> 1)));
                        flag = S16(IsWall(cell)) == 0 ? 1 : 0;
                    }
                    err = (int16_t)(err + inc2);
                }
            }
            dy = (int16_t)(dy - 1);
        } while (dy != 0 && flag != 0);
    }
    if (flag == 0) return (uint16_t)-1;
    if (S16(FUN_0002df86()) == 0) return 4;
    if (R16(CHR(C_ROW)) != R16(OPP(C_ROW))) return sy > 0 ? 2 : 3;
    return sx > 0 ? 1 : 0;
}

/* 00030a44: does the head bite again? Depends on the head's skill (per-room character
 * info +6), the opponent's word +2 and the tables at 0x10532e/0x105346. */
uint32_t ChompAgain(uint32_t p) {
    ENTRY_LINK(FN_ChompAgain, 4, 20);            /* link -4; movem D3-D7 */
    uint32_t info = R32(gGameState) + 0x21a6 + (uint32_t)RU16(CHR(C_ROOM)) * 0xc0 - 0xbe
                    + (uint32_t)((int32_t)R16(CHR(C_ID)) * 0x26);
    int16_t skill = R16(info + 6);
    int16_t adj;
    if (R16(gSkillTicks) > 0x1e) adj = 2;
    else if (R16(gSkillTicks) > 0x14) adj = 1;
    else adj = 0;
    int16_t i2 = (int16_t)((uint16_t)skill << 1);
    int16_t lo = (int16_t)(R16(GH(0x10532e) + i2) - adj);
    int16_t hi = (int16_t)(R16(GH(0x105346) + i2) - adj);
    if (R16(p + 2) < lo) return 1;
    if (R16(p + 2) >= hi) return 0;
    return (uint16_t)S16(Rnd(1));
}

/* 00030b24: set up a head (or the type-0xb character) when it is added as an opponent */
uint32_t HeadAddOpponent(uint32_t p) {
    ENTRY_LINK(FN_HeadAddOpponent, 0, 8);        /* link 0; movem D3/A2 */
    if (R16(p + 0x18) == 0) {
        W16(p + 0x18, 5);
        W16(CHR(C_TYPE), R16(GH(0x102d8a)));
        FUN_00020cca();
    }
    if (R16(CHR(C_TYPE)) == 0xb) {
        if (R16(CHR(C_ALIVE)) < 0) {
            if (R16(p + 0x1c) > 0) {
                W16(p + 8, 0xaf);
                W16(p + 0xa, 0);
                W16(CHR(C_34), 6);
            } else {
                W16(p + 8, 0xaa);
                W16(p + 0xa, 0);
                W16(p + 0x1c, -1);
            }
            W16(CHR(C_1A), 1);
            AddSound(0x80);
        } else {
            W16(CHR(C_1A), 0);
        }
    } else {
        W16(CHR(C_X), R16(GH(0x103478) + (int16_t)(R16(CHR(C_COL)) * 2)) + 0x2f);
        W16(CHR(C_FACE), -1);
        W16(p + 8, 0x82);
        W16(p + 0xa, 0);
        W16(CHR(C_1A), 0);
        W16(p + 0x1c, 0);
    }
    return p;
}

/* 00030bfc: the head's attack program (p: head state {?, dest col, dest row, sees kid}) */
uint32_t HeadAttackProg(uint32_t p) {
    ENTRY_LINK(FN_HeadAttackProg, 0, 24);        /* link 0; movem D3-D7/A2 */
    int16_t seq = R16(CHR(C_SEQ));
    if (seq == 0x8c || seq == 0x98 || seq == 0x9c || seq == 0xa2) return 0;
    if (seq >= 0x93 && seq <= 0x97) { HeadFocusOnNeck(p); return 0; }
    int16_t fr = R16(CHR(C_FRAME));
    if (fr != 0x96 && fr != 0xa5 && fr != 0xa6 && fr != 0x9f && fr != 0xad) return 0;
    int16_t dir = S16(CharCanSeeOpp());
    if (dir == -1) {
        if (R16(p + 6) != 0) {
            W16(p + 6, 0);
            W16(p + 2, R16(OPP(C_COL)));
            W16(p + 4, R16(OPP(C_ROW)));
        }
        HeadGoToDest(p);
        return 0;
    }
    W16(p + 6, 1);
    int16_t dc = iabs16((int32_t)R16(CHR(C_COL)) - (int32_t)R16(OPP(C_COL)));
    int16_t dr = iabs16((int32_t)R16(CHR(C_ROW)) - (int32_t)R16(OPP(C_ROW)));
    if (dr == 0 && (uint16_t)dc < 2) {
        dir = R16(CHR(C_X)) < R16(OPP(C_X)) ? 1 : 0;
        if (dc == 0 || S16(HeadClearInDir(dir)) != 0) {
            if (R16(CHR(C_SEQ)) != 0x93) JumpSeq(0x93);
        } else {
            if (R16(CHR(C_SEQ)) != 0xa6 && S16(YWithinMiddleOfCell(R16(CHR(C_Y)), 0x1e)) != 0) JumpSeq(0xa6);
        }
        return 0;
    }
    if (dc == 0 && dr == 1 && S16(HeadClearInDir(dir)) != 0) {
        int16_t s = R16(CHR(C_ROW)) > R16(OPP(C_ROW)) ? 0x8f : 0x90;
        if (s != R16(CHR(C_SEQ))) JumpSeq(s);
        return 0;
    }
    if (dir == 4) {
        if ((dc | dr) == 0) return 0;
        if (R16(CHR(C_SEQ)) == 0xa6) return 0;
        if (S16(YWithinMiddleOfCell(R16(CHR(C_Y)), 0x1e)) != 0) JumpSeq(0xa6);
        return 0;
    }
    W16(p + 4, R16(OPP(C_ROW)));
    W16(p + 2, R16(OPP(C_COL)));
    if ((dir == 3 || dir == 2) && S16(HeadClearInDir(dir)) == 0)
        dir = R16(CHR(C_X)) < R16(OPP(C_X)) ? 1 : 0;
    if (S16(HeadClearInDir(dir)) == 0) {
        dir = 0xa6;
        seg4_020fb2();
    } else if (dir == 3) {
        dir = 0x8f;
    } else if (dir == 2) {
        dir = 0x90;
    } else {
        dir = 0x8e;
        int32_t t = R16(CHR(C_X)) < R16(OPP(C_X)) ? 1 : 0;   /* abs() of a 0/1 compare */
        if (t > 0xc || R16(CHR(C_SEQ)) != 0x8e) {
            FUN_0003120e();
            FUN_00020cca();
        }
    }
    if (dir != R16(CHR(C_SEQ))) JumpSeq(dir);
    return 0;
}

/* 00030eaa: the head bites the kid: 0x9a, or 0x99 when the kid dies */
uint32_t FUN_00030eaa(void) {
    ENTRY_NOLINK(FN_FUN_00030eaa, 4);            /* move.l D3,-(SP) */
    int16_t r;
    if (S16(DecLifePts(1)) != 0) r = 0x9a;
    else {
        AddSound(0x83);
        seg7_031a04();
        r = 0x99;
    }
    uint32_t d0 = SetTensionLevel(1, -1);
    return (d0 & 0xffff0000u) | (uint16_t)r;
}

/* 00030ee6: can the head move in direction dir (0 left, 1 right, 2 down, 3 up)? */
uint32_t HeadClearInDir(int16_t dir) {
    ENTRY_LINK(FN_HeadClearInDir, 0, 16);        /* link 0; movem D3-D6 */
    int16_t r = 0, c, cell;
    switch (dir) {
    case 0:
        if (R16(CHR(C_COL)) < -1) break;
        if (S16(FUN_00031cba()) != 0) break;
        c = S16(GetCharCol(S16(AddCharX(0x1a))));
        if (c == R16(CHR(C_COL))) { r = 1; break; }
        {
            int16_t col = R16(CHR(C_COL));
            cell = S16(ReadCell(R16(CHR(C_ROOM)), (int16_t)(col - 1), R16(CHR(C_ROW))));
        }
        if (S16(IsWall(cell)) != 0) break;
        if (cell == 4 && S16(seg4_0235de()) != 0) break;
        r = 1;
        break;
    case 1:
        if (R16(CHR(C_COL)) > 10) break;
        if (S16(FUN_00031cba()) != 0) break;
        c = S16(GetCharCol(S16(AddCharX(0x1a))));
        if (c != R16(CHR(C_COL))) {
            int16_t col = R16(CHR(C_COL));
            cell = S16(ReadCell(R16(CHR(C_ROOM)), (int16_t)(col + 1), R16(CHR(C_ROW))));
            if (S16(IsWall(cell)) != 0) break;
            if (cell == 4 && S16(seg4_0235de()) != 0) break;
        }
        cell = S16(ReadCell(R16(CHR(C_ROOM)), R16(CHR(C_COL)), R16(CHR(C_ROW))));
        if (cell == 4 && S16(seg4_0235de()) != 0) break;
        r = 1;
        break;
    case 2:
        if (R16(CHR(C_ROW)) == 2) break;
        if (S16(XWithinMiddleOfCell(R16(CHR(C_X)), 0xc)) == 0) break;
        cell = S16(ReadCell(R16(CHR(C_ROOM)), R16(CHR(C_COL)), R16(CHR(C_ROW))));
        r = S16(IsSpace(cell));
        break;
    case 3:
        if (R16(CHR(C_ROW)) == 0) break;
        if (S16(XWithinMiddleOfCell(R16(CHR(C_X)), 0xc)) == 0) break;
        cell = S16(ReadCell(R16(CHR(C_ROOM)), R16(CHR(C_COL)), (int16_t)(R16(CHR(C_ROW)) - 1)));
        r = S16(IsSpace(cell));
        break;
    }
    return (uint16_t)r;
}

/* 000310c6: the head stops attacking the kid */
uint32_t HeadDoneAttackingKid(uint32_t p) {
    ENTRY_LINK(FN_HeadDoneAttackingKid, 0, 4);   /* link 0; move.l D3 */
    if (R16(OPP(C_ALIVE)) >= 6) {
        HeadGetPeaceful(p);
    } else if (R16(CHR(C_SEQ)) != 0xa6) {
        FUN_00020cca();
        JumpSeq(0xa6);
    }
    W16(CHR(C_34), 0);
    return 0;
}

/* 0003111e: collision of a head with a barrier: pushes it back and hurts it.
 * Returns the sequence to start (0x9d, 0xa4) or -1. */
uint32_t HeadEnemyCollide(int16_t mindist) {
    ENTRY_LINK(FN_HeadEnemyCollide, 0, 12);      /* link 0; movem D3-D5 */
    int16_t r = -1;
    if (R16(CHR(C_34)) == 1) return (uint16_t)r;
    int16_t d = S16(GetBarrDistFromFace(R16(GH(0x104b50)), R16(GH(0x104b56)), R16(GH(0x104b58))));
    if (d >= 8) return (uint16_t)r;
    if (d < mindist) return (uint16_t)r;
    if (d >= 5) return (uint16_t)r;
    if ((int32_t)d < -(int32_t)R16(GH(0x104a9e))) return (uint16_t)r;
    W16(CHR(C_X), AddCharX((int16_t)(-0x16 - d)));
    if (R16(CHR(C_ALIVE)) < 0 && R16(CHR(C_SEQ)) != 0x99) return 0x9d;
    if (R16(CHR(C_SEQ)) == 0xa4) return (uint16_t)r;
    if (RU16(CHR(C_FRAME)) > 0xbf) {
        JumpSeq(0x91);
    } else {
        r = 0xa4;
        int16_t c = S16(FUN_00021600());
        W16(CHR(C_X), R16(GH(0x103478) + (int16_t)(c * 2)) + 0x16);
        if (R16(CHR(C_FACE)) == -1) W16(CHR(C_X), R16(CHR(C_X)) + 0x33);
    }
    DecLifePts(100);
    return (uint16_t)r;
}

/* 0003120e: turn the head towards its opponent */
uint32_t FUN_0003120e(void) {
    ENTRY_NOLINK(FN_FUN_0003120e, 4);            /* move.l D3,-(SP) */
    if (R16(CHR(C_COL)) < R16(OPP(C_COL))) {
        if (R16(CHR(C_FACE)) == 0) return 0;
        W16(CHR(C_X), AddCharX(-0xc));
        W16(CHR(C_FACE), 0);
        seg4_020fb2();
    } else {
        if (R16(CHR(C_FACE)) == -1) return 0;
        W16(CHR(C_X), AddCharX(-0xc));
        W16(CHR(C_FACE), -1);
        seg4_020fb2();
    }
    return 0;
}

/* 0003125e: the head's flee program */
uint32_t HeadFleeProg(uint32_t p) {
    ENTRY_LINK(FN_HeadFleeProg, 0, 8);           /* link 0; movem D3/A2 */
    int16_t seq = R16(CHR(C_SEQ));
    if (seq == 0x99) {
        if (R16(CHR(C_34)) == 2) {
            W16(CHR(0x32), 0);
            JumpSeq(0x9f);
        }
        return 0;
    }
    if (seq == 0x9f) {
        if (R16(CHR(C_COL)) == R16(p + 2) && S16(XWithinMiddleOfCell(R16(CHR(C_X)), 3)) != 0) {
            if (R16(CHR(C_ROW)) == R16(OPP(C_ROW)) &&
                iabs16((int32_t)R16(CHR(C_COL)) - (int32_t)R16(OPP(C_COL))) < 3) {
                HeadGetAngry(p);
            } else {
                JumpSeq(0xa0);
                uint32_t info = R32(gGameState) + 0x21a6 + (uint32_t)RU16(CHR(C_ROOM)) * 0xc0 - 0xbe
                                + (uint32_t)((int32_t)R16(CHR(C_ID)) * 0x26);
                uint16_t skill = RU16(info + 6);
                W16(p + 6, R16(GH(0x10535e) + (int16_t)(skill << 1)));
            }
        }
    } else {
        int angry = 0;
        if (R16(CHR(C_ROW)) == R16(OPP(C_ROW)) &&
            iabs16((int32_t)R16(CHR(C_COL)) - (int32_t)R16(OPP(C_COL))) < 3 &&
            R16(OPP(0x1e)) != 0) {
            if (R16(OPP(0x28)) != 0x46) angry = 1;
            else if (R16(OPP(C_FRAME)) != 0 && RU16(OPP(C_FRAME)) < 0xdf) angry = 1;
        }
        if (angry) {
            HeadGetAngry(p);
        } else {
            W16(p + 6, R16(p + 6) - 1);
            if (R16(p + 6) == 0) {
                int see = 0;
                if (R16(OPP(0x1e)) != 0 &&
                    (R16(OPP(0x28)) != 0x46 || (R16(OPP(C_FRAME)) != 0 && RU16(OPP(C_FRAME)) < 0xdf)) &&
                    S16(CharCanSeeOpp()) != -1) see = 1;
                if (see) {
                    HeadGetAngry(p);
                    JumpSeq(0xa2);
                } else {
                    HeadGetPeaceful(p);
                    JumpSeq(0xa3);
                }
            }
        }
    }
    if (R16(p + 8) == 0 && S16(PrinceSndOn(0x83)) == 0) {
        W16(p + 8, 1);
        AddSong(0x88);
    }
    return 0;
}

/* 0003143c: the head has reached the kid: bite his neck */
uint32_t HeadFocusOnNeck(uint32_t p) {
    ENTRY_LINK(FN_HeadFocusOnNeck, 0, 16);       /* link 0; movem D3-D5/A2 */
    int16_t seq = R16(CHR(C_SEQ));
    if (seq != 0x93 && seq != 0x90 && seq != 0x8f) return 0;
    int16_t lo, hi;
    if (R16(OPP(C_1A)) == 1) { lo = 0; hi = 0x3c; } else { lo = 5; hi = 0x44; }
    int16_t d = S16(seg4_0224be());
    if (R16(CHR(C_ROW)) == R16(OPP(C_ROW)) && d >= lo && d <= hi) {
        seg4_020fb2();
        JumpSeq((int16_t)(S16(Rnd(1)) + 0x96));
        W16(p, 3);
        W16(p + 2, 1);
        return 0;
    }
    lo = R16(OPP(C_1A)) == 1 ? -0x10 : -0xb;
    if (R16(CHR(C_SEQ)) == 0x93) {
        if (d < lo) {
            seg4_020fb2();
            JumpSeq(0x9c);
        } else if (R16(CHR(C_ROW)) != R16(OPP(C_ROW))) {
            JumpSeq(0x8e);
        }
    }
    if ((int32_t)d - 0x65 <= (int32_t)hi && R16(CHR(C_ROW)) == R16(OPP(C_ROW)) && R16(CHR(C_SEQ)) != 0x90)
        AddSound(0x86);
    return 0;
}

/* 00031538: the head becomes hostile */
uint32_t HeadGetAngry(uint32_t p) {
    ENTRY_LINK(FN_HeadGetAngry, 0, 8);           /* link 0; movem D3/A2 */
    W16(p, 1);
    W16(p + 4, R16(OPP(C_ROW)));
    W16(p + 2, R16(OPP(C_COL)));
    W16(p + 6, 1);
    JumpSeq(0x8c);
    seg4_020fb2();
    FUN_0003120e();
    W16(CHR(0x32), 3);
    W16(CHR(C_1A), 1);
    if (R16(GH(0x102fa6)) == -1) {
        seg4_022516();
        DrawOppMeter(R16(CHR(C_ID)), R16(CHR(C_LIFE)));
        LoadShad(R16(CHR(C_ID)));
        W16(GH(0x102fa6), R16(CHR(C_ID)));
    }
    AddSound(0x83);
    return 0;
}

/* 000315c2: the head calms down and returns to its column */
uint32_t HeadGetPeaceful(uint32_t p) {
    ENTRY_LINK(FN_HeadGetPeaceful, 0, 4);        /* link 0; move.l D3 */
    W16(p, 0);
    W16(CHR(C_1A), 0);
    seg4_020fb2();
    JumpSeq(0x8b);
    int16_t col = S16(GetCharCol(S16(seg4_021542())));
    W16(CHR(C_COL), col);
    W16(CHR(C_X), R16(GH(0x103478) + (int16_t)(col * 2)) + 0x2f);
    FUN_00020cca();
    W16(CHR(0x32), 0);
    return 0;
}

/* 0003162a: move the head towards its destination cell (p+2 col, p+4 row);
 * returns nonzero while it can move */
uint32_t HeadGoToDest(uint32_t p) {
    ENTRY_LINK(FN_HeadGoToDest, 0, 16);          /* link 0; movem D3-D5/A2 */
    int16_t ok = 0;
    if (R16(p + 2) != R16(CHR(C_COL)) || R16(p + 4) != R16(CHR(C_ROW))) {
        int16_t dr = R16(p + 4), s = R16(CHR(C_ROW));
        if (dr != s) {
            if (dr > s) { dr = 2; s = 0x90; } else { dr = 3; s = 0x8f; }
            ok = S16(HeadClearInDir(dr));
            if (ok != 0 && R16(CHR(C_SEQ)) != s) JumpSeq(s);
        }
        if (ok == 0 && R16(p + 2) != R16(CHR(C_COL))) {
            int16_t d = R16(p + 2) > R16(CHR(C_COL)) ? 1 : 0;
            ok = S16(HeadClearInDir(d));
            if (ok == 0) {
                if (d == 1 && R16(GH(0x1037e4)) < 0x19) ok = 1;
                else if (d == 0 && R16(GH(0x1037e4)) > 0x19) ok = 1;
            }
            if (ok != 0 && R16(CHR(C_SEQ)) != 0x8e) JumpSeq(0x8e);
        }
    }
    if (ok == 0) HeadGetPeaceful(p);
    return (uint16_t)ok;
}

/* 00031714: the head is locked on the kid's neck: keep biting or let go */
uint32_t HeadLockedProg(uint32_t p) {
    ENTRY_LINK(FN_HeadLockedProg, 0, 16);        /* link 0; movem D3-D5/A2 */
    int16_t done = 0;
    int16_t seq = R16(CHR(C_SEQ));
    int chomp = 0;
    if (seq == 0x96 && R16(CHR(C_FRAME)) == 0xa7 && R16(CHR(C_34)) != 1) chomp = 1;
    else if (seq == 0x97 && R16(CHR(C_FRAME)) == 0xa6) chomp = 1;
    if (chomp) {
        if (S16(ChompAgain(p)) != 0) {
            if (R16(OPP(C_ALIVE)) < 0) {
                W16(p + 2, R16(p + 2) + 1);
                JumpSeq((int16_t)((R16(CHR(C_SEQ)) == 0x96 ? 1 : 0) + 0x96));
            } else {
                uint32_t info = GetOppInfoIdx(R16(CHR(C_ROOM)), R16(CHR(C_ID)));
                if (info != 0 && RU16(info + 6) >= 0xb) JumpSeq(0xa5);
            }
        } else {
            JumpSeq(0x98);
            W16(CHR(C_X), AddCharX(-0xc));
            done = 1;
        }
    }
    if (done == 0) {
        seq = R16(CHR(C_SEQ));
        if (seq != 0x9e && seq != 0x9d && seq != 0x9b) return 0;
    }
    W16(p, 1);
    W16(p + 2, R16(OPP(C_COL)));
    W16(p + 4, R16(OPP(C_ROW)));
    return 0;
}

/* 00031816: empty */
uint32_t FUN_00031816(void) { return 0; }

/* 00031818: idle head: wander at random (0x83-0x8a sequences) */
uint32_t FUN_00031818(void) {
    ENTRY_NOLINK(FN_FUN_00031818, 8);            /* movem D3-D4 */
    if (R16(CHR(C_FRAME)) != 0x96) return 0;
    int16_t dir = S16(Rnd(3));
    if (dir == 3) {
        int16_t seq = R16(CHR(C_SEQ));
        if (seq >= 0x83 && seq <= 0x85) dir = 0;
        else {
            seq = R16(CHR(C_SEQ));
            if (seq >= 0x86 && seq <= 0x88) dir = 1;
            else dir = -1;
        }
    } else if (dir == 2) {
        dir = (int16_t)(dir + S16(Rnd(1)));
    }
    if (dir == -1) return 0;
    if (S16(HeadClearInDir(dir)) == 0) return 0;
    int16_t s;
    if (dir == 3) s = 0x89;
    else if (dir == 2) s = 0x8a;
    else if (dir == 0) s = (int16_t)(S16(Rnd(2)) + 0x83);
    else s = (int16_t)(S16(Rnd(2)) + 0x86);
    JumpSeq(s);
    return 0;
}

/* is the head's opponent (the kid) a valid target? */
static int opp_targetable(void) {
    if (R16(OPP(C_LIFE)) == 0) return 0;
    if (R16(OPP(C_SEQ)) != 0x46) return 1;
    if (R16(OPP(C_FRAME)) == 0) return 0;
    return RU16(OPP(C_FRAME)) < 0xdf;
}

/* 000318d4: per-frame control of a flying head: dispatch on its program
 * (0 idle, 1 attack, 2 flee, 3 locked on the neck) */
uint32_t seg7_0318d4(void) {
    ENTRY_NOLINK(FN_seg7_0318d4, 12);            /* movem D3/A2-A3 */
    if (R16(CHR(C_ALIVE)) >= 0) return 0;
    uint32_t info = GetOppInfoIdx(R16(CHR(C_ROOM)), R16(CHR(C_ID)));
    if (info == 0) return 0;
    uint32_t st = info + 0x1c;
    FUN_00020d42();
    seg4_02055e();
    int16_t fr = R16(GH(0x102dee));
    if (fr != -1) {
        W16(GH(0x104b40), fr);
        GetCharEdges();
    }
    switch (R16(st)) {
    case 0:
        if (opp_targetable()) {
            int16_t f = R16(CHR(C_FRAME));
            if ((f == 0x96 || f == 0xa5 || f == 0xa6 || f == 0x9f || f == 0xad) && S16(CharCanSeeOpp()) != -1) {
                HeadGetAngry(st);
                break;
            }
        }
        FUN_00031818();
        break;
    case 1:
        if (opp_targetable()) HeadAttackProg(st);
        else HeadDoneAttackingKid(st);
        break;
    case 2:
        HeadFleeProg(st);
        break;
    case 3:
        if (opp_targetable()) HeadLockedProg(st);
        else HeadDoneAttackingKid(st);
        break;
    }
    FUN_00020e5e();
    return 0;
}

/* 00031a04: the head was hurt: maybe flee to a column at least 3 cells away.
 * Returns nonzero when it flees. */
uint32_t seg7_031a04(void) {
    ENTRY_NOLINK(FN_seg7_031a04, 24);            /* movem D3-D7/A2 */
    int16_t ok;
    int16_t room = R16(CHR(C_ROOM));
    uint32_t a = (uint32_t)((int32_t)R16(CHR(C_ID)) * 0x26) + (uint32_t)(uint16_t)room * 0xc0 + R32(gGameState);
    uint16_t skill = RU16(a + 0x20ee);
    if (S16(IsAbleToFleeInRoom(room)) == 0) return 0;
    if (S16(Rnd((int16_t)((uint16_t)(skill / 3) + 1))) != 0) return 0;
    int left = R16(CHR(C_FACE)) == 0;
    if (!(left && R16(CHR(C_COL)) >= 3)) {
        if (R16(CHR(C_FACE)) != -1) return 0;
        if ((int32_t)R16(CHR(C_COL)) + 3 > 10) return 0;
    }
    int16_t end = left ? -1 : 10;
    ok = 1;
    int16_t c = R16(CHR(C_COL));
    if (R16(CHR(C_COL)) != end) {
        do {
            int16_t cell = S16(ReadCell(R16(CHR(C_ROOM)), c, R16(CHR(C_ROW))));
            if (S16(GetBarrCode(cell)) != 0 && (cell != 4 || S16(seg4_0235de()) != 0)) ok = 0;
            c = (int16_t)(c + R16(GH(0x10346c) + (int16_t)(R16(CHR(C_FACE)) * 2)));
        } while (c != end && ok != 0);
    }
    if (ok == 0) {
        c = (int16_t)(c + (int16_t)(R16(GH(0x103468) + (int16_t)(R16(CHR(C_FACE)) * 2)) << 1));
        if (iabs16((int32_t)c - (int32_t)R16(CHR(C_COL))) >= 3) ok = 1;
    } else {
        c = (int16_t)(c + R16(GH(0x103468) + (int16_t)(R16(CHR(C_FACE)) * 2)));
    }
    if (ok == 0) return 0;
    W16(CHR(C_1A), 0);
    W16(CHR(0x32), 1);
    uint32_t info = GetOppInfoIdx(R16(CHR(C_ROOM)), R16(CHR(C_ID)));
    W16(info + 0x1c, 2);
    W16(info + 0x1e, c);
    W16(info + 0x20, R16(CHR(C_ROW)));
    W16(info + 0x22, -1);
    W16(info + 0x24, 0);
    seg4_022516();
    DrawOppMeter(R16(CHR(C_ID)), 0);
    LoadShad(R16(CHR(C_ID)));
    if (R16(GH(0x102fa6)) == R16(CHR(C_ID))) W16(GH(0x102fa6), -1);
    return (uint16_t)ok;
}

/* 00031bcc: can a head flee in this room (more than one character, none fleeing or of kind 8)? */
uint32_t IsAbleToFleeInRoom(int16_t room) {
    ENTRY_LINK(FN_IsAbleToFleeInRoom, 0, 20);    /* link 0; movem D3-D6/A2 */
    int16_t n = room != 0 ? R16(R32(gGameState) + (uint32_t)(uint16_t)room * 0xc0 + 0x20e6) : 0;
    if (n <= 1) return 0;
    int16_t ok = 1, i = 0;
    if (n <= 0) return (uint16_t)ok;
    for (;;) {
        uint32_t info = GetOppInfoIdx(room, i);
        ok = (R16(info + 0x1c) == 2 || R16(info + 0x18) == 8) ? 0 : 1;
        i++;
        if (ok == 0 || i >= n) break;
    }
    return (uint16_t)ok;
}

/* 00031c68: is that head locked on the kid's neck (program 3)? */
uint32_t IsHeadLockedOn(int16_t room, int16_t idx) {
    ENTRY_LINK(FN_IsHeadLockedOn, 0, 8);         /* link 0; movem D3/A2 */
    uint32_t info = GetOppInfoIdx(room, idx);
    if (info == 0) return 0;
    return R16(info + 0x1c) == 3 ? 1 : 0;
}

/* 00031cba: is the head moving down (0x90) / up (0x8f) past the middle of its row? */
uint32_t FUN_00031cba(void) {
    int16_t lim = (int16_t)(R16(CHR(C_ROW)) * 0x78 + 0x49);
    if (R16(CHR(C_SEQ)) == 0x90) return lim < R16(CHR(C_Y)) ? 1 : 0;
    if (R16(CHR(C_SEQ)) == 0x8f) return lim > R16(CHR(C_Y)) ? 1 : 0;
    return 0;
}

/* 00031d18: is x within w pixels of the middle of its column? (the original stores the
 * position within the cell into its own argument slot: see glue) */
static int16_t xmid_pos(int16_t x) { return (int16_t)(((int32_t)x - 0xe5) % 0x33); }
uint32_t XWithinMiddleOfCell(int16_t x, int16_t w) {
    int16_t m = xmid_pos(x);
    if (0x19 - (int32_t)w > (int32_t)m) return 0;
    return (int32_t)w + 0x19 >= (int32_t)m ? 1 : 0;
}
void glue_XWithinMiddleOfCell(void) {
    maddr sp = emu_sp();
    int16_t x = (int16_t)rd16(sp), w = (int16_t)rd16(sp + 2);
    wr16(sp, (uint16_t)xmid_pos(x));
    emu_set_d0(XWithinMiddleOfCell(x, w));
}

/* 00031d78: is y within h pixels of the middle of its row? (same slot write) */
static int16_t ymid_pos(int16_t y) { return (int16_t)(((int32_t)y - 5) % 0x78); }
uint32_t YWithinMiddleOfCell(int16_t y, int16_t h) {
    int16_t m = ymid_pos(y);
    if (0x3c - (int32_t)h > (int32_t)m) return 0;
    return (int32_t)h + 0x3c >= (int32_t)m ? 1 : 0;
}
void glue_YWithinMiddleOfCell(void) {
    maddr sp = emu_sp();
    int16_t y = (int16_t)rd16(sp), h = (int16_t)rd16(sp + 2);
    wr16(sp, (uint16_t)ymid_pos(y));
    emu_set_d0(YWithinMiddleOfCell(y, h));
}

/* 00031dd4: per-frame control of a snake: come out of its hole, bite, go back in */
uint32_t FUN_00031dd4(void) {
    ENTRY_NOLINK(FN_FUN_00031dd4, 8);            /* movem D3/A2 */
    if (R16(CHR(C_ALIVE)) >= 0) return 0;
    uint32_t info = GetOppInfoIdx(R16(CHR(C_ROOM)), R16(CHR(C_ID)));
    if (info == 0) return 0;
    FUN_00020d42();
    if (R16(info + 0x1c) < 0) {
        if (S16(FUN_00032376()) != 0) FUN_00031e2e();
        else SnakeGoIntoHole(info);
    } else {
        SnakeComeOutOfHole(info);
    }
    FUN_00020e5e();
    return 0;
}

/* a cell in front is solid floor for a snake (shared test of the snake routines):
 * floor (but a loose floor 4 only with spec&0xb8 above 0x38), or piece 7 with spec bit 7 */
static int snake_floor(int16_t c) {
    if (S16(IsFloor(c)) != 0) {
        if (c != 4) return 1;
        if ((RU16(GH(0x104b52)) & 0xb8) > 0x38) return 1;
    }
    return c == 7 && (RU16(GH(0x104b52)) & 0x80) != 0;
}

/* 00031e2e: the snake out of its hole: strike at the kid when he is in range */
uint32_t FUN_00031e2e(void) {
    ENTRY_NOLINK(FN_FUN_00031e2e, 16);           /* movem D3-D6 */
    int16_t hit = 0;
    int16_t d = S16(seg4_0224be());
    uint16_t of;
    if (d >= 0x1f && d <= 0x47) {
        int16_t f = R16(CHR(C_FRAME));
        if (f == 0x116 || f == 0x117) hit = 1;
        else if (R16(CHR(C_SEQ)) == 0xb0) {
            of = RU16(OPP(C_FRAME));
            if ((of >= 1 && of <= 0xe) || (of >= 0x31 && of <= 0x38)) hit = 1;
        }
        if (!hit) {
            int16_t s = R16(CHR(C_SEQ));
            if (s != 0xb0 && s != 0xad && d >= 0x14 && d <= 0x3c) {
                JumpSeq(0xb0);
                AddSound(0x80);
            }
        }
    } else {
        if (R16(CHR(C_SEQ)) == 0xb0) {
            of = RU16(OPP(C_FRAME));
            if (((of >= 1 && of <= 0xe) || (of >= 0x31 && of <= 0x38)) && d > 0x1f && d < 0x4f) hit = 1;
        }
        if (!hit && R16(CHR(C_SEQ)) == 0xb0) {
            of = RU16(OPP(C_FRAME));
            if (of >= 0x87 && of <= 0x95 && d > 0x1f && d < 0x47) hit = 1;
        }
        if (!hit) {
            int16_t f = R16(CHR(C_FRAME));
            if ((f == 0x116 || f == 0x117) && (d < 0x1f || d > 0x4d)) {
                int16_t c = S16(GetCellsInFront(1));
                if (snake_floor(c)) JumpSeq(0xb1);
            }
        }
    }
    if (hit && R16(CHR(C_SEQ)) != 0xad) {
        W16(CHR(C_X), AddCharX((int16_t)(d - 0x3a)));
        JumpSeq(0xad);
    }
    return 0;
}

/* 00031fae: count down, then the snake comes out of its hole facing the kid */
uint32_t SnakeComeOutOfHole(uint32_t p) {
    ENTRY_LINK(FN_SnakeComeOutOfHole, 0, 24);    /* link 0; movem D3-D7/A2 */
    W16(p + 0x1c, R16(p + 0x1c) - 1);
    if (R16(p + 0x1c) > 0) return 0;
    W16(p + 0x1c, -1);
    int16_t lo = S16(Get1stWallColInDir(R16(CHR(C_ROOM)), R16(CHR(C_COL)), R16(CHR(C_ROW)), -1));
    int16_t hi = S16(Get1stWallColInDir(R16(CHR(C_ROOM)), R16(CHR(C_COL)), R16(CHR(C_ROW)), 1));
    int16_t c = S16(FUN_00021076());
    int16_t col;
    if (R16(OPP(C_ROOM)) == R16(CHR(C_ROOM)) && R16(OPP(C_ROW)) == R16(CHR(C_ROW)) &&
        (col = R16(CHR(C_COL)), col > lo && col < hi)) {
        if (R16(CHR(C_COL)) > R16(OPP(C_COL))) W16(CHR(C_FACE), -1);
        else if (S16(IsWall(c)) == 0 || snake_floor(c)) W16(CHR(C_FACE), 0);
        else W16(CHR(C_FACE), -1);
    } else {
        W16(CHR(C_FACE), ~RU16(CHR(C_FACE)));
    }
    if (S16(IsWall(c)) != 0 && !snake_floor(c)) {
        JumpSeq(0xaa);
        W16(CHR(C_X), AddCharX(0x3d));
    } else {
        JumpSeq(0xac);
    }
    W16(CHR(C_34), 0);
    return 0;
}

/* 00032102: the snake goes back into its hole (or strikes when the kid is dead) */
uint32_t SnakeGoIntoHole(uint32_t p) {
    ENTRY_LINK(FN_SnakeGoIntoHole, 0, 12);       /* link 0; movem D3-D5 */
    if (R16(OPP(C_ALIVE)) >= 0) {
        if (R16(CHR(C_SEQ)) == 0xb0) return 0;
        int16_t f = R16(CHR(C_FRAME));
        if (f == 0x116 || f == 0x117) return 0;
        JumpSeq(0xb0);
        return 0;
    }
    int16_t seq = R16(CHR(C_SEQ));
    if (seq != 0xab && seq != 0xac && R16(CHR(C_34)) != 9) {
        FUN_00021076();
        if ((int32_t)(uint16_t)(RU16(GH(0x104b52)) & 0xf00) >> 8 == 0xf) {
            int16_t m = (int16_t)(((int32_t)R16(CHR(C_X)) - 0xe5) % 0x33);
            if (m < 0x10 || m > 0x1c) return 0;
            int16_t dx = (int16_t)(0x16 - m);
            if (R16(CHR(C_FACE)) == -1) dx = (int16_t)-dx;
            else dx = (int16_t)(dx - 2);
            W16(CHR(C_X), AddCharX(dx));
            JumpSeq(0xab);
            return 0;
        }
        int16_t s = R16(CHR(C_SEQ));
        int16_t f = R16(CHR(C_FRAME));
        if (s == 0xb0 || s == 0xa8 || f == 0x116 || f == 0x117) {
            int16_t c = S16(GetCellsInFront(1));
            if (snake_floor(c)) JumpSeq(0xb1);
            return 0;
        }
        int16_t c = S16(GetCellsInFront(1));
        if (snake_floor(c)) return 0;
        if (S16(IsWall(c)) != 0) {
            if (c == 7) JumpSeq(0xa8);
            else if (R16(CHR(C_FACE)) == 0) W16(CHR(C_34), 9);
            else {
                f = R16(CHR(C_FRAME));
                if (f == 0xfe || f == 0xee) JumpSeq(0xa8);
            }
        } else {
            if (S16(seg4_021562()) < 0x19) JumpSeq(0xa8);
        }
        return 0;
    }
    if (R16(CHR(C_34)) == 9) {
        int16_t c = S16(FUN_00021076());
        int16_t d = S16(seg4_021562());
        if (S16(IsWall(c)) != 0 && d < 0xc) {
            W16(CHR(C_FRAME), 0x108);
            JumpSeq(0xa8);
        }
    }
    if (R16(CHR(C_FRAME)) == 0x108) {
        W16(CHR(C_34), 6);
        if (R16(CHR(0x32)) == 2) W16(p + 0x1c, 6);
        else W16(p + 0x1c, S16(Rnd(0x3c)) + 0x24);
    }
    return 0;
}

/* 00032376: is the snake (out of its hole) facing the kid? */
uint32_t FUN_00032376(void) {
    int16_t r = 0;
    if (R16(CHR(C_SEQ)) != 0xab && R16(CHR(0x32)) >= 2) {
        if (R16(CHR(C_FACE)) == 0 && R16(CHR(C_COL)) <= R16(OPP(C_COL))) r = 1;
        else if (R16(CHR(C_FACE)) == -1 && R16(CHR(C_COL)) >= R16(OPP(C_COL))) r = 1;
    }
    return (uint16_t)r;
}

/* 000323b4: save the snake's state into its per-room info record */
uint32_t SnakeUpdateOppInfo(uint32_t p) {
    ENTRY_LINK(FN_SnakeUpdateOppInfo, 0, 12);    /* link 0; movem D3-D4/A2 */
    if (R16(CHR(C_SEQ)) == 0xab || R16(CHR(C_34)) == 9) {
        W16(CHR(C_34), 6);
        W16(p + 0x1c, S16(Rnd(0x3c)) + 0x24);
        return 0;
    }
    int16_t c = S16(GetCellsInFront(1));
    if (snake_floor(c)) return 0;
    int16_t s = R16(CHR(C_SEQ));
    if (s == 0xb0 || s == 0xa8 || s == 0xaa) {
        W16(p + 4, ~RU16(CHR(C_FACE)));
        W16(p + 2, AddCharX(-0x33));
    }
    return 0;
}

/* ---- ruins pieces: build BF items through gCurBFItem ---- */
#define ITEM()          R32(gCurBFItem)
#define PIECES()        R32(gLevelPieces)
#define gNoTopRow       GH(0x1037ce)   /* word: drawing the row above the room */
#define gCellRect       GH(0x103a00)   /* Rect: clip rect of the cell being drawn */
#define gRoomRect       GH(0x103014)   /* Rect */
#define gSectRectProc   GH(0x1060a8)   /* long: SectRect (Pascal) */
#define gUnionRectProc  GH(0x1060a4)   /* long: UnionRect (Pascal) */

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
#define SECTRECT(a, b, dst)   pascal_ptr3(R32(gSectRectProc), 1, (a), (b), (dst))
#define UNIONRECT(a, b, dst)  pascal_ptr3(R32(gUnionRectProc), 0, (a), (b), (dst))

/* item +0x1a/+0x1c = the x/y words at piece table offset off */
static void item_xy(uint32_t off) {
    W16(ITEM() + 0x1a, RU16(PIECES() + off));
    W16(ITEM() + 0x1c, RU16(PIECES() + off + 2));
}
/* item +0x18.. = 6 bytes (shape, x, y) */
static void item_copy6(uint32_t src) {
    uint32_t d = ITEM() + 0x18;
    W32(d, R32(src));
    W16(d + 4, RU16(src + 4));
}
static void add_item(int16_t reset) {
    if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(reset);
}

/* 00032476: the ruins back wall behind a piece (shape = info & 0x1f, variant = (info>>8)&15);
 * skipped when the wall image is already in the back table at that place */
uint32_t DrawRuinBackWall(void) {
    ENTRY_LINK(FN_DrawRuinBackWall, 0x14, 28);   /* link -0x14; movem D3-D7/A2-A3 */
    uint32_t item = ITEM();
    uint16_t info = RU16(item + 0x16);
    W16(a6 - 4, info);
    int16_t var = (int16_t)((uint16_t)(info & 0xf00) >> 8);
    int16_t shape = (int16_t)(RU16(a6 - 4) & 0x1f);
    if (shape == 0) return 0;
    int16_t notfound;
    uint32_t img;
    if (R16(gNoTopRow) != 0) {
        notfound = 0;
        img = 0;
    } else {
        img = GetBackTableImgRow(shape, (int16_t)(R16(item + 0xe) - 1), R16(item + 0x10));
        if (img == 0 || !SECTRECT(gCellRect, img + 0x1e, a6 - 0x10)) {
            img = 0;
            notfound = 1;
        } else {
            notfound = 0;
            UNIONRECT(gCellRect, img + 0x1e, img + 0x1e);
            if (shape >= 1 && shape <= 5) {
                item = ITEM();
                img = GetBackTableImgRow(7, (int16_t)(R16(item + 0xe) - 1), R16(item + 0x10));
                if (img != 0 && SECTRECT(img + 0x1e, gCellRect, a6 - 0x10))
                    UNIONRECT(gCellRect, img + 0x1e, img + 0x1e);
            }
        }
    }
    if (img != 0) return 0;
    if (notfound == 0 && var != 0) {
        item = ITEM();
        if (!(R16(item + 0xe) == -1 && (uint16_t)var <= 3)) {
            if (R16(item + 0x10) != 3) return 0;
        }
    }
    item = ITEM();
    W16(a6 - 8, R16(item + 0x12));
    W16(item + 0x12, 0);
    W16(ITEM() + 6, 0);
    if (shape >= 1 && shape <= 5) {
        int16_t v = var;
        if (shape == 5 && (var == 2 || var == 3 || var == 6 || var == 7)) v = (int16_t)(var - 2);
        v = (int16_t)(v + 0x24);
        W16(ITEM() + 0x18, 7);
        item_xy((uint32_t)(uint16_t)v * 0x14 + 4);
        add_item(0);
        if (shape == 5 && R16(gNoTopRow) != 0) {
            W16(ITEM() + 0xe, R16(ITEM() + 0xe) + 2);
            W16(ITEM() + 6, 0);
            W16(ITEM() + 0x18, 7);
            item_xy((uint32_t)(uint16_t)v * 0x14 + 4);
            add_item(0);
            W16(ITEM() + 0xe, R16(ITEM() + 0xe) - 2);
            W16(ITEM() + 6, 0x10);
        }
    }
    if (shape != 0) {
        var = (int16_t)(var + 0x24);
        W16(ITEM() + 0x18, shape);
        item_xy((uint32_t)(uint16_t)var * 0x14 + 4);
        add_item(0);
    }
    W16(ITEM() + 6, 0x10);
    W16(ITEM() + 0x12, R16(a6 - 8));
    return 0;
}

/* 00032756: piece proc (crevice side): back wall, or the left edge pieces */
uint32_t FUN_00032756(void) {
    ENTRY_NOLINK(FN_FUN_00032756, 12);           /* movem D3-D4/A2 */
    uint32_t item = ITEM();
    switch (RU16(item + 0xa)) {
    case 0:
        if (R16(gNoTopRow) != 0) DrawRuinBackWall();
        break;
    case 5: {
        item = ITEM();
        int16_t k = (int16_t)((uint16_t)(RU16(item + 0x14) & 0xc) >> 2);
        int16_t right = 0;
        if (k == 2) break;
        if (R16(gCurRoom) != 0) right = S16(PieceRightId(R16(item + 0xe), R16(item + 0x10)));
        if (R16(gCurRoom) != 0 && right == 0x14) break;
        if (k != 3) k = (int16_t)(k + 0x19);
        else k = (RU16(ITEM() + 0x14) & 0x80) ? 0x4d : 0x4c;
        W16(ITEM() + 0x18, k);
        item_xy(0x194);
        AddNextBFItemToTable();
        break;
    }
    case 1:
        W16(item + 0x18, (RU16(item + 0x14) & 3) + 0x1b);
        item_xy(0x1a0);
        add_item(1);
        FUN_00032dde();
        break;
    }
    return 0;
}

/* 00032864: piece proc */
uint32_t seg7_032864(void) {
    ENTRY_NOLINK(FN_seg7_032864, 8);             /* movem D3-D4 */
    int16_t add = 0;
    int16_t layer = R16(ITEM() + 0xa);
    switch ((uint16_t)layer) {
    case 0:
        DrawRuinBackWall();
        break;
    case 5: case 2:
        item_copy6(PIECES() + 0x1ac);
        add = 1;
        break;
    case 1:
        seg7_032f0e();
        if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(1);
        item_copy6(PIECES() + 0x1b2);
        add = 1;
        break;
    }
    if (add) {
        add_item(layer);
        FUN_00032dde();
    }
    return 0;
}

/* 000328f4: piece proc: crevice (info&3 = state, info bit 7 = revealed) */
uint32_t DrawRuinCrevice(void) {
    ENTRY_LINK(FN_DrawRuinCrevice, 8, 12);       /* link -8; movem D4-D5/A2 */
    uint32_t item = ITEM();
    int16_t info = R16(item + 0x14);
    W16(a6 - 4, info);
    int16_t st = (int16_t)(info & 3);
    int16_t rev = (RU16(a6 - 4) & 0x80) ? 1 : 0;
    switch (RU16(item + 0xa)) {
    case 0:
        if (R16(gNoTopRow) != 0) DrawRuinBackWall();
        break;
    case 5:
        if (rev) {
            W16(ITEM() + 0x18, st + 0x4e);
            item_xy(0x96);
            add_item(5);
        }
        if (st == 0) W16(ITEM() + 0x14, RU16(ITEM() + 0x14) | 0xc);
        else W16(ITEM() + 0x14, RU16(ITEM() + 0x14) | 8);
        FUN_00032756();
        break;
    case 2:
        if (rev) {
            W16(ITEM() + 0x18, st + 0x4e);
            item_xy(0x96);
            AddNextBFItemToTable();
        }
        break;
    case 1:
        if (rev) {
            W16(item + 0x18, st + 0x52);
            item_xy(0x9c);
            add_item(1);
            FUN_00032dde();
        } else {
            W16(ITEM() + 0x14, RU16(ITEM() + 0x14) & 0xfffc);
            W16(ITEM() + 0x14, RU16(ITEM() + 0x14) | RU16(GH(0x10537a) + (int16_t)(st * 2)));
            FUN_00032756();
        }
        break;
    }
    return 0;
}

/* 00032a7a: piece proc: exit (left part) */
uint32_t FUN_00032a7a(void) {
    ENTRY_NOLINK(FN_FUN_00032a7a, 4);            /* move.l A2 */
    switch (RU16(ITEM() + 0xa)) {
    case 0: {
        if (R16(gNoTopRow) != 0) {
            DrawRuinBackWall();
            W16(ITEM() + 0x18, RU16(PIECES() + 0x142));
            W16(ITEM() + 0x1a, 0x34);
            W16(ITEM() + 0x1c, RU16(PIECES() + 0x146));
            add_item(0);
            FUN_00032dde();
        }
        int16_t cell = (int16_t)(S16(FirstInRow(R16(ITEM() + 0x10))) + R16(ITEM() + 0xe));
        uint32_t t = GetTrobRoomCell(R16(gCurRoom), cell);
        if (t != 0 && R16(t + 4) >= 4) {
            W16(ITEM() + 0x18, RU16(PIECES() + 0x156));
            W16(ITEM() + 0x1a, -0x2c);
            W16(ITEM() + 0x1c, RU16(PIECES() + 0x15a));
            AddNextBFItemToTable();
        }
        break;
    }
    case 5: case 2: case 1:
        W16(ITEM() + 0x14, RU16(ITEM() + 0x14) & 0xfffc);
        seg7_032f0e();
        break;
    }
    return 0;
}

/* 00032b64: piece proc: exit (right part, with the moving door) */
uint32_t seg7_032b64(void) {
    ENTRY_NOLINK(FN_seg7_032b64, 16);            /* movem D3-D5/A2 */
    uint32_t item = ITEM();
    int16_t info = R16(item + 0x14);
    int16_t layer = R16(item + 0xa);
    switch ((uint16_t)layer) {
    case 0: {
        if (R16(gNoTopRow) != 0) {
            DrawRuinBackWall();
        } else {
            W16(ITEM() + 0x18, RU16(PIECES() + 0x142));
            W16(ITEM() + 0x1a, 1);
            W16(ITEM() + 0x1c, RU16(PIECES() + 0x146));
            add_item(0);
        }
        int16_t cell = (int16_t)(S16(FirstInRow(R16(ITEM() + 0x10))) + R16(ITEM() + 0xe));
        uint32_t t = GetTrobRoomCell(R16(gCurRoom), cell);
        if (t != 0 && R16(t + 4) >= 4) {
            W16(ITEM() + 0x18, RU16(PIECES() + 0x156));
            W16(ITEM() + 0x1a, 7);
            W16(ITEM() + 0x1c, RU16(PIECES() + 0x15a));
            add_item(0);
        }
        break;
    }
    case 5: case 2: case 1:
        W16(ITEM() + 0x14, RU16(ITEM() + 0x14) & 0xfffc);
        W16(ITEM() + 0x14, RU16(ITEM() + 0x14) | 2);
        seg7_032f0e();
        if (layer == 5) {
            if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(5);
            W16(ITEM() + 0x14, info);
            item_copy6(PIECES() + 0x47c);
            AddNextBFItemToTable();
        }
        break;
    }
    if (layer == 0) {
        if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(0);
        W16(ITEM() + 0x14, info);
        DrawRuinExitMovable();
    }
    return 0;
}

/* 00032cc2: the exit door (moves up by info&0xff, max 0x4f), clipped to the room */
uint32_t DrawRuinExitMovable(void) {
    ENTRY_LINK(FN_DrawRuinExitMovable, 0x10, 12); /* link -0x10; movem D3-D4/A2 */
    uint32_t item = ITEM();
    if (R16(item + 0xa) != 0) return 0;
    mcopy(a6 - 8, gCellRect, 8);
    mcopy(a6 - 0x10, gRoomRect, 8);
    W16(a6 - 0x10, R16(item + 0x10) * 0x78 - 10);
    if (SECTRECT(a6 - 0x10, gCellRect, gCellRect)) {
        item = ITEM();
        uint16_t h = RU16(item + 0x14) & 0xff;
        int16_t d = h < 0x4f ? (int16_t)h : 0x4f;
        W16(item + 0x18, RU16(PIECES() + 0x476));
        W16(ITEM() + 0x1a, RU16(PIECES() + 0x478));
        W16(ITEM() + 0x1c, -0x22 - (int16_t)(d + 1));
        add_item(0);
        uint16_t kf = RU16(KID(C_FRAME));
        if (kf >= 0xd9 && kf <= 0xe2) {
            uint32_t prev = ITEM();
            ResetNextBFItem(1);
            TransferBFItemMidFields(prev, R32(gCurBFItem));
        }
        item_copy6(PIECES() + 0x47c);
        AddNextBFItemToTable();
    }
    mcopy(gCellRect, a6 - 8, 8);
    return 0;
}

/* 00032dde: decorations on top of a piece (info bits 8-11 / 12-13) */
uint32_t FUN_00032dde(void) {
    ENTRY_NOLINK(FN_FUN_00032dde, 12);           /* movem D3-D4/A2 */
    uint32_t item = ITEM();
    if (R16(item + 0xa) == 5) {
        int16_t k = (int16_t)((uint16_t)(RU16(item + 0x14) & 0xf00) >> 8);
        if (k >= 0xd && k <= 0xf) {
            W16(item + 0x12, k == 0xf ? 1 : 0);
            item_copy6(GH(0x105364) + (uint32_t)((int32_t)k * 6));
            AddNextBFItemToTable();
            return 0;
        }
        item = ITEM();
        uint16_t t = RU16(item + 0x14) & 0x3000;
        if (t == 0) return 0;
        k = (int16_t)((int16_t)t >> 12);
        uint32_t src = GH(0x10540c) + (uint32_t)((int32_t)k * 6);
        W32(item + 0x18, R32(src));
        W16(item + 0x1c, RU16(src + 4));
        AddNextBFItemToTable();
        return 0;
    }
    item = ITEM();
    if (R16(item + 0xa) != 1) return 0;
    uint16_t t = RU16(item + 0x14) & 0xf00;
    if (t == 0 || t >= 0xd00) return 0;
    int16_t k = (int16_t)((uint16_t)(RU16(item + 0x14) & 0xf00) >> 8);
    if (k == 2 && R16(gNoTopRow) != 0) return 0;
    uint32_t src = GH(0x1053be) + (uint32_t)((int32_t)k * 6);
    W32(item + 0x18, R32(src));
    W16(item + 0x1c, RU16(src + 4));
    AddNextBFItemToTable();
    return 0;
}

/* 00032f0e: piece proc: floor */
uint32_t seg7_032f0e(void) {
    ENTRY_NOLINK(FN_seg7_032f0e, 8);             /* movem D3/A2 */
    uint32_t item = ITEM();
    switch (RU16(item + 0xa)) {
    case 0:
        DrawRuinBackWall();
        break;
    case 5: case 2: {
        item = ITEM();
        int16_t layer = R16(item + 0xa);
        W16(item + 0x18, (RU16(item + 0x14) & 3) + 0xe);
        item_xy(0x1e);
        add_item(layer);
        FUN_00032dde();
        break;
    }
    case 1:
        W16(item + 0x18, 0x12);
        item_xy(0x24);
        add_item(1);
        FUN_00032dde();
        break;
    }
    return 0;
}

/* 00032fce: piece proc: gate */
uint32_t seg7_032fce(void) {
    ENTRY_NOLINK(FN_seg7_032fce, 4);             /* move.l D3 */
    int16_t add = 0;
    switch (RU16(ITEM() + 0xa)) {
    case 0:
        DrawRuinBackWall();
        break;
    case 5: case 2:
        item_copy6(PIECES() + 0x58);
        add = 1;
        break;
    case 1:
        seg7_032f0e();
        if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(1);
        item_copy6(PIECES() + 0x5e);
        add = 1;
        break;
    }
    if (add) {
        int16_t layer = R16(ITEM() + 0xa);
        add_item(layer);
        DrawRuinGateMovable();
        if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(layer);
        FUN_00032dde();
    }
    return 0;
}

/* 00033072: the gate itself (moves up by (info&0xff)/2, max 0x5c), clipped to the room */
uint32_t DrawRuinGateMovable(void) {
    ENTRY_LINK(FN_DrawRuinGateMovable, 0x14, 20); /* link -0x14; movem D3-D6/A2 */
    uint32_t item = ITEM();
    int16_t layer = R16(item + 0xa);
    if (layer != 5 && layer != 2) return 0;
    mcopy(a6 - 8, gCellRect, 8);
    mcopy(a6 - 0x10, gRoomRect, 8);
    W16(a6 - 0x10, R16(item + 0x10) * 0x78 - 0x13);
    if (SECTRECT(a6 - 0x10, gCellRect, gCellRect)) {
        if (R16(gCurRoom) == 9 && R16(R32(gGameState) + 0x218a) == 8) {
            if (R16(ITEM() + 0xa) == 2) FUN_00033dea();
        } else {
            W16(ITEM() + 0x18, RU16(PIECES() + 0x454));
            W16(ITEM() + 0x1a, RU16(PIECES() + 0x456));
            item = ITEM();
            int32_t h = (int32_t)(uint32_t)(RU16(item + 0x14) & 0xff) >> 1;
            if (h >= 0x5c) h = 0x5c;
            W16(item + 0x1c, -10 - (int16_t)h);
            int16_t l = R16(ITEM() + 0xa);
            add_item(l);
        }
    }
    mcopy(gCellRect, a6 - 8, 8);
    return 0;
}

/* 00033186: piece proc: wall pieces with a variant table (0x105382 odd / 0x10539a even columns) */
uint32_t FUN_00033186(void) {
    ENTRY_NOLINK(FN_FUN_00033186, 16);           /* movem D4-D6/A2 */
    int16_t floor = 0;
    uint32_t item = ITEM();
    int16_t k = (int16_t)(RU16(item + 0x14) & 0xf);
    if (k != 0xd) {
        if (R16(item + 0xe) & 1) k = R16(GH(0x105382) + (int16_t)(k * 2));
        else k = R16(GH(0x10539a) + (int16_t)(k * 2));
    }
    item = ITEM();
    switch (RU16(item + 0xa)) {
    case 0:
        DrawRuinBackWall();
        break;
    case 5: case 2:
        if (k == 0xd) FUN_00032dde();
        else if (k == 0) floor = 1;
        else {
            int16_t layer = R16(ITEM() + 0xa);
            W16(ITEM() + 0x18, k + 0x2f);
            item_xy(0xe6);
            add_item(layer);
            FUN_00032dde();
        }
        break;
    case 1:
        if (k == 0xd) FUN_00032dde();
        else if (k == 0) floor = 1;
        else {
            W16(item + 0x18, k + 0x31);
            item_xy(0xec);
            add_item(1);
            FUN_00032dde();
        }
        break;
    }
    if (floor) {
        W16(ITEM() + 0x14, RU16(ITEM() + 0x14) & 0xfffc);
        seg7_032f0e();
    }
    return 0;
}

/* 000332c0: piece proc */
uint32_t seg7_0332c0(void) {
    ENTRY_NOLINK(FN_seg7_0332c0, 4);             /* move.l D3 */
    int16_t add = 0;
    switch (RU16(ITEM() + 0xa)) {
    case 0:
        DrawRuinBackWall();
        break;
    case 5: case 2:
        item_copy6(PIECES() + 0xa8);
        add = 1;
        break;
    case 1:
        seg7_032f0e();
        if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(1);
        item_copy6(PIECES() + 0xae);
        add = 1;
        break;
    }
    if (add) {
        add_item(R16(ITEM() + 0xa));
        FUN_00032dde();
    }
    return 0;
}

/* 00033352: piece proc */
uint32_t seg7_033352(void) {
    ENTRY_NOLINK(FN_seg7_033352, 8);             /* movem D3/A2 */
    int16_t add = 0;
    uint32_t item = ITEM();
    switch (RU16(item + 0xa)) {
    case 0:
        DrawRuinBackWall();
        item_copy6(PIECES() + 0xb6);
        add = 1;
        break;
    case 1: {
        uint32_t src = PIECES() + 0xc2;
        W32(item + 0x18, R32(src));
        W16(item + 0x1c, RU16(src + 4));
        add = 1;
        break;
    }
    }
    if (add) {
        add_item(R16(ITEM() + 0xa));
        FUN_00032dde();
    }
    return 0;
}

/* pressure plate pieces: up (DrawRuinDownPlate: 0x35/0x37) or down (FUN_000334d6: 0x34/0x36) */
static void ruin_plate(int16_t sh_front, int16_t sh_back) {
    uint32_t item = ITEM();
    int16_t k = (int16_t)(((RU16(item + 0x14) & 0x2000) ? 1 : 0) + 5);
    uint32_t off = (uint32_t)(uint16_t)k * 0x14;
    switch (RU16(item + 0xa)) {
    case 0:
        DrawRuinBackWall();
        break;
    case 5: case 2:
        W16(ITEM() + 0x18, sh_front);
        item_xy(off + 0xa);
        AddNextBFItemToTable();
        break;
    case 1:
        W16(item + 6, 0);
        W16(ITEM() + 0x18, sh_back);
        item_xy(off + 0x10);
        if (S16(AddNextBFItemToTable()) == 0) W16(ITEM() + 6, 0x10);
        break;
    }
}

/* 000333c2: piece proc: pressure plate */
uint32_t DrawRuinDownPlate(void) {
    ENTRY_LINK(FN_DrawRuinDownPlate, 4, 12);     /* link -4; movem D3-D4/A2 */
    ruin_plate(0x35, 0x37);
    return 0;
}

/* 000334d6: piece proc: pressed plate */
uint32_t FUN_000334d6(void) {
    ENTRY_NOLINK(FN_FUN_000334d6, 12);           /* movem D3-D4/A2 */
    ruin_plate(0x34, 0x36);
    return 0;
}

/* 000335ce: piece proc: posts */
uint32_t DrawRuinPosts(void) {
    ENTRY_LINK(FN_DrawRuinPosts, 4, 16);         /* link -4; movem D3-D5/A2 */
    uint32_t item = ITEM();
    int16_t info = R16(item + 0x14);
    W16(a6 - 4, info);
    int16_t k = (int16_t)(info & 3);
    int16_t layer = R16(item + 0xa);
    switch ((uint16_t)layer) {
    case 0:
        DrawRuinBackWall();
        break;
    case 5: case 2:
        seg7_032f0e();
        if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(layer);
        W16(ITEM() + 0x18, k + 0x23);
        item_xy(0x46);
        AddNextBFItemToTable();
        break;
    case 1: {
        int16_t saved = R16(a6 - 4);
        W16(item + 0x14, RU16(item + 0x14) & 0xf00);
        seg7_032f0e();
        if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(1);
        W16(ITEM() + 0x14, saved);
        W16(ITEM() + 0x18, k + 0x26);
        item_xy(0x4c);
        add_item(1);
        FUN_00032dde();
        break;
    }
    }
    return 0;
}

/* 000336da: piece proc: rubble (info bit 6: on a plate; bit 2: pressed, bit 3: plate) */
uint32_t DrawRuinRubble(void) {
    ENTRY_LINK(FN_DrawRuinRubble, 4, 20);        /* link -4; movem D3-D6/A2 */
    uint32_t item = ITEM();
    int16_t layer = R16(item + 0xa);
    int16_t info = R16(item + 0x14);
    W16(a6 - 4, info);
    int16_t base = (int16_t)(RU16(a6 - 4) & 0xffbf);
    if (RU16(a6 - 4) & 0x40) {
        if (base & 4) {
            W16(item + 0x14, base);
            FUN_000334d6();
        } else if (base & 8) {
            W16(ITEM() + 0x14, base);
            DrawRuinDownPlate();
        } else {
            W16(ITEM() + 0x14, base);
            seg7_032f0e();
        }
    } else if (R16(ITEM() + 0xa) == 0) {
        DrawRuinBackWall();
    } else {
        item = ITEM();
        if (R16(item + 0xa) == 1) {
            W16(item + 0x14, base);
            seg7_032f0e();
        }
    }
    if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(layer);
    W16(ITEM() + 0x14, info & 0xdfff);
    FUN_000337ac();
    if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(layer);
    FUN_00032dde();
    return 0;
}

/* 000337ac: the rubble pieces themselves ((info>>4)&3 = amount) */
uint32_t FUN_000337ac(void) {
    ENTRY_NOLINK(FN_FUN_000337ac, 12);           /* movem D3-D4/A2 */
    int16_t add = 0;
    uint32_t item = ITEM();
    int16_t k = (int16_t)((uint16_t)(RU16(item + 0x14) & 0x30) >> 4);
    switch (RU16(item + 0xa)) {
    case 2: case 5:
        W16(ITEM() + 0x18, k + 0x13);
        item_xy(0x122);
        add = 1;
        break;
    case 1:
        if (k == 0) {
            W16(item + 0x18, 0x16);
            item_xy(0x128);
            add = 1;
        }
        break;
    }
    if (add) {
        add_item(R16(ITEM() + 0xa));
        FUN_00032dde();
    }
    return 0;
}

/* 00033866: piece proc: back wall when the cell is visible from below */
uint32_t seg7_033866(void) {
    ENTRY_NOLINK(FN_seg7_033866, 12);            /* movem D3-D4/A2 */
    uint32_t item = ITEM();
    if (R16(item + 0xa) == 0) {
        int16_t draw = 0;
        if (R16(item + 0x10) == R16(GH(0x103a0a))) draw = 1;
        else {
            int32_t r1 = (int32_t)R16(GH(0x103a0a)) + 1;
            item = ITEM();
            int16_t row = R16(item + 0x10);
            if (r1 == (int32_t)row) draw = S16(VisibleAbovePiece(R16(item + 0xe), row));
        }
        if (draw == 0 && R16(GH(0x103a0a)) == -1 && S16(IsSpace(R16(GH(0x1037ec)))) != 0) draw = 1;
        if (draw) DrawRuinBackWall();
    }
    FUN_00032dde();
    return 0;
}

/* 000338d8: empty */
uint32_t seg7_0338d8(void) { return 0; }

/* 000338da: reveal (or hide) the crevice found in the row around col (the original keeps
 * its column counter in its own argument slot: see glue) */
static int16_t ruin_change_crevices(int16_t flag, int16_t room, maddr colslot, int16_t row) {
    int32_t c0 = R16(colslot);
    int32_t t = c0 + 3;
    int16_t end = (int16_t)(t >= 10 ? t : 10);
    t = c0 - 2;
    W16(colslot, t <= 0 ? t : 0);
    int16_t cell;
    do {
        cell = S16(ReadCell(room, R16(colslot), row));
        W16(colslot, R16(colslot) + 1);
    } while (R16(colslot) != end && cell != 7);
    if (cell != 7) return 0;
    do {
        cell = S16(ReadCell(R16(GH(0x104b56)), (int16_t)(R16(GH(0x104b58)) - 1), R16(GH(0x104b5a))));
    } while (cell == 7);
    ReadCell(R16(GH(0x104b56)), (int16_t)(R16(GH(0x104b58)) + 1), R16(GH(0x104b5a)));
    do {
        RuinSetCreviceRevealed(flag);
        cell = S16(ReadCell(R16(GH(0x104b56)), (int16_t)(R16(GH(0x104b58)) + 1), R16(GH(0x104b5a))));
    } while (cell == 7);
    return 0;
}
uint32_t RuinChangeCrevices(int16_t flag, int16_t room, int16_t col, int16_t row) {
    ENTRY_LINK(FN_RuinChangeCrevices, 4, 12);    /* link -4; movem D3-D5 */
    W16(a6 + 8, flag);                           /* the argument slots the caller pushed */
    W16(a6 + 0xa, room);
    W16(a6 + 0xc, col);
    W16(a6 + 0xe, row);
    ruin_change_crevices(flag, room, a6 + 0xc, row);
    return 0;
}
void glue_RuinChangeCrevices(void) {
    maddr sp = emu_sp();                         /* arguments (return address already popped) */
    RuinChangeCrevices((int16_t)rd16(sp), (int16_t)rd16(sp + 2), (int16_t)rd16(sp + 4), (int16_t)rd16(sp + 6));
}

/* 000339b4: the character steps on a crevice (piece 7) that is not revealed yet */
uint32_t FUN_000339b4(void) {
    ENTRY_NOLINK(FN_FUN_000339b4, 8);            /* movem D3-D4 */
    if (R16(CHR(C_SEQ)) == 0x11) return 0;
    if (S16(GetCellsInFront(1)) != 7) return 0;
    if ((R16(GH(0x104b52)) & 3) == 3) return 0;
    RuinChangeCrevices(1, R16(CHR(C_ROOM)), R16(CHR(C_COL)), R16(CHR(C_ROW)));
    return 0;
}

/* address of the spec word of the cell last read by ReadCell */
static uint32_t cell_spec_addr(void) {
    return (uint32_t)((int32_t)R16(GH(0x104b54)) * 4) +
           (R32(gGameState) + 0x780 + (uint32_t)RU16(GH(0x104b56)) * 0x78 - 0x78);
}

/* 000339fa: set/clear the "revealed" bit (0x80) of the crevice cell last read */
uint32_t RuinSetCreviceRevealed(int16_t on) {
    ENTRY_LINK(FN_RuinSetCreviceRevealed, 0, 8); /* link 0; movem D3-D4 */
    uint16_t spec = RU16(cell_spec_addr());
    int16_t chg;
    if (on != 0) {
        chg = (spec & 0x80) == 0 ? 1 : 0;
        if (chg) spec |= 0x80;
    } else {
        chg = (int16_t)(spec & 0x80);
        if (chg) spec &= 0xff7f;
    }
    if (chg == 0) return 0;
    if (S16(IsVisibleRoomCell(R16(GH(0x104b56)), R16(GH(0x104b54)))) != 0) FUN_00025ab6();
    if ((spec & 3) == 0 && R16(GH(0x104b58)) != 9) {
        ReadCell(R16(GH(0x104b56)), (int16_t)(R16(GH(0x104b58)) + 1), R16(GH(0x104b5a)));
        FUN_00025ab6();
        ReadCell(R16(GH(0x104b56)), (int16_t)(R16(GH(0x104b58)) - 1), R16(GH(0x104b5a)));
    }
    W16(cell_spec_addr(), spec);
    return 0;
}

/* 00033b26: clipping of the flying heads (types 7/8) against walls (ClipChar hook) */
uint32_t seg7_033b26(void) {
    ENTRY_NOLINK(FN_seg7_033b26, 20);            /* movem D3-D7 */
    int16_t t = R16(CHR(C_TYPE));
    int16_t head = (t == 7 || t == 8) ? 1 : 0;
    int16_t col = S16(GetCol(R16(GH(0x104a9a))));
    int16_t cell = S16(ReadCell(R16(CHR(C_ROOM)), col, R16(CHR(C_ROW))));
    if (S16(IsWall(cell)) != 0) {
        cell = S16(ReadCell(R16(CHR(C_ROOM)), col, R16(GH(0x104b60))));
        if (S16(IsWall(cell)) != 0 && cell != 7 && R16(GH(0x104b56)) == R16(CHR(C_ROOM)))
            W16(GH(0x104b4a), R16(GH(0x104b58)) * 0x33);
    }
    if (head == 0) return 0;
    int16_t s = R16(CHR(C_SEQ));
    if (s == 0x89 || s == 0x8a || s == 0x8f || s <= 0x90 || s == 0x90) return 0;
    int16_t c1 = S16(GetCol(R16(GH(0x104a9c))));
    int16_t c2 = S16(GetCol(R16(GH(0x104a9a))));
    if (c1 != c2) {
        int16_t a = (int16_t)(0x33 - (int16_t)(((int32_t)R16(GH(0x104a9c)) - 0xcf) % 0x33));
        int16_t b = (int16_t)(((int32_t)R16(GH(0x104a9a)) - 0x33) % 0x33);
        if (a > b) col = (int16_t)(col - 1);
    }
    cell = S16(ReadCell(R16(CHR(C_ROOM)), col, R16(GH(0x104b60))));
    if (S16(IsSpace(cell)) == 0) W16(GH(0x104b44), R16(GH(0x104b62)) * 0x78 + 5);
    return 0;
}

/* 00033c50: make the crevice cell last read fall as a mob (type 3) */
uint32_t seg7_033c50(void) {
    ENTRY_NOLINK(FN_seg7_033c50, 8);             /* movem D3-D4 */
    W16(GH(0x103b5a), R16(GH(0x103498) + (int16_t)(R16(GH(0x104b58)) * 2)));
    int16_t row = R16(GH(0x104b5a));
    W16(GH(0x103b5c), R16(GH(0x1034b2) + (int16_t)(row * 2)));
    if (R16(GH(0x104b56)) == R16(GH(0x103044)) && row == 2) W16(GH(0x103b5c), R16(GH(0x103b5c)) + 5);
    W16(GH(0x103b5e), R16(GH(0x104b56)));
    W16(GH(0x103b62), 0);
    W16(GH(0x103b60), 0);
    W16(GH(0x103b64), 3);
    W16(GH(0x103b66), R16(GH(0x104b5a)));
    W16(GH(0x103b68), 0);
    FUN_00026a94();
    MakeSpace(R16(GH(0x104b56)), R16(GH(0x104b54)));
    int16_t x = R16(GH(0x103b5a));
    if (R16(GH(0x103b5e)) == R16(GH(0x103040))) W16(GH(0x103b5a), R16(GH(0x103b5a)) - 0x1fe);
    MarkMobRedraw(0);
    W16(GH(0x103b5a), x);
    return 0;
}

/* 00033cee: put the kid at the start of the ruins (room 3, column 4) (CutChar hook) */
uint32_t seg7_033cee(void) {
    ENTRY_NOLINK(FN_seg7_033cee, 4);             /* move.l D3 */
    W16(CHR(C_FACE), 0);
    JumpSeq(5);
    W16(CHR(C_ROOM), 3);
    W16(GH(0x10303c), 3);
    W16(CHR(C_COL), 4);
    W16(CHR(C_X), R16(GH(0x103478) + 8) + 0x16);
    W16(CHR(C_ROW), 2);
    FUN_00020cca();
    AnimChar();
    mcopy(KID(0), CHR(0), 0x3e);
    seg2_016366();
    seg4_02190a();
    W16(GH(0x104b80), 0);
    return 0;
}

/* 00033d56: level hook: palette 0xdae */
uint32_t seg7_033d56(void) {
    ENTRY_NOLINK(FN_seg7_033d56, 0);
    PoPSetPalette(0xdae, 0x40);
    return 0;
}

/* 00033d66: level hook: palette 0xdac while the kid is alive and time is left */
uint32_t seg7_033d66(void) {
    ENTRY_NOLINK(FN_seg7_033d66, 0);
    if (R16(KID(C_ALIVE)) < 0 && R16(GH(0x103032)) != 0) PoPSetPalette(0xdac, 0x40);
    return 0;
}

/* 00033d86: level hook (level 9): music in rooms 0x10 and 2 */
uint32_t seg7_033d86(void) {
    ENTRY_NOLINK(FN_seg7_033d86, 4);             /* move.l D3 */
    int16_t v = R16(KID(C_COL));
    if (v >= 9) {
        if (R16(gCurRoom) != 0x10) return (uint16_t)v;
        v = R16(R32(gGameState) + 0x218a);
        if (v != 9) return (uint16_t)v;
        return AddSong(0x124);
    }
    if (R16(gCurRoom) != 2) return (uint16_t)v;
    v = R16(R32(gGameState) + 0x218a);
    if (v != 9) return (uint16_t)v;
    v = R16(GH(0x103008));
    if (v != 0) return (uint16_t)v;
    if (R16(KID(C_COL)) >= 5) return (uint16_t)v;
    AddSong(0xb9);
    W16(GH(0x103008), 1);
    return 1;
}

/* 00033dea: draw custom room image 6 (room 9 of level 8: the gate) */
uint32_t FUN_00033dea(void) {
    ENTRY_NOLINK(FN_FUN_00033dea, 8);            /* movem D3/A2 */
    uint32_t p = R32(GH(0x103362)) + 0xde;
    W16(p + 6, 1);
    DrawCustomRoomImg(6, 1);
    W16(p + 6, 0);
    return 0;
}

/* 00033e14: level hook: palette 0xdad and music */
uint32_t seg7_033e14(void) {
    ENTRY_NOLINK(FN_seg7_033e14, 0);
    PoPSetPalette(0xdad, 0x30);
    int16_t v = R16(GH(0x10300c));
    if (v != 0) return (uint16_t)v;
    if (R16(KID(C_COL)) < 9) return AddSong(0xb4);
    v = R16(GH(0x10300a));
    if (v != 0) return (uint16_t)v;
    AddSong(0xb3);
    W16(GH(0x10300a), 1);
    return 1;
}

/* 00033e54: level hook: palettes 0xdac and 0xbb8 */
uint32_t seg7_033e54(void) {
    ENTRY_NOLINK(FN_seg7_033e54, 0);
    if (R16(GH(0x103000)) == -1) {
        PoPSetPalette(0xdac, 0x40);
        PoPSetPalette(0xbb8, 0xe0);
    }
    return 0;
}

/* 00033e7a: play NIS 6, then go to level 8 room 9 keeping the game state block */
uint32_t seg7_033e7a(void) {
    ENTRY_NOLINK(FN_seg7_033e7a, 16);            /* movem D3-D4/A3-A4 */
    uint32_t h = PoPNewHandle(0x49ca);
    mcopy(DEREF(h), R32(gGameState), 0x49ca);
    if (S16(PlayNIS(6)) == 2) W16(gCurRoom, 0);
    FUN_00016112();
    LoadCodeOverlays(4, 5);
    W16(GH(0x103000), -1);
    FUN_000225d2();
    W16(CHR(C_FACE), 0);
    JumpSeq(0xe7);
    W16(CHR(C_ROOM), 9);
    W16(GH(0x10303c), 9);
    FUN_00020cca();
    W16(CHR(0x22), R16(CHR(0x20)));
    W16(GH(0x10302e), 8);
    W16(GH(0x10300c), 1);
    W16(GH(0x102ff6), 1);
    LoadLevel(8);
    seg3_019120();
    mcopy(R32(gGameState), DEREF(h), 0x49ca);
    _DisposeHandle(h);
    LoadCustomRoomData(9);
    {   /* the original pushes a third word (10) that MarkBufsForRect does not read */
        maddr sp = emu_sp();
        emu_set_sp(sp - 2);
        W16(sp - 2, 10);
        MarkBufsForRect(JT_ADDR(FN_MarkMove), GH(0x102f58));
        emu_set_sp(sp);
    }
    MarkRedraw(0x19);
    ResetStandardColors(R32(GH(0x102dbe)));
    AnimChar();
    mcopy(KID(0), CHR(0), 0x3e);
    seg4_02190a();
    W16(GH(0x104b80), 0);
    if (S16(PrinceSndOn(0xb6)) == 0) AddSong(0xb6);
    else UpdateAmbient(GH(0x1048c8), 0xb6);
    return 0;
}
