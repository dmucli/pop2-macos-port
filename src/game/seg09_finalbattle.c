/* CODE 9 "Final Battle" (Ghidra 0x37bb4-0x3a4a0): level-type 6 code (the last level).
 * Jaffar and his shadow copies (generation, positions, moves along four fixed
 * positions), the flask-dropping spider, the crystal, the gargoyles and the
 * fireballs. Ported from the disassembly of the original 68k code (Ghidra addresses
 * in comments, see docs/PORTING.md). */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* ---- character records (0x3e bytes, layout of docs/notes/seg04a.md) ---- */
#define REC_SIZE    0x3e
#define gChar       GH(0x102faa)   /* the character being processed */
#define gOpp        GH(0x102f6c)   /* its opponent */
#define gKid        GH(0x102f2e)   /* saved kid record */
#define gOppTab     GH(0x102df8)   /* the room's characters, REC_SIZE each */
#define CH(f)       (gChar + (f))
#define OPP(f)      (gOpp + (f))
#define KID(f)      (gKid + (f))
#define F_ID      0x00
#define F_FACE    0x02             /* 0 = right, -1 = left */
#define F_X       0x04
#define F_Y       0x06
#define F_TYPE    0x08
#define F_FRAME   0x0a
#define F_COL     0x0c
#define F_ROW     0x0e
#define F_ACTION  0x10
#define F_XVEL    0x12
#define F_YVEL    0x14
#define F_ROOM    0x16
#define F_18      0x18
#define F_1A      0x1a
#define F_ALIVE   0x1c             /* < 0 alive, otherwise death counter */
#define F_LIFE    0x1e
#define F_MAXLIFE 0x20
#define F_PENDING 0x22
#define F_SEQ     0x28             /* last sequence started with JumpSeq */
#define F_32      0x32
#define F_34      0x34
#define F_36      0x36
#define F_38      0x38

/* ---- globals (Ghidra addresses) ---- */
#define gGameState    GH(0x102fec)  /* long: pointer to the game state block */
#define GS()          R32(gGameState)
#define LEVEL_TYPE()  R16(GS() + 0x2186)
#define LEVEL_NUM()   R16(GS() + 0x218a)
#define gSavedGame    GH(0x102fe8)  /* long: rebirth-point copy of the game state */
#define gCurRoom      GH(0x10303e)  /* word: room being drawn / played */
#define gNextRoom     GH(0x10303c)  /* word */
#define gLevel        GH(0x10302e)  /* word: level number */
#define gNextLevel    GH(0x10306e)  /* word */
#define gCurBFItem    GH(0x103074)  /* long: BF item being built */
#define gForeTable    GH(0x10335a)  /* long: foreground BF items (0x26 bytes each) */
#define gNumFore      GH(0x1037d4)  /* word */
#define gColX         GH(0x103478)  /* word[]: x of each column */
#define gFaceDx       GH(0x103468)  /* word[] indexed by face (0/-1) */
#define gNumTrobs     GH(0x103c1a)  /* word: animated objects */
#define gFightOppId   GH(0x102f68)  /* word: id of the character fighting the kid (-1 none) */
#define gOppKindType  GH(0x102d80)  /* word[]: char type per opponent kind (gs+0x21a4) */
#define gOppBank      GH(0x103078)  /* word: opponent shape bank (0 = not loaded) */
#define gFrameTab     GH(0x104b8e)  /* long: handle to the frame table (10 bytes per frame) */
#define gJafPos       GH(0x1055fa)  /* the four Jaffar positions, 0x16 bytes each:
                                       +0 column, +4 face, +6 min x, +8 max x */
#define gWorldPal     GH(0x102dbe)  /* long */
#define gCharRight    GH(0x104a9a)  /* word: right edge of the character image */
#define gFCharSeq     GH(0x104b48)  /* word: FChar image override */
#define gInBtn        GH(0x104b80)  /* word: edge-triggered button */
#define gAmbient      GH(0x1048c8)  /* word: ambient sound on */
#define gAmbientSnd   GH(0x103c68)  /* word */

static int16_t room_char_count(uint16_t room) {
    return room ? R16(GS() + (uint32_t)room * 0xc0 + 0x20e6) : 0;
}
/* flags word of frame f in the frame table */
static uint16_t frame_flags(uint16_t f) {
    return RU16(R32(R32(gFrameTab)) + (uint32_t)f * 10 + 8);
}
static void copy_rec(maddr dst, maddr src) { mcopy(dst, src, REC_SIZE); }

/* Callers of MarkBufsForRect push a third word (-1) that it never reads: push it too so
 * that its frame is where the original's is. */
static void mark_bufs_for_rect(uint32_t fn, uint32_t rect) {
    maddr sp = emu_sp() - 2;
    emu_set_sp(sp);
    W16(sp, -1);
    MarkBufsForRect(fn, rect);
    emu_set_sp(sp + 2);
}

/* ClipRect(r) through the pointer cached at A5-0x1f64 (Pascal: the callee pops r) */
static void clip_rect_ptr(maddr r) {
    maddr sp0 = emu_sp();
    emu_set_sp(sp0 - 4);
    W32(sp0 - 4, r);
    call68_addr(R32(GH(0x10609c)));
    emu_set_sp(sp0);
}

/* 00037bb4: level-type hook: draw the crystal (room 6) or the gargoyles (rooms 7/8) of
 * the cell being drawn. Returns the original's D0 word. */
uint32_t FUN_00037bb4(void) {
    ENTRY_NOLINK(FN_FUN_00037bb4, 4);            /* move.l D3 */
    if (R16(gCurRoom) == 6 && LEVEL_TYPE() == 6 && (RU16(R32(gCurBFItem) + 0x14) & 0x1000))
        DrawFinlCrystal();
    int16_t room = R16(gCurRoom);
    if (room != 7 && room != 8) return 8;
    uint16_t v = (uint16_t)LEVEL_TYPE();
    if (v != 6) return v;
    v = RU16(R32(gCurBFItem) + 0x14) & 0x1000;
    if (v == 0) return 0;
    return DrawFinlGargoyle();
}

/* 00037c12: put the character in room 11 (column 6, row 2) and start sequence 0x47 */
uint32_t seg9_037c12(void) {
    ENTRY_NOLINK(FN_seg9_037c12, 8);             /* movem D3-D4 */
    if (R16(CH(F_ROOM)) == 0xb) return 0;
    if (R32(gSavedGame) == 0) return 0;
    W16(gNextRoom, 0xb);
    W16(CH(F_ROOM), 0xb);
    W16(CH(F_COL), 6);
    W16(CH(F_X), RU16(gColX + 6 * 2));
    W16(CH(F_ROW), 2);
    W16(CH(F_Y), 2 * 0x78 + 0x70);
    JumpSeq(0x47);
    for (uint16_t i = 0; i < 5; i++) AnimChar();
    return 0;
}

/* 00037c76: palette hook */
uint32_t seg9_037c76(void) {
    ENTRY_NOLINK(FN_seg9_037c76, 0);
    PoPSetPalette(0xdaf, 0x40);
    ResetStandardColors(R32(gWorldPal));
    return 0;
}

/* 00037c90: empty code-overlay load hook */
uint32_t seg9_037c90(void) {
    return 0;
}

/* 00037c92: a dying Jaffar copy: on the 6th frame of death maybe drops a flask in
 * room 3, then counts frames; at 0x60 it shimmers away (sequence 0xef). D0 word is
 * the char's +0x18 word (re-read last). */
uint32_t FinlCheckDeadOpp(void) {
    ENTRY_LINK(FN_FinlCheckDeadOpp, 4, 20);      /* link -4; movem D3-D7 */
    if (R16(CH(F_ALIVE)) == 6) {
        int16_t cell = S16(ReadCell(R16(CH(F_ROOM)), R16(CH(F_COL)), R16(CH(F_ROW))));
        int16_t rnd = S16(Rnd(2));
        if (rnd != 0 && cell == 1 && R16(gNumTrobs) < 4) {
            int16_t c = (int16_t)(S16(FirstInRow(R16(CH(F_ROW)))) + R16(CH(F_COL)));
            if (c < 0x16 || c > 0x18) {
                W32(a6 - 4, (uint32_t)(int32_t)c);
                W16(GS() + 0x78 + (int32_t)c * 2, 10);
                W16(GS() + 0x870 + (int32_t)c * 4, (uint16_t)((rnd << 5) | 0x4000));
                CalcBlue(3);
                TriggerFlask(3, c);
                MarkRedraw(c);
            }
        }
        W16(CH(F_ALIVE), R16(CH(F_ALIVE)) + 1);
    } else if (R16(CH(F_ALIVE)) >= 0x60 && R16(CH(F_SEQ)) != 0xef) {
        JumpSeq(0xef);
        AddSound(0xf3);
        W16(CH(F_ALIVE), R16(CH(F_ALIVE)) + 1);
        if (R16(gFightOppId) == R16(CH(F_ID))) W16(gFightOppId, -1);
    }
    uint16_t v = RU16(CH(F_18));
    if (v == 0 && R16(CH(F_ALIVE)) > 6) W16(CH(F_ALIVE), R16(CH(F_ALIVE)) + 1);
    return v;
}

/* 00037db2: maybe generate a new Jaffar copy; n = number of characters in room 3 */
uint32_t FinlCheckGenOpp(int16_t n) {
    ENTRY_LINK(FN_FinlCheckGenOpp, 0, 12);       /* link 0; movem D3-D4/D6 */
    int16_t row = -1;
    if (n <= 1) {
        if (n == 1) {
            if (R16(gOppTab + F_SEQ) != 0xee) {
                row = R16(gOppTab + F_ROW) == 1 ? 2 : 1;
                if (row == R16(KID(F_ROW))) row = -1;
            }
        } else {
            row = R16(KID(F_ROW)) == 2 ? 1 : 2;
        }
    }
    if (row < 0) return 0;
    if (n != 0 && S16(Rnd(0x14)) != 0) return 0;
    int16_t col = row == 1 ? 4 : 2;
    if (S16(Rnd(1)) != 0) col += 4;
    FinlGenerateOpp(col, row);
    return 0;
}

/* 00037e56: level 14 room 1 / final rooms: draw the foreground items in two passes
 * (some shapes first, the others after) */
uint32_t FUN_00037e56(void) {
    ENTRY_NOLINK(FN_FUN_00037e56, 20);           /* movem D3-D6/A3 */
    int16_t n = R16(gNumFore);
    uint32_t it;
    int16_t i;
    if (R16(gCurRoom) == 1 && LEVEL_NUM() == 0xe) {
        it = R32(gForeTable);
        i = 0;
        if (R16(gNumFore) > 0) {
            do {
                if (R16(it + 0x18) == 0x6310) DrawBFItem(it);
                i++; it += 0x26;
            } while (i < n);
        }
        it = R32(gForeTable);
        i = 0;
        if (n <= 0) return 0;
        do {
            if (R16(it + 0x18) != 0x6310) DrawBFItem(it);
            i++; it += 0x26;
        } while (i < n);
        return 0;
    }
    int16_t room = R16(gCurRoom);
    if (!(room == 6 && LEVEL_TYPE() == 6)) {
        if (room != 7 && room != 8) return 0;
        if (LEVEL_TYPE() != 6) return 0;
    }
    it = R32(gForeTable);
    i = 0;
    if (R16(gNumFore) > 0) {
        do {
            int16_t s = R16(it + 0x18);
            if (!((s >= 0x1025 && s <= 0x1028) || (s >= 0x1051 && s <= 0x1058))) DrawBFItem(it);
            i++; it += 0x26;
        } while (i < n);
    }
    it = R32(gForeTable);
    i = 0;
    if (n <= 0) return 0;
    do {
        int16_t s = R16(it + 0x18);
        if ((s >= 0x1025 && s <= 0x1028) || (s >= 0x1051 && s <= 0x1058)) DrawBFItem(it);
        i++; it += 0x26;
    } while (i < n);
    return 0;
}

/* 00037f78: create a Jaffar copy in room 3 at (col, row) and make it the current char */
uint32_t FinlGenerateOpp(int16_t col, int16_t row) {
    ENTRY_LINK(FN_FinlGenerateOpp, 0, 20);       /* link 0; movem D3-D4/D6/A2-A3 */
    uint32_t gs = GS();
    int16_t id = R16(gs + 0x2326);
    W16(gs + 0x2326, R16(gs + 0x2326) + 1);
    uint32_t info = GetOppInfoIdx(3, id);
    W16(info, S16(FirstInRow(row)) + col);
    W16(info + 2, RU16(gColX + (int16_t)(col * 2)));
    W16(info + 4, col < 5 ? 0 : -1);
    W16(info + 6, 5);
    if (R16(gOppBank) == 0) {
        PresetOppBanks(3);
        FUN_0002e81c();
    }
    W16(info + 0xc, RU16(gOppBank));
    W16(info + 0xe, id);
    W16(info + 0x10, 0);
    W16(info + 0x14, 0);
    W16(info + 0x20, row);
    int16_t life = (int16_t)(S16(Rnd(2)) + 3);
    W16(info + 0x12, life);
    W16(info + 0x1a, life);
    W16(CH(F_ID), id);
    W16(CH(F_FACE), RU16(info + 4));
    W16(CH(F_X), RU16(info + 2));
    W16(CH(F_TYPE), RU16(gOppKindType + (int16_t)(R16(GS() + 0x21a4) * 2)));
    W16(CH(F_COL), col);
    W16(CH(F_ROW), row);
    W16(CH(F_38), 0);
    W16(CH(F_ROOM), 3);
    life = R16(info + 0x12);
    W16(CH(F_MAXLIFE), life);
    W16(CH(F_LIFE), life);
    W16(CH(F_PENDING), life);
    W16(CH(F_ALIVE), -1);
    W16(CH(F_36), 0x20);
    FUN_00020cca();
    seg4_020fb2();
    W16(CH(F_34), 0);
    W16(CH(F_18), 1);
    W16(CH(F_1A), 0);
    JumpSeq(0xee);
    W16(CH(F_32), 0);
    AnimChar();
    AddSound(0xf3);
    if (R16(gFightOppId) == -1) W16(gFightOppId, RU16(CH(F_ID)));
    seg4_022516();
    return 0;
}

/* 000380da: which room character to cut to in rooms 7/8 (0, 1 or -1) */
uint32_t FUN_000380da(void) {
    ENTRY_NOLINK(FN_FUN_000380da, 4);            /* move.l D3 */
    int16_t i = R16(gOppTab + F_SEQ) == 0xe6 ? 0 : 1;
    if (R16(gCurRoom) == 7 && R16(gOppTab + F_COL + (int32_t)i * REC_SIZE) >= 10) return 1;
    if (R16(gCurRoom) == 8 && R16(gOppTab + F_COL + (int32_t)i * REC_SIZE) < 0) return 0;
    return (uint16_t)-1;
}

/* 0003814e: a character dead in room 3 row 1 of the last level is drawn with image 0xe2 */
uint32_t seg9_03814e(void) {
    ENTRY_NOLINK(FN_seg9_03814e, 0);
    if (R16(CH(F_ROOM)) == 3 && LEVEL_TYPE() == 6 && R16(CH(F_ROW)) == 1 &&
        S16(IsDeadPos(R16(CH(F_FRAME)))) != 0)
        W16(gFCharSeq, 0xe2);
    return 0;
}

/* 00038182: place the kid in room 4 (column 5, row 1) falling in (sequence 0x37) */
uint32_t FUN_00038182(void) {
    ENTRY_NOLINK(FN_FUN_00038182, 8);            /* movem D3-D4 */
    W16(CH(F_36), 0x10);
    W16(CH(F_FACE), 0);
    JumpSeq(0x37);
    W16(CH(F_1A), 1);
    AddSound(0xe);
    W16(CH(F_ROOM), 4);
    W16(gNextRoom, 4);
    W16(CH(F_COL), 5);
    W16(CH(F_X), RU16(gColX + 5 * 2) + 0x16);
    W16(CH(F_ROW), 1);
    FUN_00020cca();
    W16(CH(F_PENDING), RU16(CH(F_LIFE)));
    AnimChar();
    copy_rec(gKid, gChar);
    seg4_02190a();
    W16(gInBtn, 0);
    return 0;
}

/* 00038204: per-frame hook of the last level: kid events, new copies, music */
uint32_t seg9_038204(void) {
    ENTRY_NOLINK(FN_seg9_038204, 4);             /* move.l D4 */
    FUN_00038310();
    FinlWatchOpps();
    if (R16(gAmbient) == 0) return 0;
    if (S16(PrinceSndOn(R16(gAmbientSnd))) != 0) return 0;
    if (R16(KID(F_ALIVE)) >= 0) return 0;
    if (R16(gNextLevel) != R16(gLevel)) return 0;
    int16_t song = -1;
    int16_t r = (int16_t)(R16(gCurRoom) - 3);    /* jump table at 0x3825a */
    if (r >= 0 && r <= 5) {
        switch (r) {
        case 0: song = 0x10c; break;               /* room 3 */
        case 1: case 2: song = 0x10b; break;       /* rooms 4, 5 */
        case 3:                                    /* room 6 */
            if (R16(GS() + 0x2566) != 0) song = 0x10d;
            else if (S16(PrinceSndOn(0x10d)) != 0) PrinceResetSnd(0x10d);
            break;
        case 5:                                    /* room 8 */
            W16(GH(0x10300e), 1);
            /* fall through */
        case 4:                                    /* room 7 */
            if (R16(GH(0x10300e)) != 0) song = 0x106;
            break;
        }
    }
    if (song != -1 && S16(PrinceSndOn(song)) == 0) AddSong(song);
    return 0;
}

/* 000382ca: kid reaches room 2's exit: play NIS 5, reload the level, start in room 4 */
uint32_t FUN_000382ca(void) {
    ENTRY_NOLINK(FN_FUN_000382ca, 0);
    PlayNIS(5);
    FUN_00016112();
    LoadLevel(R16(gLevel));
    PoPSetPalette(0x3ee, 0xf0);
    SetKidPalette(6);
    ResetStandardColors(R32(gWorldPal));
    FUN_00038182();
    W16(GH(0x103000), -1);
    return 0;
}

/* 00038310: kid events of the last level: leaving room 2, or being hit in room 5 */
uint32_t FUN_00038310(void) {
    ENTRY_NOLINK(FN_FUN_00038310, 12);           /* movem D3-D5 */
    if (R16(KID(F_ROOM)) == 2) {
        if (R16(gCharRight) > 0x1e6) return 0;
        if (R16(KID(F_ACTION)) == 4) return 0;
        FUN_000225d2();
        FUN_000382ca();
        copy_rec(gKid, gChar);
        return 0;
    }
    if (R16(KID(F_TYPE)) != 1) return 0;
    if (R16(KID(F_SEQ)) == 0x47) return 0;
    int16_t idx = S16(GetKidIdxAsOpp());
    LoadShad(idx);
    int16_t r = S16(FUN_0002ed6a());
    if (r != 7 && r != 8) return 0;
    if (LEVEL_TYPE() != 6) return 0;
    if (R16(KID(F_ROOM)) != 5) return 0;
    if (LEVEL_TYPE() != 6) return 0;
    if (R16(KID(F_COL)) > 4) return 0;
    uint16_t f = RU16(KID(F_FRAME));
    if ((frame_flags(f) & 0x40) == 0 && (f < 0xbf || f > 0xcd)) return 0;
    FUN_000225d2();
    DecLifePts(100);
    JumpSeq(0x47);
    copy_rec(gKid, gChar);
    ChangeMeters();
    seg2_01585a();
    return 0;
}

/* 00038424: in room 3, maybe generate another Jaffar copy */
uint32_t FinlWatchOpps(void) {
    ENTRY_LINK(FN_FinlWatchOpps, 4, 8);          /* link -4; movem D3-D4 */
    int16_t room = R16(gCurRoom);
    if (room == 3 && LEVEL_TYPE() == 6)
        FinlCheckGenOpp(room_char_count((uint16_t)room));
    return 0;
}

/* 00038498: control of a Jaffar copy */
uint32_t FUN_00038498(void) {
    ENTRY_NOLINK(FN_FUN_00038498, 4);            /* move.l D3 */
    if (R16(CH(F_ROOM)) == 6 && LEVEL_TYPE() == 6) FUN_000390d6();
    else FUN_000392ae();
    return 0;
}

/* 000384be: every Jaffar copy of the opponent's room that is standing shimmers away */
uint32_t AllOtherJaffarsShimmerAway(void) {
    ENTRY_LINK(FN_AllOtherJaffarsShimmerAway, 0x40, 12);  /* link -0x40; movem D3-D5 */
    maddr saved = a6 - 0x40;
    copy_rec(saved, gChar);
    int16_t n = room_char_count(RU16(OPP(F_ROOM)));
    for (int16_t i = 0; i < n; i++) {
        LoadShad(i);
        if (R16(CH(F_TYPE)) == 6 && R16(CH(F_ID)) != R16(OPP(F_ID)) && R16(CH(F_FRAME)) == 0xf) {
            JumpSeq(0xf0);
            seg4_022516();
        }
    }
    copy_rec(gChar, saved);
    return 0;
}

/* 0003857a: turn to face the opponent (5), or 0xf2 / 0xd */
uint32_t FUN_0003857a(void) {
    ENTRY_NOLINK(FN_FUN_0003857a, 8);            /* movem D3-D4 */
    int16_t seq = -1;
    if (R16(CH(F_FRAME)) == 0xf) {
        int16_t ox = R16(OPP(F_X));
        int16_t x = R16(CH(F_X));
        if ((ox < x && R16(CH(F_FACE)) != -1) || (ox >= x && R16(CH(F_FACE)) != 0)) seq = 5;
        else seq = 0xf2;
    } else {
        uint16_t f = RU16(CH(F_FRAME));
        if ((f >= 1 && f <= 0xe) || (f >= 0x31 && f <= 0x38)) {
            int16_t s = R16(CH(F_SEQ));
            if (s != 0xd && s != 5) seq = 0xd;
        }
    }
    if (seq != -1) JumpSeq(seq);
    return 0;
}

/* 000385f2: Jaffar copy in "attack" mode (info +0x1c = 2) */
uint32_t FUN_000385f2(void) {
    ENTRY_NOLINK(FN_FUN_000385f2, 16);           /* movem D3-D4/A2-A3 */
    int16_t go = 1;
    uint32_t info = GetOppInfoIdx(R16(CH(F_ROOM)), R16(CH(F_ID)));
    uint32_t pos = info + 0x1c;                  /* {mode, target pos, current pos} */
    if (R16(pos) != 2) W16(pos, 2);
    if (R16(CH(F_SEQ)) == 0xf2) {
        if (R16(CH(F_FRAME)) == 0x119) AddSound(0x121);
        else if (R16(CH(F_FRAME)) == 0x11a) {
            if (S16(FUN_00038756()) != 0) FUN_00039078();
        }
        go = 0;
    } else if (R16(pos + 2) == R16(pos + 4)) {
        if (R16(OPP(F_ROW)) == R16(CH(F_ROW)) && S16(FUN_00038756()) != 0) FUN_0003857a();
        else if (R16(OPP(F_SEQ)) != 0xf1) {
            if (S16(ShadowInRange(R16(pos + 2), -1)) != 0) JaffarSetToNextPos(pos);
            else JaffarSetToPrevPos(pos);
        }
    }
    if (go && R16(pos + 2) != R16(pos + 4) && R16(OPP(F_LIFE)) > 0) {
        if (R16(CH(F_ROW)) == 1 && S16(FUN_00038756()) != 0) FUN_0003857a();
        else JaffarGoToPos(pos);
    }
    return 0;
}

/* 000386d8: Jaffar copy in "evade" mode (info +0x1c = 1) */
uint32_t FUN_000386d8(void) {
    ENTRY_NOLINK(FN_FUN_000386d8, 12);           /* movem D3/A2-A3 */
    uint32_t info = GetOppInfoIdx(R16(CH(F_ROOM)), R16(CH(F_ID)));
    uint32_t pos = info + 0x1c;
    if (R16(pos) != 1) W16(pos, 1);
    int16_t p = R16(pos + 2);
    if (p == R16(pos + 4)) {
        if (S16(ShadowInRange(p, -1)) != 0) JaffarSetToPrevPos(pos);
        else if (S16(ShadowInRange(R16(pos + 2), 1)) != 0) JaffarSetToNextPos(pos);
    }
    if (R16(pos + 2) != R16(pos + 4)) JaffarGoToPos(pos);
    return 0;
}

/* 00038756: can the current char strike the opponent (kid) from here? */
uint32_t FUN_00038756(void) {
    int16_t r = 0;
    if (R16(OPP(F_LIFE)) > 0) {
        int16_t oroom = R16(OPP(F_ROOM));
        if ((oroom == 7 || oroom == 8) && LEVEL_TYPE() == 6 && R16(OPP(F_SEQ)) != 0xf1 &&
            R16(CH(F_ROW)) == R16(OPP(F_ROW))) {
            uint16_t f = RU16(CH(F_FRAME));
            if (((frame_flags(f) & 0x40) != 0 || (f >= 0xbf && f <= 0xcd)) && f != 0x6d) {
                int16_t c = R16(CH(F_COL));
                if (R16(CH(F_ROOM)) == 8) c += 10;
                if (R16(CH(F_ROW)) == 0) {
                    r = (c > 6 && c < 0xd) ? 0 : 1;
                } else if (R16(CH(F_ROW)) == 1) {
                    int16_t oc = R16(OPP(F_COL));
                    if (R16(OPP(F_ROOM)) == 8) oc += 10;
                    if (c <= 0xc) r = oc > 0xc ? 0 : 1;
                    else r = oc >= 0xe ? 1 : 0;
                } else if (R16(CH(F_ROW)) == 2) {
                    r = 1;
                }
            }
        }
    }
    return (uint16_t)r;
}

/* 0003885c: next of the four positions (cyclic); p points at {mode, target, current} */
uint32_t JaffarSetToNextPos(uint32_t p) {
    if (R16(p + 2) == 3) W16(p + 2, 0);
    else W16(p + 2, R16(p + 2) + 1);
    return 0;
}

/* 0003889c: previous position (cyclic) */
uint32_t JaffarSetToPrevPos(uint32_t p) {
    if (R16(p + 2) != 0) W16(p + 2, R16(p + 2) - 1);
    else W16(p + 2, 3);
    return 0;
}

/* 000388d8: move towards the target position */
uint32_t JaffarGoToPos(uint32_t p) {
    ENTRY_LINK(FN_JaffarGoToPos, 0, 8);          /* link 0; movem D4/A2 */
    /* D4 is not set for a target outside 0..3: the original then uses the caller's D4 */
    int16_t seq = (int16_t)cpu_get(R_D4);
    switch (R16(p + 2)) {
    case 0: seq = S16(JaffarGoToPos0(p)); break;
    case 1: seq = S16(JaffarGoToPos1(p)); break;
    case 2: seq = S16(JaffarGoToPos2(p)); break;
    case 3: seq = S16(JaffarGoToPos3(p)); break;
    }
    if (seq != -1) JumpSeq(seq);
    return 0;
}

/* 0003895c: step to the exact x range of the current position and face its way;
 * returns a sequence to start (5 = turn) or -1 */
uint32_t JaffarHoneInOnExactPos(uint32_t p) {
    ENTRY_LINK(FN_JaffarHoneInOnExactPos, 0, 24); /* link 0; movem D3/D5-D7/A2-A3 */
    int16_t seq = -1;
    if (R16(CH(F_FRAME)) != 0xf) return (uint16_t)seq;
    if (R16(p) == 2 && S16(FUN_00038756()) != 0) {
        FUN_0003857a();
        return (uint16_t)seq;
    }
    uint32_t jp = gJafPos + (int32_t)R16(p + 2) * 0x16;
    int16_t x = R16(CH(F_X));
    int16_t face = R16(jp + 4);
    if (R16(CH(F_FACE)) != face)
        x = (int16_t)(x + (int16_t)(R16(gFaceDx + (int16_t)(face * 2)) * 10));
    if (R16(jp) < 10) {
        if (R16(CH(F_ROOM)) == 8) x += 0x1fe;
    } else if (R16(CH(F_ROOM)) == 7) {
        x -= 0x1fe;
    }
    int16_t xmin = R16(jp + 6);
    if (x < xmin) {
        if (R16(CH(F_FACE)) == 0) DoStepFwd((int16_t)((int16_t)(xmin - x) + 4));
        else seq = 5;
    } else if (x > R16(jp + 8)) {
        if (R16(CH(F_FACE)) == -1) DoStepFwd((int16_t)((int16_t)(x - R16(jp + 6)) + 4));
        else seq = 5;
    } else if (R16(CH(F_FACE)) != R16(jp + 4)) {
        seq = 5;
    } else {
        W16(p + 4, RU16(p + 2));
    }
    return (uint16_t)seq;
}

/* the column counted across rooms 7 (0-9) and 8 (10-19) */
static int16_t wide_col(void) {
    return (int16_t)((R16(CH(F_ROOM)) == 8 ? 1 : 0) * 10 + R16(CH(F_COL)));
}

/* 00038a82: go to position 0 */
uint32_t JaffarGoToPos0(uint32_t p) {
    ENTRY_LINK(FN_JaffarGoToPos0, 0, 20);        /* link 0; movem D3-D5/A2-A3 */
    int16_t c = wide_col();
    int16_t seq = -1;
    uint32_t jp = gJafPos + (int32_t)R16(p + 2) * 0x16;
    if (R16(p + 4) == 3) {
        if (R16(CH(F_FRAME)) == 0xf) {
            if (c == 6) seg6_02bc2a();
            else if (c > 7 && (int32_t)R16(jp) - 1 > (int32_t)c) seg6_02b7e0();
            else seq = S16(JaffarHoneInOnExactPos(p));
        } else {
            int16_t f = R16(CH(F_FRAME));
            if (f == 7 || f == 0xb) {
                if ((c >= 5 && c <= 6) || (c >= 0xa && c <= 0xb)) DoRunJump(4);
                else if ((int32_t)R16(jp) - 1 <= (int32_t)c) seq = 0xd;
            }
        }
    } else if (R16(CH(F_ROW)) == 2) {
        if (R16(CH(F_FRAME)) == 0xf) {
            if (R16(CH(F_FACE)) == 0) DoJumpUp();
            else seq = 5;
        } else if (R16(CH(F_FRAME)) == 0x5b) {
            seg6_02bfd8();
        }
    } else if (R16(CH(F_ROW)) == 1) {
        if (R16(CH(F_FRAME)) == 0xf) {
            if (R16(CH(F_FACE)) == 0) {
                if (c < 0x11) seg6_02b7e0();
                else seq = 5;
            } else {
                if (c < 0x11) seq = 5;
                else DoJumpUp();
            }
        } else {
            int16_t f;
            if (c >= 0x11 && ((f = R16(CH(F_FRAME))) == 7 || f == 0xb)) seq = 0xd;
            else if (R16(CH(F_FRAME)) == 0x5b) seg6_02bfd8();
            else if (R16(CH(F_FRAME)) == 0x6d) seq = 0x31;
        }
    } else {
        seq = S16(JaffarHoneInOnExactPos(p));
    }
    return (uint16_t)seq;
}

/* 00038c2c: go to position 1 */
uint32_t JaffarGoToPos1(uint32_t p) {
    ENTRY_LINK(FN_JaffarGoToPos1, 0, 20);        /* link 0; movem D3-D5/A2-A3 */
    int16_t seq = -1;
    int16_t target = R16(p + 2);
    uint32_t jp = gJafPos + (int32_t)target * 0x16;
    if (R16(p + 4) < target) {
        if (R16(CH(F_ROW)) == 0) {
            if (R16(CH(F_FACE)) == 0 && R16(CH(F_SEQ)) != 5) seq = 5;
            else if (R16(CH(F_FRAME)) == 0xf) seg6_02b4ce();
        } else if (R16(CH(F_ROW)) == 1) {
            uint16_t f = RU16(CH(F_FRAME));
            if (f >= 0x57 && f <= 0x5b) seg6_02c0c4();
            else if (R16(CH(F_FRAME)) == 0xf) {
                if (R16(CH(F_FACE)) == -1) seg6_02b7e0();
                else seq = 5;
            } else if (R16(CH(F_FRAME)) == 0x6d) {
                seq = 0x31;
            }
        } else if (R16(CH(F_ROW)) == 2) {
            if (R16(CH(F_FRAME)) == 0x6d) seq = 0x31;
            else seq = S16(JaffarHoneInOnExactPos(p));
        }
    } else {
        int16_t c = wide_col();
        if (c <= R16(gJafPos + 2 * 0x16) && R16(CH(F_FRAME)) == 0xf) {
            seg6_02b7e0();
        } else if (c >= 6 && c <= 7 && R16(CH(F_X)) >= R16(gColX + 7 * 2)) {
            DoRunJump(4);
        } else if (c > 0xa) {
            int16_t f;
            if ((int32_t)R16(jp) - 1 <= (int32_t)c && ((f = R16(CH(F_FRAME))) == 7 || f == 0xb))
                seq = 0xd;
            else
                seq = S16(JaffarHoneInOnExactPos(p));
        }
    }
    return (uint16_t)seq;
}

/* 00038dae: go to position 2 */
uint32_t JaffarGoToPos2(uint32_t p) {
    ENTRY_LINK(FN_JaffarGoToPos2, 4, 16);        /* link -4; movem D3-D5/A2 */
    int16_t seq = -1;
    if (R16(p + 4) < R16(p + 2)) {
        int16_t c = wide_col();
        int16_t f;
        if ((int32_t)c + 1 >= (int32_t)R16(gJafPos + 1 * 0x16) && R16(CH(F_FRAME)) == 0xf) {
            seg6_02b7e0();
        } else if (c >= 0xb && (int32_t)R16(gColX + 2 * 2) + 0x39 > (int32_t)R16(CH(F_X)) &&
                   R16(CH(F_SEQ)) != 4 && ((f = R16(CH(F_FRAME))) == 7 || f == 0xb)) {
            W16(CH(F_X), AddCharX(3));
            DoRunJump(4);
        } else if (c <= 6 && ((f = R16(CH(F_FRAME))) == 7 || f == 0xb)) {
            seq = 0xd;
        } else {
            seq = S16(JaffarHoneInOnExactPos(p));
        }
    } else if (R16(CH(F_ROW)) == 0) {
        if (R16(CH(F_FRAME)) == 0xf) {
            if (R16(CH(F_FACE)) == 0) seq = 5;
            else seg6_02b4ce();
        }
    } else if (R16(CH(F_ROW)) == 1) {
        uint16_t f = RU16(CH(F_FRAME));
        if (f >= 0x57 && f <= 0x5b) seg6_02c0c4();
        else if (R16(CH(F_FRAME)) == 0xf) {
            if (R16(CH(F_FACE)) == -1) seg6_02b7e0();
            else seq = 5;
        } else if (R16(CH(F_FRAME)) == 0x6d) {
            seq = 0x31;
        }
    } else if (R16(CH(F_ROW)) == 2) {
        if (R16(CH(F_FRAME)) == 0x6d) seq = 0x31;
        else seq = S16(JaffarHoneInOnExactPos(p));
    }
    return (uint16_t)seq;
}

/* 00038f1e: go to position 3 */
uint32_t JaffarGoToPos3(uint32_t p) {
    ENTRY_LINK(FN_JaffarGoToPos3, 0, 20);        /* link 0; movem D3-D5/A2-A3 */
    int16_t seq = -1;
    uint32_t jp = gJafPos + (int32_t)R16(p + 2) * 0x16;
    int16_t c = wide_col();
    if (R16(p + 4) == 0) {
        if (R16(CH(F_FRAME)) == 0xf) {
            if (c > 0xa) seg6_02b7e0();
            else if (c <= R16(jp)) seq = S16(JaffarHoneInOnExactPos(p));
        } else {
            int16_t f = R16(CH(F_FRAME));
            if (f == 7 || f == 0xb) {
                if (c <= R16(jp)) seq = 0xd;
                else if ((c >= 0xe && c <= 0xf) || (c >= 9 && c <= 0xa)) DoRunJump(4);
            }
        }
    } else if (R16(CH(F_ROW)) == 2) {
        if (R16(CH(F_FRAME)) == 0xf) DoJumpUp();
        else if (R16(CH(F_FRAME)) == 0x5b) seg6_02bfd8();
    } else if (R16(CH(F_ROW)) == 1) {
        if (R16(CH(F_FRAME)) == 0xf) {
            if (R16(CH(F_FACE)) == 0) {
                if (c == 6) DoStepFwd(0x33);
                else seq = 5;
            } else {
                DoJumpUp();
            }
        } else if (R16(CH(F_FRAME)) == 0x5b) {
            seg6_02bfd8();
        } else if (R16(CH(F_FRAME)) == 0x6d) {
            seq = 0x31;
        }
    } else {
        seq = S16(JaffarHoneInOnExactPos(p));
    }
    return (uint16_t)seq;
}

/* 00039078: Jaffar copy strikes the kid down (kid death method 0x11) */
uint32_t FUN_00039078(void) {
    ENTRY_NOLINK(FN_FUN_00039078, 4);            /* move.l D3 */
    JumpSeq(0xf4);
    int16_t id = R16(CH(F_ID));
    seg4_022516();
    FUN_000225d2();
    JumpSeq(0x47);
    AddSound(0xc);
    DecLifePts(100);
    SetKidDeathMethod(0x11);
    copy_rec(gKid, gChar);
    LoadShadwOpp(id);
    return 0;
}

/* 000390d6: control of a Jaffar copy in room 6 (the crystal room) */
uint32_t FUN_000390d6(void) {
    ENTRY_NOLINK(FN_FUN_000390d6, 16);           /* movem D3-D6 */
    if (R16(CH(F_ALIVE)) >= 0) return 0;
    int16_t n = room_char_count(RU16(CH(F_ROOM)));
    if (R16(OPP(F_TYPE)) == 1) n--;
    if (n == 1 && R16(CH(F_SEQ)) != 0xf0 && R16(KID(F_ALIVE)) < 0) {
        JumpSeq(0xf0);
        return 0;
    }
    if (R16(CH(F_FRAME)) == 0xf) {
        if (R16(KID(F_ALIVE)) >= 0) return 0;
        int16_t ks = R16(KID(F_SEQ));
        if (ks == 0x2c || ks == 6) return 0;
        if (R16(KID(F_TYPE)) != 0) return 0;
        uint16_t kf = RU16(KID(F_FRAME));
        if ((frame_flags(kf) & 0x40) == 0 && (kf < 0xbf || kf > 0xcd)) return 0;
        int16_t ka = R16(KID(F_ACTION));
        if (ka == 3 || ka == 4) return 0;
        int16_t go = 0;
        int16_t d = S16(seg4_0224be());
        if (R16(CH(F_FACE)) == R16(OPP(F_FACE))) {
            d = (int16_t)(d - 0xf);
            if (d > 0x18 && d < 0x47) go = 1;
        } else {
            int16_t face = R16(CH(F_FACE));
            if (face != R16(OPP(F_FACE))) {
                if (d < 0) {
                    d = (int16_t)(-0xf - d);
                    if (d > 0x18 && d < 0x47) {
                        W16(CH(F_FACE), (uint16_t)~face);
                        go = 1;
                    }
                } else if (R16(KID(F_FRAME)) == 0x91) {
                    d = (int16_t)(d - 0xf);
                    if (d > 0x18 && d < 0x4e) go = 1;
                }
            }
        }
        if (go) {
            W16(CH(F_X), AddCharX(0xf));
            W16(CH(F_1A), 1);
            JumpSeq(0x4b);
        }
    } else if (R16(CH(F_FRAME)) == 0xab) {
        W16(CH(F_1A), 0);
        JumpSeq(0x5c);
    } else if (R16(CH(F_FRAME)) == 0x34 && R16(KID(F_ALIVE)) > 0) {
        JumpSeq(0xc5);
        AddSound(0x105);
    } else if (R16(CH(F_FRAME)) == 0x6d) {
        JumpSeq(0x31);
    }
    return 0;
}

/* 000392ae: control of a Jaffar copy (rooms 7/8): choose evade / attack mode */
uint32_t FUN_000392ae(void) {
    ENTRY_NOLINK(FN_FUN_000392ae, 24);           /* movem D3-D6/A2-A3 */
    uint32_t info = GetOppInfoIdx(R16(CH(F_ROOM)), R16(CH(F_ID)));
    uint32_t pos = info + 0x1c;
    int16_t evade = (R16(pos) != 1 || R16(pos + 2) == R16(pos + 4)) ? 0 : 1;
    int16_t attack = 0;
    if (R16(OPP(F_TYPE)) == 0) {
        int done = 0;
        if (R16(OPP(F_1A)) == 1) {
            uint16_t f = RU16(CH(F_FRAME));
            if ((f < 0xcf || f > 0xd2) && f != 0x9e) {
                int16_t d = S16(seg4_0224be());
                int32_t ad = d < 0 ? -(int32_t)d : (int32_t)d;
                if (ad < 0x5b) {
                    int16_t id = R16(CH(F_ID));
                    seg4_022516();
                    FUN_000225d2();
                    JumpSeq(0xf1);
                    AddSong(0x108);
                    W16(CH(F_1A), -1);
                    copy_rec(gKid, gChar);
                    LoadShad(id);
                }
                done = 1;
            }
        }
        if (!done) {
            if (R16(OPP(F_ROW)) == R16(CH(F_ROW)) && R16(GH(0x102ffc)) >= 4)
                evade = (R16(OPP(F_LIFE)) > 2 || R16(OPP(F_SEQ)) == 0xf2) ? 1 : 0;
            else
                attack = R16(OPP(F_1A)) == -1 ? 1 : 0;
        }
    } else {
        int16_t life = R16(OPP(F_LIFE));
        evade = (life > 2 || R16(OPP(F_SEQ)) == 0xf2) ? 1 : 0;
        attack = (life > 2 || R16(OPP(F_SEQ)) == 0xf2) ? 0 : 1;
    }
    if (R16(CH(F_SEQ)) == 0xe6 && R16(CH(F_34)) == 0xc) W16(gNextLevel, R16(gNextLevel) + 1);
    if (evade) FUN_000386d8();
    else if (attack) FUN_000385f2();
    return 0;
}

/* 0003940a: move a Jaffar copy to its new position when its target changed;
 * p = its per-room info record (+0x1c: {mode, target, current, y}) */
uint32_t JaffarUpdateOppInfo(uint32_t p) {
    ENTRY_LINK(FN_JaffarUpdateOppInfo, 0, 16);   /* link 0; movem D3/A2-A4 */
    uint32_t pos = p + 0x1c;
    int16_t room = R16(gNextRoom);
    if (!((room == 7 || room == 8) && LEVEL_TYPE() == 6)) {
        int16_t t = R16(pos + 2);
        if (t != R16(pos + 4)) {
            W16(pos + 4, t);
            uint32_t jp = gJafPos + (int32_t)R16(pos + 2) * 0x16;
            W16(CH(F_ROW), RU16(jp + 2));
            W16(CH(F_COL), RU16(jp));
            W16(CH(F_X), RU16(jp + 6) + 10);
            if (R16(CH(F_ROOM)) == 8) {
                W16(CH(F_X), RU16(CH(F_X)) - 0x1fe);
                W16(CH(F_COL), RU16(CH(F_COL)) - 10);
            }
            W16(CH(F_FACE), RU16(jp + 4));
            FUN_00020cca();
            JumpSeq(2);
            AnimChar();
            W16(p, IndexCell(R16(CH(F_COL)), R16(CH(F_ROW))));
            W16(p + 2, RU16(CH(F_X)));
            W16(p + 4, RU16(CH(F_FACE)));
            W16(p + 0xa, RU16(CH(0x24)));
            W16(p + 8, RU16(CH(0x26)));
        }
    }
    W16(pos + 6, RU16(CH(F_Y)));
    return 0;
}

/* 00039504: is the kid in the range covered by position pos in direction dir
 * (dir < 0: range 0, else range 1)? The original stores the normalised dir back
 * into its argument slot (glue_ShadowInRange). */
static uint32_t ShadowInRange_(int16_t pos, int16_t *pdir) {
    *pdir = *pdir < 0 ? 0 : 1;
    uint32_t r = gJafPos + (int32_t)pos * 0x16 + (int32_t)*pdir * 2;
    if (R16(r + 0xa) != R16(OPP(F_ROW))) return 0;
    int16_t c = (int16_t)((R16(OPP(F_ROOM)) == 8 ? 1 : 0) * 10 + R16(OPP(F_COL)));
    return (c < R16(r + 0xe) || c > R16(r + 0x12)) ? 0 : 1;
}
uint32_t ShadowInRange(int16_t pos, int16_t dir) {
    return ShadowInRange_(pos, &dir);
}
void glue_ShadowInRange(void) {
    maddr sp = emu_sp();
    int16_t dir = R16(sp + 2);
    uint32_t r = ShadowInRange_(R16(sp), &dir);
    W16(sp + 2, dir);
    emu_set_d0(r);
}

/* 000395aa: put the char (Jaffar) high above room column 17, facing like position 0 */
uint32_t FUN_000395aa(void) {
    ENTRY_NOLINK(FN_FUN_000395aa, 4);            /* move.l A2 */
    W16(CH(F_COL), 0x11);
    W16(CH(F_X), 0x46c);
    W16(CH(F_ROW), -3);
    W16(CH(F_Y), -3 * 0x78 + 0x6a);
    W16(CH(F_FACE), RU16(gJafPos + 4));
    uint32_t info = GetOppInfoIdx(R16(CH(F_ROOM)), R16(CH(F_ID)));
    W16(info + 0x22, 0);
    JumpSeq(2);
    AnimChar();
    UpdateOppInfo();
    return 0;
}

/* 000395fc: flash the screen (fill the game rect with color 0x40) */
uint32_t seg9_0395fc(void) {
    ENTRY_NOLINK(FN_seg9_0395fc, 0);
    if (R16(GH(0x103000)) == -1) PoPSetPalette(0xdac, 0x40);
    SaveCurrentWorld(GH(0x102dc2));
    clip_rect_ptr(GH(0x103014));
    PoPFillRect(GH(0x103014), 0x40);
    RestoreOriginalWorld(GH(0x102dc2));
    return 0;
}

/* 00039640: room palettes and music for rooms 4 and 3 */
uint32_t seg9_039640(void) {
    ENTRY_NOLINK(FN_seg9_039640, 0);
    PoPSetPalette(0xdad, 0x40);
    if (R16(gCurRoom) == 4 && LEVEL_TYPE() == 6) {
        W16(GH(0x105490), 0xd0);
        PoPSetPalette(0xfed, R16(GH(0x105490)));
        PoPSetPalette(0x3ee, 0xf0);
        AddSong(0x10b);
        SaveCurrentWorld(GH(0x102dc2));
        _EraseRect(GH(0x103014));
        RestoreOriginalWorld(GH(0x102dc2));
        WaitSndId(0x7724, 1);
    } else if (R16(gCurRoom) == 3 && LEVEL_TYPE() == 6) {
        PoPSetPalette(0xbb8, 0xe0);
        AddSong(0x10c);
    }
    return 0;
}

#define gTrobState    GH(0x1037e8)  /* word: state of the trob being animated */
#define gTrobDir      GH(0x103c16)  /* word: -1 ends the trob */
#define gCustRoom     GH(0x103362)  /* long: locked custom-room data (+0 template id) */

/* 000396ea: animate custom room 0x1a's piece 3 (redraw frames 12-15, then stop) */
uint32_t seg9_0396ea(void) {
    ENTRY_NOLINK(FN_seg9_0396ea, 16);            /* movem D3-D4/A2-A3 */
    W16(gTrobState, R16(gTrobState) + 1);
    uint16_t ph = RU16(gTrobState) & 0x1f;
    uint32_t cr = R32(gCustRoom);
    if (R16(cr) == 0x1a) {
        if (ph < 0xc) return 0;
        ph -= 0xc;
        if (ph < 4) {
            uint32_t piece = cr + 0x7e;
            mark_bufs_for_rect(JT_ADDR(FN_MarkMove), piece + 0x10);
            return 0;
        }
    }
    W16(gTrobDir, -1);
    W16(gTrobState, 0);
    return 0;
}

/* 00039754: draw the custom-room image for frames 12-15 of a layer-0xb item */
uint32_t seg9_039754(void) {
    ENTRY_NOLINK(FN_seg9_039754, 12);            /* movem D3-D4/A2 */
    uint32_t it = R32(gCurBFItem);
    uint16_t ph = RU16(it + 0x14) & 0x1f;
    if (ph < 0xc) return 0;
    if (R16(it + 0xa) != 0xb) return 0;
    ph -= 0xc;
    if (ph >= 4) return 0;
    DrawCustomRoomImg((int16_t)(R16(GH(0x105652) + (int32_t)ph * 2) + 3), 0xb);
    return 0;
}

/* 000397a0: the spider (custom room 0x19): state = wait counter << 3 | frame */
uint32_t AnimFinlSpider(void) {
    ENTRY_LINK(FN_AnimFinlSpider, 4, 12);        /* link -4; movem D3-D5 */
    if (R16(R32(gCustRoom)) != 0x19) {
        W16(gTrobDir, -1);
        return 0;
    }
    uint16_t st = RU16(gTrobState);
    int16_t wait = (int16_t)((RU16(gTrobState) & 0x7f8) >> 3);
    if (wait == 0) {
        uint16_t fr = RU16(gTrobState) & 7;
        if (fr == 5) wait = (int16_t)(S16(Rnd(0x3c)) + 0x1e);
        else fr++;
        st = (uint16_t)((st & 0xfff8) | fr);
        mark_bufs_for_rect(JT_ADDR(FN_MarkMove), GH(0x105672));
    } else {
        wait--;
        if (wait <= 0) {
            if (R16(GH(0x102ffe)) == 0) {
                wait = (int16_t)(S16(Rnd(5)) + 1);
                st |= 7;
            } else {
                wait = (int16_t)(S16(Rnd(0x3c)) + 0x1e);
            }
        }
    }
    st = (uint16_t)((st & 0xf807) | (uint16_t)(wait << 3));
    W16(gTrobState, st);
    return 0;
}

#define gCellCol      GH(0x103a08)  /* word: column of the cell being drawn */
#define gCellRow      GH(0x103a0a)  /* word: row of the cell being drawn */
#define gCellRectL    GH(0x103a02)  /* word: left of the cell rect (0x103a00) */
#define gRoomSpecs    GH(0x1037dc)  /* long: spec longs of the current room (cell * 4) */
#define gCrystalCount GH(0x10578e)  /* word: crystals triggered so far */
#define gGargoyleNext GH(0x10578c)  /* word: next gargoyle state */
#define gNoNewTrobs   GH(0x102ffe)  /* word */

/* 0003986a: draw the spider (custom room 0x19): body frame and leg frame images */
uint32_t DrawFinlSpider(void) {
    ENTRY_LINK(FN_DrawFinlSpider, 4, 24);        /* link -4; movem D3-D7/A2 */
    uint32_t it = R32(gCurBFItem);
    if (R16(it + 0xa) != 0xb) return 0;
    uint16_t a, b;
    if ((RU16(it + 0x14) & 0x7f8) == 0) {
        a = 0; b = 0;
    } else {
        uint16_t st = RU16(R32(gCurBFItem) + 0x14);
        W16(a6 - 4, st);
        uint16_t fr = st & 7;
        if (RU16(a6 - 4) & 0x800) { a = 0; b = fr; }
        else { a = fr; b = 0; }
    }
    int16_t savedLeft;
    if (R16(gCellRow) == 2 && R16(gCellCol) == 3) {
        savedLeft = R16(gCellRectL);
        W16(gCellRectL, RU16(GH(0x105674)));
    } else {
        savedLeft = -1;
    }
    DrawCustomRoomImg((int16_t)(R16(GH(0x10565a) + (uint32_t)a * 2) + 3), 0xb);
    DrawCustomRoomImg((int16_t)(R16(GH(0x105666) + (uint32_t)b * 2) + 3), 0xb);
    if (savedLeft != -1) W16(gCellRectL, savedLeft);
    return 0;
}

/* 00039934: room palettes and music when entering rooms 7/8 (gargoyle rooms) */
uint32_t FUN_00039934(void) {
    ENTRY_NOLINK(FN_FUN_00039934, 4);            /* move.l D3 */
    PoPSetPalette(0xdae, 0x40);
    PoPSetPalette(0x3ee, 0xf0);
    PoPSetPalette(0x61b0, 0x20);
    W16(gCrystalCount, 0);
    if (R16(gCurRoom) == 7) {
        W16(gGargoyleNext, 0);
        PoPSetPalette(0xbb8, 0xe0);
        if (R16(GH(0x10300e)) != 0) AddSong(0x106);
    } else if (R16(gCurRoom) == 8) {
        W16(gGargoyleNext, 2);
        PoPSetPalette(0xbb8, 0xe0);
        if (S16(PrinceSndOn(0x106)) == 0 && R16(GS() + 0x26e6) != 0) AddSong(0x106);
    } else {
        AddSong(0x10d);
    }
    return 0;
}

/* 000399e0: animate a crystal (custom room 0x1c). State: 0x1000 | 0x800 (moving) |
 * counter << 4 | crystal index */
uint32_t AnimFinlCrystal(void) {
    ENTRY_LINK(FN_AnimFinlCrystal, 8, 28);       /* link -8; movem D3-D6/A2-A4 */
    uint32_t cr = R32(gCustRoom);
    if (R16(cr) != 0x1c) {
        W16(gTrobDir, -1);
        return 0;
    }
    uint16_t st = RU16(gTrobState);
    if (RU16(gTrobState) & 0x800) {
        uint16_t n = (uint16_t)(((RU16(gTrobState) & 0x7f0) >> 4) + 1);
        if (n <= 5) {
            st = (uint16_t)((st & 0xf80f) | (uint16_t)(n << 4));
            uint32_t d = GH(0x105684) + (uint32_t)(st & 0xf) * 4;   /* {dy, dx} */
            uint32_t piece = cr + 0x17e;
            maddr r = a6 - 8;
            mcopy(r, piece + 0x10, 8);
            _OffsetRect(r, R16(d + 2), R16(d));
            mark_bufs_for_rect(JT_ADDR(FN_MarkMove), r);
        } else {
            st &= 0xf80f;
            int16_t w = (int16_t)(S16(Rnd(0x28)) + 0x28);
            st |= (uint16_t)(w << 4);
            st &= 0xf7ff;
        }
    } else {
        int16_t w = (int16_t)((RU16(gTrobState) & 0x7f0) >> 4);
        st &= 0xf80f;
        w--;
        if (w > 0) {
            st |= (uint16_t)(w << 4);
        } else if (R16(gNoNewTrobs) == 0 || R16(gNumTrobs) == 0) {
            st |= 0x800;
        } else {
            w = (int16_t)(S16(Rnd(0x28)) + 0x28);
            st |= (uint16_t)(w << 4);
        }
    }
    st |= 0x1000;
    W16(gTrobState, st);
    return 0;
}

/* 00039b0e: draw a moving crystal: offset its custom piece, draw it, restore it */
uint32_t DrawFinlCrystal(void) {
    ENTRY_LINK(FN_DrawFinlCrystal, 0x10, 32);    /* link -0x10; movem D3-D7/A2-A4 */
    uint32_t it = R32(gCurBFItem);
    if (R16(it + 0xa) != 1) return 0;
    uint16_t info = RU16(it + 0x14);
    W16(a6 - 4, info);
    if ((info & 0x800) == 0) return 0;
    int16_t n = (int16_t)((int16_t)(RU16(a6 - 4) & 0x7f0) >> 4);
    n = (int16_t)(n - 1);
    if (n < 0) return 0;
    uint16_t shape = (uint16_t)(R16(GH(0x10567a) + (int32_t)n * 2) + 8);
    uint32_t piece = R32(gCustRoom) + 0x1e + (uint32_t)shape * 32;
    mcopy(a6 - 0xc, piece + 0x10, 8);
    W32(a6 - 0x10, R32(piece + 2));
    uint16_t k = RU16(a6 - 4) & 0xf;
    uint32_t dy = GH(0x105684) + (int16_t)(k * 4);
    W16(piece + 2, R16(piece + 2) + R16(dy));
    uint32_t dx = GH(0x105686) + (int16_t)(k * 4);
    W16(piece + 4, R16(piece + 4) + R16(dx));
    _OffsetRect(piece + 0x10, R16(dx), R16(dy));
    uint32_t lay = GH(0x1056a8) + (uint32_t)k * 2;
    W16(piece + 6, RU16(lay));
    DrawCustomRoomImg((int16_t)shape, R16(lay));
    W16(piece + 6, 0xb);
    mcopy(piece + 0x10, a6 - 0xc, 8);
    W32(piece + 2, R32(a6 - 0x10));
    return 0;
}

/* 00039c18: start a crystal at cell `cell` of room `room` */
uint32_t TriggerFinlCrystal(int16_t room, int16_t cell) {
    ENTRY_LINK(FN_TriggerFinlCrystal, 0, 12);    /* link 0; movem D3-D5 */
    if (RU16(gCrystalCount) >= 9) return 0;
    uint16_t st = RU16(gCrystalCount);
    W16(gCrystalCount, R16(gCrystalCount) + 1);
    uint16_t w = (uint16_t)(Rnd(0x28) << 4);
    st |= w;
    st |= 0x1000;
    W16(R32(gRoomSpecs) + (int32_t)cell * 4, st);
    AddTrob(room, cell, 1, 0x29);
    return 0;
}

/* 00039c8c: animate a gargoyle (custom rooms 0x1d/0x1e) */
uint32_t AnimFinlGargoyle(void) {
    ENTRY_LINK(FN_AnimFinlGargoyle, 8, 32);      /* link -8; movem D3-D7/A2-A4 */
    uint32_t cr = R32(gCustRoom);
    int16_t id = R16(cr);
    if (id != 0x1d && id != 0x1e) {
        W16(gTrobDir, -1);
        return 0;
    }
    uint16_t st = RU16(gTrobState);
    uint16_t k = RU16(gTrobState) & 0xf;
    if (!((k < 2 && R16(gCurRoom) == 7) || (k >= 2 && R16(gCurRoom) == 8))) {
        W16(gTrobDir, -1);
        return 0;
    }
    if (RU16(gTrobState) & 0x800) {
        uint16_t n = (uint16_t)(((RU16(gTrobState) & 0x7f0) >> 4) + 1);
        if (n <= 7) {
            st = (uint16_t)((st & 0xf80f) | (uint16_t)(n << 4));
            uint32_t d = GH(0x1056c8) + (uint32_t)k * 4;           /* {dy, dx} */
            uint32_t piece = cr + 0x5e;
            maddr r = a6 - 8;
            mcopy(r, piece + 0x10, 8);
            _OffsetRect(r, R16(d + 2), R16(d));
            mark_bufs_for_rect(JT_ADDR(FN_MarkFRedraw), r);
        } else {
            st &= 0xf80f;
            int16_t w = (int16_t)(S16(Rnd(0x28)) + 0x28);
            st |= (uint16_t)(w << 4);
            st &= 0xf7ff;
        }
    } else {
        int16_t w = (int16_t)((RU16(gTrobState) & 0x7f0) >> 4);
        st &= 0xf80f;
        w--;
        if (w > 0) {
            st |= (uint16_t)(w << 4);
        } else if (R16(gNoNewTrobs) == 0 || R16(gNumTrobs) == 0) {
            st |= 0x800;
        } else {
            w = (int16_t)(S16(Rnd(0x28)) + 0x28);
            st |= (uint16_t)(w << 4);
        }
    }
    st |= 0x1000;
    W16(gTrobState, st);
    return 0;
}

/* 00039de8: draw a moving gargoyle. D0 word: the item layer when it is not 1, 0 when
 * nothing is drawn, 0x10 after drawing. */
uint32_t DrawFinlGargoyle(void) {
    ENTRY_LINK(FN_DrawFinlGargoyle, 0xc, 24);    /* link -0xc; movem D3-D6/A2/A4 */
    uint32_t it = R32(gCurBFItem);
    uint16_t layer = RU16(it + 0xa);
    if (layer != 1) return layer;
    if ((RU16(it + 0x14) & 0x800) == 0) return 0;
    uint16_t info = RU16(it + 0x14);
    W16(a6 - 4, info);
    int16_t n = (int16_t)(((int16_t)(info & 0x7f0) >> 4) - 1);
    if (n < 0) return 0;
    uint16_t k = RU16(a6 - 4) & 0xf;
    uint16_t shape = (uint16_t)(R16(GH(0x1056ba) + (int32_t)n * 2) + 2);
    if (k != 0) shape += 4;
    uint32_t piece = R32(gCustRoom) + 0x1e + (uint32_t)shape * 32;
    mcopy(a6 - 0xc, piece + 0x10, 8);
    W32(piece + 2, R32(GH(0x1056c8) + (uint32_t)k * 4));
    _OffsetRect(piece + 0x10, R16(piece + 4), R16(piece + 2));
    uint32_t lay = GH(0x1056d8) + (uint32_t)k * 2;
    W16(piece + 6, RU16(lay));
    DrawCustomRoomImg((int16_t)shape, R16(lay));
    W16(piece + 6, 0xb);
    mcopy(piece + 0x10, a6 - 0xc, 8);
    W16(piece + 2, 0);
    W16(piece + 4, 0);
    return 0x10;
}

/* 00039eea: start the next gargoyle at cell `cell` of room `room` */
uint32_t TriggerFinlGargoyle(int16_t room, int16_t cell) {
    ENTRY_LINK(FN_TriggerFinlGargoyle, 0, 12);   /* link 0; movem D3-D5 */
    uint16_t st = RU16(gGargoyleNext);
    W16(gGargoyleNext, R16(gGargoyleNext) + 1);
    uint16_t w = (uint16_t)(Rnd(0x28) << 4);
    st |= w;
    st |= 0x1000;
    W16(R32(gRoomSpecs) + (int32_t)cell * 4, st);
    AddTrob(room, cell, 1, 0x2a);
    return 0;
}

/* ---- the mob being processed (16 bytes at 0x103b5a, see docs/notes/seg05b.md) ---- */
#define gMobX         GH(0x103b5a)
#define gMobY         GH(0x103b5c)
#define gMobRoom      GH(0x103b5e)
#define gMobSpeed     GH(0x103b60)  /* -1 = finished */
#define gMobAccel     GH(0x103b62)  /* fireball: x velocity */
#define gMobType      GH(0x103b64)
#define gMobRow       GH(0x103b66)
#define gMobState     GH(0x103b68)  /* fireball: bit 3 = exploding, bits 0-2 = frame */
#define gMobImgW      GH(0x103b00)  /* word: image width of the mob being drawn */
#define gMobImgH      GH(0x103b1c)  /* word: second size word of the mob image */
#define gLeftRoom     GH(0x103040)
#define gRightRoom    GH(0x103042)
#define gCharLeft     GH(0x104a9c)  /* word: left edge of the character image */

/* 00039f58: draw the fireball mob (adds it to the mob display table) */
uint32_t ATMFireball(void) {
    ENTRY_LINK(FN_ATMFireball, 8, 20);           /* link -8; movem D3-D7 */
    int16_t vis = 1;
    W16(a6 - 8, RU16(gMobY));
    if (R16(gMobRoom) == R16(gCurRoom)) {
        int16_t x = R16(gMobX);
        if (x > 0x2cc || x < 0) vis = 0;
    } else if (R16(gMobRoom) == R16(gLeftRoom)) {
        if ((int32_t)R16(gMobX) + (int32_t)R16(gMobImgH) > 0x1fe) {
            W16(gMobX, R16(gMobX) - 0x1fe);
            W16(gMobRoom, RU16(gCurRoom));
        } else {
            vis = 0;
        }
    } else if (R16(gMobRoom) == R16(gRightRoom)) {
        if ((int32_t)R16(gMobX) - (int32_t)R16(gMobImgH) < 0x1fe) {
            W16(gMobX, R16(gMobX) + 0x1fe);
            W16(gMobRoom, RU16(gCurRoom));
        } else {
            vis = 0;
        }
    } else {
        vis = 0;
    }
    if (!vis) return 0;
    int16_t col = (int16_t)(R16(gMobX) / 0x33);
    int16_t row = S16(GetRow(R16(a6 - 8)));
    W16(GH(0x104b38), IndexCell1(col, row));
    int16_t img = S16(GetFireballImg(R16(gMobState)));
    if (GetFlameImgSize(img, a6 - 4) == 0) return 0;
    int16_t savedX = R16(gMobX);
    int16_t h = R16(a6 - 2);
    W16(gMobX, R16(gMobX) - (int16_t)((int32_t)h >> 1));
    int16_t savedY = R16(a6 - 8);
    int16_t w = R16(a6 - 4);
    W16(a6 - 8, R16(a6 - 8) + (int16_t)((int32_t)w >> 1));
    W16(gMobY, RU16(a6 - 8));
    W16(gMobImgW, w);
    W16(gMobImgH, h);
    AddMobObj(R16(a6 - 8));
    MarkMobRedraw(1);
    W16(gMobY, savedY);
    W16(gMobX, savedX);
    return 0;
}

/* 0003a09e: a Jaffar copy (type 6) touching a fireball in its row is hit by it */
uint32_t FUN_0003a09e(void) {
    ENTRY_NOLINK(FN_FUN_0003a09e, 16);           /* movem D3-D5/A3 */
    if (R16(CH(F_TYPE)) != 6) return 0;
    if (R16(CH(F_ALIVE)) >= 0) return 0;
    if (R16(CH(F_SEQ)) == 0xe6) return 0;
    int16_t n = 1;
    uint32_t mob;
    do {
        mob = GetMobTypeItem(0xc, n);
        if (mob != 0 && R16(mob + 0xc) == R16(CH(F_ROW)) && (R16(mob + 0xe) & 8) == 0) {
            int16_t x;
            if (R16(mob + 4) == R16(CH(F_ROOM))) x = 0;
            else if (R16(CH(F_ROOM)) == 8 && LEVEL_TYPE() == 6) x = (int16_t)0xfe02;
            else x = 0x1fe;
            x = (int16_t)(x + (int16_t)(R16(mob) + R16(mob + 8) + 0xbe));
            int16_t x2 = (int16_t)(x + 0x22);
            if (x2 > R16(gCharLeft) && x < R16(gCharRight)) {
                W16(mob + 0xe, 8);
                if (R16(mob + 8) > 0) W16(mob, R16(gCharLeft) - 0xc7);
                else W16(mob, R16(gCharRight) - 0xd7);
                DecLifePts(100);
                JumpSeq(0xe6);
                AnimChar();
                AddSound(0x121);
                W16(gNextRoom, RU16(CH(F_ROOM)));
            }
        }
        n++;
    } while (mob != 0);
    return 0;
}

/* 0003a194: stop the fireball at a wall (explosion) */
uint32_t FUN_0003a194(void) {
    ENTRY_NOLINK(FN_FUN_0003a194, 12);           /* movem D3-D5 */
    int16_t x = (int16_t)(R16(gMobX) - 0x16);
    x = (int16_t)(x + R16(gMobImgH));
    if (R16(gMobAccel) < 0) x = (int16_t)(x - (int16_t)(R16(gMobImgH) << 1));
    int16_t col = (int16_t)(x / 0x33);
    if (x < 0) col--;
    int16_t cell = S16(ReadCell(R16(gMobRoom), col, R16(gMobRow)));
    if (S16(IsWall(cell)) != 0) {
        W16(gMobX, (int16_t)(col * 0x33) + 0x16);
        if (R16(gMobAccel) < 0) W16(gMobX, R16(gMobX) + 0x33);
        W16(gMobState, 8);
        AddSound(0x103);
        PrinceResetSnd(0x109);
    }
    return 0;
}

/* 0003a224: Jaffar (life > 2) is hit by his own fireball: returns sequence 0xf2, or -1 */
uint32_t seg9_03a224(void) {
    ENTRY_NOLINK(FN_seg9_03a224, 4);             /* move.l D3 */
    if (R16(CH(F_LIFE)) <= 2) return (uint16_t)-1;
    int16_t d = S16(seg4_02397e());
    if (d < 0x23) W16(CH(F_X), AddCharX((int16_t)(d - 0x23)));
    DecLifePts(2);
    AddSong(0x109);
    return 0xf2;
}

/* 0003a26e: draw a fireball object (mid item, shape set 2) */
uint32_t DrawObjFireball(void) {
    ENTRY_LINK(FN_DrawObjFireball, 0, 4);        /* link 0; move.l D3 */
    int16_t img = S16(GetFireballImg(R16(GH(0x104b40))));
    if (S16(AddMid(2, (int16_t)(img + 1), R16(GH(0x104b3c)), R16(GH(0x104b3e)), 0x10)) != 0) {
        W16(R32(GH(0x103356)) + (int32_t)R16(GH(0x1037d8)) * 0x18 - 6, 0x30);
        W16(R32(GH(0x103356)) + (int32_t)R16(GH(0x1037d8)) * 0x18 - 4, 0);
    }
    return 0;
}

/* 0003a30c: fireball image for a mob state */
uint32_t GetFireballImg(int16_t st) {
    uint16_t fr = (uint16_t)st & 7;
    if (st & 8) return (uint16_t)(fr + 0x1017);
    return (uint16_t)(fr + 0x1013);
}

/* 0003a350: move / animate the fireball mob */
uint32_t seg9_03a350(void) {
    ENTRY_NOLINK(FN_seg9_03a350, 8);             /* movem D3-D4 */
    if (R16(gMobSpeed) < 0) return 0;
    uint16_t fr = RU16(gMobState) & 7;
    if (RU16(gMobState) & 8) {
        if (fr >= 3) W16(gMobSpeed, -1);
        else fr++;
    } else {
        int16_t v = R16(gMobAccel);
        if (v > 0) {
            if (v < 0x19) W16(gMobAccel, R16(gMobAccel) + 3);
        } else if (R16(gMobAccel) > -0x19) {
            W16(gMobAccel, R16(gMobAccel) - 3);
        }
        W16(gMobX, R16(gMobX) + R16(gMobAccel));
        if (R16(gMobRoom) == 0) {
            W16(gMobSpeed, -1);
            PrinceResetSnd(0x102);
        } else {
            FUN_0003a194();
            if (RU16(gMobState) & 8) {
                fr = 0;
            } else {
                if (fr >= 3) fr = 0;
                else fr++;
                AddSound(0x102);
            }
        }
    }
    if (R16(gMobSpeed) == -1) {
        if (R16(KID(F_LIFE)) <= 2) PoPSetPalette(0x7d0, 0x30);
    } else {
        W16(gMobState, RU16(gMobState) & 0xfff8);
        W16(gMobState, RU16(gMobState) | fr);
    }
    return 0;
}

/* 0003a418: Jaffar's fireball cast (frame 0x119 launches it) */
uint32_t seg9_03a418(void) {
    ENTRY_NOLINK(FN_seg9_03a418, 0);
    if (R16(CH(F_FRAME)) == 0x119) FUN_0003a430();
    AddSound(0x102);
    return 0;
}

/* 0003a430: launch a fireball from the current char */
uint32_t FUN_0003a430(void) {
    ENTRY_NOLINK(FN_FUN_0003a430, 0);
    W16(gMobX, (int16_t)(S16(AddCharX(0x2b)) - 0xcf));
    W16(gMobY, (int16_t)(R16(CH(F_Y)) - 0x37));
    W16(gMobRoom, RU16(CH(F_ROOM)));
    W16(gMobSpeed, 0);
    W16(gMobAccel, (int16_t)(R16(gFaceDx + (int16_t)(R16(CH(F_FACE)) * 2)) * 0xc));
    W16(gMobType, 0xc);
    W16(gMobRow, GetRow(R16(gMobY)));
    W16(gMobState, 2);
    FUN_0003a194();
    FUN_00026a94();
    AddSound(0x102);
    return 0;
}
