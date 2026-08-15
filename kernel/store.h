/* nat-os — the persistent record.
 *
 * One sector holding kernel state that should outlive a power cycle. Written
 * whole and validated whole: a checksum over the payload, so a record torn by a
 * reset mid-write is rejected rather than half-believed.
 *
 * A boot counter is the first field on purpose. Persistence is easy to think
 * you have — the value read back is the value just written, whether or not it
 * ever reached the chip. A counter that survives a POWER CYCLE cannot be
 * produced by a variable that stayed in RAM.
 */

#ifndef NATOS_STORE_H
#define NATOS_STORE_H

#include <stdint.h>

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t boots;         /* incremented every time the kernel starts */
    uint32_t frames;        /* cumulative raycaster frames, across boots */

    /* Why the kernel died last time, if it did.
     *
     * A panic prints to the UART and halts, which is only useful to somebody
     * with a serial cable attached at the moment it happens. This device is
     * standalone and normally has nothing attached at all, so a fault has been
     * effectively invisible: the panel freezes and there is no way to learn
     * what stopped it. Recording the reason before halting means the NEXT boot
     * can say what happened, whether or not anyone was watching. */
    uint32_t fault_kind;    /* store_fault_t; STORE_FAULT_NONE if clean */
    uint32_t fault_detail;  /* exccause, or the task id for a guard break */
    uint32_t fault_epc;     /* faulting instruction, 0 when not applicable */
    uint32_t fault_boot;    /* which boot it happened on */

    uint32_t checksum;
} store_t;

typedef enum {
    STORE_FAULT_NONE      = 0,
    STORE_FAULT_EXCEPTION = 1,   /* hardware exception; detail = exccause */
    STORE_FAULT_GUARD     = 2    /* stack guard broken; detail = task id  */
} store_fault_t;

/* Called from the panic path, immediately before halting. Writes the record
 * synchronously — there is no later. */
int  store_record_fault(uint32_t kind, uint32_t detail, uint32_t epc);

uint32_t store_fault_kind(void);
uint32_t store_fault_detail(void);
uint32_t store_fault_epc(void);
uint32_t store_fault_boot(void);

/* Reads and validates the record. Returns 0 if a valid one was found; on
 * failure the record is reset to defaults so a first run and a corrupt sector
 * behave identically. */
int  store_load(void);

/* Erases the sector and writes the record back. Returns 0 on success. */
int  store_save(void);

uint32_t store_boots(void);
uint32_t store_frames(void);
void     store_set_frames(uint32_t f);
int      store_valid(void);

#endif /* NATOS_STORE_H */
