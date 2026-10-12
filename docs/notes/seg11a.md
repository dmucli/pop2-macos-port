# seg11a — CODE 11 "Mohawk Libs 2", first half (0x3d4e4–0x40aba)

All 118 functions are ported in `src/game/seg11a_mohawklibs2.c`.

## What the unit does

The music and sound layer, below the portable audio code of CODE 19/20 (`midiMap*`,
`waveObj`, `mmaudio*`) and above the encrypted music driver code resource.

* **Live notes** (instrument playback mode, `gMusicMode` 0x107052 == 0x64):
  * Two queues of 100 note records (16 bytes each), allocated by `SetupMIDIPort`:
    note-ons at `*0x106e1c` and note-offs at `*0x106e20`.
  * Record fields: +0 program, +2 note, +4 duration (0x4e20 for note-on, 0 for note-off),
    +6 velocity, +8 channel, +a time (TickCount + 1 − `0x106e2c`), +e in-use flag,
    +f padding. The padding is stack garbage of the caller's record; the checker
    ignores it by design.
  * `QNoteOn`/`QNoteOff` copy a record into the first free slot. The busy flag is
    `0x106e54`.
  * `HandleLiveMusicVBLTask` (sound VBL) plays the due records with
    `MDH_PlayNote`/`MDH_StopNote`.
  * Per-channel program and volume are bytes at `0x106e30` and `0x106e40`.
* **Synth MIDI parser** (the only "midiOut" device):
  * `midiOutShortMsg` and `midiOutLongMsg` feed bytes to `synthMidiData`.
  * Parser state: status `0x106ee6`, running status `0x106ee8`, bytes still expected
    `0x106fea`, message `0x106ee2..e5` (channel, data1, data2, pad).
  * Message lengths come from the byte tables `0x106eb2` (channel messages) and
    `0x106eba` (system messages).
  * Each complete message goes to its handler in the long table `0x106ec2`, indexed by
    `(status >> 4) & 7`: `synthNoteOn`, `synthNoteOff`, `synthControlChange`,
    `synthProgramChange`. These call `ProcessMidiNoteOn/Off`, `ProcessController` and
    `ProcessProgramChange`.
  * Patch cache: `midiOutCachePatches`, 128 words at `0x106eea`.
  * `ProcessMidiEvents` is the MIDI Manager read hook. `SetupMIDIPort` installs it only
    when MIDI input is requested, which the game never does.
* **midiObj** (Symantec C++ methods, Pascal convention, `this` pushed last): a Standard
  MIDI File player.
  * `timerProc` is the timer callback. It calls `advance` (moves the Fixed song position,
    then runs `trackAdvance` for every due track) and re-arms the timer with
    `calcTime2Next` through `timerCreate`. At the end of the song it calls the object's
    vtable method +0x1c.
  * `calcTempo` handles the meta tempo event and `getVLQ` reads variable-length numbers.
  * Meta event 6 markers handle loops: looping calls vtable +0x34 from `advance`.
  * The object layout is in the comment above `advance__7midiObj`. The 16-byte tracks
    start at +0xda: +4 pointer, +8 done, +a delta, +e running status.
* **Wave out**: two devices of 0x2a bytes at `0x106ffe`.
  * Fields: +0 rate, +4 callback, +8 instance, +c open flags, +10 position, +14 queue,
    +18 header playing, +1c/+20 loop start/end, +24 loop count, +26 count,
    +28 flags (bit7 paused, bit6 break loop, bit5 open).
  * `waveOutWrite` queues WAVEHDRs.
  * The sound VBL callback `FUN_0003eecc` starts the next header with
    `LowLevelPlaySample`.
  * `HS_CallbackForPlaySample` (sample done) advances the queue, handles loops and calls
    the client callback with WOM_DONE (0x3bd).
* **Music driver front end (MDH)**:
  * `InitSoundMusicSystem` loads the driver code with `GetDriverCodeResource`:
    'Jnth' as is, or 'MDRV' decrypted by `DecryptHandle`/`Decrypt` (seed `0x107104`)
    and decompressed. It locks the driver and keeps its entry point in `gDrvEntry`
    (`0x1070dc`).
  * Every driver request is `drv(selector, args...)`, a C call through that pointer.
  * `MusicDriverHandler(arg, selector)` (Pascal, long result) dispatches the requests
    of the sound code: 0x64..0x69 sound data, 0xc8/0xc9 notes, 2 end song, and others.
  * Song state: current song `0x1070e4` (−1 = none), paused `0x1070e6`, start time
    `0x1070e8`, pause time `0x1070ec`.
  * Fade state: active `0x1070f2`, done `0x1070f3`, out `0x1070f4`, level `0x1070f6`,
    step `0x1070f8`, ticks `0x1070fa`/`0x1070fc`. `AutoFadeHandler` runs it from
    `HandleMusicVBLTask`.
  * `HS_SetupSystem` initialises the sound system and switches to instrument mode.
    `FUN_0003ebf6`/`seg11_03ebfc` pause/resume the music, and `seg11_03ec02` shuts it
    down.

## Porting notes

* **Pascal functions with a result slot whose D0 is not the result** (the midiOut*/waveOut*
  API, `MusicDriverHandler`, `ProcessMidiEvents`) are `"conv": "manual"`.
  * Their glue, the `PGLUE` macro, stores the result above the arguments and pops the
    arguments.
  * For the ones called from C (`MusicDriverHandler`, `waveOutPause`), `ENTRY_LINK_S`
    also reserves the caller's result slot.
  * `midiOutCachePatches` leaves the slot unwritten for op 0 or op > 4, as the original
    does.
* **Functions that write their own argument slots**:
  * `MS_SetMemory` and `MMB_SetMemory` count down to 0.
  * `synthMidiData` advances the pointer and counts down.
  * `RemapWaveDevice` stores the remapped device.
  * All four are manual, with a glue that writes the final values back.
* `midiOutGetNumDevs`/`waveOutGetNumDevs` are C-convention functions that store their
  result in the caller's slot at 8(A6). Their glue writes that slot.
* **Extra pushed arguments**:
  * `HandleMusicVBLTask` pushes its argument for `AutoFadeHandler`.
  * `MusicDriverHandler` pushes the argument for `MDH_EndSong`, `MDH_IsSongDone`,
    `MDH_FreeSong` and `MDH_SetMIDIClock`.
  * `ProcessMidiNoteOn` and `synthNoteOff` push a third word for `ProcessMidiNoteOff`.
  * The port pushes these by hand (`WITH_PUSH32`/`WITH_PUSH16`).
* **Delayed pops**: several MDH functions call `FUN_0003f5d4` or `FUN_0004239a` while
  the previous driver call's arguments are still on the stack. The port keeps them pushed
  (`drv*_keep`).
  * `MDH_EnableMIDIInput` reuses `ChangeSystemVoices`' pushed arguments as the driver
    arguments. The second argument is `mix << 16` | the high word of the saved D6. The
    port writes the saved D6 image and builds that stack.
* `MDH_GetErrors` needs D1 from the driver, but `call68_addr` restores the registers. It
  enters the driver through a native "jump" thunk that sets a "capture" thunk as the
  return address (`drv2_d1`).
* `ProcessMidiEvents` swaps A5 with its refCon (`exg D0,A5`). The port sets the CPU's A5
  around its body and returns D0 = refCon.
* `trackAdvance` sends D7 as the short message. If the status is unknown, D7 is the
  caller's register (`cpu_get(R_D7)`).
* Ghidra decodes the memory-indirect `lea ([-0x11e4,A5],D0.l),A1` of `QNoteOn`/`QNoteOff`
  correctly as post-indexed (`*queue + i*16`). The switch tables of
  `MDH_ChangeOutputQuality` (0x3fe24) and `InitSoundMusicSystem` (0x4083c) were decoded
  by hand.
* The `MusicDriverHandler` selector tree was decoded by hand. Selectors: 0/1 begin song
  (loop flag), 2 end, 3 done?, 4 time, 5/6 flags, 7 driver 0xd, 10/11 fade out/in,
  12 get fade, 13 set fade, 14 quality, 15 load, 16 free, 17 errors, 18..23 queries,
  100..105 sound data, 200/201 note on/off, 202 all notes off, 203 MIDI input.

## Signature overrides (`tools/sig_overrides/seg11a.json`)

All of them are for functions of this unit.
* `HandleLiveMusicVBLTask`: `params [[4,4]]`. It reads 8(A6), and its caller pushes a
  long.
* `synthNoteOff`: `params [[4,2],[6,2]]`. It reads 0xa(A6), the third byte of the
  message long.
* `midiOutGetDevCaps`: `ret 2`, `retslot true`. The index had no result slot. Without it
  `midiMapGetDevice` saw a garbage result, and MIDI music never started.
* `"conv": "manual"` with `ret 0`: the Pascal functions above. `ProcessMidiEvents` keeps
  `ret 4` (D0 = refCon exactly).
* `"conv": "manual"`: `MS_SetMemory`, `MMB_SetMemory`, `synthMidiData`,
  `RemapWaveDevice`, `midiOutGetNumDevs`, `waveOutGetNumDevs`.
* Return sizes:
  * `ProcessController` and `DecryptHandle`: 0 (D0 is garbage, unused).
  * `SetupMIDIPort`, `HS_SetupSystem`, `seg11_03ebfc`, `FUN_0003f608`,
    `BeginSongFromMemory` and `ChangeSystemVoices`: 2 (`move.w` result; callers use the
    word).
  * `FUN_0003ebf6`, `FUN_0003f5d4` and `MDH_IsSongDone`: 4 (exact long D0, used by the
    callers).
* `AddWaveHeaderToQueue` returns 0x10 when the header is not NULL: D0 holds the `lsl`
  count.

## Verification

`tools/check.sh` (all scenarios) reports no mismatch.
* 42 functions of the unit are verified directly.
* Extra runs verified more functions individually:
  * `POP2_NATIVE_EXCLUDE` of the callers, so the callees are entered from 68k code.
  * `POP2_NATIVE_ONLY` = this unit, so the calls from other ported units are not nested
    in their checks.
* A temporary test hook (not committed) called the unreachable functions through their
  68k entries with chosen arguments. That covered:
  * the waveOut/midiOut queries, LongMsg, controller/program/running-status/realtime
    short messages, and patch caching (ops 0–5, bank ≠ 0);
  * MIDI Manager packets for `ProcessMidiEvents`;
  * the MDH functions in instrument mode, in mode 2, with the music paused and with
    mode 0;
  * every `MusicDriverHandler` selector group;
  * `AutoFadeHandler` fade out and fade in;
  * the song-done callback in `HandleMusicVBLTask`.
  None of these showed a mismatch.

Not executed in any run, reviewed against the disassembly only:
* `MDH_SetMasterFade` and `MDH_AutoFadeSoundIn` past their early returns: they busy-wait
  for 5 driver ticks, which never pass in a nested call.
* `BeginSongFromMemory` past its early return.
* The MIDI Manager paths of `SetupMIDIPort` and `FUN_0003dbc2`.
* `MDH_ChangeOutputQuality` 1..7 except 2.
