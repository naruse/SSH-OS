/* Minimal C library pieces. The compiler may call memcpy/memset on its own,
 * so these must exist even where the kernel never calls them by name. */
#include "kernel.h"

/* True when a pointer can be used for 32-bit access (the 68000 faults on
 * word or long access to an odd address). */
static int is_even(const void *p)
{
    return ((u32)p & 1) == 0;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    u8 *d = dst;
    const u8 *s = src;

    /* Two odd addresses become even after one byte. Only when one is odd
     * and the other even is there no way round the byte loop. */
    if (n && !is_even(d) && !is_even(s)) {
        *d++ = *s++;
        n--;
    }
    if (is_even(d) && is_even(s)) {
        u32 *dl = (u32 *)d;
        const u32 *sl = (const u32 *)s;
        for (; n >= 4; n -= 4)
            *dl++ = *sl++;
        d = (u8 *)dl;
        s = (const u8 *)sl;
    }
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    u8 *d = dst;
    const u8 *s = src;

    if (d <= s || d >= s + n)
        return memcpy(dst, src, n);

    /* Overlapping with dst above src: copy backwards. */
    d += n;
    s += n;
    while (n--)
        *--d = *--s;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    u8 *d = dst;

    if (is_even(d)) {
        u32 fill = (u8)c;
        u32 *dl = (u32 *)d;
        fill |= fill << 8;
        fill |= fill << 16;
        for (; n >= 4; n -= 4)
            *dl++ = fill;
        d = (u8 *)dl;
    }
    while (n--)
        *d++ = (u8)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;

    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x < *y ? -1 : 1;
    return 0;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n])
        n++;
    return n;
}
