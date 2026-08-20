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
#include "app.h"                /* APP_MAX: one slot bank per application */

/* One bank per application, plus one for the kernel and the shell. */
#define STORE_SLOT_BANKS      (APP_MAX + 1)
#define STORE_SLOTS_PER_BANK  4u
#define STORE_KERNEL_BANK     ((uint32_t)APP_MAX)

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

    /* Touch calibration, so a calibration survives a reboot. Zero means never
     * calibrated, and the driver's compiled-in defaults stand. */
    uint32_t cal_x_min, cal_x_max, cal_y_min, cal_y_max;

    /* Persistent slots, reached by applications through the `store` device.
     *
     * Banked BY CALLER, not shared. Everything else an application owns in this
     * kernel is confined to it -- its arena, its viewport, its mailbox -- and
     * persistence with a single global pool would be the one place a program
     * could read what another program wrote. Bank APP_MAX belongs to the kernel
     * and the shell, so a diagnostic at the prompt cannot land on top of an
     * application's saved state either.
     *
     * Deliberately tiny. This is somewhere to keep a high score, a resume point
     * or a setting; a program that needs a filesystem needs the SD card. */
    uint32_t slots[STORE_SLOT_BANKS][STORE_SLOTS_PER_BANK];

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

/* Non-zero if the record carries a calibration. */
int  store_has_calibration(void);
void store_get_calibration(uint32_t *xmin, uint32_t *xmax,
                           uint32_t *ymin, uint32_t *ymax);

/* Reads and validates the record. Returns 0 if a valid one was found; on
 * failure the record is reset to defaults so a first run and a corrupt sector
 * behave identically. */
int  store_load(void);

/* Erases the sector and writes the record back. Returns 0 on success.
 *
 * UNCONDITIONAL, and costs 125 ms with interrupts masked -- measured, see
 * next_moves/04. Correct for boot, shutdown, and anything a human asked for.
 * The periodic path should use store_save_if_allowed() instead. */
int  store_save(void);

/* ---- deciding WHEN to spend 125 ms --------------------------------------
 *
 * next_moves/04 measured store_save() at 125 ms with interrupts masked, and
 * next_moves/10 records what that costs a relay node: a LoRa receive window is
 * shorter than the erase, so a node that writes a bundle down as it arrives
 * does not receive the next packet. Not late -- at all.
 *
 * The two other ways out are closed. A scheduler change cannot preempt a masked
 * interrupt, and erase-suspend needs a controller the original ESP32 does not
 * have (kernel/flash.c has the evidence). What is left is not erasing at a bad
 * moment, which is a decision this layer cannot make and the radio can.
 *
 * So the decision moves up. Whoever knows whether now is a bad time registers a
 * predicate; the periodic save asks before spending the time. Nothing in
 * flash.c changes.
 */
typedef int (*store_may_save_fn)(void);

/* Register the predicate, or NULL to always allow. Non-zero return means "now
 * is fine". */
void store_set_may_save(store_may_save_fn fn);

/* The periodic save. Asks the predicate first.
 *
 *   0  written
 *   1  deferred, still dirty, will be retried
 *  -1  error from the write itself
 *
 * ---- and it will not defer forever ---------------------------------------
 *
 * A predicate that never says yes -- a busy radio, or a bug -- would mean the
 * record is never written and everything since the last save is lost at the
 * next power cut. That is a worse outcome than a dropped packet, and the whole
 * point of this store is surviving power loss.
 *
 * So a deferral is bounded. After STORE_DEFER_MAX consecutive refusals the save
 * happens anyway, and store_forced() counts how often that has been necessary.
 * A rising forced count means the predicate is wrong or the system never idles,
 * and either way it is something to look at rather than something to trust. */
#define STORE_DEFER_MAX 32u
int  store_save_if_allowed(void);

/* Telemetry for the above. deferrals is cumulative; forced is the subset that
 * hit STORE_DEFER_MAX and went ahead regardless. */
uint32_t store_deferrals(void);
uint32_t store_forced(void);

/* Persistent slots. `bank` is the calling application's id, or
 * STORE_KERNEL_BANK for the kernel and the shell. Both return 0 on a bad bank
 * or slot rather than clamping, because a silently redirected write is worse
 * than a refused one.
 *
 * A write lands in the in-RAM record and marks it dirty; it reaches flash on
 * the next store_save(). That is deliberate -- a flash erase per write would
 * cost tens of milliseconds with interrupts masked and would burn a sector
 * rated for a hundred thousand cycles in an afternoon. store_dirty() reports
 * whether anything is waiting. */
int  store_slot_get(uint32_t bank, uint32_t slot, uint32_t *out);
int  store_slot_set(uint32_t bank, uint32_t slot, uint32_t value);
int  store_dirty(void);

uint32_t store_boots(void);
uint32_t store_frames(void);
void     store_set_frames(uint32_t f);
int      store_valid(void);

#endif /* NATOS_STORE_H */
