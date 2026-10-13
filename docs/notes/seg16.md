# seg16: CODE 16 "Anim & Audio" (0x4b604-0x4e432) + CODE 23 "Rooftops" (0x6de20-0x6f37a)

All 114 functions are ported: CODE 16 in `src/game/seg16_animaudio.c`, the CODE 23 ones
in `src/game/seg23_rooftops.c` (next to the 4 pilot functions). Signature fixes are in
`tools/sig_overrides/seg16.json`.

## What the unit does

### Cut-scene animations (tAnimation / KBAnimation)

These are Symantec C++ classes. Their methods are "Pascal": C argument order, `this`
first (at 8(A6)), and the callee pops (`rtd`). `PlayTheAnimation` creates a KBAnimation
with `operator new(0x2ca)` and calls it only through the vtable.

* **Object fields**:
  * +0 vtable: `0x105d1c` for tAnimation, `0x105d78` for KBAnimation.
  * +4 frame, +8 running, +a "just started", +c frame delay (ticks), +e next frame time.
  * +12 'SCRP' script handle, +16 script offset, +1a bounds (Rect), +22 layer count.
  * +26 layers, 0x1c bytes each: +0 changed count, +4 rect drawn, +c x, +10 y, +14 shape,
    +18 mode.
  * KBAnimation adds +2c6: base shape id (= 'ANI ' base + 1).
* **vtable**: +0 deleting dtor, +4 create, +8 draw, +10 getBounds, +18 isRunning,
  +1c maintain, +20 neglect, +24 prepare, +28 runScript, +2c seekFrame, +30 setPos,
  +34 startAnim, +38 stop, +40 notifyApp, +44 shapeDraw, +48 shapeTestPixel,
  +4c soundControl.
* **'ANI '**: bounds (8 bytes), layer count (word +8), script id (word +a).
* **Script**: a long length, then commands {byte op, byte size, words}:
  0 end, 1 next frame, 2 notify, 3 sound, 4 delay, 5 layer shape, 6 layer x/y, 7 layer mode.
* **Each frame**:
  * `maintain` clears the layer "changed" counts and runs the script for one frame.
    It returns 1 if a layer changed.
  * `draw` calls `shapeDraw` for every layer and stores the drawn rects in
    `0x105f58` (24 rects).
  * `CombineBounds` merges them with the previous frame's rects (`0x105e98`) into the
    update rects `0x105dd8`, then merges the overlapping update rects.
* **KBAnimation::prepare** loads the shapes into the handle `0x105d74` (0x202 shape
  handles; `GetShape(base + n - 1)`). One shape id (`0x106018`, when `0x10601a` is set)
  gets a private unpacked copy (`HandToHand` + `UnPackAndMapSHAP`). `prepare` also creates
  the script's sounds.
* **PlayTheAnimation(base, offset, ..., callback, ..., abortable, flags, color, h, v)**:
  * Flags: 1 simple loop; 2 position given; 4 update only the changed rects;
    8 background kept in an offscreen world (`0x10601c`, built flag `0x10603c`);
    0x10 use the resource position; 0x100 redraw from y 0x6c.
  * `callback(frame)` runs after every frame. A non-zero result ends the animation.
  * Returns -1 or -2 on errors, else the abort or callback code (a word).

### Mohawk libraries

* `MohawkInit` starts the libraries: os, mem, file, timer, res, config. Then it starts
  audio in the "Data" folder. Each library registers its quit routine with `_atexit`.
  The bits left in the mask go to `ErrorHandler`.
* It then allocates the sound item table and saves the master volume.
* `OpenMohawkRsrcVerify` / `CloseMohawkRsrc` work on a {long refnum; C name} record.
  Any error is fatal (`Terminate`).
* `SwitchOut` / `SwitchIn` handle suspend/resume: menu bar, colours, volume, and pausing
  or resuming the audio.

### Sound items

* **Table**: the handle `0x106094` holds up to 0xc8 records of 0xe bytes, numbered from 1.
  * +0 audio ref (`audioCreate`), +4 resource ('snd ' or 'MIDI'), +8 resource id.
  * +a type: 0 sound effect, 1 music.
  * +c use count.
* **Globals**:
  * Highest item: `0x106082`.
  * Playing: sound `0x106086`, music `0x106084`.
  * Finished refs to unprepare: `0x10607a` (sound) and `0x106076` (music). They are set
    by `PoPAudioCallBack`.
  * Music cue: `0x106092`. Paused: `0x106080`. Saved master volume: `0x106074`.
  * Resource files: `0x10608e` ('snd ') and `0x10608a` ('MIDI').
* **CreateSound(item, id, isSnd)** reuses or creates an item; item -1 means a free slot,
  found by `FUN_0004d446`. `SetSoundItem` starts an item and stops the item of the same
  kind first. `ResetSoundItem(0)` stops everything.
* `GetSndItemForId` compares the zero-extended item fields with the sign-extended
  arguments. Ids >= 0x8000 never match.

### Misc

* **Trap cache**: `GetPOPTrap` returns a trap address, or 0 when it is unimplemented.
  `FUN_0004ddfa` caches these Pascal entry points:
  * `0x1060a8` SectRect, `0x1060a4` UnionRect, `0x1060a0` EmptyRect.
  * `0x10609c` ClipRect, `0x106098` GetKeys.
* **Debug helpers**: `CheckCheatModes`, `DrawTestMarker`, `DrawFootmark`,
  `DrawCollideMark`, and `FUN_0004e40c` (wait for a key).
* **"Go to level" dialog**: `GoToLevelDialog`, `GetNumber` (DLOG 0x721),
  `GetEditTextNumericValue`, and `GetNumberFilter`.

### Rooftops (CODE 23, level type 5)

* **Piece procs**: `DrawRoofFloor`, `FUN_0006e3c2`, `DrawRoofGlass`, `DrawRoofPillar`,
  `DrawRoofShip`. `DrawRoofLedgeInBack` and `DrawPillarInBack` are their tests.
* **Rooms**: in the normal rooms, rooms other than 0x13/0x10/0xf, the floor has a ledge
  in the back. Rooms 0x13 and 0x10 have water and the ship.
* **Splash**: a character falls into the water below the roofs.
  * `gSplashChar` 0x104e58, `gSplashFrame` 0x104e5a (frames 1..6), x 0x104e5c,
    "high" 0x104e5e.
  * Shape and y tables per frame: 0x104d72 and 0x104d80.
  * `FUN_0006e51e` starts the splash (through `FUN_0006e806`). After 7 frames it kills
    the kid (death method 0xf).
  * `RoofMakeWaterTestRect`, `SetupSplash`, `seg23_06e646` (clip), and `RoofWatchOpps`
    (opponents).
* **Trobs**:
  * Glass: `AnimRoofGlass`, `TriggerGlass`. Falling glass mob: `ATMGlass`,
    `FUN_0006f2a0`, `DrawObjGlass`; path 0x104d42.
  * Pillar water: `AnimRoofPillar`, `TriggerRoofPillar`.
  * Ship: `AnimRoofShip`, `TriggerRoofShip`, `RoofClimbingIntoShip`,
    `RoofHangingFromShip`, `FUN_0006f110`.
* **End of the roof**: `seg23_06e69e` runs when the kid passes x 0x332. He falls and his
  life drains (death method 0xe).

## Porting notes

* **Virtual calls** use `call_ptr(VMETHOD(obj, off), ...)` in C argument order.
  * The rect returned by `shapeDraw` is the caller's local at A6-0x1c (`ENTRY_LINK` frame).
  * KBAnimation's `shapeDraw` returns that pointer, and DrawIt fills it.
* **Rect traps**: SectRect, UnionRect, EmptyRect and ClipRect are called through the
  cached pointers (`sect_rect` and the other helpers).
* **Own argument slots**: `CreateSound` (item, when -1) and `SetVolumeLevel` (level)
  write their own argument slot (C convention, so the slot is compared). The port writes
  `args_` directly. That is the real slot when the function is entered from 68k code, and
  the reserved slot when it is called from C. No manual glue is needed.
* **GetNumber / GetNumberFilter** are Pascal with a Boolean byte result. They are
  `"conv": "manual"`: the glue writes only the high byte of the slot, like `move.b`.
  `ENTRY_LINK_S` reserves the slot when GetNumber is called from C (`GoToLevelDialog`).
* **MohawkInit register mirroring.** `fileGetDir` copies a local FSSpec that the trap
  only partly fills into the library globals. The bytes it leaves come from stack residue,
  which includes the D5 the earlier library inits saved with `movem`. The original's D5
  is the "still to initialise" mask (0xbc, 0x80...).
  * The port mirrors the original's D3/D4/D5/A2 in the CPU registers before the library
    calls, and restores them on return.
  * Without this, `MohawkInit` mismatched on 2 bytes (0x16d4d7, and 0xdfd191 in the
    globals).
* **PC-relative strings** (`fn_address(FN_x) + delta`):
  * "Data" (MohawkInit +0x1ea) and "Seq %d, Pos %d" (CheckCheatModes +0x7e).
  * "%d" (GetNumber +0x144) and "Enter level number..." (GoToLevelDialog +0x6e).
  * The audio error formats: PauseAudioSystem +0x80, ResumeAudioSystem +0x62,
    OpenMohawkRsrcVerify +0xaa.
* **Kept quirks**:
  * `seekFrame` runs the script for (target - old frame) frames even after rewinding to 0.
  * `CombineBounds` merges only 23 of the 24 rects in its first loop.
  * `DestroySoundItem`, `PurgeSounds` and `FUN_0004d446` keep pointers into the unlocked
    sound handle across calls.
  * `SetSoundItem` returns early without unlocking the handle.
  * `KBAnimation::create` leaves the 'ANI ' locked.
  * `FUN_0004e40c` compares a zero-extended word with -1/-2, so the test is always false.
  * `DrawObjGlass` writes 0 (the low word of `clr.w D0`).
* **Return sizes** (override file):
  * ret 0 where D0 is garbage and every caller ignores it (callers checked in Ghidra):
    draw, CombineBounds, DestroySoundId/Item, ResetSoundId/Item, Pause/ResumeAudioSystem,
    SetSoundItem, UnprepareAudioRef, CheckCheatModes, DrawRoofFloor, FUN_0006e806,
    TriggerGlass.
  * ret 2 where only the low word is defined and used: PlayTheAnimation, the
    create/prepare/neglect/isRunning methods, FUN_0004d446, SoundIdOn, SoundItemOn,
    StopAudioRef, GetEditTextNumericValue, FUN_0004e40c, DrawRoofLedgeInBack.
  * ret 4 for the methods returning `this`/pointers/longs: ctors, `__sd`, maintain,
    testPixel, getBounds, getFrame, shapeDraw, shapeTestPixel, and RectArea.
  * Methods ending with `clr.w D0` return the caller's high word (`cpu_get(R_D0)`).
* **Parameter fixes** (unit functions): the method prototypes use longs (create, setPos,
  neglect, prepare, notifyApp, shapeDraw, shapeTestPixel, soundControl), and RectArea
  takes one long.
* **Overrides outside the unit**:
  * `BuildOffGWorld` (seg17b): `params [[4,4],[8,4]]`. It reads its second argument (the
    rect) at 0xc(A6); the index had one parameter.
  * `__dt__8fileSpecDv` (seg21a): `params [[4,4]]`. Callers push `this`; the callee
    ignores it, but the stack layout must match.

## Verification

* `tools/check.sh`: no mismatch.
* Most functions are reached only nested inside other ported calls, which are not
  checked. Extra runs used `POP2_NATIVE_ONLY` = this unit minus their callers, level by
  level of the call tree:
  * minus PlayTheAnimation;
  * then minus the methods and sound routines that call others;
  * then minus `__dt__11`/`DestroySoundItem`;
  * then minus `__dt__10`/`free`.
  These runs used the scenarios Level 4, 10, 15 (cut-scenes), newgame (rooftops), and
  intro.
* **Temporary test hooks** (not committed) called the remaining functions through their
  68k entries:
  * in a KBAnimation's `maintain`: testPixel over a grid, the base-class trivial methods,
    seekFrame rewind (InitLayer), a plain tAnimation create/delete, sound item queries,
    debug markers, GetNumberFilter with key events, SwitchOut/SwitchIn, SetVolumeLevel;
  * in the rooftop per-frame hook: splash states, the pillar/ship/glass piece procs for
    kinds 0/5/1, the trob animations and triggers, ship tests, falling glass, and the end
    of roof;
  * the dialogs with scripted keys: GoToLevelDialog (in range, out of range), GetNumber
    (OK, Esc), GetEditTextNumericValue, FUN_0004e40c.
* **All 114 functions were verified at least once, with no mismatch.**
* **Not executed, reviewed against the disassembly only**:
  * the private-copy shape path of `KBAnimation::prepare` (needs `0x10601a` set by an NIS);
  * the `ErrorHandler` and `Terminate` error paths (MohawkInit, OpenMohawkRsrcVerify, audio
    activation);
  * the empty-text path of `GetEditTextNumericValue`;
  * `CheckCheatModes` with cheat mode on.
