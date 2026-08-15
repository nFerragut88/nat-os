/* nat-os — fault reporting. See panic.c. */
#ifndef NATOS_PANIC_H
#define NATOS_PANIC_H

/* Called from the exception vectors with the fault state already extracted.
 * Does not return. */
void kernel_panic(unsigned int exccause, unsigned int epc, unsigned int ps);

/* Kernel-detected failure with no hardware exception behind it — a broken stack
 * guard, say. Same ending as kernel_panic(): report, then stop.
 *
 * `why` must be a string literal in panic.o or another file whose .rodata the
 * linker places in DRAM. A string from a flash-mapped .rodata would need the
 * cache to print the reason the system is dying, which is the one moment it may
 * not be available. */
void kernel_panic_msg(const char *why, unsigned int detail);

#endif /* NATOS_PANIC_H */
