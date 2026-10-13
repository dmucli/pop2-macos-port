# seg11b — CODE 11 "Mohawk Libs 2", second half (0x40ad0–0x43f42)

All 94 functions are ported in `src/game/seg11b_mohawklibs2.c`. Signature fixes are in `tools/sig_overrides/seg11b.json`.

## What the unit does

The unit is part of Broderbund's "Mohawk" support library. It has four parts.

### LZSS compression (0x40ad0–0x40f2e)

The compressor works with a 4 KB window and match lengths from 3 to 0x12.
- `findLongestMatch` returns the match length and stores the code word `(len-3)<<12 | (0x1000-dist)`.
- `doLZSSEncode` writes one flag byte per 8 items; bit set means a literal. It returns -1 when the output is not smaller than the input.
- `doLZSSDecode` is the matching decoder.

Wrappers:
- `LZSS(Delta)(Un)Compress`. The delta forms run `DeltaSample`/`UnDeltaSample` (byte differences) around the codec.
- `CompressHandle` and `DecompressHandle` work on handles whose first long is the original size.
- `DeltaDecompressHandle` does the same for 'csnd' resources.

### Sampled-sound layer (0x40f42–0x42b2c)

The layer plays samples on voices through `MusicDriverHandler` (seg11a).

**Sound slots.** The table of 0x26-byte slots is at `*0x107142`, with the count at `0x10713e`. It is built by `RegisterSounds`/`LoadSamples` from a -1-terminated id list. Slot fields:
- +0 reversed
- +1 from 'snd ' (resource kept)
- +2 locked
- +3 saved HGetState
- +4 handle
- +8 sound header
- +0xc data
- +0x10 length
- +0x14/+0x18 loop start/end
- +0x1c id
- +0x1e rate

`LoadSampleIntoSlot` tries 'csnd' first (delta-LZSS, decompressed into a new handle), then 'snd '. It parses format 1 and format 2 headers.

**Voices.** The 0x7a-byte voices are at `*0x107220`, with the count at `0x10721e`. Voice fields:
- +0 active
- +0x12 VBLs left (-1 = loop)
- +0x16 start tick
- +0x1e the MusicDriverHandler sample block: data, len, rate, loop, &+0x44, id at +0x36, A5 at +0x38, callback at +0x3c (`ProcessCallback`), voice at +0x40
- +0x44 envelope value/result
- +0x4a envelope on, +0x4b envelope called
- +0x4c ended
- +0x4e envelope proc (Pascal `proc(id): Boolean`)
- +0x52 done callback (normally `CallPlaySampleCallback`, which calls `*0x107158` with the id)

MusicDriverHandler codes: 0x64 start, 0x65 stop voice, 0x67 stop all, 0x68 change pitch.

**Starting and stopping sounds.**
- `LowLevelPlaySample` starts a sample.
- `BeginSound`, `BeginSoundReverse`, `BeginSoundSection`, `BeginSoundLoop`, `BeginSoundEnvelope(Proc)`, `PlayTheSample(WithID)` are the front ends.
- `EndSound`/`EndSoundQuick` stop a voice through `EndVoice`. `EndVoice` copies the voice to its frame, clears it, then calls the done callback with the copy.

**Sound VBL task.** `FUN_00041904` builds a 0x12-byte block at `*0x10716a` that holds A5 followed by the VBLTask (`*0x10716e`). It also builds a 12-byte system-heap stub, `bsr; movea.l (sp)+,a1; movea.l (a1),a1; jmp (a1)`, that jumps through the jump table to `DoSoundVB` with A0 = task. `DoSoundVB` does the following:
- calls `HandleMusicVBLTask(ticks)`
- restarts file playback when both file voices have ended
- counts voices down
- calls envelope procs
- plays the sound list at `*0x10715c`: 8-byte entries {id, rate, delay}; id -1 = stop; delay -2 = wait for the voice
- increments the tick counter at `0x10714c`

It does all this under the zone saved at `0x107172` and the busy flag at `0x107176`.

**File playback.** `StartFilePlayback(spec, rate, size)` opens the file with `OPENWD(…,'ERIK',…)` and `FSOpen`. It double-buffers the file into `*0x10717e`/`*0x107182` with voices 0x8001/0x8002. `PlayBuffer` (Pascal done callback) switches between the buffers. `ReadBuffer` refills a buffer and wraps at EOF. `FUN_00042956` is the idle task. `FUN_000427b6` stops playback.

**Other functions.**
- `FUN_00041b2e`/`FUN_00041b76` suspend and resume the whole layer (switch out/in).
- `FUN_00041ae8` shuts it down.
- `deviceCallback__7waveObj` is the waveOut device callback: on WOM_DONE (0x3bd) it reports the finished header through the object's vtable at +0x1c.

### Config files (0x42bbc–0x43f42)

Most of these functions are Pascal. A config file is cached in a 0x116-byte memory handle:
- +0 prev, +2 next (list head at `0x107230`)
- +4 Mohawk fileSpec (0x106 bytes)
- +0x10a modification time
- +0x110 text size
- +0x112 current offset
- +0x114 text handle

`configFindFile` revalidates the cache by time and size.

`configRead(spec, type, name, buf, size)` has three modes:
- `type == 0` lists the "[type]" sections, NUL-separated.
- `name == 0` lists the names in a section.
- Otherwise it returns the value of `name`. Quotes are stripped.

`configWrite(spec, type, name, value)` has four modes:
- `name == 0` deletes a section.
- `value == 0` deletes a name.
- If the name exists, its value is replaced.
- Otherwise the name, or the section and the name, is added.

`configWrite` goes through the variadic `configWriteText(h, offset, oldLen, count, strings...)`, which resizes the text, splices the strings in and writes the file back. The error is at `0x10722a`. The init count and the client-byte offset are at `0x10722c`/`0x10722e`.

## Porting notes

**Register convention: `DoSoundVB`.** It takes the VBLTask in A0 (A5 is at A0-4). It is `"conv": "manual"`; `glue_DoSoundVB` reads A0. D0 returns that A5, as in the original.

**Variadic function that writes its own arguments: `configWriteText__DUsUsUsUse`.** It reads its strings from the stack after the four words. It also advances its own offset and count argument slots (`add.w`/`subq.w` on 0xa/0xe(A6)). It is therefore `"conv": "manual"` with `"ret": 2`, and the body works on the argument block in emulated memory (`configWriteText_args(ap)`). `configWrite` pushes the arguments exactly like the original, including words pushed before an intermediate `configFindNextLine` call, and enters it through its 68k entry, so the call is checked.

**Stack depth.** Some calls are made deeper than their own arguments because the original leaves earlier arguments on the stack. These are reproduced with `DEEPER(n, call)`:
- the third `CalcPlaybackLength` in `LowLevelPlaySample`
- `MusicDriverHandler` (8 bytes deeper)
- `LowLevelPlaySample` after `ReadBuffer`/`EndSoundQuick`
- the second `ChangeSoundPitch`
- `CalcPlaybackLength` in `GetSoundTime`

**Byte arguments in the high byte of a word.** Several Boolean or char arguments are pushed with `move.b` and read as high bytes, through `B_HI()`:
- `EndVoice`'s two flags
- `LoadSampleIntoSlot`/`LoadSamples`/`RegisterSounds` `dontLoad`
- `LowLevelPlaySample`'s envelope flag
- `SoundLock`

**Original quirks kept.**
- `ClearVoice` clears only 0x79 of the 0x7a bytes.
- `ReverseSample` swaps `p[0]` with `p[n]` (one past the end).
- `CompressHandle` leaks the work handle when the data does not compress.
- `FUN_000419ce` leaves `gInSoundVB` set and does not clear `*0x10716a`.
- `FUN_00041904` tests the byte at the address held in CPUFlag (0x12F) before flushing the caches.
- `FUN_00041904` returns the caller's D7 when its allocation fails. The port reads `cpu_get(R_D7)`, which is exact when it is entered from 68k code.
- `configGotoNextName`/`configScanWhitespace` treat space and BS (8), not tab, as blanks.

**`FUN_00042bb2`.** It returns the address of a table in the segment (0x42b80). The port computes it as `fn_address(FN_FUN_00042bb2) - 0x32`.

**Return-size overrides.**
- `"ret": 0`: `EndVoice`, `FreeSampleFromSlot` and `configQuit` are void and leave garbage in D0. `configQuit` is an atexit handler.
- `"ret": 2`: `LoadSamples`, `RegisterSounds`, `StartFilePlayback` (16-bit error, garbage high word) and `configWriteText`.
- `"ret": 4`: `GetSoundTime` returns `CalcPlaybackLength`'s full D0.

**Override outside the unit.** `__dt__8fileSpecDv` (CODE 21) is called with the fileSpec pointer, but the index listed no parameter, so `"params": [[4, 4]]` was added. The function reads the argument and does nothing.

## Verification

**Standard run.** I ran `tools/check.sh` with all 22 scenarios, after merging master (with seg11a merged). There are no mismatches and no crashes. The same holds with `CALLS=300` and with the long intro (`VMS=1800000`).

Only 11 of the unit's functions are checked as top-level calls:
- the config reader used at startup: configInit, configQueryInit, configRead
- GetMasterVolume
- the VBL task and voice start/stop with its callbacks

The other functions the game uses run only nested inside calls that are already being checked: calls from the now-native seg11a code (InitSoundSystem, deviceCallback, …) and the helpers called by the functions above. The parents' memory comparison covers them. Most of the rest of the layer is never used by Prince of Persia 2: LZSS compression, sound slots, BeginSound*, file playback and configWrite.

**Temporary self-test driver** (not committed). It was started from the checker when no check was in progress. It exercised every function through its 68k entry, with forced checking:
- LZSS round trips, including incompressible data
- Compress/Decompress/DeltaDecompress handles
- a 'csnd' resource made at run time with AddResource, for the csnd paths of LoadSampleIntoSlot and SoundMemorySize
- RegisterSounds/BeginSound* variants, sound lists, envelope procs (`fileMapOSError` as the Pascal proc), a VB callback and a done callback
- direct DoSoundVB calls
- file playback of "Level 7": buffer switching, restart countdown, suspend/resume
- configWrite in all four modes and configRead in all three modes
- configFreeFile/configQuit/configInit
- shutdown and re-init of the sound layer

With this driver all 94 functions were checked with no mismatch, on all scenarios. The one class of difference is in the dead interrupt-level stack (0xFE8000–0xFF0000) after DoSoundVB calls made from the self-test: the sound callbacks that the Sound Manager runs at interrupt level are native code, which does not leave the 68k code's saved registers there. This is the known checker limit for interrupt-level code. With that region ignored, nothing differs.
