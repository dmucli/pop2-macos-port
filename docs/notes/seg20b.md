# seg20b — CODE 20 "Mohawk Libs 1", second half (0x5d8be–0x5fb3e)

All 34 functions are ported in `src/game/seg20b_mohawklibs1.c`. Signature fixes are in `tools/sig_overrides/seg20b.json`.

## What the unit does

This is the portable audio layer of the Mohawk library. It sits on the MMSYSTEM-like `midiOut*`/`waveOut*` API of CODE 11 (seg11a). Its callers are `audio*` and the `midiObj` methods (CODE 20, first half).

### MIDI mapper (`midiMap*`)

The mapper keeps its state in Mohawk OS client bytes. Their offset is the word at A5-0x13a (0x107ec6):
- +0: word "initialised"
- +2: long, the patch-map resource file (`resOpenFile` of the file named at A5-0x150)
- +6: one long per midiOut device, pointing to its device record

Other globals: A5-0x13e is the `midiMapInit` count, A5-0x13c the device count, A5-0x144 the resource type of the patch maps.

**Device record** (0x4e bytes, `memAllocPtr`, made by `midiMapGetDevice` on first use):
- +0 device id
- +2 MIDIOUTCAPS (0x32 bytes; the name is at +8)
- +0x28 technology: when it is not 1, the patch-map number is looked up in the config file. The section is at A5-0x160 and the file at A5-0x16c. The lookup matches the device name against the values.
- +0x34 patch-map number
- +0x36 mute flags, one byte per channel
- +0x46 open count
- +0x48 midiOut handle (word)
- +0x4a first open handle

**Handle** (0x81c bytes, made by `midiMapOpen`): a circular list of handles per device.
- +0 device record
- +4 next, +8 prev
- +0xc mute flags, one per channel
- +0x1c note-on counts, 16 channels × 128 keys

**Messages and reset.**
- `midiMapShortMsg` counts note-ons and note-offs per key. It drops muted channels and note-offs that have no matching note-on.
- `midiMapReset` and `midiMapResetChannel` send one note-off per held note.
- `midiMapMuteChannel` mutes a device channel. On unmute it releases the notes held on that channel by every handle.

**Sysex and patch maps.**
- `midiMapLongMsg` handles the Mohawk sysex `F0 00 00 34 <map> <op> <ch>`. Op 1/2 (un)mutes a channel of the handle, op 3/4 a channel of the device. Any other sysex goes to `midiOutLongMsg`.
- The first open of a device sends its patch map, the resource with number +0x34. The last close (`midiMapClose`, seg20a) sends the resource with the negative number.

### Sound system (`mmaudio*`)

`mmaudioInit` reads five settings from the config file:
- the voices
- the file voices
- the maximum voices
- "mono"/"ste…"
- a sample-size string

It derives an output quality from them (1..7) and calls `HS_SetupSystem(voices + file voices, max, file voices, quality)`. Its count is at A5-0x48.

`mmaudioEnable(0/1)` pauses and resumes the sound system through `FUN_0003ebf6`/`seg11_03ebfc`. The calls nest; the pause count is at A5-0x46.

### waveObj

`waveObj` is a Symantec C++ class with Pascal methods; `this` is pushed last. It plays an `'MHWK'`/`'WAVE'` resource through `waveOut*`. The game uses it for its digitised sounds, through `audioCreate` and `CreateSound`.

**Construction.** `create` lays out the object (0xb8 bytes plus 0x2c per block, in a locked Mohawk handle). It sets the vtable from A5-0x1b6 (audioObj), then A5-0x44 (waveObj), and parses the `'Data'` and `'Cue#'` chunks. `calcBlocks` then cuts the sound into blocks at the cue points and at the loop start and end.

**Object fields:**
- +0x14 paused, +0x16 playing, +0x18 prepared
- +0x30 position (frames)
- +0x34 bytes per frame
- +0x36 and +0x3a: frames per ms and ms per frame (Fixed)
- +0x3e waveOut handle
- +0x40 frame at device position 0
- +0x46/+0x48/+0x4a: loop state
- +0x4e loop block
- +0x52 WAVEHDR of the loop tail
- +0x72 rate, +0x74 frames, +0x78 bits, +0x79 channels, +0x7c loop count
- +0x7e/+0x82 loop start and end
- +0x86 samples
- +0x8a block count, +0x8c blocks

The comment at the top of the C file has the full layout.

**Block fields:**
- +0 first frame
- +4 frames
- +8 cue point
- +0xc WAVEHDR (flags at +0x1c, loops at +0x20)

**Methods called through the vtable:**
- +8 `devicePrepare`
- +0xc `deviceStart`
- +0x10 `deviceStop`
- +0x14 `deviceUnprepare`
- +0x1c notify(event): an 8-byte event on the caller's frame, of which only byte 0 is set (2 pause, 3 resume, 4 start, 5 stop)
- +0x2c `resetLoop`
- +0x34 `seek`

`deviceStart` queues one WAVEHDR per block from the position. When it starts inside the loop, it first queues the rest of the loop. `getFrame` folds the device position into the loop.

Errors are stored in A5-0x172 (0x107e8e, `gAudioError`): 0x29cc..0x29d1 and 0x2a01..0x2a07. A5-0x16e is the offset of the audio client bytes.

## Porting notes

**`fileSpec` passed by value.** `__ct__8fileSpecDC8fileSpecPCc` (CODE 21) takes its `fileSpec` argument by value, 0x106 bytes on the stack. The port pushes the name and the copy by hand and calls the function with `call68_fn`.
- In `midiMapGetDevice`, the caller pops only 8 bytes, so the copy stays on the stack. The following config calls run 0x106 bytes deeper, as in the original.
- In `midiMapInit` and `mmaudioInit`, the result slot of the next call (`resOpenFile`/`configRead`) is reserved before the `fileSpec` is built.

**midiOut/waveOut calls.** These seg11a functions are `"conv": "manual"` Pascal functions with hand-written glue (`PGLUE`) and an index result size of 0. Their generated check-build trampolines reserve no result slot and return D0, which the glue never sets. The first check run therefore saw garbage results (`midiMapOpen` mismatched: it took `midiOutOpen`'s garbage result as an error). The port calls them like the original instead (`mmcall`): it reserves the result slot, pushes the arguments, enters through the jump table and reads the slot. The same code works in the normal build and the check build.

**`midiOutGetNumDevs`.** It stores its result in the caller's slot, so it is called the same way.

**Virtual calls** (`vbegin`/`vend`) push the result slot and the arguments exactly as the original does.

**Original quirks kept.**
- `midiMapLongMsg` busy-waits while the header's done bit is *set*. A successful non-Mohawk sysex would therefore hang, both in the original and in the port.
- `midiMapOpen` waits until the bit is set.
- `deviceUnprepareBlks` unprepares the block headers with size 0x2c.
- `calcBlocks` reads A4 and D7 unset when there is no cue chunk. The port takes them from the CPU registers; they are never used then.

**Division by zero.** The loop divisions in `deviceStop`, `getFrame` and `releaseLoop` are guarded with `port_divide_by_zero`, for a loop of length 0.

**Signature overrides.**
- `midiMapOpen`: the parameters are `(long, long, long, word device, long &handle)`, at offsets 4, 8, 12, 16, 18. The index listed seven words.
- `midiMapQuit` and `mmaudioQuit`: `"ret": 0`. D0 is left over from the callees, and `audioQuit`/`audioInit` ignore it.

No override for functions outside the unit.

## Verification

**Standard run.** `tools/check.sh` (22 scenarios) reports no mismatch, including with `CALLS=300`.

The game exercises these functions directly:
- `midiMapInit`, `midiMapOpen`, `midiMapShortMsg`, `midiMapPrepareHeader`, `midiMapUnprepareHeader`, `midiMapReset`
- `mmaudioInit`
- `create`, `prepare`, `seek`, `resetLoop`, `start`, `stop`, `unprepare`

Most of the rest (`midiMapGetDevice`, `midiMapResetChannel`, `calcBlocks`, the `device*` methods, `getFrame`, `mmaudioEnable`, the `fileSpec` copy) runs only nested inside those checks.

**Nested functions.** Runs with `POP2_NATIVE_ONLY` set to the nested functions, with their callers left on the 68k code, checked these directly with no mismatch:
- `calcBlocks`, `devicePrepare`, `deviceUnprepare`, `midiMapResetChannel` and `__ct__8fileSpecFRC8fileSpec`
- `deviceUnprepareBlks`, in a run of its own

**Temporary self-test (not committed).** It was called from the checker after a check completed, and called functions through their 68k entries:
- the `waveObj` methods on a playing sound: getPosition, getFrame, releaseLoop, pause and resume (twice each), seek and start in every state, stop, unprepare and prepare errors, deviceStop and deviceStart
- `midiMapGetID`, `midiMapMuteChannel` (mute, unmute with held notes, errors), `midiMapShortMsg` (note on/off, unbalanced, muted, NULL), `midiMapLongMsg` (Mohawk sysex ops 1–4, other map, unprepared header, NULL handle), and the error paths of `midiMapOpen`/`midiMapGetDevice`/`midiMapReset`/`midiMap*Header`
- `mmaudioEnable` nesting
- `midiMapQuit`/`mmaudioQuit`, followed by re-init

All 34 functions were checked with no mismatch.

**Not executed, reviewed against the disassembly only:** the `midiOutLongMsg` path of `midiMapLongMsg` (it would hang, see above), and the error paths of `create` (bad chunks, allocation failures).
