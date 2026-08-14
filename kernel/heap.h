/* cyd-os — kernel heap.
 *
 * A first-fit free list over the DRAM left between .bss and the boot stack.
 * Predictability matters more than throughput here (UM-CYDOS-007 §5): this
 * allocator serves arena creation and a small number of kernel structures, not
 * a workload with millions of short-lived objects.
 *
 * There is no free() ordering requirement and no thread safety. Allocation
 * happens from task context only — never from an interrupt handler — because
 * the free list would need locking that the kernel has no primitive for yet.
 */

#ifndef CYDOS_HEAP_H
#define CYDOS_HEAP_H

#include <stdint.h>

/* All payloads are aligned to this. Xtensa needs 4 for word access; 8 is used
 * so a payload can hold a 64-bit quantity without the caller thinking about
 * it. */
#define HEAP_ALIGN 8u

void  heap_init(void);

/* Returns NULL on exhaustion, having incremented the failure counter. Never
 * returns a block smaller than requested, and never returns a misaligned
 * pointer. */
void *heap_alloc(uint32_t bytes);

/* Ignores NULL. A pointer that is not a live allocation is refused and counted
 * rather than acted on: a corrupt free list is far harder to diagnose later
 * than a rejected free is now. */
void  heap_free(void *p);

/* ---- instrumentation ----
 * Exported because the exit criteria are stated in terms of them, and because
 * "no leak" is only meaningful if it can be measured rather than asserted. */
uint32_t heap_total(void);        /* payload bytes if perfectly packed        */
uint32_t heap_free_bytes(void);   /* sum of all free payloads                 */
uint32_t heap_largest_free(void); /* biggest single free payload              */
uint32_t heap_used_bytes(void);   /* sum of live payloads                     */
uint32_t heap_high_water(void);   /* peak of heap_used_bytes() since init     */
uint32_t heap_blocks(void);       /* total blocks, free and used              */
uint32_t heap_fail_count(void);   /* allocations that returned NULL           */
uint32_t heap_bad_free_count(void);

/* Walks the whole list and checks structural invariants: magic values, size
 * accounting, and physical adjacency. Returns 0 if consistent. */
int heap_check(void);

#endif /* CYDOS_HEAP_H */
