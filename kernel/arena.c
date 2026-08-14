/* cyd-os — VM application arenas. See arena.h for the isolation rationale. */

#include "arena.h"
#include "heap.h"

typedef struct {
    uint32_t base;      /* 0 when the slot is unused */
    uint32_t len;
} arena_t;

static arena_t  g_arenas[ARENA_MAX];
static uint32_t g_committed;
static uint32_t g_rejects;

int arena_create(uint32_t bytes)
{
    if (bytes == 0u) {
        g_rejects++;
        return -1;
    }

    for (int id = 0; id < ARENA_MAX; id++) {
        if (g_arenas[id].base != 0u) {
            continue;
        }

        void *p = heap_alloc(bytes);
        if (!p) {
            g_rejects++;
            return -1;          /* heap counted its own failure already */
        }

        /* Zero before handing over. An arena is application-visible memory and
         * must not leak the previous occupant's contents. */
        uint32_t *w = (uint32_t *)p;
        uint32_t  n = bytes / 4u;
        for (uint32_t i = 0; i < n; i++) {
            w[i] = 0;
        }
        uint8_t *tail = (uint8_t *)p + (n * 4u);
        for (uint32_t i = 0; i < (bytes % 4u); i++) {
            tail[i] = 0;
        }

        g_arenas[id].base = (uint32_t)p;
        g_arenas[id].len  = bytes;
        g_committed += bytes;
        return id;
    }

    g_rejects++;
    return -1;
}

void arena_destroy(int id)
{
    if (id < 0 || id >= ARENA_MAX || g_arenas[id].base == 0u) {
        g_rejects++;
        return;
    }

    heap_free((void *)g_arenas[id].base);
    g_committed -= g_arenas[id].len;
    g_arenas[id].base = 0;
    g_arenas[id].len  = 0;
}

int arena_bounds(int id, uint32_t *base, uint32_t *len)
{
    if (id < 0 || id >= ARENA_MAX || g_arenas[id].base == 0u) {
        return -1;
    }
    if (base) {
        *base = g_arenas[id].base;
    }
    if (len) {
        *len = g_arenas[id].len;
    }
    return 0;
}

int arena_contains(int id, uint32_t addr, uint32_t len)
{
    if (id < 0 || id >= ARENA_MAX || g_arenas[id].base == 0u) {
        return 0;
    }

    uint32_t base = g_arenas[id].base;
    uint32_t size = g_arenas[id].len;

    /* A zero-length access is inside any live arena provided its start is.
     * Treating it as out of bounds would reject legitimate empty copies. */
    if (addr < base) {
        return 0;
    }

    uint32_t offset = addr - base;
    if (offset > size) {
        return 0;
    }

    /* Compare in the offset domain, where the arena length bounds both terms.
     * Testing `addr + len` directly can wrap past the end of the address space
     * and report success for an access that is wildly out of range — the exact
     * case an attacker-supplied length would aim for. */
    if (len > size - offset) {
        return 0;
    }

    return 1;
}

uint32_t arena_count(void)
{
    uint32_t n = 0;
    for (int i = 0; i < ARENA_MAX; i++) {
        if (g_arenas[i].base != 0u) {
            n++;
        }
    }
    return n;
}

uint32_t arena_bytes_committed(void) { return g_committed; }
uint32_t arena_reject_count(void)    { return g_rejects; }
