/* Soft-math helpers libdragon's bundled libgcc.a doesn't ship for
 * -mabi=32 -mips3. Compiled as C so symbol names match the compiler-emitted
 * calls without mangling. */

#include <stdint.h>

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

uint64_t __udivdi3(uint64_t n, uint64_t d)
{
    if (d == 0) return 0;

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

int64_t __divdi3(int64_t a, int64_t b)
{
    int neg = 0;
    uint64_t ua, ub;
    if (a < 0) { ua = (uint64_t)(-a); neg ^= 1; } else { ua = (uint64_t)a; }
    if (b < 0) { ub = (uint64_t)(-b); neg ^= 1; } else { ub = (uint64_t)b; }
    uint64_t q = __udivdi3(ua, ub);
    return neg ? -(int64_t)q : (int64_t)q;
}

int64_t __moddi3(int64_t a, int64_t b)
{
    int neg = (a < 0);
    uint64_t ua = (a < 0) ? (uint64_t)(-a) : (uint64_t)a;
    uint64_t ub = (b < 0) ? (uint64_t)(-b) : (uint64_t)b;
    uint64_t r = __umoddi3(ua, ub);
    return neg ? -(int64_t)r : (int64_t)r;
}
