# seg22b — CODE 22 "Mohawk Libs 4", second half (0x6a70e–0x6ddf6)

All 45 functions are ported in `src/game/seg22b_mohawklibs4.c`.

## What the unit does

Two libraries:

* the rest of the Mohawk resource manager (seg22a has the first half and the layout of
  the client record, the open file "map" and its entries);
* the Mohawk timer library, built on the Time Manager.

Every `res*` and `timer*` routine is a Pascal function, so p4 in the C prototype is the
*last* Pascal argument. The `TimerLinkList` methods are C++ with `rtd` (callee pops,
`this` at p4, result in D0).

### Resources

* `resRead(rh, buf, countp, offset)` reads from the memory handle when the resource is
  loaded, else from the file. A read past the end is cut: `*countp` is set and the
  result is 0x28a0.
* `resWrite(rh, buf, count, offset)`: offset -1 appends. A resource that grows, or is
  written at or past its end, is first moved to the end of the file with `resCopy`
  (unless it is already the last one). The function updates the size, the offset, the
  file end and the map flags (+2e has holes, +30 modified).
* `resUpdate` writes a dirty loaded resource back, then the map (`resMapUpdate`).
  `resUpdateFile` does the same for every dirty resource of a file. map+0x32 counts the
  dirty entries.
* `resSetChanged` (flag 1, keeps the count at map+0x32), `resSetPreload` (0x20),
  `resSetProtected` (0x40) and `resSetPurgeable` (0x80, and `memSetPurgeState` with the
  flag as given) return the previous state or -1. `resSetFile` sets the current file
  (client record +8; 0 = the most recent file, +c).
* Error 0x28a0: range.

### Queued loads (`resQ*`)

A queued item is a 0x32-byte memory handle:

| Offset | Size | Contents |
| --- | --- | --- |
| +0 | long | next item |
| +4 | long | previous item |
| +8 | long | resource |
| +c | long | proc |
| +10 | long | refcon (the proc gets its address) |
| +14 | word | done |
| +16 | long | buffer (from the proc) |
| +1a | long | offset in the resource |
| +1e | long | count |
| +22 | long | bytes read |
| +26 | long | file offset of the resource |
| +2a | long | `resQLoad` only: user proc |
| +2e | long | `resQLoad` only: user refcon |

* Each map keeps a circular list of its items, sorted by `resQCompare`. The head is at
  map+0x1e and the count (word) at map+0x1c.
* Maps with items form a circular list through map+8/map+c. The client record's +0x10
  holds the first one.
* The proc is a Pascal function `proc(&refcon, rh, char msg): long`. It is called
  through `resQCallback`, which switches to the instance of the client that owns the
  file. Messages: 0 give a buffer, 1 release it, 2 cancelled, 3 done, 4 error.
* `resQRead` queues an item. When the resource is already loaded, it runs the item at
  once and returns 0.
* `resQLoad` queues a `resQRead` of the whole resource through `resQLoadProc`. That
  proc allocates a handle, attaches it to the entry at the end (or swaps it into the
  purged handle the entry kept), and calls the user proc. An empty resource is loaded at
  once with `resLoad`.
* `resQExec(budget ms)` works like an elevator:
  * For each file, it picks the item nearest to the file's current position in the
    sorted list.
  * `resQPrefetch` seeks and calls `filePrefetch`.
  * `resQExecRef` reads in chunks of at most 100 bytes per remaining millisecond.
* `resQFlush` runs the items of one resource to the end. `resQCancel` frees them.
* `resQFreeRef` does three things:
  * it tells the proc (messages 1 then 2) when the item is not done;
  * it unlinks the item (and the map, for its last item);
  * it updates the entry's queued flag (8).

  An item that is still locked (being run) is left alone.

### Timers

| Address | Name | Contents |
| --- | --- | --- |
| `0x107c32` | `gTimerStartTicks` | TickCount at `timerInit` |
| `0x107c36` | `gTimerError` | word read by `timerGetError` (never set) |
| `0x107c4e` | | long cleared by `timerInit` |
| `0x107c56` | `gTimerCurTask` | timer whose proc is running |
| `0x107c5a` | `gTimerDelay` | byte: delay given to `timerCreate` inside a proc |
| `0x107c5b` | `gTimerRearmed` | byte: periodic timer, or re-armed inside its proc |
| `0x107c5c` | `gTimerInProc` | byte: a timer proc is running |
| `0x107c5d` | `gTimerHasXTime` | byte: system ≥ 7.0 (`gOsSysVersion`) |
| `0x107c5e` | `gTimerUseXTime` | byte: the last `timerCreate` used `InsXTime` |
| `0x107c60` | `gTimerCalib` | Time Manager measurement by the first `timerInit` |
| `0x107c64` | `gTimerInitCount` | word |
| `0x107c66` | `gTimerList` | `TimerLinkList` (built by the static constructor `seg22_06d918`) |

A timer is a `TTimerInfo` (0x2c bytes, an extended TMTask):

| Offset | Contents |
| --- | --- |
| +6 | tmAddr: `periodicTask` or `periodicXTask` (jump-table entries) |
| +a | tmCount |
| +16 | proc |
| +1a | A5 |
| +1e | period in −µs (0 = one shot) |
| +22 | refcon |
| +26 | id |
| +2a | byte: in use |
| +2b | byte: installed |

* A `TimerLinkItem` is a `TTimerInfo` plus a next pointer at +2c.
* The list record holds: +0 head, +4 tail, +8 current item, +c previous item, +10 word
  count. The `Select*` methods move the current item.
* `timerInit` adds 10 free timers each time it is called. Their ids come from
  `FUN_0006d8aa`, the smallest id not used.
* `timerCreate(refcon, proc, period, delay)` works in one of two ways:
  * normally, it reuses a free timer (`FUN_0006d8e6`) or appends a new one, installs it
    (`InsXTime` when the system has it and the timer is periodic) and primes it;
  * inside a timer proc, it re-arms the running timer instead (only one-shot timers).
* The proc is `proc(refcon, id, 0)`, Pascal, popping 12 bytes (`timerProc__7midiObj`).
* The tasks `periodicTask`/`periodicXTask` (A1 = the timer) run the proc with the
  timer's A5. Then they re-prime the timer, or `timerDestroy` a finished one-shot timer.
  `periodicTask` measures the proc's run time with a TMTask on its stack and primes
  the rest of the period.
* `timerDestroy(id)` only marks the timer free when it is called inside a proc. Error:
  0x27da when there is no such timer.

## Porting notes

* **The inlined "map of a resource" sequence** is the helper `res_map` (lock: with the
  unlock on a bad entry, or `memXlat`). The port skips the dead stores it makes into
  the frame. `resQFreeRef`, `resQExecRef` and `resQLink` go on after errors with
  frame slots the original may leave unset. The port keeps those at their `a6 - k`
  offsets.
* **Argument slots.** Two functions use the address of their own arguments:
  * `resWrite` writes offset -1 → size into its slot and passes the address of count
    to `fileWrite`;
  * `resQLoad` passes the address of its refcon to the proc.

  Both use `args_`.
* **`resQCallback`** pushes the message with `move.b`. The low byte of that word is the
  low byte of the return address of the `osSetInstance` call just before (0x6b708,
  runtime address). The port writes it. Callers pass the message as `msg << 8`.
* **Nested calls** (`NESTED`): `resRead`'s `memXlat` runs under `memMove`'s slot and
  buffer (8 bytes). `resUpdate`/`resUpdateFile`'s `memLock` runs under `resWrite`'s slot
  and rh (6 bytes). `resQCallback`'s `osGetClientInstance` runs under `osSetInstance`'s
  slot.
* **Arguments left on the stack:** `operator new`/`delete` in the list methods are
  called without popping their argument. `leave_arg` lowers SP the same way until the
  function returns.
* **`timerCreate`** loads A5 from CurrentA5 and does not restore it; the port does the
  same (`cpu_set(R_A5, ...)`). It stores the old A5 in +1a. The tasks set A5 to the
  timer's A5 around the proc and return it in D0 (`exg`).
* `fileSeek`'s mode is a char in the high byte (`resQExec` passes 1 << 8).
* `SelectItem` reads +26 of a null current item before it tests for the end. Kept.

## Signature overrides (`tools/sig_overrides/seg22b.json`)

* `periodicTask`, `periodicXTask`: `"conv": "manual"`. They are Time Manager tasks, and
  their glue reads A1.
* `ret 4` for the `TimerLinkList`/`TimerLinkItem` methods that return D0 (constructors,
  `AddItem`, `InsertItem`, `Select*`). The callers use it (the index had 0).

None for functions outside the unit.

## Verification

* **Standard run.** `JOBS=6 SECS=2400 tools/check.sh` (22 scenarios) reports no
  mismatch, crash or timeout.
* **`POP2_NATIVE_ONLY` = this unit.** The game reaches these functions on their own
  entry, and all were checked:
  * `resRead`, `resSetPurgeable`, `resUpdateFile`, `resQFlush`;
  * `timerCreate`, `timerDestroy`, `timerGetTime`, `timerInit`, `timerQueryInit`;
  * `periodicTask`, `FUN_0006d8aa`, `FUN_0006d8e6`, `seg22_06d918`;
  * `AddItem`, `SelectItem`, `SelectFirstItem`.
* **Temporary self-test driver** (not committed). It ran with `CALLS=40`, triggered at
  the 45th `resRead`, in the Level 7, 3 and 12 saves. It covered:
  * the flag setters and error paths on a game resource;
  * a new file (`resCreateFile`, `resOpenFile` with the read-only flag cleared, because
    the emulated drive reports "locked"), with:
    * writes: growing, appending, overwriting, relocation, protected, range;
    * reads from the file and from the handle;
    * `resUpdate`/`resUpdateFile`;
    * queued loads with stub procs: `resQLoad`, `resQRead`, `resQCompare`,
      `resQFindItem`, `resQCallback`, `resQPrefetch`, `resQExecRef`, `resQExec` with
      small budgets, `resQCancel`, `resQFlush`, `resQLoadProc` messages 0/1/3/9,
      `resQFreeRef`, a direct `resQLink`;
    * preload on reopen;
  * a temporary `TimerLinkList` (every method);
  * timers created with `InsXTime` forced (so `periodicXTask` ran), a one-shot timer,
    `timerDestroy` of a missing id;
  * later, `timerDestroy`, `timerGetError`, `timerQuit`, the static destructor
    `seg22_06d92e` and the constructor again.

  All 45 functions were checked with no mismatch.
