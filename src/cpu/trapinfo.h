#pragma once
#include <stdint.h>
typedef struct { uint16_t word; const char *name; int16_t pop; int8_t retsize; int8_t regbased; } TrapInfo;
extern const TrapInfo trap_info[];
const TrapInfo *trap_lookup(uint16_t word);
typedef struct { uint16_t trap; uint32_t selector; const char *name; int16_t pop; int8_t retsize; const char *loc; } DispatchInfo;
extern const DispatchInfo dispatch_info[];
/* Find a selector entry; for D0<0xFF>/D0W/StackW matching uses the low bits. */
const DispatchInfo *dispatch_lookup(uint16_t trap, uint32_t selector);
/* Default handling for an unimplemented selector: pops arguments (selector already
 * popped for Stack* locations), zeroes the result, logs once. Returns 0 if unknown. */
int dispatch_default(uint16_t trap, uint32_t selector);
