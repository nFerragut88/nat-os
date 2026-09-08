/* nat-os — VM application arenas.
 *
 * An arena is a contiguous block of DRAM with a recorded base and length. It is
 * the unit of memory an application owns, and its bounds are what the bytecode
 * interpreter will check every load and store against (UM-NATOS-001 §4.2).
 *
 * This is the ONLY isolation mechanism this kernel will have. The ESP32 has no
 * MMU paging, so there is no hardware that can be asked to enforce these
 * bounds — the interpreter must do it in software on every access, and it must
 * not be compiled out for speed. Native tasks are not confined by arenas and
 * never will be; they are trusted code.
 *
 * Arenas are deliberately not resizable. A growing arena would invalidate the
 * base the interpreter is holding, and a bytecode program that survives its
 * memory moving underneath it is a much harder thing to get right than one
 * that is told its size once.
 */

#ifndef NATOS_ARENA_H
#define NATOS_ARENA_H

#include <stdint.h>
#include "app.h"                /* APP_MAX -- see the count below */

/* [step 367] APP_MAX + 1, and the +1 is the point.
 *
 * This was a bare 4, the same number as APP_MAX, which read as "one arena per
 * application" and was not. kmain.c takes one at boot for the kernel's own VM
 * task and never releases it, so four arenas meant THREE applications -- and
 * with ping and pong starting at boot and never exiting, exactly one slot was
 * left. `run paint` while anything else ran reported "no free slot or no
 * memory" with 28 KB of heap free.
 *
 * Nobody chose that. Two independent limits happened to be the same number and
 * one of them silently had a tenant.
 *
 * The kernel's arena is not an application's, so it gets its own room. The
 * assert below is what keeps this true: raising APP_MAX without raising this
 * would quietly go back to an application being unable to start while the
 * kernel holds one. */
#define ARENA_MAX (APP_MAX + 1)

/* Returns an id >= 0, or -1 if no slot is free or the heap cannot satisfy the
 * request. Contents are zeroed: an application must not be able to read
 * whatever the previous occupant left behind. */
int arena_create(uint32_t bytes);

/* Releases the arena. Ignores an invalid id rather than trapping, and counts
 * it. */
void arena_destroy(int id);

/* The interpreter's query. Writes base and length and returns 0 on success,
 * non-zero for an unknown id. */
int arena_bounds(int id, uint32_t *base, uint32_t *len);

/* The bounds check itself, exported so there is exactly one implementation of
 * it rather than one per caller. Returns non-zero when [addr, addr+len) lies
 * entirely inside the arena. Overflow-safe: an addr+len that wraps is refused,
 * which is the case a naive check gets wrong. */
int arena_contains(int id, uint32_t addr, uint32_t len);

uint32_t arena_count(void);
uint32_t arena_bytes_committed(void);
uint32_t arena_reject_count(void);

#endif /* NATOS_ARENA_H */
