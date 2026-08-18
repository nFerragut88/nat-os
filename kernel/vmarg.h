/* nat-os — the validated-argument harness.
 *
 * UM-NATOS-007 §2.1 and book chapter 31 name the same gap: every syscall
 * validates its own arguments, four places in the tree do it correctly, each was
 * reasoned about individually, and
 *
 *     "nothing enforces that a future one will, and there is no shared harness
 *      that would catch an unchecked length in a new service."
 *
 * Twelve services was tolerable. A device model turns twelve into any number,
 * and an ad-hoc check per service does not survive that. This is the one place a
 * program-supplied quantity is checked, and it exists BEFORE the device model
 * rather than alongside it, so the model has nothing to invent.
 *
 * Nothing here is new policy. Every rule below is lifted from a site that
 * already got it right, and the point is that there is now one copy:
 *
 *   offset domain      vm_in_bounds() compares `len <= size - off`, never
 *                      `off + len <= size`, because the sum wraps and a hostile
 *                      length aims squarely at that.
 *   bound then multiply
 *                      SYS BLIT bounds w and h against the panel BEFORE
 *                      computing w*h*2. A 65536x1 image passes a byte check and
 *                      then blits whatever follows it.
 *   copy, do not lend  copy_string() walks the arena a bounds-checked byte at a
 *                      time into kernel memory, because display_text() takes a
 *                      kernel pointer and an arena address would let the
 *                      program's own stores change the string mid-render.
 *
 * Every function returns 1 on success and 0 on failure, and a failure has
 * ALREADY recorded the fault on the vm. The caller's whole error path is
 * therefore `return 1;` — refusing without diagnosing is what this replaces.
 */

#ifndef NATOS_VMARG_H
#define NATOS_VMARG_H

#include <stdint.h>
#include "vm.h"

/* A checked view of program memory.
 *
 * `ptr` is a KERNEL pointer and is valid only until the calling service
 * returns — the arena it points into belongs to a program that runs again the
 * moment the syscall ends. Anything outliving the call must be copied; see
 * vmarg_string(), which exists because that rule was learned the hard way. */
typedef struct {
    const uint8_t *ptr;
    uint32_t       len;
} vm_span_t;

/* An (offset, length) pair, bounds-checked in the offset domain.
 *
 * `align` is a power of two, or 1 for no alignment requirement; a misaligned
 * offset faults rather than silently reading a shifted structure, which is the
 * rule SYS BLIT applies to its 16-bit pixels. */
int vmarg_span(vm_t *vm, uint32_t off, uint32_t len, uint32_t align,
               vm_span_t *out);

/* `count` elements of `elem` bytes. The generalisation of SYS BLIT's rule:
 * `count` is rejected against `max_items` BEFORE it is multiplied, so the
 * product cannot wrap. Pass the largest count the service could ever legitimately
 * want — not UINT32_MAX, which defeats the check it is there to perform. */
int vmarg_items(vm_t *vm, uint32_t off, uint32_t count, uint32_t elem,
                uint32_t max_items, uint32_t align, vm_span_t *out);

/* Copies a NUL-terminated string out of the arena into caller storage. Returns
 * 0 only on a bounds fault; a string longer than `max` is truncated and is NOT
 * a fault, matching the existing contract. */
int vmarg_string(vm_t *vm, uint32_t off, char *dst, uint32_t max);

/* A single word, aligned and bounds-checked, copied out. Convenience for
 * services that take a small struct by offset. */
int vmarg_u32(vm_t *vm, uint32_t off, uint32_t *out);

/* Copies kernel data INTO the arena, bounds-checked.
 *
 * Deliberately a copy rather than a mutable span. Handing a service a writable
 * pointer into a program's memory is the mirror of the borrowed-string mistake
 * and fails the same way: the pointer outlives the check the moment anyone
 * stores it. Services that need to return bytes hand them here instead, and no
 * writable arena pointer ever leaves this file. */
int vmarg_store(vm_t *vm, uint32_t off, const void *src, uint32_t len);

/* How many arguments have been checked, and how many were rejected.
 *
 * A harness whose rejection count is zero across a long run is either perfect or
 * unreachable, and those look identical from the outside. m4_selftest() drives
 * known-bad arguments through it so the number is known to move. */
uint32_t vmarg_checks(void);
uint32_t vmarg_rejects(void);

#endif /* NATOS_VMARG_H */
