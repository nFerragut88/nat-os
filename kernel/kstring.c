/* nat-os — the memory routines the compiler is entitled to call.
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

/* ---- lwIP port shims -- next_moves/08 step 232 --------------------------
 *
 * lwIP needs a millisecond clock, a random source, an assertion sink, and the
 * handful of str* functions this kernel never needed before. Nothing here is
 * lwIP-specific in principle; it is the libc surface a third-party library
 * assumes and a freestanding kernel does not have.
 */

uint32_t sys_now(void);
uint32_t sys_now(void)
{
    /* lwIP measures timeouts in milliseconds. timer_ticks() is 10 ms, which
     * makes this exact rather than approximate -- and 32 bits of milliseconds
     * wraps in 49 days, which lwIP handles by design. */
    extern uint32_t timer_ticks(void);
    return timer_ticks() * 10u;
}

unsigned int lwip_rand_u32(void);
unsigned int lwip_rand_u32(void)
{
    /* The hardware RNG. A constant here would make every TCP initial sequence
     * number and every DHCP transaction id predictable. */
    extern uint32_t osi_impl_random(void);
    return (unsigned int)osi_impl_random();
}

void lwip_die(const char *msg);
void lwip_die(const char *msg)
{
    extern void uart_puts(const char *s);
    uart_puts("\n*** LWIP ASSERT: ");
    uart_puts(msg ? msg : "(null)");
    uart_puts(" ***\n");
    for (;;) { }
}

size_t strlen(const char *s);
size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) { p++; }
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b);
int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n);
int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (int)(unsigned char)*a - (int)(unsigned char)*b : 0;
}


/* ---- newlib's ctype table -- next_moves/08 step 233 ---------------------
 *
 * lwIP's ip4addr_aton() uses isdigit() and isxdigit(), which newlib implements
 * as macros indexing a shared table rather than as functions. Linking lwIP
 * against a -nostdlib kernel therefore fails on `_ctype_` -- a symbol nothing
 * in this project had ever needed, and one no amount of reading lwIP would
 * predict, because in lwIP's source it appears only as isdigit().
 *
 * Indexed as _ctype_[c + 1]: slot 0 covers EOF (-1), so a lookup on EOF does
 * not read before the array. Generated rather than typed.
 *
 *   _U 01 upper   _L 02 lower   _N 04 digit   _S 010 space
 *   _P 020 punct  _C 040 control  _X 0100 hex  _B 0200 blank
 */
const char _ctype_[257] = {
      0,  32,  32,  32,  32,  32,  32,  32,  32,  32,  40,  40,
     40,  40,  40,  32,  32,  32,  32,  32,  32,  32,  32,  32,
     32,  32,  32,  32,  32,  32,  32,  32,  32, 136,  16,  16,
     16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,  16,
     16,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,  16,
     16,  16,  16,  16,  16,  16,  65,  65,  65,  65,  65,  65,
      1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,
      1,   1,   1,   1,   1,   1,   1,   1,  16,  16,  16,  16,
     16,  16,  66,  66,  66,  66,  66,  66,   2,   2,   2,   2,
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
      2,   2,   2,   2,  16,  16,  16,  16,  32,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,
};

/* Used by lwIP's netif_index_to_name path. Deliberately minimal and
 * deliberately NOT a strtol: no sign handling beyond '-', no bases, no
 * overflow detection. lwIP calls it on its own generated digits. */
int atoi(const char *s);
int atoi(const char *s)
{
    int sign = 1, v = 0;
    while (*s == ' ' || (*s >= 9 && *s <= 13)) { s++; }
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v * sign;
}
