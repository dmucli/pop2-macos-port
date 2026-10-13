# seg21b — CODE 21 "Mohawk Libs 3", second part (0x6314a–0x66834)

All 102 functions are ported in `src/game/seg21b_mohawklibs3.c`. Signature fixes are in
`tools/sig_overrides/seg21b.json`.

## What the unit does

Four parts of Broderbund's "Mohawk" support library.

### mem: numbered memory handles (0x6314a–0x65062)

The `mem*` API hands out 16-bit handle ids (1-based) instead of Mac handles. Each id has a
14-byte master record. The records live in up to 16 blocks of 0x200 records, allocated
with `operator new(0x1c00)` by `FUN_0006314a`. The id is `block * 0x200 + index + 1`.
`getMaster__FUs` (seg21a) turns an id into its record; `findEmptySlot__FRs` (seg21a)
finds a free one.

Master record:
- +0 long: the Mac handle (0 when purged)
- +4 word: flags
  - bit0: the Mac handle was given out (`memGetOSHandle`, cleared by `memReleaseOSHandle`)
  - bit1: purged
  - bit2: in use
  - bit3: owner flag (`memSetOwner`/`memGetOwner`; `memFreeHandle` refuses it)
  - bits4-7: purge level (`memLock`/`memXlat` set 0xf; `purgeThisMuch` lowers it)
  - bits8-15: lock count (`memLock`/`memUnlock`/`memSetLockCount`)
- +6 word: bit15 purgeable (`memSetPurgeState`)
- +8 word: the id
- +a long: value attached by `memAttachHandle`

Globals (cleared by `memInit`, 0x10761c..0x107675):
- `0x10761c` init flag
- `0x10761e`/`0x107620` error and previous error. Every setter copies the error to the
  previous one first (`MEM_SETERR`).
- `0x107622` purging enabled (`memPurgeEnable`)
- `0x107624` block count, `0x107626` block pointers
- `0x107666` `memRegisterOwner` count
- `0x107668` fail proc: Pascal `proc(size, 0x2777): Boolean` (retry)
- `0x10766c` purge proc: Pascal `proc(id): Boolean` (may purge). The res layer installs
  `resPurgeProc`.
- `0x107670` the one value attached by `memAttachPtr`
- `0x107674` offset of the mem client bytes

Allocation:
- `_memAllocPtr`, `_memAllocHandle` and `_memSetHandleSize` try the Toolbox call, then
  `purgeThisMuch(size)`, then loop on the fail proc.
- `purgeThisMuch` purges purgeable, unlocked handles of purge level 0 until `CompactMem`
  finds the size. Between passes, it lowers the level of the others by the smallest level
  seen.
- `purgeAll__Fv` purges everything purgeable. `FUN_000631f6` sums the purgeable sizes.
- `memPurge(0)` returns that sum. Otherwise it purges, everything or just enough.

### String helpers (0x6506e–0x65586)

- `stricmp`, `strnicmp` and `memicmp` compare with `(a ^ b) & 0xdf`, so they ignore bit 5
  of every byte, not only letters.
- `strrev`, `strupr`, `strlwr`, `strset`, `strnset`, `strdup`: as their names say.
- `dprintf` (DebugStr of a printf).
- `pstrcat`, `pstrinsert`, `pstrnicmp`, `pstricmp`: Pascal strings.
- `ReallocPtr`: realloc with `NewPtrClear`.

### DEQBlock (0x6562a–0x65996)

A list of 12-byte records kept in a handle. The object is 0xe bytes:
- +0 word: record count
- +2 long: used list head
- +6 long: free list head
- +a: the record handle

Each record has +0 next, +4 prev and +8 data; -1 ends a list. The head's prev is its own
index. `NewRef` takes a free record, or grows the handle by one record, and returns
index + 1. `FreeRef` unlinks the record and pushes it on the free list.

### os: clients, interrupt tasks, clock (0x659ac–0x66834)

A client is a locked handle with this layout:
- +0 next handle, +4 prev handle (circular list, head in `0x10787a`)
- +8 its own handle
- +c instance (copied from `*0x1076c2`)
- +10 activate hook, C `hook(flag, client)`
- +14 ring of interrupt tasks
- +24 client bytes, reserved by `osReserveClientBytes` in every client

`osGetClientBytes` returns the dereferenced handle; its callers add their offset.
A client argument of -1 means the current client (`osGetClient`). The functions store the
resolved client back into their own argument slot.

Init and activation:
- `osInit` reads Gestalt 'proc' (minus 1), 'sysv', 'mach', 'mmu ' and 'addr' into
  0x107842..0x10784a.
- The first `osInit` clears the timer block, requires system 6.0.7, then calls
  `MaxApplZone`, `MoreMasters` and `TickCount` (into 0x10786e).
- `osInit` then creates and activates a client.
- Active client: `0x107884`. Error: `0x10787e`. Init count: `0x107880`.

Interrupt tasks (`osCreateITask`/`osDestroyITask`, 0x14 bytes):
- Layout: +0 next, +4 prev, +8 client, +c proc, +10 refcon.
- The first task patches `GetNextEvent`/`WaitNextEvent` with `osGetNextEvent`/
  `osWaitNextEvent` (`FUN_00066788`). It does so only if both trap addresses are at or
  below `**0x10779a`; in this emulator they are not, so creation fails with error 1.
- The patches run the original trap (saved at 0x107872/0x107876, called with A5 =
  CurrentA5), then `osExecuteIdleTask`. That calls the active client's next task
  `proc(refcon, task)`.
- `FUN_000667d8` removes the patches.

Clock:
- The Time Manager task at `0x10784c` is a TMTask, with A5 stored at +0x16.
- `osGetTime` removes the task, adds the elapsed time to the millisecond clock
  (`0x107866`, with `0x10786a` for fractions of a millisecond), and re-primes it for
  0x5200000 ms.
- `osResetSystemTimer` is the task procedure. It takes A1 = task.

Other:
- `osIntrEnable` stores its flag at `0x107882` and calls `FUN_000659c4`. In supervisor
  mode, that sets the interrupt mask to 7 (flag ≠ 0) or 0.
- `osPageLock`/`osPageUnlock`/`memPageLock`/`memPageUnlock` call `MemoryDispatch`
  (A05C, selectors 2/3) only when virtual memory may exist: system ≥ 7, an MMU, and not
  machine 8 (memPage*: nor 0x31).
- `osCallClient(client, &result, fn, n, args...)` calls through `FUN_000659e8`. That
  pushes the low words of n longs and calls fn as C (n > 0) or Pascal (n < 0, long
  result read above the arguments).
- `threadInit`/`threadQuit` are stubs.
- `FUN_000659ac` returns A4. `FUN_000659b6` returns its argument (callee pops 4).
- Dead code (no callers in the game): `osCallClient`, `FUN_000659e8`, `osCreateITask`,
  `osSetActivateHook`, `ReallocPtr`, `stricmp`, `dprintf`, `memDummy`, `FUN_000659ac`,
  `FUN_000659b6`.

## Porting notes

**A6 kept like the original.** Native functions do not maintain A6, but 68k callees save
it in their frames. `MohawkInit` → `memInit` → `tc_memset` left a different saved A6 on
the stack. `timerCreate` then copied it into 10 heap timer records, as stack garbage of a
TTimerInfo. So every `link` function of the unit starts with `ENTRY_LINK6`. This is
`ENTRY_LINK`, plus it writes the old A6 at `a6` and sets the CPU's A6 to `a6` while the
function runs (restored on return).

`memInit` also sets D3/D4/D5/D7 like the original (`SETW`, restored by `SAVE_REGS`).
This mirrors the registers it holds across 68k calls.

**Result slot left unwritten.** These functions return without writing their Pascal
result in one path:
- `memReallocPtr` and `ReallocPtr`: the old pointer's size is 0.
- `memRegisterOwner`: called a second time.

They are `"conv": "manual"`. The C body writes the slot itself and keeps D0 in `s_d0`.
The glue pops the arguments and sets D0. C callers get the slot contents.

**Other manual glue:**
- `osGetNextEvent`/`osWaitNextEvent` write only the result byte, the high byte of the
  slot. Their D0 is CurrentA5 with the result in the low byte.
- `FUN_000659e8`: its result slot is not in the index. A C caller reserves the 4 bytes
  itself (`osCallClient` does). D0 is the callee's D0, while the slot is read above the
  arguments. It leaves 2·n in D3 (its caller saved D3). The port sets D3, and
  `osCallClient` restores it.
- `osResetSystemTimer` is a Time Manager task: the glue reads A1.

**Argument slots written by the function** (`osActivate`, `osCallClient`,
`osCreateITask`, `osGetClientBytes`, `osGetClientInstance`, `osQueryActive`,
`osSetActivateHook`): the body writes the resolved client into the argument area
(`args_ + off`) and reads it back from there, so no glue is needed.

**Variadic functions:**
- `dprintf(fmt, ...)` passes `args_ + 4` to `tc_vsprintf`.
- `osCallClient` passes `args_ + 0xe`.

**Bytes in the high byte of a word:** the fill character of `memSet`, `strset` and
`strnset` (`B_HI`).

**Original quirks kept:**
- `memInit` does not count a second call. When the memory is already set up, it only sets
  "purge enabled".
- `memQuit` frees the blocks but leaves the block count.
- `memPurgeHandle` calls the purge proc without checking that it is set.
- `memGetHandle` dereferences the master handle of purged records (address 0).
- `memAttachPtr` keeps one global value, not one per pointer.
- `osWaitNextEvent` returns the uninitialised local byte when `sleep` expires before the
  first call.
- `PopRec`/`PushRec` set the head's prev to its own index.

## Signature overrides (`tools/sig_overrides/seg21b.json`)

All of them are for functions of this unit.

- `purgeThisMuch__FUl`: `ret 4`, `retslot false`. It is Pascal, but its callers use D0
  (bytes purged).
- `FUN_000659b6`: `conv pascal`, `ret 4`, `retslot false`. It pops its argument and
  returns it in D0.
- `"conv": "manual"`: `memRegisterOwner`, `memReallocPtr`, `ReallocPtr`,
  `osGetNextEvent`, `osWaitNextEvent`, `osResetSystemTimer`. `FUN_000659e8` also has
  `"ret": 4`.
- `ret 2`: `FUN_0006314a` and `FUN_00066788`. The high word of D0 is left from traps; the
  callers test the word.
- `dprintf_`: `params [[4,4]]` (format, then variadic), `ret 0`.
- `osPageLock`/`osPageUnlock`: `params [[4,4],[8,4]]` (count, address). The index had
  four words.
- `threadInit`/`threadQuit`: `params [[4,4]]`. The callers push the client handle as a
  long.

**seg21a helpers.** `getMaster__FUs` and `findEmptySlot__FRs` (seg21a) return their
record in D0. seg21a sets `{"ret": 4, "retslot": false}` for them. This unit uses the C
return value and adds no override of its own for them.

## Verification

**Standard run.** `tools/check.sh` (22 scenarios, on master with seg19b/seg22a merged)
shows no mismatch, crash or timeout. Most of the unit is called only from other ported
code (MohawkInit, seg11b, seg22a), so those calls are nested inside their callers'
checks. Those checks pass.

**More calls and the long intro.** The run with `CALLS=300` (all scenarios) and the
long intro (`VMS=1800000`) show no mismatch either.

**This unit alone.** With `POP2_NATIVE_ONLY` = this unit, the calls come from 68k code and
are checked directly: 40 functions over all 22 scenarios (`CALLS=100`), with no
mismatch.

**Temporary self-test driver** (not committed). It was started from `osGetTime`, made
`nocheck` for that run. It called every reachable function with chosen arguments,
through the 68k entries:
- strings and Pascal strings
- error paths with invalid ids
- lock, purge, swap and OS-handle paths
- a purge proc and an enum proc that return TRUE (small 68k stubs)
- DEQBlock grow/reuse/free
- `osCallClient` with C and Pascal callees
- `osResetSystemTimer` with A1 set
- interrupt tasks: the trap-address limit was raised temporarily, so the GNE/WNE patches
  ran during play and `osWaitNextEvent` was checked from the game's event loop
- a second client: osInit/osQuit, then the full osQuit path
- `dprintf`

A second run excluded the callers (`memPurge`, `memAllocHandle`, `osInit`, ...), so the
helpers were checked directly too: purgeAll, purgeThisMuch, `FUN_0006314a`,
`FUN_000659e8`, `FUN_00066788`/`667d8`, threadInit/threadQuit and `_mem*`. A cmd-q run
checked `memQuit` and `osQuit` (atexit).

Result: all 102 functions are verified at least once, with no mismatch.
