/* CODE 4 "Characters 2", first half (Ghidra 0x1dad4-0x20e4e): opponent fight AI
 * (en garde, strikes, blocks, run-jumps, tumbling), skeleton/jinnee/shadow special
 * opponents, room-cell lookup (ReadCell/Handler), and the sequence interpreter
 * (AnimChar) that drives every character's animation.
 * Ported from the Ghidra decompilation/disassembly of the original 68k code; addresses
 * in comments are Ghidra addresses (see docs/PORTING.md). */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* ---- Character records (0x3e bytes each, same layout everywhere) ----
 *   +00 id (index in the opponent table)   +02 face (0 = right, -1 = left)
 *   +04 x  +06 y  +08 type (0 kid, 1 shadow, 4 skeleton, 10 ..., 12 jinnee)
 *   +0a frame  +0c column  +0e row  +10 action  +12/+14 x/y momentum
 *   +16 room  +18/+1a/+1c flags  +1e life  +20 max life  +24 sequence position
 *   +26 sequence number  +28 last jump-sequence  +32 fight state  +34  +36 blink/palette
 *   +38 sword  +3c flame counter */
#define REC_SIZE    0x3e
#define gChar       GH(0x102faa)   /* the character being processed */
#define gOpp        GH(0x102f6c)   /* its opponent */
#define gKid        GH(0x102f2e)   /* saved kid record */
#define gOppTab     GH(0x102df8)   /* table of the room's characters, REC_SIZE each */
#define gGameState  GH(0x102fec)   /* long: pointer to the game state block */
#define gCurRoom    GH(0x10303e)   /* word: current room */

#define F_ID      0x00
#define F_FACE    0x02
#define F_X       0x04
#define F_Y       0x06
#define F_TYPE    0x08
#define F_FRAME   0x0a
#define F_COL     0x0c
#define F_ROW     0x0e
#define F_ACTION  0x10
#define F_XMOM    0x12
#define F_YMOM    0x14
#define F_ROOM    0x16
#define F_LIFE    0x1e
#define F_MAXLIFE 0x20
#define F_SEQPOS  0x24
#define F_SEQ     0x26
#define F_JSEQ    0x28
#define F_FIGHT   0x32
#define F_SWORD   0x38

#define CH(o)       R16(gChar + (o))
#define SET_CH(o,v) W16(gChar + (o), (v))
#define OP(o)       R16(gOpp + (o))
#define KD(o)       R16(gKid + (o))
#define SET_KD(o,v) W16(gKid + (o), (v))
#define GS()        R32(gGameState)
#define GS16(o)     R16(GS() + (o))

/* Cell lookup state (ReadCell/Handler) */
#define gCellIdx    GH(0x104b54)   /* word: cell index in the room */
#define gCellRoom   GH(0x104b56)   /* word */
#define gCellCol    GH(0x104b58)   /* word */
#define gCellRow    GH(0x104b5a)   /* word */
#define gCellId     GH(0x104b50)   /* word: tile id of the last cell read */
#define gCellSpec   GH(0x104b52)   /* word: tile modifier of the last cell read */

/* Opponent AI "key presses" */
#define gAutoBtn1   GH(0x104b68)
#define gAutoBtn2   GH(0x104b6a)
#define gAutoBtn3   GH(0x104b6c)
#define gAutoBtn1b  GH(0x104b78)
#define gAutoBtn1c  GH(0x104b7a)
#define gAutoBtn2b  GH(0x104b7c)
#define gAutoBtn2c  GH(0x104b7e)
#define gAutoBtn3b  GH(0x104b80)

#define gDirTab     GH(0x103468)   /* word[]: indexed by face*2 (face 0/-1) */
#define gDirTab2    GH(0x10346c)

/* word: number of characters in room r */
static maddr room_count_addr(uint16_t r) { return GS() + (uint32_t)r * 0xc0 + 0x20e6; }
static int16_t room_count(uint16_t r) { return r ? R16(room_count_addr(r)) : 0; }
/* the per-room character info record (0x26 bytes) of character `id` */
static maddr opp_info(int16_t id, uint16_t room) {
    uint32_t a1 = (uint32_t)((int32_t)id * 0x26);
    return a1 + (GS() + 0x21a6 + (uint32_t)room * 0xc0 - 0xbe);
}
/* fields of entry i of the character table (word index arithmetic like the original) */
static maddr tab_field(int16_t i, int off) { return GH(0x102df8 + off) + (int16_t)(i * REC_SIZE); }

static void save_rec(uint8_t *buf, maddr src) { for (int i = 0; i < REC_SIZE; i++) buf[i] = R8(src + i); }
static void load_rec(maddr dst, const uint8_t *buf) { for (int i = 0; i < REC_SIZE; i++) W8(dst + i, buf[i]); }

/* Kid frame in a fighting stance */
static int fight_frame(uint16_t f) {
    return (f >= 1 && f <= 0xe) || (f >= 0x31 && f <= 0x38) || (f >= 0x22 && f <= 0x2c);
}
/* level 5 rooms 10/7/12 (fighting next to the rooftop edge) */
static int roof_fight_room(void) {
    if (CH(F_ROW) == 0) return 0;
    if (GS16(0x218a) != 5) return 0;
    int16_t r = CH(F_ROOM);
    return r == 10 || r == 7 || r == 0xc;
}

/* 0001dad4 */
uint32_t FUN_0001dad4(void) {
    return 0;
}

/* 0001dad6: release button 1 */
uint32_t FUN_0001dad6(void) {
    W16(gAutoBtn1, -1);
    W16(gAutoBtn1b, -1);
    return 0xffffffffu;
}

/* 0001dae2: press button 1 */
uint32_t FUN_0001dae2(void) {
    W16(gAutoBtn1c, -1);
    W16(gAutoBtn1, 1);
    return 1;
}

/* 0001daf0: release button 2 */
uint32_t FUN_0001daf0(void) {
    W16(gAutoBtn2, -1);
    W16(gAutoBtn2b, -1);
    return 0xffffffffu;
}

/* 0001dafc: press button 2 */
uint32_t FUN_0001dafc(void) {
    W16(gAutoBtn2c, -1);
    W16(gAutoBtn2, 1);
    return 1;
}

/* 0001db0a */
uint32_t FUN_0001db0a(void) {
    W16(gAutoBtn2c, -1);
    return FUN_0001dae2();
}

/* 0001db16 */
uint32_t FUN_0001db16(void) {
    W16(gAutoBtn2c, -1);
    return FUN_0001dad6();
}

/* 0001db22 */
uint32_t AutoPress(int16_t v) {
    W16(gAutoBtn3, v);
    W16(gAutoBtn3b, v);
    return (uint16_t)v;
}

/* 0001db44: dead opponent: play its death sequence once */
uint32_t FUN_0001db44(void) {
    if (CH(F_FRAME) == 0xb9) {
        int16_t seq = S16(GetOppDeadSeq(GS16(0x21a4)));
        if (seq != 0) {
            JumpSeq(seq);
            AnimChar();
        }
        if (CH(0x1c) < 0x14) SET_CH(0x1c, CH(0x1c) + 1);
    }
    return 0;
}

/* 0001db7c: random death sequence for the given opponent kind (0 if none) */
uint32_t GetOppDeadSeq(int16_t kind) {
    int16_t res = 0, base = 0;
    uint16_t n = 0;
    if (kind == 7) {
        base = 0xbf; n = 4;
    } else if (kind == 0 && GS16(0x2186) != 6) {
        base = 0xc3; n = 1;
    }
    if (base != 0) {
        int16_t r = (int16_t)((int32_t)CH(F_ID) % (int32_t)((uint32_t)n + 1));
        if (r != 0) res = (int16_t)(base + r - 1);
    }
    return (uint16_t)res;
}

/* 0001dbee */
uint32_t FUN_0001dbee(void) {
    if (CH(F_FIGHT) != 1) {
        if (CH(F_TYPE) == 4) JumpSeq(0x66);
        else JumpSeq(0x54);
        SET_CH(0x1a, 0);
        SET_CH(F_FIGHT, 0);
    }
    return 0;
}

/* 0001dc24: should the opponent tumble (fall from a ledge)? */
uint32_t DoOppTumbleSeq(void) {
    int16_t d4, d3, d5, d6;
    if (GS16(0x2186) == 5) {
        d3 = CH(F_ROOM);
        if (d3 == 0x13 || d3 == 0x10) d4 = 1;
        else if (S16(IsSpace(S16(FUN_00021518()))) == 0) d4 = 0;
        else d4 = CH(F_FACE) == -1;
    } else {
        d4 = 0;
    }
    if (d4 != 0) return (uint16_t)d4;
    if (S16(IsOppGenPtWithTumbleOn(CH(F_ROOM))) == 0) return (uint16_t)d4;
    d3 = S16(GetTotalDeadOpps());
    d5 = S16(Rnd(3));
    d4 = d5 <= d3;
    if (d4 == 0 && d3 != 0) {
        uint8_t save[REC_SIZE];
        save_rec(save, gChar);
        d3 = R16(gCurRoom);
        d5 = room_count((uint16_t)d3);
        d6 = 0;
        if (d5 > 0 && d4 == 0) {
            int16_t sid = (int16_t)(save[0] << 8 | save[1]);
            int16_t scol = (int16_t)(save[F_COL] << 8 | save[F_COL + 1]);
            int16_t srow = (int16_t)(save[F_ROW] << 8 | save[F_ROW + 1]);
            do {
                if (d6 != sid) {
                    LoadShad(d6);
                    if (CH(F_ROW) == srow && CH(F_COL) == scol && S16(IsDeadPos(CH(F_FRAME))) != 0) d4 = 1;
                }
                d6++;
            } while (d6 < d5 && d4 == 0);
        }
        load_rec(gChar, save);
    }
    if (d4 != 0) {
        if (CH(F_FACE) == OP(F_FACE)) {
            d4 = 0;
        } else {
            d3 = S16(FUN_00021076());
            if (d3 == 3 || d3 == 8 || d3 == 4) {
                d4 = 0;
            } else {
                d3 = S16(FUN_00021518());
                if (d3 == 3 || d3 == 8) d4 = 0;
            }
        }
    }
    return (uint16_t)d4;
}

/* 0001ddb0: decide whether to advance (button 1) when nothing blocks the way */
uint32_t FUN_0001ddb0(void) {
    int16_t d3 = OP(F_ACTION);
    if (d3 == 2 || d3 == 6) return 0;
    d3 = 0;
    int16_t d5 = S16(GetCellsInFront(1));
    int16_t d6 = S16(GetCellsInFront(2));
    if (S16(GetBarrCode(d5)) != 0 || S16(GetBarrCode(d6)) != 0) {
        d3 = 1;
    } else if (S16(IsSpace(d5)) == 0 && S16(IsSpace(d6)) == 0) {
        FUN_0001dad6();
    } else {
        int16_t d4 = S16(ReadCell(R16(gCellRoom), R16(gCellCol), (int16_t)(R16(gCellRow) + 1)));
        if ((S16(IsImpassable(d4)) != 0 && CH(F_TYPE) != 10) ||
            S16(GetBarrCode(d4)) != 0 || S16(IsSpace(d4)) != 0 ||
            (int32_t)CH(F_ROW) + 1 != (int32_t)OP(F_ROW)) {
            d3 = 1;
        } else {
            if (R8(GS() + 0x218c) != 0) {
                d4 = S16(ReadCell(R16(gCellRoom), (int16_t)(R16(gCellCol) - 1), (int16_t)(R16(gCellRow) + 1)));
                d3 = d4 == 2;
            }
            if (d3 == 0) FUN_0001dad6();
        }
    }
    if (d3 != 0) FUN_0001dae2();
    return 0;
}

/* 0001dec4: number of live characters in the current room */
uint32_t GetTotalDeadOpps(void) {
    int16_t n = 0;
    int16_t cnt = room_count(RU16(gCurRoom));
    if (cnt != 0 && cnt > 0) {
        maddr p = GH(0x102e14);
        int16_t i = 0;
        do {
            if (R16(p) >= 0) n++;
            i++;
            p += REC_SIZE;
        } while (i < cnt);
    }
    return (uint16_t)n;
}

/* 0001df36: opponent within striking range */
uint32_t InRange(int16_t dist) {
    if (OP(0x1a) == 1 && CH(F_FACE) != OP(F_FACE)) {
        GenFight(dist);
        return 0;
    }
    if (R16(GH(0x104966)) != 0) return 0;
    FRAME(f, 4);
    GetCharStrikeRanges(f + 2, f);
    if (dist < R16(f)) {
        AutoPress(-2);
        FRAME_END(f);
        return 0;
    }
    FRAME_END(f);
    if (roof_fight_room() && S16(FUN_00034462()) == 0) return 0;
    FUN_0001dad6();
    return 0;
}

/* 0001dfcc */
uint32_t FUN_0001dfcc(void) {
    if (OP(0x1c) >= 0) return 0;
    int16_t d4 = S16(seg4_0224be());
    if (OP(F_ROW) == CH(F_ROW) && d4 < 0 && d4 >= -0x1b) {
        if (CH(F_FIGHT) >= 2 && CH(F_FRAME) == 0xa6) FUN_0001db16();
        return 0;
    }
    if (R16(GH(0x103062)) != 0 ||
        (CH(F_FIGHT) == 3 && d4 < 0 && CH(F_JSEQ) != 0x4d)) {
        if (d4 < -6) FUN_0001dafc();
        return 0;
    }
    if (d4 >= 0 && d4 <= 0x297 && CH(F_FIGHT) != 0) FUN_0001db16();
    return 0;
}

/* 0001e04c: opponent chasing / turning toward the kid */
uint32_t seg4_01e04c(void) {
    int16_t d3, d4, d5, d6;
    uint16_t fr = RU16(gChar + F_FRAME);
    if (!(((fr & 1) != 0 && fr >= 0xc2 && fr <= 0xc7) || CH(F_FRAME) == 0xd4)) {
        if (CH(F_SWORD) >= 1) {
            d3 = CH(F_FRAME);
            if (d3 == 0xc0 || d3 == 0xc4) DoOppRunJump();
        }
        return 0;
    }
    d4 = S16(GetOppIdxClosestToKid((int16_t)~CH(F_FACE)));
    if (d4 == CH(F_ID)) {
        d5 = 0x7f;
    } else if (d4 == -1) {
        d5 = 0;
    } else {
        int32_t dx = (int32_t)CH(F_X) - (int32_t)R16(tab_field(d4, F_X));
        if (dx < 0) dx = -dx;
        d5 = (int16_t)dx;
    }
    d6 = S16(GetCellsInFront(1));
    {
        int32_t dx = (int32_t)CH(F_X) - (int32_t)KD(F_X);
        if (dx < 0) dx = -dx;
        d4 = (int16_t)dx;
    }
    if (fight_frame(RU16(gKid + F_FRAME))) {
        if (CH(F_FACE) != KD(F_FACE)) d4 = (int16_t)(d4 - 0x33);
        else d4 = (int16_t)(d4 + 0x33);
    } else if (KD(0x1a) != 1) {
        d4 = (int16_t)(d4 + 0x33);
    }
    maddr info = GetOppInfoIdx(CH(F_ROOM), CH(F_ID));
    d3 = KD(F_ROW);
    if ((d3 == CH(F_ROW) || d3 == R16(info + 0x20)) && S16(IsAdvancableId(d6, R16(gCellSpec))) != 0) {
        /* 0001e1aa */
        if (CH(F_ROW) == KD(F_ROW)) {
            int notclose = 0;
            if (CH(F_FACE) == OP(F_FACE) && fight_frame(RU16(gOpp + F_FRAME)) &&
                OP(F_ACTION) != 7 && d4 >= 0x66) notclose = 1;
            if (!notclose && !(d5 > 0x7e && d4 > 0xb2)) {
                SET_CH(F_FIGHT, 2);
                d4 = 0x65;
                goto L270;
            }
        }
        d3 = CH(F_FACE);
        if ((d3 == -1 && CH(F_COL) < KD(F_COL)) || (d3 == 0 && CH(F_COL) > KD(F_COL))) {
            SET_CH(F_FIGHT, 2);
            d4 = 0x65;
            goto L270;
        }
        d3 = 0;
        if (KD(0x1c) >= 0 || KD(0x34) == 0xd) {
            SET_CH(F_FIGHT, d3);
            d4 = 0x65;
            goto L270;
        }
        if (S16(IsExitChasingSkel(CH(F_ROOM))) != 0 && d4 <= 0x7f) {
            d4 = 0x57;
            SET_CH(0x1a, 1);
        } else {
            d4 = -1;
        }
    } else {
        /* 0001e166 */
        d5 = S16(seg4_021562());
        d3 = KD(F_ROW);
        if (d3 == CH(F_ROW) || d3 == R16(info + 0x20)) {
            d3 = CH(F_SWORD);
            if (d3 >= 1 && d5 >= 0x27 && d6 != 4) {
                d4 = -1;
                goto L270;
            }
        }
        SET_CH(F_FIGHT, 0);
        d4 = 0x65;
    }
L270:
    if (d4 != -1) {
        JumpSeq(d4);
        d6 = S16(FUN_00021518());
        if (CH(F_FRAME) == 0xd4 && S16(IsAdvancableId(d6, R16(gCellSpec))) == 0)
            SET_CH(F_X, AddCharX(0xc));
    }
    return 0;
}

/* 0001e2ce */
uint32_t seg4_01e2ce(void) {
    int16_t d3 = S16(seg4_02397e());
    if (d3 < 0x44 && R16(GH(0x104a8a)) != 2)
        SET_CH(F_X, AddCharX((int16_t)(d3 - 0x44)));
    JumpSeq(0x6e);
    return 0;
}

/* 0001e304: opponent run-jumps over a gap when it is short enough */
uint32_t DoOppRunJump(void) {
    int16_t d3, d4, d5, d6, d7;
    d6 = S16(GetCellsInFront(3));
    if (S16(IsSpace(d6)) == 0) return 0;
    d3 = 1;
    d4 = S16(GetCellsInFront(d3));
    d5 = S16(GetCellsInFront(2));
    if (S16(IsSpace(d4)) != 0) {
        d7 = d3;
    } else if (S16(IsSpace(d5)) != 0) {
        d7 = 2; d4 = d5;
    } else {
        d7 = 3; d4 = d6;
    }
    d5 = 0;
    if (S16(IsSpace(d4)) != 0) {
        d6 = d7;
        do {
            d6++;
            d3 = (int16_t)((int32_t)d6 * R16(gDirTab + (int16_t)(CH(F_FACE) * 2)));
            d3 = (int16_t)(d3 + CH(F_COL));
            d4 = S16(ReadCell(CH(F_ROOM), d3, CH(F_ROW)));
        } while (S16(IsSpace(d4)) != 0 && (uint16_t)(d7 + 4) >= (uint16_t)d6);
        d5 = (int16_t)(d6 - d7);
    }
    if (d5 < 4 || (d5 == 4 && S16(Rnd((int16_t)(CH(F_SWORD) - 1))) == 0)) {
        d3 = 1;
    } else if (CH(F_ROOM) == 0xd && GS16(0x218a) == 0xd && KD(0x34) != 0xd) {
        d3 = 1;
    } else {
        maddr info = GetOppInfoIdx(CH(F_ROOM), CH(F_ID));
        d3 = !(info == 0 || R16(info + 0x20) == CH(F_ROW));
    }
    if (d3 != 0) d3 = S16(DoRunJump(100));
    else d3 = 0;
    if (d3 == 0) {
        JumpSeq(0x65);
        SET_CH(F_FIGHT, 2);
    }
    return 0;
}

/* 0001e45c: sword-fighting opponent AI */
uint32_t EnGarde(void) {
    int16_t d3, d4, d5, d6;
    d3 = CH(F_FIGHT);
    if (d3 == 1 || d3 == 2) {
        WaitingEngarde();
        return 0;
    }
    d3 = CH(F_FRAME);
    if (d3 == 0xa6) return 0;
    if ((uint16_t)d3 < 0x96) return 0;
    if (CH(F_FIGHT) == 0) {
        if (R16(GH(0x103064)) != 0) { FUN_0001ddb0(); return 0; }
        if (CH(F_TYPE) == 4) return 0;
        FUN_0001db0a();
        return 0;
    }
    uint16_t ofr = RU16(gOpp + F_FRAME);
    d5 = S16(seg4_0224be());
    if (d5 >= 0x27 && ofr >= 0x66 && ofr < 0x76 && OP(F_ACTION) == 5) return 0;
    FRAME(f, 8);
    GetCharStrikeRanges(f + 2, f + 4);
    int16_t rmin = R16(f + 2), rmax = R16(f + 4);
    FRAME_END(f);
    if ((int32_t)rmax + 0x12 > (int32_t)d5) {
        d4 = (roof_fight_room() && S16(seg8_03426e()) != 0) ? 1 : 0;
        if (d5 >= rmin && d4 == 0) {
            InRange(d5);
            return 0;
        }
        if (d4 != 0) { FUN_0001dad6(); return 0; }
        if (CH(F_FRAME) != 0xab) return 0;
        if (CH(F_JSEQ) != 0x3c) {
            d4 = S16(GetCellsInFront(1));
            if (S16(IsAdvancableId(d4, R16(gCellSpec))) != 0) {
                d3 = CH(F_JSEQ);
                if (d3 != 0x39 && d3 != 0x68) { FUN_0001dad6(); return 0; }
            }
            d4 = S16(FUN_00021518());
            if (S16(IsAdvancableId(d4, R16(gCellSpec))) == 0) return 0;
            if (roof_fight_room() && S16(FUN_00034462()) == 0) return 0;
            FUN_0001dae2();
            return 0;
        }
        d6 = 0;
        d4 = S16(FUN_00021518());
        if (S16(IsAdvancableId(d4, R16(gCellSpec))) != 0 && d5 > 0) {
            d3 = CH(F_JSEQ);
            if (d3 != 0x56 && d3 != 0x6c) {
                d3 = S16(seg4_021562());
                if (d3 <= 0x19) {
                    d6 = 1;
                } else if ((int32_t)rmin - (int32_t)d5 <= 0x11) {
                    d6 = 1;
                } else {
                    d3 = (int16_t)(R16(gDirTab2 + (int16_t)(CH(F_FACE) * 2)) << 1);
                    d3 = (int16_t)(d3 + CH(F_COL));
                    d4 = S16(ReadCell(CH(F_ROOM), d3, CH(F_ROW)));
                    if (S16(IsAdvancableId(d4, R16(gCellSpec))) != 0) d6 = 1;
                }
            }
        }
        if (d6 != 0) { FUN_0001dae2(); return 0; }
        d4 = S16(GetCellsInFront(1));
        if (S16(IsAdvancableId(d4, R16(gCellSpec))) == 0) return 0;
        if (roof_fight_room() && S16(FUN_00034462()) == 0) return 0;
        FUN_0001dad6();
        return 0;
    }
    if (R16(GH(0x104966)) != 0) return 0;
    if (CH(F_FACE) == OP(F_FACE)) { FUN_0001ea88(); return 0; }
    if (ofr >= 7 && ofr < 0xf) {
        if (d5 < 0x96) AutoPress(-2);
        return 0;
    }
    if (ofr >= 0x22 && ofr < 0x2c) {
        if (d5 < 0xba) AutoPress(-2);
        return 0;
    }
    FUN_0001ea88();
    return 0;
}

/* 0001e74e: opponent waiting en garde for the kid to come closer */
uint32_t WaitingEngarde(void) {
    int16_t d3, d4, d5, d6, d7;
    d5 = S16(GetOppIdxClosestToKid((int16_t)~CH(F_FACE)));
    if (d5 == -1) d5 = S16(GetOppIdxClosestToKid(1));
    if (d5 == -1) return 0;
    if (CH(F_ID) != d5) {
        int16_t clear = 0;
        d3 = S16(GetCellsInFront(1));
        if (S16(IsAdvancableId(d3, R16(gCellSpec))) != 0) {
            d3 = S16(GetCellsInFront(2));
            if (S16(IsAdvancableId(d3, R16(gCellSpec))) != 0) {
                d3 = S16(GetCellsInFront(3));
                if (S16(IsAdvancableId(d3, R16(gCellSpec))) != 0) clear = 1;
            }
        }
        int32_t dx = (int32_t)CH(F_X) - (int32_t)R16(tab_field(d5, F_X));
        if (dx < 0) dx = -dx;
        d6 = (int16_t)dx;
        d7 = S16(FUN_0001e9f8());
        int near = 0;
        if ((int32_t)d7 + 0x18 < (int32_t)d6) near = 1;
        else if (R16(tab_field(d5, F_FACE)) == CH(F_FACE) && R16(tab_field(d5, F_JSEQ)) == 0x54) near = 1;
        if (near) {
            /* 0001e854 */
            if (clear == 0) return 0;
            if (CH(F_JSEQ) == 0x54) return 0;
            if (!(R16(tab_field(d5, F_JSEQ)) == 0x54 && R16(tab_field(d5, F_FACE)) == CH(F_FACE))) {
                if (CH(F_FACE) != OP(F_FACE) || !fight_frame(RU16(gOpp + F_FRAME)) || OP(F_ACTION) == 7) {
                    FUN_0001dad6();
                    return 0;
                }
            }
            if (CH(F_SWORD) == 0) { FUN_0001dad6(); return 0; }
            FUN_0001dbee();
            return 0;
        }
        /* 0001e8d2 */
        if (clear != 0 && CH(F_FACE) == OP(F_FACE) && fight_frame(RU16(gOpp + F_FRAME)) &&
            OP(F_ACTION) != 7 && CH(F_SWORD) != 0) {
            d4 = CH(F_X);
            d3 = OP(F_X);
            if ((d4 < d3 && OP(F_FACE) == 0) || (d4 > d3 && OP(F_FACE) == -1)) {
                FUN_0001dbee();
                return 0;
            }
        }
        if (d6 >= d7) return 0;
        d3 = S16(FUN_00021518());
        if (S16(IsAdvancableId(d3, R16(gCellSpec))) != 0) FUN_0001dae2();
        return 0;
    }
    /* 0001e964 */
    {
        int32_t dx = (int32_t)KD(F_X) - (int32_t)CH(F_X);
        if (dx < 0) dx = -dx;
        d6 = (int16_t)dx;
    }
    if (d6 < 0x33) d3 = S16(GetCellsInFront(1));
    else d3 = S16(GetCellsInFront(2));
    if (S16(IsImpassable(d3)) == 0 && S16(IsSpace(d3)) == 0) {
        if (d3 != 4) return 0;
        if ((uint16_t)(RU16(gCellSpec) & 0xff) >= 0x68) return 0;
    }
    d3 = S16(FUN_00021518());
    if (S16(IsAdvancableId(d3, R16(gCellSpec))) != 0) FUN_0001dae2();
    return 0;
}

/* 0001e9f8: engage distance, larger when other characters stand close by */
uint32_t FUN_0001e9f8(void) {
    int16_t d6 = room_count(RU16(gCurRoom));
    int16_t d5 = (int16_t)(CH(F_ID) + 1);
    maddr a2 = gOppTab + (uint32_t)((int32_t)d5 * REC_SIZE);
    int16_t d4 = 0x66;
    if (d5 < d6) {
        maddr a3 = a2 + 4;
        do {
            int32_t dx = (int32_t)CH(F_X) - (int32_t)R16(a3);
            if (dx < 0) dx = -dx;
            if (dx < 0x24) d4 = (int16_t)(d4 + 0x24);
            d5++;
            a3 += REC_SIZE;
        } while (d5 < d6);
    }
    return (uint16_t)d4;
}

/* 0001ea88 */
uint32_t FUN_0001ea88(void) {
    int16_t d3 = S16(GetCellsInFront(1));
    int16_t d4 = S16(GetCellsInFront(2));
    if (S16(IsSpace(d3)) != 0 && S16(IsSpace(d4)) != 0) {
        if (roof_fight_room() && S16(seg8_03426e()) != 0) return 0;
        FUN_0001dae2();
        return 0;
    }
    if (CH(F_FACE) == OP(F_FACE) && fight_frame(RU16(gOpp + F_FRAME)) && OP(F_ACTION) != 7 &&
        CH(F_SWORD) != 0 && !roof_fight_room()) {
        FUN_0001dbee();
        return 0;
    }
    FUN_0001ebea();
    return 0;
}

/* 0001eb6e */
uint32_t GenFight(int16_t dist) {
    FRAME(f, 4);
    GetCharStrikeRanges(f + 2, f);
    int16_t rmin = R16(f + 2), rmax = R16(f);
    FRAME_END(f);
    if (dist < rmin || dist >= rmax || (roof_fight_room() && S16(seg8_03426e()) != 0)) {
        FUN_0001ebea();
        return 0;
    }
    FUN_0001ecd4();
    if (R16(GH(0x104966)) == 0) FUN_0001ed96();
    return 0;
}

/* 0001ebea: strike at random, with a probability that depends on the opponent's skill */
uint32_t FUN_0001ebea(void) {
    if (roof_fight_room() && S16(seg8_03426e()) != 0) {
        FUN_0001dad6();
        return 0;
    }
    int16_t room = CH(F_ROOM);
    int16_t skill = R16(opp_info(CH(F_ID), (uint16_t)room) + 6);
    if (skill != 0 && R16(GH(0x104964)) != 0) return 0;
    if (CH(F_ROW) != 0 && GS16(0x218a) == 5 && (room == 10 || room == 7 || room == 0xc)) {
        if (S16(FUN_00034462()) == 0) return 0;
    }
    uint16_t r = (uint16_t)Rnd(0xff);
    if (r < RU16(GH(0x1049c8) + (int16_t)((uint16_t)skill * 2))) FUN_0001dad6();
    return 0;
}

/* 0001ecd4: block the kid's strike at random */
uint32_t FUN_0001ecd4(void) {
    if (CH(F_TYPE) == 10) return 0;
    int16_t d3 = OP(F_FRAME);
    if (d3 != 0x98 && d3 != 0x99 && d3 != 0xa2) return 0;
    d3 = R16(opp_info(CH(F_ID), RU16(gChar + F_ROOM)) + 6);
    if (R16(GH(0x104962)) != 0) {
        uint16_t r = (uint16_t)Rnd(0xff);
        if (r < RU16(GH(0x1049b0) + (int16_t)((uint16_t)d3 * 2))) FUN_0001daf0();
    } else {
        uint16_t r = (uint16_t)Rnd(0xff);
        if (r < RU16(GH(0x104998) + (int16_t)((uint16_t)d3 * 2))) FUN_0001daf0();
    }
    return 0;
}

/* 0001ed96: step back at random */
uint32_t FUN_0001ed96(void) {
    int16_t d3 = OP(F_FRAME);
    if (d3 == 0xa9 || d3 == 0x97) return 0;
    int16_t d4 = R16(opp_info(CH(F_ID), RU16(gChar + F_ROOM)) + 6);
    d3 = CH(F_TYPE);
    if (d3 != 7 && d3 != 8) {
        d3 = CH(F_FRAME);
        if (d3 == 0xa1 || d3 == 0x96) {
            uint16_t r = (uint16_t)Rnd(0xff);
            if (r < RU16(GH(0x104980) + (int16_t)((uint16_t)d4 * 2))) AutoPress(-2);
            return 0;
        }
    }
    uint16_t r = (uint16_t)Rnd(0xff);
    if (r < RU16(GH(0x104968) + (int16_t)((uint16_t)d4 * 2))) AutoPress(-2);
    return 0;
}

/* 0001ee74: is character `id` close enough (same row, clear floor in between) to
 * the cell (col,row)? The original walks its own `col` argument toward the character,
 * so the final value is written back into the argument slot by the glue. */
static uint32_t OpponentClose_(int16_t id, int16_t *pcol, int16_t row, int16_t range) {
    int16_t d3, d6 = 0, d7, step;
    maddr info = GetOppInfoIdx(R16(gCurRoom), id);
    if (info == 0) return 0;
    if (CH(0x1c) < 0) return 0;
    if (CH(0x34) == 5) return 0;
    if (CH(F_ROW) != row) return 0;
    if (KD(0x08) != 0) return 0;
    if (R16(info + 0x1e) != 0 && S16(IsExitChasingSkel(CH(F_ROOM))) == 0) return 0;
    d3 = CH(F_COL);
    if (d3 > *pcol) { d7 = (int16_t)(d3 + 1); step = 1; }
    else { d7 = (int16_t)(CH(F_COL) - 1); step = -1; }
    int32_t c = *pcol, r = range, col = CH(F_COL);
    if (c - r > col) return 0;
    if (c + r < col) return 0;
    do {
        d3 = S16(ReadCell(CH(F_ROOM), *pcol, row));
        if (S16(IsFloor(d3)) == 0) d6 = 0;
        else if (S16(IsImpassable(d3)) != 0 && CH(F_TYPE) != 10) d6 = 0;
        else if (d3 == 4 && S16(seg4_0235de()) != 0) d6 = 0;
        else d6 = 1;
        *pcol = (int16_t)(*pcol + step);
    } while (d6 != 0 && *pcol != d7);
    return (uint16_t)d6;
}
uint32_t OpponentClose(int16_t id, int16_t col, int16_t row, int16_t range) {
    return OpponentClose_(id, &col, row, range);
}
void glue_OpponentClose(void) {
    maddr sp = emu_sp();
    int16_t col = R16(sp + 2);
    uint32_t r = OpponentClose_(R16(sp), &col, R16(sp + 4), R16(sp + 6));
    W16(sp + 2, col);
    emu_set_d0(r);
}

/* 0001ef96: skeletons/statues in the room come alive when the kid gets close */
uint32_t FUN_0001ef96(void) {
    int16_t d3, d4 = (int16_t)cpu_get(R_D4), d5, d6, d7;  /* d4 may be used unset (caller's D4) */
    if (KD(0x1c) >= 0) return 0;
    d3 = KD(0x10);
    if (d3 == 4 || d3 == 3 || d3 == 5) return 0;
    d6 = room_count(RU16(gCurRoom));
    if (d6 == 0) return 0;
    if (S16(IsExitChasingSkel(KD(F_ROOM))) != 0) d7 = 8;
    else d7 = GS16(0x21a4) == 2 ? 4 : 5;
    d5 = 0;
    if (d6 <= 0) return 0;
    do {
        LoadShad(d5);
        if (S16(OpponentClose(d5, KD(F_COL), KD(F_ROW), d7)) != 0) {
            maddr info = GetOppInfoIdx(CH(F_ROOM), CH(F_ID));
            if (GS16(0x21a4) == 2) {
                if (R16(info + 0x1c) != -1) {
                    d3 = R16(info + 0x1e);
                    if (S16(SkelDoneShaking(CH(F_ID))) != 0) {
                        JumpSeq(0x58);
                        d4 = 1;
                        SET_CH(F_TYPE, 4);
                    } else {
                        if (d3 == 0 && CH(F_JSEQ) != 0x70) JumpSeq(0x70);
                        d4 = 0;
                    }
                }
            } else {
                JumpSeq(0x6a);
                d4 = 1;
                SET_CH(F_TYPE, 10);
            }
            AnimChar();
            if (d4 != 0) {
                SetOppLifePts(info);
                SET_CH(0x1c, -1);
                SET_CH(0x1a, 1);
                SET_CH(F_FIGHT, 3);
                if (CH(F_TYPE) == 10) SET_CH(F_FACE, CH(F_X) < KD(F_X) ? 0 : -1);
                W16(GH(0x103062), 0);
                W16(GH(0x104966), 0);
                seg4_020fb2();
                if (R16(GH(0x102f68)) == -1) W16(GH(0x102f68), CH(F_ID));
            }
            seg4_022516();
        }
        d5++;
    } while (d5 < d6);
    return 0;
}

/* 0001f12c: per-frame update of a skeleton */
uint32_t seg4_01f12c(void) {
    int16_t d4;
    if (CH(0x1c) >= 0) {
        maddr info = GetOppInfoIdx(CH(F_ROOM), CH(F_ID));
        if (info != 0 && R16(info + 0x1e) > 0) W16(info + 0x1e, R16(info + 0x1e) - 1);
        return 0;
    }
    d4 = 1;
    if (roof_fight_room()) {
        d4 = S16(seg8_0342fc());
    } else if (KD(0x1c) >= 5 && CH(F_TYPE) == 4 && CH(F_JSEQ) != 0x78) {
        seg4_01f1de();
        d4 = 0;
    }
    if (d4 == 0) return 0;
    if (CH(F_JSEQ) == 0x58 && RU16(gChar + F_FRAME) >= 0xb3) {
        if (CH(F_FRAME) == 0xb6) FUN_0001f66c();
        return 0;
    }
    if (d4 != 0) seg6_02f5a0();
    return 0;
}

/* 0001f1de: skeleton collapses into a heap of bones */
uint32_t seg4_01f1de(void) {
    int16_t d3, d4;
    JumpSeq(0x78);
    AddSound(0x3d);
    SET_CH(0x18, 1);
    SET_CH(0x1a, 0);
    if (CH(0x1c) < 0) SET_CH(0x1c, 0);
    if (CH(F_FACE) == 0) {
        d3 = S16(FUN_00021518());
        if (S16(IsSpace(d3)) != 0) {
            d4 = (int16_t)(0x33 - S16(seg4_021562()));
            if (d4 <= 0x36) SET_CH(F_X, AddCharX((int16_t)(0x36 - d4)));
        }
    }
    seg4_022516();
    DrawOppMeter(CH(F_ID), 0);
    SetSkelTimer(CH(F_ID), 0x78);
    SetSkelShakes(CH(F_ID));
    LoadShad(CH(F_ID));
    return 0;
}

/* 0001f27a: level 4 rooms 0x16-0x1c (except 0x17): the skeleton chase near the exit */
uint32_t IsExitChasingSkel(int16_t room) {
    int16_t r = 0;
    if (GS16(0x218a) == 4) {
        uint16_t d3 = (uint16_t)room;
        if (d3 >= 0x16 && d3 <= 0x1c && d3 != 0x17) r = 1;
    }
    return (uint16_t)r;
}

/* 0001f2ca */
uint32_t SetSkelShakes(int16_t id) {
    maddr info = GetOppInfoIdx(R16(gCurRoom), id);
    if (info != 0) {
        if (R16(info + 6) != 0) W16(info + 0x22, (R16(info + 6) + 1) & 1);
        else W16(info + 0x22, 3);
    }
    return 0;
}

/* 0001f31a */
uint32_t SetSkelTimer(int16_t id, int16_t t) {
    if (GS16(0x21a4) == 2 || R16(GH(0x102e00)) == 10) {
        maddr info = GetOppInfoIdx(R16(gCurRoom), id);
        if (info != 0) W16(info + 0x1e, t);
    }
    return 0;
}

/* 0001f368: wake the bone heaps lying in row `row` of `room` */
uint32_t ShakeBones(int16_t room, int16_t row) {
    uint8_t save[REC_SIZE];
    save_rec(save, gChar);
    int16_t d6 = 0, d5 = 0;
    int16_t d4 = room_count((uint16_t)room);
    if (d4 > 0) {
        maddr pRoom = GH(0x102e0e), pX34 = GH(0x102e2c), pFrame = GH(0x102e02), pRow = GH(0x102e06);
        do {
            if (R16(pRow) == row && R16(pFrame) == 0xce && R16(pX34) != 5) {
                maddr info = GetOppInfoIdx(R16(pRoom), d5);
                if (R16(info + 0x1c) == 0) {
                    d6 = 1;
                    LoadShad(d5);
                    JumpSeq(0x70);
                    seg4_022516();
                }
            }
            d5++;
            pRoom += REC_SIZE; pX34 += REC_SIZE; pFrame += REC_SIZE; pRow += REC_SIZE;
        } while (d5 < d4);
    }
    if (d6 != 0) load_rec(gChar, save);
    return 0;
}

/* 0001f45a: put a skeleton into the room */
uint32_t SkelAddOpponent(uint32_t p) {
    int16_t d5, d3;
    if (R16(p + 0x1c) == 1) {
        W16(p + 8, 0x3f);
        SET_CH(0x1a, 1);
        goto done;
    }
    if (CH(F_ID) == 0 && R16(p + 0x1c) == 0) W16(p + 8, 0x70);
    else W16(p + 8, 0x77);
    W16(p + 0x1e, R16(p + 0x1c));
    SetSkelShakes(CH(F_ID));
    SET_CH(0x1a, 0);
    if (CH(F_FACE) == -1) {
        if (S16(IsSpace(S16(GetCellsInFront(1)))) != 0) goto shift;
    }
    if (CH(F_FACE) != 0) goto done;
    if (S16(IsSpace(S16(FUN_00021518()))) == 0) goto done;
shift:
    d5 = S16(seg4_021562());
    if (CH(F_FACE) == 0) d5 = (int16_t)(0x33 - d5);
    if (d5 > 0x36) goto done;
    d3 = (int16_t)(0x36 - d5);
    d5 = d3;
    if (CH(F_FACE) == -1) d5 = (int16_t)-d3;
    SET_CH(F_X, AddCharX(d5));
done:
    W16(p + 0xa, 0);
    SET_CH(0x36, 0);
    return p;
}

/* 0001f556 */
uint32_t FUN_0001f556(void) {
    if (S16(DecLifePts(1)) != 0) {
        seg6_02fba2();
    } else {
        JumpSeq(0x7e);
        AddSound(0x3b);
    }
    return 0;
}

/* 0001f580: has the skeleton finished shaking (ready to stand up)? */
uint32_t SkelDoneShaking(int16_t id) {
    int16_t d4 = (int16_t)cpu_get(R_D4), d5;   /* returned unset when there is no info record */
    maddr info = GetOppInfoIdx(R16(gCurRoom), id);
    if (info == 0) return (uint16_t)d4;
    d4 = 0;
    d5 = 0;
    if (R16(info + 0x1c) != -1) {
        if (S16(IsExitChasingSkel(CH(F_ROOM))) != 0) {
            ReadCell(0x16, 4, 2);
            if (S16(seg5_02580e()) != 0 && KD(F_ROOM) == CH(F_ROOM) && KD(F_X) < CH(F_X)) {
                if (S16(PrinceSndOn(0x70)) == 0) AddSong(0x70);
                d4 = 1;
            } else {
                d5 = (R16(info + 0x1e) == 0 && CH(F_JSEQ) == 0x70) ? 1 : 0;
            }
        } else if (R16(info + 0x22) != 0) {
            W16(info + 0x22, R16(info + 0x22) - 1);
            d5 = 1;
        } else {
            d4 = 1;
        }
    }
    if (d5 != 0) SetSkelTimer(id, (int16_t)(S16(Rnd(0x1e)) + 0xf));
    return (uint16_t)d4;
}

/* 0001f66c: turn the skeleton toward the kid */
uint32_t FUN_0001f66c(void) {
    if (CH(F_X) < KD(F_X)) {
        if (CH(F_FACE) != 0) {
            SET_CH(F_FACE, 0);
            SET_CH(F_X, AddCharX(0x10));
        }
    } else if (CH(F_FACE) != -1) {
        SET_CH(F_FACE, -1);
        SET_CH(F_X, AddCharX(0x10));
    }
    return 0;
}

/* 0001f6b4: flame sword position */
uint32_t seg4_01f6b4(void) {
    AddFCharX(R16(GH(0x102dea)));
    int16_t dy = R16(GH(0x102dec));
    W16(GH(0x104b3e), R16(GH(0x104b3e)) + dy);
    int16_t d3 = R16(GH(0x104b40));
    if (d3 == 4 || d3 == 0x13 || d3 == 0x14) {
        if (CH(0x3c) != 0) W16(GH(0x104b40), 0x12 - CH(0x3c));
        if (CH(0x3c) <= -4) SET_CH(0x3c, 0);
        else SET_CH(0x3c, CH(0x3c) - 1);
    }
    return 0;
}

/* 0001f706: hit by the flame sword: next sequence (-1 = none) */
uint32_t seg4_01f706(void) {
    int16_t d3 = S16(FlameSwordDecLifePts(1)) != 0 ? -1 : 0x67;
    AddSound(10);
    FUN_00020cca();
    return (uint16_t)d3;
}

/* 0001f730 */
uint32_t FlameSwordDecLifePts(int16_t n) {
    int16_t d3;
    if (S16(DecLifePts(n)) != 0) {
        FUN_0001f76e();
        d3 = 1;
    } else {
        d3 = 0;
    }
    return (uint16_t)d3;
}

/* 0001f76e */
uint32_t FUN_0001f76e(void) {
    JumpSeq(0x6b);
    SetSkelTimer(CH(F_ID), 0x1e0);
    AddSong(0xee);
    SET_CH(0x1c, 0);
    SET_CH(0x18, 1);
    return 1;
}

/* 0001f79e: flame flicker */
uint32_t seg4_01f79e(void) {
    int16_t d3 = R16(GH(0x104b40)), d4, d5;
    if (d3 != 4 && d3 != 0x13 && d3 != 0x14) return 0;
    d4 = 8;
    if ((int32_t)CH(F_ROW) * 0x78 + 0x66 >= (int32_t)CH(F_Y)) {
        d4 = 1;
        d5 = 1;
    } else {
        d5 = -4;
        if (!((int32_t)CH(F_ROW) * 0x78 + 0x6e > (int32_t)CH(F_Y))) d4 = 1;
    }
    d3 = (int16_t)(S16(Rnd(d4)) + d5);
    SET_CH(F_Y, CH(F_Y) + d3);
    W16(GH(0x104b3e), R16(GH(0x104b3e)) + d3);
    _OffsetRect(GH(0x104b44), 0, d3);
    return 0;
}

/* 0001f834 */
uint32_t seg4_01f834(void) {
    int16_t d3 = CH(F_COL);
    if (d3 < -1 || d3 > 10) FUN_000223c8();
    return 0;
}

/* 0001f84e: the jinnee appears in front of the kid */
uint32_t TriggerJinnee(void) {
    int16_t d3, d4, d5, d7;
    mcopy(gKid, gChar, REC_SIZE);
    d7 = (int16_t)((int32_t)R16(gDirTab + (int16_t)(KD(F_FACE) * 2)) * 0x55);
    d7 = (int16_t)(d7 + KD(F_X));
    int16_t col = S16(GetCol(d7));
    int16_t row = KD(F_ROW);
    d3 = KD(F_ROOM);
    SET_CH(F_ID, room_count((uint16_t)d3));
    maddr cnt = GS() + 0x21a6 + (uint32_t)(uint16_t)d3 * 0xc0 - 0xc0;
    W16(cnt, R16(cnt) + 1);
    maddr info = GetOppInfoIdx(KD(F_ROOM), CH(F_ID));
    SET_CH(F_TYPE, 0xc);
    W16(info + 0x18, 9);
    SET_CH(F_ROOM, KD(F_ROOM));
    SET_CH(F_ROW, row);
    W16(info, (int16_t)(S16(FirstInRow(row)) + col));
    FUN_00020cca();
    W16(info + 2, d7);
    SET_CH(F_X, d7);
    SET_CH(F_COL, col);
    d3 = -1;
    W16(info + 4, d3);
    SET_CH(F_FACE, d3);
    d4 = 0;
    W16(info + 0x14, d4);
    SET_CH(F_SWORD, d4);
    W16(info + 0x16, 0);
    d5 = 0;
    SET_CH(0x3a, d5);
    SET_CH(0x18, 1);
    SET_CH(F_FIGHT, d4);
    SET_CH(0x34, d4);
    SET_CH(0x36, 0);
    SET_CH(0x1a, 1);
    W16(info + 8, 0xb4);
    W16(info + 0xa, d4);
    JumpSeq(0xb4);
    AddSong(0xf4);
    W16(info + 0x1a, 6);
    SetOppLifePts(info);
    W16(info + 6, 10);
    SET_CH(0x1c, d3);
    W16(GH(0x103062), d5);
    W16(GH(0x104962), d4);
    W16(GH(0x104966), d4);
    seg4_020fb2();
    SET_CH(F_ACTION, 1);
    CheckLoadedOppSet(R16(info + 0x18));
    seg4_022516();
    FUN_000225d2();
    return 0;
}

/* 0001f9ee: shadow merges with the kid when they are close: sequence or -1 */
uint32_t FUN_0001f9ee(void) {
    int16_t d3;
    int16_t d5 = OP(F_ID);
    int16_t d4 = S16(FUN_0001ff82());
    int32_t dist = d4 < 0 ? -(int32_t)d4 : (int32_t)d4;
    if (dist < 0x33) {
        d3 = CH(F_FACE);
        if (d3 != OP(F_FACE)) {
            d3 = (int16_t)~d3;
            SET_CH(F_FACE, d3);
            SET_CH(F_X, AddCharX(6));
        }
        int16_t d0 = (int16_t)(d4 - (int16_t)((int32_t)R16(gDirTab2 + (int16_t)(CH(F_FACE) * 2)) * 7));
        SET_CH(F_X, CH(F_X) - d0);
        SET_CH(F_Y, CH(F_Y) - 1);
        SET_CH(0x18, 0);
        mcopy(gKid, gChar, REC_SIZE);
        d3 = 0x47;
    } else {
        d3 = -1;
    }
    LoadShadAsOpp(d5);
    return (uint16_t)d3;
}

/* 0001fa82: kid touching the shadow's flame */
uint32_t seg4_01fa82(void) {
    int16_t d3, d4;
    if (CH(F_TYPE) != 0 || CH(F_JSEQ) != 5) {
        W16(GH(0x102ffc), 1);
        return 0;
    }
    W16(GH(0x102ffc), R16(GH(0x102ffc)) + 1);
    int16_t n = R16(GH(0x102ffc));
    d3 = 4;
    if (n < d3) return 0;
    if (n == d3) {
        d3 = S16(IsShadowImbued()) != 0 ? 1 : 0;
        PoPSetPalette((int16_t)(d3 + 0x7d0), 0x30);
    }
    d3 = 1;
    d4 = S16(DecLifePts(d3));
    if (d4 == 0) {
        SET_CH(0x20, CH(0x20) - 1);
    } else {
        JumpSeq(0x47);
        if (GS16(0x2186) == 6) SetKidDeathMethod(0x11);
        else SetKidDeathMethod(0xd);
    }
    if (R16(GH(0x102ffc)) == 8) {
        if (CH(F_LIFE) > 4) {
            FUN_0001fcd6();
            SET_CH(F_TYPE, 1);
            SET_CH(0x36, 0x30);
            JumpSeq(2);
            AddSound(0x104);
            AddSong(0x122);
        } else {
            JumpSeq(0x47);
            DecLifePts(100);
        }
        W16(GH(0x102ffc), 0);
    }
    AddSound(0x120);
    return 0;
}

/* 0001fb80 */
uint32_t seg4_01fb80(void) {
    if (CH(F_ROOM) == 4 && GS16(0x218a) == 0xd && CH(0x34) == 0xd) {
        FUN_0001c5a0();
        return 0;
    }
    if (CH(F_JSEQ) == 0xf2 && S32(GetMobTypeItem(0xc, 1)) == 0 && CH(F_LIFE) <= 2)
        PoPSetPalette(0x7d0, 0x30);
    return 0;
}

/* 0001fbda: kid palette from the kid's life points */
uint32_t seg4_01fbda(void) {
    int16_t d4 = -1;
    int16_t d3 = KD(F_LIFE);
    if (d3 > 2 && (int32_t)d3 + KD(0x22) <= 2 && KD(F_JSEQ) != 0xf2) {
        d4 = 0;
    } else {
        d3 = KD(F_LIFE);
        if (d3 <= 2 && (int32_t)d3 + KD(0x22) > 2) d4 = 1;
    }
    if (d4 != -1) PoPSetPalette((int16_t)(d4 + 0x7d0), 0x30);
    return 0;
}

/* 0001fc44: shadow blinking while the kid is touching the flame */
uint32_t seg4_01fc44(void) {
    int16_t d3, d4;
    if (CH(F_FRAME) == 0xf) {
        W16(GH(0x102ffc), 0);
        return 0;
    }
    d3 = CH(F_FRAME);
    if ((uint16_t)d3 >= 0x2d && (uint16_t)d3 <= 0x34) {
        d4 = CH(F_ROOM);
        if (!((d4 == 1 || d4 == 2) && GS16(0x218a) == 0xe)) {
            d4 = R16(GH(0x102ffc));
            if (d4 < 4) return 0;
            if (S16(IsShadowBlinkOn(d3, d4)) != 0) SET_CH(0x36, 0x30);
            else SET_CH(0x36, 0);
            return 0;
        }
    }
    if (CH(F_JSEQ) == 5) return 0;
    if (R16(GH(0x102ffc)) == 0) return 0;
    SetKidPalette(GS16(0x2186));
    W16(GH(0x102ffc), 0);
    return 0;
}

/* 0001fcd6: the shadow splits off from the kid as a new character */
uint32_t FUN_0001fcd6(void) {
    int16_t d3, d4;
    if (S16(IsSpace(S16(FUN_00021518()))) != 0) {
        SET_CH(F_X, AddCharX(0x19));
    } else if (S16(IsSpace(S16(GetCellsInFront(1)))) != 0) {
        SET_CH(F_X, AddCharX(-0x19));
    }
    mcopy(gKid, gChar, REC_SIZE);
    d3 = CH(F_ROOM);
    d4 = room_count((uint16_t)d3);
    maddr cnt = GS() + 0x21a6 + (uint32_t)(uint16_t)d3 * 0xc0 - 0xc0;
    W16(cnt, R16(cnt) + 1);
    maddr info = GetOppInfoIdx(CH(F_ROOM), d4);
    W16(info, (int16_t)(S16(FirstInRow(CH(F_ROW))) + CH(F_COL)));
    W16(info + 2, CH(F_X));
    W16(info + 4, CH(F_FACE));
    d3 = 0;
    W16(info + 6, d3);
    W16(info + 0xc, d3);
    W16(info + 0xe, d4);
    W16(info + 0x10, d3);
    W16(info + 0x14, d3);
    W16(info + 0x12, CH(F_LIFE));
    W16(info + 0x18, 10);
    W16(info + 0x1a, CH(0x20));
    SET_CH(F_ID, d4);
    SET_CH(F_TYPE, d3);
    SET_CH(0x36, 0x10);
    JumpSeq(0x47);
    AnimChar();
    seg4_022516();
    FUN_000225d2();
    return 0;
}

/* 0001fe08: scroll the view to the kid's room */
uint32_t CutToKidRoom(void) {
    uint8_t save[REC_SIZE];
    mcopy(gKid, gChar, REC_SIZE);
    DrawKidMeter(CH(F_LIFE), CH(0x20));
    int16_t k = S16(GetKidIdxAsOpp());
    LoadShad(k);
    save_rec(save, gChar);
    SET_CH(F_ID, 10);
    SET_CH(0x36, 0);
    SET_CH(F_LIFE, KD(F_LIFE));
    SET_CH(0x20, KD(0x20));
    SET_CH(0x22, KD(0x22));
    FUN_00020542();
    FUN_00018138();
    GetCharEdges();
    W16(GH(0x104a92), R16(GH(0x104aa0)));
    W16(GH(0x104a90), R16(GH(0x104a9e)));
    W16(GH(0x104a8e), R16(GH(0x104a98)));
    W16(GH(0x104a8c), R16(GH(0x104a96)));
    mcopy(gKid, gChar, REC_SIZE);
    load_rec(gChar, save);
    FUN_000223c8();
    W16(GH(0x107e48), 1);
    do {
        FUN_0002e9c2();
        if (R16(GH(0x10306c)) == 1) {
            W16(GH(0x104a8e), R16(GH(0x104a8e)) - 0x1fe);
            W16(GH(0x104a8c), R16(GH(0x104a8c)) - 0x1fe);
        } else if (R16(GH(0x10306c)) == 0) {
            W16(GH(0x104a8e), R16(GH(0x104a8e)) + 0x1fe);
            W16(GH(0x104a8c), R16(GH(0x104a8c)) + 0x1fe);
        }
    } while (R16(GH(0x10306c)) != -1);
    W16(GH(0x107e48), 0);
    FUN_000225d2();
    return 0;
}

/* 0001ff12: table index of the kid (first entry whose type is 0) */
uint32_t GetKidIdxAsOpp(void) {
    int16_t d4 = room_count(RU16(gCurRoom));
    int16_t d5 = 0;
    if (d4 > 0 && R16(GH(0x102e00)) != 0) {
        maddr p = GH(0x102e00);
        do {
            d5++;
            p += REC_SIZE;
            if (d5 >= d4) break;
        } while (R16(p) != 0);
    }
    return (uint16_t)d5;
}

/* 0001ff82: horizontal distance from the shadow to the kid (999 if not on the same row) */
uint32_t FUN_0001ff82(void) {
    int16_t d3 = S16(GetKidIdxAsOpp());
    int16_t d4;
    LoadKidwOpp(d3);
    if (CH(F_ROW) != OP(F_ROW)) {
        d4 = 0x3e7;
    } else {
        d4 = (int16_t)(CH(F_X) - OP(F_X));
        d3 = CH(F_FACE);
        if (d3 == OP(F_FACE) && CH(F_FRAME) != 0xb9)
            d4 = (int16_t)(d4 + (int16_t)((int32_t)R16(gDirTab2 + (int16_t)(d3 * 2)) * 6));
    }
    return (uint16_t)d4;
}

/* 0001ffda: blink pattern of the shadow touching the flame (frame 0x2e-0x33).
 * The original rebases its `frame` argument in place; the glue writes it back. */
static uint32_t IsShadowBlinkOn_(int16_t *pframe, int16_t count) {
    int16_t d3 = count, d4, d5;
    if (d3 < 4) return 0;
    d4 = *pframe;
    if ((uint16_t)d4 <= 0x2d || (uint16_t)d4 >= 0x34) return 0;
    d5 = (int16_t)(d3 - 4);
    *pframe = (int16_t)(*pframe - 0x2e);
    if (d5 < 2) {
        int32_t rem = (int32_t)(uint32_t)(uint16_t)*pframe % (int32_t)(3 - (int32_t)d5);
        d4 = rem == 0 ? 1 : 0;
    } else {
        d4 = (int16_t)(((int32_t)(uint32_t)(uint16_t)*pframe + 1) % 3);
    }
    return (uint16_t)d4;
}
uint32_t IsShadowBlinkOn(int16_t frame, int16_t count) {
    return IsShadowBlinkOn_(&frame, count);
}
void glue_IsShadowBlinkOn(void) {
    maddr sp = emu_sp();
    int16_t frame = R16(sp);
    uint32_t r = IsShadowBlinkOn_(&frame, R16(sp + 2));
    W16(sp, frame);
    emu_set_d0(r);
}

/* 00020060 */
uint32_t IsShadowImbued(void) {
    if (CH(0x34) == 0xd) return 1;
    if (GS16(0x2186) == 6 && CH(F_LIFE) > 2) return 1;
    return 0;
}

/* 0002009e: the shadow merges back into the kid */
uint32_t FUN_0002009e(void) {
    if (OP(F_TYPE) != 0) return 0;
    mcopy(gKid, gChar, REC_SIZE);
    LoadShad(OP(F_ID));
    FUN_000223c8();
    seg4_022516();
    FUN_000225d2();
    SET_CH(F_TYPE, 0);
    SET_CH(0x36, 0);
    SET_CH(0x18, 1);
    JumpSeq(0xe7);
    AnimChar();
    mcopy(gKid, gChar, REC_SIZE);
    return 0;
}

/* 00020100: draw the flame around an imbued shadow */
uint32_t SetupShadowFlame(void) {
    if (S16(IsShadowImbued()) == 0) return 0;
    seg4_022442();
    int16_t d3 = (int16_t)(RU16(GH(0x104b66)) % 9 + 0x1006);
    W16(GH(0x104b40), d3);
    FRAME(f, 4);
    uint32_t ok = GetFlameImgSize(d3, f);
    int16_t w = R16(f + 2);
    FRAME_END(f);
    if (ok != 0) {
        W16(GH(0x104b4c), R16(GH(0x105490)));
        W16(GH(0x104b42), 4);
        int16_t d4 = (int16_t)((int32_t)w >> 1);
        d4 = (int16_t)(d4 - (int16_t)((int32_t)R16(GH(0x104a9e)) >> 1));
        AddFCharX(d4);
        MakeFlameFCharClipRect();
        AddCharObj(0xe);
        /* the original also pushes CH(F_ID) as a third (unused) argument */
        MarkBufsForRect(JT_ADDR(FN_MarkFRedraw), GH(0x104b44));
    }
    seg4_022454();
    return 0;
}

/* 000201b0 */
uint32_t FUN_000201b0(void) {
    return 0;
}

/* 000201b2: follow the room links until (gCellCol, gCellRow) lies inside gCellRoom */
uint32_t Handler(void) {
    int16_t d7 = 0, d6 = R16(gCellRoom);
    if (R16(gCellCol) < 0) {
        d7 = 10;
        if (R16(gCellRoom) != 0) {
            d6 = R16(GS() + (uint32_t)RU16(gCellRoom) * 8 + 0x2078);
            if (d6 != 0) {
                W16(gCellCol, R16(gCellCol) + 10);
                d7 = 0;
                W16(gCellRoom, d6);
                Handler();
            }
        }
    } else if (R16(gCellCol) >= 10) {
        d7 = -10;
        if (R16(gCellRoom) != 0) {
            d6 = R16(GS() + (uint32_t)RU16(gCellRoom) * 8 + 0x207a);
            if (d6 != 0) {
                W16(gCellCol, R16(gCellCol) - 10);
                d7 = 0;
                W16(gCellRoom, d6);
                Handler();
            }
        }
    }
    if (R16(gCellRoom) != 0) {
        if (R16(gCellRow) < 0) {
            W16(gCellRow, R16(gCellRow) + 3);
            d6 = R16(GS() + (uint32_t)RU16(gCellRoom) * 8 + 0x207c);
            W16(gCellRoom, d6);
            Handler();
        } else if (R16(gCellRow) >= 3) {
            W16(gCellRow, R16(gCellRow) - 3);
            d6 = R16(GS() + (uint32_t)RU16(gCellRoom) * 8 + 0x207e);
            W16(gCellRoom, d6);
            Handler();
        }
    }
    if (d6 == 0 && R16(gCellRoom) != 0) W16(gCellRoom, d6);
    int16_t d3 = R16(gCellCol);
    if (d3 < 0 || d3 >= 10) W16(gCellCol, R16(gCellCol) + d7);
    return RU16(gCellRoom);
}

/* 000202be: tile id of cell (col,row) of room (following room links) */
uint32_t ReadCell(int16_t room, int16_t col, int16_t row) {
    int16_t d3 = col;
    if (!(R16(gCellCol) == d3 && R16(gCellRow) == row && R16(gCellRoom) == room)) {
        W16(gCellRoom, room);
        W16(gCellCol, d3);
        W16(gCellRow, row);
        W16(gCellRoom, Handler());
        W16(gCellIdx, (int16_t)(S16(FirstInRow(R16(gCellRow))) + R16(gCellCol)));
    }
    if (R16(gCellRoom) != 0 && RU16(gCellRoom) <= RU16(GS() + 0x2180)) {
        CalcBlue(R16(gCellRoom));
        W16(gCellId, R16(R32(GH(0x1037e0)) + (int32_t)R16(gCellIdx) * 2));
        W16(gCellSpec, R16(R32(GH(0x1037dc)) + (int32_t)R16(gCellIdx) * 4));
    } else {
        W16(gCellSpec, 0);
        W16(gCellId, GetUndefineCellId(col, row));
    }
    return RU16(gCellId);
}

/* 00020386: tile id outside the level (walls or empty space depending on the level) */
uint32_t GetUndefineCellId(int16_t col, int16_t row) {
    int16_t d3;
    maddr gs = GS();
    switch (RU16(gs + 0x2186)) {
    case 5:
        d3 = 0;
        break;
    case 6:
        d3 = R16(gCurRoom);
        if ((d3 == 7 || d3 == 8) && GS16(0x2186) == 6 && row < 3) { d3 = 0x14; break; }
        if (d3 == 3 && GS16(0x2186) == 6 && (col < 0 || col == 9) && row == 2) { d3 = 0x14; break; }
        d3 = 0;
        break;
    case 4:
        if (R16(GH(0x103044)) == 0 && row == -1) { d3 = 0; break; }
        d3 = R16(gCurRoom);
        if ((d3 == 0x1b && GS16(0x218a) == 6) || (d3 == 0x10 && GS16(0x218a) == 9)) d3 = 0;
        else d3 = 0x14;
        break;
    case 2:
        d3 = R16(gCurRoom);
        if ((d3 == 4 && R16(gs + 0x218a) == 0xd) || (R16(gs + 0x218a) == 0xd && d3 == 0xd)) d3 = 0;
        else d3 = 0x14;
        break;
    default:
        d3 = 0x14;
        break;
    }
    return (uint16_t)d3;
}

/* 000204a8: cell index of (col,row) in a room (30 = outside) */
uint32_t IndexCell(int16_t col, int16_t row) {
    int16_t d3 = row;
    if (d3 >= 3 || d3 < -1) return 0x1e;
    d3 = col;
    if (d3 >= 10 || d3 < 0) return 0x1e;
    if (row == -1) d3 = (int16_t)-(int16_t)(col + 1);
    else d3 = (int16_t)(S16(FirstInRow(row)) + col);
    return (uint16_t)d3;
}

/* 00020508 */
uint32_t IndexCell1(int16_t col, int16_t row) {
    int16_t d3 = S16(IndexCell(col, row));
    return (uint16_t)(d3 >= 0 ? d3 : 0x1e);
}

/* 00020542 */
uint32_t FUN_00020542(void) {
    seg4_02055e();
    FUN_0002054c();
    return 0;
}

/* 0002054c: recompute the character's column */
uint32_t FUN_0002054c(void) {
    SET_CH(F_COL, GetCharCol(S16(seg4_021542())));
    return 0;
}

/* 0002055e: fetch the frame definition (10 bytes) and sword definition (8 bytes)
 * of the character's current frame */
uint32_t seg4_02055e(void) {
    int16_t d3, d4 = 0;
    maddr a2 = DEREF(R32(GH(0x104b86)));
    maddr a3 = DEREF(R32(GH(0x104b82)));
    maddr src;
    switch (CH(F_TYPE)) {
    case 0: case 1: case 6:
        a2 = DEREF(R32(GH(0x104b8e)));
        src = a2 + (uint32_t)RU16(gChar + F_FRAME) * 10;
        mcopy(GH(0x102dee), src, 10);
        a3 = DEREF(R32(GH(0x104b8a)));
        break;
    case 2: case 4: case 10: case 12:
        d3 = CH(F_FRAME);
        if ((uint16_t)d3 >= 0x66 && (uint16_t)d3 <= 0x6a) d4 = 0x46;
        src = a2 + (uint32_t)(((int32_t)RU16(gChar + F_FRAME) - 0x95 + d4) * 10);
        mcopy(GH(0x102dee), src, 10);
        break;
    case 8:
        if (RU16(gChar + F_FRAME) < 0xb6) d4 = 0x2b;
        /* fall through */
    case 7: case 11:
        src = a2 + (uint32_t)(((int32_t)RU16(gChar + F_FRAME) + d4) * 10) - 0x5d2;
        mcopy(GH(0x102dee), src, 10);
        break;
    default:
        break;
    }
    src = a3 + (uint32_t)RU16(GH(0x102df0)) * 8;
    mcopy(GH(0x102de6), src, 8);
    return 0;
}

/* 0002067e: x moved by dx in the facing direction. The original negates its argument
 * in place; the glue writes it back. */
static uint32_t AddCharX_(int16_t *pdx) {
    if (CH(F_FACE) != 0) *pdx = (int16_t)-*pdx;
    return (uint16_t)(int16_t)(*pdx + CH(F_X));
}
uint32_t AddCharX(int16_t dx) {
    return AddCharX_(&dx);
}
void glue_AddCharX(void) {
    maddr sp = emu_sp();
    int16_t dx = R16(sp);
    uint32_t r = AddCharX_(&dx);
    W16(sp, dx);
    emu_set_d0(r);
}

/* 000206aa: same for the flame/sword image x (argument negated in place too) */
static uint32_t AddFCharX_(int16_t *pdx) {
    if (R16(GH(0x104b3a)) != 0) *pdx = (int16_t)-*pdx;
    W16(GH(0x104b3c), R16(GH(0x104b3c)) + *pdx);
    return RU16(GH(0x104b3c));
}
uint32_t AddFCharX(int16_t dx) {
    return AddFCharX_(&dx);
}
void glue_AddFCharX(void) {
    maddr sp = emu_sp();
    int16_t dx = R16(sp);
    uint32_t r = AddFCharX_(&dx);
    W16(sp, dx);
    emu_set_d0(r);
}

/* 000206dc: run the character's animation sequence: execute sequence opcodes
 * (negative words) until a frame number is reached */
uint32_t AnimChar(void) {
    int16_t d6 = CH(F_SEQPOS);
    int16_t d5 = 1, d7, loc6, loc4;
    maddr a3 = hGetSequence(CH(F_SEQ));
    if (a3 == 0) return 0;
    uint16_t st = (uint8_t)_HGetState(a3);
    _HLock(a3);
    do {
        d7 = S16(GetSeqOp(a3));
        int32_t op = (int32_t)d7 + 0x19;
        if (op < 0 || (int16_t)op > 0x18) {
            SET_CH(F_FRAME, d7);
            d5 = 0;
            continue;
        }
        switch ((int16_t)op) {
        case 0:     /* -25: blink/palette */
            SET_CH(0x36, GetSeqOp(a3));
            break;
        case 1:     /* -24: palette */
            d7 = S16(GetSeqOp(a3));
            if (d7 != 0) {
                loc6 = S16(GetSeqOp(a3));
                if (loc6 == 0xe1 && GS16(0x2186) == 6) loc6 = (int16_t)(loc6 - 0x10);
                SetPaletteToColorIdx(loc6);
            } else {
                FUN_0001c5a0();
            }
            break;
        case 2: {   /* -23: back up one sequence step */
            int32_t v = (int32_t)d6 - 1;
            if (v <= 0) v = 0;
            SET_CH(F_SEQPOS, (int16_t)v);
            break;
        }
        case 3:     /* -22: random jump */
            d7 = S16(GetSeqOp(a3));
            loc6 = S16(GetSeqOp(a3));
            loc4 = S16(GetSeqOp(a3));
            if (S16(Rnd(d7)) != 0) JumpSeq(loc6);
            else JumpSeq(loc4);
            break;
        case 4:     /* -21 */
            SET_CH(0x34, GetSeqOp(a3));
            break;
        case 5:     /* -20 */
            SET_CH(F_FRAME, d7);
            d5 = 0;
            break;
        case 6:     /* -19 */
            FUN_00020cca();
            break;
        case 7:     /* -18 */
            SET_CH(F_JSEQ, 0);
            break;
        case 8:     /* -17 */
            FUN_000223c8();
            d5 = 0;
            break;
        case 9:     /* -16 */
            W16(GH(0x10306e), R16(GH(0x10306e)) + 1);
            if (GS16(0x218a) == 6) {
                if (CH(F_COL) < 6) W16(GH(0x102ff4), 1);
                else W16(GH(0x102ff4), 2);
            } else {
                SetTensionLevel(0, 0);
            }
            break;
        case 10:    /* -15: sound */
            AnimCharTap(S16(GetSeqOp(a3)));
            break;
        case 11:    /* -14 */
            d7 = S16(GetSeqOp(a3));
            if (d7 == 1 || (d7 == 2 && R16(GH(0x104b92)) == 6)) {
                FUN_000220ac();
                if (CH(F_SEQPOS) != 0) break;
                _HSetState(a3, st & 0xff);
                a3 = hGetSequence(CH(F_SEQ));
                st = (uint8_t)_HGetState(a3);
                _HLock(a3);
            } else if (d7 == 3) {
                TriggerJinnee();
            }
            break;
        case 12:    /* -13 */
            W16(GH(0x103072), -1);
            break;
        case 13:    /* -12 */
            W16(GH(0x103072), 1);
            break;
        case 14:    /* -11 */
            d7 = S16(GetSeqOp(a3));
            SetKidDeathMethod(d7);
            break;
        case 15:    /* -10 */
            if (R16(GH(0x10305a)) == 0) {
                GetSeqOp(a3);
                break;
            }
            goto jump;
        case 16:    /* -9: add momentum */
            d7 = S16(GetSeqOp(a3));
            loc6 = S16(GetSeqOp(a3));
            if (d7 != 0 && CH(F_XMOM) < 0x18 && (CH(F_YMOM) == 0 || CH(F_YMOM) < 0x3f)) {
                int32_t v = (int32_t)CH(F_XMOM) + d7;
                if (v >= 0x18) v = 0x18;
                SET_CH(F_XMOM, (int16_t)v);
            }
            if (loc6 != 0 && CH(F_YMOM) < 0x3f) {
                int32_t v = (int32_t)CH(F_YMOM) + loc6;
                if (v >= 0x3f) v = 0x3f;
                SET_CH(F_YMOM, (int16_t)v);
            }
            break;
        case 17:    /* -8: set momentum (10000 = keep) */
            d7 = S16(GetSeqOp(a3));
            if (d7 != 0x2710) SET_CH(F_XMOM, d7);
            d7 = S16(GetSeqOp(a3));
            if (d7 != 0x2710) SET_CH(F_YMOM, d7);
            break;
        case 18:    /* -7: action */
            SET_CH(F_ACTION, GetSeqOp(a3));
            break;
        case 19: {  /* -6: dy */
            int16_t dy = S16(GetSeqOp(a3));
            SET_CH(F_Y, CH(F_Y) + dy);
            break;
        }
        case 20:    /* -5: dx */
            SET_CH(F_X, AddCharX(S16(GetSeqOp(a3))));
            break;
        case 21:    /* -4: down */
            SET_CH(F_ROW, CH(F_ROW) + 1);
            break;
        case 22:    /* -3: up */
            SET_CH(F_ROW, CH(F_ROW) - 1);
            break;
        case 23:    /* -2: turn around */
            if (CH(F_TYPE) == 10) SET_CH(F_X, AddCharX(0x19));
            SET_CH(F_FACE, ~CH(F_FACE));
            break;
        case 24:    /* -1: jump to sequence */
        jump:
            d7 = S16(GetSeqOp(a3));
            if (roof_fight_room() && d7 == 0x39 && CH(F_JSEQ) != 0x3c && CH(F_COL) == 9) {
                d7 = 0x56;
            } else if (d7 == 0xb && CH(F_SEQ) == 0x49 && S16(IsFloor(S16(FUN_00021076()))) == 0) {
                d7 = 0x17;
            }
            SET_CH(F_SEQ, d7);
            SET_CH(F_SEQPOS, 0);
            _HSetState(a3, st & 0xff);
            a3 = hGetSequence(d7);
            if (a3 == 0) break;
            st = (uint8_t)_HGetState(a3);
            _HLock(a3);
            break;
        }
    } while (d5 != 0);
    _HSetState(a3, st & 0xff);
    return 0;
}

/* 00020b2e: next word of the sequence (advances the sequence position) */
uint32_t GetSeqOp(uint32_t h) {
    int16_t d3 = (int16_t)cpu_get(R_D3);   /* returned unset for a NULL handle */
    if (h != 0) {
        maddr p = DEREF(h) + (uint32_t)((int32_t)CH(F_SEQPOS) * 2);
        d3 = R16(p);
        SET_CH(F_SEQPOS, CH(F_SEQPOS) + 1);
    }
    return (uint16_t)d3;
}

/* 00020b68: 'SEQS' resource of a sequence, from the sequence file */
uint32_t hGetSequence(int16_t id) {
    int16_t saved = S16(_CurResFile());
    _UseResFile(R16(GH(0x10385a)));
    uint32_t h = PoPGetResource(0x53455153, id);
    _HPurge(h);
    _UseResFile(saved);
    return h;
}

/* 00020bb0: sequence sound opcode */
uint32_t AnimCharTap(int16_t snd) {
    if (snd == 1) {
        W16(GH(0x104b64), R16(GH(0x104b64)) + 1);
        if (GS16(0x2186) != 1) {
            AddSound((int16_t)((R16(GH(0x104b64)) & 1) + 0x126));
            if (CH(F_TYPE) == 4 && S16(IsExitChasingSkel(CH(F_ROOM))) != 0 && CH(F_JSEQ) == 0xd8)
                SET_CH(F_X, AddCharX((int16_t)(CH(F_ID) << 1)));
        }
    } else if (snd != 0) {
        if (snd > 0x12d) return 0;
        if (snd == 0x10e) AddSong(snd);
        else AddSound(snd);
        return 0;
    }
    if (CH(F_TYPE) == 0) W16(GH(0x103062), 1);
    return 0;
}

/* 00020c66 */
uint32_t SetKidDeathMethod(int16_t how) {
    if (CH(F_TYPE) == 0 || CH(F_TYPE) == 1) {
        int16_t d3 = how;
        W16(GH(0x102ff2), d3);
        if (d3 == 3 && GS16(0x218a) == 5 && R16(gCurRoom) == 10) SetTensionLevel(3, 5);
    }
    return 0;
}

/* 00020cca: y of the character standing on its row */
uint32_t FUN_00020cca(void) {
    SET_CH(F_Y, (int16_t)((int32_t)CH(F_ROW) * 0x78 + 0x6a));
    int16_t d3 = CH(F_TYPE);
    if (d3 == 7 || d3 == 8) SET_CH(F_Y, CH(F_Y) - 0x21);
    return RU16(gChar + F_Y);
}

/* 00020cfa: index of the first cell of a row */
uint32_t FirstInRow(int16_t row) {
    int16_t d4;
    if (row >= 0) d4 = R16(GH(0x104af2) + (int16_t)(row * 2));
    else d4 = (int16_t)(R16(GH(0x104af4) + (int16_t)(row * 2)) - 1);
    return (uint16_t)d4;
}

/* 00020d42 */
uint32_t FUN_00020d42(void) {
    mcopy(gOpp, gKid, REC_SIZE);
    return 0xffff;
}

/* 00020d56: load table entry `i` into the opponent record */
uint32_t LoadKidwOpp(int16_t i) {
    FUN_000225d2();
    mcopy(gOpp, gOppTab + (uint32_t)((int32_t)i * REC_SIZE), REC_SIZE);
    return 0xffff;
}

/* 00020da0: save the character into the kid record and the opponent into the table */
uint32_t FUN_00020da0(void) {
    mcopy(gKid, gChar, REC_SIZE);
    mcopy(gOppTab + (uint32_t)((int32_t)OP(F_ID) * REC_SIZE), gOpp, REC_SIZE);
    return 0xffff;
}

/* 00020de2 */
uint32_t LoadShadAsOpp(int16_t i) {
    mcopy(gOpp, gOppTab + (uint32_t)((int32_t)i * REC_SIZE), REC_SIZE);
    return 0xffff;
}

/* 00020e2a */
uint32_t LoadShadwOpp(int16_t i) {
    LoadShad(i);
    mcopy(gOpp, gKid, REC_SIZE);
    return 0xffff;
}
