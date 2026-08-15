/* cyd-os — the persistent record.
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

#ifndef CYDOS_STORE_H
#define CYDOS_STORE_H

#include <stdint.h>

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t boots;         /* incremented every time the kernel starts */
    uint32_t frames;        /* cumulative raycaster frames, across boots */
    uint32_t checksum;
} store_t;

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

#endif /* CYDOS_STORE_H */
