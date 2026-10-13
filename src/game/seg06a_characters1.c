/* CODE 6 "Characters 1" (first half, 0x2a644-0x2d58e): the character control layer.
 * Starting sequences (JumpSeq), falling and landing, the generic per-frame control
 * (GenCtrl) that turns the input words into moves (steps, jumps, climbing, picking up
 * objects, stairs), the sword-fight moves (en garde, strike, block, advance, retreat),
 * crushing by falling objects, and the set-up of a room's opponents (FUN_0002d1be).
 * Ported from the Ghidra decompilation / disassembly of the original 68k code;
 * addresses in comments are Ghidra addresses (see docs/PORTING.md). */
#include "port.h"
#include "gen/proto.h"

/* ---- the current character record (0x3e bytes at 0x102faa, see docs/notes/seg04*.md) ---- */
#define CHAR_SIZE       0x3e
#define gChar           GH(0x102faa)   /* record: the character being processed */
#define CH_ID           GH(0x102faa)   /* word +0x00: index in the room's table (10 = the kid) */
#define CH_FACE         GH(0x102fac)   /* word +0x02: facing (0 = right, -1 = left) */
#define CH_X            GH(0x102fae)   /* word +0x04 */
#define CH_Y            GH(0x102fb0)   /* word +0x06 */
#define CH_TYPE         GH(0x102fb2)   /* word +0x08: 0 kid, 1 shadow, 2 guard, 4 skeleton,
                                          6, 7/8 flying heads, 10, 0xb ... */
#define CH_FRAME        GH(0x102fb4)   /* word +0x0a: animation frame (compared unsigned) */
#define CH_COL          GH(0x102fb6)   /* word +0x0c */
#define CH_ROW          GH(0x102fb8)   /* word +0x0e */
#define CH_ACTION       GH(0x102fba)   /* word +0x10 */
#define CH_XVEL         GH(0x102fbc)   /* word +0x12 */
#define CH_YVEL         GH(0x102fbe)   /* word +0x14 */
#define CH_ROOM         GH(0x102fc0)   /* word +0x16 */
#define CH_FLAG18       GH(0x102fc2)   /* word +0x18 */
#define CH_SWORD        GH(0x102fc4)   /* word +0x1a: 1 = en garde (sword drawn), -1 = never */
#define CH_ALIVE        GH(0x102fc6)   /* word +0x1c: < 0 alive, >= 0 death counter */
#define CH_LIFE         GH(0x102fc8)   /* word +0x1e */
#define CH_MAXLIFE      GH(0x102fca)   /* word +0x20 */
#define CH_LIFEDELTA    GH(0x102fcc)   /* word +0x22: pending life change */
#define CH_SEQPOS       GH(0x102fce)   /* word +0x24: position in the sequence */
#define CH_SEQ          GH(0x102fd0)   /* word +0x26: current sequence */
#define CH_JSEQ         GH(0x102fd2)   /* word +0x28: last sequence started by JumpSeq */
#define CH_F32          GH(0x102fdc)   /* word +0x32 */
#define CH_F34          GH(0x102fde)   /* word +0x34 */
#define CH_F36          GH(0x102fe0)   /* word +0x36 */
#define CH_F38          GH(0x102fe2)   /* word +0x38 */
#define CH_OPPIDX       GH(0x102fe4)   /* word +0x3a: index of the opponent it fights */
#define CH_F3C          GH(0x102fe6)   /* word +0x3c */

/* the opponent record (same layout) and the saved kid record */
#define gOpp            GH(0x102f6c)
#define OPP_TYPE        GH(0x102f74)   /* gOpp +0x08 */
#define OPP_FRAME       GH(0x102f76)   /* gOpp +0x0a */
#define OPP_COL         GH(0x102f78)   /* gOpp +0x0c */
#define OPP_ROOM        GH(0x102f82)   /* gOpp +0x16 */
#define OPP_F32         GH(0x102f9e)   /* gOpp +0x32 */
#define gKid            GH(0x102f2e)
#define KID_X           GH(0x102f32)   /* gKid +0x04 */
#define KID_ROOM        GH(0x102f44)   /* gKid +0x16 */
#define gOppIdx         GH(0x102f68)   /* word: index of the current opponent */
#define gOppTab         GH(0x102df8)   /* the room's characters, CHAR_SIZE each */

#define gGameState      GH(0x102fec)   /* long: pointer to the game state block */
#define GS16(o)         R16(R32(gGameState) + (o))
#define LEVEL_TYPE()    GS16(0x2186)
#define LEVEL_NUM()     GS16(0x218a)
#define OPP_KIND()      GS16(0x21a4)
#define gCurRoom        GH(0x10303e)   /* word */
#define gScreaming      GH(0x103060)   /* word: the falling scream was started */
#define gLanded103062   GH(0x103062)   /* word: set when the kid lands hard */

/* last cell read by ReadCell (seg 4) */
#define gCellId         GH(0x104b50)
#define gCellSpec       GH(0x104b52)
#define gCellCol        GH(0x104b58)

/* the control inputs (already mirrored for the facing): < 0 pressed, > 0 used */
#define gInFwd          GH(0x104b78)
#define gInBack         GH(0x104b7a)
#define gInUp           GH(0x104b7c)
#define gInDown         GH(0x104b7e)
#define gInBtn          GH(0x104b80)

#define gColLeftX       GH(0x103478)   /* word[]: left x of each column */
#define gColRightX      GH(0x10347a)   /* word[]: (gColLeftX + 2) */
#define gRowFloorY      GH(0x1034b2)   /* word[]: floor y of each row */
#define gDirDX          GH(0x103468)   /* word[2] indexed by face: column step forward */

/* 0002a644: empty */
uint32_t FUN_0002a644(void) {
    return 0;
}

/* 0002a646: start sequence seq for the current character */
uint32_t JumpSeq(int16_t seq) {
    ENTRY_LINK(FN_JumpSeq, 0, 4);                /* link 0; movem D3 */
    if (R16(CH_TYPE) == 1) seg4_01fb80();
    uint32_t r = PoPGetResource(0x53455153, seq);  /* 'SEQS' */
    if (r == 0) return 0;
    W16(CH_SEQ, seq);
    W16(CH_JSEQ, seq);
    W16(CH_SEQPOS, 0);
    int16_t type = R16(CH_TYPE);
    if (type != 7) {
        if (type != 8) return 8;
    }
    uint32_t d0 = (uint32_t)type;                /* moveq #7 / moveq #8 */
    if (!((seq >= 0x82 && seq <= 0x8b) || seq == 0xa6)) return d0;
    if (R16(CH_FACE) != 0) return RU16(CH_FACE);
    W16(CH_FACE, -1);
    int16_t frame = R16(CH_FRAME);
    if (frame == 0x9f || frame == 0x96) W16(CH_X, AddCharX(0xc));
    return FUN_00020542();
}

/* 0002a6e6: the character is falling: scream, test for landing (HitFloor) */
uint32_t Falling(void) {
    ENTRY_LINK(FN_Falling, 4, 12);               /* link -4; movem D3-D5 */
    if (R16(gScreaming) == 0 && R16(CH_YVEL) >= 0x3b) {
        if (R16(CH_TYPE) == 0) {
            AddKidScream(R16(CH_ROOM));
            W16(gScreaming, 1);
        } else if (R16(CH_TYPE) == 2 && R16(CH_YVEL) <= 0x3f) {
            AddSound(0x1e);
            W16(CH_YVEL, 0x40);
        }
    }
    int32_t y = R16(CH_Y);
    int32_t floor = (int32_t)R16(CH_ROW) * 0x78 + 0x6a;
    if (y - 3 < floor && y + 0x30 >= floor) {
        seg4_02129a();
        int16_t t = R16(CH_TYPE);
        if ((t == 7 || t == 8) && R16(CH_YVEL) != 0) W16(CH_ROW, GetRow(R16(CH_Y)));
    }
    int16_t cell = S16(FUN_00021076());
    if (S16(IsWall(cell)) != 0 && R16(CH_JSEQ) != 0xf) cell = S16(seg4_021452());
    if ((int32_t)R16(CH_ROW) * 0x78 + 0x6a >= (int32_t)R16(CH_Y)) return 0;
    int16_t act = R16(CH_ACTION);
    if (act != 4 && act != 9) return 0;
    if (S16(IsSpace(cell)) != 0) {
        W16(CH_ROW, R16(CH_ROW) + 1);
        return 0;
    }
    if (R16(GH(0x103c62)) == 8) W16(GH(0x103c62), -1);
    PrinceResetSnd(8);
    W16(gScreaming, 0);
    if (cell == 0xb || cell == 0xf || cell == 0x1a) {
        if (cell == 0xb) BreakLoose(R16(CH_YVEL));
        else if (cell == 0xf) seg7_033c50();
        else if (cell == 0x1a) seg10_03ca4a();
        if ((R16(CH_YVEL) >= 0x3f || R16(CH_ID) != 10) && R16(CH_TYPE) != 1) DecLifePts(100);
        W16(CH_YVEL, (int16_t)(R16(CH_YVEL) / 2));
        W16(CH_Y, R16(gRowFloorY + (int32_t)R16(CH_ROW) * 2) - 0xd);
    } else {
        HitFloor();
    }
    return 0;
}

/* 0002a8c0: the character lands: choose the landing sequence, damage, sounds.
 * Returns 1 when it lands on a special floor (0x17/0x18), -1 when nothing more
 * happens, else the character type (1 for the kid) after a landing sound. */
uint32_t HitFloor(void) {
    ENTRY_LINK(FN_HitFloor, 8, 20);              /* link -8; movem D3-D7 */
    int16_t special = 0;                         /* D7 */
    int16_t done = 0;                            /* -4(A6) */
    int16_t die = 0;                             /* -6(A6) */
    int16_t hurt = 0;                            /* -8(A6) */
    int16_t seq = (int16_t)cpu_get(R_D4);        /* D4: set on every path that uses it */
    int16_t snd = -1;                            /* D5 */
    W16(gScreaming, 0);
    FUN_00020cca();
    int16_t cell = S16(FUN_00021076());
    if (cell == 0x17 || cell == 0x18) {
        special = 1;
    } else {
        int16_t dist = S16(seg4_021562());
        cell = S16(GetCellsInFront(1));
        int16_t t = R16(CH_TYPE);
        if (t != 7 && t != 8) {
            if (S16(IsSpace(cell)) != 0 || S16(IsImpassable(cell)) != 0) {
                if (dist <= 0x1e) W16(CH_X, AddCharX((int16_t)(dist - 0x20)));
            }
        }
        if (R16(CH_ALIVE) >= 0) hurt = 1;
    }
    cell = S16(FUN_00021076());
    if (special != 0 || cell == 0x17 || cell == 0x18) {
        FUN_00037466();
        done = 1;
    }
    if (done != 0) return 1;
    if (R16(CH_YVEL) < 0x32 || R16(CH_TYPE) == 1) {
        int16_t t = R16(CH_TYPE);
        if ((t < 2 && R16(CH_SWORD) != 1) || t == 6 || t == 1) {
            seq = 0x11;
        } else {
            if (R16(CH_JSEQ) == 0xba) {
                seq = 0xbb;
            } else {
                W16(GH(0x104966), 9);
                seq = 0x3f;
            }
            W16(CH_SWORD, 1);
        }
        snd = (R16(CH_TYPE) == 4) ? 0x3d : 0x128;
    } else if (R16(CH_YVEL) < 0x3f) {
        if (R16(CH_ID) != 10) {
            hurt = 1;
        } else if (S16(DecLifePts(1)) != 0 ||
                   (R16(CH_ROOM) == 0xb && LEVEL_TYPE() == 5 && R16(CH_ROW) >= 3)) {
            die = 1;
            DecLifePts(100);
        } else {
            snd = 0xd;
            seq = 0x14;
        }
    } else {
        hurt = 1;
    }
    if (hurt != 0) {
        DecLifePts(100);
        die = 1;
    }
    if (die != 0) {
        if (R16(CH_TYPE) == 4) {
            seg4_01f1de();
            seq = 0x70;
        } else {
            snd = 7;
            seq = 0x16;
        }
        SetKidDeathMethod(3);
        FUN_0002aaea();
    }
    int16_t t = R16(CH_TYPE);
    if (t == 7 || t == 8) {
        W16(CH_X, AddCharX(10));
        seq = 0x9b;
    }
    JumpSeq(seq);
    AnimChar();
    W16(CH_YVEL, 0);
    if (snd == -1) return 0xffffffff;
    uint32_t r = AddSound(snd);
    if (R16(CH_TYPE) != 0) return (r & 0xffff0000) | RU16(CH_TYPE);
    W16(gLanded103062, 1);
    return 1;
}

/* 0002aaea: a dying character slides off the edge it landed on */
uint32_t FUN_0002aaea(void) {
    ENTRY_NOLINK(FN_FUN_0002aaea, 16);           /* movem D3-D6 */
    int16_t dist = S16(seg4_021562());           /* D4 */
    int16_t cell;                                /* D6 */
    if (dist < 0x20) cell = S16(GetCellsInFront(1));
    else if (dist > 0x20) cell = S16(FUN_00021518());
    else cell = 1;
    int16_t kind;                                /* D5 */
    if (S16(IsSpace(cell)) != 0) kind = 1;
    else if (S16(IsMobFloor(cell)) != 0) kind = 2;
    else kind = 0;
    if (kind == 0) return 0;
    W16(CH_X, AddCharX((int16_t)(dist - 0x20)));
    if (kind != 2 || R16(CH_ROW) < 3) return 0;
    int16_t dir = ((int32_t)dist - 0x20 < 0) ? -1 : 1;
    W16(CH_X, AddCharX((int16_t)(dir * 0x1d)));
    if (cell == 0x1a) W16(CH_X, AddCharX((int16_t)(dir * 0xd)));
    return 0;
}

/* 0002aba0: generic per-frame control of the current character */
uint32_t GenCtrl(void) {
    ENTRY_LINK(FN_GenCtrl, 4, 8);                /* link -4; movem D3/D4 */
    uint16_t frame0 = RU16(CH_FRAME);            /* D4 */
    if (R16(CH_ALIVE) >= 0) {
        /* dead or dying */
        if (R16(CH_TYPE) != 0 && R16(gOppIdx) == R16(CH_ID))
            W16(gOppIdx, GetOppIdxClosestToKid(1));
        if (R16(CH_TYPE) == 0) {
            if (LEVEL_TYPE() == 6 && R16(CH_ALIVE) > 6) seg9_037c12();
        } else if (R16(CH_TYPE) == 1) {
            if (R16(CH_ALIVE) > 6) CutToKidRoom();
        } else if (R16(CH_ROOM) == 3 && LEVEL_TYPE() == 6 && R16(CH_TYPE) == 2) {
            FinlCheckDeadOpp();
        }
        return 0;
    }
    int16_t act = R16(CH_ACTION);
    int16_t jseq = R16(CH_JSEQ);
    if (act == 5 || act == 4 || act == 9 || jseq == 0x1b || jseq == 0x6e) {
        seg4_02190a();
        if (LEVEL_NUM() == 0xc && R16(GH(0x102e00)) == 0xc && R16(GH(0x102dfa)) != 0x56 &&
            R16(GH(0x102dfc)) < R16(GH(0x103484)))
            JumpSeq(0xd4);
        return 0;
    }
    if (R16(CH_SWORD) == 1) { FUN_0002c7a6(); return 0; }
    if (R16(CH_TYPE) >= 2) { seg4_022466(); return 0; }
    uint16_t f = RU16(CH_FRAME);
    if (f == 0xf || (f >= 0x32 && f <= 0x34)) { FUN_0002b272(); return 0; }
    f = RU16(CH_FRAME);
    if (f >= 0x2d && f <= 0x31) { FUN_0002b654(); return 0; }
    f = RU16(CH_FRAME);
    if (f >= 1 && f < 4) { FUN_0002bbf6(); return 0; }
    f = RU16(CH_FRAME);
    if (f >= 0x43 && f < 0x46) { FUN_0002bc1e(); return 0; }
    if (RU16(CH_FRAME) < 0xf) { FUN_0002b824(); return 0; }
    f = RU16(CH_FRAME);
    if (f >= 0x57 && f <= 0x63) {
        if (LEVEL_TYPE() != 1) FUN_0002bf20();
        return 0;
    }
    if (RU16(CH_FRAME) == 0x51 && R16(CH_JSEQ) == 0x44) { seg6_02c0c4(); return 0; }
    if (RU16(CH_FRAME) == 0x6d) { FUN_0002afda(); return 0; }
    f = RU16(CH_FRAME);
    if ((f >= 0xf6 && f <= 0x105) || (f >= 0x100 && f <= 0x107)) { FUN_0002ae2e(); return 0; }
    f = RU16(CH_FRAME);
    if (f >= 0xd9 && f <= 0xe2) { StairClimbing(); return 0; }
    if (S16(IsDeadPos(R16(CH_FRAME))) != 0 || (frame0 >= 0xb3 && frame0 <= 0xb7)) {
        FUN_0002b19a();
        return 0;
    }
    if (RU16(CH_FRAME) == 0x127) { Sitting(); return 0; }
    f = RU16(CH_FRAME);
    if (f >= 0x110 && f <= 0x119) { seg9_03a418(); return 0; }
    f = RU16(CH_FRAME);
    if (f == 0x2c || f == 0x1a) seg4_02190a();
    return 0;
}

/* 0002ae2e: control while hanging/climbing on a ledge (frames 0xf6-0x105) */
uint32_t FUN_0002ae2e(void) {
    ENTRY_NOLINK(FN_FUN_0002ae2e, 12);           /* movem D3-D5 */
    uint16_t f = RU16(CH_FRAME);
    if (f < 0xf6 || f > 0x105) return 0;
    if (LEVEL_TYPE() == 4) FUN_000339b4();
    int16_t cell = S16(ReadCell(R16(CH_ROOM), R16(CH_COL), R16(CH_ROW)));   /* D5 */
    int hold = cell == 7 || cell == 0xc || cell == 0xd ||
               S16(GetCellsInFront(1)) == 7 || S16(GetCellsInFront(1)) == 0xc ||
               S16(GetCellsInFront(1)) == 0xd ||
               S16(FUN_00021518()) == 7 || S16(FUN_00021518()) == 0xc ||
               S16(FUN_00021518()) == 0xd || R16(CH_JSEQ) == 0x7c;
    if (!hold) {
        JumpSeq(0x7c);
        return 0;
    }
    if (RU16(CH_FRAME) != 0xfb) {
        seg4_02190a();
        return 0;
    }
    if (R16(gInUp) != 0 && cell != 7) {
        int16_t d = S16(FUN_00021518());
        if (d == 7) {
            if (R16(CH_FACE) == 0) {
                int16_t x = R16(CH_X);
                x = (int16_t)(x - R16(gColLeftX + (int32_t)R16(CH_COL) * 2));
                d = (int16_t)(x - 0x3a);
            } else {
                int16_t x = R16(CH_X);
                x = (int16_t)(x - R16(gColRightX + (int32_t)R16(CH_COL) * 2));
                d = (int16_t)(-0xe - x);
            }
        } else if (d == 0xc || d == 0xd) {
            d = (S16(FUN_0002a09a()) == 0) ? 0x33 : -0x33;
        } else {
            d = 0x33;
        }
        if (d >= 0) {
            JumpSeq(0x7c);
            W16(gInUp, seg4_02190a());
            if (d < 9) W16(CH_X, AddCharX((int16_t)(9 - d)));
        }
    }
    if (R16(CH_JSEQ) != 0x7c) {
        if (R16(gInFwd) < 0) {
            seg4_02190a();
            JumpSeq(0x7a);
        } else if (R16(gInBack) < 0) {
            seg4_02190a();
            JumpSeq(0x7b);
        }
    }
    if (R16(gInFwd) == 0 && R16(CH_FLAG18) == 0) W16(CH_FLAG18, 1);
    return 0;
}

/* 0002afda: control while crouching (frame 0x6d): pick up, stand up, crawl */
uint32_t FUN_0002afda(void) {
    ENTRY_NOLINK(FN_FUN_0002afda, 12);           /* movem D3-D5 */
    if (LEVEL_TYPE() == 4) FUN_000339b4();
    if (LEVEL_NUM() == 5 && R16(CH_ROOM) == 3) {
        int16_t js = R16(CH_JSEQ);
        if (js != 0x11 && js != 0x14) {
            if (S16(FUN_00021076()) == 0x12 ||
                S16(ReadCell(R16(CH_ROOM), (int16_t)(R16(CH_COL) - 1), R16(CH_ROW))) == 0x12) {
                FUN_0003546e();
                return 0;
            }
        }
    }
    if (R16(CH_JSEQ) == 0x6f || S16(TryPickUp()) != 0) {
        W16(gInBtn, seg4_02190a());
        return 0;
    }
    int16_t seq = -1;                            /* D4 */
    if (R16(gInDown) == 0) {
        if (S16(GateBarrToStandUp()) == 0 &&
            (R8(R32(gGameState) + 0x218f) == 0 || S16(FUN_0002a09a()) == 0)) {
            seq = 0x31;
        } else if (R16(gInFwd) < 0) {
            seq = 0x4f;
        }
    } else if (R16(gInFwd) < 0) {
        W16(gInFwd, seg4_02190a());
        seq = S16(GetCellsInFront(1));
        if ((seq == 7 || seq == 0xc || seq == 0xd) &&
            (seq != 7 || (RU16(gCellSpec) & 3) != 3)) {
            int16_t col = S16(FUN_00021600());   /* D5 */
            if (R16(CH_FACE) == 0 &&
                (int32_t)R16(CH_X) - (int32_t)R16(gColLeftX + (int32_t)col * 2) > 10) {
                seq = (int16_t)(10 - (int16_t)(R16(CH_X) - R16(gColLeftX + (int32_t)col * 2)));
            } else if (R16(CH_FACE) == -1 &&
                       (int32_t)R16(CH_X) - (int32_t)R16(gColRightX + (int32_t)col * 2) < 0x22) {
                seq = (int16_t)((int16_t)(R16(CH_X) - R16(gColRightX + (int32_t)col * 2)) - 0x22);
            } else {
                seq = 0;
            }
            if (seq != 0) W16(CH_X, AddCharX(seq));
            seq = 0x7d;
        } else {
            seq = 0x4f;
        }
    }
    if (R16(gInDown) != 0) W16(gInDown, seg4_02190a());
    if (seq != -1) JumpSeq(seq);
    return 0;
}

/* 0002b19a: control in a death/dying position (and the shadow merging with the kid) */
uint32_t FUN_0002b19a(void) {
    ENTRY_NOLINK(FN_FUN_0002b19a, 8);            /* movem D3/D4 */
    if (S16(IsDeadPos(R16(CH_FRAME))) != 0) {
        if (LEVEL_TYPE() == 1 && R16(CH_FRAME) == 0xb9 && R16(CH_LIFE) != 0) {
            DecLifePts(R16(CH_LIFE));
            return 0;
        }
        if (R16(CH_TYPE) != 1 || R16(CH_FLAG18) != 0) return 0;
        int32_t d;
        if (S16(FUN_0001ff82()) < 0) d = -(int32_t)S16(FUN_0001ff82());
        else d = S16(FUN_0001ff82());
        if (d <= 2) FUN_0002009e();
        return 0;
    }
    if (R16(CH_TYPE) != 0 || R16(CH_ID) == 10) return 0;
    int16_t f = R16(CH_FRAME);
    if (f == 0xb3 || f == 0xb7) return 0;
    int16_t id = R16(CH_ID);                     /* D4 */
    seg4_022516();
    FUN_000225d2();
    if (S16(DecLifePts(1)) == 0) W16(CH_MAXLIFE, R16(CH_MAXLIFE) - 1);
    mcopy(gKid, gChar, CHAR_SIZE);
    LoadShad(id);
    DecLifePts(1);
    return 0;
}

/* 0002b272: control while standing (frames 0xf, 0x32-0x34) */
uint32_t FUN_0002b272(void) {
    ENTRY_NOLINK(FN_FUN_0002b272, 12);           /* movem D3-D5 */
    int16_t lt = LEVEL_TYPE();
    if (lt == 2 || lt == 6) seg4_01fc44();
    int16_t more = 1;                            /* D4 */
    if (R16(gInBtn) == -1 && S16(TryPickUp()) != 0) {
        more = 0;
    } else if (R16(CH_ID) == 10) {
        if ((R16(CH_SWORD) != -1 || (LEVEL_TYPE() == 6 && R16(CH_TYPE) == 1)) &&
            R16(gInBtn) == -2 && R16(gInFwd) == 0 && R16(gInBack) == 0 &&
            R16(gInUp) == 0 && R16(gInDown) == 0) {
            W16(gInBtn, 2);
            seg4_02190a();
            DoEngarde();
            W16(CH_OPPIDX, GetOppIdxClosestToKid(R16(CH_FACE)));
            if (R16(CH_OPPIDX) == -1) W16(CH_OPPIDX, 0);
            more = 0;
        } else if (R16(OPP_F32) >= 2 && R16(CH_TYPE) != 1 && R16(CH_F34) != 0xd &&
                   R16(OPP_TYPE) != 6) {
            int16_t d = S16(seg4_0224be());      /* D3 */
            if (d >= -0xf && d < 0x14b &&
                (-(int32_t)R16(GH(0x104a9e))) >> 1 > (int32_t)d && d >= -0x17) {
                FUN_0002b724();
                more = 0;
            }
        }
    } else if (R16(gInDown) < 0 && R16(gInFwd) < 0) {
        DoEngarde();
        more = 0;
    }
    if (more != 0) {
        int16_t btn = R16(gInBtn);
        if (btn == -1 || btn == 1) {
            more = 0;
            if (R16(gInBack) < 0) FUN_0002b724();
            else if (R16(gInUp) < 0) FUN_0002b476();
            else if (R16(gInDown) < 0) seg6_02b4ce();
            else if (R16(gInFwd) < 0 && R16(gInUp) == 0 && R16(gInDown) == 0) DoStepFwd(0);
            else if (R16(gInFwd) == 0) more = 1;
        }
        if (more != 0) {
            if (R16(gInFwd) < 0) {
                if (R16(gInUp) < 0) seg6_02bc2a();
                else seg6_02b7e0();
            } else if (R16(gInBack) < 0) {
                FUN_0002b724();
            } else if (R16(gInUp) < 0) {
                if (R16(gInFwd) < 0) seg6_02bc2a();
                else FUN_0002b476();
            } else if (R16(gInDown) < 0) {
                seg6_02b4ce();
            } else if (R16(gInFwd) != 0) {
                seg6_02b7e0();
            }
        }
    }
    if (R16(CH_JSEQ) != 5 && R16(CH_TYPE) == 0 && R16(CH_F36) != 0) W16(CH_F36, 0);
    return 0;
}

/* 0002b476: up pressed while standing: climb the stairs (exit door) or jump up */
uint32_t FUN_0002b476(void) {
    ENTRY_NOLINK(FN_FUN_0002b476, 4);            /* move.l D3 */
    if (GS16(0x2198) != R16(gCurRoom) && R16(CH_TYPE) == 0 &&
        (S16(FUN_00021076()) == 0x11 || S16(FUN_00021518()) == 0x11 ||
         S16(GetCellsInFront(1)) == 0x11) &&
        S16(seg5_02580e()) != 0) {
        FUN_0002b598();
        return 0;
    }
    if (R16(gInFwd) != 0) seg6_02bc2a();
    else DoJumpUp();
    return 0;
}

/* 0002b4ce: down pressed while standing: climb down an edge or crouch */
uint32_t seg6_02b4ce(void) {
    ENTRY_NOLINK(FN_seg6_02b4ce, 16);            /* movem D3-D6 */
    int16_t crouch = 1;                          /* D4 */
    W16(gInDown, 1);
    if (S16(IsSpace(S16(GetCellsInFront(1)))) != 0 && S16(seg4_021562()) < 5) {
        W16(CH_X, AddCharX(0xf));
        FUN_00020542();
        crouch = 0;
    } else if (S16(IsSpace(S16(FUN_00021518()))) != 0 && S16(seg4_021562()) >= 0xb) {
        int16_t behind = S16(FUN_00021518());    /* D3 */
        int16_t spec = R16(gCellSpec);           /* D5 */
        int16_t here = S16(FUN_00021076());      /* D6 */
        if (S16(IsLedge(behind, here, spec, R16(gCellSpec))) != 0) {
            if (R16(CH_FACE) != 0 && S16(FUN_00021076()) == 4 && (RU16(gCellSpec) >> 2) < 0xc) {
                /* keep crouch */
            } else {
                W16(CH_X, AddCharX((int16_t)(S16(seg4_021562()) - 0x26)));
                JumpSeq(0x44);
                crouch = 0;
            }
        }
    }
    if (crouch != 0) FUN_0002b6a8();
    return 0;
}

/* 0002b598: start climbing the stairs of the exit */
uint32_t FUN_0002b598(void) {
    ENTRY_NOLINK(FN_FUN_0002b598, 0);
    W16(CH_X, R16(gColLeftX + (int32_t)R16(gCellCol) * 2) - 3);
    W16(CH_FACE, -1);
    JumpSeq(0x46);
    return 0;
}

/* 0002b5c0: climbing the exit stairs: animate the exit door trob */
uint32_t StairClimbing(void) {
    ENTRY_LINK(FN_StairClimbing, 4, 20);         /* link -4; movem D3-D7 */
    int16_t cell = S16(IndexCell(R16(CH_COL), R16(CH_ROW)));
    int16_t room = R16(CH_ROOM);
    W16(GH(0x103c14), room);
    int16_t piece = R16(R32(gGameState) + (uint32_t)(uint16_t)room * 0x3c - 0x3c + (int32_t)cell * 2);
    W16(GH(0x103c12), piece == 0x11 ? cell : (int16_t)(cell + 1));
    if (R16(CH_FRAME) == 0xe0) FUN_00028cc0();
    RedrawExit();
    return 0;
}

/* 0002b654: control while turning (frames 0x2d-0x31) */
uint32_t FUN_0002b654(void) {
    ENTRY_NOLINK(FN_FUN_0002b654, 0);
    if (R16(CH_ID) == 10 && R16(CH_JSEQ) == 5) seg4_01fc44();
    if (R16(CH_FRAME) == 0x30 && R16(gInBtn) >= 0 && R16(gInFwd) != 0 &&
        R16(gInUp) == 0 && R16(gInDown) == 0) {
        JumpSeq(0x2b);
        if (R16(CH_TYPE) == 0 && R16(CH_F36) != 0) W16(CH_F36, 0);
    }
    return 0;
}

/* 0002b6a8: crouch (or the shadow's special move) */
uint32_t FUN_0002b6a8(void) {
    ENTRY_NOLINK(FN_FUN_0002b6a8, 16);           /* movem D3-D6 */
    int16_t seq;                                 /* D4 */
    if (R16(CH_TYPE) == 1 && R16(gInDown) != 0) seq = S16(FUN_0001f9ee());
    else seq = -1;
    if (seq == -1) seq = 0x32;
    if (LEVEL_TYPE() == 4) {
        int16_t c = S16(GetCellsInFront(1));
        if (c == 0xc || c == 0xd) {
            int16_t d = S16(seg4_02397e());
            if (d < 8) W16(CH_X, AddCharX((int16_t)-(int16_t)(8 - d)));
        }
    }
    JumpSeq(seq);
    W16(gInDown, seg4_02190a());
    return 0;
}

/* 0002b724: turn around */
uint32_t FUN_0002b724(void) {
    ENTRY_NOLINK(FN_FUN_0002b724, 20);           /* movem D3-D6/A2 */
    W16(gInBack, seg4_02190a());
    W16(CH_FLAG18, 1);
    int16_t dist = S16(seg4_021562());           /* D5 */
    if (S16(GetCellsInFront(1)) == 4 && S16(seg4_0235de()) != 0) {
        int16_t min;                             /* D6 */
        if (LEVEL_TYPE() == 3) min = 0xb;
        else if (R16(gCurRoom) == 9 && LEVEL_NUM() == 8) min = -100;
        else min = 6;
        if (dist < min) W16(CH_X, AddCharX((int16_t)(dist - min)));
    }
    maddr gs = R32(gGameState);
    int16_t lt = R16(gs + 0x2186);
    if (lt == 2) {
        seg4_01fa82();
    } else if (lt == 6) {
        int16_t room = R16(CH_ROOM);
        if ((room != 1 && room != 2) || R16(gs + 0x218a) != 0xe) seg4_01fa82();
    }
    if (R16(CH_JSEQ) != 0x47) JumpSeq(5);
    return 0;
}

/* 0002b7e0: forward pressed: step (near an edge) or start running */
uint32_t seg6_02b7e0(void) {
    ENTRY_NOLINK(FN_seg6_02b7e0, 8);             /* movem D3/D4 */
    int16_t d = S16(seg4_02397e());
    if (R16(GH(0x104a8a)) == 1 && d < 0x1b) {
        if (R16(gInFwd) < 0) DoStepFwd(0);
    } else {
        JumpSeq(1);
    }
    W16(gInFwd, seg4_02190a());
    return 0;
}

/* 0002b824: control while running (frames < 0xf) */
uint32_t FUN_0002b824(void) {
    ENTRY_NOLINK(FN_FUN_0002b824, 4);            /* move.l D3 */
    int16_t f;
    if (R16(gInFwd) == 0 && R16(gInBack) == 0 && ((f = R16(CH_FRAME)) == 7 || f == 0xb)) {
        W16(gInFwd, seg4_02190a());
        JumpSeq(0xd);
    } else if (R16(gInBack) != 0) {
        W16(gInBack, seg4_02190a());
        JumpSeq(6);
    } else if (R16(gInUp) != 0) {
        if (R16(gInUp) < 0) DoRunJump(4);
    } else if (R16(gInDown) < 0) {
        W16(gInDown, seg4_02190a());
        JumpSeq(0x1a);
    }
    if (R16(gInFwd) < 0) W16(gInFwd, 1);
    return 0;
}

/* 0002b8b6: step forward (n = distance to the edge, 0 = compute it) */
uint32_t DoStepFwd(int16_t n) {
    ENTRY_LINK(FN_DoStepFwd, 0, 8);              /* link 0; movem D3/D4 */
    maddr ap = a6 + 8;                           /* the original updates its argument slot */
    W16(ap, n);
    W16(GH(0x104944), 1);
    W16(gInFwd, 1);
    W16(gInBtn, 1);
    if (R16(ap) == 0) W16(ap, seg4_02397e());
    W16(ap, R16(ap) - 4);
    int16_t seq;                                 /* D4 */
    if (R16(ap) > 0) {
        W16(CH_FLAG18, 1);
        if (R16(ap) >= 0x2a) {
            seq = 0x2a;
        } else {
            if (R16(ap) < 3) seq = -1;
            else seq = (int16_t)(R16(ap) / 3 + 0x1c);
            int16_t rem = (int16_t)(R16(ap) % 3);
            if (rem != 0) W16(CH_X, AddCharX(rem));
        }
    } else if (R16(CH_FLAG18) != 0 &&
               (R16(GH(0x104a8a)) != 1 || R16(gCellId) == 0xc || R16(gCellId) == 0xd)) {
        W16(CH_FLAG18, 0);
        seq = 0x2c;
    } else {
        seq = 0x2a;
    }
    if (seq != -1) JumpSeq(seq);
    W16(GH(0x104944), 0);
    return 0;
}

/* 0002b990: pick up the object (flask 10 / sword 0x16) under or in front of the kid */
uint32_t TryPickUp(void) {
    ENTRY_LINK(FN_TryPickUp, 0x14, 12);          /* link -0x14; movem D3-D5 */
    int16_t found = 0;                           /* D4 */
    int16_t c = S16(FUN_00021076());
    if (c == 10 || c == 0x16) {
        mcopy(a6 - 0x14, gCellId, 0x14);         /* saved and restored unchanged */
        found = 1;
        mcopy(gCellId, a6 - 0x14, 0x14);
    }
    if (found != 0 || S16(GetCellsInFront(1)) == 10 || S16(GetCellsInFront(1)) == 0x16)
        found = S16(PickItUp());
    return (uint16_t)found;
}

/* 0002ba10: pick up the object in gCellId (the last cell read); returns 0 if not now */
uint32_t PickItUp(void) {
    ENTRY_LINK(FN_PickItUp, 0x14, 20);           /* link -0x14; movem D3-D7 */
    int16_t obj = R16(gCellId);                  /* D6 */
    mcopy(a6 - 0x14, gCellId, 0x14);
    int16_t ok = 1;                              /* D4 */
    int16_t dist = S16(seg4_021562());           /* D5 */
    int16_t here = S16(FUN_00021076());          /* D7 */
    if (R16(CH_FRAME) != 0x6d) {
        int16_t dx;
        if (here == obj) dx = (R16(CH_FACE) == 0) ? (int16_t)(dist - 0x25) : (int16_t)(dist - 0x1a);
        else dx = (R16(CH_FACE) == 0) ? (int16_t)(dist + 1) : (int16_t)(dist + 0x18);
        W16(CH_X, AddCharX(dx));
        FUN_0002b6a8();
        return (uint16_t)ok;
    }
    int16_t js = R16(CH_JSEQ);
    if (js != 0x32 && (js == 0x14 || R16(gInBtn) != -1)) return 0;
    if (R16(CH_FACE) == 0) {
        if (dist < 0x24 && here != obj) W16(CH_X, AddCharX((int16_t)(dist - 6)));
    } else {
        if (here != obj) dist = (int16_t)(dist + 0x33);
        W16(CH_X, AddCharX((int16_t)(dist - 0x22)));
    }
    mcopy(gCellId, a6 - 0x14, 0x14);
    if (obj == 0x16) {
        if (R16(CH_ROOM) == 9 && LEVEL_NUM() == 8 && R16(GH(0x10300c)) == 0) {
            W16(CH_X, AddCharX(-0x17));
            if (R16(CH_FACE) == -1) W16(CH_X, AddCharX(4));
            JumpSeq(0xed);
            AddSong(0xb5);
        } else {
            JumpSeq(0x5b);
            RemoveObj(-1);
            int16_t set = (int16_t)(R16(gCellSpec) & 3);
            W16(GH(0x102ff6), set);
            LoadSwordSet(set);
            if (R16(CH_ROOM) != 9 || LEVEL_NUM() != 8) AddSong(0xb7);
        }
    } else if (obj == 10) {
        RemoveObj((int16_t)((uint32_t)(RU16(gCellSpec) & 0xe0) >> 5));
        JumpSeq(0x4e);
    }
    return (uint16_t)ok;
}

/* 0002bbf6: control at the start of a run (frames 1-3): up + forward = running jump */
uint32_t FUN_0002bbf6(void) {
    ENTRY_NOLINK(FN_FUN_0002bbf6, 4);            /* move.l D3 */
    if (R16(gInUp) < 0 && R16(gInFwd) < 0) {
        uint32_t r = seg4_02190a();
        W16(gInFwd, r);
        W16(gInUp, r);
        seg6_02bc2a();
    }
    return 0;
}

/* 0002bc1e: control at frames 0x43-0x45: forward = standing jump */
uint32_t FUN_0002bc1e(void) {
    ENTRY_NOLINK(FN_FUN_0002bc1e, 0);
    if (R16(gInFwd) == 0) return 0;
    return seg6_02bc2a();
}

/* 0002bc2a: standing jump */
uint32_t seg6_02bc2a(void) {
    ENTRY_NOLINK(FN_seg6_02bc2a, 0);
    W16(gInFwd, 1);
    W16(gInUp, 1);
    return JumpSeq(3);
}

/* 0002bc40: up pressed: grab the ledge above, in front or behind, or jump straight up */
uint32_t DoJumpUp(void) {
    ENTRY_LINK(FN_DoJumpUp, 4, 20);              /* link -4; movem D3-D7 */
    W16(gInUp, seg4_02190a());
    int16_t c1 = S16(FUN_00021ac8());            /* D6 */
    int16_t spec = R16(gCellSpec);               /* D7 */
    int16_t c2 = S16(seg4_021b0e());             /* D5 */
    if (c2 == 4) W16(gCellSpec, 0xb8);
    int16_t rock = S16(GetSlideRockX(R16(CH_ROOM), R16(CH_ROW)));   /* -4(A6) */
    if (rock != 0) {
        int32_t d = (int32_t)rock - (int32_t)R16(CH_X);
        if (d < 0) d = -d;
        if (d > 0x33) rock = 0;
    }
    if (rock == 0 && S16(IsLedge(c1, c2, spec, R16(gCellSpec))) != 0) {
        DoJumpHang(0);
        return 0;
    }
    c1 = S16(seg4_021ae0());
    spec = R16(gCellSpec);
    c2 = S16(FUN_00021ac8());
    if (c2 == 4) W16(gCellSpec, 0xb8);
    if (rock == 0 && S16(IsLedge(c1, c2, spec, R16(gCellSpec))) != 0) {
        DoJumpBack();
        return 0;
    }
    DoJumpHigh();
    return 0;
}

/* 0002bd18: jump up to the ledge behind */
uint32_t DoJumpBack(void) {
    ENTRY_LINK(FN_DoJumpBack, 4, 4);             /* link -4; move.l D3 */
    int16_t dist = S16(seg4_021562());
    GetSlideRockX(R16(CH_ROOM), R16(CH_ROW));
    if (dist < 0x18) {
        DoJumpHigh();
    } else if (S16(IsSpace(S16(FUN_00021518()))) != 0) {
        FUN_0002bd88();
    } else {
        W16(CH_X, AddCharX((int16_t)(dist - 0x33)));
        FUN_00020542();
        DoJumpHang(1);
    }
    return 0;
}

/* 0002bd88: jump up backwards (turning to the ledge behind) */
uint32_t FUN_0002bd88(void) {
    ENTRY_NOLINK(FN_FUN_0002bd88, 4);            /* move.l D3 */
    FUN_00021ac8();
    int16_t dist = S16(seg4_021562());
    W16(CH_X, AddCharX((int16_t)(dist - 0x29)));
    JumpSeq(0x10);
    return 0;
}

/* 0002bdb4: jump straight up (0xe: can reach the ceiling, 0x1c: high jump) */
uint32_t DoJumpHigh(void) {
    ENTRY_LINK(FN_DoJumpHigh, 4, 20);            /* link -4; movem D3-D7 */
    W16(gInUp, seg4_02190a());
    int16_t d = S16(seg4_02397e());
    if (d < 6 && R16(GH(0x104a8a)) == 1) W16(CH_X, AddCharX((int16_t)(d - 10)));
    int16_t x = R16(CH_X);                       /* -2(A6) */
    W16(CH_X, AddCharX(8));
    int16_t col = S16(GetCharCol(S16(seg4_021542())));
    int16_t above = S16(ReadCell(R16(CH_ROOM), col, (int16_t)(R16(CH_ROW) - 1)));
    W16(CH_X, x);
    int high;
    if (S16(IsSpace(above)) != 0 && (R16(CH_TYPE) != 1 || S16(FUN_0002bed2()) != 0)) {
        high = 1;
    } else {
        int16_t lt = LEVEL_TYPE();               /* D4 */
        int16_t room;                            /* D5 */
        if (lt == 5 && R16(CH_ROOM) == 0xf) high = 1;
        else if ((room = R16(CH_ROOM)) == 7 && lt == 6 &&
                 (R16(CH_ROW) != 2 || (R16(CH_COL) != 2 && R16(CH_COL) != 6))) high = 1;
        else if (room == 8 && lt == 6) {
            int16_t c;
            high = (R16(CH_ROW) != 2 || (c = R16(CH_COL)) >= 0 || c <= 4);
        } else high = 0;
    }
    JumpSeq(high ? 0x1c : 0xe);
    return 0;
}

/* 0002bed2: 0 if the falling mob of type 10 is in the character's cell, else 1 */
uint32_t FUN_0002bed2(void) {
    ENTRY_NOLINK(FN_FUN_0002bed2, 8);            /* movem D3/A2 */
    uint32_t mob = GetMobTypeItem(10, 1);
    if (mob == 0) return 1;
    if ((int16_t)(R16(mob) / 0x33) == R16(CH_COL) &&
        (int16_t)(R16(mob + 2) / 0x78) == R16(CH_ROW)) return 0;
    return 1;
}

/* 0002bf20: control while hanging (frames 0x57-0x63): climb up, drop, keep hanging */
uint32_t FUN_0002bf20(void) {
    ENTRY_NOLINK(FN_FUN_0002bf20, 4);            /* move.l D3 */
    if (R16(CH_ALIVE) >= 0) { seg6_02c0c4(); return 0; }
    if (R16(GH(0x10306a)) == 0 && R16(gInUp) != 0) { seg6_02bfd8(); return 0; }
    if (R16(gInBtn) == 0 || R16(CH_F34) == 0xb) {
        W16(CH_F34, 0);
        seg6_02c0c4();
        return 0;
    }
    int16_t c = S16(FUN_00021076());
    if ((R16(CH_ACTION) != 6 && S16(IsWall(c)) != 0) ||
        (R16(CH_FACE) == -1 && c == 4) ||
        (LEVEL_TYPE() == 5 && R16(CH_ROOM) == 0xf)) {
        JumpSeq(0x19);
        return 0;
    }
    if (S16(IsSpace(S16(FUN_00021ac8()))) == 0) return 0;
    if (LEVEL_TYPE() == 5 && S16(RoofHangingFromShip()) != 0) return 0;
    seg6_02c0c4();
    return 0;
}

/* 0002bfd8: climb up from hanging */
uint32_t seg6_02bfd8(void) {
    ENTRY_NOLINK(FN_seg6_02bfd8, 16);            /* movem D3-D6 */
    int16_t seq;                                 /* D4 */
    if (LEVEL_NUM() == 5 && R16(CH_ROOM) == 10 && R16(CH_SWORD) != -1) {
        seq = 0xec;
        W16(CH_SWORD, -1);
    } else {
        seq = 10;
    }
    uint32_t r = seg4_02190a();
    W16(gInBtn, r);
    W16(gInUp, r);
    int16_t c = S16(FUN_00021ac8());             /* D6 */
    int16_t lim = (LEVEL_TYPE() == 3 && R16(CH_FACE) == -1) ? 0x1e : 0xc;   /* D5 */
    int blocked = 0;
    if (R16(CH_TYPE) != 1 && c == 4 &&
        (int32_t)((RU16(gCellSpec) & 0xff) >> 2) < (int32_t)lim) {
        int16_t face = R16(CH_FACE);
        if (face == -1 || (face == 0 && LEVEL_TYPE() == 3)) blocked = 1;
    }
    if (!blocked && LEVEL_TYPE() == 2 && S16(SlideRockInCell(0)) != 0) blocked = 1;
    if (blocked) seq = 0x49;
    else if (LEVEL_TYPE() == 5 && S16(RoofHangingFromShip()) != 0) seq = 0x3b;
    uint32_t j = JumpSeq(seq);
    return (j & 0xffff0000) | RU16(CH_FACE);
}

/* 0002c0c4: let go: fall, or drop to the floor below */
uint32_t seg6_02c0c4(void) {
    ENTRY_NOLINK(FN_seg6_02c0c4, 20);            /* movem D3-D7 */
    W16(gInDown, seg4_02190a());
    int16_t dx = 0;                              /* D5 */
    int16_t behind = S16(FUN_00021518());        /* D3 */
    int16_t here = S16(FUN_00021076());          /* D4 */
    int16_t seq;
    if (S16(IsSpace(behind)) != 0 && S16(IsSpace(here)) != 0) {
        seq = (S16(seg6_02c1be()) != 0) ? 0x63 : 0x17;
    } else {
        int16_t landH = S16(IsLandOnable(here));     /* D6 */
        int16_t landB = S16(IsLandOnable(behind));   /* D7 */
        if (landH == 0 && landB == 0) {
            if (S16(IsFloor(behind)) != 0 && S16(IsWall(here)) != 0) landB = 1;
            else if (S16(IsFloor(here)) != 0 && S16(IsWall(behind)) != 0) landH = 1;
        }
        if (S16(IsWall(here)) != 0) {
            dx = -0x17;
            seq = (landB != 0) ? 0xb : 0x17;
        } else if (landH == 0 && landB != 0) {
            dx = -0xc; seq = 0xb;
        } else if (landH != 0 && landB == 0) {
            dx = 10; seq = 0xb;
        } else if (landH != 0 && landB != 0) {
            seq = 0xb;
        } else {
            seq = 0x17;
        }
        if (dx != 0) W16(CH_X, AddCharX(dx));
    }
    JumpSeq(seq);
    return 0;
}

/* 0002c1be: run AnimChar once to test whether the frame number goes back (1) or on (0);
 * the sequence state is restored */
uint32_t seg6_02c1be(void) {
    ENTRY_NOLINK(FN_seg6_02c1be, 20);            /* movem D3-D7 */
    int16_t jseq = R16(CH_JSEQ);
    int16_t pos = R16(CH_SEQPOS);
    int16_t seq = R16(CH_SEQ);
    uint16_t frame = RU16(CH_FRAME);
    AnimChar();
    int16_t r = (RU16(CH_FRAME) <= frame) ? 1 : 0;
    W16(CH_SEQ, seq);
    W16(CH_SEQPOS, pos);
    W16(CH_JSEQ, jseq);
    return (uint16_t)r;
}

/* 0002c1fe: jump and grab the ledge (back = 1: the ledge behind) */
uint32_t DoJumpHang(int16_t back) {
    ENTRY_LINK(FN_DoJumpHang, 0, 4);             /* link 0; move.l D3 */
    int16_t dist = S16(seg4_021562());
    if (back != 0 && dist == 0x33) dist = 0;
    dist = (int16_t)(dist - 0x15);               /* same on both facings */
    W16(CH_X, AddCharX(dist));
    JumpSeq(0x18);
    return 0;
}

/* 0002c254: running jump (seq 4) or the other long jump (seq 100), adjusting the take-off
 * point to the edge in front; returns 1 if the jump started */
uint32_t DoRunJump(int16_t seq) {
    ENTRY_LINK(FN_DoRunJump, 8, 20);             /* link -8; movem D3-D7 */
    uint16_t f;
    if (!((seq == 4 && (f = RU16(CH_FRAME)) >= 7 && f <= 0xe) ||
          (seq == 100 && ((f = RU16(CH_FRAME)) == 0xc0 || f == 0xc4))))
        return 0;
    int16_t count = 0;                           /* -8(A6) */
    int16_t dx0 = 0xd;                           /* D5 (0xd for both jumps) */
    int16_t x = S16(AddCharX(dx0));              /* D6 */
    int16_t col = S16(GetCharCol(x));            /* D7 */
    int16_t done = 0;                            /* -6(A6) */
    int16_t ok = 1;                              /* -4(A6) */
    do {
        col = (int16_t)(col + R16(gDirDX + (int32_t)R16(CH_FACE) * 2));
        int16_t c = S16(ReadCell(R16(CH_ROOM), col, R16(CH_ROW)));
        if (S16(IsAdvancableId(c, R16(gCellSpec))) == 0 && c != 0xb) {
            ok = 0;
        } else {
            count = (int16_t)(count + 1);
            if (count == 2) done = 1;
        }
    } while (done == 0 && ok != 0);
    if (ok == 0) {
        int16_t d = S16(GetDist1(x));
        d = (int16_t)(d + (int16_t)(count * 0x33));
        if (seq == 4) d = (int16_t)(d - 0x30);
        else d = (int16_t)(d - 0x4b);
        if (!(d >= -0x1f && d < 0xf)) {
            if (d < 0 && seq == 4) return 0;
            d = (int16_t)-dx0;
        }
        W16(CH_X, AddCharX((int16_t)(d + dx0)));
    }
    JumpSeq(seq);
    uint32_t r = seg4_02190a();
    W16(gInUp, r);
    return (r & 0xffff0000) | 1;
}

/* 0002c3ae: retreat one step in a sword fight */
uint32_t DoRetreat(void) {
    ENTRY_LINK(FN_DoRetreat, 4, 8);              /* link -4; movem D3/D4 */
    int16_t t = R16(CH_TYPE);
    if (t == 7 || t == 8 || t == 0xb) return 0;
    int16_t f = R16(CH_FRAME);
    if (f != 0x9e && f != 0xaa && f != 0xab) return 0;
    JumpSeq(t == 10 ? 0x68 : 0x39);
    W16(gInBack, seg4_02190a());
    W16(GH(0x103064), 0);
    return 0;
}

/* 0002c41a: advance one step in a sword fight */
uint32_t DoAdvance(void) {
    ENTRY_LINK(FN_DoAdvance, 4, 8);              /* link -4; movem D3/D4 */
    int16_t t = R16(CH_TYPE);
    if (t == 7 || t == 8 || t == 0xb) return 0;
    int16_t f = R16(CH_FRAME);
    if (f != 0x9e && f != 0xaa && f != 0xab) return 0;
    int16_t seq;
    if (t == 0 || t == 1) seq = 0x38;
    else if (R16(CH_TYPE) == 10) seq = 0x6c;
    else if (LEVEL_TYPE() == 5 && R16(OPP_ROOM) == 0x13 && R16(OPP_COL) <= 5) seq = -1;
    else seq = 0x56;
    if (seq != -1) JumpSeq(seq);
    W16(gInFwd, seg4_02190a());
    return 0;
}

/* 0002c4b8: draw the sword (en garde) */
uint32_t DoEngarde(void) {
    ENTRY_LINK(FN_DoEngarde, 4, 8);              /* link -4; movem D3/D4 */
    W16(gInFwd, seg4_02190a());
    W16(gInBtn, 2);
    W16(CH_SWORD, 1);
    int16_t seq;
    if (R16(CH_TYPE) == 0 || R16(CH_TYPE) == 1) {
        seq = S16(DoKidEngarde());
    } else {
        int16_t t = R16(CH_TYPE);
        if (t == 7 || t == 8) {
            seq = -1;
        } else {
            int16_t d = S16(seg4_02397e());
            if (d > 0 && d < 0x12) W16(CH_X, AddCharX((int16_t)(d - 0x12)));
            seq = 0x5a;
        }
    }
    if (seq != -1) JumpSeq(seq);
    return 0;
}

/* 0002c544: the kid (or shadow) draws his sword: choose the sequence and step back from
 * an edge or a sliding rock; returns the sequence (-1 = none) */
uint32_t DoKidEngarde(void) {
    ENTRY_LINK(FN_DoKidEngarde, 8, 24);          /* link -8; movem D3-D7/A2 */
    int16_t seq;                                 /* D7 */
    int16_t dx;                                  /* D6 */
    int16_t rock;                                /* -8(A6) */
    int16_t c;                                   /* D5 */
    uint32_t d0;
    uint16_t f = RU16(CH_FRAME);
    int16_t room = R16(CH_ROOM);
    if ((f >= 0xf6 && f <= 0x105) || ((room == 1 || room == 2) && LEVEL_NUM() == 0xe) ||
        LEVEL_TYPE() == 1) {
        seq = -1;
        goto tail;
    }
    if (R16(CH_TYPE) == 1 && ((room = R16(CH_ROOM)) == 7 || room == 8) && LEVEL_TYPE() == 6) {
        seq = S16(seg9_03a224());
        uint16_t i = 0;
        if (R16(GH(0x102e00)) != 0) {
            maddr p = GH(0x102e00);
            do {
                i++;
                p += CHAR_SIZE;
                if (R16(p) == 0) break;
            } while (i < 5);
        }
        W16(CH_SWORD, R16(GH(0x102e12) + (int32_t)(int16_t)i * CHAR_SIZE));
        goto tail;
    }
    seq = 0x37;
    dx = -1;
    rock = S16(GetSlideRockX(R16(CH_ROOM), R16(CH_ROW)));
    if (rock != 0) {
        int32_t d = (int32_t)S16(GetCharCol(rock)) - (int32_t)R16(CH_COL);
        if (d < 0) d = -d;
        if (d > 1) rock = 0;
    }
    c = (R16(CH_FACE) == 0) ? S16(FUN_00021076()) : 0;
    if (c == 4 && S16(seg4_0235de()) != 0) {
        dx = -0x33;
    } else {
        int toward = 0;
        if (rock != 0) {
            int16_t face = R16(CH_FACE);
            if (face == -1 && R16(CH_X) >= rock) toward = 1;
            else if (face == 0 && R16(CH_X) <= rock) toward = 1;
        }
        if (toward) {
            int16_t dist = S16(seg4_021562());
            int32_t d = (int32_t)rock - (int32_t)R16(CH_X);
            if (d < 0) d = -d;
            dx = (int16_t)((int16_t)d - dist);
            if (dx == -1) dx = -3;
            dx = (int16_t)(dx - 6);
        } else {
            c = S16(GetCellsInFront(1));
            if (S16(IsFloor(c)) == 0 || c == 0xb || (c == 4 && S16(seg4_0235de()) != 0)) dx = 0;
        }
    }
    if (dx == -1) goto tail;
    c = S16(FUN_00021518());
    if (S16(IsFloor(c)) == 0 || c == 0xb || (c == 4 && S16(seg4_0235de()) != 0)) goto nofight;
    if (rock != 0) {
        int16_t face = R16(CH_FACE);
        if (!(face == -1 && R16(CH_X) >= rock)) {
            if (face != 0 || R16(CH_X) > rock) goto nofight;
        }
    }
    if (R16(CH_F34) == 4) goto nofight;
    {
        int16_t d = (int16_t)(S16(seg4_02397e()) + dx);
        if (d < 0x38) W16(CH_X, AddCharX((int16_t)(d - 0x38)));
    }
    goto tail;
nofight:
    W16(CH_SWORD, 0);
    seq = -1;
tail:
    if (seq != -1 && seq != 0xf2) {
        d0 = AddSound(0xe);
    } else {
        d0 = 0xffffffff;
        if (R16(CH_SWORD) != -1) W16(CH_SWORD, 0);
    }
    return (d0 & 0xffff0000) | (uint16_t)seq;
}

/* 0002c7a6: control while en garde (sword drawn) */
uint32_t FUN_0002c7a6(void) {
    ENTRY_NOLINK(FN_FUN_0002c7a6, 16);           /* movem D3-D6 */
    if (R16(CH_ID) == 10) {
        int16_t idx = -1;                        /* D5 */
        int16_t cur = R16(CH_OPPIDX);            /* D3 */
        if (cur != -1) {
            uint16_t room = RU16(CH_ROOM);
            int32_t n = (room != 0) ? (int32_t)R16(R32(gGameState) + 0x20e6 + (uint32_t)room * 0xc0) : 0;
            if (n > (int32_t)cur && R16(GH(0x102e16) + (int32_t)cur * CHAR_SIZE) > 0) idx = cur;
        }
        if (idx == -1) {
            idx = S16(GetOppIdxClosestToKid(1));
            if (idx == -1) idx = 0;
            else W16(CH_OPPIDX, idx);
        }
        LoadShadAsOpp(idx);
    }
    if (R16(CH_ACTION) >= 2) return 0;
    int16_t delta = R16(CH_LIFEDELTA);
    int32_t ad = delta < 0 ? -(int32_t)delta : (int32_t)delta;
    if (ad >= (int32_t)R16(CH_LIFE)) return 0;
    int16_t more = 1;                            /* D5 */
    int16_t c = S16(FUN_00021076());
    if (c == 0xb || c == 0xf || R16(OPP_F32) >= 2) {
        int16_t d = S16(seg4_0224be());          /* D4: distance to the opponent */
        if (d >= -0xf && d < 0x14b) {
            OnAlert();
            more = 0;
        } else if (d < 0) {
            int16_t ot;
            if (d >= -6 || (d >= -7 && ((ot = R16(OPP_TYPE)) == 7 || ot == 8))) {
                OnAlert();
            } else if (R16(CH_TYPE) != 0xb) {
                int16_t room;
                if ((R16(CH_ID) == 10 && R16(GH(0x102ff0)) == 0) || R16(OPP_TYPE) == 6) {
                    OnAlert();
                } else if (R16(CH_ROW) != 0 && LEVEL_NUM() == 5 &&
                           ((room = R16(CH_ROOM)) == 10 || room == 7 || room == 0xc) &&
                           S16(seg8_03426e()) != 0) {
                    OnAlert();
                } else {
                    seg6_02cc78();
                }
            }
            more = 0;
        }
    }
    int16_t btn;
    if (R16(CH_ID) == 10 && R16(gInBack) < 0 && ((btn = R16(gInBtn)) == -1 || btn == 1) &&
        !(LEVEL_NUM() == 5 && R16(CH_ROOM) == 10 && R16(GH(0x104ed0)) > 0)) {
        seg6_02cc78();
        W16(gInBtn, 2);
        W16(gInBack, seg4_02190a());
        W16(GH(0x102ff0), R16(GH(0x102ff0)) == 0 ? 1 : 0);
        W16(CH_OPPIDX, GetOppIdxClosestToKid((int16_t)~R16(CH_FACE)));
    } else if (more != 0) {
        if (R16(CH_ID) != 10) OnAlert();
        else if (R16(gInDown) < 0) FUN_0002cbd2();
        else OnAlert();
    }
    return 0;
}

/* 0002c9d2: is there a live opponent in the kid's room worth drawing the sword for?
 * (the current character record is saved and restored around the search) */
uint32_t OppToFight(void) {
    ENTRY_LINK(FN_OppToFight, 0x40, 24);         /* link -0x40; movem D3-D6/A3-A4 */
    int16_t found = 0;                           /* D4 */
    mcopy(a6 - 0x40, gChar, CHAR_SIZE);
    FUN_000225d2();
    if (R16(CH_F34) != 0xd && R16(CH_TYPE) != 1) {
        int16_t act = R16(CH_ACTION);
        if (act != 3 && act != 4 && S16(IsDeadPos(R16(CH_FRAME))) == 0) {
            maddr p = gOppTab;                   /* A4 */
            uint16_t room = RU16(KID_ROOM);
            int16_t n = (room != 0) ? R16(R32(gGameState) + (uint32_t)room * 0xc0 + 0x20e6) : 0;
            int16_t i = 0;                       /* D5 */
            if (n != 0) {
                do {
                    if (R16(p + 0x1c) < 0 && R16(p + 0x32) > 0 && R16(p + 8) != 0xb) {
                        int16_t t = R16(p + 8);
                        if (t == 7 || t == 8) {
                            maddr info = GetOppInfoIdx(R16(p + 0x16), R16(p));
                            int16_t k = R16(info + 0x1c);
                            found = (k == 1 || k == 3) ? 1 : 0;
                        } else {
                            found = 1;
                        }
                    }
                    i = (int16_t)(i + 1);
                    p += CHAR_SIZE;
                } while (found == 0 && (int32_t)i < (int32_t)(uint16_t)n);
            }
        }
    }
    mcopy(gChar, a6 - 0x40, CHAR_SIZE);
    return (uint16_t)found;
}

/* 0002cafc: en garde control: strike, block, advance, retreat, put the sword away */
uint32_t OnAlert(void) {
    ENTRY_LINK(FN_OnAlert, 4, 4);                /* link -4; move.l D3 */
    int16_t t = R16(CH_TYPE);
    if (t == 7 || t == 8) {
        FUN_00031816();
    } else if (R16(CH_FRAME) == 0xa1 && R16(gInBtn) >= 0) {
        JumpSeq(0x39);
        if (R16(CH_TYPE) == 0) W16(CH_X, AddCharX(-9));
    } else if (R16(gInBtn) == -2) {
        if (R16(CH_TYPE) == 0 || R16(CH_TYPE) == 1) W16(GH(0x104964), 0xf);
        DoStrike();
    } else if (R16(gInDown) < 0) {
        int16_t f = R16(CH_FRAME);
        if (f == 0x9e || f == 0xaa || f == 0xab) FUN_0002cbd2();
    } else if (R16(gInUp) < 0) {
        FUN_0002ce6a();
    } else if (R16(gInFwd) < 0) {
        DoAdvance();
    } else if (R16(gInBack) < 0 && R16(gInBtn) == 0) {
        DoRetreat();
    }
    return 0;
}

/* 0002cbd2: put the sword away (down pressed while en garde) */
uint32_t FUN_0002cbd2(void) {
    ENTRY_NOLINK(FN_FUN_0002cbd2, 0);
    W16(CH_SWORD, 0);
    W16(gInDown, seg4_02190a());
    if (R16(CH_TYPE) == 0 || R16(CH_TYPE) == 1) {
        W16(GH(0x104966), 9);
        if (S16(OppToFight()) != 0) JumpSeq(0x5d);
        else JumpSeq(0x5c);
    } else if (R16(CH_TYPE) == 10) {
        JumpSeq(0x5a);
    } else {
        FUN_0002cc2e();
    }
    return 0;
}

/* 0002cc2e: a guard puts the sword away, or retreats when standing on an edge */
uint32_t FUN_0002cc2e(void) {
    ENTRY_NOLINK(FN_FUN_0002cc2e, 8);            /* movem D3/D4 */
    int16_t seq = 0x4d;
    if (S16(IsSpace(S16(GetCellsInFront(1)))) != 0 && S16(IsSpace(S16(FUN_00021518()))) == 0) {
        DoRetreat();
        seq = -1;
    }
    if (seq != -1) JumpSeq(seq);
    return 0;
}

/* 0002cc78: back away / the heads detach (the opponent is too close) */
uint32_t seg6_02cc78(void) {
    ENTRY_NOLINK(FN_seg6_02cc78, 8);             /* movem D3/D4 */
    int16_t t = R16(CH_TYPE);
    if (t == 7 || t == 8) {
        int16_t js = R16(CH_JSEQ);
        if (js == 0x9c || js == 0xa6) return 0;
        if (S16(IsHeadLockedOn(R16(CH_ROOM), R16(CH_ID))) != 0) return 0;
        JumpSeq(0x9c);
    } else if (R16(CH_TYPE) == 0 || R16(CH_TYPE) == 1) {
        FUN_0002ccf8();
    } else {
        uint16_t f;
        if (R16(CH_TYPE) == 10 && (f = RU16(CH_FRAME)) >= 0xd5 && f <= 0xd9) return 0;
        JumpSeq(0x3c);
    }
    return 0;
}

/* 0002ccf8: the kid steps back en garde (keeping clear of an edge behind) */
uint32_t FUN_0002ccf8(void) {
    ENTRY_NOLINK(FN_FUN_0002ccf8, 8);            /* movem D3/D4 */
    if (LEVEL_NUM() == 5 && R16(CH_ROOM) == 10 && R16(GH(0x104ed0)) > 0) return 0;
    int16_t d = -1;                              /* D4 */
    int16_t c = S16(GetCellsInFront(1));
    if (S16(IsFloor(c)) == 0 || (c == 4 && S16(seg4_0235de()) != 0)) {
        d = 0;
    } else {
        c = S16(GetCellsInFront(2));
        if (S16(IsFloor(c)) == 0 || (c == 4 && S16(seg4_0235de()) != 0)) d = 0x33;
    }
    if (d != -1) {
        if (R16(CH_JSEQ) == 0x5e) d = (int16_t)(d + 0x16);
        else if (R16(CH_FRAME) == 0xcf) d = (int16_t)(d + 0x33);
        int16_t x = (int16_t)(S16(seg4_021562()) + d);
        if (x < 0x54) W16(CH_X, AddCharX((int16_t)(x - 0x54)));
    }
    JumpSeq(0x7f);
    return 0;
}

/* 0002cdc0: strike with the sword */
uint32_t DoStrike(void) {
    ENTRY_LINK(FN_DoStrike, 4, 8);               /* link -4; movem D3/D4 */
    int16_t t = R16(CH_TYPE);
    if (t == 7 || t == 8) return 0;
    int16_t f = R16(CH_FRAME);
    int16_t seq;
    if (f == 0x9d || f == 0x9e || f == 0xaa || f == 0xab || f == 0xa5) {
        if (t == 0 || t == 1) seq = (R16(OPP_TYPE) == 0xb) ? 0x60 : 0x4b;
        else if (t == 10) seq = 0x6d;
        else seq = 0x3a;
    } else {
        f = R16(CH_FRAME);
        seq = (f == 0x96 || f == 0xa1) ? 0x42 : -1;
    }
    if (seq == -1) return 0;
    JumpSeq(seq);
    W16(gInBtn, 2);
    seg4_02190a();
    return 0;
}

/* 0002ce6a: block (up pressed while en garde) */
uint32_t FUN_0002ce6a(void) {
    ENTRY_NOLINK(FN_FUN_0002ce6a, 12);           /* movem D3-D5 */
    int16_t t = R16(CH_TYPE);
    if (t == 7 || t == 8) return 0;
    int16_t anim = 0;                            /* D5 */
    int16_t seq;                                 /* D4 */
    int16_t f = R16(CH_FRAME);
    if (f == 0x9e || f == 0xaa || f == 0xab || f == 0xa8 || f == 0xa5) {
        if (t == 0 || t == 1) {
            seq = 0x3e;
            if (R16(OPP_FRAME) == 0xa8) {
                seq = -1;
            } else {
                int16_t of = R16(OPP_FRAME);
                if (of != 0x97 && of != 0x98 && of != 0xa2 && of == 0x99) anim = 1;
            }
        } else {
            seq = 0xf6;
            if (S16(seg4_0224be()) >= 0x4a) {
                DoRetreat();
                seq = -1;
            } else if (R16(OPP_FRAME) != 0x98) {
                seq = -1;
            }
        }
    } else {
        seq = (R16(CH_FRAME) == 0xa7) ? 0x3d : -1;
    }
    if (seq == -1) return 0;
    W16(gInUp, 1);
    JumpSeq(seq);
    if (anim != 0) AnimChar();
    return 0;
}

/* 0002cf30: the character is hit by a falling object (crushed or knocked down) */
uint32_t CrushChar(void) {
    ENTRY_LINK(FN_CrushChar, 4, 8);              /* link -4; movem D3/D4 */
    int16_t f0 = R16(CH_FRAME);                  /* D4 */
    if (R16(CH_FRAME) >= 5 && R16(CH_FRAME) < 0xf) return 0;
    if (R16(CH_FRAME) == 0x6d) return 0;
    int16_t act = R16(CH_ACTION);
    if (!(act < 2 || act == 7)) return 0;
    if (R16(CH_TYPE) == 1) return 0;
    FUN_00020cca();
    if (S16(DecLifePts(R16(CH_TYPE) == 0 ? 1 : 100)) != 0) {
        int16_t t = R16(CH_TYPE);
        if (t == 0xb) {
            JumpSeq(0xa9);
        } else if ((t = R16(CH_TYPE)) == 7 || t == 8) {
            JumpSeq(0x92);
        } else if (R16(CH_TYPE) == 4) {
            seg4_01f1de();
        } else {
            JumpSeq(0x16);
            SetKidDeathMethod(3);
        }
        return 0;
    }
    if (f0 == 0x6d) return 0;
    if (S16(IsSpace(S16(FUN_00021518()))) != 0) W16(CH_X, AddCharX(-6));
    W16(CH_F34, 8);
    AddSound(0x12c);
    JumpSeq(0x34);
    return 0;
}

/* 0002d03a: a falling object hits the opponent described by info record p (index idx) */
uint32_t CrushOppInfo(uint32_t p, int16_t idx) {
    ENTRY_LINK(FN_CrushOppInfo, 0, 4);           /* link 0; move.l A2 */
    if (OPP_KIND() != 2) W16(p + 0x12, 0);
    if (OPP_KIND() == 5) {
        if (R16(p + 0x18) == 8) {
            W16(p + 8, 0xa9);
            W16(p + 0xa, 0);
        } else {
            RemoveOppFromRoom(idx, R16(p + 0xe));
            if (R16(gOppIdx) > R16(CH_ID)) W16(gOppIdx, R16(gOppIdx) - 1);
            else if (R16(gOppIdx) == R16(CH_ID)) W16(gOppIdx, GetOppIdxClosestToKid(R16(CH_FACE)));
        }
    } else if (OPP_KIND() == 2) {
        if (R16(p + 0x1c) == 1) W16(p + 0x1c, 0);
    } else {
        W16(p + 8, 0x16);
        W16(p + 0xa, 0);
    }
    return 0;
}

/* 0002d108: killed outright */
uint32_t FUN_0002d108(void) {
    ENTRY_NOLINK(FN_FUN_0002d108, 0);
    JumpSeq(0x76);
    DecLifePts(100);
    return 0;
}

/* 0002d11e: empty */
uint32_t seg6_02d11e(void) {
    return 0;
}

/* 0002d120: set the opponent's life points from its info record p */
uint32_t SetOppLifePts(uint32_t p) {
    ENTRY_LINK(FN_SetOppLifePts, 0, 4);          /* link 0; move.l D3 */
    SetOppMaxLifePts(p);
    int16_t t = R16(CH_TYPE);
    if ((t == 4 && R16(CH_JSEQ) == 0x77) || (t == 10 && R16(CH_JSEQ) == 0x69)) {
        W16(CH_LIFE, 0);
        W16(CH_LIFEDELTA, 0);
    } else {
        int16_t m = R16(CH_MAXLIFE);
        W16(CH_LIFE, m);
        W16(CH_LIFEDELTA, m);
    }
    return RU16(CH_LIFEDELTA);
}

/* 0002d184: the opponent's maximum life points (info +0x1a, default 3) */
uint32_t SetOppMaxLifePts(uint32_t p) {
    if (R16(p + 0x1a) == 0) {
        W16(CH_MAXLIFE, 3);
        return 3;
    }
    W16(CH_MAXLIFE, R16(p + 0x1a));
    return RU16(p + 0x1a);
}

/* 0002d1be: load the characters of the current room from their info records into the
 * room's character table (gOppTab), starting their sequences */
uint32_t FUN_0002d1be(void) {
    ENTRY_NOLINK(FN_FUN_0002d1be, 20);           /* movem D3-D6/A3 */
    CheckSideRoomsForVisibleOpps();
    uint16_t room = RU16(gCurRoom);
    int16_t n = (room != 0) ? R16(R32(gGameState) + (uint32_t)room * 0xc0 + 0x20e6) : 0;   /* D6 */
    int16_t i = 0;                               /* D5 */
    if (n > 0) {
        do {
            maddr info = GetOppInfoIdx(R16(gCurRoom), i);   /* A3 */
            int16_t kind = OPP_KIND();
            if (kind != 5 && kind != 6) {
                int16_t k = R16(info + 0x18);
                if (k != 1 && k != 9 && k != 10 && k != 3) W16(info + 0x18, kind);
            }
            W16(CH_TYPE, R16(GH(0x102d80) + (int32_t)R16(info + 0x18) * 2));
            CheckLoadedOppSet(R16(info + 0x18));
            W16(CH_ID, i);
            W16(CH_ROOM, R16(gCurRoom));
            W16(CH_ROW, (int16_t)(R16(info) / 10));
            if (R16(info) < 0) W16(CH_ROW, R16(CH_ROW) - 1);
            if (R16(CH_TYPE) == 2 && R16(info + 8) == 100) {
                W16(CH_Y, R16(info + 0x1c));
            } else if (R16(CH_TYPE) == 6 && R16(info + 0x22) != 0) {
                W16(CH_Y, R16(info + 0x22));
                if (R16(info + 8) == 0xc) {
                    W16(CH_XVEL, 3);
                    W16(CH_YVEL, 0x21);
                }
            } else {
                FUN_00020cca();
            }
            W16(CH_X, R16(info + 2));
            W16(CH_COL, GetCharCol(R16(info + 2)));
            W16(CH_FACE, R16(info + 4));
            W16(CH_F38, R16(info + 0x14));
            W16(CH_OPPIDX, R16(info + 0x16));
            W16(CH_F32, 0);
            W16(CH_F34, 0);
            W16(CH_F3C, 0);
            SetCharacterBank(R16(info + 0xc));
            if (R16(info + 0xa) == 0) {
                int16_t t = R16(CH_TYPE);
                if (t == 4) {
                    info = SkelAddOpponent(info);
                } else if ((t = R16(CH_TYPE)) == 7 || t == 8 || t == 0xb) {
                    info = HeadAddOpponent(info);
                } else if (R16(CH_TYPE) == 10) {
                    if (S16(IsFloor(S16(FUN_00021076()))) != 0) {
                        W16(info + 8, 0x69);
                        W16(CH_SWORD, 0);
                    } else {
                        W16(info + 8, 0x6a);
                        W16(CH_SWORD, 1);
                        W16(CH_F32, 3);
                        W16(CH_FACE, R16(CH_X) < R16(KID_X) ? 0 : -1);
                    }
                } else if (R16(CH_TYPE) == 6) {
                    if (R16(info + 8) == 0xc5) AddSound(0x105);
                    else W16(info + 8, 2);
                } else {
                    if (R16(info + 8) != 0x16) W16(info + 8, 0x4d);
                    W16(CH_SWORD, 0);
                }
                JumpSeq(R16(info + 8));
                SetOppLifePts(info);
            } else {
                W16(CH_FLAG18, 1);
                W16(CH_SEQ, R16(info + 8));
                W16(CH_SEQPOS, R16(info + 0xa));
                W16(CH_JSEQ, R16(info + 8));
                W16(CH_SWORD, R16(info + 0x10));
                int16_t life = R16(info + 0x12);
                W16(CH_LIFEDELTA, life);
                W16(CH_LIFE, life);
                if (R16(CH_TYPE) != 0) SetOppMaxLifePts(info);
            }
            AnimChar();
            if (S16(IsDeadPos(R16(CH_FRAME))) != 0 && R16(CH_TYPE) != 0) {
                if (R16(CH_TYPE) == 2) FUN_0001db44();
                W16(CH_ALIVE, 7);
                W16(CH_FLAG18, 0);
                W16(CH_LIFE, 0);
                W16(CH_LIFEDELTA, -R16(CH_MAXLIFE));
                int16_t t = R16(CH_TYPE);
                if (t == 4 || t == 10) SetSkelTimer(R16(CH_ID), 0);
            } else {
                W16(CH_ALIVE, -1);
                W16(gLanded103062, 0);
                W16(GH(0x104962), 0);
                W16(GH(0x104966), 0);
                if (R16(CH_TYPE) == 0xb && R16(CH_FRAME) == 0x108) W16(CH_F34, 6);
            }
            if (R16(CH_TYPE) == 6 && R16(CH_JSEQ) == 0xc) {
                W16(CH_ACTION, 4);
            } else {
                seg4_020fb2();
                int16_t t = R16(CH_TYPE);
                W16(CH_ACTION, (t == 7 || t == 8) ? 0 : 1);
            }
            seg4_022516();
            i = (int16_t)(i + 1);
        } while (i < n);
    }
    if (n != 0) return 0;
    int16_t r;
    if (LEVEL_NUM() == 0xc && ((r = R16(gCurRoom)) == 0xb || r == 0x13) && R16(GH(0x103924)) != 1) {
        LoadOppSet(1);
        return 0;
    }
    if (R16(R32(gGameState) + (uint32_t)RU16(gCurRoom) * 0x44 + 0x3986) != 0) {
        int16_t kind = OPP_KIND();
        if (R16(GH(0x103924)) != kind && kind != 5 && kind != 6) LoadOppSet(OPP_KIND());
    }
    return 0;
}
