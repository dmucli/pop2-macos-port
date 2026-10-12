/* Support for ported game code.
 *
 * Ported functions run natively but keep the original data model: globals,
 * heap blocks and Toolbox records live in the emulated big-endian address
 * space (see mac/mem.h). Locals whose address escapes (passed to another
 * function or the Toolbox) must live on the emulated stack: use FRAME().
 */
#pragma once
#include <stdint.h>
#include <string.h>
#include "../mac/mac.h"
#include "../mac/qd.h"

/* ---- emulated registers / stack (CPU-backed today, plain variables later) ---- */
static inline maddr    emu_sp(void)            { return cpu_get(R_SP); }
static inline void     emu_set_sp(maddr v)     { cpu_set(R_SP, v); }
static inline void     emu_set_d0(uint32_t v)  { cpu_set(R_D0, v); }
extern maddr g_a5;                               /* the application's A5 (constant) */

/* ---- A5-relative globals: G16(-0x46fa) is the word at A5-0x46FA ---- */
#define GA(off)      ((maddr)(g_a5 + (int32_t)(off)))
#define G8(off)      rd8(GA(off))
#define G16(off)     rds16(GA(off))
#define GU16(off)    rd16(GA(off))
#define G32(off)     rd32(GA(off))
#define SG8(off, v)  wr8(GA(off), (uint8_t)(v))
#define SG16(off, v) wr16(GA(off), (uint16_t)(v))
#define SG32(off, v) wr32(GA(off), (uint32_t)(v))

/* Globals by their Ghidra address (the Ghidra image puts A5 at 0x108000). GH() gives an
 * address for R16/W32/...; G16/SG32/... take A5 offsets instead.
 *   R32(GH(0x102fec))  ==  G32(-0x5014)  ==  the long at A5-0x5014 */
#define GHIDRA_A5 0x108000
#define GH(addr)     ((maddr)(g_a5 + ((int32_t)(addr) - GHIDRA_A5)))

/* ---- memory helpers with explicit widths ---- */
#define R8(a)        rd8((maddr)(a))
#define RS8(a)       ((int8_t)rd8((maddr)(a)))
/* 16/32-bit reads go through port_rd*: a read of Ticks ($16A) lets virtual time pass
 * in deterministic mode exactly like the 68k core does (see runtime_ticks_peek). */
void runtime_ticks_peek(void);
static inline maddr port_peek(maddr a) { if ((a & ADDR_MASK) - 0x168u < 6u) runtime_ticks_peek(); return a; }
#define R16(a)       rds16(port_peek((maddr)(a)))
#define RU16(a)      rd16(port_peek((maddr)(a)))
#define R32(a)       rd32(port_peek((maddr)(a)))
#define W8(a, v)     wr8((maddr)(a), (uint8_t)(v))
#define W16(a, v)    wr16((maddr)(a), (uint16_t)(v))
#define W32(a, v)    wr32((maddr)(a), (uint32_t)(v))
#define DEREF(h)     rd32((maddr)(h))          /* *handle */

/* ---- locals on the emulated stack ----
 *   FRAME(f, 0x6c);          // reserves 0x6c bytes; f is the base address (like A6-0x6c)
 *   ... use f + offset ...
 *   FRAME_END(f);            // must run before every return
 */
#define FRAME(name, size)  maddr name##__sp = emu_sp(); maddr name = (name##__sp - (uint32_t)(((size) + 3) & ~3)); emu_set_sp(name)
#define FRAME_END(name)    emu_set_sp(name##__sp)

/* ---- calling original 68k functions / Toolbox traps ---- */
typedef struct { const char *name; int seg; uint32_t segoff; int jt_a5off; void (*glue)(void); int ret_bytes; } FnInfo;
extern const FnInfo fn_info[];
uint32_t call68_fn(int fn_index);              /* args already on the emulated stack */
uint32_t call68_addr(maddr addr);              /* call 68k code at an emulated address */
maddr    fn_address(int fn_index);             /* runtime address (loads its segment) */

void tb_begin(void);
void tb_trap(uint16_t word);
void tb_end(void);

/* Function pointers: the original code refers to functions through their jump-table
 * entry (A5 + offset). JT_ADDR(FN_Name) is that address; call_ptr() calls whatever an
 * emulated function pointer points to, pushing C-convention args described by fmt
 * ('w' = 16-bit, 'l' = 32-bit), e.g. call_ptr(p, "wl", 5, addr). */
#define JT_ADDR(fn)  ((maddr)(g_a5 + fn_info[fn].jt_a5off))
uint32_t call_ptr(maddr fn, const char *fmt, ...);

/* Signed views of uniform return values */
#define S16(x) ((int16_t)(x))
#define S32(x) ((int32_t)(x))
