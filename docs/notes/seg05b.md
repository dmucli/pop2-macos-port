# seg05b — CODE 5 "Modular Set 2", second half (0x27476–0x2a632)

All 79 functions ported in `src/game/seg05b_modularset2.c`.

## What the unit does

* **Mobs** (moving objects: falling floors, spikes, darts, ...). The mob being processed lives in
  16 bytes at 0x103b5a: x, y, room, speed (-1 = finished), accel, type, row, state.
  `ComputeMobRect`/`AddMobObj`/`MarkMobRedraw` build its screen rectangle (offset by ±0x1fe/±0x16d
  for neighbour rooms, clipped with SectRect through the pointer at 0x1060a8) and append it to the
  mob display table (`*0x10334e`, 0x1c-byte entries, count 0x1037da). `FUN_000276f6`,
  `CharUnderMob`, `OppInfoUnderMob` crush the kid / guards (Char structs, 0x3e bytes: kid at
  0x102f2e, current char at 0x102faa, room guards at 0x102df8) under a falling mob.
* **Spikes**: `ATMSpikes`, `MobSpikes`, `TriggerSpikes`, `CheckSpikesJammed`, `DrawObjSpikes`,
  `FUN_00027c78`/`seg5_027dc2`/`seg5_027e6e` (step on / trigger / impaled). Cell state words are
  at `*0x1037dc + cell*4` (current room) or `gs + 0x708 + room*0x78 + cell*4`; 0xf0 = jammed,
  bit 7 = retracting, low bits = frame.
* **Trobs** (animated objects): table at 0x103b72 (8-byte entries, count 0x103c1a); the one
  being animated is cell 0x103c12, room 0x103c14, direction 0x103c16, state word 0x1037e8.
  `AnimGate`, `AnimLoose`, `AnimScythe`, `TriggerDart`, `TriggerScythe`, `CheckScytheHit`,
  `ComputeBladeHitRect`, `FUN_00029c48`/`DrawScythe2` (scythe pieces into the current BF item
  `*0x103074`).
* **Sound/music**: sound table at 0x103f60 (8 bytes per id: type, priority, sound item, kind
  0 = 'MIDI' / else 'snd '). `AddSound` (highest priority wins, 0x103c62) and `AddSong` (first
  wins, 0x103c64) queue requests; `FUN_000292a2` plays them once per frame. Ambient sounds:
  record {on, index, last sound} at 0x1048c8, tables by level type at 0x103f02/0x103f1e/0x103f3a
  (`PlayAmbient`, `TimeForNewAmbient`, `SpecialMusic`). `Prince*Snd` wrap the sound item calls.
* **Recorded games** (attract-mode demos, 'RECG' 0xfa..0xfc): mode 0x103c4a (3 = playing,
  4 = loaded), handle 0x103c5c, frame counter 0x104b66; `ConvertDirToRecDir`/`ConvertRecDirToDir`
  pack/unpack the five input words at 0x104b78 into one word.

## Findings

* Ghidra mis-decodes the 68020 memory-indirect operands in this segment: `([-0x5014,A5,D1],0x780)`
  is shown (and decompiled) as pre-indexed, but the extension words (I/IS = 101/110) are
  post-indexed, i.e. `*gGameState + D1 + 0x780`. Same for `([-0x4824,A5,D0])` = `*0x1037dc + D0`.
* `TriggerDart` and `TriggerScythe` store into their own argument slots (the caller's stack), so
  they are `"conv": "manual"` with hand-written glue that writes the final values back. Note the
  hook pops the return address before calling the glue: arguments start at SP.
* The 68k sound item routines (SetSoundItem, ...) read uninitialised locals and copy them into the
  heap, so the checker flagged the sound functions whenever the dead stack below them differed.
  The functions on the sound paths keep the original stack depth (`WITH_DEPTH`/`C_<Name>` in the
  file); with that every check passes.
* `Rnd` takes a word argument (range) — override added (outside this unit, same as seg02b's).
* `IncRecordedGameToNextFrame` passes a PC-relative string (0x2a4a2) to `Terminate`; the port
  uses `fn_address(FN_IncRecordedGameToNextFrame) + 0x82`.
* `SpecialMusic`, `CharUnderMob`, `OppInfoUnderMob`, `IsVisibleRoomCell`, `ComputeBladeHitRect`
  return a 16-bit value with garbage in the high word (ret overridden to 2); `seg5_027e6e`,
  `CheckScytheHit`, `LoadSounds` are void (ret 0; LoadSounds' D0 after a Pascal call cannot be
  reproduced). D0 is reproduced exactly where callers might pass it on (`DrawObjSpikes`, `AddSound`,
  `GetCol`, `Prince*Snd`, `FUN_00029860`, `PlayAmbient`).

## Verification

`tools/check.sh` (also with CALLS=300, the long intro with the demo recordings, and with the
unit's internal callers excluded so the leaf functions are reached through hooks): 64 functions
verified, no mismatch except one `ComputeMobRect` call in the long intro, where the checker's child
crashes (68k exception at 0x10c inside the sound callbacks run at the OffsetRect trap boundary);
running the original code in place of the port gives the same report.

Never exercised (reviewed against the disassembly): OppInfoUnderMob, AddGateSound, GetTrobId,
seg5_027e6e, GetSpikeIdx, TriggerSpikes, ReadCellWithCell, TriggerDart, FUN_00028cc0,
PrinceReleaseSnd, FUN_00029860, seg5_02988a, ScytheBladeExtended, ConvertDirToRecDir, seg5_02a4da.
