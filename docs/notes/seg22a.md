# seg22a — CODE 22 "Mohawk Libs 4", first half (0x66854–0x6a6fe)

All 34 functions are ported in `src/game/seg22a_mohawklibs4.c`.

## What the unit does

This is the Mohawk resource manager (`res*`). It reads the game's archive files: the
"MHWK"/"RSRC" files that hold the levels, sounds, MIDI songs and so on. The second half
of CODE 22 (seg22b: `resRead`, `resUpdate`, `resUpdateFile`, the `resQ*` load queue) is
in another unit.

The code was compiled with a Pascal-style compiler. Every routine except `resQuit` and
`resQsortList` is a Pascal function: the callee pops its arguments and puts the result
in the slot above them. In the C prototypes, p4 is therefore the *last* argument of the
Pascal declaration. For example, Pascal `resFind(type, id, file)` is
`resFind(file, id, type)` in C.

### Globals

| Address | Name | Contents |
| --- | --- | --- |
| `0x107a5e` (A5-0x5a2) | `gResError` | word: last error (`resGetError`) |
| `0x107a60` | `gResInitCount` | word: number of clients that called `resInit` |
| `0x107a62` | `gResClientOff` | word: offset of the resource record in the client bytes (`osReserveClientBytes(0x14)`) |
| `0x107a64` | `gResCopyBuf` | word: memory handle used as `resCopy`'s buffer (purgeable) |

`resInit` clears all four with one `memset(&gResError, 0, 8)`.

### Per-client record

There is one record per client. It is 0x14 bytes, at `osGetClientBytes(client) + gResClientOff`.

| Offset | Size | Contents |
| --- | --- | --- |
| +0 | word | initialised |
| +2 | word | memory owner id (`memRegisterOwner`) |
| +4 | long | previous purge proc, chained by `resPurgeProc` |
| +8 | long | current file |
| +c | long | head of the list of open files |

### Open file (map)

An open file is a locked memory handle. A file id is a long whose low word is that
handle.

| Offset | Size | Contents |
| --- | --- | --- |
| +0 | long | next file |
| +4 | long | previous file |
| +10 | long | client |
| +14 | word | open count |
| +16 | word | file ref |
| +18 | word | read-only |
| +1a | word | directory handle |
| +1e | long | queued-load list (`resQ*`) |
| +22 | long | file end |
| +26 | long | directory offset in the file |
| +2a | long | directory + list size in the file |
| +2e | word | "has holes": compact on close |
| +30 | word | modified |
| +34 | word | head of the free entry list |
| +36 | word | entry count |
| +38 | | entries |

Each entry is 10 bytes:

* +0 long: offset in the file.
* +4 word and +6 byte: the size (low word, high byte).
* +7 byte: flags.
* +8 word: the memory handle, or the next free entry when the entry is free.

Entry flags:

| Bit | Meaning |
| --- | --- |
| 1 | dirty |
| 2 | loaded |
| 4 | purged, handle kept |
| 8 | queued |
| 0x10 | free |
| 0x20 | preload |
| 0x40 | protected |
| 0x80 | purgeable |

A resource is the long `file id << 16 | entry index`. The entry index starts at 1.

### Directory

The directory is in the memory handle at map+1a. All its offsets are relative to its
start.

| Offset | Size | Contents |
| --- | --- | --- |
| +0 | word | offset of the name pool |
| +2 | word | type count |
| +4 | 8 bytes each | type records, sorted by type |

Each type record holds:

* the type (long);
* the offset of its id list (word);
* the offset of its name list (word).

The id list is a word count, then 4-byte records sorted by id. Each record is
{word id, word index}.

The name list is a word count, then 4-byte records sorted by name. Each record is
{word offset in the pool, word index}. Names are C strings of at most 31 characters,
compared without case (`memicmp_`).

`resFind`, `resFindName`, `resEnum*` and `resDirInsert` use binary searches. The
`lo`/`hi`/`mid` indices are unsigned longs.

### File header

The header is 0x1c bytes at offset 0:

* `'MHWK'`;
* the file size − 8;
* `'RSRC'`;
* the version word, 0x100;
* the "has holes" word;
* the file end (long);
* the directory offset (long);
* the directory size (word);
* the entry list size (word). The list is read into, and written from, map+0x34.

`resXlatChunk`, `resXlatDir` and `resXlatList` are the byte-order hooks. On the Mac they
do nothing: the last two rewrite every field with its own value.

### Function roles

* **Setup and shutdown.**
  * `resInit` / `resQuit`: `resQuit` is registered with atexit by `MohawkInit`.
  * `resQueryInit` returns 0x100 when the current client is initialised.
  * `resPurgeProc` is the memory manager purge procedure. It handles our handles:
    it writes back a dirty resource, then marks it "purged, handle kept". It passes
    other handles to the previous purge proc.
* **Files.**
  * `resOpenFile`: a second open of the same `fileSpec` only increments the count.
    The read-only flag is set when the read-write open fails with 0x283c, or when the
    drive is locked. The function also starts the preloads.
  * `resCloseFile(file, compact)`.
  * `resCreateFile`, `resEnumFiles`, `resGetFile`, `resHomeFile`.
* **Lookup.**
  * `resFind` and `resFindName` search one file, or the current file and the files
    opened before it.
  * `resFindHandle` searches by memory handle.
  * `resGetInfo`, `resGetSize`.
  * `resEnumTypes`, `resEnumIds` and `resEnumNames` call a Pascal callback
    `cb(ref, arg): word` through `resEnumCallback`. `resEnumCallback` switches to the
    owning client's instance around the call.
* **Memory.**
  * `resLoad` allocates (or resizes the kept handle), reads with `resRead`, then
    `resAttachHandle` and `resQFlush`.
  * `resFree`, `resAttach` (swap the handle).
* **Editing (unused by the game).**
  * `resCreate` / `resDelete` maintain the free list and call `resDirInsert` /
    `resDirDelete`.
  * `resMapUpdate` writes the directory and list back. They go in their old place when
    they fit, else at the end of the file.
  * `resCopy` moves data inside a file. It is used by the compaction in
    `resCloseFile`, which sorts the list with `qsort` and `resQsortList`.
* **Errors.**

  | Code | Meaning |
  | --- | --- |
  | 0x28a1 | duplicate |
  | 0x28a2 | not found / no file |
  | 0x28a3 | bad file |
  | 0x28a4 | protected |
  | 0x28a5 | read-only |
  | 0x28d2 | not initialised / not in the directory |
  | 0x28d3 | bad resource |
  | 0x28d4 | library version |

## Porting notes

* **Inlined macro.** Most functions start with the same inlined "lock the map of a
  resource" sequence. It writes many dead copies of the argument into the frame; the
  port skips those dead stores.
  * Some paths read an entry pointer that is still uninitialised on the stack:
    `resPurgeProc` when the index is 0. The port keeps that pointer at its original
    frame slot (`a6-0x58`).
* **Nested calls.**
  * `resEnumCallback` and `resPurgeProc` reserve `osSetInstance`'s result slot before
    calling `osGetClientInstance`.
  * `resLoad` pushes `resRead`'s slot and `rh` before calling `memLock`.
  * `resOpenFile` pushes `fileRead`'s slot and the file ref before calling `memLock`.
  * The port reproduces these with `NESTED(n, ...)`.
* **Pascal callbacks through pointers.** These are the enumeration callbacks and the
  previous purge proc. They use the small `pcall_w_ll` / `pcall_w_w` helpers: push the
  result slot, push the arguments, `call68_addr`, read the slot.
* **`fileSeek`'s mode is pushed with `clr.b`.** It occupies the high byte of the word
  and is always 0.
* **`resMapUpdate`** returns `fileGetError()` without setting `gResError` when the
  header write fails. `resCreateFile` replaces a successful `fileWrite` result with
  `fileGetError()`. Both behaviours are kept.
* **`memicmp`** is `memicmp_` in the generated prototypes (it is a reserved name).

## Signature overrides (`tools/sig_overrides/seg22a.json`)

* `resXlatChunk`: `params [[4, 4]]`. The callers push one long, the header address
  (`pea`). The index had two words.
* `resQuit`: `ret 0`. Its D0 is whatever its callees left, and its only caller is the
  atexit list, which ignores it.

None for functions outside the unit. `__dt__8fileSpecDv`'s one-argument override already
exists (seg11b/seg16).

## Verification

* **Standard run.** `JOBS=4 SECS=2400 tools/check.sh` (22 scenarios) reports no
  mismatch, no crash and no timeout. The scripted gameplay reaches 12 of the 34
  functions: `resInit`, `resOpenFile`, `resCloseFile`, `resFind`, `resFindHandle`,
  `resLoad`, `resAttachHandle`, `resPurgeProc`, `resGetError` and the three `resXlat*`.
  All of them run nested inside checks of other units' callers.
* **`POP2_NATIVE_ONLY` = this unit**, with the 68k callers entering through the hook:
  * `resInit`, `resOpenFile`, `resCloseFile`, `resFind`, `resLoad`, `resGetError` and
    `resPurgeProc` are checked on their own entry.
  * A second group (`resAttachHandle`, `resFindHandle`, `resXlatChunk/Dir/List`, with
    their callers left in 68k) checked those five.
* **Quit run** (`cmd-n` then `cmd-q`, unit only): `resQuit` was checked, with nested
  `resQueryInit` and `resCloseFile`.
* **Temporary self-test driver** (not committed). It was triggered at the 45th
  `resLoad`, with `CALLS=40` so that the trigger itself was not under check. It ran in
  the Level 7, Level 3 and Level 12 saves. It called every function through its hooked
  68k entry:
  * enumerations with a continue callback and a stop callback (two 68k stubs written to
    the stack);
  * `resGetSize`, `resHomeFile`, `resGetInfo` and `resFind`/`resFindName`, with real ids
    and names, missing ones, and invalid resources;
  * a load / purge (`resPurgeProc` on our handle) / reload through the kept handle /
    detach / reattach / free cycle;
  * `resPurgeProc` on a foreign handle;
  * `resQsortList` and `resEnumCallback` directly;
  * a new file made with `resCreateFile`, then edited:
    * `resCreate` with and without names, duplicates, and a name over 31 characters;
    * `resDelete` and free-entry reuse;
    * direct `resDirInsert` / `resDirDelete`, including a new type, a duplicate and a
      missing index;
    * `resCopy` with a small copy, a zero count, and a read past the end;
    * `resMapUpdate`;
    * a double open/close;
    * a compacting close, then a reopen of the edited file.

  All 34 functions were checked with no mismatch.
