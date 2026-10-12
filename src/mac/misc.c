/* Toolbox utilities, packages (SANE subset, binary-decimal), queues, strings. */
#include "mac.h"
#include "trapnums.h"
#include "../cpu/trapinfo.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>

/* ---- fixed point ---- */
static void T_FixMul(uint16_t w) { (void)w; int32_t b = (int32_t)pop32(), a = (int32_t)pop32(); ret32((uint32_t)(int32_t)(((int64_t)a * b) >> 16)); }
static void T_FixRatio(uint16_t w) { (void)w; int16_t d = pops16(), n = pops16(); ret32(d ? (uint32_t)(int32_t)(((int64_t)n << 16) / d) : (n < 0 ? 0x80000000u : 0x7FFFFFFFu)); }
static void T_FixDiv(uint16_t w) { (void)w; int32_t d = (int32_t)pop32(), n = (int32_t)pop32(); ret32(d ? (uint32_t)(int32_t)(((int64_t)n << 16) / d) : (n < 0 ? 0x80000000u : 0x7FFFFFFFu)); }
static void T_FixRound(uint16_t w) { (void)w; int32_t f = (int32_t)pop32(); ret16((uint16_t)(int16_t)((f + 0x8000) >> 16)); }
static void T_HiWord(uint16_t w) { (void)w; uint32_t v = pop32(); ret16((uint16_t)(v >> 16)); }
static void T_LoWord(uint16_t w) { (void)w; uint32_t v = pop32(); ret16((uint16_t)v); }
static void T_LongMul(uint16_t w) { (void)w; maddr r = pop32(); int32_t b = (int32_t)pop32(), a = (int32_t)pop32(); int64_t p = (int64_t)a * b; wr32(r, (uint32_t)(p >> 32)); wr32(r + 4, (uint32_t)p); }
static void T_FracMul(uint16_t w) { (void)w; int32_t b = (int32_t)pop32(), a = (int32_t)pop32(); ret32((uint32_t)(int32_t)(((int64_t)a * b) >> 30)); }
static void T_FracDiv(uint16_t w) { (void)w; int32_t d = (int32_t)pop32(), n = (int32_t)pop32(); ret32(d ? (uint32_t)(int32_t)(((int64_t)n << 30) / d) : 0x7FFFFFFF); }

/* ---- bits (bit 0 = high bit of the first byte) ---- */
static void T_BitTst(uint16_t w) { (void)w; int32_t n = (int32_t)pop32(); maddr p = pop32(); retbool((rd8(p + (n >> 3)) >> (7 - (n & 7))) & 1); }
static void T_BitSet(uint16_t w) { (void)w; int32_t n = (int32_t)pop32(); maddr p = pop32(); wr8(p + (n >> 3), rd8(p + (n >> 3)) | (uint8_t)(0x80 >> (n & 7))); }
static void T_BitClr(uint16_t w) { (void)w; int32_t n = (int32_t)pop32(); maddr p = pop32(); wr8(p + (n >> 3), rd8(p + (n >> 3)) & (uint8_t)~(0x80 >> (n & 7))); }
static void T_BitAnd(uint16_t w) { (void)w; uint32_t b = pop32(), a = pop32(); ret32(a & b); }
static void T_BitOr(uint16_t w) { (void)w; uint32_t b = pop32(), a = pop32(); ret32(a | b); }
static void T_BitXor(uint16_t w) { (void)w; uint32_t b = pop32(), a = pop32(); ret32(a ^ b); }
static void T_BitNot(uint16_t w) { (void)w; ret32(~pop32()); }
static void T_BitShift(uint16_t w) { (void)w; int16_t c = pops16(); uint32_t v = pop32(); ret32(c >= 0 ? v << (c & 31) : v >> ((-c) & 31)); }

/* ---- queues: QHdr { qFlags(2) qHead(4) qTail(4) }, QElem { qLink(4) ... } ---- */
static void T_Enqueue(uint16_t w) {
    (void)w; maddr e = A0, h = cpu_get(R_A1);
    wr32(e, 0);
    if (!rd32(h + 2)) { wr32(h + 2, e); wr32(h + 6, e); }
    else { wr32(rd32(h + 6), e); wr32(h + 6, e); }
    cpu_set(R_D0, 0);
}
static void T_Dequeue(uint16_t w) {
    (void)w; maddr e = A0, h = cpu_get(R_A1);
    maddr prev = 0, cur = rd32(h + 2);
    while (cur && cur != e) { prev = cur; cur = rd32(cur); }
    if (!cur) { cpu_set(R_D0, (uint32_t)-1); return; }
    if (prev) wr32(prev, rd32(cur)); else wr32(h + 2, rd32(cur));
    if (rd32(h + 6) == cur) wr32(h + 6, prev);
    cpu_set(R_D0, 0);
}

/* ---- strings ---- */
static uint8_t up(uint8_t c) { return (c >= 'a' && c <= 'z') ? (uint8_t)(c - 32) : c; }
static void T_UprString(uint16_t w) { (void)w; maddr s = A0; int n = (int16_t)D0; for (int i = 0; i < n; i++) wr8(s + i, up(rd8(s + i))); }
/* CmpString/EqualString: A0, A1 pointers to chars, D0 = len1<<16 | len2. Bit 9: case sensitive... returns D0 = 0 if equal */
static void T_CmpString(uint16_t w) {
    maddr a = A0, b = cpu_get(R_A1); uint32_t l = D0;
    int la = (int)(l >> 16), lb = (int)(l & 0xFFFF);
    int icase = !(w & 0x0400);
    int eq = la == lb;
    for (int i = 0; eq && i < la; i++) { uint8_t x = rd8(a + i), y = rd8(b + i); if (icase) { x = up(x); y = up(y); } if (x != y) eq = 0; }
    cpu_set(R_D0, eq ? 0 : 1);
}
static void T_RelString(uint16_t w) {
    maddr a = A0, b = cpu_get(R_A1); uint32_t l = D0;
    int la = (int)(l >> 16), lb = (int)(l & 0xFFFF);
    int icase = !(w & 0x0400);
    int r = 0;
    for (int i = 0; i < la && i < lb && !r; i++) { uint8_t x = rd8(a + i), y = rd8(b + i); if (icase) { x = up(x); y = up(y); } r = x < y ? -1 : x > y ? 1 : 0; }
    if (!r) r = la < lb ? -1 : la > lb ? 1 : 0;
    cpu_set(R_D0, (uint32_t)(int32_t)r);
}
static void T_NewString(uint16_t w) { (void)w; maddr s = pop32(); uint32_t n = rd8(s) + 1u; maddr h = NewHandle(n); mcopy(rd32(h), s, n); ret32(h); }
static void T_SetString(uint16_t w) { (void)w; maddr s = pop32(), h = pop32(); uint32_t n = rd8(s) + 1u; SetHandleSize(h, n); mcopy(rd32(h), s, n); }
static void T_GetString(uint16_t w) { (void)w; int16_t id = pops16(); ret32(GetResource(FOURCC('S','T','R',' '), id)); }

/* ---- Pack7: binary <-> decimal (selector word on stack, register args) ---- */
static void T_Pack7(uint16_t w) {
    (void)w;
    uint16_t sel = pop16();
    if (sel == 0) {          /* NumToString: D0 long, A0 str */
        char b[32]; snprintf(b, sizeof b, "%d", (int32_t)D0); c_to_pstr(b, A0);
    } else if (sel == 1) {   /* StringToNum: A0 str -> D0 */
        char b[256]; pstr_to_c(A0, b, sizeof b); cpu_set(R_D0, (uint32_t)(int32_t)strtol(b, NULL, 10));
    } else fprintf(stderr, "[pack7] selector %d unsupported\n", sel);
}

/* ---- SANE (Pack4 = FP68K): opword on the stack top, operands by address ---- */
static double rd_ext(maddr a) {
    uint16_t se = rd16(a); uint64_t m = (uint64_t)rd32(a + 2) << 32 | rd32(a + 6);
    if (!m && !(se & 0x7FFF)) return (se & 0x8000) ? -0.0 : 0.0;
    int e = (se & 0x7FFF) - 16383;
    double v = ldexp((double)m, e - 63);
    return (se & 0x8000) ? -v : v;
}
static void wr_ext(maddr a, double v) {
    if (v == 0) { wr16(a, signbit(v) ? 0x8000 : 0); wr32(a + 2, 0); wr32(a + 6, 0); return; }
    int neg = v < 0; if (neg) v = -v;
    int e; double f = frexp(v, &e);           /* v = f * 2^e, f in [0.5,1) */
    uint64_t m = (uint64_t)ldexp(f, 64);
    wr16(a, (uint16_t)((neg ? 0x8000 : 0) | (e - 1 + 16383)));
    wr32(a + 2, (uint32_t)(m >> 32)); wr32(a + 6, (uint32_t)m);
}
static double rd_fmt(maddr a, int fmt) {
    switch (fmt) {
        case 0: return rd_ext(a);
        case 1: { uint64_t u = (uint64_t)rd32(a) << 32 | rd32(a + 4); double d; memcpy(&d, &u, 8); return d; }
        case 2: { uint32_t u = rd32(a); float f; memcpy(&f, &u, 4); return f; }
        case 4: return (int16_t)rd16(a);
        case 5: return (int32_t)rd32(a);
        case 6: { int64_t c = (int64_t)((uint64_t)rd32(a) << 32 | rd32(a + 4)); return (double)c; }
    }
    return 0;
}
static void wr_fmt(maddr a, int fmt, double v) {
    switch (fmt) {
        case 0: wr_ext(a, v); break;
        case 1: { uint64_t u; memcpy(&u, &v, 8); wr32(a, (uint32_t)(u >> 32)); wr32(a + 4, (uint32_t)u); break; }
        case 2: { float f = (float)v; uint32_t u; memcpy(&u, &f, 4); wr32(a, u); break; }
        case 4: wr16(a, (uint16_t)(int16_t)lrint(v)); break;
        case 5: wr32(a, (uint32_t)(int32_t)lrint(v)); break;
        case 6: { int64_t c = llrint(v); wr32(a, (uint32_t)((uint64_t)c >> 32)); wr32(a + 4, (uint32_t)c); break; }
    }
}
static void T_Pack4(uint16_t w) {
    (void)w;
    uint16_t op = pop16();
    int fmt = (op >> 11) & 7, code = op & 0xFF;
    switch (code) {
        case 0x00: case 0x02: case 0x04: case 0x06: case 0x0C: {   /* add sub mul div rem: dst(ext) op= src */
            maddr dst = pop32(), src = pop32();
            double a = rd_ext(dst), b = rd_fmt(src, fmt), r;
            r = code == 0 ? a + b : code == 2 ? a - b : code == 4 ? a * b : code == 6 ? a / b : remainder(a, b);
            wr_ext(dst, r); break;
        }
        case 0x08: case 0x0A: {   /* compare: sets CCR */
            maddr dst = pop32(), src = pop32();
            double a = rd_ext(dst), b = rd_fmt(src, fmt);
            uint32_t sr = cpu_get(R_SR) & ~0x1F;
            if (a == b) sr |= 0x04; else if (a < b) sr |= 0x08 | 0x01;
            cpu_set(R_SR, sr); break;
        }
        case 0x0E: { maddr dst = pop32(), src = pop32(); wr_ext(dst, rd_fmt(src, fmt)); break; }         /* to extended */
        case 0x10: { maddr dst = pop32(), src = pop32(); wr_fmt(dst, fmt, rd_ext(src)); break; }         /* from extended */
        case 0x12: { maddr dst = pop32(); wr_ext(dst, sqrt(rd_ext(dst))); break; }
        case 0x14: { maddr dst = pop32(); wr_ext(dst, nearbyint(rd_ext(dst))); break; }
        case 0x16: { maddr dst = pop32(); wr_ext(dst, trunc(rd_ext(dst))); break; }
        case 0x18: { maddr dst = pop32(), src = pop32(); wr_ext(dst, ldexp(rd_ext(dst), (int16_t)rd16(src))); break; }
        case 0x1A: { maddr dst = pop32(); wr_ext(dst, logb(rd_ext(dst))); break; }
        case 0x1C: { maddr dst = pop32(), src = pop32(); double v = rd_fmt(src, fmt); wr16(dst, (uint16_t)(v == 0 ? 3 : isinf(v) ? 1 : isnan(v) ? 0 : 4)); break; }
        case 0x0B: {                                                  /* FOX2DEC: src, decform, decimal */
            maddr dec = pop32(), src = pop32(), form = pop32();
            double v = rd_fmt(src, fmt);
            int style = rd16(form) & 0xFF, digits = (int16_t)rd16(form + 2);
            char buf[64];
            if (style == 1) snprintf(buf, sizeof buf, "%.*f", digits < 0 ? 0 : digits, fabs(v));
            else snprintf(buf, sizeof buf, "%.*e", digits > 0 ? digits - 1 : 0, fabs(v));
            /* decimal record: sgn(2) exp(2) sig(Str20 = 21 bytes) */
            char sig[32] = ""; int exp10 = 0;
            if (style == 1) {
                char *dot = strchr(buf, '.');
                int frac = dot ? (int)strlen(dot + 1) : 0;
                int k = 0; for (char *p = buf; *p; p++) if (isdigit((unsigned char)*p)) sig[k++] = *p; sig[k] = 0;
                exp10 = -frac;
            } else {
                char *e = strchr(buf, 'e'); exp10 = e ? atoi(e + 1) : 0; if (e) *e = 0;
                int k = 0; for (char *p = buf; *p; p++) if (isdigit((unsigned char)*p)) sig[k++] = *p; sig[k] = 0;
                exp10 -= k - 1;
            }
            wr16(dec, v < 0 ? 1 : 0); wr16(dec + 2, (uint16_t)(int16_t)exp10); c_to_pstr(sig, dec + 4);
            break;
        }
        case 0x09: {                                                  /* FODEC2X: decimal -> dst */
            maddr dst = pop32(), dec = pop32();
            char sig[32]; pstr_to_c(dec + 4, sig, sizeof sig);
            double v = atof(sig) * pow(10.0, (int16_t)rd16(dec + 2));
            if (rd16(dec)) v = -v;
            wr_fmt(dst, fmt, v); break;
        }
        case 0x01: case 0x03: { /* set/get environment */ maddr p = pop32(); if (code == 3) wr16(p, 0); break; }
        case 0x05: case 0x07: case 0x0D: case 0x0F: case 0x11: case 0x13: case 0x15: case 0x17: case 0x19: case 0x1B: case 0x1D: case 0x1F:
            pop32(); break;   /* environment / halt / exceptions: one address operand */
        default:
            fprintf(stderr, "[sane] FP68K op %04x unsupported\n", op);
            pop32(); pop32();
    }
}
/* Elems68K (Pack5): transcendental functions on an extended at addr */
static void T_Pack5(uint16_t w) {
    (void)w; uint16_t op = pop16(); maddr dst = pop32();
    double v = rd_ext(dst);
    switch (op & 0xFF) {
        case 0x00: v = log(v); break; case 0x02: v = log2(v); break; case 0x04: v = log1p(v); break;
        case 0x06: v = log1p(v) / log(2); break; case 0x08: v = exp(v); break; case 0x0A: v = exp2(v); break;
        case 0x0C: v = expm1(v); break; case 0x0E: v = exp2(v) - 1; break;
        case 0x18: v = sin(v); break; case 0x1A: v = cos(v); break; case 0x1C: v = tan(v); break; case 0x1E: v = atan(v); break;
        default: fprintf(stderr, "[sane] Elems op %04x unsupported\n", op);
    }
    wr_ext(dst, v);
}

static void T_Pack2(uint16_t w) { (void)w; uint16_t sel = pop16(); (void)sel; fprintf(stderr, "[pack2] Disk Initialization unsupported\n"); }
static void T_Pack6(uint16_t w) {
    (void)w; uint16_t sel = pop16();
    if (!dispatch_default(0xA9ED, sel)) fprintf(stderr, "[pack6] selector %d\n", sel);
}
static void T_InitUtil(uint16_t w) { (void)w; cpu_set(R_D0, 0); }
static void T_HWPriv(uint16_t w) { (void)w; cpu_set(R_D0, 0); }
static void T_MemoryDispatch(uint16_t w) { (void)w; cpu_set(R_D0, 0); }
static void T_ReadXPRam(uint16_t w) { (void)w; uint32_t d = D0; mset(A0, 0, d >> 16); cpu_set(R_D0, 0); }
static void T_WriteParam(uint16_t w) { (void)w; cpu_set(R_D0, 0); }
static void T_CountADBs(uint16_t w) { (void)w; cpu_set(R_D0, 0); }

static void T_FlushInstructionCache(uint16_t w) { (void)w; cpu_set(R_D0, 0); }

void traps_misc_init(void) {
    trap_register(TRAP_FixMul, T_FixMul);
    trap_register(TRAP_FixRatio, T_FixRatio);
    trap_register(TRAP_FixDiv, T_FixDiv);
    trap_register(TRAP_FixRound, T_FixRound);
    trap_register(TRAP_HiWord, T_HiWord);
    trap_register(TRAP_LoWord, T_LoWord);
    trap_register(TRAP_LongMul, T_LongMul);
    trap_register(TRAP_FracMul, T_FracMul);
    trap_register(TRAP_FracDiv, T_FracDiv);
    trap_register(TRAP_BitTst, T_BitTst);
    trap_register(TRAP_BitSet, T_BitSet);
    trap_register(TRAP_BitClr, T_BitClr);
    trap_register(TRAP_BitAnd, T_BitAnd);
    trap_register(TRAP_BitOr, T_BitOr);
    trap_register(TRAP_BitXor, T_BitXor);
    trap_register(TRAP_BitNot, T_BitNot);
    trap_register(TRAP_BitShift, T_BitShift);
    trap_register(TRAP_Enqueue, T_Enqueue);
    trap_register(TRAP_Dequeue, T_Dequeue);
    trap_register(0xA054, T_UprString);
    trap_register(0xA03C, T_CmpString);
    trap_register(0xA050, T_RelString);
    trap_register(TRAP_NewString, T_NewString);
    trap_register(TRAP_SetString, T_SetString);
    trap_register(TRAP_GetString, T_GetString);
    trap_register(TRAP_Pack7, T_Pack7);
    trap_register(TRAP_Pack4, T_Pack4);
    trap_register(TRAP_Pack5, T_Pack5);
    trap_register(TRAP_Pack2, T_Pack2);
    trap_register(TRAP_Pack6, T_Pack6);
    trap_register(TRAP_InitUtil, T_InitUtil);
    trap_register(TRAP_HWPriv, T_HWPriv);
    trap_register(0xA05C, T_MemoryDispatch);
    trap_register(0xA051, T_ReadXPRam);
    trap_register(0xA038, T_WriteParam);
    trap_register(0xA077, T_CountADBs);
    (void)T_FlushInstructionCache;
}
