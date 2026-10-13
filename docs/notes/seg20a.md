# seg20a — CODE 20 "Mohawk Libs 1", first part (0x5b134–0x5d8ae)

All 94 functions are ported in `src/game/seg20a_mohawklibs1.c`. Signature fixes are in
`tools/sig_overrides/seg20a.json`.

## What the unit does

### THINK C Toolbox glue (0x5b134–0x5ba1e)

- `tc_CtoPstr`/`tc_PtoCstr` convert strings in place (C convention, return `s`).
- `tc_SysEnvirons` uses the trap when `GetTrapAddress($90)` differs from Unimplemented.
  Otherwise it fills the 16-byte record by hand and returns envNotPresent (-5500).
  - The keyboard types are a 5-byte table inside the code (0x5b19e).
  - sysVRefNum comes from GetFCBInfo(SysMap) → PBHGetVInfo → OpenWD('ERIK'), using a
    0x7a-byte block pushed on the stack.
- `tc_Gestalt` uses `_Gestalt` ($A1AD) when it is implemented. Otherwise it uses a
  {selector, offset} table at 0x5b334 whose handlers jump back to 0x5b31a:
  'vers' 'mach' 'sysv' 'proc' 'fpu ' 'qd  ' 'kbd ' (byte table 0x5b46a) 'atlk' 'mmu '
  'ram ' 'lram'. An unknown selector gives -5551; an unknown keyboard gives -5550.
- Parameter-block glue `tc_PBxxx(pb, async)` pops its arguments and traps with the async
  bit when the Boolean (high byte of its word) is set. The HFSDispatch ones (OpenWD 1,
  CloseWD 2, GetFCBInfo 8, GetCatInfo 9, SetCatInfo 10) load D0 with the selector.
- High-level File Manager glue builds a parameter block in a `link` frame:
  - `tc_FSOpen` tries OpenDF ($A060, selector $1A) and falls back to PBOpen on paramErr.
    It writes the refNum even on error.
  - `tc_FSRead`/`tc_FSWrite` share one body (D1 = sf/st). They read and write at the mark.
  - `tc_GetVInfo` computes freeBytes = ((alBlkSiz asr 9) mulu.w freeBlocks) << 9.
  - `tc_GetFInfo`, `tc_GetVol`, `tc_SetVol`, `tc_GetEOF`, `tc_SetFPos`.
  - `tc_Create` runs PBCreate, then PBGetFInfo and PBSetFInfo for the type and creator.
  - `tc_SetFInfo` ignores the result of its PBGetFInfo.
- Memory and misc glue: `tc_GetPtrSize`/`tc_GetHandleSize` return 0 on error.
  - `tc_NGetTrapAddress`, `tc_StripAddress` (masks with Lo3Bytes on 24-bit-only ROMs),
    `tc_HandToHand`, `tc_PurgeSpace`, `tc_FlushInstruction/DataCache` (HWPriv 1/3).
  - `tc_CountAppFiles`/`tc_GetAppFiles` read AppParmHandle. GetAppFiles copies entries
    1..index into the record, so the last copy is entry `index`.
  - `tc_SetSoundVol` makes an immediate Control call to the sound driver (-4, csCode 2).
  - `tc_NumToString` is Pack7 selector 0.
  - `tc_GetIndString` ('STR#') and `seg20_05b9e0` = GetIndPattern ('PAT#', 8-byte entries).
    Both are Pascal and pop 8 bytes.
  - `tc__Pack2_dispatch` pushes D2 under the arguments and traps $ADE9 (auto-pop).
    `tc_DIBadMount` (D2 = 0) jumps to it.

### Mohawk working directories

- `HGETVOL(volName, VAR vRefNum, VAR dirID)` uses PBHGetVol.
- `OPENWD(vRefNum, dirID, procID, VAR wdRefNum)` uses PBOpenWD.
- `CLOSEWD(wdRefNum)`.

All three use a 0x34-byte parameter block in their frame.

### Mohawk audio API (handle-based objects, Symantec C++)

Globals:
- `gAudioError` 0x107e8e (A5-0x172)
- `gAudioInitCount` 0x107e90
- `gAudioClientOff` 0x107e92: offset of the 12 client bytes reserved with
  `osReserveClientBytes`

Client bytes, at `osGetClientBytes(-1) + gAudioClientOff`:
- +0 initialised
- +2 active
- +4 output enabled
- +6 MIDI out device
- +8 wave out device
- +a head of the object list (memory handle)

Audio object (memory handle, `memXlat`):
- +0 vtable
- +4 next / +6 prev handle
- +8 own handle (long)
- +c data handle
- +e client
- +12 device enabled
- +14 paused
- +16 playing
- +18 prepared
- +1a device
- +1c notify proc / +20 instance

The vtables are `audioObj` 0x107e4a (A5-0x1b6) and `midiObj` 0x106e56 (A5-0x11aa). Slots:

| Offset | Method | Offset | Method |
| --- | --- | --- | --- |
| +0 | destroy | +24 | prepare |
| +4 | deviceEnable | +28 | releaseLoop |
| +8 | devicePrepare | +2c | resetLoop |
| +c | deviceStart | +30 | resume |
| +10 | deviceStop | +34 | seek |
| +14 | deviceUnprepare | +38 | start |
| +18 | getPosition | +3c | stop |
| +1c | notify | +40 | unprepare |
| +20 | pause | | |

The methods are Pascal, with `this` pushed last.

- `audioInit` needs at least one of the os/mem/timer/file/config/res libraries at version
  0x100 or later; the test is an OR chain. It initialises `mmaudio` and `midiMap`.
- `audioCreate` checks the 'MHWK' header and creates a midiObj ('MIDI') or a waveObj
  ('WAVE'). It inserts the object at the head of the list.
- `audioActivate` enables or disables every object's device through `deviceEnable`.
  - On a failure it walks the failing object's `prev` link. The original re-reads that
    link from the failing object on every iteration (kept).
- Every `audio*` call validates the handle, then calls the method through the vtable.
  - Errors: 0x29cc..0x29d1, 0x29fe/0x29ff, 0x2a00..0x2a07.
- `audioObj::notify` calls the client proc `proc(handle, msg, instance)` (Pascal) under the
  client's instance (`osSetInstance`). The msg is a byte in the caller's frame:
  2 pause, 3 resume, 4 start, 5 stop.
- `midiObj` (0xda + 16 × tracks bytes) is a Standard MIDI File player. `create` parses the
  MThd, MTrk and 'Prg#' chunks, then seeks to 0 and resets the loop. Fields:
  - +24 data, +28 end offset
  - +2c 'Prg#'
  - +30 midiMap, +34 device caps, +62 caps flags
  - +66 MIDIHDR
  - +9e timer, +a2 period, +a4 start time, +a8 elapsed
  - +b8 position
  - +c2 first delta
  - +c6..+cc loop state
  - +d2 tracks done, +d4 format, +d6 division, +d8 track count
  - +da tracks
- Playback runs on a Mohawk timer (`timerCreate(this, timerProc, 0, period, client)`) and
  `advance` (seg11a).
- `seek` rewinds all tracks when seeking backwards. It then advances to the position and
  returns 0x29cf past the end.
- `cachePatches(op)` builds a 128-word patch table from the 'Prg#' chunk and passes it to
  `midiMapCachePatches`.
- `midiMapClose` runs on the last close of a device when a reset SysEx resource exists:
  1. It sends the resource with `midiOutLongMsg`.
  2. It busy-waits for MHDR_DONE.
  3. It closes the midiOut device.
  4. It unlinks the map from the device's list and frees it.

## Porting notes

**Manual glue (`RESULT_SLOT_GLUE`).** Some Pascal functions return a D0 that differs from
their result slot. The C function returns the slot value and leaves the exact D0 in
`s_d0`; the glue writes the slot and sets D0. They are `"conv": "manual"`:
- `tc_SysEnvirons`: the fallback path.
- `tc_GetPtrSize`/`tc_GetHandleSize`: the error code stays in D0.
- `tc_NGetTrapAddress`: D0 is the trap's D0.
- `tc_GetVInfo`: D0 = freeBytes.
- `tc_GetFInfo`: D0 = BlockMove's result.
- `tc_DIBadMount`: D0 is whatever Pack2 leaves.

`tc__Pack2_dispatch` (register convention) is manual too. Its glue leaves the stack
popped by the package.

**`seg20_05b9e0`** (GetIndPattern) pops its 8 argument bytes. It is `"conv": "pascal",
"purge": 8, "retslot": false`.

**Prototypes kept from the index.**
- `audioStart` takes three longs (instance, notify proc, object), pushed as six words.
- `getPosition__7midiObj` takes `this` as two words.
- The port rebuilds the longs, so callers in other units keep their prototypes.

**Trap glue that pops its arguments first** runs the trap with SP on the caller's result
slot (`ENTRY_POPPED`). `tc_SetSoundVol` re-pushes its return address, so its `link` frame
sits 2 bytes above the usual one. The parameter blocks live at their original frame
offsets.

**Entry D0.** The registers the original only partly loads (`move.b` of the async flag,
`move.w` of a trap number) keep the entry D0 (`cpu_get(R_D0)`).

**Device counts.** `midiOutGetNumDevs`/`waveOutGetNumDevs` (seg11a) are C functions that
write the caller's result slot. They are called through their jump-table entry with a
reserved slot (`num_devs`), which works in both builds.

**Registers that leak into the heap.** `audioInit` holds D3 = 0x100 and A2 from its first
query onwards, and A3/A4 before its memset. Deeper 68k code copies the saved registers into
the heap. This showed as a mismatch at call 1. `KEEP_REGS()`/`set_reg_w` reproduce the
registers and restore them on return.

**Original quirks kept.**
- The `audioActivate` failure loop.
- `audioDestroy` returns the stale `gAudioError` on success.
- `seek` uses word arithmetic for the last advance.
- `midiMapClose` busy-waits.
- `tc_Gestalt` with selector 0 matches the table terminator, and the original jumps into
  the table. The port reports this and returns -5551.
- The 'fpu ' probe (FNOP/FSAVE) cannot run here and is reported as "no FPU". It is
  unreachable: HWCfgFlags says no FPU.

## Signature overrides (`tools/sig_overrides/seg20a.json`)

All of them are for functions of this unit:
- `"conv": "manual"`: `tc_SysEnvirons`, `tc_GetPtrSize`, `tc_GetHandleSize`,
  `tc_NGetTrapAddress`, `tc_GetVInfo`, `tc_GetFInfo`, `tc_DIBadMount`,
  `tc__Pack2_dispatch`.
- `seg20_05b9e0`: `"conv": "pascal", "purge": 8, "retslot": false`.

## Verification

- `JOBS=4 SECS=2400 tools/check.sh`: see the final report in the commit/hand-off.
- `POP2_NATIVE_ONLY=<this unit>` with `CALLS=100` on all scenarios: no mismatch, 39
  functions verified directly.
- The midiObj methods are called from this unit through vtables, so their checks are
  nested. Runs with `POP2_NATIVE_ONLY` limited to the methods (and audioGetState) verified
  them: cachePatches, deviceEnable, deviceStart/Stop/Unprepare, start, stop, unprepare,
  midiMapCachePatches, midiMapClose.
- A temporary test hook (not committed) called the remaining functions through their 68k
  entries, with the check counters reset. Each run showed no mismatch. It covered:
  - all PB/File Manager glue (mostly error paths; the game covers the success paths);
  - GetIndString/GetIndPattern, NumToString, HandToHand, App files, PurgeSpace, cache
    flushes, SetSoundVol, DIBadMount;
  - OPENWD/CLOSEWD;
  - `tc_SysEnvirons` and `tc_Gestalt` fallbacks, with the trap addresses patched to
    Unimplemented: all table selectors, an unknown selector and an unknown keyboard;
  - audioPause/Resume/ReleaseLoop/GetPosition/GetError/QueryInit/UseMidiOut/UseWaveOut;
  - seeks forwards, backwards, past the end and to -1;
  - audioCreate/create__7midiObj on invalid data;
  - audioActivate off/on;
  - audioQuit then audioInit.

Never entered through its own 68k entry: `tc__Pack2_dispatch`. Its only caller is
`tc_DIBadMount`, which calls the C body (verified with DIBadMount). It was reviewed
against the disassembly.

Reviewed against the disassembly only:
- the `audioActivate` failure loop;
- the `create__7midiObj` duplicate-'Prg#' error;
- the FSFCBLen < 0 / ROM85 < 0 branches of `tc_SysEnvirons`;
- the `midiMapClose` SysEx reset path.
