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

#define ARENA_MAX 4

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
