/* cyd-os — kernel heap.
 *
 * Design: an address-ordered doubly-linked list of blocks covering the entire
 * heap region with no gaps. Every block carries a 16-byte header holding its
 * physical neighbours, its payload size, and a magic word that doubles as the
 * used/free flag.
 *
 * Address-ordered with physical links means coalescing is O(1) in both
 * directions and needs no search. The cost is 16 bytes per allocation, which is
 * the right trade here: arenas are large and few, and the alternative — a
 * segregated free list — buys throughput this kernel does not need in exchange
 * for state that is harder to verify.
 *
 * The magic word is the reason a double free or a wild pointer is reported
 * instead of silently linking garbage into the list. On a kernel with no memory
 * protection (UM-CYDOS-001 §4.2) that distinction is most of the debugging
 * budget.
 */

#include "heap.h"
#include "critical.h"

/* Magic values chosen to be implausible as data, and distinct from each other
 * so a block's state is unambiguous even in a raw memory dump. */
#define BLK_FREE 0xF2EEB10Cu
#define BLK_USED 0x05EDB10Cu

typedef struct block {
    struct block *prev;     /* physically previous block, NULL at heap start */
    struct block *next;     /* physically next block, NULL at heap end       */
    uint32_t      size;     /* payload bytes, always a multiple of HEAP_ALIGN */
    uint32_t      magic;    /* BLK_FREE or BLK_USED                          */
} block_t;

#define HDR_BYTES ((uint32_t)sizeof(block_t))

/* Splitting only pays if the remainder can hold a header plus a useful
 * payload. Below this the leftover is left attached to the allocation, which
 * wastes a few bytes but avoids manufacturing unusable slivers. */
#define SPLIT_MIN (HDR_BYTES + HEAP_ALIGN)

extern char _heap_start;
extern char _heap_end;

static block_t *g_first;
static uint32_t g_total;
static uint32_t g_used;
static uint32_t g_high_water;
static uint32_t g_fail;
static uint32_t g_bad_free;

static uint32_t align_up(uint32_t v)
{
    return (v + (HEAP_ALIGN - 1u)) & ~(HEAP_ALIGN - 1u);
}

static void *payload_of(block_t *b)
{
    return (void *)((char *)b + HDR_BYTES);
}

void heap_init(void)
{
    uint32_t start = align_up((uint32_t)&_heap_start);
    uint32_t end   = (uint32_t)&_heap_end & ~(HEAP_ALIGN - 1u);

    g_first = 0;
    g_total = g_used = g_high_water = g_fail = g_bad_free = 0;

    if (end <= start || (end - start) < (HDR_BYTES + HEAP_ALIGN)) {
        return;                 /* no usable heap; every alloc will fail */
    }

    g_first        = (block_t *)start;
    g_first->prev  = 0;
    g_first->next  = 0;
    g_first->size  = (end - start) - HDR_BYTES;
    g_first->magic = BLK_FREE;

    g_total = g_first->size;
}

void *heap_alloc(uint32_t bytes)
{
    if (bytes == 0u) {
        return 0;               /* a zero-size allocation has no useful answer */
    }

    uint32_t want = align_up(bytes);

    /* The free list is walked and rewritten here, and a tick landing mid-split
     * would let another task observe — or allocate from — a half-linked block.
     * A critical section rather than a mutex: these are a handful of memory
     * operations, and a mutex would make the allocator depend on the scheduler
     * that may itself want to allocate. */
    uint32_t crit = crit_enter();

    for (block_t *b = g_first; b; b = b->next) {
        if (b->magic != BLK_FREE || b->size < want) {
            continue;
        }

        /* Split only when the tail can stand on its own. */
        if (b->size - want >= SPLIT_MIN) {
            block_t *tail = (block_t *)((char *)b + HDR_BYTES + want);
            tail->size  = b->size - want - HDR_BYTES;
            tail->magic = BLK_FREE;
            tail->prev  = b;
            tail->next  = b->next;
            if (tail->next) {
                tail->next->prev = tail;
            }
            b->next = tail;
            b->size = want;

            /* The new header consumes payload that used to be allocatable. */
            g_total -= HDR_BYTES;
        }

        b->magic = BLK_USED;
        g_used += b->size;
        if (g_used > g_high_water) {
            g_high_water = g_used;
        }
        crit_exit(crit);
        return payload_of(b);
    }

    g_fail++;
    crit_exit(crit);
    return 0;
}

/* Merge b with its successor when both are free. */
static void coalesce_with_next(block_t *b)
{
    block_t *n = b->next;
    if (!n || b->magic != BLK_FREE || n->magic != BLK_FREE) {
        return;
    }

    b->size += HDR_BYTES + n->size;
    b->next  = n->next;
    if (n->next) {
        n->next->prev = b;
    }

    /* The absorbed header becomes allocatable payload again. */
    g_total += HDR_BYTES;

    n->magic = 0;               /* poison, so a stale pointer is not mistaken
                                 * for a live header */
}

void heap_free(void *p)
{
    if (!p) {
        return;                 /* free(NULL) is defined as a no-op */
    }

    uint32_t addr = (uint32_t)p;
    uint32_t lo   = (uint32_t)&_heap_start;
    uint32_t hi   = (uint32_t)&_heap_end;

    if (addr < lo + HDR_BYTES || addr >= hi) {
        g_bad_free++;
        return;
    }

    block_t *b = (block_t *)((char *)p - HDR_BYTES);

    uint32_t crit = crit_enter();

    /* A live allocation is the only thing that may be freed. Anything else —
     * double free, interior pointer, wild pointer — is counted and refused.
     * Checked inside the section: two tasks freeing the same pointer must not
     * both see BLK_USED. */
    if (b->magic != BLK_USED) {
        g_bad_free++;
        crit_exit(crit);
        return;
    }

    b->magic = BLK_FREE;
    g_used  -= b->size;

    coalesce_with_next(b);
    if (b->prev) {
        coalesce_with_next(b->prev);
    }

    crit_exit(crit);
}

uint32_t heap_total(void)          { return g_total; }
uint32_t heap_used_bytes(void)     { return g_used; }
uint32_t heap_high_water(void)     { return g_high_water; }
uint32_t heap_fail_count(void)     { return g_fail; }
uint32_t heap_bad_free_count(void) { return g_bad_free; }

uint32_t heap_free_bytes(void)
{
    uint32_t n = 0;
    for (block_t *b = g_first; b; b = b->next) {
        if (b->magic == BLK_FREE) {
            n += b->size;
        }
    }
    return n;
}

uint32_t heap_largest_free(void)
{
    uint32_t best = 0;
    for (block_t *b = g_first; b; b = b->next) {
        if (b->magic == BLK_FREE && b->size > best) {
            best = b->size;
        }
    }
    return best;
}

uint32_t heap_blocks(void)
{
    uint32_t n = 0;
    for (block_t *b = g_first; b; b = b->next) {
        n++;
    }
    return n;
}

int heap_check(void)
{
    uint32_t lo = align_up((uint32_t)&_heap_start);
    uint32_t hi = (uint32_t)&_heap_end & ~(HEAP_ALIGN - 1u);

    if (!g_first) {
        return g_total == 0u ? 0 : 1;
    }
    if ((uint32_t)g_first != lo) {
        return 2;
    }

    uint32_t span = 0, used = 0;
    block_t *prev = 0;

    for (block_t *b = g_first; b; prev = b, b = b->next) {
        if (b->magic != BLK_FREE && b->magic != BLK_USED) {
            return 3;                                  /* corrupt header */
        }
        if (b->prev != prev) {
            return 4;                                  /* broken back link */
        }
        if ((b->size % HEAP_ALIGN) != 0u) {
            return 5;
        }
        /* Blocks must tile the region exactly: the next header has to begin
         * where this block's payload ends. A gap or overlap means the split or
         * coalesce arithmetic is wrong. */
        char *end = (char *)b + HDR_BYTES + b->size;
        if (b->next && (char *)b->next != end) {
            return 6;
        }
        if (!b->next && (uint32_t)end != hi) {
            return 7;
        }
        /* Two adjacent free blocks mean a coalesce was missed, which is how a
         * heap fragments itself to death while reporting plenty free. */
        if (b->magic == BLK_FREE && b->next && b->next->magic == BLK_FREE) {
            return 8;
        }

        span += b->size;
        if (b->magic == BLK_USED) {
            used += b->size;
        }
    }

    if (used != g_used) {
        return 9;
    }
    if (span != g_total) {
        return 10;
    }
    return 0;
}
