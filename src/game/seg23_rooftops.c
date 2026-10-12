/* CODE 23 "Rooftops" — level-specific code for the rooftop levels.
 * Ported from the Ghidra decompilation of the original 68k code; addresses in
 * comments are Ghidra addresses (see docs/PORTING.md). */
#include "port.h"
#include "gen/proto.h"

/* Globals (Ghidra addresses) */
#define gGameState      GH(0x102fec)   /* long: pointer to the game state block */
#define gCurBFItem      GH(0x103074)   /* long: pointer to the background/foreground item being built */
#define gLevelPieces    GH(0x103462)   /* long: pointer to the level's piece table */
#define gCurRoom        GH(0x10303e)   /* word: current room */
#define gRoofPalette    GH(0x104e56)   /* word: which roof palette is active (0/1) */

#define LEVEL_TYPE()    R16(R32(gGameState) + 0x2186)

/* 0006de24 */
uint32_t DrawRoofBackWall(void) {
    ENTRY_LINK(FN_DrawRoofBackWall, 4, 16);      /* link -4; movem A2/D3-D5 */
    uint32_t item = R32(gCurBFItem);
    uint16_t shape = RU16(item + 0x16) & 0x3f;
    if (shape == 0) return 0;
    int16_t saved12 = R16(item + 0x12);
    W16(item + 0x12, 0);
    W16(item + 6, 0);
    int16_t room = R16(gCurRoom);
    if (LEVEL_TYPE() != 5 || room == 0x13 || room == 0x10 || room == 0xf) shape += 0x6c;
    uint32_t pieces = R32(gLevelPieces);
    W32(item + 0x18, R32(pieces + 0x2d2));
    W16(item + 0x1c, RU16(pieces + 0x2d6));
    room = R16(gCurRoom);
    if (LEVEL_TYPE() == 5 && room != 0x13 && room != 0x10 && room != 0xf &&
        R16(GH(0x104c4a) + (int16_t)shape * 2) != 0) {
        W16(R32(gCurBFItem) + 0x18, RU16(GH(0x104c4a) + (int16_t)shape * 2));
        if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(0);
        W16(R32(gCurBFItem) + 6, 0x10);
        item = R32(gCurBFItem); pieces = R32(gLevelPieces);
        W32(item + 0x18, R32(pieces + 0x2d2));
        W16(item + 0x1c, RU16(pieces + 0x2d6));
    }
    item = R32(gCurBFItem); pieces = R32(gLevelPieces);
    W32(item + 0x18, R32(pieces + 0x2d2));
    W16(item + 0x1c, RU16(pieces + 0x2d6));
    W16(R32(gCurBFItem) + 0x18, shape);
    if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(0);
    W16(R32(gCurBFItem) + 0x12, saved12);
    W16(R32(gCurBFItem) + 6, 0x10);
    return 0;
}

/* 0006dfae: add the floor-edge piece that matches the neighbouring cell */
uint32_t FUN_0006dfae(void) {
    ENTRY_NOLINK(FN_FUN_0006dfae, 12);           /* movem A2/D3/D4 */
    uint32_t item = R32(gCurBFItem);
    int16_t idx = (int16_t)(((RU16(item + 0x14) & 0xf00) >> 8) - 1);
    if (idx < 0) return 0;
    int16_t kind = R16(item + 10);
    int16_t want = R16(GH(0x104cea) + idx * 2);
    if ((kind == 5 && want == 5) || (kind == 1 && want == 1)) {
        W32(item + 0x18, R32(GH(0x104ca8) + idx * 6));
        W16(item + 0x1c, RU16(GH(0x104cac) + idx * 6));
        AddNextBFItemToTable();
    } else if (idx == 1 && R16(R32(gCurBFItem) + 10) == 1) {
        item = R32(gCurBFItem);
        W32(item + 0x18, R32(GH(0x104ca8)));
        W16(item + 0x1c, RU16(GH(0x104cac)));
        AddNextBFItemToTable();
    }
    return 0;
}

/* 0006df5e */
uint32_t FUN_0006df5e(void) {
    ENTRY_NOLINK(FN_FUN_0006df5e, 8);            /* movem A2/D3 */
    uint32_t item = R32(gCurBFItem);
    if (R16(item + 10) != 1) return 0;
    uint32_t pieces = R32(gLevelPieces);
    W32(item + 0x18, R32(pieces + 0x2de));
    W16(item + 0x1c, RU16(pieces + 0x2e2));
    item = R32(gCurBFItem);
    W16(item + 0x18, (RU16(item + 0x14) & 0x1f) + 0x45);
    if (S16(AddNextBFItemToTable()) != 0) ResetNextBFItemWithPrevInfo(1);
    FUN_0006dfae();
    return 0;
}

/* 0006e5b8: switch between the two rooftop palettes depending on the room */
uint32_t RoofCheckPalette(int16_t room) {
    ENTRY_LINK(FN_RoofCheckPalette, 0, 8);       /* link 0; movem D3/D4 */
    int16_t sel = -1;
    if (LEVEL_TYPE() == 5 && room != 0x13 && room != 0x10 && room != 0xf) {
        if (R16(gRoofPalette) != 0) { W16(gRoofPalette, 0); sel = 0; }
    } else if (R16(gRoofPalette) != 1) {
        W16(gRoofPalette, 1); sel = 1;
    }
    if (sel != -1) PoPSetPalette((int16_t)(sel + 0xdac), 0x40);
    return 0;
}
