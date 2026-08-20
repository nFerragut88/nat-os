/* nat-os — calling the vendor blob with the scheduler running. See blobcall.c. */
#ifndef NATOS_BLOBCALL_H
#define NATOS_BLOBCALL_H

#include <stdint.h>

/* Enters the blob holding a mutex instead of masking interrupts, so the
 * scheduler keeps running and blob code that blocks can make progress.
 *
 * Use this for anything that may ask the OS adapter for a task, a queue or a
 * semaphore. phy_stack_call() direct is still correct for self-contained work
 * that asks the OS for nothing -- register_chipv7_phy is the example. */
uint32_t blob_call(uint32_t fn, uint32_t a, uint32_t b, uint32_t c, uint32_t d);

void     blob_call_init(void);
uint32_t blob_call_count(void);
uint32_t blob_call_contended(void);   /* must stay 0 with a single caller */

/* Off by default: a blob task runs windowed code concurrently with a caller
 * that is also inside windowed code, and that collides. See blobcall.c. */
void     blob_lock(void);
void     blob_unlock(void);
void     blob_task_enable(int on);
uint32_t blob_task_count(void);
uint32_t blob_task_stack_short(void);  /* requests larger than a nat-os stack */
uint32_t blob_task_want_stack(void);

#endif
