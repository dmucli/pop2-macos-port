/* CODE 6 "Characters 1", second part (Ghidra 0x2d58e-0x3059e): the per-room opponent
 * info records (moving guards between rooms, saving/restoring them on room changes),
 * opponent generation points, the room "cut" (the kid or a guard leaving the room),
 * the automatic control of non-kid characters, and sword-strike tests.
 * Ported from the Ghidra decompilation/disassembly of the original 68k code; addresses
 * in comments are Ghidra addresses (see docs/PORTING.md). */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* ---- Character records (0x3e bytes, same layout everywhere, see seg04a) ----
 *   +00 id  +02 face (0 right, -1 left, 0x56 none)  +04 x  +06 y  +08 type
 *   +0a frame  +0c column  +0e row  +10 action  +12/+14 momentum  +16 room
 *   +18  +1a  +1c alive (<0 alive)  +1e life  +20 max life  +22 pending life
 *   +24 sequence position  +26 sequence  +28 last jump sequence  +32 fight state
 *   +34  +36 palette bank  +38 sword  +3a */
#define REC_SIZE    0x3e
#define gChar       GH(0x102faa)   /* the character being processed */
#define gOpp        GH(0x102f6c)   /* its opponent */
#define gKid        GH(0x102f2e)   /* the kid (saved record) */
#define gOppTab     GH(0x102df8)   /* the current room's characters, REC_SIZE each */
#define gGameState  GH(0x102fec)   /* long: pointer to the game state block */
#define gCurRoom    GH(0x10303e)   /* word: current room */
#define gNextRoom   GH(0x10303c)   /* word: room the kid is going to */
#define gRoomLeft   GH(0x103040)   /* word: room on the left of the current room */
#define gRoomRight  GH(0x103042)   /* word: room on the right */
#define gRoomBelow  GH(0x103046)   /* word: room below */
#define gLevel      GH(0x10302e)   /* word: current level */
#define gNextLevel  GH(0x10306e)   /* word: next level (== gLevel while playing it) */
#define gCutDir     GH(0x10306c)   /* word: direction of the last room cut (CutChar) */
#define gCutDelay   GH(0x103038)   /* word: frames before the next room cut is allowed */
#define gOppMeter   GH(0x102f68)   /* word: id of the character whose life meter is shown */
#define gOppTarget  GH(0x102fa6)   /* word: id of the opponent the kid fights */
#define gOppBanks   GH(0x103078)   /* word[2]: opponent kind loaded in palette banks 0x20/0x30 */
#define gOppSkillTab GH(0x102d96)  /* word[char type]: per-type value stored at info+0x18 */
#define gKindToType GH(0x102d80)   /* word[opponent kind]: character type */
#define gColX       GH(0x103474)   /* word[col]: x of a column, facing right */
#define gColX2      GH(0x10347c)   /* word[col]: x of a column, facing left */
#define gColX3      GH(0x103478)   /* word[]: x positions (FUN_00021600 result) */
#define gDirStep    GH(0x10346c)   /* word[face]: column step by face */
#define gRecFrame   GH(0x104b66)   /* word: frame counter */
#define gCellId     GH(0x104b50)   /* word: tile id of the last cell read */
#define gCellCol    GH(0x104b58)   /* word: column of the last cell read */
#define gCharBoxR   GH(0x104a8c)   /* word: character image box (right edge) */
#define gCharBoxL   GH(0x104a8e)   /* word: character image box (left edge) */
#define gCharBoxW   GH(0x104a90)   /* word: character image width */
#define gCharBoxH   GH(0x104a92)   /* word: character image height */
#define gNoCutFlag  GH(0x107e48)   /* word: when set, edge cuts need the link */
#define gRoofOppId  GH(0x104e58)   /* word: rooftop opponent kept in its room */
#define gTensionLvl GH(0x10495e)   /* word: music tension level */
#define gTension    GH(0x104960)   /* word: music tension amount */
#define gAITimer1   GH(0x104962)   /* word: AI timers counted down by AutoCtrl */
#define gAITimer2   GH(0x104964)
#define gAITimer3   GH(0x104966)
#define gStabTab    GH(0x104946)   /* word[skill]: value for gAITimer3 */
#define gFrameTabH  GH(0x104b8e)   /* long: handle to the 10-byte frame definitions */
#define gDifficulty GH(0x102ff6)   /* word */
#define gGenDone    GH(0x103062)   /* word */

/* field offsets */
#define F_ID      0x00
#define F_FACE    0x02
#define F_X       0x04
#define F_Y       0x06
#define F_TYPE    0x08
#define F_FRAME   0x0a
#define F_COL     0x0c
#define F_ROW     0x0e
#define F_ACTION  0x10
#define F_YMOM    0x14
#define F_ROOM    0x16
#define F_18      0x18
#define F_1A      0x1a
#define F_ALIVE   0x1c
#define F_LIFE    0x1e
#define F_MAXLIFE 0x20
#define F_PEND    0x22
#define F_SEQPOS  0x24
#define F_SEQ     0x26
#define F_JSEQ    0x28
#define F_FIGHT   0x32
#define F_34      0x34
#define F_BANK    0x36
#define F_SWORD   0x38
#define F_3A      0x3a

#define CH(o)       R16(gChar + (o))
#define SET_CH(o,v) W16(gChar + (o), (v))
#define OP(o)       R16(gOpp + (o))
#define SET_OP(o,v) W16(gOpp + (o), (v))
#define KD(o)       R16(gKid + (o))
#define GS()        R32(gGameState)
#define GS16(o)     R16(GS() + (o))
#define LEVEL_TYPE() GS16(0x2186)
#define LEVEL_NUM()  GS16(0x218a)
#define OPP_KIND()   GS16(0x21a4)

/* Opponent info records: 0x26 bytes, up to 5 per room, at
 * gs + 0x21a6 + room*0xc0 - 0xbe + i*0x26; the count is the word at gs + room*0xc0 + 0x20e6.
 *   +00 cell  +02 x  +04 face  +06  +08 sequence  +0a sequence position  +0c bank
 *   +0e index  +10 (char +1a)  +12 life  +14 sword  +16  +18 skill  +1a  +1c..+25 */
#define INFO_SIZE 0x26

/* character count of a room (0 for room 0): room zero-extended / sign-extended as the
 * original computes it (identical for real rooms) */
static int16_t cnt_u(uint16_t room) {
    if (room == 0) return 0;
    return R16(GS() + (uint32_t)room * 0xc0 + 0x20e6);
}
static int16_t cnt_s(int16_t room) {
    if (room == 0) return 0;
    return R16(GS() + 0x21a6 + (uint32_t)((int32_t)room * 0xc0) - 0xc0);
}
static maddr cnt_addr_u(uint16_t room) { return GS() + (uint32_t)room * 0xc0 + 0x20e6; }
/* room links {left, right, up, down} */
static maddr link_addr(uint16_t room) { return GS() + (uint32_t)room * 8 + 0x2078; }
/* opponent generation points of a room: count word, then 0x14-byte entries
 *   +02 max opponents near  +04 row  +06 col  +08 timer  +0a timer reload  +0c sword
 *   +0e second row  +10 opponents left  +12 skill (low 4 bits) / 0x80 tumble */
static maddr genpt_room(uint16_t room) { return GS() + (uint32_t)room * 0x44 + 0x3986; }

static void copy_char_to_kid(void) { mcopy(gKid, gChar, REC_SIZE); }

/* 0002d58e: move guards of the left/right rooms that are visible from the current room
 * into it */
uint32_t CheckSideRoomsForVisibleOpps(void) {
    ENTRY_LINK(FN_CheckSideRoomsForVisibleOpps, 0x30, 24);   /* D3-D7/A2 */
    maddr rec = a6 - 0x2c;
    uint16_t side = 0;
    do {
        uint16_t room; int16_t dx;
        if (side == 0) { room = RU16(gRoomLeft); dx = -0x1fe; }
        else { room = RU16(gRoomRight); dx = 0x1fe; }
        if (room != 0) {
            int16_t n = cnt_u(room);
            int16_t i = 0;
            if (n > 0) {
                int32_t left = (side == 0);
                do {
                    maddr info = GetOppInfoIdx((int16_t)room, i);
                    if ((left != 0 && R16(info + 2) > 0x249) ||
                        (side == 1 && R16(info + 2) < 0x14b) ||
                        ((room == 7 || room == 8) && LEVEL_TYPE() == 6 && R16(info + 0x18) == 3)) {
                        mcopy(rec, info, INFO_SIZE);
                        W16(rec + 2, R16(rec + 2) + dx);
                        W16(rec + 0xe, cnt_u(RU16(gCurRoom)));
                        AddOppToRoom(R16(gCurRoom), rec);
                        RemoveOppFromRoom((int16_t)room, i);
                        i--;
                        n--;
                    }
                    i++;
                } while (i < n);
            }
        }
        side++;
    } while (side < 2);
    return 0;
}

/* 0002d6f0: store the current character back into its room's info record (or move it
 * to the room it walked into / remove it); returns 1 when it left the room */
uint32_t UpdateOppInfo(void) {
    ENTRY_LINK(FN_UpdateOppInfo, 0x28, 12);   /* D3/D4/A3 */
    int16_t r = 0;
    int16_t j;
    if (CH(F_FACE) == 0x56 || CH(F_34) == 5 || CH(F_34) == 4 ||
        (j = CH(F_JSEQ)) == 0xc4 || j == 0x9a) {
        RemoveOppFromRoom(CH(F_ROOM), CH(F_ID));
        return 1;
    }
    maddr info = GetOppInfoIdx(CH(F_ROOM), CH(F_ID));
    if (info != 0) {
        int16_t cell = S16(IndexCell(CH(F_COL), CH(F_ROW)));
        W16(info, cell);
        if (cell != 0x1e) {
            W16(info + 2, CH(F_X));
            W16(info + 4, CH(F_FACE));
            if (CH(F_ALIVE) < 0) {
                W16(info + 0xa, 0);
            } else {
                W16(info + 0xa, CH(F_SEQPOS));
                W16(info + 8, CH(F_SEQ));
            }
            W16(info + 0x10, CH(F_1A));
            W16(info + 0x12, CH(F_LIFE));
            W16(info + 0x18, R16(gOppSkillTab + (int16_t)(CH(F_TYPE) * 2)));
            SetSpecialOppInfo(info);
            int16_t k = S16(FUN_00021076());
            if (k == 5 || k == 6) FUN_000266ae();
        } else {
            int16_t nr = S16(FUN_0002ed6a());
            if (nr != 0 && CH(F_TYPE) != 0xc) {
                maddr p = GetOppInfoIdx(CH(F_ROOM), CH(F_ID));
                maddr rec = a6 - 0x28;
                W16(rec, S16(FirstInRow(CH(F_ROW))));
                W16(rec + 2, CH(F_X));
                W16(rec + 4, CH(F_FACE));
                W16(rec + 6, R16(p + 6));
                W16(rec + 8, CH(F_SEQ));
                W16(rec + 0xa, CH(F_SEQPOS));
                W16(rec + 0xc, R16(p + 0xc));
                W16(rec + 0x10, CH(F_1A));
                W16(rec + 0x12, CH(F_LIFE));
                W16(rec + 0x14, CH(F_SWORD));
                W16(rec + 0x16, CH(F_3A));
                W16(rec + 0x18, R16(p + 0x18));
                W16(rec + 0x1a, R16(p + 0x1a));
                mcopy(rec + 0x1c, p + 0x1c, 10);
                AddOppToRoom(nr, rec);
            } else if (CH(F_ACTION) == 9 && LEVEL_TYPE() == 5) {
                PrinceResetSnd(0x1e);
                int16_t room = R16(gCurRoom);
                if (room != 0x13 && room != 0x10) AddSound(7);
            }
            RemoveOppFromRoom(CH(F_ROOM), CH(F_ID));
            r = 1;
            if (R16(gCurRoom) == 0 || cnt_s(R16(gCurRoom)) == 0)
                W16(GH(0x102dfa) + (int16_t)((int32_t)CH(F_ID) * REC_SIZE), 0x56);
        }
    }
    SET_CH(F_LIFE, 0);
    return (uint16_t)r;
}

/* 0002d944: the current character leaves the room through the last cut: its info
 * record moves to the new room */
uint32_t TransferOppInfo(void) {
    ENTRY_LINK(FN_TransferOppInfo, 0x28, 4);   /* A2 */
    maddr p = GetOppInfoIdx(CH(F_ROOM), CH(F_ID));
    maddr rec = a6 - 0x28;
    mcopy(rec, p, INFO_SIZE);
    RemoveOppFromRoom(CH(F_ROOM), CH(F_ID));
    Cut(R16(gCutDir));
    W16(rec, S16(FirstInRow(CH(F_ROW))));
    W16(rec + 2, CH(F_X));
    W16(rec + 4, CH(F_FACE));
    W16(rec + 8, CH(F_SEQ));
    W16(rec + 0xa, CH(F_SEQPOS));
    W16(rec + 0x10, CH(F_1A));
    W16(rec + 0x12, CH(F_LIFE));
    if (CH(F_BANK) == 0x20) W16(rec + 0xc, R16(gOppBanks));
    else W16(rec + 0xc, R16(gOppBanks + 2));
    SetSpecialOppInfo(rec);
    AddOppToRoom(CH(F_ROOM), rec);
    return 0;
}

/* 0002d9fc: type-specific fields of an info record */
uint32_t SetSpecialOppInfo(uint32_t info) {
    ENTRY_LINK(FN_SetSpecialOppInfo, 0, 8);   /* A2/D3 */
    if (CH(F_TYPE) == 2) {
        int16_t j = CH(F_JSEQ);
        if (j == 100 || j == 0xba) {
            W16(info + 0x1c, CH(F_Y));
        } else if (OPP_KIND() == 7) {
            W16(info + 8, CH(F_SEQ));
            W16(info + 0x1c, 0);
        }
        if (OPP_KIND() == 7) W16(info + 0x18, 7);
    } else if (CH(F_TYPE) == 0xb) {
        SnakeUpdateOppInfo(info);
    } else if (CH(F_TYPE) == 6) {
        JaffarUpdateOppInfo(info);
    }
    return 0;
}

/* 0002daa2: remove info record idx of a room (later ones move down; in the current
 * room the character table moves too). The original advances idx in its own argument
 * slot, see glue_RemoveOppFromRoom. */
static void RemoveOppFromRoom_(int16_t room, int16_t *pidx) {
    ENTRY_LINK(FN_RemoveOppFromRoom, 0, 24);   /* D3-D6/A2/A3 */
    int16_t n = cnt_u((uint16_t)room);
    if (*pidx >= n) return;
    int32_t d6 = (int32_t)n;
    if ((int32_t)*pidx + 1 < d6) {
        do {
            maddr dst = GetOppInfoIdx(room, *pidx);
            maddr src = GetOppInfoIdx(room, (int16_t)(*pidx + 1));
            mcopy(dst, src, INFO_SIZE);
            W16(dst + 0xe, *pidx);
            if (room == R16(gCurRoom)) {
                uint32_t off = (uint32_t)((int32_t)*pidx * REC_SIZE);
                mcopy(gOppTab + off, GH(0x102e36) + off, REC_SIZE);
                W16(gOppTab + off, *pidx);
            }
            *pidx = (int16_t)(*pidx + 1);
        } while ((int32_t)*pidx + 1 < d6);
    }
    maddr c = cnt_addr_u((uint16_t)room);
    W16(c, R16(c) - 1);
}
uint32_t RemoveOppFromRoom(int16_t room, int16_t idx) {
    RemoveOppFromRoom_(room, &idx);
    return 0;
}
void glue_RemoveOppFromRoom(void) {
    maddr sp = emu_sp();
    int16_t room = (int16_t)rd16(sp), idx = (int16_t)rd16(sp + 2);
    RemoveOppFromRoom_(room, &idx);
    wr16(sp + 2, (uint16_t)idx);
    emu_set_d0(0);
}

/* 0002dbce: append an info record to a room (at most 5) */
uint32_t AddOppToRoom(int16_t room, uint32_t rec) {
    ENTRY_LINK(FN_AddOppToRoom, 0, 16);   /* D3/D4/A2/A3 */
    int16_t n = cnt_s(room);
    if (n < 5) {
        W16(rec + 0xe, n);
        maddr c = GS() + 0x21a6 + (uint32_t)((int32_t)room * 0xc0) - 0xc0;
        W16(c, R16(c) + 1);
        maddr dst = GetOppInfoIdx(room, n);
        mcopy(dst, rec, INFO_SIZE);
    } else {
        DrawOppMeter(0, 0);
    }
    return 0;
}

/* 0002dc82: address of info record idx of a room, 0 if there is none */
uint32_t GetOppInfoIdx(int16_t room, int16_t idx) {
    int32_t n = cnt_s(room);
    int32_t i = idx;
    if (n <= i) return 0;
    return GS() + 0x21a6 + (uint32_t)((int32_t)room * 0xc0) - 0xbe + (uint32_t)(i * INFO_SIZE);
}

/* 0002dd32: id of the living character of the current room closest to the kid in
 * the given facing direction (-1 if none) */
uint32_t GetOppIdxClosestToKid(int16_t face) {
    ENTRY_LINK(FN_GetOppIdxClosestToKid, 0x1c, 32);   /* D3-D7/A2-A4 */
    maddr best = 0;
    int16_t bestIsSnake = 0;
    if (KD(F_ROOM) != 0) {
        int16_t i = 0;
        int16_t n = cnt_u(RU16(gKid + F_ROOM));
        maddr p = gOppTab;
        if (n > 0) {
            int32_t f0 = (face == 0);
            int16_t dy = 0, dx = 0;
            do {
                maddr info = GetOppInfoIdx(R16(gCurRoom), i);
                int32_t t = (int32_t)R16(p + F_Y) - (int32_t)KD(F_Y);
                if (t < 0) t = -t;
                dy = (int16_t)t;
                int skip = 0;
                if (R16(p + F_ALIVE) >= 0) {
                    skip = 1;
                } else {
                    int16_t ty = R16(p + F_TYPE);
                    if ((ty == 7 || ty == 8) && R16(info + 0x1c) == 2) skip = 1;
                }
                if (skip) {
                    dy = -1;
                } else if (face == 1) {
                    t = (int32_t)R16(p + F_X) - (int32_t)KD(F_X);
                    if (t < 0) t = -t;
                    dx = (int16_t)t;
                } else if (face == -1 && KD(F_X) >= R16(p + F_X)) {
                    t = (int32_t)KD(F_X) - (int32_t)R16(p + F_X);
                    if (t <= 0) t = 0;
                    dx = (int16_t)t;
                } else if (f0 != 0 && KD(F_X) < R16(p + F_X)) {
                    t = (int32_t)R16(p + F_X) - (int32_t)KD(F_X);
                    if (t <= 0) t = 0;
                    dx = (int16_t)t;
                } else {
                    dy = -1;
                }
                if (dy >= 0 && dx >= 0 && !(best != 0 && R16(p + F_TYPE) == 0xb)) {
                    if (best != 0 && bestIsSnake == 0) {
                        int32_t bdx = (int32_t)R16(best + F_X) - (int32_t)KD(F_X);
                        if (bdx < 0) bdx = -bdx;
                        int32_t bdy = (int32_t)R16(best + F_Y) - (int32_t)KD(F_Y);
                        if (bdy < 0) bdy = -bdy;
                        int32_t dd = (int32_t)dy - (int32_t)(int16_t)bdy;
                        if (dd < 0) dd = -dd;
                        int16_t d7 = (int16_t)dd;
                        if ((d7 > 0x50 && dy < (int16_t)bdy) || (d7 < 0x50 && dx < (int16_t)bdx))
                            best = p;
                    } else {
                        best = p;
                        bestIsSnake = (R16(p + F_TYPE) == 0xb);
                    }
                }
                i++;
                p += REC_SIZE;
            } while (i < n);
        }
    }
    if (best != 0) return (uint32_t)(int32_t)R16(best);
    return 0xffffffffu;
}

/* 0002df86: is the current character the one closest to the kid, looking from the kid
 * towards it */
uint32_t FUN_0002df86(void) {
    ENTRY_NOLINK(FN_FUN_0002df86, 4);   /* D3 */
    int16_t face = (KD(F_X) < CH(F_X)) ? 0 : -1;
    int16_t id = S16(GetOppIdxClosestToKid(face));
    return id == CH(F_ID) ? 1 : 0;
}

/* 0002dfb2: choose the palette bank of the current character */
uint32_t SetCharacterBank(int16_t kind) {
    ENTRY_LINK(FN_SetCharacterBank, 0, 4);   /* D3 */
    if (CH(F_TYPE) == 0) {
        SET_CH(F_BANK, 0x10);
    } else if (CH(F_TYPE) == 6) {
        SET_CH(F_BANK, 0x20);
        PoPSetPalette(0x61b0, 0x20);
        if (R16(gCurRoom) == 6 && LEVEL_TYPE() == 6)
            SET_CH(F_FACE, S16(Rnd(1)) - 1);
    } else {
        int16_t k = OPP_KIND();
        if (k == 0 || k == 5 || k == 6) PresetOppBanks(kind);
        else SET_CH(F_BANK, 0);
    }
    return 0;
}

/* 0002e04c: generate an opponent at the generation point on the kid's row when due */
uint32_t CheckOppGenPts(int16_t room) {
    ENTRY_LINK(FN_CheckOppGenPts, 0, 4);   /* A2 */
    if (KD(F_ALIVE) < 0 && KD(F_TYPE) != 1) {
        maddr g = GetOppGenPtOnRow(KD(F_ROOM), KD(F_ROW));
        if (g != 0 && S16(IsTimeToGenOpp(g, room)) != 0) {
            if (R16(g + 8) == 0) {
                GenerateOpponent(g, room);
                W16(g + 8, R16(g + 0xa));
                W16(g + 0x10, R16(g + 0x10) - 1);
            } else {
                W16(g + 8, R16(g + 8) - 1);
            }
        }
    }
    return 0;
}

/* 0002e0c4: create a new opponent of a room from a generation point and make it the
 * current character */
uint32_t GenerateOpponent(uint32_t g, int16_t room) {
    ENTRY_LINK(FN_GenerateOpponent, 0x10, 24);   /* D3-D5/A2-A4 */
    int16_t n = (room == 0) ? 0 : R16(GS() + 0x21a6 + (uint32_t)(uint16_t)room * 0xc0 - 0xc0);
    if (n >= 5) return 0;
    maddr c = GS() + 0x21a6 + (uint32_t)(uint16_t)room * 0xc0 - 0xc0;
    W16(c, R16(c) + 1);
    maddr info = GetOppInfoIdx(room, n);
    W16(info, S16(FirstInRow(R16(g + 4))) + R16(g + 6));
    int16_t col = R16(g + 6);
    if (col <= 5) {
        W16(info + 2, R16(gColX + (int16_t)(col * 2)));
        W16(info + 4, 0);
    } else {
        W16(info + 2, R16(gColX2 + (int16_t)(R16(g + 6) * 2)));
        W16(info + 4, -1);
    }
    maddr gp = GS() + 0x39ca + (uint32_t)(uint16_t)room * 0x44 - 0x44;
    W16(info + 6, R16(gp + 2));
    W16(info + 0xc, R16(gOppBanks));
    W16(info + 0xe, n);
    W16(info + 0x10, 0);
    W16(info + 0x14, R16(g + 0xc));
    W16(info + 0x20, R16(g + 0xe));
    int16_t sk = R16(g + 0x12) & 0xf;
    W16(info + 0x12, sk);
    W16(info + 0x1a, sk);
    SET_CH(F_ID, n);
    SET_CH(F_FACE, R16(info + 4));
    SET_CH(F_X, R16(info + 2));
    maddr gs = GS();
    SET_CH(F_TYPE, R16(gKindToType + (int16_t)(R16(gs + 0x21a4) * 2)));
    SET_CH(F_COL, R16(g + 6));
    SET_CH(F_ROW, R16(g + 4));
    SET_CH(F_SWORD, R16(g + 0xc));
    SET_CH(F_ROOM, room);
    int16_t life = R16(info + 0x12);
    SET_CH(F_MAXLIFE, life);
    SET_CH(F_LIFE, life);
    SET_CH(F_PEND, life);
    SET_CH(F_ALIVE, -1);
    if (R16(gs + 0x2186) == 2) SET_CH(F_BANK, 0);
    else SET_CH(F_BANK, 0x20);
    FUN_00020cca();
    seg4_020fb2();
    SET_CH(F_34, 0);
    SET_CH(F_18, 1);
    int16_t kcol = KD(F_COL);
    int16_t kface = KD(F_FACE);
    if (CH(F_FACE) != kface) {
        uint16_t fr = RU16(gKid + F_FRAME);
        if ((fr >= 1 && fr <= 0xe) || (fr >= 0x31 && fr <= 0x38) || (fr >= 0x22 && fr <= 0x2c))
            kcol = (int16_t)(kcol - R16(gDirStep + (int16_t)(kface * 2)));
    }
    int32_t gcol = R16(g + 6);
    if (gcol + 3 < (int32_t)kcol || gcol - 3 > (int32_t)kcol || (uint16_t)(R16(g + 0x12) & 0xf) < 6) {
        SET_CH(F_1A, 0);
        JumpSeq(0x54);
        SET_CH(F_FIGHT, 0);
    } else {
        SET_CH(F_1A, 1);
        JumpSeq(0x5a);
        SET_CH(F_FIGHT, 3);
    }
    AnimChar();
    if (R16(gOppMeter) == -1) W16(gOppMeter, CH(F_ID));
    seg4_022516();
    return 0;
}

/* 0002e368: the active generation point of a room on a row (0 if none) */
uint32_t GetOppGenPtOnRow(int16_t room, int16_t row) {
    ENTRY_LINK(FN_GetOppGenPtOnRow, 0, 12);   /* D4/D5/A2 */
    maddr g = 0;
    if (room == 0) return 0;
    int16_t n = R16(genpt_room((uint16_t)room));
    int16_t i = 0;
    if (n <= 0) return 0;
    do {
        g = GetOppGenPtIdx(room, i);
        if (S16(IsActiveOppGenPt(g, row)) != 0) break;
        i++;
        g = 0;
    } while (i < n);
    return g;
}

/* 0002e3ea: generation point on this row, more than 2 columns from the kid, with
 * opponents left */
uint32_t IsActiveOppGenPt(uint32_t g, int16_t row) {
    if (R16(g + 4) == row || R16(g + 0xe) == row) {
        int32_t d = (int32_t)R16(g + 6) - (int32_t)KD(F_COL);
        if (d < 0) d = -d;
        if (d > 2 && R16(g + 0x10) != 0) return 1;
    }
    return 0;
}

/* 0002e44a: may the generation point produce an opponent now (no living character
 * between it and the kid, ...) */
uint32_t IsTimeToGenOpp(uint32_t g, int16_t room) {
    ENTRY_LINK(FN_IsTimeToGenOpp, 0x20, 32);   /* D3-D7/A2-A4 */
    int16_t n = cnt_u((uint16_t)room);
    if (n >= 5) return 0;
    if (R16(gNextLevel) != R16(gLevel)) return 0;
    {   /* divu #3, swap, tst.l: tests quotient and remainder together, i.e. frame == 0 */
        uint16_t f = RU16(gRecFrame);
        if ((((uint32_t)(f % 3) << 16) | (uint32_t)(f / 3)) == 0) return 0;
    }
    int16_t ok = 1;
    int16_t i = 0;
    uint16_t near = 0;
    int16_t gcol = R16(g + 6);
    int16_t dir = gcol < 5 ? 1 : -1;
    int16_t wall = S16(Get1stWallColInDir(room, gcol, R16(g + 4), dir));
    if (n <= 0) return 1;
    maddr pcol = GH(0x102e04), px = GH(0x102dfc), prow = GH(0x102e06), palive = GH(0x102e14);
    for (;;) {
        if (R16(palive) < 0) {
            int32_t d = (int32_t)R16(prow) - (int32_t)KD(F_ROW);
            if (d < 0) d = -d;
            if (d <= 1) {
                int16_t c = R16(g + 6), kc = KD(F_COL);
                if ((c < kc && R16(px) < KD(F_X)) || (c > kc && R16(px) > KD(F_X))) {
                    int16_t w = wall;
                    int take;
                    if (w > R16(g + 6) && w > KD(F_COL)) take = 1;
                    else take = (w < R16(g + 6) && w < KD(F_COL));
                    if (take) {
                        near++;
                        if (near >= RU16(g + 2)) {
                            ok = 0;
                        } else {
                            c = R16(g + 6); kc = KD(F_COL);
                            if ((c < kc && c >= R16(pcol)) || (c > kc && c <= R16(pcol))) ok = 0;
                        }
                    }
                }
            }
        }
        i++;
        pcol += REC_SIZE; px += REC_SIZE; prow += REC_SIZE; palive += REC_SIZE;
        if (i >= n || ok == 0) break;
    }
    return (uint16_t)ok;
}

/* 0002e608: generation point idx of a room (0 if none) */
uint32_t GetOppGenPtIdx(int16_t room, int16_t idx) {
    if (room == 0) return 0;
    if (idx >= R16(genpt_room((uint16_t)room))) return 0;
    return GS() + 0x39ca + (uint32_t)(uint16_t)room * 0x44 - 0x3c + (uint32_t)((int32_t)idx * 0x14);
}

/* 0002e6ae: does a room have a generation point whose opponents tumble in */
uint32_t IsOppGenPtWithTumbleOn(int16_t room) {
    ENTRY_LINK(FN_IsOppGenPtWithTumbleOn, 0, 16);   /* D4-D6/A2 */
    int16_t r = 0;
    int16_t n = R16(genpt_room(RU16(gChar + F_ROOM)));
    int16_t i = 0;
    if (n > 0) {
        do {
            maddr g = GetOppGenPtIdx(room, i);
            r = (g != 0 && (R16(g + 0x12) & 0x80) != 0) ? 1 : 0;
            i++;
        } while (i < n && r == 0);
    }
    return (uint16_t)r;
}

/* 0002e736: find or allocate the palette bank (0x20/0x30) of an opponent kind. The
 * original replaces a 0 argument by 1 in its own argument slot. */
static uint32_t PresetOppBanks_(int16_t *pkind) {
    ENTRY_LINK(FN_PresetOppBanks, 0, 12);   /* D3/D4/A2 */
    int16_t k = OPP_KIND();
    maddr bank = gChar + F_BANK;
    if (k == 0 || k == 5 || k == 6) {
        W16(bank, 0);
        if (*pkind == 0) *pkind = 1;
        uint16_t i = 0;
        if (R16(bank) == 0) {
            int16_t b = 0x20;
            do {
                if (R16(gOppBanks + (int16_t)(i * 2)) == *pkind) W16(bank, b);
                b += 0x10;
                i++;
            } while (i < 2 && R16(bank) == 0);
        }
        if (R16(bank) == 0) {
            i = 0;
            if (R16(bank) == 0) {
                int16_t b = 0x20;
                do {
                    maddr a = gOppBanks + (int16_t)(i * 2);
                    if (R16(a) == 0) {
                        W16(a, *pkind);
                        W16(bank, b);
                    }
                    b += 0x10;
                    i++;
                } while (i < 2 && R16(bank) == 0);
            }
        }
        if (R16(bank) == 0) W16(bank, 0x20);
    } else if (OPP_KIND() == 2) {
        W16(bank, 0x30);
    } else {
        W16(bank, 0x20);
    }
    return RU16(bank);
}
uint32_t PresetOppBanks(int16_t kind) {
    return PresetOppBanks_(&kind);
}
void glue_PresetOppBanks(void) {
    maddr sp = emu_sp();
    int16_t kind = (int16_t)rd16(sp);
    uint32_t r = PresetOppBanks_(&kind);
    wr16(sp, (uint16_t)kind);
    emu_set_d0(r);
}

/* 0002e81c: install the palettes of the opponent banks */
uint32_t FUN_0002e81c(void) {
    ENTRY_NOLINK(FN_FUN_0002e81c, 12);   /* D3/D4/A2 */
    int16_t k = OPP_KIND();
    if (k == 0 || k == 5 || k == 6) {
        uint16_t i = 0;
        int16_t b = 0x20;
        for (;;) {
            maddr a = gOppBanks + (int16_t)(i * 2);
            if (R16(a) != 0) PoPSetPalette((int16_t)(R16(a) + 0x2ed), b);
            b += 0x10;
            i++;
            if (i >= 2) break;
        }
    } else if (OPP_KIND() == 2) {
        PoPSetPalette(0x2ee, 0x30);
    }
    return 0;
}

/* 0002e88c: the kid left the room: save the room's characters back into their info
 * records */
uint32_t CutOpponent(void) {
    ENTRY_LINK(FN_CutOpponent, 4, 12);   /* D3-D5 */
    int16_t n = cnt_u(RU16(gCurRoom));
    int16_t i = 0;
    if (n <= 0) return 0;
    do {
        LoadShad(i);
        if (CH(F_FACE) != 0x56 && CH(F_Y) >= 0x2da &&
            !(LEVEL_TYPE() == 5 && CH(F_ID) == R16(gRoofOppId))) {
            int16_t id = CH(F_ID);
            if (R16(gOppMeter) == id) {
                DrawOppMeter(id, 0);
                W16(gOppMeter, -1);
            }
            int16_t act = CH(F_ACTION);
            uint16_t fr;
            if (act == 4 || act == 3 || ((fr = RU16(gChar + F_FRAME)) >= 0xca && fr <= 0xd4)) {
                if (R16(gRoomBelow) == 0) {
                    UpdateOppInfo();
                    if (CH(F_TYPE) == 2 && S16(PrinceSndOn(0x1e)) == 0) AddSound(0x1e);
                }
            } else if (S16(UpdateOppInfo()) == 0) {
                seg4_022516();
            } else if (R16(gOppMeter) > CH(F_ID)) {
                W16(gOppMeter, R16(gOppMeter) - 1);
            } else if (R16(gOppMeter) == CH(F_ID)) {
                W16(gOppMeter, S16(GetOppIdxClosestToKid(CH(F_FACE))));
            }
            FUN_00028d38();
        }
        i++;
    } while (i < n);
    return 0;
}

/* 0002e9c2: per-frame room cut of the kid (leaving the room through an edge) */
uint32_t FUN_0002e9c2(void) {
    ENTRY_NOLINK(FN_FUN_0002e9c2, 0);
    if (R16(gCutDelay) != 0) {
        W16(gCutDelay, R16(gCutDelay) - 1);
        return 0;
    }
    FUN_000225d2();
    int16_t dir = S16(CutChar());
    W16(gCutDir, dir);
    if (dir != -1 && CH(F_ROOM) != 0) {
        copy_char_to_kid();
        W16(gNextRoom, CH(F_ROOM));
        CutAllOpponents();
        return 0;
    }
    if (CH(F_ROOM) != 0 || CH(F_ALIVE) >= 0) {
        copy_char_to_kid();
        return 0;
    }
    if (LEVEL_TYPE() == 5 && R16(gCutDir) == 0) {
        SET_CH(F_X, S16(AddCharX(0x1fe)));
        FUN_0006e806();
    } else {
        AddKidScream(R16(gCurRoom));
        if (LEVEL_TYPE() != 5) {
            DecLifePts(100);
            SET_CH(F_FRAME, 0xb9);
            SetKidDeathMethod(3);
            copy_char_to_kid();
            ChangeMeters();
            FUN_000225d2();
        }
    }
    copy_char_to_kid();
    return 0;
}

/* 0002eab6: after the kid's cut, decide for each character of the old room whether
 * it follows the kid (TransferOppInfo) or stays (UpdateOppInfo) */
uint32_t CutAllOpponents(void) {
    ENTRY_LINK(FN_CutAllOpponents, 4, 20);   /* D3-D7 */
    int16_t n = cnt_u(RU16(gCurRoom));
    int16_t i = 0;
    if (n <= 0) return 0;
    do {
        LoadShad(i);
        if (CH(F_FACE) != 0x56) {
            int16_t r;
            uint16_t fr = RU16(gChar + F_FRAME);
            int16_t ty;
            if (((fr >= 0xca && fr <= 0xd4 && CH(F_SWORD) == 1) ||
                 (fr >= 0xba && fr <= 0xc9 && CH(F_SWORD) != 0)) &&
                ((ty = CH(F_TYPE)) == 2 || ty == 4)) {
                r = 0;
            } else if (S16(IsCharInKidRoom()) != 0) {
                r = 0;
            } else if ((CH(F_ROW) != 0 && LEVEL_NUM() == 5 &&
                        ((ty = CH(F_ROOM)) == 10 || ty == 7 || ty == 0xc)) ||
                       CH(F_TYPE) == 0) {
                r = 0;
            } else {
                int16_t rm, lt, nr;
                if (CH(F_TYPE) == 6 && ((rm = CH(F_ROOM)) == 7 || rm == 8) &&
                    (lt = LEVEL_TYPE()) == 6 && ((nr = R16(gNextRoom)) == 7 || nr == 8) && lt == 6) {
                    r = 0;
                } else if (CH(F_ALIVE) >= 0 || S16(FUN_0002ec7c()) == 0) {
                    r = 1;
                } else if (R16(gCutDir) == 0) {
                    r = CH(F_X) >= 0x14b;
                } else if (R16(gCutDir) == 1) {
                    r = CH(F_X) < 0x250;
                } else if (R16(gCutDir) == 2) {
                    r = CH(F_ROW) >= 0;
                } else {
                    r = CH(F_ROW) < 3;
                }
            }
            if (r != 0) {
                r = S16(UpdateOppInfo());
            } else {
                TransferOppInfo();
                r = 1;
            }
            if (r != 0) {
                i--;
                n--;
            }
        }
        i++;
    } while (i < n);
    return 0;
}

/* 0002ec7c: may the current character follow the kid into his room */
uint32_t FUN_0002ec7c(void) {
    int16_t n = cnt_u(RU16(gKid + F_ROOM));
    int16_t r = n < 5;
    if (r) {
        int16_t ty;
        r = (CH(F_1A) == 1 || (ty = CH(F_TYPE)) == 7 || ty == 8);
        if (r) r = !(CH(F_TYPE) == 10 && OP(F_ROW) != CH(F_ROW));
    }
    return (uint16_t)r;
}

/* 0002ed0a: is the room the current character's position falls in the kid's room
 * (the character record is restored afterwards) */
uint32_t IsCharInKidRoom(void) {
    ENTRY_LINK(FN_IsCharInKidRoom, 0x40, 8);   /* D3/D4 */
    maddr save = a6 - 0x40;
    mcopy(save, gChar, REC_SIZE);
    int16_t room = S16(FUN_0002ed6a());
    int16_t r = (room == KD(F_ROOM));
    mcopy(gChar, save, REC_SIZE);
    return (uint16_t)r;
}

/* 0002ed6a: follow the room links until the current character's column/row lie inside
 * the room (adjusting its coordinates); returns that room, 0 if it left the level */
uint32_t FUN_0002ed6a(void) {
    uint16_t room = RU16(gChar + F_ROOM);
    int16_t c = CH(F_COL), r;
    uint16_t prev = 0;
    if (c >= 0 && c < 10 && (r = CH(F_ROW)) >= 0 && r < 3) return room;
    for (;;) {
        prev = room;
        if (CH(F_ROW) < 0) {
            room = RU16(link_addr(room) + 4);
            if (room != 0) {
                SET_CH(F_Y, CH(F_Y) + 0x16d);
                SET_CH(F_ROW, CH(F_ROW) + 3);
            } else {
                room = prev;
                prev = 0;
            }
        } else if (CH(F_ROW) >= 3) {
            room = RU16(link_addr(room) + 6);
            if (room != 0) {
                SET_CH(F_Y, CH(F_Y) - 0x16d);
                SET_CH(F_ROW, CH(F_ROW) - 3);
            } else {
                room = prev;
                prev = 0;
            }
        }
        if (CH(F_COL) < 0) {
            room = RU16(link_addr(room));
            if (room != 0) {
                SET_CH(F_X, CH(F_X) + 0x1fe);
                SET_CH(F_COL, CH(F_COL) + 10);
            }
        } else if (CH(F_COL) >= 10) {
            room = RU16(link_addr(room) + 2);
            if (room != 0) {
                SET_CH(F_X, CH(F_X) - 0x1fe);
                SET_CH(F_COL, CH(F_COL) - 10);
            }
        } else if (prev == 0) {
            room = 0;
        }
        if (room == 0) return 0;
        c = CH(F_COL);
        if (c < 0 || c >= 10) continue;
        r = CH(F_ROW);
        if (r < 0 || r >= 3) continue;
        return room;
    }
}

/* 0002eea4: move the current character into the neighbour room in direction dir
 * (0 left, 1 right, 2 up, 3 down); returns the opposite direction */
uint32_t Cut(int16_t dir) {
    ENTRY_LINK(FN_Cut, 0, 4);   /* D3 */
    int16_t r;
    SET_CH(F_ROOM, R16(link_addr(RU16(gChar + F_ROOM)) + (uint32_t)((int32_t)dir * 2)));
    if (dir == 0) {
        SET_CH(F_X, CH(F_X) + 0x1fe);
        SET_CH(F_COL, S16(GetCharCol(CH(F_X))));
        r = 1;
    } else if (dir == 1) {
        SET_CH(F_X, CH(F_X) - 0x1fe);
        SET_CH(F_COL, S16(GetCharCol(CH(F_X))));
        r = 0;
    } else if (dir == 2) {
        SET_CH(F_Y, CH(F_Y) + 0x168);
        SET_CH(F_ROW, S16(GetRow(CH(F_Y))));
        r = 3;
    } else {
        SET_CH(F_Y, CH(F_Y) - 0x168);
        SET_CH(F_ROW, S16(GetRow(CH(F_Y))));
        r = 2;
    }
    return (uint16_t)r;
}

/* 0002ef58: does the kid leave the room this frame; returns the direction (0 left,
 * 1 right, 2 up, 3 down) or -1, after moving him with Cut */
uint32_t CutChar(void) {
    ENTRY_LINK(FN_CutChar, 0xc, 20);   /* D3-D7 */
    int16_t d5 = -1;
    int16_t w = R16(gCharBoxW);
    int16_t left = R16(gCharBoxL);
    int16_t right = R16(gCharBoxR);
    int16_t room = R16(gCurRoom);
    if ((room == 7 || room == 8) && LEVEL_TYPE() == 6 &&
        (R16(GH(0x102e20)) == 0xe6 || R16(GH(0x102e5e)) == 0xe6)) {
        d5 = S16(FUN_000380da());
        goto sw;
    }
    if (R16(gNextLevel) != R16(gLevel)) goto sw;
    if (CH(F_JSEQ) == 0x46) goto sw;
    if (CH(F_TYPE) == 1 && CH(F_FRAME) == 0xb9) goto sw;
    {
        int16_t act = CH(F_ACTION);
        if (act != 5 && act != 4 && act != 3 && CH(F_Y) < 0x13) {
            if (LEVEL_NUM() == 5 && CH(F_ROOM) == 3) goto sw;
            d5 = 2;
            goto sw;
        }
    }
    {
        int16_t y = CH(F_Y);
        if (y >= 0x1b8) {
            int down = 0;
            if (R16(link_addr(RU16(gChar + F_ROOM)) + 6) != 0) down = 1;
            else if (!((int32_t)R16(gCharBoxH) + 0x1b8 > (int32_t)y) && R16(gNoCutFlag) == 0) down = 1;
            if (down && !(LEVEL_TYPE() == 5 && CH(F_ID) == R16(gRoofOppId))) {
                d5 = 3;
                goto sw;
            }
        }
    }
    {
        uint16_t fr = RU16(gChar + F_FRAME);
        if ((fr >= 0x87 && fr < 0x96) || (fr >= 0x6e && fr <= 0x77) || (fr >= 0x96 && fr < 0xa3) ||
            (fr >= 0xa6 && fr < 0xa9) || CH(F_ACTION) == 7) {
            d5 = -1;
            goto sw;
        }
    }
    if (CH(F_FACE) == -1) {
        int16_t l = R16(gCharBoxL);
        if (l <= 0xbf) {
            if (R16(link_addr(RU16(gChar + F_ROOM))) != 0 ||
                (!(0xbf - (int32_t)R16(gCharBoxW) < (int32_t)l) && R16(gNoCutFlag) == 0)) {
                if (LEVEL_NUM() == 9 && CH(F_ROOM) == 8 && CH(F_ROW) == 0 && CH(F_FRAME) == 0x29) {
                    W16(gNextLevel, R16(gNextLevel) + 1);
                } else if (S16(NoWallInDirOfChar(0)) != 0) {
                    d5 = 0;
                }
                goto sw;
            }
        }
        if (R16(gCharBoxL) < 0x2cd) goto sw;
        if (R16(link_addr(RU16(gChar + F_ROOM)) + 2) == 0 || S16(NoWallInDirOfChar(1)) == 0) {
            if ((int32_t)w + 0x2cc > (int32_t)left) goto sw;
            if (R16(gNoCutFlag) != 0) goto sw;
        }
        if (LEVEL_TYPE() == 1 && CH(F_ROOM) == 3) goto sw;
        d5 = 1;
        goto sw;
    }
    ReadCell(CH(F_ROOM), 9, CH(F_ROW));
    if (right >= 0x2dc && S16(NoWallInDirOfChar(1)) != 0) {
        int16_t rm = CH(F_ROOM);
        if (R16(link_addr((uint16_t)rm) + 2) != 0 ||
            (!((int32_t)w + 0x2dc > (int32_t)right) && R16(gNoCutFlag) == 0)) {
            if (!(LEVEL_TYPE() == 1 && rm == 3)) {
                d5 = 1;
                goto sw;
            }
        }
    }
    if (right > 0xce) goto sw;
    if (S16(NoWallInDirOfChar(0)) == 0) goto sw;
    if (R16(link_addr(RU16(gChar + F_ROOM))) == 0) {
        if (0xcf - (int32_t)w < (int32_t)right) goto sw;
        if (R16(gNoCutFlag) != 0) goto sw;
    }
    d5 = 0;
sw:
    switch (d5) {
    case 1:
        if (LEVEL_TYPE() == 5 && CH(F_ROOM) == 0xf) {
            d5 = -1;
            if (CH(F_JSEQ) != 0x47) seg23_06e69e();
        } else if (CH(F_ROOM) == 3 && LEVEL_TYPE() == 6) {
            d5 = -1;
        }
        break;
    case 0:
        if (CH(F_ROOM) == 1 && LEVEL_NUM() == 2) d5 = -1;
        break;
    case 3: {
        int16_t rm = CH(F_ROOM);
        int yes;
        int16_t ln;
        if (rm == 0xb && LEVEL_TYPE() == 5) yes = 1;
        else if (rm == 3 && LEVEL_TYPE() == 6) yes = 1;
        else if (rm == 5 && LEVEL_TYPE() == 6) yes = 1;
        else {
            ln = LEVEL_NUM();
            if (ln == 5 && rm == 10) yes = 1;
            else if (rm == 4 && ln == 0xd) yes = 1;
            else if (ln == 0xd && rm == 0xd) yes = 1;
            else if ((rm == 0x10 && ln == 9) || (rm == 0x1b && ln == 6)) {
                int16_t j = CH(F_JSEQ);
                yes = (j == 0x44 || j == 0x19 || j == 0xa);
            } else yes = 0;
        }
        if (yes) {
            d5 = -1;
            ln = LEVEL_NUM();
            if ((ln == 5 && rm == 10) || (rm == 4 && ln == 0xd) || (ln == 0xd && rm == 0xd)) {
                DecLifePts(100);
                SET_CH(F_ROOM, 0);
            }
        }
        break;
    }
    default:
        break;
    }
    if (d5 != -1) Cut(d5);
    if (d5 == 0 && R16(gCurRoom) == 0x1b && LEVEL_NUM() == 6) seg7_033cee();
    return (uint16_t)d5;
}

/* 0002f3ec: no wall in the first cell of the neighbour room in direction dir
 * (0 left, 1 right) on the current character's row */
uint32_t NoWallInDirOfChar(int16_t dir) {
    ENTRY_LINK(FN_NoWallInDirOfChar, 0, 20);   /* D3-D7 */
    int16_t r = 1;
    int16_t row = (CH(F_FRAME) == 0x66) ? (int16_t)(CH(F_ROW) - 1) : CH(F_ROW);
    if (row < 0 || row >= 3) return 1;
    int16_t nb = R16(link_addr(RU16(gCurRoom)) + (uint32_t)((int32_t)dir * 2));
    if (nb == 0) return 1;
    int16_t cell;
    if (dir == 0) cell = (int16_t)(S16(FirstInRow(row)) + 9);
    else cell = S16(FirstInRow(row));
    int16_t id = R16(GS() + (uint32_t)((int32_t)nb * 0x3c) - 0x3c + (uint32_t)((int32_t)cell * 2));
    int wall = (S16(IsWall(id)) != 0 && id != 7);
    r = wall ? 0 : 1;
    return (uint16_t)r;
}

/* 0002f4d0: automatic control of the current (non-kid) character */
uint32_t AutoCtrl(void) {
    ENTRY_LINK(FN_AutoCtrl, 4, 0);
    FUN_0002f55e();
    if (R16(gAITimer1) != 0) W16(gAITimer1, R16(gAITimer1) - 1);
    if (R16(gAITimer2) != 0) W16(gAITimer2, R16(gAITimer2) - 1);
    if (R16(gAITimer3) != 0) W16(gAITimer3, R16(gAITimer3) - 1);
    switch (CH(F_TYPE)) {
    case 1:  FUN_0002f584(); break;
    case 4:  case 10: seg4_01f12c(); break;
    case 6:  FUN_00038498(); break;
    case 7:  case 8: seg7_0318d4(); break;
    case 11: FUN_00031dd4(); break;
    default:
        if (CH(F_TYPE) != 0) seg6_02f5a0();
        break;
    }
    return 0;
}

/* 0002f55e: clear the control inputs */
uint32_t FUN_0002f55e(void) {
    ENTRY_NOLINK(FN_FUN_0002f55e, 0);
    tc_memset(GH(0x104b78), 0, 10);
    tc_memset(GH(0x104b68), 0, 6);
    return 0;
}

/* 0002f584 */
uint32_t FUN_0002f584(void) {
    ENTRY_NOLINK(FN_FUN_0002f584, 0);
    if (CH(F_ROOM) == 4 && LEVEL_NUM() == 0xd) seg10_03ccf0();
    return 0;
}

/* 0002f5a0: control of an ordinary guard */
uint32_t seg6_02f5a0(void) {
    ENTRY_NOLINK(FN_seg6_02f5a0, 4);   /* D3 */
    if (CH(F_ALIVE) >= 0) return 0;
    if (CH(F_LIFE) == 0) return 0;
    if ((int32_t)CH(F_LIFE) == -(int32_t)CH(F_PEND)) return 0;
    if (CH(F_1A) == 0) {
        uint16_t fr = RU16(gChar + F_FRAME);
        if (fr >= 0xba && fr <= 0xd4) seg4_01e04c();
        else FUN_0001dfcc();
        if (R16(gGenDone) != 0) {
            int32_t id1 = (int32_t)CH(F_ID) + 1;
            int32_t n = (int32_t)cnt_u(RU16(gChar + F_ROOM));
            if (id1 == n) W16(gGenDone, 0);
        }
    } else if (KD(F_34) != 0xd) {
        EnGarde();
    } else if (R8(GS() + 0x2196) != 0 && CH(F_JSEQ) != 0x6e) {
        seg4_01e2ce();
    }
    return 0;
}

/* 0002f662: music tension level/amount */
uint32_t SetTensionLevel(int16_t level, int16_t amount) {
    int16_t cur = R16(gTensionLvl);
    if (cur != level) {
        if ((cur == 3 && level == 2) || (cur == 2 && level == 3)) W16(gTension, R16(gTension) - 10);
        else W16(gTension, 0);
        W16(gTensionLvl, level);
    }
    W16(gTension, R16(gTension) + amount);
    if (R16(gTension) < 0) W16(gTension, 0);
    return 0;
}

static void FUN_0002f832_(int16_t d5);

/* 0002f6d2: handle the characters that were stabbed this frame (action 10), then the
 * kid when an opponent hit him */
uint32_t CheckStab(void) {
    ENTRY_LINK(FN_CheckStab, 4, 20);   /* D3-D5/A2/A3 */
    int16_t n = cnt_u(RU16(gCurRoom));
    if (n != 0) {
        FUN_00020d42();
        int16_t i = 0;
        if (n > 0) {
            maddr pact = GH(0x102e08);
            do {
                if (R16(pact) == 10) {
                    LoadShad(i);
                    if (OP(F_ACTION) == 10) SET_OP(F_ACTION, 1);
                    FUN_0002f832_(n);
                    uint16_t v = RU16(GS() + (uint32_t)((int32_t)CH(F_ID) * INFO_SIZE) +
                                      (uint32_t)RU16(gChar + F_ROOM) * 0xc0 + 0x20ee);
                    W16(gAITimer3, R16(gStabTab + (int16_t)(v * 2)));
                    seg4_022516();
                }
                i++;
                pact += REC_SIZE;
            } while (i < n);
        }
        mcopy(gKid, gOpp, REC_SIZE);
    }
    if (KD(F_ACTION) == 10) {
        int16_t i = 0;
        if (n > 0) {
            maddr pfr = GH(0x102e02), pty = GH(0x102e00);
            do {
                int16_t ty = R16(pty);
                if (ty != 7 && ty != 8) {
                    if (S16(IsTheStrikePos(ty, R16(pfr))) != 0) break;
                } else {
                    int16_t fr = R16(pfr);
                    if (fr == 0xa7 || fr == 0xb5) break;
                }
                i++;
                pfr += REC_SIZE;
                pty += REC_SIZE;
            } while (i < n);
        }
        if (i < n) {
            LoadKidwOpp(i);
            FUN_0002f832_(n);
            FUN_00020da0();
        }
    }
    return 0;
}

/* 0002f832: the current character was stabbed: lose life, die or get pushed back.
 * D5 is read before it is set when the opponent is a snake (type 11) and the character
 * is not; d5 is the caller's D5 (CheckStab's character count). */
static void FUN_0002f832_(int16_t d5) {
    ENTRY_NOLINK(FN_FUN_0002f832, 16);   /* D3-D6 */
    int16_t d6, d4, d3;
    if (CH(F_ALIVE) >= 0) return;
    if (CH(F_TYPE) == 0 && CH(F_JSEQ) == 5) {
        int16_t lt = LEVEL_TYPE();
        if (lt == 2 || lt == 6) {
            SetKidPalette(LEVEL_TYPE());
            SET_CH(F_BANK, 0);
        }
    }
    {
        int16_t j;
        if (CH(F_1A) == -1 || (j = CH(F_JSEQ)) == 0xa || j == 0x10 || j == 0x1c || j == 0xe) {
            DecLifePts(100);
            seg6_02fba2();
            goto l_2fab8;
        }
    }
    d3 = CH(F_TYPE);
    if (d3 == 7 || d3 == 8) {
        d6 = (R16(gDifficulty) == 2) ? 1 : 2;
        d5 = 0x99;
        if (CH(F_LIFE) > d6) seg7_031a04();
        goto l_2fa4a;
    }
    if (CH(F_TYPE) == 0xb || OP(F_TYPE) == 0xb) {
        d6 = 100;
        goto l_2fa4a;
    }
    d6 = (CH(F_TYPE) == 6 || OP(F_TYPE) == 6) ? 100 : 1;
    if (CH(F_FACE) != OP(F_FACE)) {
        d5 = (CH(F_TYPE) == 0) ? 0x4a : 0xb7;
        if (CH(F_TYPE) == 2) SET_CH(F_X, S16(AddCharX(-10)));
    } else {
        d5 = 0x5e;
        if (CH(F_1A) == 0) {
            d3 = S16(GetCellsInFront(1));
            if (S16(IsWall(d3)) != 0) {
                d4 = S16(seg4_021562());
                if (d4 < 0x24) SET_CH(F_X, S16(AddCharX((int16_t)(d4 - 0x24))));
            }
        }
    }
    d3 = OP(F_TYPE);
    if (d3 == 7 || d3 == 8) {
        int far = 0;
        if (CH(F_1A) != 1) {
            uint16_t fr = RU16(gChar + F_FRAME);
            maddr def = (uint32_t)fr * 10 + R32(R32(gFrameTabH));
            if ((R16(def + 8) & 0x40) != 0 || (fr >= 0xbf && fr <= 0xcd)) {
                if (fr >= 0x66 && fr <= 0x6a) far = 1;
            } else {
                far = 1;
            }
        }
        if (far) {
            d5 = 0x2d;
        } else {
            uint16_t fr = RU16(gChar + F_FRAME);
            if (fr >= 0xf6 && fr <= 0x105) {
                if (S16(IsWall(S16(FUN_00021518()))) != 0) SET_CH(F_X, S16(AddCharX(0x19)));
                else if (S16(IsWall(S16(GetCellsInFront(1)))) != 0) SET_CH(F_X, S16(AddCharX(-0x19)));
            }
        }
    }
    if (CH(F_TYPE) == 0 && d5 != 0x2d) SET_CH(F_1A, 1);
l_2fa4a:
    if (S16(DecLifePts(d6)) == 0 && CH(F_TYPE) != 0xb) {
        JumpSeq(d5);
        goto l_2fab8;
    }
    seg4_02055e();
    d4 = S16(seg4_021562());
    if (S16(IsSpace(S16(FUN_00021076()))) != 0 ||
        (S16(IsSpace(S16(FUN_00021518()))) != 0 && d4 >= 6)) {
        d3 = CH(F_TYPE);
        if (d3 != 7 && d3 != 8 && d3 != 0xb && d3 != 6) {
            d4 = (int16_t)(d4 - 0xc);
            if (CH(F_COL) != R16(gCellCol)) d4 = (int16_t)(d4 - 0x33);
            SET_CH(F_X, S16(AddCharX(d4)));
            FUN_00020542();
            SET_CH(F_ROW, CH(F_ROW) + 1);
            JumpSeq(0x51);
            goto l_2faf4;
        }
    }
    seg6_02fba2();
l_2fab8:
    FUN_00020cca();
    SET_CH(F_YMOM, 0);
l_2faf4:
    d3 = OP(F_TYPE);
    if (d3 != 7 && d3 != 8 && d3 != 0xb) {
        if (CH(F_TYPE) == 0) d3 = (d3 == 10) ? 0xf0 : 0xc;
        else if (CH(F_TYPE) == 4) d3 = 0x3b;
        else if ((d3 = CH(F_TYPE)) == 7 || d3 == 8) d3 = 0x82;
        else if (CH(F_TYPE) == 0xb) d3 = 0x7f;
        else d3 = 0x1f;
        AddSound(d3);
    }
    AnimChar();
    if (CH(F_JSEQ) == 0x55) {
        FUN_00020542();
        d3 = S16(FUN_00021076());
        if (S16(IsSpace(d3)) != 0 || S16(IsMobFloor(d3)) != 0) {
            SET_CH(F_X, S16(AddCharX(-0x24)));
            FUN_00020542();
        }
        FUN_0002aaea();
    }
}
uint32_t FUN_0002f832(void) {
    FUN_0002f832_((int16_t)cpu_get(R_D5));
    return 0;
}

/* 0002fba2: the current character dies (sword hit): death sequence, sounds */
uint32_t seg6_02fba2(void) {
    ENTRY_NOLINK(FN_seg6_02fba2, 8);   /* D3/D4 */
    int16_t d3, d4;
    SET_CH(F_FIGHT, 0);
    SET_CH(F_18, 1);
    d4 = -1;
    if (CH(F_TYPE) == 4) {
        seg4_01f1de();
        goto l_2fdbe;
    }
    d3 = CH(F_TYPE);
    if (d3 == 7 || d3 == 8) {
        SET_CH(F_FACE, (int16_t)~KD(F_FACE));
        SET_CH(F_LIFE, 0);
        d4 = 0x9a;
        SetTensionLevel(1, -1);
        goto l_2fdbe;
    }
    if (CH(F_TYPE) == 0xb) { d4 = 0xae; goto l_2fdbe; }
    if (CH(F_TYPE) == 6) { d4 = 0xc4; goto l_2fdbe; }
    d4 = S16(FUN_00021518());
    if (CH(F_TYPE) != 0 && CH(F_FACE) == -1) {
        d3 = 4;
        if (d4 == d3 || S16(FUN_00021076()) == d3) {
            d3 = S16(FUN_00021600());
            int32_t d1 = (int32_t)d3 - (R16(gCellId) == 4 ? 0 : 1);
            SET_CH(F_X, R16(gColX3 + (int16_t)(d1 * 2)) + 0x16);
            SET_CH(F_X, S16(AddCharX(0x1f)));
        }
    }
    {
        uint16_t fr = RU16(gChar + F_FRAME);
        if (CH(F_TYPE) == 0 && fr >= 0xf6 && fr <= 0x105) {
            d4 = 0x62;
            if (OP(F_TYPE) == 0xb) {
                AddSound(0x7e);
            } else if ((d3 = OP(F_TYPE)) == 7 || d3 == 8) {
                AddSound(0x85);
            } else {
                AddSound(0xc);
            }
        } else if (OP(F_TYPE) == 0xb) {
            d4 = 0x61;
            AddSound(0x7e);
        } else {
            if (OP(F_TYPE) == 6) AllOtherJaffarsShimmerAway();
            d4 = 0x55;
            if (CH(F_TYPE) != 0 && S16(DoOppTumbleSeq()) != 0) {
                d4 = 0xb9;
                SET_CH(F_X, S16(AddCharX(-0xc)));
            } else if (CH(F_TYPE) == 2) {
                SET_CH(F_X, S16(AddCharX(-0x11)));
            }
        }
    }
    if (CH(F_TYPE) == 0) {
        switch (OP(F_TYPE)) {
        case 2:  d3 = (OPP_KIND() == 0) ? 0xe : 0xa; break;
        case 4:
            if (LEVEL_NUM() == 5 && R16(gCurRoom) == 10) SetTensionLevel(2, 5);
            d3 = 2;
            break;
        case 6:  d3 = 0x11; break;
        case 7:  case 8: d3 = 6; SetTensionLevel(1, 5); break;
        case 10: d3 = 0xc; break;
        case 11: d3 = 7; break;
        default: d3 = 2; break;
        }
        SetKidDeathMethod(d3);
    }
l_2fdbe:
    if (OP(F_TYPE) == 0 || OP(F_TYPE) == 1) {
        FUN_00020e5e();
        W16(gOppTarget, S16(GetOppIdxClosestToKid(OP(F_FACE))));
    }
    if (d4 != -1) JumpSeq(d4);
    return 0;
}

/* 0002fdf2: strike tests of the frame: each character of the room against the kid,
 * then the kid against each character */
uint32_t seg6_02fdf2(void) {
    ENTRY_NOLINK(FN_seg6_02fdf2, 12);   /* D3/D5/D6 */
    uint16_t kf = RU16(gKid + F_FRAME);
    if (kf == 0) return 0;
    if (kf >= 0xdb && kf < 0xe5) return 0;
    int16_t n = cnt_u(RU16(gCurRoom));
    if (n == 0) {
        uint16_t f = RU16(gKid + F_FRAME);
        if (f == 0x9a || f == 0xf5) AddSound(0xb);
        return 0;
    }
    FUN_00020d42();
    int16_t i = 0;
    if (n > 0) {
        int32_t last = (int32_t)n - 1;
        do {
            LoadShad(i);
            TestStrike();
            if ((int32_t)i == last) FUN_00020e5e();
            else seg4_022516();
            i++;
        } while (i < n);
    }
    FUN_000225d2();
    i = 0;
    if (n > 0) {
        int32_t last = (int32_t)n - 1;
        do {
            LoadShadAsOpp(i);
            TestStrike();
            if ((int32_t)i == last) FUN_00020da0();
            else seg4_020e86();
            i++;
        } while (i < n);
    }
    return 0;
}

/* 0002fed6: does the current character's strike hit its opponent (or get blocked) */
uint32_t TestStrike(void) {
    ENTRY_LINK(FN_TestStrike, 4, 16);   /* D3-D6 */
    maddr plo = a6 - 4, phi = a6 - 2;
    int16_t d4, d5, d3 = 1;
    if (CH(F_1A) != 1 || CH(F_ROW) != OP(F_ROW)) goto l_30140;
    {
        int32_t dy = (int32_t)CH(F_Y) - (int32_t)OP(F_Y);
        if (dy < 0) dy = -dy;
        if (dy >= 0xf) {
            int16_t t = CH(F_TYPE);
            if (t != 7 && t != 8) {
                t = OP(F_TYPE);
                if (t != 7 && t != 8) goto l_30140;
            }
        }
    }
    if (S16(IsStrikingPos(CH(F_TYPE), CH(F_FRAME))) == 0) goto l_30140;
    if (OP(F_ALIVE) >= 0) goto l_30140;
    d4 = OP(F_TYPE);
    if (d4 == 10 || d4 == d3) goto l_30140;
    if (OP(F_ACTION) == 8) goto l_30140;
    if (OP(F_JSEQ) == 0x6e) goto l_30140;
    if (CH(F_TYPE) == d3 && d4 == 0) goto l_30140;
    d5 = S16(seg4_0224be());
    d3 = CH(F_TYPE);
    if (d3 == 7 || d3 == 8) { W16(plo, 0); W16(phi, 0x37); }
    else if (CH(F_TYPE) == 10) { W16(plo, 0x18); W16(phi, 0x46); }
    else { W16(plo, 0x3d); W16(phi, 0x63); }
    if (CH(F_FACE) != OP(F_FACE) && d5 >= R16(plo) && d5 <= R16(phi)) {
        d3 = OP(F_TYPE);
        if (d3 != 7 && d3 != 8 && d3 != 0xb && CH(F_TYPE) != 0xb) {
            d3 = OP(F_FRAME);
            if (d3 == 0xa1 || d3 == 0x96) goto l_3007c;
        }
    }
    if (S16(IsTheStrikePos(CH(F_TYPE), CH(F_FRAME))) != 0) {
        GetCharStrikeRanges(plo, phi);
        if (d5 >= R16(plo) && d5 <= R16(phi)) {
            SET_OP(F_ACTION, 10);
            if (CH(F_TYPE) == 0) SET_CH(F_3A, OP(F_ID));
        }
    } else {
        d3 = CH(F_TYPE);
        if ((d3 == 7 || d3 == 8) && CH(F_34) == 1 && R16(gAITimer1) == 0) SET_OP(F_ACTION, 10);
    }
    goto l_300f4;
l_3007c:
    SET_OP(F_FRAME, 0xa1);
    if (CH(F_TYPE) == 0) {
        SET_CH(F_3A, OP(F_ID));
    } else {
        W16(gOppTarget, CH(F_ID));
        W16(gAITimer1, 4);
    }
    d3 = CH(F_TYPE);
    if (d3 == 7 || d3 == 8) {
        d4 = S16(FUN_00030eaa());
    } else if (CH(F_TYPE) == 10) {
        d4 = S16(seg4_01f706());
    } else {
        d4 = 0x45;
        if (OPP_KIND() == 7) SET_CH(F_X, S16(AddCharX(0xd)));
    }
    if (d4 != -1) JumpSeq(d4);
    AnimChar();
l_300f4:
    if (S16(IsTheStrikePos(CH(F_TYPE), CH(F_FRAME))) != 0) {
        if (OP(F_FRAME) == 0xa1 && (d3 = OP(F_TYPE)) != 7 && d3 != 8) return 0;
        if (OP(F_ACTION) == 10) return 0;
        AddSound(CH(F_TYPE) == 10 ? 0xef : 0xb);
    }
    return 0;
l_30140:
    if (CH(F_TYPE) == 0 && S16(IsTheStrikePos(CH(F_TYPE), CH(F_FRAME))) != 0) AddSound(0xb);
    return 0;
}

/* 00030178: the range of distances (opponent x relative to the character) in which
 * the current character's strike can hit */
uint32_t GetCharStrikeRanges(uint32_t plo, uint32_t phi) {
    W16(plo, 0);
    W16(phi, -1);
    if (CH(F_TYPE) == 0 || CH(F_TYPE) == 1) {
        int16_t t;
        if (OP(F_TYPE) == 0xb) {
            if (OP(F_34) == 6 || OP(F_JSEQ) == 0xab || CH(F_JSEQ) != 0x60) goto l_30282;
            if (CH(F_FACE) == OP(F_FACE)) { W16(plo, 0x3e); W16(phi, 0x7e); }
            else { W16(plo, -0xb); W16(phi, 0x4f); }
            goto l_30282;
        }
        t = OP(F_TYPE);
        if (t == 7 || t == 8) { W16(plo, 0x20); W16(phi, 0x5c); goto l_30282; }
        if (OP(F_TYPE) == 6) {
            if (CH(F_FACE) != OP(F_FACE)) { W16(plo, 0x10); W16(phi, 0x56); }
            else { W16(plo, 0x10); W16(phi, 0x4f); }
            goto l_30282;
        }
        W16(plo, 0x3d);
        W16(phi, 0x63);
        if (OP(F_FIGHT) == 0) {
            W16(plo, R16(plo) + 0x16);
            W16(phi, R16(phi) + 0x16);
            if (OP(F_FACE) == CH(F_FACE)) {
                W16(plo, R16(plo) - 0x25);
                W16(phi, R16(phi) - 0x16);
            }
        }
        if (OP(F_TYPE) == 4) W16(phi, R16(phi) + 3);
    l_30282:
        if (R16(gDifficulty) == 2) W16(phi, R16(phi) - 10);
        return 0;
    }
    if (CH(F_TYPE) == 10) { W16(plo, -0x10); W16(phi, 0x3d); return 0; }
    if (CH(F_TYPE) == 0xb) {
        if ((int32_t)OP(F_ROW) * 0x78 + 0x60 > (int32_t)OP(F_Y)) return 0;
        if (CH(F_FACE) == OP(F_FACE)) { W16(plo, 8); W16(phi, 0x34); }
        else { W16(plo, -0x20); W16(phi, 6); }
        return 0;
    }
    if (CH(F_TYPE) == 6) { W16(plo, 0x10); W16(phi, 0x4f); return 0; }
    if (CH(F_FACE) != OP(F_FACE)) {
        W16(plo, 0x3d);
        W16(phi, 0x63);
        if (OP(F_1A) != 1) W16(plo, R16(plo) - 0x1a);
    } else if (OP(F_1A) == 1) {
        W16(plo, 0x2d); W16(phi, 0x55);
    } else {
        W16(plo, 0x2a); W16(phi, 0x53);
    }
    return 0;
}

/* 000303a0: is a frame of a character type a striking frame */
uint32_t IsStrikingPos(int16_t type, int16_t frame) {
    int16_t r;
    if (type == 7 || type == 8) {
        uint16_t f = (uint16_t)frame;
        r = (f >= 0xa5 && f <= 0xa7) || f == 0xb5;
    } else if (type == 10) {
        r = frame == 0xdf || frame == 0xe0;
    } else if (type == 0xb) {
        r = CH(F_JSEQ) == 0xad && (frame == 0x112 || frame == 0x118);
    } else if (type == 0xc) {
        r = 0;
    } else {
        r = frame == 0x99 || frame == 0x9a;
        if (!r && type == 0) r = frame == 0xf4 || frame == 0xf5;
    }
    return (uint16_t)r;
}

/* 00030482: is a frame of a character type the frame where the strike lands */
uint32_t IsTheStrikePos(int16_t type, int16_t frame) {
    int16_t r;
    if (type == 10) r = frame == 0xe0;
    else if (type == 0xb) r = frame == 0x118;
    else {
        r = frame == 0x9a;
        if (!r && type == 0) r = frame == 0xf5;
    }
    return (uint16_t)r;
}

/* 0003050a: can the current character advance onto a cell (id, spec) */
uint32_t IsAdvancableId(int16_t id, int16_t spec) {
    ENTRY_LINK(FN_IsAdvancableId, 0, 8);   /* D3/D4 */
    int16_t r;
    if (S16(IsWall(id)) != 0) r = 0;
    else if (id == 4 && (uint16_t)(spec & 0xff) < 0x68) r = 0;
    else r = 1;
    if (r == 0) return 0;
    if (CH(F_TYPE) == 10) return (uint16_t)r;
    r = (S16(IsImpassable(id)) != 0 && id != 0x17) ? 0 : 1;
    if (r == 0) return 0;
    int16_t t = CH(F_TYPE);
    if (t == 7 || t == 8) return (uint16_t)r;
    r = S16(IsSpace(id)) == 0 ? 1 : 0;
    return (uint16_t)r;
}

