/* cyd-os — the memory routines the compiler is entitled to call.
 *
 * These are not here because kernel code calls them by name. They are here
 * because GCC synthesises calls to memcpy/memset from ordinary C — a byte-copy
 * loop, a struct assignment, a large local initialiser — and does so even under
 * -fno-builtin, which only stops it treating them as builtins when they ARE
 * written by name. Under -nostdlib nothing supplies them, and the failure is a
 * link error naming a function the source never mentions.
 *
 * The build passes -fno-tree-loop-distribute-patterns, which is what stops GCC
 * recognising the loop inside memcpy() below as a memcpy and rewriting it into
 * a call to itself. That particular bug is silent, infinitely recursive, and
 * would present as a stack overflow in whatever unrelated code first copied a
 * struct.
 */

#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char       *d = dst;
    const unsigned char *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    while (n--) {
        *d++ = (unsigned char)c;
    }
    return dst;
}

/* Overlap-safe: copies backwards when the regions overlap the wrong way. */
void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char       *d = dst;
    const unsigned char *s = src;

    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    while (n--) {
        if (*p != *q) {
            return (int)*p - (int)*q;
        }
        p++;
        q++;
    }
    return 0;
}
