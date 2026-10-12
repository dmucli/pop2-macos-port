# Porting guide: 68k → C

This is the contract for translating the original Prince of Persia 2 code (decompiled
in Ghidra) into C. The goal is a **faithful** translation: same memory effects in the
same order, same return values, same calls. Readability comes from names and comments,
not from restructuring the logic.

## 1. Where things are

| What | Where |
| --- | --- |
| Ghidra program | `pop2_image.bin` via the Ghidra MCP tools (always pass `program="pop2_image.bin"`) |
| Function index | `tools/fn_index.json` (name, segment, offset, stack params, return size) |
| Generated prototypes | `src/game/gen/proto.h` (built by `make`) |
| Toolbox wrappers | `src/game/gen/traps_c.h` (`_GetResource(...)`, `_NewPtrClear(...)`, …) |
| Helpers | `src/game/port.h` |
| Your code | `src/game/segNN_<name>.c` (one file per CODE segment, or `..._a.c/_b.c` halves) |
| Signature fixes | `tools/sig_overrides/segNN.json` |

Ghidra memory map: CODE segments at `0x10000…0x6f380`, A5 = `0x108000` (globals are
`DAT_00102906…DAT_00107fff`), jump table above A5, trap stubs at `0x200000+`.

## 2. Getting the source of a function

Use `mcp__ghidra__get_functions` with `fields="decompiled_code"` (up to 20 functions per call).
Whenever the decompiled code looks odd (CONCAT, extraout_, in_stack_, unaff_, stack
arithmetic like `*(undefined4 *)(puVar2 + -4) = ...`, UNRECOVERED_JUMPTABLE), read the
disassembly (`fields="disassembly"`, `include_disasm=true`, or `mcp__ghidra__disassemble_function`)
and translate from the instructions. **The disassembly is the ground truth.**

## 3. Types

THINK C: `int` is **16 bits**. In the decompiler output:

| Decompiler | Size | C in the port |
| --- | --- | --- |
| `int`, `short`, `undefined2` | 2 | `int16_t` |
| `uint`, `ushort` | 2 | `uint16_t` |
| `long`, `undefined4`, any pointer | 4 | `int32_t` / `uint32_t` (pointers are emulated addresses, `maddr`) |
| `ulong` | 4 | `uint32_t` |
| `char`, `byte`, `undefined1`, `bool` | 1 | `int8_t` / `uint8_t` |

Keep 16-bit wraparound and signedness exactly: `int16_t x = (int16_t)(a + b);`.
68k `divs`/`divu` truncate toward zero like C; `asr` is arithmetic like C `>>` on signed
values with clang. `CONCAT22(hi, lo)` = `(uint32_t)hi << 16 | lo`; `SUB41(x, 0)` = low byte.
Decompiler expressions like `((ulong)(long)(int)i >> 0x10) * 0x60000 + (ulong)i * 6` are
just a 32-bit index computed from a 16-bit value: `(int32_t)i * 6`.

## 4. Memory

All game data lives in the emulated big-endian address space. **Never** dereference
host pointers into it. Use the helpers from `port.h`:

```c
R8(a)  RS8(a)  R16(a) /*signed*/  RU16(a)  R32(a)      // reads
W8(a,v) W16(a,v) W32(a,v)                              // writes
GH(0x103074)        // address of the global the decompiler calls DAT_00103074
DEREF(h)            // *handle
mcopy(dst, src, n) mset(dst, v, n)                     // bulk ops on emulated memory
```

Pointer arithmetic follows the decompiler's types: `(undefined2 *)p + 3` is `p + 6`,
`(int *)p + i` is `p + 2*i`, `(long *)p + i` is `p + 4*i`.

**Order matters.** Perform reads and writes in the same order as the original, and
**re-read memory after every call** (any call can change globals: the decompiler shows
`DAT_xxx` re-read for this reason). Name globals with a `#define` at the top of your file:

```c
#define gCurBFItem  GH(0x103074)   /* long: current background/foreground item */
```

## 5. Functions and calls

* Each original function becomes `uint32_t Name(<params>)` with **exactly** the generated
  prototype from `gen/proto.h` (params: 2-byte → `int16_t`, 4-byte → `uint32_t`; the
  result is the full D0 register). You may rename parameters in your definition.
  Return `0` from functions the decompiler calls `void`.
* Call other game functions by name (any function, ported or not: unported ones go
  through generated trampolines into the 68k code). Pass `int16_t`/`uint32_t` as the
  prototype says and cast the result: `int16_t r = S16(Foo(a, b));`
* If a prototype is wrong (decompiler missed or invented a parameter, wrong size), put
  the fix in `tools/sig_overrides/segNN.json`:
  `{"Name": {"params": [[4, 2], [6, 4]], "ret": 2}}` (offsets from SP at entry, return
  address included; first parameter is at 4). Pascal-convention functions (callee pops,
  result written above the arguments): add `"conv": "pascal"`.
* Register-convention routines (Time Manager tasks get their TMTask in A1, VBL tasks in A0,
  some hand-written assembly takes args in D0/A0): mark `"conv": "manual"` and write
  `void glue_Name(void)` yourself (read `cpu_get(R_A1)` etc., call your C function, set
  `emu_set_d0`). C callers then call your C function directly.
* Library routines (`tc_memcpy`, `tc_sprintf`, `tc_FSOpen`, …) are implemented natively
  in `src/game/lib_thinkc.c`; just call them.
* Function pointers: the original passes `&Foo` as the address of Foo's jump-table entry:
  `JT_ADDR(FN_Foo)`. To call through a pointer read from memory:
  `call_ptr(ptr, "wl", word_arg, long_arg)` (C convention, args in source order).

## 6. Toolbox calls

Use the wrappers in `gen/traps_c.h`, named like the Ghidra stubs: `_GetResource(type, id)`,
`_HLock(h)`, `_CopyBits(...)`. All arguments are `uint32_t`/`int16_t` by stack size;
Booleans are passed as plain 0/1 (the wrapper encodes them); results come back as
`uint32_t` (Booleans as 0/1). Check the disassembly for flagged OS traps: `A31E` is
`_NewPtrClear`, `A51E` `_NewPtrSys`, `A322` `_NewHandleClear` … (the decompiler shows all
of them as `_NewPtr`/`_NewHandle`). The plate comment of each stub in Ghidra gives the
Pascal signature. Selector-dispatched calls appear under their own names
(`_NewGWorld`, `_SndPlayDoubleBuffer`).

## 7. Locals whose address escapes

Plain C locals are fine — unless their address is passed somewhere (a Toolbox call,
another game function, stored in memory). Those must live on the emulated stack:

```c
FRAME(f, 0x10);              // 16 bytes on the emulated stack, f = base address
maddr rect = f;              // like the decompiler's local_10
_SetRect(rect, 0, 0, 10, 10);
...
FRAME_END(f);                // before every return
```

## 8. Style

* One file per segment, functions in address order, each preceded by
  `/* 0001a41a */` (Ghidra entry) and a one-line description when you understand it.
* Keep original names (MacsBug symbols and `FUN_xxxxxxxx` / `segNN_xxxxxx`).
* Faithful, not clever: same branches, same evaluation order, same magic numbers
  (name them with `#define`s when you know what they are).
* `#include "port.h"`, `#include "gen/proto.h"`, `#include "gen/traps_c.h"` as needed.

## 9. Verify

```sh
make -j8 HOOK=1                    # regenerates glue; must build warning-free for your file
tools/check.sh                     # all scenarios (~1 min); or e.g. tools/check.sh newgame "save:Level 7"
```

`check.sh` runs the game deterministically with scripted input. Every call of a ported
function is replayed on the **original** 68k code in a forked process and the complete
memory images and return values are compared. Fix every `MISMATCH` (the log lines show
the first differing addresses). Functions listed as "never called" were not exercised:
review those translations against the disassembly with extra care.
Bisect with `POP2_NATIVE_EXCLUDE=Name1,Name2` or `POP2_NATIVE_ONLY=...`.

Pilot example: `src/game/seg23_rooftops.c`.
