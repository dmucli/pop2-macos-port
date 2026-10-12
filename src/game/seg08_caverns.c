/* CODE 8 "Caverns" — level-specific code for the cavern levels (level type 3) and the
 * collapsing bridge of level 5: bridge floors, flying carpet, cave bars, cave piece
 * drawing, darts, lava, squishing gates.
 * Ported from the Ghidra decompilation / disassembly of the original 68k code; addresses
 * in comments are Ghidra addresses (see docs/PORTING.md and docs/notes/seg08.md). */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* ---- game state / rooms ---- */
#define gGameState      GH(0x102fec)   /* long: pointer to the game state block */
#define LEVEL_TYPE()    R16(R32(gGameState) + 0x2186)
#define LEVEL_NUM()     R16(R32(gGameState) + 0x218a)
#define gCurRoom        GH(0x10303e)   /* word */
#define gRoomLeft       GH(0x103040)
#define gRoomRight      GH(0x103042)
#define gRoomAbove      GH(0x103044)
#define gRoomBelow      GH(0x103046)
#define gPlayRect       GH(0x103014)   /* Rect */
#define gRoomTiles      GH(0x1037e0)   /* long: tile words of the current room */
#define gRoomSpecs      GH(0x1037dc)   /* long: spec longs of the current room */
#define gColX           GH(0x103498)   /* word[]: x of each column */
#define gRowTop         GH(0x1034b2)   /* word[]: y of each row */
#define gLevelRsrc      GH(0x1038dc)   /* word[3 per level type]: resource file refnums */
#define pSectRect       GH(0x1060a8)   /* long: address of SectRect (Pascal) */

/* ---- characters (0x3e-byte records) ---- */
#define CH(o)           GH(0x102faa + (o))   /* the current character */
#define KID(o)          GH(0x102f2e + (o))   /* the kid's saved record */
#define OPP(o)          GH(0x102f6c + (o))   /* the opponent */
#define OPPTAB(o)       GH(0x102df8 + (o))   /* the room's character table, entry 0 */
#define gChar           CH(0)
#define gKid            KID(0)
#define gFrameFlags     GH(0x102df6)   /* word: flags of the current frame definition */

/* ---- background/foreground items, objects ---- */
#define gCurBFItem      GH(0x103074)   /* long: BF item being built (0x26 bytes) */
#define gLevelPieces    GH(0x103462)   /* long: the level's piece table */
#define gCustomRoom     GH(0x103362)   /* long: locked custom-room data (pieces at +0x1e, 0x20 bytes) */
#define gBackCount      GH(0x1037d2)   /* word */
#define gFullRedraw     GH(0x1037ce)   /* word */
#define gObjLink        GH(0x104b38)   /* word: FChar kind / object link */
#define gObjX           GH(0x104b3c)
#define gObjY           GH(0x104b3e)
#define gObjShape       GH(0x104b40)

/* ---- trobs and mobs ---- */
#define gTrobCell       GH(0x103c12)
#define gTrobRoom       GH(0x103c14)
#define gTrobPhase      GH(0x103c16)
#define gObjSpec        GH(0x1037e8)   /* word: state word of the current object */
#define gMobX           GH(0x103b5a)
#define gMobY           GH(0x103b5c)
#define gMobRoom        GH(0x103b5e)
#define gMobSpeed       GH(0x103b60)
#define gMobAccel       GH(0x103b62)
#define gMobType        GH(0x103b64)
#define gMobRow         GH(0x103b66)
#define gMobState       GH(0x103b68)
#define gMobRectW       GH(0x103afe)   /* word: piece width + 1 for the mob rect (custom bridge floor) */
#define gMobRectH       GH(0x103b1a)   /* word: piece height + 1 */

/* ---- level 5 bridge ---- */
#define gTension1       GH(0x10495e)   /* words passed to SetTensionLevel (music tension) */
#define gTension2       GH(0x104960)
#define gBrgMarks       GH(0x104ecc)   /* word: bit per bridge column that shakes this frame */
#define gBrgStress      GH(0x104ece)   /* word: frames both characters stood on the bridge */
#define gBrgState       GH(0x104ed0)   /* word: 0 intact, 1..3 breaking, -1 broken; word[3..7] col fallen */
#define gBrgFloorDY     GH(0x104e66)   /* word[]: y offset of each bridge column */
#define gBrgPieceIdx    GH(0x104e78)   /* word[]: custom piece of each falling-floor frame */
#define gBrgFallDY      GH(0x104e94)   /* word[]: y offset per falling-floor frame */
#define gBrgFallDX      GH(0x104eb0)   /* word[]: x offset per falling-floor frame */

/* ---- helpers ---- */

/* The original pushes an extra word (unused by the callee, e.g. -1 for MarkBufsForRect):
 * keep it on the stack around the call so the callee's frame sits where it did. */
#define WITH_WORD(w, call) do { maddr xw_ = emu_sp() - 2; emu_set_sp(xw_); W16(xw_, (w)); call; emu_set_sp(xw_ + 2); } while (0)

/* Pascal SectRect(src1, src2, VAR dst): Boolean through the pointer at pSectRect */
static uint8_t sect_rect_ptr(maddr src1, maddr src2, maddr dst) {
    maddr sp0 = emu_sp();
    push16(0);
    push32(src1);
    push32(src2);
    push32(dst);
    call68_addr(R32(pSectRect));      /* the callee pops its arguments */
    uint8_t r = R8(sp0 - 2);
    emu_set_sp(sp0);
    return r;
}

/* copy the current character record to the kid's saved record (0x3e bytes) */
static void save_kid(void) { mcopy(gKid, gChar, 0x3e); }

/* address of custom piece `idx` */
static maddr custom_piece(int16_t idx) {
    return R32(gCustomRoom) + 0x1e + (uint32_t)(uint16_t)idx * 0x20;
}

/* ======================================================================== */

/* 00033fc4: per-frame bridge handling (level 5) */
uint32_t FUN_00033fc4(void) {
    ENTRY_NOLINK(FN_FUN_00033fc4, 8);            /* movem D3/D4 */
    int16_t kidOn = S16(FUN_000341a2());
    int16_t oppOn = S16(FUN_00034212());
    MarkBrgFloorsForChars(kidOn, oppOn);
    BridgeCheckBreak(kidOn, oppOn);
    return 0;
}

/* 00033fee: the bridge breaks after both characters stood on it long enough */
uint32_t BridgeCheckBreak(int16_t kidOn, int16_t oppOn) {
    ENTRY_LINK(FN_BridgeCheckBreak, 0, 12);      /* link 0; movem D3-D5 */
    uint32_t d0;
    if (R16(gBrgState) == 0) {
        if (kidOn == 0 || oppOn == 0) {
            d0 = 0;
            if (R16(gBrgStress) > 1) W16(gBrgStress, R16(gBrgStress) - 2);
            return d0;
        }
        int16_t d3 = R16(KID(0xc));
        int16_t d5 = R16(OPPTAB(0xc));
        int32_t d4 = (int32_t)d3 - (int32_t)d5;
        if (d4 < 0) d4 = -d4;
        if (d4 > 3) {
            d0 = (uint16_t)R16(gBrgStress);
            if (R16(gBrgStress) != 0) W16(gBrgStress, R16(gBrgStress) - 1);
            return d0;
        }
        W16(gBrgStress, R16(gBrgStress) + 1);
        if (R16(gBrgStress) < 0x3c) return 3;
        if (d3 < 4) return 4;
        if (d3 > 6) return 4;
        if (d5 < 5) return 5;
        if (d5 > 6) return 5;
        if (R16(KID(0x28)) == 0x55) return 5;
        W16(gBrgState, 1);
        return 1;
    }
    int16_t d3 = R16(gBrgState);
    if (d3 <= 0) return (uint16_t)d3;
    if (d3 != 3) {
        W16(gBrgState, R16(gBrgState) + 1);
        return 3;
    }
    uint16_t d4 = 7;
    d0 = 0;
    do {
        d3 = (int16_t)d4;
        d0 = (uint32_t)((int32_t)d3 * 2);
        if (R16(gBrgState + (int16_t)d0) == 0) {
            d3 = (int16_t)(d3 + 10);
            d0 = TriggerBrgFloorMob(10, d3);
            break;
        }
        d4--;
    } while (d4 >= 3);
    if (d4 == 7) {
        AddSound(0x7b);
        d0 = AddSong(0x79);
    }
    if (d4 < 3 || R16(KID(0x28)) == 0x55) {
        W16(gBrgState, -1);
        FUN_000225d2();
        if (R16(CH(0xc)) <= 4 && R16(CH(2)) == 0) W16(CH(4), AddCharX(0x11));
        save_kid();
        d0 = 0xffff;
    }
    return d0;
}

/* 00034148: column of a character on the bridge, counted across rooms 7 / 10 / 12 of level 5.
 * The original adjusts its own column argument slot (glue_BridgeGetCharCol writes it back). */
static int16_t s_bgcc_col;
uint32_t BridgeGetCharCol(int16_t room, int16_t col) {
    if (LEVEL_NUM() == 5 && room == 7) col = (int16_t)(col - 10);
    else if (LEVEL_NUM() == 5 && room == 0xc) col = (int16_t)(col + 10);
    s_bgcc_col = col;
    return (uint16_t)col;
}
void glue_BridgeGetCharCol(void) {
    maddr sp = emu_sp();
    uint32_t r = BridgeGetCharCol((int16_t)rd16(sp), (int16_t)rd16(sp + 2));
    wr16(sp + 2, (uint16_t)s_bgcc_col);
    emu_set_d0((cpu_get(R_D0) & 0xffff0000u) | (r & 0xffff));
}

/* 000341a2: is the kid on a bridge floor? (opens the gate behind him in room 7) */
uint32_t FUN_000341a2(void) {
    ENTRY_NOLINK(FN_FUN_000341a2, 8);            /* movem D3/D4 */
    FUN_000225d2();
    int16_t d4 = S16(FUN_000344a6());
    if (d4 != 0) {
        FUN_000345fe();
        int16_t d3 = R16(CH(0xa));
        if (d3 == 0xf || d3 == 0xb9) d4 = 0;
    } else if (R16(CH(0x16)) == 7 && R16(CH(0x1a)) != -1 && S16(IsBrgGateOpen()) != 0 &&
               R16(CH(0xc)) <= 5) {
        TriggerGateWithRevPlate(7, 0xd);
    }
    save_kid();
    return (uint16_t)d4;
}

/* 00034212: is the room's first opponent on a bridge floor? */
uint32_t FUN_00034212(void) {
    ENTRY_NOLINK(FN_FUN_00034212, 4);            /* move.l D3 */
    if (R16(gCurRoom) == 0) return 0;
    uint32_t info = (uint32_t)RU16(gCurRoom) * 0xc0 + R32(gGameState) + 0x21a6;
    if (R16(info - 0xc0) == 0) return 0;
    LoadShad(0);
    int16_t d3 = S16(FUN_000344a6());
    if (d3 != 0) FUN_000345fe();
    seg4_022516();
    return (uint16_t)d3;
}

/* 0003426e: may the opponent (guard on the bridge) engage? */
uint32_t seg8_03426e(void) {
    ENTRY_NOLINK(FN_seg8_03426e, 16);            /* movem D3-D6 */
    int16_t d4 = S16(BridgeGetCharCol(R16(CH(0x16)), R16(CH(0xc))));
    if (R16(CH(2)) != 0) return d4 >= 8 ? 1 : 0;
    if (d4 < 2) return 1;
    uint32_t d0 = FUN_000343e4();
    if ((int16_t)d0 != 0) return d0;
    if (R16(OPP(0x1a)) == 1) {
        int32_t d6 = (int32_t)R16(CH(4)) - (int32_t)R16(OPP(4));
        if (d6 < 0 && d6 > -0xcc && R16(OPP(0xc)) <= 6 && R16(gTension1) == 2 &&
            R16(gTension2) >= 0x14)
            return 1;
    }
    return 0;
}

/* 000342fc: bridge guard behaviour; returns 1 to let the normal AI run */
uint32_t seg8_0342fc(void) {
    ENTRY_NOLINK(FN_seg8_0342fc, 16);            /* movem D3-D6 */
    int16_t d6 = S16(BridgeGetCharCol(R16(CH(0x16)), R16(CH(0xc))));
    int16_t d3 = R16(CH(0x10));
    if (d3 == 5) return 1;
    if (d3 == 4) {
        if (R16(CH(0xe)) >= 3 || LEVEL_NUM() != 5) { FUN_000223c8(); return 0; }
        d3 = R16(CH(0x16));
        if (d3 != 10 && d3 != 7 && d3 != 0xc) { FUN_000223c8(); return 0; }
    }
    if (R16(CH(2)) == 0) {
        d3 = R16(CH(0x28));
        if (d3 == 0x66 || d3 == 0xd8) {
            if (d6 >= 9 || S16(IsBrgGateOpen()) == 0) {
                W16(CH(0x32), 2);
                W16(CH(0x1a), 1);
                JumpSeq(0x65);
            }
            return 0;
        }
        if (R16(KID(4)) >= R16(CH(4))) return 1;
        if ((int16_t)FUN_000343e4() != 0) { FUN_0001dbee(); return 0; }
        if (d6 < 9) return 1;
        seg6_02cc78();
        return 0;
    }
    if ((int16_t)FUN_000343e4() == 0) return 1;
    W16(CH(2), 0);
    W16(CH(4), AddCharX(10));
    return 0;
}

/* 000343e4: is the opponent behind the open gate (bridge column < 2)? */
uint32_t FUN_000343e4(void) {
    ENTRY_NOLINK(FN_FUN_000343e4, 8);            /* movem D3/D4 */
    int16_t d4 = S16(BridgeGetCharCol(R16(OPP(0x16)), R16(OPP(0xc))));
    uint32_t d0 = IsBrgGateOpen();
    if ((int16_t)d0 == 0) return d0 & 0xffff;
    return d4 < 2 ? 1 : 0;
}

/* 00034418: is the gate at room 7 cell 13 open (no trob, or trob phase 0 or 2)? */
uint32_t IsBrgGateOpen(void) {
    ENTRY_LINK(FN_IsBrgGateOpen, 4, 8);          /* link -4; movem D3/A2 */
    maddr t = GetTrobRoomCell(7, 0xd);
    if (t == 0) return 0;
    int16_t d3 = R16(t + 4);
    if (d3 == 0 || d3 == 2) return 1;
    return 0;
}

/* 00034462: is the character close enough to the bridge end it faces? */
uint32_t FUN_00034462(void) {
    ENTRY_NOLINK(FN_FUN_00034462, 8);            /* movem D3/D4 */
    int16_t d4 = S16(BridgeGetCharCol(R16(CH(0x16)), R16(CH(0xc))));
    if (R16(CH(2)) == 0) return d4 < 3 ? 1 : 0;
    return d4 > 3 ? 1 : 0;
}

/* 000344a6: is the character standing on a bridge floor (piece 0x2c)? */
uint32_t FUN_000344a6(void) {
    ENTRY_NOLINK(FN_FUN_000344a6, 4);            /* move.l D3 */
    int16_t d3 = S16(ReadCell(R16(CH(0x16)), R16(CH(0xc)), R16(CH(0xe))));
    return d3 == 0x2c ? 1 : 0;
}

/* 000344ce: reset the bridge shaking state on room entry */
uint32_t seg8_0344ce(void) {
    ENTRY_NOLINK(FN_seg8_0344ce, 0);
    W16(gBrgMarks, 0);
    W16(gBrgStress, 0);
    if (R16(gBrgState) == 0) return 0;
    return SetTensionLevel(R16(gTension1), (int16_t)-R16(gTension2));
}

/* 000344f4: mark the bridge columns under the characters as shaking; creak sound */
uint32_t MarkBrgFloorsForChars(int16_t kidOn, int16_t oppOn) {
    ENTRY_LINK(FN_MarkBrgFloorsForChars, 0, 4);  /* link 0; move.l D3 */
    W16(gBrgMarks, 0);
    int16_t d3 = 2;
    if (kidOn != 0 && oppOn != 0) {
        W16(gBrgMarks, 0xff);
        d3 = 1;
    } else if (kidOn != 0) {
        MarkBridgeForCol(R16(KID(0xc)));
    } else if (oppOn != 0) {
        MarkBridgeForCol(R16(OPPTAB(0xc)));
    } else {
        d3 = 0;
    }
    if (d3 != 0 && S16(PrinceSndOn(0x7a)) == 0 && S16(Rnd((int16_t)(d3 * 0x28))) == 0)
        AddSound(0x7a);
    return 0;
}

/* 0003458c: mark columns col-1..col+1 (clipped to 0..7) as shaking.
 * The loop variable lives in the argument slot (glue_MarkBridgeForCol writes it back). */
static int16_t s_mbfc_slot;
uint32_t MarkBridgeForCol(int16_t col) {
    int32_t d3 = (int32_t)col;
    int32_t d4 = d3 - 1 >= 1 ? d3 - 1 : 1;
    int16_t d5 = (int16_t)((int16_t)d4 - 1);
    d3 = (int32_t)col + 1;
    d4 = d3 <= 8 ? d3 : 8;
    int16_t d6 = (int16_t)((int16_t)d4 - 1);
    int16_t i = d5;
    while (i <= d6) {
        uint32_t sh = (uint32_t)i & 63;
        W16(gBrgMarks, RU16(gBrgMarks) | (uint16_t)(sh < 16 ? 1u << sh : 0));
        i++;
    }
    s_mbfc_slot = i;
    return 0;
}
void glue_MarkBridgeForCol(void) {
    maddr sp = emu_sp();
    MarkBridgeForCol((int16_t)rd16(sp));
    wr16(sp, (uint16_t)s_mbfc_slot);
    emu_set_d0(0);
}

/* 000345fe: put the character's feet on the (shaking) bridge floor */
uint32_t FUN_000345fe(void) {
    ENTRY_NOLINK(FN_FUN_000345fe, 8);            /* movem D3/D4 */
    if (S16(FUN_000344a6()) == 0) return 0;
    uint16_t d3 = RU16(CH(0xa));
    if (d3 >= 0x22 && d3 <= 0x2c) return 0;
    W16(CH(6), (int16_t)(R16(CH(0xe)) * 0x78 + 0x6a));
    W16(CH(6), R16(CH(6)) + R16(gBrgFloorDY + (int16_t)(R16(CH(0xc)) * 2)));
    seg4_02055e();
    if ((RU16(gFrameFlags) & 0x40) == 0) return 0;
    int16_t c = (int16_t)(R16(CH(0xc)) + 10);
    int16_t d4 = (int16_t)(R16(R32(gGameState) + 0xbb8 + (int32_t)c * 4) & 0xf);
    if (d4 == 1) W16(CH(6), R16(CH(6)) - 1);
    else if (d4 == 2) W16(CH(6), R16(CH(6)) + 1);
    return 0;
}

/* 00034688 */
uint32_t seg8_034688(void) {
    ENTRY_NOLINK(FN_seg8_034688, 0);
    PoPSetPalette(0xdac, 0x40);
    return 0;
}

/* 00034698: trob animation of a bridge floor: shake it while it is marked */
uint32_t seg8_034698(void) {
    ENTRY_NOLINK(FN_seg8_034698, 12);            /* movem D3-D5 */
    if (LEVEL_NUM() != 5 || R16(gCurRoom) != 10) {
        W16(gTrobPhase, -1);
        return 0;
    }
    int16_t d3 = (int16_t)(R16(gTrobCell) % 10);
    int16_t d4 = (int16_t)(R16(gObjSpec) & 0xf);
    W16(gObjSpec, RU16(gObjSpec) & 0xfff0);
    uint32_t sh = (uint32_t)((int32_t)d3 - 1) & 63;
    uint32_t bit = sh < 32 ? 1u << sh : 0;
    int16_t d5;
    if ((bit & (uint32_t)RU16(gBrgMarks)) == 0) {
        d5 = 0;
    } else {
        d5 = S16(Rnd(3));
        if ((uint16_t)d5 <= 1) d5 = d4;
        else if (d4 == 0) { d5--; W16(gObjSpec, RU16(gObjSpec) | (uint16_t)d5); }
        else d5 = 0;
    }
    if ((d5 | d4) != 0) RedrawBrgFloor(d5, R16(gTrobCell));
    return 0;
}

/* 00034732: piece proc: draw a custom bridge-floor piece at its shaken y */
uint32_t seg8_034732(void) {
    ENTRY_NOLINK(FN_seg8_034732, 16);            /* movem D3-D5/A2 */
    maddr item = R32(gCurBFItem);
    int16_t d3 = R16(item + 0xa);
    if (d3 != 5 && d3 != 2) return 0;
    int16_t d5 = (int16_t)((R16(item + 0x14) & 0xf) + 3);
    int16_t d4 = S16(OffsetBrgFloorY(d5, R16(item + 0xe)));
    DrawCustomRoomImgAtInfo(d5);
    RestoreBrgFloorY(d5, d4);
    return 0;
}

/* 0003477c: move custom piece idx down by the shake offset of column col; returns old y */
uint32_t OffsetBrgFloorY(int16_t idx, int16_t col) {
    ENTRY_LINK(FN_OffsetBrgFloorY, 0, 12);       /* link 0; movem D3/D4/A2 */
    maddr p = custom_piece(idx);
    int16_t d3 = R16(p + 2);
    int16_t d4 = R16(gBrgFloorDY + (int16_t)(col * 2));
    W16(p + 2, R16(p + 2) + d4);
    _OffsetRect(p + 0x10, 0, d4);
    return (uint16_t)d3;
}

/* 000347de: put custom piece idx back at y */
uint32_t RestoreBrgFloorY(int16_t idx, int16_t y) {
    ENTRY_LINK(FN_RestoreBrgFloorY, 0, 8);       /* link 0; movem D3/A2 */
    maddr p = custom_piece(idx);
    int16_t d3 = (int16_t)(R16(p + 2) - y);
    W16(p + 2, y);
    _OffsetRect(p + 0x10, 0, (int16_t)-d3);
    return 0;
}

/* 00034838: mark the rectangle of the bridge floor at cell (frame = shake frame) for redraw */
uint32_t RedrawBrgFloor(int16_t frame, int16_t cell) {
    ENTRY_LINK(FN_RedrawBrgFloor, 8, 20);        /* link -8; movem D3/D5-D7/A2 */
    int16_t d6 = (int16_t)(frame + 3);
    int16_t d7 = (int16_t)(cell % 10);
    int16_t d5 = S16(OffsetBrgFloorY(d6, d7));
    maddr p = custom_piece(d6);
    maddr rect = a6 - 8;
    ComputeOffsetCellRect(rect, p + 0x10, d7, (int16_t)(cell / 10 + 1));
    W16(rect, R16(rect) - 1);
    WITH_WORD(-1, MarkBufsForRect(JT_ADDR(FN_MarkMove), rect));
    RestoreBrgFloorY(d6, d5);
    return 0;
}

/* 000348dc: trigger handler of a bridge floor */
uint32_t TriggerBrgFloor(int16_t room, int16_t cell) {
    ENTRY_LINK(FN_TriggerBrgFloor, 0, 16);       /* link 0; movem D3-D5/A2 */
    maddr spec = (maddr)((int32_t)cell * 4 + R32(gRoomSpecs));
    int16_t d5 = (int16_t)(R16(spec) & 0xfff0);
    W16(spec, d5);
    AddTrob(room, cell, 1, 0x2c);
    RedrawBrgFloor(d5, cell);
    return 0;
}

/* 00034936: ATM (draw) routine of a falling bridge-floor mob */
uint32_t ATMBrgFloor(void) {
    ENTRY_LINK(FN_ATMBrgFloor, 0x18, 24);        /* link -0x18; movem D3-D7/A2 */
    int16_t d7 = R16(gMobX);
    int16_t y = R16(gMobY);                      /* -0x16(A6) */
    int16_t d5 = R16(gMobRoom);
    int16_t room = R16(gMobRoom);
    int16_t d3 = R16(gCurRoom);
    if (room == d3) {
    } else if (room == R16(gRoomBelow)) {
        y = (int16_t)(y + 0x16c);
        d5 = d3;
    } else if (R16(gMobRoom) == R16(gRoomAbove)) {
        y = (int16_t)(y - 0x16c);
        d5 = R16(gCurRoom);
    } else if (R16(gMobRoom) == R16(gRoomLeft)) {
        d7 = (int16_t)(R16(gMobX) - 0x1fe);
        d5 = R16(gCurRoom);
    } else if (R16(gMobRoom) == R16(gRoomRight)) {
        d7 = (int16_t)(R16(gMobX) + 0x1fe);
        d5 = R16(gCurRoom);
    }
    int16_t d6 = (int16_t)(d7 / 0x33);
    d3 = (int16_t)(y / 0x78);
    int16_t c = S16(IndexCell1(d6, d3));
    W16(gObjLink, c);
    if (c != 0x1e) {
        if (d6 != 0) W16(gObjLink, R16(gObjLink) - 1);
        if (R16(gObjLink) < 0x14) W16(gObjLink, R16(gObjLink) + 10);
    }
    FirstInRow(R16(gMobRow));
    int16_t fr = (int16_t)(R16(gMobState) & 0xf);
    int32_t d4 = (int32_t)fr * 2;
    int16_t piece = R16(gBrgPieceIdx + (int16_t)d4);
    W16(a6 - 6, piece);
    if (LEVEL_NUM() != 5 || d5 != 10) return 0;
    d5 = R16(gMobY);
    maddr dy = gBrgFallDY + (int16_t)d4;
    W16(gMobY, R16(gMobY) + R16(dy));
    y = (int16_t)(y + R16(dy));
    W16(a6 - 8, R16(gMobX));
    W16(gMobX, R16(gMobX) + R16(gBrgFallDX + (int16_t)d4));
    maddr size = a6 - 0xc;
    if (GetCustomPieceSize(piece, size) != 0) {
        W16(gMobRectW, R16(size) + 1);
        W16(gMobRectH, R16(size + 2) + 1);
    }
    MarkMobRedraw(0);
    if (R16(gMobSpeed) != -1) {
        maddr rect = a6 - 0x14;
        ComputeMobRect(rect);
        W16(a6 - 0x12, R16(gColX + 2 + (int16_t)(d6 * 2)));
        if (sect_rect_ptr(gPlayRect, rect, rect) != 0) {
            WITH_WORD(-1, MarkBufsForRect(JT_ADDR(FN_MarkFloor), rect));
            AddMobObj(y);
        }
    }
    W16(gMobX, R16(a6 - 8));
    W16(gMobY, d5);
    return 0;
}

/* 00034b04: DrawObj routine of a bridge floor object */
uint32_t DrawObjBrgFloor(void) {
    ENTRY_LINK(FN_DrawObjBrgFloor, 0, 8);        /* link 0; movem D3/D4 */
    uint16_t d4 = RU16(gObjShape) & 0xf;
    int16_t d3 = R16(gBrgPieceIdx + (uint32_t)d4 * 2);
    AddMid(4, (int16_t)(d3 + 1), R16(gObjX), (int16_t)(R16(gObjY) - 1), 0x10);
    return 0;
}

/* 00034b62: mob routine of a falling bridge floor: advance its frame, stop at the end */
uint32_t FUN_00034b62(void) {
    if (LEVEL_NUM() != 5 || R16(gCurRoom) != 10) {
        W16(gMobSpeed, -1);
        return 0;
    }
    int16_t d5 = (int16_t)(R16(gMobState) & 0xf);
    int16_t d6 = (int16_t)((uint32_t)(uint16_t)(R16(gMobState) & 0x30) >> 4);
    if ((d6 == 1 && d5 != 7) || (d6 == 2 && d5 != 0xd)) {
        d5++;
        W16(gMobState, RU16(gMobState) & 0xfff0);
        W16(gMobState, RU16(gMobState) | (uint16_t)d5);
    } else if (d6 == 2 && d5 == 0xd) {
        W16(gMobSpeed, -1);
    }
    return 0;
}

/* 00034bda: make the bridge floor at cell of room fall (start a type 11 mob) */
uint32_t TriggerBrgFloorMob(int16_t room, int16_t cell) {
    ENTRY_LINK(FN_TriggerBrgFloorMob, 8, 20);    /* link -8; movem D3-D7 */
    int32_t c = (int32_t)cell;
    W16(R32(gGameState) + 0x21c + c * 2, 0);
    W16(R32(gGameState) + 0xbb8 + c * 4, 0xc000);
    W16(gMobAccel, 0);
    W16(gMobSpeed, 0);
    int16_t d6 = (int16_t)(cell % 10);
    int16_t d7 = (int16_t)(cell / 10);
    W16(gMobX, R16(gColX + (int16_t)(d6 * 2)));
    int16_t kind;
    if (S16(Rnd(1)) != 0) { W16(gMobState, 8); kind = 2; }
    else { W16(gMobState, 0); kind = 1; }
    W16(a6 - 8, kind);
    W16(gBrgState + (int16_t)(d6 * 2), kind);
    W16(gMobState, RU16(gMobState) | (uint16_t)(kind << 4));
    W16(gMobRoom, room);
    W16(gMobRow, d7);
    W16(gMobY, R16(gRowTop + (int16_t)(d7 * 2)));
    W16(gMobY, R16(gMobY) + R16(gBrgFloorDY + (int16_t)(d6 * 2)));
    W16(gMobType, 0xb);
    FUN_00026a94();
    MarkMobRedraw(0);
    return 0;
}

/* 00034cf0: level 5 start: carpet palettes, carpet data, carpet music */
uint32_t FUN_00034cf0(void) {
    ENTRY_NOLINK(FN_FUN_00034cf0, 0);
    PoPSetPalette(0x62d7, 0x20);
    PoPSetPalette(0xdad, 0x40);
    FUN_00035238();
    AddSong(0x72);
    return 0;
}

/* 00034d1c: level 5 end: free the carpet, restore the palettes */
uint32_t seg8_034d1c(void) {
    ENTRY_NOLINK(FN_seg8_034d1c, 0);
    FUN_000354b0();
    PoPSetPalette(0xdac, 0x40);
    PoPSetPalette(0x2ee, 0x20);
    return 0;
}

/* ---- flying carpet (level 5) ----
 * gCarpet (0x104f3c) is a 6-byte handle: +0 frame counter (0 = not started), +2 flying flag,
 * +4 save-under peel pending. Carpet shapes are 0x62d7 + n in the level type's resource file. */
#define gCarpet         GH(0x104f3c)   /* long: handle */
#define gCarpetY        GH(0x104ee2)   /* word: bottom of the carpet when parked */
#define gCarpetX        GH(0x104ee4)   /* word */
#define gCarpetFrames   GH(0x104ee6)   /* word[]: carpet shape per animation frame */
#define gCarpetDY       GH(0x104efc)   /* {dy, dx} per frame */
#define gCarpetDX       GH(0x104efe)
#define gBloodRGB2      GH(0x104f30)   /* RGBColor saved for palette entry 0x2d */
#define gBloodRGB1      GH(0x104f36)   /* RGBColor saved for palette entry 0x2e */
#define gMidTable       GH(0x103356)   /* long: mid item table (0x18-byte items) */
#define gNumMid         GH(0x1037d8)
#define gObjFace        GH(0x104b3a)
#define gObjRect        GH(0x104b44)   /* Rect */
#define gMainWindow     GH(0x102dbe)
#define gReadCellIdx    GH(0x104b54)
#define gReadCellRoom   GH(0x104b56)

/* 00034d3e: load carpet shape n (0x62d7 + n) from the level resource file, non-purgeable */
uint32_t CheckCarpetImg(int16_t n) {
    ENTRY_LINK(FN_CheckCarpetImg, 0, 8);         /* link 0; movem D3/A2 */
    if (R32(gCarpet) == 0) return 0;
    int16_t d3 = (int16_t)_CurResFile();
    _UseResFile(R16(gLevelRsrc + (int16_t)((uint32_t)RU16(R32(gGameState) + 0x2186) * 6)));
    maddr h = GetShape((int16_t)(n + 0x62d7));
    _HNoPurge(h);
    _UseResFile(d3);
    return h;
}

/* 00034db6: one RGB component faded from a towards b in 8 steps: a + (b - a) / 8 * step */
uint32_t ComputeBloodRGB(int16_t a, int16_t b, int16_t step) {
    uint32_t d3 = (uint16_t)a;
    int32_t d5 = (int32_t)((uint32_t)(uint16_t)b - d3);
    d5 = d5 / 8;
    d5 = (int32_t)((uint32_t)d5 * (uint32_t)(uint16_t)step + d3);
    return (uint32_t)d5;
}

/* 00034e08: piece proc: draw the parked carpet as a back item */
uint32_t DrawCarpet(void) {
    ENTRY_LINK(FN_DrawCarpet, 4, 16);            /* link -4; movem D3/A2-A4 */
    if (R32(gCarpet) == 0) return 0;
    _HLock(R32(gCarpet));
    maddr p = R32(R32(gCarpet));
    W32(a6 - 4, p);
    if (R16(p) == 0 && R16(R32(gCurBFItem) + 0xa) == 0 && R16(gFullRedraw) != 0) {
        maddr a4 = R32(GetCarpetShape(9));
        W16(R32(gCurBFItem) + 0x1c, R16(gCarpetY) - R16(a4 + 6));
        W16(R32(gCurBFItem) + 0x1a, R16(gCarpetX));
        W16(R32(gCurBFItem) + 0x18, 0x62df);
        maddr item = R32(gCurBFItem);
        MakeShapeRect(item + 0x1e, R16(item + 0x1c), R16(item + 0x1a), a4);
        item = R32(gCurBFItem) + 0x1e;
        if (sect_rect_ptr(gPlayRect, item, item) != 0) {
            W16(gBackCount, R16(gBackCount) + 1);
            W32(gCurBFItem, 0);
        }
    }
    return _HUnlock(R32(gCarpet));
}

/* 00034ece: save what is under the parked carpet before it first moves */
uint32_t SaveUnderCarpet(void) {
    ENTRY_LINK(FN_SaveUnderCarpet, 8, 16);       /* link -8; movem D3/A2-A4 */
    if (R32(gCarpet) == 0) return 0;
    maddr p = R32(R32(gCarpet));
    if (R16(p + 4) != 0) return 0;
    W16(p + 4, 1);
    maddr a4 = R32(GetCarpetShape(9));
    maddr r = a6 - 8;
    _SetRect(r, R16(gCarpetX), (int16_t)(R16(gCarpetY) - R16(a4 + 6)),
             (int16_t)(R16(gCarpetX) + R16(a4 + 4)), R16(gCarpetY));
    AddPeel(r, 0x65, 2);
    return 0;
}

/* 00034f4c: DrawObj routine of the flying carpet; also fades the blood colours */
uint32_t DrawObjCarpet(void) {
    ENTRY_LINK(FN_DrawObjCarpet, 0x10, 28);      /* link -0x10; movem D3-D6/A2-A4 */
    if (R32(gCarpet) == 0) return 0;
    _HLock(R32(gCarpet));
    maddr p = R32(R32(gCarpet));
    W32(a6 - 8, p);
    if (R16(p) != 0) {
        int16_t d5 = R16(gNumMid);
        W16((uint32_t)(int32_t)R16(gNumMid) * 0x18 + R32(gMidTable), R16(gObjX));
        int16_t d4 = S16(FUN_000351a4());
        maddr a2 = gCarpetFrames + (int16_t)(d4 * 2);
        maddr a4 = R32(GetCarpetShape(R16(a2)));
        int16_t y = (int16_t)(R16(gObjY) - R16(a4 + 6));
        uint32_t off = (uint32_t)(int32_t)d5 * 0x18;
        W32(a6 - 4, off);
        W16(off + R32(gMidTable) + 2, y);
        W16(off + R32(gMidTable) + 4, 4);
        W16(off + R32(gMidTable) + 6, R16(a2) + 0x62d8);
        mcopy(off + R32(gMidTable) + 8, gObjRect, 8);
        W16(off + R32(gMidTable) + 0x10, 0x10);
        W16(off + R32(gMidTable) + 0x12, 0);
        W16(off + R32(gMidTable) + 0x14, 0);
        W16(off + R32(gMidTable) + 0x16, -1);
        p = R32(a6 - 8);
        if (R16(p + 2) != 0) {
            int16_t d6 = (int16_t)(R16(p) - 4);
            if (d6 != 0 && (uint16_t)d6 < 8) {
                maddr rgb = a6 - 0x10;
                GetNthvRGB(0x2e << 8, rgb);
                if (d6 == 1) mcopy(gBloodRGB1, rgb, 6);
                W16(rgb, ComputeBloodRGB(R16(gBloodRGB1), 0x7272, d6));
                W16(rgb + 2, ComputeBloodRGB(R16(gBloodRGB1 + 2), 0, d6));
                W16(rgb + 4, ComputeBloodRGB(R16(gBloodRGB1 + 4), 0, d6));
                SetAndShowNthRGB(0x2e << 8, rgb, R32(gMainWindow));
                GetNthvRGB(0x2d << 8, rgb);
                if (d6 == 1) mcopy(gBloodRGB2, rgb, 6);
                W16(rgb, ComputeBloodRGB(R16(gBloodRGB2), 0x4bc2, d6));
                W16(rgb + 2, ComputeBloodRGB(R16(gBloodRGB2 + 2), 0, d6));
                W16(rgb + 4, ComputeBloodRGB(R16(gBloodRGB2 + 4), 0, d6));
                SetAndShowNthRGB(0x2d << 8, rgb, R32(gMainWindow));
            }
        }
        W16(gNumMid, R16(gNumMid) + 1);
        p = R32(a6 - 8);
        W16(p, R16(p) + 1);
        if (R16(p + 4) != 0) {
            RestoreKeepCustomPeel(1);
            W16(p + 4, 0);
        }
    }
    _HUnlock(R32(gCarpet));
    return 0;
}

/* 000351a4: current carpet animation frame */
uint32_t FUN_000351a4(void) {
    if (R32(gCarpet) == 0) return cpu_get(R_D5);  /* D5 unset in the original (callers check first) */
    maddr p = R32(R32(gCarpet));
    int16_t d5;
    if (R16(p + 2) == 0) {
        int16_t d3 = R16(p);
        if (d3 >= 9) d5 = (int16_t)(d3 % 6 + 3);
        else d5 = (int16_t)(R16(p) % 9);
    } else {
        int32_t d3 = (int32_t)R16(p) + 8;
        if (d3 >= 10) d3 = 10;
        d5 = (int16_t)d3;
    }
    return (uint16_t)d5;
}

/* 00035200: handle of carpet shape n (NULL when there is no carpet) */
uint32_t GetCarpetShape(int16_t n) {
    ENTRY_LINK(FN_GetCarpetShape, 0, 4);         /* link 0; move.l A2 */
    if (R32(gCarpet) == 0) return 0;
    return CheckCarpetImg(n);
}

/* 00035238: allocate the carpet record and preload its 16 shapes */
uint32_t FUN_00035238(void) {
    ENTRY_NOLINK(FN_FUN_00035238, 8);            /* movem D3/A2 */
    maddr h = PoPNewHandle(6);
    W32(gCarpet, h);
    maddr p = R32(h);
    tc_memset(p, 0, 6);
    uint16_t d3 = 0;
    do {
        CheckCarpetImg((int16_t)d3);
        d3++;
    } while (d3 < 0x10);
    return 0;
}

/* 00035276: set up the carpet character object (FChar globals) */
uint32_t SetupCarpet(void) {
    ENTRY_LINK(FN_SetupCarpet, 4, 20);           /* link -4; movem D3-D5/A2/A3 */
    if (R32(gCarpet) == 0) return 0;
    if (R16(R32(R32(gCarpet))) == 0) return 0;
    seg4_022442();
    int16_t d5 = S16(FUN_000351a4());
    int32_t i = (int32_t)d5;
    W32(a6 - 4, i);
    W16(gObjShape, R16(gCarpetFrames + (int16_t)(i * 2)) + 0x62d7);
    int16_t d3 = (int16_t)(i * 4);
    W16(gObjY, R16(gObjY) + R16(gCarpetDY + d3));
    W16(gObjX, R16(gObjX) + R16(gCarpetDX + d3));
    if (R16(gObjFace) == 0) W16(gObjX, R16(gObjX) - 0x1e);
    maddr a3 = R32(GetCarpetShape(R16(gCarpetFrames + (int16_t)(d5 * 2))));
    MakeShapeRect(gObjRect, (int16_t)(R16(gObjY) - R16(a3 + 6)), R16(gObjX), a3);
    if (sect_rect_ptr(gPlayRect, gObjRect, gObjRect) != 0) {
        AddCharObj(0xd);
        WITH_WORD(-1, MarkBufsForRect(JT_ADDR(FN_MarkFRedraw), gObjRect));
    }
    seg4_022454();
    return 0;
}

/* 00035368: the kid sits on the carpet: start it / take off / die if the bars are closed */
uint32_t Sitting(void) {
    ENTRY_LINK(FN_Sitting, 8, 16);               /* link -8; movem D3/A2-A4 */
    if (R32(gCarpet) == 0) return 0;
    maddr p = R32(R32(gCarpet));
    if (R16(p) == 0 && R16(CH(0xa)) == 0x127) {
        W16(p, 1);
        ReadCell(3, 3, 2);
        MakeSpace(R16(gReadCellRoom), R16(gReadCellIdx));
        maddr a3 = R32(GetCarpetShape(-9));
        maddr r = a6 - 8;
        W16(r, R16(gCarpetY) - R16(a3 + 6));
        W16(r + 2, R16(gCarpetX));
        W16(r + 4, R16(gCarpetY));
        W16(r + 6, R16(r + 2) + R16(a3 + 4));
        WITH_WORD(-1, MarkBufsForRect(JT_ADDR(FN_MarkMove), r));
        return 0;
    }
    if (R16(p + 2) == 0 && R16(CH(6)) < 0x64 && (int16_t)IsCaveBarsOpen() == 0) {
        W16(p, 1);
        W16(p + 2, 1);
        W16(CH(6), 0x27);
        W16(CH(0x34), 10);
        JumpSeq(0x81);
        AddSound(0x75);
        return 0;
    }
    if (R16(p + 2) != 0 && R16(p) >= 1) {
        DecLifePts(0x64);
        SetKidDeathMethod(9);
    }
    return 0;
}

/* 0003546e: the kid lands on the carpet */
uint32_t FUN_0003546e(void) {
    ENTRY_NOLINK(FN_FUN_0003546e, 0);
    if (R32(gCarpet) == 0) return 0;
    JumpSeq(0x80);
    W16(CH(4), 0x1a9);
    if (R16(CH(2)) == 0) W16(CH(4), R16(CH(4)) + 0x1e);
    if ((int16_t)IsCaveBarsOpen() != 0) AddSong(0x73);
    else AddSong(0x74);
    return 0;
}

/* 000354b0: free the carpet record */
uint32_t FUN_000354b0(void) {
    ENTRY_NOLINK(FN_FUN_000354b0, 0);
    PrinceResetSnd(0x72);
    _DisposeHandle(R32(gCarpet));
    W32(gCarpet, 0);
    return 0;
}

/* ---- cave bars (level 5 carpet room) ----
 * State word: low 3 bits = frame 0 (closed) .. 3 (open). */
#define gCaveBarsRect   GH(0x104f28)   /* Rect of the bars, for ComputeTrobMoveRect */
#define gReadCellSpec   GH(0x104b52)   /* word: spec of the cell ReadCell found */
#define gCellRect       GH(0x103a00)   /* Rect of the cell being drawn */
#define gCellCol        GH(0x103a08)
#define gCellRow        GH(0x103a0a)
#define pUnionRect      GH(0x1060a4)   /* long: address of UnionRect (Pascal) */

/* 000354c8: trob animation of the cave bars: open (phase 0) or close them one frame */
uint32_t seg8_0354c8(void) {
    ENTRY_NOLINK(FN_seg8_0354c8, 4);             /* move.l D3 */
    uint16_t d3 = RU16(gObjSpec) & 7;
    if (R16(gTrobPhase) == 0) {
        if (d3 < 3) {
            if (d3 == 0) AddSound(0x11a);
            W16(gObjSpec, RU16(gObjSpec) & 0xfff8);
            W16(gObjSpec, RU16(gObjSpec) | (uint16_t)(d3 + 1));
            RedrawCaveBars();
        } else {
            W16(gTrobPhase, -1);
        }
    } else if (d3 != 0) {
        if (d3 == 3) AddSound(0x11c);
        W16(gObjSpec, RU16(gObjSpec) & 0xfff8);
        W16(gObjSpec, RU16(gObjSpec) | (uint16_t)(d3 - 1));
        RedrawCaveBars();
    } else {
        W16(gTrobPhase, -1);
    }
    return 0;
}

/* 0003553a: are the bars at room 3 cell 6 fully open? */
uint32_t IsCaveBarsOpen(void) {
    ENTRY_LINK(FN_IsCaveBarsOpen, 4, 8);         /* link -4; movem D3/D4 */
    ReadCell(3, 6, 0);
    uint16_t d4 = RU16(gReadCellSpec) & 7;
    return d4 >= 3 ? 1 : 0;
}

/* 00035580: piece proc: draw the bars (custom piece kind 0xb) unclipped by the cell */
uint32_t DrawCaveBars(void) {
    ENTRY_LINK(FN_DrawCaveBars, 8, 16);          /* link -8; movem D3-D5/A2 */
    maddr item = R32(gCurBFItem);
    if (R16(item + 0xa) != 0xb || R16(item + 0xe) != R16(gCellCol)) return 0;
    mcopy(a6 - 8, gCellRect, 8);
    mcopy(gCellRect, gPlayRect, 8);
    int16_t d5 = (int16_t)((R16(item + 0x14) & 7) + 1);
    DrawCustomRoomImg(d5, 0xb);
    mcopy(gCellRect, a6 - 8, 8);
    return 0;
}

/* 000355f2: mark the bars for redraw */
uint32_t RedrawCaveBars(void) {
    ENTRY_LINK(FN_RedrawCaveBars, 8, 0);         /* link -8 */
    maddr r = a6 - 8;
    if (ComputeTrobMoveRect(r, gCaveBarsRect) != 0)
        WITH_WORD(-1, MarkBufsForRect(JT_ADDR(FN_MarkMove), r));
    return 0;
}

/* 00035632: trigger handler of the bars: 0 = start opening, 1 = already open, -1 = no */
uint32_t TriggerCaveBars(int16_t room, int16_t cell) {
    ENTRY_LINK(FN_TriggerCaveBars, 0, 8);        /* link 0; movem D3/D4 */
    int16_t d3 = -1;
    if ((RU16(gReadCellSpec) & 0x2000) == 0) {
        maddr a = (uint32_t)(uint16_t)room * 0x78 + R32(gGameState) + 0x780 - 0x78 +
                  (uint32_t)((int32_t)cell * 4);
        int16_t d4 = R16(a);
        if (d4 == 0) d3 = 0;
        else if (d4 == 3) d3 = 1;
    }
    return (uint16_t)d3;
}

/* 000356ae: add the back-wall piece behind the current cave piece, unless an existing
 * back item already covers it */
uint32_t DrawCaveBackWall(void) {
    ENTRY_LINK(FN_DrawCaveBackWall, 0xc, 28);    /* link -0xc; movem D3-D7/A2/A3 */
    maddr item = R32(gCurBFItem);
    uint16_t info = RU16(item + 0x16);
    W16(a6 - 4, info);
    int16_t d6 = (int16_t)((uint32_t)(uint16_t)(info & 0xf00) >> 8);
    int16_t d5 = (int16_t)(RU16(a6 - 4) & 0x1f);
    if (d5 == 0) return 0;
    int16_t d3;
    maddr a3;
    if (R16(gFullRedraw) == 0) {
        a3 = GetBackTableImgRow(d5, (int16_t)(R16(item + 0xe) - 1), R16(item + 0x10));
        if (a3 != 0 && sect_rect_ptr(gCellRect, a3 + 0x1e, a6 - 0xc) != 0) {
            d3 = 0;
            call_ptr(R32(pUnionRect), "lll", a3 + 0x1e, a3 + 0x1e, gCellRect);
        } else {
            a3 = 0;
            d3 = 1;
        }
    } else {
        d3 = 0;
        a3 = 0;
    }
    if (a3 != 0) return 0;
    if (d3 == 0 && d6 != 0) return 0;
    item = R32(gCurBFItem);
    int16_t d7 = R16(item + 0x12);
    W16(item + 0x12, 0);
    d6 = (int16_t)(d6 + 0x24);
    W16(R32(gCurBFItem) + 0x18, d5);
    uint32_t off = (uint32_t)(uint16_t)d6 * 0x14;
    W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + off + 4));
    W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + off + 6));
    W16(R32(gCurBFItem) + 6, 0);
    if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(0);
    W16(R32(gCurBFItem) + 6, 0x10);
    W16(R32(gCurBFItem) + 0x12, d7);
    return 0;
}

/* 000357f2: piece proc: cave wall pieces (layer 5: wall face, layer 1: front edge) */
uint32_t FUN_000357f2(void) {
    ENTRY_NOLINK(FN_FUN_000357f2, 8);            /* movem D3/A2 */
    maddr a2 = R32(gCurBFItem);
    uint16_t layer = RU16(a2 + 0xa);
    if (layer == 5) {
        int16_t d3 = 0;
        if (R16(gCurRoom) != 0) {
            a2 = R32(gCurBFItem);
            d3 = S16(PieceRightId(R16(a2 + 0xe), R16(a2 + 0x10)));
        }
        if (R16(gCurRoom) != 0 && d3 == 0x14) return 0;
        a2 = R32(gCurBFItem);
        d3 = (int16_t)((uint32_t)(uint16_t)(R16(a2 + 0x14) & 0x30) >> 4);
        W16(a2 + 0x18, d3 + 0x32);
        W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0x194));
        W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + 0x196));
        AddNextBFItemToTable();
    } else if (layer == 1) {
        W16(a2 + 0x18, (R16(a2 + 0x14) & 0xf) + 0x25);
        W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0x1a0));
        W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + 0x1a2));
        if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(1);
        FUN_000358ca();
    }
    return 0;
}

/* 000358ca: add the overlay (crack / flaw) piece of the current cave cell; the y of
 * overlay 9 depends on the stalactite state of the cell above */
#define gCaveOverlays   GH(0x104f90)   /* {shape, x, y} per overlay */
#define gCaveOverlayY   GH(0x104f94)
#define gStalacDY       GH(0x1050d0)   /* word[]: y offset per state of the piece above */
uint32_t FUN_000358ca(void) {
    ENTRY_NOLINK(FN_FUN_000358ca, 24);           /* movem D3-D7/A2 */
    maddr a2 = R32(gCurBFItem);
    if (R16(a2 + 0x10) != R16(gCellRow)) return 0;
    if (R16(a2 + 0xe) != R16(gCellCol)) return 0;
    int16_t d5 = (int16_t)((uint32_t)(uint16_t)(R16(a2 + 0x14) & 0xf00) >> 8);
    if (d5 == 0) return 0;
    d5--;
    if (d5 == 9) {
        W16(gCaveOverlayY + (int16_t)((int32_t)d5 * 6), -0x66);
        a2 = R32(gCurBFItem);
        int16_t d3 = S16(PieceAboveId(R16(a2 + 0xe), R16(a2 + 0x10)));
        if (d3 == 0xb) {
            int16_t d4, d6;
            if (R16(R32(gCurBFItem) + 0x10) == 0) { d4 = R16(gRoomAbove); d6 = 2; }
            else { d4 = R16(gCurRoom); d6 = (int16_t)(R16(R32(gCurBFItem) + 0x10) - 1); }
            uint32_t roomOff = (uint32_t)(uint16_t)d4 * 0x78;
            int32_t c = (int32_t)S16(FirstInRow(d6)) + (int32_t)R16(R32(gCurBFItem) + 0xe);
            maddr a = roomOff + (uint32_t)(c * 4) + R32(gGameState);
            int16_t d7 = R16(a + 0x708);
            d3 = (int16_t)(d7 & 0xf);
            maddr y = gCaveOverlayY + (int16_t)((int32_t)d5 * 6);
            W16(y, R16(y) + R16(gStalacDY + (int16_t)(d3 * 2)));
        }
    }
    int16_t d4 = (int16_t)(d5 + 0x5f);
    a2 = R32(gCurBFItem);
    if (R16(a2 + 0xa) == 0 && d4 == 0x6c) {
        mcopy(a2 + 0x18, gCaveOverlays + (uint32_t)((int32_t)d5 * 6), 6);
        AddNextBFItemToTable();
    } else if (R16(R32(gCurBFItem) + 0xa) == 1) {
        if (d4 == 0x6c) d5++;
        mcopy(R32(gCurBFItem) + 0x18, gCaveOverlays + (uint32_t)((int32_t)d5 * 6), 6);
        AddNextBFItemToTable();
    }
    return 0;
}

/* 00035a7a: piece proc: dart shooter in a cave floor */
#define gDartShapes     GH(0x1050c2)   /* word[8] */
uint32_t DrawCaveDart(void) {
    ENTRY_LINK(FN_DrawCaveDart, 4, 12);          /* link -4; movem D4/D5/A2 */
    maddr a2 = R32(gCurBFItem);
    int16_t d4 = R16(a2 + 0xa);
    uint16_t d5 = RU16(a2 + 0x14);
    W16(a2 + 0x14, RU16(a2 + 0x14) & 0xfffc);
    W16(R32(gCurBFItem) + 0x14, RU16(R32(gCurBFItem) + 0x14) | 1);
    DrawCaveFloor();
    if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(d4);
    W16(R32(gCurBFItem) + 0x14, d5);
    if (d4 == 1) {
        W16(R32(gCurBFItem) + 0x18, R16(gDartShapes + (uint32_t)(d5 & 7) * 2));
        W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0x2a4));
        W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + 0x2a6));
        AddNextBFItemToTable();
    }
    return 0;
}

/* 00035b16: piece proc: cave doorway/pillar back (shape 0x40) */
uint32_t FUN_00035b16(void) {
    ENTRY_NOLINK(FN_FUN_00035b16, 0);
    if (R16(R32(gCurBFItem) + 0xa) == 0) {
        DrawCaveBackWall();
        W16(R32(gCurBFItem) + 0x18, 0x40);
        W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0x144));
        W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + 0x146));
        AddNextBFItemToTable();
    } else {
        W16(R32(gCurBFItem) + 0x14, RU16(R32(gCurBFItem) + 0x14) & 0xfffc);
        DrawCaveFloor();
    }
    return 0;
}

/* 00035b64: piece proc: cave exit (back, the start-room marker, the movable door) */
uint32_t seg8_035b64(void) {
    ENTRY_NOLINK(FN_seg8_035b64, 0);
    if (R16(R32(gCurBFItem) + 0xa) == 0) {
        DrawCaveBackWall();
        W16(R32(gCurBFItem) + 0x18, 0x40);
        W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0x158));
        W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + 0x15a));
        if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(0);
        if (R16(gCurRoom) == R16(R32(gGameState) + 0x2198)) {
            W16(R32(gCurBFItem) + 0x18, 0x43);
            W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0x15e));
            W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + 0x160));
            if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(0);
        }
        DrawCaveExitMovable();
    } else {
        W16(R32(gCurBFItem) + 0x14, RU16(R32(gCurBFItem) + 0x14) & 0xfffc);
        W16(R32(gCurBFItem) + 0x14, RU16(R32(gCurBFItem) + 0x14) | 2);
        DrawCaveFloor();
    }
    return 0;
}

/* 00035c18: the movable part of the cave exit door (raised by the exit state) */
#define gKidFrame       KID(0xa)
uint32_t DrawCaveExitMovable(void) {
    ENTRY_LINK(FN_DrawCaveExitMovable, 0x10, 12); /* link -0x10; movem D3/D4/A2 */
    maddr a2 = R32(gCurBFItem);
    if (R16(a2 + 0xa) != 0) return 0;
    mcopy(a6 - 8, gCellRect, 8);
    mcopy(a6 - 0x10, gPlayRect, 8);
    W16(a6 - 0x10, (int16_t)(R16(a2 + 0x10) * 0x78 + 1));
    if (sect_rect_ptr(a6 - 0x10, gCellRect, gCellRect) != 0) {
        a2 = R32(gCurBFItem);
        uint16_t d3 = RU16(a2 + 0x14) & 0xff;
        int16_t d4 = (int16_t)(d3 < 0x4f ? d3 : 0x4f);
        W16(a2 + 0x18, 0x42);
        W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0x478));
        W16(R32(gCurBFItem) + 0x1c, (int16_t)(-0x1d - (int16_t)(d4 + 1)));
        if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(0);
        d3 = RU16(gKidFrame);
        if (d3 >= 0xd9 && d3 <= 0xe2 &&
            (int32_t)R16(CH(0xe)) * 0x78 + 0x52 > (int32_t)R16(CH(6))) {
            a2 = R32(gCurBFItem);
            ResetNextBFItem(1);
            TransferBFItemMidFields(a2, R32(gCurBFItem));
        }
        W16(R32(gCurBFItem) + 0x18, 0x41);
        W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0x47e));
        W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + 0x480));
        AddNextBFItemToTable();
    }
    mcopy(gCellRect, a6 - 8, 8);
    return 0;
}

/* 00035d68: piece proc: cave floor (back wall for layer 0, floor edge for layers 5/2,
 * floor front for layer 1) */
uint32_t DrawCaveFloor(void) {
    ENTRY_LINK(FN_DrawCaveFloor, 4, 20);         /* link -4; movem D3-D6/A2 */
    int16_t d5 = 0;
    maddr a2 = R32(gCurBFItem);
    int16_t d6 = R16(a2 + 0xa);
    int16_t d4 = (int16_t)(R16(a2 + 0x14) & 3);
    switch ((uint16_t)d6) {
    case 0:
        DrawCaveBackWall();
        break;
    case 5: case 2:
        d5 = 1;
        W16(R32(gCurBFItem) + 0x18, d4 + 0x19);
        W16(R32(gCurBFItem) + 0x1a, d4 == 3 ? 9 : 2);
        W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + 0x20));
        break;
    case 1:
        if (d4 == 3) {
            W16(a2 + 0x18, 0x18);
        } else {
            W16(R32(gCurBFItem) + 6, 0);
            W16(R32(gCurBFItem) + 0x18, 0x17);
        }
        W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0x24));
        W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + 0x26));
        d5 = 1;
        break;
    }
    if (d5 != 0) {
        if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(d6);
        W16(R32(gCurBFItem) + 6, 0x10);
        if (d6 == 1) FUN_000358ca();
    }
    return 0;
}

/* 00035e6a: piece proc: cave gate (frame in the back, floor and movable gate in front) */
uint32_t FUN_00035e6a(void) {
    ENTRY_NOLINK(FN_FUN_00035e6a, 12);           /* movem D3/D4/A2 */
    maddr a2 = R32(gCurBFItem);
    int16_t d4 = R16(a2 + 0xa);
    if (d4 == 0) {
        W16(a2 + 6, 0);
        W16(R32(gCurBFItem) + 0x18, 0x3d);
        W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0x2d4));
        W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + 0x2d6));
        if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(0);
        W16(R32(gCurBFItem) + 6, 0x10);
    } else {
        a2 = R32(gCurBFItem);
        int16_t d3 = R16(a2 + 0x14);
        W16(a2 + 0x14, RU16(a2 + 0x14) & 0xf0fc);
        W16(R32(gCurBFItem) + 0x14, RU16(R32(gCurBFItem) + 0x14) | 2);
        DrawCaveFloor();
        if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(d4);
        W16(R32(gCurBFItem) + 0x14, d3);
    }
    DrawCaveGateMovable();
    return 0;
}

/* Pascal EmptyRect(r): Boolean through the pointer at A5-0x1f60 */
#define pEmptyRect      GH(0x1060a0)
static uint8_t empty_rect_ptr(maddr r) {
    maddr sp0 = emu_sp();
    push16(0);
    push32(r);
    call68_addr(R32(pEmptyRect));
    uint8_t res = R8(sp0 - 2);
    emu_set_sp(sp0);
    return res;
}

/* 00035f00: the movable part of a cave gate (its height comes from the gate state) */
uint32_t DrawCaveGateMovable(void) {
    ENTRY_LINK(FN_DrawCaveGateMovable, 0x18, 24); /* link -0x18; movem D3-D7/A2 */
    mcopy(a6 - 0x10, gCellRect, 8);
    mcopy(a6 - 0x18, gPlayRect, 8);
    W16(a6 - 0x18, (int16_t)(R16(R32(gCurBFItem) + 0x10) * 0x78 - 0x1f));
    if (sect_rect_ptr(a6 - 0x18, gCellRect, gCellRect) != 0) {
        maddr a2 = R32(gCurBFItem);
        uint16_t d3 = RU16(a2 + 0x14) & 0xff;
        W16(a6 - 4, d3);
        d3 >>= 2;
        int16_t d5 = (int16_t)(d3 < 0x2e ? (uint16_t)(RU16(a6 - 4) >> 2) : 0x2e);
        int16_t d6 = (int16_t)(-7 - (int16_t)(d5 << 1));
        W16(a6 - 8, 0);
        int16_t d7 = R16(a2 + 0xa);
        switch ((uint16_t)d7) {
        case 5: case 2:
            W16(R32(gCurBFItem) + 0x18, 0x3f);
            W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0x456));
            W16(R32(gCurBFItem) + 0x1c, d6 - 1);
            W16(a6 - 8, 1);
            break;
        case 1: {
            W16(a2 + 0x18, 0x3e);
            W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0x45c));
            W16(R32(gCurBFItem) + 0x1c, d6);
            if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(1);
            int16_t col = R16(gCellCol);
            W16(gCellCol, R16(R32(gCurBFItem) + 0xe));
            FUN_000358ca();
            W16(gCellCol, col);
            if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(1);
            break;
        }
        }
        if (R16(a6 - 8) != 0) {
            if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(d7);
            if (!(d7 == 2 && d5 == 0x2e)) {
                if (d7 == 2) W16(gCellRect + 4, d6 - 0xd);
                if (empty_rect_ptr(gCellRect) == 0) {
                    W16(R32(gCurBFItem) + 0x18, 0x3c);
                    W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0x5a));
                    W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + 0x5c));
                    if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(d7);
                }
            }
        }
    }
    mcopy(gCellRect, a6 - 0x10, 8);
    return 0;
}

/* 000360d2: draw the movable part of a cave gate in the foreground */
uint32_t FUN_000360d2(void) {
    ENTRY_NOLINK(FN_FUN_000360d2, 0);
    return DrawCaveGateMovable();
}

/* 000360d8: piece proc: lava pit (left part) */
uint32_t seg8_0360d8(void) {
    ENTRY_NOLINK(FN_seg8_0360d8, 8);             /* movem D3/A2 */
    maddr a2 = R32(gCurBFItem);
    switch (RU16(a2 + 0xa)) {
    case 0:
        DrawCaveBackWall();
        break;
    case 5: case 2:
        if (R16(gFullRedraw) == 0) {
            a2 = R32(gCurBFItem);
            int16_t d3 = R16(a2 + 0xa);
            W16(a2 + 0x18, 0x72);
            W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0x1d6));
            W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + 0x1d8));
            if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(d3);
            DrawCaveLavaBubbles();
        }
        break;
    case 1:
        W16(a2 + 0x14, RU16(a2 + 0x14) & 0xf00);
        DrawCaveFloor();
        break;
    }
    return 0;
}

/* 0003615e: piece proc: lava pit (right part) */
uint32_t seg8_03615e(void) {
    ENTRY_NOLINK(FN_seg8_03615e, 8);             /* movem D3/A2 */
    maddr a2 = R32(gCurBFItem);
    switch (RU16(a2 + 0xa)) {
    case 0:
        DrawCaveBackWall();
        break;
    case 5: case 2: {
        a2 = R32(gCurBFItem);
        int16_t d3 = R16(a2 + 0xa);
        W16(a2 + 0x18, 0x72);
        W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0x1ea));
        W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + 0x1ec));
        if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(d3);
        DrawCaveLavaBubbles();
        break;
    }
    case 1:
        W16(a2 + 0x14, RU16(a2 + 0x14) & 0xf00);
        DrawCaveFloor();
        break;
    }
    return 0;
}

/* ---- lava ----
 * Lava info handle (GetLavaInfoHnd): +0 state word (low 5 bits: boil phase, 0x800 flip),
 * then 15 bubble records of 12 bytes at +2: +0 active, +2 frame, +4 kind, +6 y, +8 x,
 * +0xa layer. */
#define gLavaBoilShapes GH(0x1050a4)   /* {shape, x, y} for boil phases 1 and 2 */
#define gLavaSeqShapes  GH(0x1050b6)   /* long[]: {shape, x, y}[] per character sequence */
#define gBubbleSeqs     GH(0x105306)   /* long[]: word[] shapes per bubble kind */

/* 000361de: draw the lava surface, the character's splash and the bubbles of the cell */
uint32_t DrawCaveLavaBubbles(void) {
    ENTRY_LINK(FN_DrawCaveLavaBubbles, 0x14, 32); /* link -0x14; movem D3-D7/A2-A4 */
    maddr a2 = R32(gCurBFItem);
    int16_t d6 = R16(a2 + 0x12) == 0x18 ? 1 : 0;
    if (R16(gFullRedraw) != 0 && d6 == 0) return 0;
    maddr a3 = GetLavaInfoHnd(R16(gCurRoom), R16(a2 + 0xe), R16(a2 + 0x10));
    if (a3 == 0) return 0;
    maddr a4 = R32(a3);
    int16_t d7 = R16(R32(gCurBFItem) + 0xa);
    int16_t d4 = R16(a4);
    W16(a6 - 4, d4);
    d4 &= 0x1f;
    int16_t d5 = S16(GetLavaCharSeqIdx(R16(a6 - 4)));
    if (d5 >= 0) {
        W16(R32(gCurBFItem) + 0x18, 0x73);
        uint32_t off = (uint32_t)((int32_t)d6 * 0x14);
        W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + off + 0x1d6));
        W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + off + 0x1d8));
        if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(d7);
        uint32_t d0 = (uint32_t)((int32_t)d4 * 6);
        maddr seq = gLavaSeqShapes + (int16_t)(d5 * 4);
        if (R16(d0 + R32(seq)) != -1) {
            W16(R32(gCurBFItem) + 8, R16(a4) & 0x800);
            mcopy(R32(gCurBFItem) + 0x18, d0 + R32(seq), 6);
            if (d6 != 0) W16(R32(gCurBFItem) + 0x1a, R16(R32(gCurBFItem) + 0x1a) - 0x33);
            if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(d7);
            if (d4 == 1 || d4 == 2) {
                mcopy(R32(gCurBFItem) + 0x18, gLavaBoilShapes + (uint32_t)((int32_t)d4 * 6), 6);
                if (d6 != 0) W16(R32(gCurBFItem) + 0x1a, R16(R32(gCurBFItem) + 0x1a) - 0x33);
                if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(d7);
            }
        }
        W16(R32(gCurBFItem) + 8, 0);
        if (d4 < 3) return 0;
    }
    d4 = 0;
    a2 = a4 + 2;
    W16(a6 - 0xc, (int16_t)(d6 * 0x33));
    W32(a6 - 8, (uint32_t)((int32_t)d6 * 0x14));
    do {
        a3 = a2;
        uint16_t d3 = 0;
        do {
            if (R16(a3) != 0 && R16(a3 + 0xa) != d4) {
                uint32_t p = (uint32_t)((int32_t)R16(a3 + 2) * 2) +
                             R32(gBubbleSeqs + (int16_t)(R16(a3 + 4) * 4));
                W16(R32(gCurBFItem) + 0x18, R16(p) + 0x74);
                W16(R32(gCurBFItem) + 0x1a, R16(a3 + 8) - R16(a6 - 0xc));
                W16(R32(gCurBFItem) + 0x1c, R16(a3 + 6));
                if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(d7);
            }
            d3++;
            a3 += 0xc;
        } while (d3 < 0xf);
        if (d4 == 0 && d5 < 0) {
            W16(R32(gCurBFItem) + 0x18, 0x73);
            W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + R32(a6 - 8) + 0x1d6));
            W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + R32(a6 - 8) + 0x1d8));
            if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(d7);
        }
        d4++;
    } while (d4 < 2);
    return 0;
}

/* 00036494: piece proc: loose cave floor (the y of the loose piece follows its shake
 * state; the cell is clipped to the kid's row when he hangs from it) */
#define gKidSeq         KID(0x28)
#define gKidCol         KID(0xc)
#define gKidRow         KID(0xe)
uint32_t DrawCaveLoose(void) {
    ENTRY_LINK(FN_DrawCaveLoose, 8, 24);         /* link -8; movem D3-D7/A2 */
    int16_t d6 = (int16_t)(R16(R32(gCurBFItem) + 0x14) & 0xf);
    if (d6 != 0xd) d6 = R16(gStalacDY + (int16_t)(d6 * 2));
    switch (RU16(R32(gCurBFItem) + 0xa)) {
    case 0:
        DrawCaveBackWall();
        return 0;
    case 5:
        W16(R32(gCurBFItem) + 0x18, 0x38);
        W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0xe0));
        W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + 0xe2));
        if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(5);
        /* fall through */
    case 2: {
        if (d6 == 0xd) return 0;
        maddr a2 = gStalacDY + (int16_t)(d6 * 2);
        W16(R32(gLevelPieces) + 0xe8, R16(a2));
        W16(R32(gCurBFItem) + 0x18, 0x39);
        W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0xe6));
        W16(R32(gCurBFItem) + 0x1c, R16(a2));
        mcopy(a6 - 8, gCellRect, 8);
        if (R16(gKidSeq) == 0xa) {
            a2 = R32(gCurBFItem);
            if (R16(a2 + 0xa) == 2 && R16(gKidCol) == R16(a2 + 0xe)) {
                int16_t d5 = R16(gKidRow);
                int16_t d7 = R16(a2 + 0x10);
                if ((int32_t)d5 - 1 == (int32_t)d7 || (d5 == d7 && R16(gKidFrame) == 0x8d))
                    W16(gCellRect + 4, R16(gRowTop + (int16_t)(R16(R32(gCurBFItem) + 0x10) * 2)));
            }
        }
        W16(gCellRect + 4, R16(gCellRect + 4) + 1);
        AddNextBFItemToTable();
        mcopy(gCellRect, a6 - 8, 8);
        return 0;
    }
    case 1:
        FUN_000358ca();
        return 0;
    }
    return 0;
}

/* 0003660e: piece proc: cave floor with a rock (shapes 0x22 / 0x21) */
uint32_t FUN_0003660e(void) {
    ENTRY_NOLINK(FN_FUN_0003660e, 12);           /* movem D3/D4/A2 */
    maddr a2 = R32(gCurBFItem);
    int16_t d3 = R16(a2 + 0xa);
    int16_t d4 = R16(a2 + 0x14);
    W16(a2 + 0x14, RU16(a2 + 0x14) | (RU16(a2 + 0xe) & 1));
    DrawCaveFloor();
    if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(d3);
    W16(R32(gCurBFItem) + 0x14, d4);
    switch ((uint16_t)d3) {
    case 5: case 2:
        W16(R32(gCurBFItem) + 0x18, 0x22);
        W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0xaa));
        W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + 0xac));
        AddNextBFItemToTable();
        break;
    case 1:
        W16(R32(gCurBFItem) + 0x18, 0x21);
        W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0xb0));
        W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + 0xb2));
        AddNextBFItemToTable();
        break;
    }
    return 0;
}

/* 000366c2: piece proc: top of a cave pillar */
uint32_t DrawCavePillarTop(void) {
    ENTRY_LINK(FN_DrawCavePillarTop, 4, 4);      /* link -4; move.l A2 */
    maddr a2 = R32(gCurBFItem);
    switch (RU16(a2 + 0xa)) {
    case 0:
        DrawCaveBackWall();
        break;
    case 5:
        W16(R32(gCurBFItem) + 0x18, 0x24);
        W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0xbe));
        W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + 0xc0));
        AddNextBFItemToTable();
        break;
    case 1:
        W16(a2 + 0x18, 0x23);
        W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0xc4));
        W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + 0xc6));
        if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(1);
        FUN_000358ca();
        break;
    }
    return 0;
}

/* 00036772: piece proc: cave pillar */
uint32_t FUN_00036772(void) {
    ENTRY_NOLINK(FN_FUN_00036772, 16);           /* movem D3-D5/A2 */
    maddr a2 = R32(gCurBFItem);
    int16_t d5 = R16(a2 + 0x14);
    int16_t d4 = R16(a2 + 0xa);
    W16(a2 + 0x14, RU16(a2 + 0x14) | (uint16_t)((R16(a2 + 0xe) & 1) + 1));
    W16(R32(gCurBFItem) + 0x14, RU16(R32(gCurBFItem) + 0x14) & 0xf0ff);
    DrawCaveFloor();
    if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(d4);
    W16(R32(gCurBFItem) + 0x14, d5);
    int16_t d3 = 0;
    switch ((uint16_t)d4) {
    case 5: case 2:
        W16(R32(gCurBFItem) + 0x18, 0x20);
        W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0x46));
        W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + 0x48));
        d3 = 1;
        break;
    case 1:
        W16(R32(gCurBFItem) + 0x18, 0x1f);
        W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0x4c));
        W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + 0x4e));
        d3 = 1;
        break;
    }
    if (d3 != 0) {
        AddNextBFItemToTable();
        if (d4 == 1) {
            if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(d4);
            FUN_000358ca();
        }
    }
    return 0;
}

/* 00036850: piece proc: stalactite (0x2000 in the spec: broken) */
uint32_t seg8_036850(void) {
    ENTRY_NOLINK(FN_seg8_036850, 8);             /* movem D3/A2 */
    maddr a2 = R32(gCurBFItem);
    switch (RU16(a2 + 0xa)) {
    case 0:
        DrawCaveBackWall();
        break;
    case 5: case 2:
        a2 = R32(gCurBFItem);
        W16(a2 + 0x18, (RU16(a2 + 0x14) & 0x2000) ? 0x37 : 0x36);
        W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0x2b2));
        W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + 0x2b4));
        AddNextBFItemToTable();
        break;
    case 1: {
        W16(a2 + 0x14, RU16(a2 + 0x14) & 0xcf00);
        a2 = R32(gCurBFItem);
        int16_t d3 = S16(PieceAboveId(R16(a2 + 0xe), R16(a2 + 0x10)));
        if (d3 == 0xb) {
            W16(R32(gCurBFItem) + 0x14, RU16(R32(gCurBFItem) + 0x14) & 0xf0ff);
            W16(R32(gCurBFItem) + 0x14, RU16(R32(gCurBFItem) + 0x14) | 0xa00);
        }
        DrawCaveFloor();
        break;
    }
    }
    return 0;
}

/* 000368f8: piece proc: stalactite above a floor */
uint32_t seg8_0368f8(void) {
    ENTRY_NOLINK(FN_seg8_0368f8, 8);             /* movem D3/A2 */
    maddr a2 = R32(gCurBFItem);
    int16_t d3 = R16(a2 + 0xa);
    if (RU16(a2 + 0x14) & 0x2000) seg8_036850();
    else DrawCaveFloor();
    if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(d3);
    FUN_00036930();
    return 0;
}

/* 00036930: add the torch-holder/ledge piece (shapes 0x1d / 0x1e) */
uint32_t FUN_00036930(void) {
    ENTRY_NOLINK(FN_FUN_00036930, 4);            /* move.l A2 */
    maddr a2 = R32(gCurBFItem);
    switch (RU16(a2 + 0xa)) {
    case 5: case 2:
        W16(R32(gCurBFItem) + 0x18, 0x1d);
        W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0x122));
        W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + 0x124));
        AddNextBFItemToTable();
        break;
    case 1:
        W16(a2 + 0x18, 0x1e);
        W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0x128));
        W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + 0x12a));
        AddNextBFItemToTable();
        break;
    }
    return 0;
}

/* 000369b0: piece proc: torch */
uint32_t seg8_0369b0(void) {
    ENTRY_NOLINK(FN_seg8_0369b0, 4);             /* move.l D3 */
    int16_t d3 = R16(R32(gCurBFItem) + 0xa);
    DrawCaveTorch();
    if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(d3);
    FUN_00036930();
    return 0;
}

/* 000369d4: piece proc: loose floor (drawn as a cave loose piece once it shakes) */
uint32_t seg8_0369d4(void) {
    ENTRY_NOLINK(FN_seg8_0369d4, 4);             /* move.l A2 */
    switch (RU16(R32(gCurBFItem) + 0xa)) {
    case 0:
        DrawCaveBackWall();
        break;
    case 5: {
        maddr a2 = R32(gCurBFItem);
        if ((RU16(a2 + 0x14) & 7) < 3) break;
        W16(a2 + 0x14, RU16(a2 + 0x14) & 0xf00);
        W16(R32(gCurBFItem) + 0x14, RU16(R32(gCurBFItem) + 0x14) | 0xd);
        W16(R32(gCurBFItem) + 0x12, 0xb);
        DrawCaveLoose();
        break;
    }
    case 1:
        FUN_000358ca();
        break;
    }
    return 0;
}

/* 00036a32: piece proc: cave wall edge */
uint32_t seg8_036a32(void) {
    ENTRY_NOLINK(FN_seg8_036a32, 4);             /* move.l A2 */
    maddr a2 = R32(gCurBFItem);
    switch (RU16(a2 + 0xa)) {
    case 0:
        DrawCaveBackWall();
        break;
    case 5:
        a2 = R32(gCurBFItem);
        if (R16(a2 + 0xe) == 9) break;
        W16(a2 + 0x18, 0x44);
        W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0x194));
        W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + 0x196));
        AddNextBFItemToTable();
        break;
    case 1:
        W16(a2 + 0x14, RU16(a2 + 0x14) & 0xfff0);
        W16(R32(gCurBFItem) + 0x14, RU16(R32(gCurBFItem) + 0x14) | 4);
        FUN_000357f2();
        break;
    }
    return 0;
}

/* 00036aa8: draw a cave torch (flame frame from the spec) */
uint32_t DrawCaveTorch(void) {
    ENTRY_LINK(FN_DrawCaveTorch, 4, 12);         /* link -4; movem D3/D4/A2 */
    maddr a2 = R32(gCurBFItem);
    uint16_t d4 = RU16(a2 + 0x14);
    W16(a6 - 4, d4);
    W16(a2 + 0x14, (uint16_t)((uint32_t)(uint16_t)(RU16(a6 - 4) & 0xe0) >> 5));
    W16(R32(gCurBFItem) + 0x14, RU16(R32(gCurBFItem) + 0x14) | (d4 & 0xf00));
    switch (RU16(R32(gCurBFItem) + 0xa)) {
    case 0: {
        DrawCaveFloor();
        int16_t d3 = (d4 & 0x10) ? 9 : (int16_t)(d4 & 0xf);
        if (R32(gCurBFItem) == 0) ResetNextBFItemWithPrevInfo(0);
        W16(R32(gCurBFItem) + 0x18, d3 + 0x51);
        W16(R32(gCurBFItem) + 0x1a, R16(R32(gLevelPieces) + 0x180));
        W16(R32(gCurBFItem) + 0x1c, R16(R32(gLevelPieces) + 0x182));
        AddNextBFItemToTable();
        break;
    }
    case 2: case 5: case 1:
        DrawCaveFloor();
        break;
    }
    return 0;
}

/* 00036b74: DrawObj routine of a falling cave loose floor */
#define gObjXCopy       GH(0x1037e6)
uint32_t DrawObjCaveLoose(void) {
    ENTRY_LINK(FN_DrawObjCaveLoose, 0, 8);       /* link 0; movem D3/D4 */
    W16(gObjFace, -1);
    AddMid(4, 0x39, R16(gObjXCopy), (int16_t)(R16(gObjY) - 0x12), 0x10);
    AddMid(4, 0x68, R16(gObjXCopy), R16(gObjY), 0x10);
    return 0;
}

/* 00036bd8: install the cave spike shape tables */
uint32_t FUN_00036bd8(void) {
    W32(GH(0x103c42), GH(0x104f40));
    W32(GH(0x103c3e), GH(0x104f50));
    W16(GH(0x103c3c), 0x45);
    W32(GH(0x103c38), GH(0x104f60));
    return 0;
}

/* 00036bf8: trigger a gate that has a reverse plate: returns the new trob phase or -1.
 * The cell under the character is read with ReadCell while the current-room pointers and
 * the ReadCell result globals are saved and restored around it. */
#define gReadCell       GH(0x104b50)   /* 20 bytes: last ReadCell result (id, spec, idx, room, col, row...) */
#define gKidAlive       KID(0x1c)
uint32_t TriggerGateWithRevPlate(int16_t room, int16_t cell) {
    ENTRY_LINK(FN_TriggerGateWithRevPlate, 0x1c, 32); /* link -0x1c; movem D3-D7/A2-A4 */
    int16_t d7 = -1;
    int16_t d3 = cell;
    uint16_t spec = RU16(R32(gRoomSpecs) + (int32_t)d3 * 4);
    W16(a6 - 4, spec);
    uint16_t d5 = spec & 0xff;
    uint16_t d6 = RU16(a6 - 4) & 0xff00;
    W16(a6 - 0x1c, seg5_027190());
    maddr a3 = GetTrobRoomCell(room, cell);
    if (a3 != 0) {
        d3 = R16(a3 + 4);
        if (d3 == 0 || d3 == 1) {
            if (R16(a6 - 0x1c) != 0) {
                d3 = R16(gReadCellSpec);
            } else if (R16(gReadCell) == 0x22) {
                d3 = R16(gReadCellSpec);
            } else {
                d3 = 0x22;
                mcopy(a6 - 0x18, gReadCell, 20);
                maddr tiles = R32(gRoomTiles);
                maddr specs = R32(gRoomSpecs);
                int16_t d4 = S16(ReadCell(R16(CH(0x16)), R16(CH(0xc)), R16(CH(0xe))));
                if (d4 == d3) d3 = R16(gReadCellSpec);
                else d3 = 0;
                W32(gRoomSpecs, specs);
                W32(gRoomTiles, tiles);
                mcopy(gReadCell, a6 - 0x18, 20);
            }
            if (d3 & 0x2000) {
                W16(R32(gRoomSpecs) + (int32_t)cell * 4, d6 + 0xed);
            } else {
                if (d5 > 0xb8) W16(R32(gRoomSpecs) + (int32_t)cell * 4, d6 + 0xb8);
                W16(a3 + 4, 4);
            }
        } else if (R16(a3 + 4) == 2 && R16(a6 - 0x1c) != 0) {
            W16(a3 + 4, 3);
        }
        return (uint16_t)d7;
    }
    if (d5 == 0xff) return (uint16_t)d7;
    int alt = 0;
    if (d5 == 0) {
        if (R16(CH(8)) == 4 && R16(CH(0xe)) != 0 && LEVEL_NUM() == 5) {
            d3 = R16(CH(0x16));
            if (d3 == 10 || d3 == 7 || d3 == 0xc) alt = 1;
        }
        if (!alt) {
            if (R16(a6 - 0x1c) != 0) d7 = 3;
            else if ((RU16(gReadCellSpec) & 0x2000) == 0) d7 = 2;
            return (uint16_t)d7;
        }
    }
    if (d5 != 0 && d5 != 0x38 && R16(gKidAlive) < 0) d7 = 4;
    return (uint16_t)d7;
}

/* 00036daa: trob animation of a dart shooter: count up to 6, fire on the first frames */
uint32_t FUN_00036daa(void) {
    ENTRY_NOLINK(FN_FUN_00036daa, 4);            /* move.l D3 */
    uint16_t d3 = RU16(gObjSpec) & 7;
    if (d3 < 6) {
        W16(gObjSpec, R16(gObjSpec) + 1);
        if (d3 < 3) FUN_000371a2();
    } else {
        W16(gObjSpec, RU16(gObjSpec) & 0xfff8);
        W16(gTrobPhase, -1);
    }
    return 0;
}

/* 00036dda: ATM routine of a flying dart (mob type 2) */
uint32_t seg8_036dda(void) {
    ENTRY_NOLINK(FN_seg8_036dda, 8);             /* movem D3/D4 */
    int16_t d3 = R16(gMobSpeed);
    if (d3 == -1 || d3 == -3) return 0;
    d3 = (int16_t)(R16(gMobX) / 0x33);
    int16_t d4 = S16(GetRow(R16(gMobY)));
    W16(gObjLink, IndexCell1(d3, d4));
    if (R16(gMobRoom) == R16(gCurRoom)) {
        MarkMobRedraw(1);
        AddMobObj(R16(gMobY));
        W16(gMobType, 2);
    }
    return 0;
}

/* 00036e42: does a flying dart hit the current character? */
#define gCharRect       CH(0x2a)       /* Rect: top, left, bottom, right */
uint32_t CheckDartHit(void) {
    ENTRY_LINK(FN_CheckDartHit, 0x10, 12);       /* link -0x10; movem D3/D4/A3 */
    uint16_t fr = RU16(CH(0xa));
    if (fr >= 0x6c && fr <= 0x70 && R16(CH(0x28)) == 0x4f) return 0;
    int16_t d4 = 1;
    maddr a3;
    do {
        a3 = GetMobTypeItem(2, d4++);
        if (a3 == 0 || R16(a3 + 6) == -1) continue;
        maddr r = a6 - 0x10;
        int16_t right = (int16_t)(R16(a3) + 0x16);
        W16(r + 6, right);
        W16(r + 2, (int16_t)(right - 9));
        W16(r + 4, R16(a3 + 2));
        W16(r, (int16_t)(R16(a3 + 2) - 7));
        if (sect_rect_ptr(r, gCharRect, a6 - 8) != 0) {
            FUN_000370f8();
            W16(a3 + 6, -1);
            continue;
        }
        int16_t d3 = R16(r + 2);
        if (d3 < R16(CH(0x2c))) continue;
        if ((int32_t)R16(CH(0x2a)) - 3 > (int32_t)R16(r + 4)) continue;
        if ((int32_t)R16(CH(0x2e)) - 3 < (int32_t)R16(r)) continue;
        if ((int32_t)d3 - (int32_t)R16(a3 + 8) >= (int32_t)R16(CH(0x30))) continue;
        if ((int32_t)R16(r + 4) - (int32_t)R16(a3 + 6) >= (int32_t)R16(CH(0x2e))) continue;
        FUN_000370f8();
        W16(a3 + 6, -1);
    } while (a3 != 0);
    return 0;
}

/* 00036f58: mob routine of a flying dart: move it, stop it at walls (-3 = hit) */
uint32_t MobDart(void) {
    ENTRY_LINK(FN_MobDart, 4, 16);               /* link -4; movem D3-D6 */
    if (R16(gMobSpeed) == -3) {
        W16(gMobSpeed, -1);
        return 0;
    }
    int16_t d3 = R16(gMobSpeed);
    if (d3 == -1) return 0;
    int16_t d4 = (int16_t)(R16(gMobX) + R16(gMobAccel));
    int16_t d5 = (int16_t)(R16(gMobY) + d3);
    d3 = 0x1fe;
    if (d4 > d3) {
        if (R16(gMobRoom) == R16(gRoomLeft)) {
            W16(gMobRoom, R16(gCurRoom));
            d4 = (int16_t)(d4 - d3);
        } else {
            if (R16(R32(gGameState) + (uint32_t)RU16(gMobRoom) * 8 + 0x207a) == 0) AddSound(0x43);
            W16(gMobSpeed, -3);
        }
    }
    if (R16(gMobSpeed) != -1 && d5 < 5) {
        if (R16(gMobRoom) == R16(gRoomBelow)) {
            W16(gMobRoom, R16(gCurRoom));
            d5 = (int16_t)(d5 + 0x16d);
        } else {
            if (R16(R32(gGameState) + (uint32_t)RU16(gMobRoom) * 8 + 0x207c) == 0) AddSound(0x43);
            W16(gMobSpeed, -3);
        }
    }
    d3 = -3;
    if (R16(gMobSpeed) == d3) return 0;
    if ((int32_t)R16(gMobRow) * 0x78 + 5 >= (int32_t)d5) {
        W16(gMobRow, R16(gMobRow) - 1);
        int16_t d6 = S16(ReadCell(R16(gMobRoom), (int16_t)(d4 / 0x33), R16(gMobRow)));
        if (S16(IsSpace(d6)) == 0) W16(gMobSpeed, d3);
    } else {
        int32_t c = (int32_t)(int16_t)(d4 / 0x33);
        if (c != (int32_t)(int16_t)(R16(gMobX) / 0x33)) {
            int16_t d6 = S16(ReadCell(R16(gMobRoom), (int16_t)c, R16(gMobRow)));
            if (S16(IsWall(d6)) != 0 || (d6 == 4 && S16(seg4_0235de()) != 0))
                W16(gMobSpeed, -3);
        }
    }
    if (R16(gMobSpeed) == -3) AddSound(0x43);
    W16(gMobY, d5);
    W16(gMobX, d4);
    return 0;
}

/* 000370f8: the current character is hit by a dart */
uint32_t FUN_000370f8(void) {
    ENTRY_NOLINK(FN_FUN_000370f8, 8);            /* movem D3/D4 */
    if (R16(CH(0x1c)) >= 0) return 0;
    if (S16(DecLifePts(1)) == 0) {
        W16(CH(0x34), 8);
        if (R16(CH(8)) == 0) AddSound(0x1d);
        else AddSound(0x3b);
    } else {
        int16_t d3 = S16(seg4_021562());
        int16_t d4 = S16(FUN_00021518());
        if (S16(IsSpace(d4)) == 0 || d3 < 6) {
            seg6_02fba2();
            FUN_00020cca();
            W16(CH(0x14), 0);
        } else {
            W16(CH(4), AddCharX((int16_t)(d3 - 0x3f)));
            FUN_00020542();
            W16(CH(0xe), R16(CH(0xe)) + 1);
            JumpSeq(0x12);
        }
    }
    AddSound(0x42);
    SetKidDeathMethod(0);
    return 0;
}

/* 000371a2: fire a dart from the current trob (or wait one more frame) */
uint32_t FUN_000371a2(void) {
    ENTRY_NOLINK(FN_FUN_000371a2, 0);
    if (S16(FUN_00025362()) != 0) RedrawTrob(GH(0x10519c));
    else W16(gTrobPhase, 1);
    return 0;
}

/* Lava pits: up to 4 lava info handles (0xb6 bytes each, see DrawCaveLavaBubbles) in
 * gLavaInfo, indexed by the trob phase of the pit's lava trob (type 0x17). */
#define gLavaInfo       GH(0x10531e)   /* Handle[4] */
#define gLavaBubbleTmpl GH(0x1051b2)   /* 0x16 bubble templates of 10 bytes: +0 boil frame, +2 y, +4 {x, kind}, +8 layer */
#define gLavaSplashEnd  GH(0x1051ac)   /* word[]: last boil frame per character sequence */
#define gLavaRect       GH(0x1051a4)   /* Rect of a lava pit */
#define LAVA_HND(i)     (gLavaInfo + (int16_t)((i) * 4))

/* 000371be: trob animation of a lava pit: boil after a character fell in, move bubbles */
uint32_t AnimLava(void) {
    ENTRY_LINK(FN_AnimLava, 0x10, 32);           /* link -0x10; movem D3-D7/A2-A4 */
    int16_t idx = (int16_t)(R16(gTrobPhase) & 7);
    W16(a6 - 8, idx);
    _HLock(R32(LAVA_HND(idx)));
    maddr a3 = R32(R32(LAVA_HND(R16(a6 - 8))));
    if (S16(FUN_00025362()) == 0) {
        _HUnlock(R32(LAVA_HND(R16(a6 - 8))));
        _DisposeHandle(R32(LAVA_HND(R16(a6 - 8))));
        W32(LAVA_HND(R16(a6 - 8)), 0);
        return 0;
    }
    int16_t d5 = R16(a3);
    W16(a6 - 4, d5);
    d5 &= 0x1f;
    int16_t seq = S16(GetLavaCharSeqIdx(R16(a6 - 4)));
    W16(a6 - 0xa, seq);
    if (d5 == 0x16 || (seq >= 0 && d5 == 3)) {
        W16(a6 - 0xc, R16(a3) & 0xf800);
        ResetLavaInfo(a3, 0, 0);
        if (d5 == 3) {
            W16(a3, 4);
            W16(a3, RU16(a3) | RU16(a6 - 0xc));
        }
        RedrawLava();
    } else {
        W16(a3, R16(a3) + 1);
        RedrawLava();
        int16_t d3 = R16(a6 - 0xa);
        if (!(d3 >= 0 && d5 < 3)) {
            if (d3 >= 0) {
                d3 = R16(gLavaSplashEnd + (int16_t)(d3 * 2));
                if (d5 >= d3) {
                    if ((RU16(a3) & 0x1000) == 0 || !((int32_t)d3 + 0x10 > (int32_t)d5))
                        W16(a3, RU16(a3) & 0x7ff);
                    if (R16(gLavaSplashEnd + (int16_t)(R16(a6 - 0xa) * 2)) == d5) AddSound(0x44);
                }
                d5 = 0;
            }
            maddr a4 = a3 + 2;
            for (int16_t d6 = 0; d6 < 0xf; d6++, a4 += 0xc) {
                if (R16(a4) == 0) continue;
                W16(a4 + 2, R16(a4 + 2) + 1);
                maddr p = (uint32_t)((int32_t)R16(a4 + 2) * 2) +
                          R32(gBubbleSeqs + (int16_t)(R16(a4 + 4) * 4));
                int16_t d7 = R16(p);
                if (d7 == -1) {
                    W16(a4, 0);
                } else if (d7 == -2) {
                    W16(a4 + 0xa, 1);
                    W16(a4 + 2, R16(a4 + 2) + 1);
                }
            }
            W32(a6 - 0x10, gLavaBubbleTmpl);
            int16_t d6 = 0;
            if (R16(gLavaBubbleTmpl) <= d5) {
                do {
                    maddr t = R32(a6 - 0x10);
                    if (R16(t) == d5) TriggerLavaBubble(a3, t);
                    d6++;
                    W32(a6 - 0x10, R32(a6 - 0x10) + 10);
                } while (d6 < 0x16 && R16(R32(a6 - 0x10)) <= d5);
            }
        }
    }
    _HUnlock(R32(LAVA_HND(R16(a6 - 8))));
    return 0;
}

/* 000373bc: does the current character fall into boiling lava? */
uint32_t CheckLavaBoil(void) {
    ENTRY_LINK(FN_CheckLavaBoil, 8, 20);         /* link -8; movem D3-D7 */
    int16_t d5 = S16(ReadCell(R16(CH(0x16)), R16(CH(0xc)), R16(CH(0xe))));
    if (d5 != 0x17 && d5 != 0x18) return 0;
    int16_t d6 = (int16_t)(R16(CH(0xe)) * 0x78 + 0x4c);
    int16_t dx = S16(seg4_021542());
    W16(a6 - 8, dx);
    int16_t colx = R16(GH(0x103478) + (int16_t)(R16(CH(0xc)) * 2));
    W16(a6 - 4, colx);
    int16_t d7 = (int16_t)(colx + 0x2b - dx);
    int16_t d4 = (int16_t)(R16(a6 - 4) + 0x24 - dx);
    if ((d5 == 0x17 && d7 <= 0) || (d5 == 0x18 && d4 >= 0)) {
        if ((RU16(gFrameFlags) & 0x40) != 0 && R16(CH(6)) > d6 && R16(CH(0x34)) != 5)
            FUN_00037466();
    }
    return 0;
}

/* 00037466: the current character falls into the lava */
uint32_t FUN_00037466(void) {
    ENTRY_NOLINK(FN_FUN_00037466, 16);           /* movem D3/A2-A4 */
    maddr h = GetLavaInfoHnd(R16(CH(0x16)), R16(CH(0xc)), R16(CH(0xe)));
    if (h == 0) return 0;
    _HLock(h);
    maddr a3 = R32(h);
    W16(a3, 0x8000);
    if (R16(CH(8)) == 0) {
        int16_t d3 = R16(CH(0x10));
        if (d3 != 3 && d3 != 4) {
            W16(a3, RU16(a3) | 0x2000);
            if (S16(Rnd(3)) == 0) W16(a3, RU16(a3) | 0x1000);
        }
        JumpSeq(0x74);
        AddSound(0x46);
    } else {
        W16(a3, RU16(a3) | 0x4000);
        AddSound(0x45);
        JumpSeq(0x73);
    }
    if (R16(CH(2)) == -1) W16(a3, RU16(a3) | 0x800);
    W16(CH(0x32), 0);
    W16(CH(0x18), 1);
    W16(CH(0x34), 5);
    seg4_020fb2();
    DecLifePts(0x64);
    AnimChar();
    maddr t = GetLavaTrob(R16(CH(0x16)), R16(CH(0xc)), R16(CH(0xe)));
    if (t != 0) {
        mcopy(gTrobCell, t, 8);
        RedrawLava();
    }
    _HUnlock(h);
    return 0;
}

/* 00037550: first free lava info slot (alert and reuse the last one when all 4 are used) */
uint32_t FUN_00037550(void) {
    ENTRY_NOLINK(FN_FUN_00037550, 4);            /* move.l D3 */
    uint16_t d3 = 0;
    if (R32(gLavaInfo) != 0) {
        do {
            d3++;
            if (d3 >= 4) break;
        } while (R32(gLavaInfo + (uint32_t)d3 * 4) != 0);
    }
    if (d3 == 4) {
        AlertOKOnly(fn_address(FN_FUN_00037550) + 0x3e);   /* PC-relative string at 0003758e */
        d3--;
    }
    return d3;
}

/* 000375bc: lava info handle of the pit at room/col/row (NULL if none) */
uint32_t GetLavaInfoHnd(int16_t room, int16_t col, int16_t row) {
    ENTRY_LINK(FN_GetLavaInfoHnd, 4, 4);         /* link -4; move.l A2 */
    maddr t = GetLavaTrob(room, col, row);
    if (t == 0) return 0;
    return R32(LAVA_HND(R16(t + 4)));
}

/* 00037608: which character sequence fell in (bits 13-14) when bit 15 is set, else -1 */
uint32_t GetLavaCharSeqIdx(int16_t state) {
    int16_t d3;
    if ((uint16_t)state & 0x8000) d3 = (int16_t)((int32_t)(int16_t)(state & 0x6000) >> 13);
    else d3 = -1;
    return (uint16_t)d3;
}

/* 0003764a: lava trob of the pit at room/col/row (the right half 0x18 belongs to the
 * cell on its left) */
uint32_t GetLavaTrob(int16_t room, int16_t col, int16_t row) {
    ENTRY_LINK(FN_GetLavaTrob, 0, 12);           /* link 0; movem D3/D4/A2 */
    int16_t d3 = S16(ReadCell(room, col, row));
    int16_t d4 = R16(gReadCellIdx);
    if (d3 == 0x18) d4--;
    maddr t = GetTrobRoomCell(room, d4);
    CalcBlue(R16(gCurRoom));
    return t;
}

/* 000376a4: free all lava info handles */
uint32_t FUN_000376a4(void) {
    ENTRY_NOLINK(FN_FUN_000376a4, 8);            /* movem D3/A2 */
    int16_t d3 = 0;
    do {
        maddr h = R32(LAVA_HND(d3));
        if (h != 0) {
            _HUnlock(h);
            _DisposeHandle(R32(LAVA_HND(d3)));
            W32(LAVA_HND(d3), 0);
        }
        d3++;
    } while (d3 < 4);
    return 0;
}

/* 000376f0: mark the current lava pit for redraw (taller rect while a character sinks).
 * The original locks the handle and never unlocks it here. */
uint32_t RedrawLava(void) {
    ENTRY_LINK(FN_RedrawLava, 8, 4);             /* link -8; move.l A2 */
    _HLock(R32(LAVA_HND(R16(gTrobPhase))));
    maddr a2 = R32(R32(LAVA_HND(R16(gTrobPhase))));
    maddr r = a6 - 8;
    if (RU16(a2) & 0x8000) {
        W16(r, 0x1f);
        W16(r + 2, 0x11);
        W16(r + 4, R16(GH(0x1051a8)));
        W16(r + 6, 0x6e);
    } else {
        mcopy(r, gLavaRect, 8);
    }
    if (ComputeTrobMoveRect(r, r) != 0)
        WITH_WORD(-1, MarkBufsForRect(JT_ADDR(FN_MarkMove), r));
    return 0;
}

/* 00037782: reset a lava info record (boil frame start*5, re-arm the bubbles) */
uint32_t ResetLavaInfo(uint32_t info, int16_t start, int16_t full) {
    ENTRY_LINK(FN_ResetLavaInfo, 0, 4);          /* link 0; move.l D3 */
    if (full == 0) {
        W16(info, 0);
        return 0;
    }
    tc_memset(info, 0, 0xb6);
    W16(info, (int16_t)(start * 5));
    uint16_t d3 = 0;
    do {
        TriggerLavaBubble(info, (uint32_t)((int32_t)d3 * 10) + gLavaBubbleTmpl);
        d3++;
    } while (d3 < 0x16);
    return 0;
}

/* 00037804: trigger handler of a lava pit: allocate its info and add its trob */
uint32_t TriggerLava(int16_t room, int16_t cell) {
    ENTRY_LINK(FN_TriggerLava, 0, 8);            /* link 0; movem D3/A2 */
    if (GetTrobRoomCell(room, cell) != 0) return 0;
    int16_t d3 = S16(FUN_00037550());
    maddr h = PoPNewHandle(0xb6);
    W32(gLavaInfo + (uint32_t)(uint16_t)d3 * 4, h);
    _HLock(h);
    maddr slot = gLavaInfo + (uint32_t)(uint16_t)d3 * 4;
    ResetLavaInfo(R32(R32(slot)), (int16_t)(cell % 3), 1);
    _HUnlock(R32(slot));
    AddTrob(room, cell, d3, 0x17);
    return 0;
}

/* 000378a4: start bubble template tmpl in the first free bubble record of info */
uint32_t TriggerLavaBubble(uint32_t info, uint32_t tmpl) {
    uint16_t d4 = 0;
    if (R16(info + 2) != 0) {
        do {
            d4++;
            if (d4 >= 0xf) break;
        } while (R16((uint32_t)((int32_t)(int16_t)d4 * 12) + info + 2) != 0);
    }
    if (d4 >= 0xf) return 0;
    maddr d3 = (uint32_t)((int32_t)(int16_t)d4 * 12) + info;
    maddr a2 = d3 + 2;
    int16_t on;
    if (R16(a2) != 0) on = 1;
    else on = ((int32_t)R16(info) - 1 == (int32_t)R16(tmpl)) ? 1 : 0;
    W16(a2, on);
    if (on != 0) {
        W16(d3 + 4, 0);
        W16(d3 + 6, R16(tmpl + 2));
        W32(d3 + 8, R32(tmpl + 4));
        W16(d3 + 0xc, R16(tmpl + 8));
    }
    return 0;
}

/* 00037978: a loose floor fell: clear the flaw of the piece below its cell.
 * The original updates its own argument slots (glue writes them back). */
static int16_t s_ccf_room, s_ccf_cell;
uint32_t CaveClearFlawFromPieceBelow(int16_t room, int16_t cell) {
    cell = (int16_t)(cell + 10);
    if (cell > 0x1e) {
        room = R16(R32(gGameState) + 0x2080 + (uint32_t)(uint16_t)room * 8 - 2);
        cell = (int16_t)(cell - 0x1e);
    }
    s_ccf_room = room;
    s_ccf_cell = cell;
    maddr a = (uint32_t)((int32_t)cell * 4) +
              ((uint32_t)(uint16_t)room * 0x78 + R32(gGameState) + 0x780 - 0x78);
    W16(a, RU16(a) & 0xf0ff);
    return 0;
}
void glue_CaveClearFlawFromPieceBelow(void) {
    maddr sp = emu_sp();
    CaveClearFlawFromPieceBelow((int16_t)rd16(sp), (int16_t)rd16(sp + 2));
    wr16(sp, (uint16_t)s_ccf_room);
    wr16(sp + 2, (uint16_t)s_ccf_cell);
    emu_set_d0(0);
}

/* 00037a18: a cave gate closing on the kid (squishing gate): stop it on him or crush him.
 * The gate height lives in the argument slot (glue writes it back). */
#define gKidRoom        KID(0x16)
#define gKidAction34    KID(0x34)
#define gGateSpeed      GH(0x103a24)   /* word[]: gate speed per phase */
static int16_t s_cmsg_v;
uint32_t CaveMoveSquishingGate(int16_t v) {
    ENTRY_LINK(FN_CaveMoveSquishingGate, 0, 12); /* link 0; movem D3-D5 */
    int16_t d5 = (int16_t)(S16(FirstInRow(R16(gKidRow))) + R16(gKidCol));
    int16_t d3 = R16(gTrobRoom);
    if (R16(gKidRoom) != d3) {
        if (d3 == R16(gRoomLeft)) d5 = (int16_t)(d5 + 10);
        else if (R16(gTrobRoom) == R16(gRoomRight)) d5 = (int16_t)(d5 - 10);
        else d5 = 0x1e;
    }
    d3 = R16(gTrobCell);
    if (d5 == d3 || (int32_t)d5 + 1 == (int32_t)d3) {
        if (R16(gKidAction34) == 3) {
            v = 0x30;
            AddGateSound(0x14);
            StopGate(v);
        } else if (R16(gKidFrame) == 0x108) {
            v = (int16_t)(v - (int16_t)(R16(gGateSpeed + (int16_t)(R16(gTrobPhase) * 2)) + 1));
        } else if (R16(gKidFrame) == 0x109) {
            v = 0x38;
        } else if (R16(gKidFrame) == 0x10a) {
            W16(gTrobPhase, -1);
            PrinceResetSnd(0x11);
            RedrawGate();
        }
    }
    s_cmsg_v = v;
    return (uint16_t)v;
}
void glue_CaveMoveSquishingGate(void) {
    maddr sp = emu_sp();
    uint32_t r = CaveMoveSquishingGate((int16_t)rd16(sp));
    wr16(sp, (uint16_t)s_cmsg_v);
    emu_set_d0(r);
}

/* 00037b0e: random torch flame frame 0..8, different from the current one */
uint32_t CaveGetTorchFrame(int16_t cur) {
    ENTRY_LINK(FN_CaveGetTorchFrame, 0, 4);      /* link 0; move.l D3 */
    uint32_t r = Rnd(8);
    int16_t d3 = (int16_t)r;
    if (d3 == cur) {
        d3++;
        if ((uint16_t)d3 >= 9) d3 = 0;
    }
    return (r & 0xffff0000u) | (uint16_t)d3;
}

/* 00037b52: per-frame hook of the cavern levels: bridge rooms of level 5 */
uint32_t seg8_037b52(void) {
    ENTRY_NOLINK(FN_seg8_037b52, 8);             /* movem D3/D4 */
    int16_t d3 = LEVEL_NUM();
    if ((d3 == 5 && R16(gCurRoom) == 10) || (d3 == 5 && R16(gCurRoom) == 7) ||
        (d3 == 5 && R16(gCurRoom) == 0xc))
        FUN_00033fc4();
    return 0;
}

/* 00037b8e: level 5 start: the bridge is intact */
uint32_t seg8_037b8e(void) {
    ENTRY_NOLINK(FN_seg8_037b8e, 0);
    W16(gBrgState, 0);
    tc_memset(GH(0x104ed2), 0, 0x10);
    return 0;
}
