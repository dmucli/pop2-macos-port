# MDRV — the SoundMusicSys music driver ('MDRV' 11)

The whole driver is ported in `src/game/mdrv_driver.c`. Its entry points are listed in
`tools/coderes/mdrv.json`. The Ghidra program `mdrv11.bin` now has its 92 routines named
like the C functions (CamelCase).

## What it is

SoundMusicSys by Halestorm (Steve Hales): hand-written 68k assembly that contains:

* a Standard MIDI File sequencer;
* a "live" instrument player for notes sent by the game;
* a sound-effect player;
* a software mixer that writes 8-bit output.

seg11a's `GetDriverCodeResource` loads the code. It reads 'MDRV' 11, decrypts it, and
LZSS-decompresses it into a handle of 0x78f0 bytes. The game then calls the code through
`gDrvEntry` as `drv(selector, arg)` (C convention).

The code itself runs from 0 to 0x45fc. The globals and constant data (tables, Pascal error
strings, a SndDoubleBufferHeader, a VBLTask) follow at 0x4600. The code reaches them as
`(d,A4)`, with `A4 = code + 0x4600` (`lea (0x45f2,PC),A4`). In the port, `G(off)` is that
address and `CODE(off)` is a code address. Both are computed from
`fn_address(FN_MDRV_Entry)` (`MDRV_BASE()`) at every entry.

The driver has no self-modifying code. It does build two small stubs in the system heap:

* 0x1f0a: `bsr.s *+4; dc.l VBLTask; movea.l (A7)+,A1; movea.l (A1),A1; jmp (A1)`;
* 0x39de: `jmp CardInterrupt`, followed by a BlockMove onto itself to flush the cache.

The driver also reads code bytes as data once: the CPU speed test reads at 0x3f8.

## Entry points (tools/coderes/mdrv.json)

| off | name | how it is entered | port |
| --- | --- | --- | --- |
| 0x0000 | `MDRV_Entry` | `drv(sel, arg)`, C, 2 longs (callers push 1–3; it reads 4(SP) and 8(SP) in all cases) | generated glue; the function restores D2–D7/A0–A6 and sets D1 |
| 0x080a | `MDRV_DoubleBack` | Sound Manager doubleBack, `pascal (chan, SndDoubleBuffer*)`, `cpu_call_irq` | `conv pascal`, ret 0 |
| 0x0924 | `MDRV_VBLTask` | VBL task, through the stub (all registers preserved) | `conv manual`, `glue_MDRV_VBLTask` |
| 0x3c78 | `MDRV_CardCompletion` | PAS16 driver callback, pascal, 4 longs | `conv pascal`, ret 0 (D0 is kept) |
| 0x3cb8 | `MDRV_CardInterrupt` | PAS16 interrupt task, through the `jmp` stub | `conv manual`, `glue_MDRV_CardInterrupt` |

## Entry and selectors

The entry does the following, in order:

1. Saves D2–D7/A0–A6.
2. Pushes the globals `(0,A4)` and `(4,A4)` (selector and argument), so it can be re-entered.
3. Stores the new selector and argument there.
4. Clears the result word `(8,A4)`.
5. Jumps through a table of 38 `bra` at 0x3c.

It returns D0 = the result word (zero-extended). Selectors 0xf and 0x1c–0x21 return a long
in D0 instead (`bra $e0`). An unknown selector returns -21.

| sel | routine | meaning |
| --- | --- | --- |
| 0, 1 | 3e68 | load 'SONG' id (and play it for 0) |
| 2 | 182c | restart the song |
| 3 | 1c0 | reload the instruments |
| 4 | 176a | song playing? (-1/0) |
| 5 | 1744 | end the song |
| 6 | 3322 | free every voice |
| 7 | 42d0 | free the song |
| 8 | 4358 | shut down |
| 9 / c | 3382 / 333a | note on / off `{inst, key, dur, vel, chan}` |
| a / b | 90a / 914 | pause / resume the output |
| d | 45c | song loop flag |
| e | 298 | last error: value in D0, Pascal message in **D1** |
| f / 10 | — | get / set the buffer counter |
| 11, 12, 14, 16, 17 | 376a, 393c, 3994, 3884, 38b4 | sound effects: play, stop, done?, stop all, change rate |
| 13 | 2058 | master volume (rebuilds the output table) |
| 15 | 4ac | voices `{notes, mix level, effects}`; opens the output |
| 18 | 352 | quality (see below) |
| 19, 23 | ee, 146 | instrument (live) playback, with all / no instruments |
| 1a | — | returns 1 |
| 1b | 32da | sound effect done callback `{proc, refCon}` |
| 1c–21 | 178e… | song time / length queries |
| 22 / 24 | 4018 / 3fdc | load / unload instrument |
| 25 | 3e6e | song from memory `{SONG h, MIDI h, id}` |

Quality code (selector 0x18):

* low byte 0x16: 22 kHz mixing (370 frames per buffer);
* low byte 0x0b: 11 kHz (185 frames, doubled on output);
* low byte 0: a CPU speed test picks one. It counts loop passes during 5 ticks, re-reading
  Ticks each pass.
* bit 8 sets "average" and bit 9 sets "interpolate". This is the byte at `(0x2e,A4)`, read as
  a word.

The game uses selectors 6, 9, a–c, f, 10–12, 15, 16, 18 (with 0x0b), 1b, 22–24. It never
plays a SONG; its music is notes sent by seg11a's MIDI parser in instrument mode.

## Structure

* **Output**:
  * `OpenOutput` (0x604) asks for SoundDispatch, then for the Sound Manager version.
  * With version 3 or later (always the case in the runtime), it creates a sampled-synth
    channel and starts `SndPlayDoubleBuffer`.
  * The two buffers are 370 frames each; the doubleBack is `MDRV_DoubleBack`.
  * Otherwise it takes over the hardware sound buffer (`$266`, every other byte) with a VBL
    task (`$27E` = $81 marks it).
  * The PAS16 card path is dead: `CardPresent` (0x3a24) begins with `bra` to its failure
    exit.
* **One buffer**, in the doubleBack or VBL task:
  * `OutHalf`/`OutFull` convert the previous mix (words at 0x1724) into bytes through the
    output table `gVolTable`.
  * If SdVolume (`$260`) is 0, the output is zeros instead.
  * `MixBuffer` (0x20da) mixes the next buffer.
  * `SequencerTick` (0x1a4a) runs the song.
* **Voices**:
  * There are 16 slots. Each field is an array of 16 longs (`field + 4*voice`, base 0x24f2).
  * The note voices come first, then the sound-effect voices.
  * The fields are named `vPtr`…`vDoneProc` in the C file.
* **Mixing** (`MixVoice`, 0x219e):
  * It handles the note duration and release, the pitch envelope (a word list with relative
    jumps), and the fade-out of a stolen voice.
  * It then calls one of four mixers: 8-bit plain (0x2262), through an amplitude table
    (0x260c), and the same two counting each sample twice for sound effects (0x2a50,
    0x2e4c).
  * Each mixer has paths for: integer steps, plain, "average", interpolation (two 64K
    tables), sample end in the buffer, loop end in the buffer (with loop counts), and the
    ramps (one silent buffer for a new note, a zero-crossing fade for the old sound).
* **Notes** (`NoteOn`, 0x3382):
  * It handles key splits (by key, velocity or random, with octave retry, or a redirect to
    another instrument).
  * It picks a voice: the same note, then a free voice, then a released one.
  * It sets the pitch from the table at 0x2c56, and the volume table (128 amplitude tables
    of 256 bytes).
* **Songs**:
  * `BeginSong` loads the 'SONG' and its MIDI ('cmid' LZSS, 'Midi' or 'MIDI'), the 'SMOD's,
    and the instruments ('INST' + 'snd ' or 'csnd' (LZSS + delta)).
  * `ScanSongInstruments` first plays the whole song silently to mark the instruments it
    uses.

## Porting notes

* **Register routines**:
  * Register arguments are C parameters.
  * The carry/extend result (`andi #$ee,CCR` / `ori #$11,CCR`) is the int result.
  * The entry's `roxl` takes X. For selector 0x24 X is always clear: the unload path only
    runs `lsr.w #3` of a shifted value, then flag-neutral or borrow-free instructions, and
    the runtime's OS traps keep X.
* **Mixers**: they are register-level transcriptions with gotos to the original labels
  (`STEP` = `add.w D2,D3; addx.w D1,D5`, `(A2,D5.w)` sign-extended, byte and word
  register semantics with `setb`/`setw`).
* **Stack**:
  * Every internal routine starts with `SUB(saved bytes)`: the emulated SP goes down by the
    `bsr` and `movem` of the original.
  * Explicit pushes are mirrored too.
  * So Toolbox traps, the sound-effect callback (`CallDoneProc` → `call_ptr`) and the SMOD
    `jsr` run at the original depth.
* **Flags after traps**:
  * `CloseOutput` takes or skips `DisposPtr` on the flags left by `_VRemove` (`movea` does
    not set flags).
  * `CardClose` chains its steps on the flags of the previous trap.
* **Register leaks reproduced**:
  * `SetQuality` doubles the caller's D4 and stores its low byte (0x1e,A4).
  * `SetMasterVolume` starts from the caller's D7 when nothing lies below the ramp.
  * `SeqNoteOn/Off` use the low word of the track slot address as the note id.
  * The error report leaves the message address in A0. The sequencer, `SetupSong` and
    `CallSMOD` then go on with it as the original does (e.g. "SMOD missing" returns the
    string address as the sample pointer).
* **Quirks kept**:
  * `StopAllSamples` runs its `dbra` once more than there are effect voices.
  * The loop/average mixers add D5 to A2 once more on exit.
  * The sequencer's first `move.l (0x1e94,A4),D0` uses A4 instead of A0.
  * `BeginSong` clears gFromMemory around `FreeSong`, so the previous song's handles are
    disposed even when they came from memory.
* **D1 on return**:
  * Exact for selector 0xe (read by `MDH_GetErrors`), 0xf, 0x10, 0x1b and the time queries.
  * The other selectors return the argument in D1. No caller reads D1 after them.
* **Not reproduced: the stack after one error path.** When `SndPlayDoubleBuffer` fails, the
  original leaves `SndDisposeChannel`'s result word on the stack and returns through it (a
  crash). The port returns normally.
* **Selector 0x18 with code 0**: the speed test counts passes per 5 ticks. In deterministic
  mode the count depends on how often Ticks is read, and the port reads it exactly as often
  as the original.

## Verification

* `JOBS=6 SECS=2400 tools/check.sh`: no mismatch.
  * `MDRV_Entry` and `MDRV_DoubleBack` are verified in 21 scenarios.
  * The intro's calls are all nested inside checked functions.
* The same check with `POP2_NATIVE_ONLY=` the MDRV functions plus `GetDriverCodeResource`
  and `CALLS=10000000` checks every call in all 22 scenarios. No mismatch: 196k entry calls
  and 166k doubleBack calls.
* A cmd-q scenario (shutdown path), checked call by call: no mismatch.
* Audio: `POP2_AUDIOSTATS=1` output (`mixed`, `peak` every 2 s, music playing, peaks up to
  0.39) is identical, line for line, between the port and `POP2_NATIVE_EXCLUDE=<the five MDRV
  functions>` in all 22 scenarios.
* A temporary self-test (not committed) was run with the checker. It drove, through the 68k
  entries, every selector except 8 at first, then a shutdown and re-setup, with:
  * live notes in 11/22 kHz, average and interpolate modes; looping instruments, integer
    steps;
  * sound effects (looping, ending, rate changes, stop), with the voice state forced into
    every mixer path (interpolate/average/amplitude-table effect voices and both ramps);
  * made-up instruments for splits, redirects, octave retry, pitch envelopes, random
    detune, fixed pitch, format-2 sounds and corrupt sounds;
  * three in-memory songs (tempo, time signatures, running status, all channel messages,
    sysex, meta skip, a track ended by its length, looping);
  * the VBL task (with `$266` and the VIA pointer moved to a scratch block), the card
    interrupt and completion with fabricated card state;
  * LZSS and delta decoding compared directly against the original code at 0x43a2/0x440e.

  Every checked call matched. Source coverage of `mdrv_driver.c` (llvm-cov): 87% of regions.

Reviewed against the disassembly only (never executed):

* The PAS16 card path except its two entry points: `CardStart/Open/Close/GetBase/Silence/
  AllocStub`, the Control/Status calls in `CardStop`.
* The hardware VBL output setup and teardown in `OpenOutput`/`CloseOutput`: VInstall, the
  VBL queue search, `$27E`, the VIA.
* `AllocVBLStub` and `FlushCaches`.
* Loading from resources:
  * 'SONG'/'MIDI'/'cmid' and 'csnd' (the decompression helpers themselves are verified);
  * `HandToHand` copies;
  * INST key splits and SMOD conversion (`LoadSndCopy`, `CallSMOD`), because the game's
    INSTs have neither.
* The speed-test branches of `SetQuality` beyond the first, which depend on the host count.
* `MixDblVol`'s plain path with a fractional step, and the "loop count" path of note voices,
  which no API reaches.

## Infrastructure note (outside this unit)

* The Makefile rule for `src/game/gen/glue.c` does not depend on `tools/coderes/*.json`.
  After editing that file, touch a game source (or run `python3 tools/gen_port.py`) to
  regenerate the glue.
