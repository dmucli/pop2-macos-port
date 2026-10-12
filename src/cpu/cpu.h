/* 68k CPU glue (Musashi): register access, stack helpers, native thunks,
 * nested calls from C into 68k code, and the line-A trap dispatcher. */
#pragma once
#include <stdint.h>
#include "../mac/mem.h"

enum { R_D0, R_D1, R_D2, R_D3, R_D4, R_D5, R_D6, R_D7,
       R_A0, R_A1, R_A2, R_A3, R_A4, R_A5, R_A6, R_SP, R_PC, R_SR };

uint32_t cpu_get(int r);
void     cpu_set(int r, uint32_t v);
#define D0 cpu_get(R_D0)
#define A0 cpu_get(R_A0)

void     cpu_init(void);
void     cpu_run_forever(void);           /* main emulation loop */
void     cpu_stop(void);

/* stack helpers (operate on the 68k SP) */
uint32_t pop32(void);
uint16_t pop16(void);
uint8_t  pop8(void);                      /* Pascal Boolean/char: word on stack, value in high byte */
void     push32(uint32_t v);
void     push16(uint16_t v);
static inline int16_t pops16(void) { return (int16_t)pop16(); }
/* After popping a Pascal function's arguments, SP points at the result slot. */
void     ret16(uint16_t v);
void     ret32(uint32_t v);
void     retbool(int v);

/* Native code reachable from 68k code. Executing the thunk address runs fn as a
 * subroutine (return address is popped before fn runs; fn may change trap_ret). */
typedef void (*NativeFn)(void);
maddr    cpu_thunk(NativeFn fn, const char *name);
extern maddr trap_ret;                    /* where execution resumes after a native routine */

/* Call 68k code at pc from C. Arguments must already be pushed. Saves and restores
 * the full CPU context (except memory); returns D0, and A0 via *a0 if non-null. */
uint32_t cpu_call(maddr pc, uint32_t *a0);
/* Same, but with explicit initial register values (used for Time Manager / VBL tasks). */
uint32_t cpu_call_regs(maddr pc, const uint32_t *regs16, uint32_t *a0);

/* ---- traps ---- */
typedef void (*TrapFn)(uint16_t trapword);
void   trap_register(uint16_t word, TrapFn fn);
maddr  trap_get_address(uint16_t word);           /* native thunk or patched address */
void   trap_set_address(uint16_t word, maddr addr);
extern uint16_t cur_trap_word;
void   trap_unimplemented(uint16_t word);

/* Periodic work (timers, VBL, sound) run between CPU slices. */
void   cpu_set_idle_hook(void (*fn)(void));
extern volatile int cpu_running;
extern uint64_t cpu_instr_count;
