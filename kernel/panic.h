/* cyd-os — fault reporting. See panic.c. */
#ifndef CYDOS_PANIC_H
#define CYDOS_PANIC_H

/* Called from the exception vectors with the fault state already extracted.
 * Does not return. */
void kernel_panic(unsigned int exccause, unsigned int epc, unsigned int ps);

#endif /* CYDOS_PANIC_H */
