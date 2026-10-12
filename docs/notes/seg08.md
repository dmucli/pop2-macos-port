# seg08: CODE 8 "Caverns" (0x33fc4-0x37ba8)

All 96 functions are ported in `src/game/seg08_caverns.c`. Signature fixes are in
`tools/sig_overrides/seg08.json`.

## What the unit does

This is the level code of the cavern levels (level type 3: levels 4 and 5). It covers the
collapsing bridge and the flying carpet of level 5, the cave piece drawing, darts, lava
pits and squishing gates.

**Collapsing bridge** (level 5, rooms 7 / 10 / 12; `seg8_037b52` is the per-frame hook).
- `FUN_00033fc4` runs once per frame:
  - `FUN_000341a2` tests whether the kid stands on a bridge floor (tile 0x2c). In room 7
    it also opens the gate at room 7 cell 13 (`TriggerGateWithRevPlate`).
  - `FUN_00034212` does the same test for the room's first guard.
  - `MarkBrgFloorsForChars` and `MarkBridgeForCol` set the shaking columns in `gBrgMarks`
    (0x104ecc, one bit per column, 0xff when both characters are on the bridge) and play
    the creak sound.
  - `BridgeCheckBreak` breaks the bridge.
- `BridgeGetCharCol(room, col)` converts a column to bridge coordinates (room 7: -10,
  room 12: +10).
- Break state:
  - `gBrgStress` (0x104ece) counts the frames both characters stood close together on the
    middle columns. At 0x3c the state `gBrgState` (0x104ed0) goes 1 → 2 → 3.
  - In state 3, `TriggerBrgFloorMob` drops the floors of columns 7..3, one per frame. It
    makes them falling mobs of type 0xb and records each column in word[3..7] after
    0x104ed0.
  - When the bridge is gone, the state becomes -1 and the kid falls (`AddCharX(0x11)`).
- Shaking floors:
  - The trob animation `seg8_034698` and `RedrawBrgFloor` / `OffsetBrgFloorY` /
    `RestoreBrgFloorY` move the custom pieces 3+frame of the custom room by the column
    offsets in `gBrgFloorDY` (0x104e66).
  - `ATMBrgFloor`, `FUN_00034b62` and `DrawObjBrgFloor` draw and animate the falling
    floors. Each falling frame has a piece (0x104e78) and a dy/dx (0x104e94 / 0x104eb0).
- Guard AI on the bridge: `seg8_03426e`, `seg8_0342fc`, `FUN_00034462`, `FUN_000343e4` and
  `IsBrgGateOpen`.

**Flying carpet** (level 5, room 3).
- `gCarpet` (0x104f3c) is a 6-byte handle with these fields:
  - +0: frame counter.
  - +2: flying flag.
  - +4: save-under peel pending.
- Carpet shapes are 0x62d7+n and come from the level-type resource file (`CheckCarpetImg`,
  `GetCarpetShape`).
- `FUN_00034cf0` / `seg8_034d1c` are the level start/end hooks. They set the palettes and
  allocate/free the handle (`FUN_00035238` / `FUN_000354b0`).
- `Sitting` (kid frame 0x127) starts the carpet, and it dies if the cave bars are closed.
- `FUN_0003546e` lands the kid on the carpet.
- `SetupCarpet` builds the FChar object, and `DrawObjCarpet` draws it.
  - `DrawObjCarpet` also fades palette entries 0x2e/0x2d towards the blood red
    (`ComputeBloodRGB`).
- `DrawCarpet` / `SaveUnderCarpet` handle the parked carpet.

**Cave bars** (room 3, cell 6).
- The low 3 bits of the state are the frame, 0 (closed) to 3 (open).
- `seg8_0354c8` is the trob animation; the other functions are `IsCaveBarsOpen`,
  `TriggerCaveBars`, `RedrawCaveBars` and `DrawCaveBars`.

**Cave pieces.** These are the piece procs called through the per-level-type table. They
build BF items through `gCurBFItem` (0x103074) using the offsets of the level piece table
(`gLevelPieces`, 0x103462).
- `DrawCaveBackWall` adds the back wall unless an existing back item already covers the cell.
- `DrawCaveFloor` and `FUN_000358ca` (overlays 0x104f90, stalactite offsets 0x1050d0).
- Exits and gates: `seg8_035b64`, `DrawCaveExitMovable`, `FUN_00035e6a`,
  `DrawCaveGateMovable`.
- Lava pits: `seg8_0360d8`, `seg8_03615e`, `DrawCaveLavaBubbles`.
- Loose floors: `DrawCaveLoose`, `seg8_0369d4`, `DrawObjCaveLoose`.
- Torches: `DrawCaveTorch`, `seg8_0369b0`, `CaveGetTorchFrame`.
- Pillars: `DrawCavePillarTop`, `FUN_00036772`.
- Stalactites: `seg8_036850`, `seg8_0368f8`.
- Others: `FUN_000357f2`, `FUN_00035b16`, `FUN_0003660e`, `FUN_00036930`,
  `seg8_036a32`, `DrawCaveDart`.

**Darts.** `FUN_00036daa` is the trob animation of the shooter, and `FUN_000371a2` fires.
`MobDart` moves the mob (type 2) and `seg8_036dda` draws it. `CheckDartHit` and
`FUN_000370f8` handle the hit.

**Lava.**
- Lava info: up to 4 handles of 0xb6 bytes in `gLavaInfo` (0x10531e), indexed by the lava
  trob's phase.
  - +0 is the state word: bits 0-4 are the boil frame, 0x800 flip, 0x1000/0x2000 sequence
    bits, 0x8000 "a character fell in".
  - Then come 15 bubble records of 12 bytes: +0 active, +2 frame, +4 kind, +6 y, +8 x,
    +0xa layer.
  - The bubble templates are 0x16 records of 10 bytes at 0x1051b2.
- `TriggerLava` allocates the info (`FUN_00037550` finds a free slot) and calls
  `ResetLavaInfo` / `TriggerLavaBubble`.
- `AnimLava` is the trob animation. `RedrawLava` marks the pit for redraw, and
  `FUN_000376a4` frees everything.
- `CheckLavaBoil` and `FUN_00037466` make a character fall into the lava.
- Lookups: `GetLavaTrob`, `GetLavaInfoHnd` and `GetLavaCharSeqIdx`.

**Gates.**
- `TriggerGateWithRevPlate` triggers a gate that has a reverse plate.
- `CaveMoveSquishingGate` stops a closing gate on the kid, or crushes him.
- `CaveClearFlawFromPieceBelow` and `FUN_00036bd8` (installs the cave spike shape tables
  at 0x103c38..0x103c42).

## Porting notes

- **Calls.** Every function that calls anything starts with `ENTRY_LINK` / `ENTRY_NOLINK`.
  The original pushes an extra unused word (-1) to `MarkBufsForRect`. `WITH_WORD` keeps it
  on the emulated stack so the callee's frame sits where it does in the original.
- **Rect procs.** SectRect and EmptyRect are called through the cached trap pointers
  (A5-0x1f58, A5-0x1f60), and UnionRect through A5-0x1f5c with `call_ptr`.
- **Own argument slots.** These functions write their own argument slots, so they are
  `"conv": "manual"` with a hand-written glue that writes the final slot values back:
  - `BridgeGetCharCol`: the column.
  - `MarkBridgeForCol`: the loop counter.
  - `CaveClearFlawFromPieceBelow`: the room and the cell.
  - `CaveMoveSquishingGate`: the gate height.
- **`ComputeBloodRGB`.** It takes three word parameters. The index had one, so the
  override gives three. Its 32-bit divide/multiply helpers become C operators.
- **Kept quirks:**
  - `RedrawLava` locks the lava handle and never unlocks it.
  - `DrawCaveLoose` looks up the stalactite offset table twice.
  - `TriggerBrgFloor` passes the masked spec (`& 0xfff0`) as the "frame" of
    `RedrawBrgFloor`.
- **Unset register.** `FUN_000351a4` returns the caller's D5 when there is no carpet. Its
  callers check for the carpet first, so this never happens; the port reads
  `cpu_get(R_D5)` there.
- **PC-relative string.** `FUN_00037550` passes the string at 0x3758e to `AlertOKOnly`
  through `fn_address(FN_FUN_00037550) + 0x3e`.
- **Return sizes** (override file). These set `ret` to 0 where D0 is callee garbage and
  every caller ignores it. I checked the callers in the disassembly:
  - `GenCtrl`'s result is dropped by `FUN_000217f4` / `FUN_00021866`.
  - Piece procs, trob animations and ATM routines are dispatched by callers that ignore D0.
  - `DrawObj`, `DrawBFItem`, `FUN_00016716`, `FUN_000168bc` and `FUN_00017db8` ignore D0.

  `ret` is 2 for `FUN_00034212`, `FUN_000351a4`, `TriggerCaveBars` and `FUN_00037550`,
  whose callers only use the low word. All other return values reproduce the original D0,
  including `CaveGetTorchFrame`, which keeps Rnd's high word, and `DrawCarpet`, which
  returns the D0 of `_HUnlock`.
- **No overrides outside the unit.**

## Verification

- **`tools/check.sh`: no mismatch.** The default scenarios never enter a cavern level, so
  none of these functions is reached there (see the issues below).
- **Gameplay runs on the cave saves** ("Level 4", "Level 5", "Level 5/2", "Level 5/3"):
  - `POP2_NATIVE_ONLY` was set per call-graph depth group, so each function is entered
    from 68k code and checked on its own, with CALLS=100.
  - 52 functions verified, no mismatch.
  - These runs found one real bug: `TriggerLavaBubble` had its comparison inverted. It is
    fixed.
- **Temporary self-test driver** (not committed):
  - At 16.5 s into "Level 5/3" it called the remaining functions through their 68k entries
    with prepared states: bridge states, guard positions, a fake custom room, carpet
    frames, bars, lava pits, darts, gates.
  - All 96 functions were checked, with no mismatch.
- **Coverage.** A coverage-instrumented check build (llvm-cov) over all these runs executed
  98.9% of the lines of the file. These lines never ran and were reviewed against the
  disassembly:
  - the room-7 gate trigger in `FUN_000341a2`
  - the random creak sound and the `Rnd(1) == 0` floor variant
  - the last frame of a kind-2 falling floor
  - the `seg5_027190 != 0` paths of `TriggerGateWithRevPlate`
  - a dart hitting a wall when it changes column
  - the "fall back one row" branch of `FUN_000370f8`
  - the bubble -2 marker in `AnimLava`
  - the "all 4 lava slots used" alert in `FUN_00037550`

## Issues found outside this unit

1. **`GetOS` (seg05a) returns 0.** The original returns D0 = the tile id, with the high
   word of the tile address. The index says `ret 0`, but `FUN_00015400` (seg02b, the room
   entry triggers) uses it as the object id.
   - With seg05a native, no room-entry trigger fires: lava pits, flasks, torches, bridge
     floors. The game diverges from the original (for example in "Level 5", no lava trob).
   - A run with only seg05a native reproduces it.
   - Fix: return the tile word (and the address high word), with `ret` 2 or 4.
2. **`tools/check.sh` loads the wrong save for "Level X:2" scenarios.** In zsh,
   `"${file/:/\/}"` gives `Level 5\/2`, so the lookup fails. `SFGetFile` then opens the
   newest file instead (Level 15). Every ":2"/":3" scenario actually runs Level 15. A
   working form is `"${file/:/"/"}"`.
