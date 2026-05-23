/**
 * @file N64SoftMath.c
 * @brief Soft-math helpers libdragon's libgcc.a doesn't ship for
 *        `-mabi=32 -mips3`.
 *
 * mips64-elf-gcc 14.2.0 emits calls to these compiler-rt-style helpers for
 * operations the underlying ISA can't do natively:
 *
 *   __clzsi2   — count leading zeros in uint32_t. mips3 has no `clz`
 *                instruction (mips32r2+ does); gcc falls back to a call.
 *   __divdi3   — signed   int64_t / int64_t. mips3 in `-mabi=32` mode has
 *                only 32-bit registers; 64-bit ops route through helpers.
 *   __udivdi3  — unsigned uint64_t / uint64_t. Same reason.
 *
 * libdragon's gcc-14.2 distribution for this multilib doesn't include
 * them in the bundled libgcc.a — engine code that uses 64-bit division
 * or count-leading-zeros (Renderer's frustum culling, regex compilation,
 * SYS_GetTimeMicroseconds' tick conversion, etc.) fails to link.
 *
 * Implementations are the canonical public-domain shift-subtract / binary-
 * search algorithms. Performance is not critical — these are rare ops on
 * N64 hot paths (most engine math is float, and clz is used in fixed-size
 * lookup hashing where the call cost is dwarfed by the surrounding work).
 *
 * Compiled as C (not C++) so name mangling doesn't apply and the symbols
 * land in the link with exactly the names gcc generates calls to.
 */

#include <stdint.h>

/* Count leading zeros: returns 0..31 for x != 0, undefined for x == 0
 * (libgcc convention — gcc never calls clzsi2 with x == 0). */
int __clzsi2(unsigned int x)
{
    int n = 0;
    if ((x & 0xFFFF0000u) == 0) { n += 16; x <<= 16; }
    if ((x & 0xFF000000u) == 0) { n +=  8; x <<=  8; }
    if ((x & 0xF0000000u) == 0) { n +=  4; x <<=  4; }
    if ((x & 0xC0000000u) == 0) { n +=  2; x <<=  2; }
    if ((x & 0x80000000u) == 0) { n +=  1;            }
    return n;
}

/* Unsigned 64-bit division by shift-subtract. ~64 iterations worst case;
 * fast enough for engine use. Returns 0 if d == 0 (libgcc doesn't define
 * behaviour either — division by zero is UB and gcc warns/traps at the
 * caller). */
uint64_t __udivdi3(uint64_t n, uint64_t d)
{
    if (d == 0) return 0; /* defensive — UB at the C level */

    uint64_t q = 0;
    uint64_t r = 0;
    for (int i = 63; i >= 0; --i)
    {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d)
        {
            r -= d;
            q |= ((uint64_t)1) << i;
        }
    }
    return q;
}

/* Unsigned 64-bit modulo via the same shift-subtract loop. Some libgcc
 * variants also use this internally; safe to provide. */
uint64_t __umoddi3(uint64_t n, uint64_t d)
{
    if (d == 0) return 0;

    uint64_t r = 0;
    for (int i = 63; i >= 0; --i)
    {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) r -= d;
    }
    return r;
}

/* Signed 64-bit division: factor out signs, do unsigned, restore. */
int64_t __divdi3(int64_t a, int64_t b)
{
    int neg = 0;
    uint64_t ua, ub;
    if (a < 0) { ua = (uint64_t)(-a); neg ^= 1; } else { ua = (uint64_t)a; }
    if (b < 0) { ub = (uint64_t)(-b); neg ^= 1; } else { ub = (uint64_t)b; }
    uint64_t q = __udivdi3(ua, ub);
    return neg ? -(int64_t)q : (int64_t)q;
}

/* Signed 64-bit modulo: result takes sign of dividend (C99/C++11 rule). */
int64_t __moddi3(int64_t a, int64_t b)
{
    int neg = (a < 0);
    uint64_t ua = (a < 0) ? (uint64_t)(-a) : (uint64_t)a;
    uint64_t ub = (b < 0) ? (uint64_t)(-b) : (uint64_t)b;
    uint64_t r = __umoddi3(ua, ub);
    return neg ? -(int64_t)r : (int64_t)r;
}
