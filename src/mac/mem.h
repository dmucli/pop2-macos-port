/* Flat big-endian "classic Mac" address space shared by ported C code, the
 * Toolbox reimplementation and the 68k CPU core. Addresses are 24-bit clean
 * (high byte ignored), like a Mac running in 24-bit mode. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define MEM_SIZE   0x01000000u          /* 16 MB */
#define ADDR_MASK  0x00FFFFFFu

typedef uint32_t maddr;                 /* address in emulated memory */

extern uint8_t *M;

static inline uint8_t *mptr(maddr a) { return M + (a & ADDR_MASK); }
static inline uint8_t  rd8(maddr a)  { return M[a & ADDR_MASK]; }
static inline uint16_t rd16(maddr a) { const uint8_t *p = mptr(a); return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t rd32(maddr a) { const uint8_t *p = mptr(a); return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline void wr8(maddr a, uint8_t v)   { M[a & ADDR_MASK] = v; }
static inline void wr16(maddr a, uint16_t v) { uint8_t *p = mptr(a); p[0] = v >> 8; p[1] = (uint8_t)v; }
static inline void wr32(maddr a, uint32_t v) { uint8_t *p = mptr(a); p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }
static inline int16_t  rds16(maddr a) { return (int16_t)rd16(a); }
static inline int8_t   rds8(maddr a)  { return (int8_t)rd8(a); }
static inline void mcopy(maddr dst, maddr src, uint32_t n) { memmove(mptr(dst), mptr(src), n); }
static inline void mset(maddr dst, int v, uint32_t n) { memset(mptr(dst), v, n); }
static inline void mput(maddr dst, const void *src, uint32_t n) { memcpy(mptr(dst), src, n); }
static inline void mget(void *dst, maddr src, uint32_t n) { memcpy(dst, mptr(src), n); }

/* Big-endian helpers for host buffers */
static inline uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }
static inline void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }

/* Pascal strings in emulated memory */
void   pstr_to_c(maddr p, char *out, size_t outsz);
void   c_to_pstr(const char *s, maddr p);

/* ---- layout of the emulated address space ---- */
#define LOWMEM_END      0x00003000u
#define SYSHEAP_START   0x00003000u     /* system heap: Toolbox records we create */
#define SYSHEAP_END     0x00100000u
#define APPHEAP_START   0x00100000u     /* application zone */
#define APPHEAP_END     0x00D00000u
#define STACK_TOP       0x00E00000u     /* A5 world sits just below the stack; see loader */
#define SCREEN_BASE     0x00E00000u     /* 8-bit framebuffer (up to 1 MB) */
#define THUNK_BASE      0x00F80000u     /* native-call thunks: one F-line word + RTS each */
#define THUNK_COUNT     0x1000u
#define IRQ_STACK_TOP   0x00FF0000u     /* stack for interrupt-level code (tasks, callbacks) */
#define IRQ_STACK_SIZE  0x00008000u     /* zeroed before each outermost interrupt-level call */
