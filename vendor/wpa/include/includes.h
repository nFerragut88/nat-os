/* nat-os -- the compatibility shim ESP-IDF's crypto expects.
 * next_moves/08 step 238.
 *
 * The vendored crypto is nine files from ESP-IDF's wpa_supplicant: SHA-1,
 * HMAC-SHA1, PBKDF2, the SHA-1 PRF, AES and AES key unwrap. They are the
 * primitives WPA2-PSK needs and they are NOT hand-written here, because
 * hand-rolled SHA-1 and AES is where subtle, silent, security-relevant bugs
 * live.
 *
 * What IS hand-written is the four-way handshake state machine, and that is a
 * deliberate split. Porting ESP-IDF's rsn_supp instead would mean 5,660 lines
 * of state machine handling WPA, WPA2, WPA3, enterprise, FT roaming and PMKSA
 * caching -- five protocols where nat-os needs one -- plus utils/eloop, wpabuf
 * and a set of IDF headers. Measured before choosing, not guessed.
 *
 * This header is what those nine files think they are including. IDF's real
 * utils/includes.h pulls in a libc this kernel does not have; the surface the
 * crypto actually uses turned out to be five os_* calls and two typedefs,
 * which is small enough to satisfy honestly.
 */

#ifndef WPA_INCLUDES_H
#define WPA_INCLUDES_H

#include <stdint.h>
#include <stddef.h>

/* Use the SOFTWARE implementations. ESP-IDF leaves this undefined and routes
 * SHA-1 and AES to mbedtls, which nat-os does not have -- so sha1_vector and
 * friends compile to nothing and the link fails on symbols whose source is
 * plainly sitting in the tree. Defining it selects the internal versions,
 * which are also the ones the published test vectors were written against. */
#ifndef CONFIG_CRYPTO_INTERNAL
#define CONFIG_CRYPTO_INTERNAL 1
#endif

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;

/* IDF's crypto.h marks return values __must_check, a macro their utils/common.h
 * defines. The shim replaces that header, so it must define it too -- and the
 * warn_unused_result attribute is worth keeping rather than defining away,
 * because a silently ignored crypto failure is the worst kind. */
#ifndef __must_check
#define __must_check __attribute__((warn_unused_result))
#endif

/* IDF's utils/common.h defines this to launder a pointer past GCC's strict
 * aliasing analysis. Same trick, stated plainly. */
static inline void *wpa_hide_aliasing(void *p) { return p; }
/* IDF's crypto has fault-injection hooks for its own test suite -- TEST_FAIL()
 * returns nonzero when a test wants to force a failure path. There is no test
 * harness here, so it is always false: the real code path, always taken. */
#ifndef TEST_FAIL
#define TEST_FAIL() 0
#endif

#ifndef aliasing_hide_typecast
#define aliasing_hide_typecast(a, t) (t *)wpa_hide_aliasing((void *)(a))
#endif

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

/* nat-os spells these without the prefix. kernel/kstring.c provides the first
 * three; heap.h the last two. */
void  *memcpy(void *d, const void *s, size_t n);
void  *memset(void *d, int c, size_t n);
size_t strlen(const char *s);
void  *heap_alloc(uint32_t n);
void   heap_free(void *p);

#define os_memcpy(d, s, n)  memcpy((d), (s), (n))
#define os_memset(d, c, n)  memset((d), (c), (n))
#define os_memcmp(a, b, n)  memcmp((a), (b), (n))
#define os_strlen(s)        strlen((s))
#define os_malloc(n)        heap_alloc((uint32_t)(n))
#define os_free(p)          heap_free((p))
#define os_zalloc(n)        wpa_zalloc((size_t)(n))

int memcmp(const void *a, const void *b, size_t n);
void *wpa_zalloc(size_t n);

/* Overwrite a key buffer so it does not linger in RAM. Marked volatile-through
 * so the compiler may not optimise the store away, which is the entire point
 * of the function and the usual way it gets silently removed. */
static inline void forced_memzero(void *p, size_t n)
{
    volatile unsigned char *q = (volatile unsigned char *)p;
    while (n--) { *q++ = 0u; }
}

#endif /* WPA_INCLUDES_H */
