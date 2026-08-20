/* nat-os — the persistent record. See store.h. */

#include "store.h"
#include "flash.h"

#define STORE_MAGIC   0x59444F53u      /* "SODY" little-endian */
#define STORE_VERSION 4u   /* 3 added touch calibration; 4 adds slots */

static store_t g_rec;
static int     g_dirty;    /* a slot changed and has not reached flash yet */
static int     g_valid;

/* Sum over every field but the checksum itself. Deliberately trivial: this
 * detects a torn or blank sector, which is what actually happens here. It is
 * not a defence against a determined corruption, and pretending otherwise would
 * be worse than the simple version. */
static uint32_t checksum(const store_t *r)
{
    return r->magic + r->version + r->boots + r->frames +
           r->fault_kind + r->fault_detail + r->fault_epc + r->fault_boot +
           r->cal_x_min + r->cal_x_max + r->cal_y_min + r->cal_y_max +
           0x9E3779B9u;
}

int store_load(void)
{
    store_t r;
    g_valid = 0;

    if (flash_read(FLASH_DATA_ADDR, &r, sizeof r) != 0) {
        goto defaults;
    }
    if (r.magic != STORE_MAGIC || r.version != STORE_VERSION) {
        goto defaults;      /* first run, or an erased sector reading 0xFF */
    }
    if (r.checksum != checksum(&r)) {
        goto defaults;      /* torn by a reset mid-write */
    }

    g_rec   = r;
    g_valid = 1;
    return 0;

defaults:
    g_rec.magic   = STORE_MAGIC;
    g_rec.version = STORE_VERSION;
    g_rec.boots        = 0;
    g_rec.frames       = 0;
    g_rec.fault_kind   = STORE_FAULT_NONE;
    g_rec.fault_detail = 0;
    g_rec.fault_epc    = 0;
    g_rec.fault_boot   = 0;
    g_rec.cal_x_min = g_rec.cal_x_max = 0;
    g_rec.cal_y_min = g_rec.cal_y_max = 0;
    for (uint32_t b = 0; b < STORE_SLOT_BANKS; b++) {
        for (uint32_t s = 0; s < STORE_SLOTS_PER_BANK; s++) {
            g_rec.slots[b][s] = 0;
        }
    }
    return -1;
}

int store_save(void)
{
    g_rec.checksum = checksum(&g_rec);

    if (flash_erase_sector(FLASH_DATA_ADDR) != 0) {
        return -1;
    }
    int rc = flash_write(FLASH_DATA_ADDR, &g_rec, sizeof g_rec);
    if (rc == 0) {
        g_dirty = 0;
    }
    return rc;
}

/* ---- deciding when to spend the 125 ms ----------------------------------
 *
 * See store.h. The decision belongs to whoever knows what the machine is busy
 * with, which is not this file.
 */
static store_may_save_fn g_may_save;
static uint32_t          g_defer_run;      /* consecutive refusals            */
static uint32_t          g_deferrals;      /* cumulative, for telemetry       */
static uint32_t          g_forced;         /* went ahead despite a refusal    */

void store_set_may_save(store_may_save_fn fn) { g_may_save = fn; }
uint32_t store_deferrals(void)               { return g_deferrals; }
uint32_t store_forced(void)                  { return g_forced; }

int store_save_if_allowed(void)
{
    if (!g_dirty) {
        return 0;               /* nothing to write; not a deferral */
    }
    if (g_may_save && !g_may_save()) {
        g_deferrals++;
        if (++g_defer_run < STORE_DEFER_MAX) {
            return 1;
        }
        /* Bound reached. Losing the record at the next power cut is worse than
         * whatever the predicate was protecting, so this goes ahead and says
         * so through store_forced(). */
        g_forced++;
    }
    g_defer_run = 0;
    return store_save();
}

/* ---- persistent slots ---------------------------------------------------
 *
 * Banked by caller. See store.h for why they are not shared, and why a write
 * does not go straight to flash. */

int store_slot_get(uint32_t bank, uint32_t slot, uint32_t *out)
{
    if (bank >= STORE_SLOT_BANKS || slot >= STORE_SLOTS_PER_BANK) {
        return 0;
    }
    *out = g_rec.slots[bank][slot];
    return 1;
}

int store_slot_set(uint32_t bank, uint32_t slot, uint32_t value)
{
    if (bank >= STORE_SLOT_BANKS || slot >= STORE_SLOTS_PER_BANK) {
        return 0;
    }
    /* Only mark dirty on an actual change. A program looping on a write of the
     * same value would otherwise force an erase a minute forever, which is how
     * a flash sector rated for a hundred thousand cycles gets spent by a
     * program that thought it was doing nothing. */
    if (g_rec.slots[bank][slot] != value) {
        g_rec.slots[bank][slot] = value;
        g_dirty = 1;
    }
    return 1;
}

int store_dirty(void) { return g_dirty; }

uint32_t store_boots(void)  { return g_rec.boots; }
uint32_t store_frames(void) { return g_rec.frames; }
int      store_valid(void)  { return g_valid; }

void store_set_frames(uint32_t f) { g_rec.frames = f; }

uint32_t store_fault_kind(void)   { return g_rec.fault_kind; }
uint32_t store_fault_detail(void) { return g_rec.fault_detail; }
uint32_t store_fault_epc(void)    { return g_rec.fault_epc; }
uint32_t store_fault_boot(void)   { return g_rec.fault_boot; }

int store_has_calibration(void) { return g_rec.cal_x_max > g_rec.cal_x_min; }

void store_get_calibration(uint32_t *xmin, uint32_t *xmax,
                           uint32_t *ymin, uint32_t *ymax)
{
    *xmin = g_rec.cal_x_min;
    *xmax = g_rec.cal_x_max;
    *ymin = g_rec.cal_y_min;
    *ymax = g_rec.cal_y_max;
}

/* Declared in calib.h. Writing the record here keeps every write to it in one
 * file, which is what makes "the record is only changed in store.c" a property
 * rather than a habit. */
void calib_persist(uint32_t xmin, uint32_t xmax, uint32_t ymin, uint32_t ymax)
{
    g_rec.cal_x_min = xmin;
    g_rec.cal_x_max = xmax;
    g_rec.cal_y_min = ymin;
    g_rec.cal_y_max = ymax;
    (void)store_save();
}

/* Runs inside a panic, so it assumes as little as possible about the state of
 * the system. It does not read the existing record first: g_rec already holds
 * what was loaded at boot, and re-reading flash here would add a failure mode
 * to a path that exists precisely because something has already gone wrong. */
int store_record_fault(uint32_t kind, uint32_t detail, uint32_t epc)
{
    g_rec.fault_kind   = kind;
    g_rec.fault_detail = detail;
    g_rec.fault_epc    = epc;
    g_rec.fault_boot   = g_rec.boots;
    return store_save();
}

/* Bumped by kmain once the record has been loaded. Kept here so the only code
 * that writes the counter is the code that owns it. */
void store_count_boot(void);
void store_count_boot(void) { g_rec.boots++; }
