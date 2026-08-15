/* nat-os — the persistent record. See store.h. */

#include "store.h"
#include "flash.h"

#define STORE_MAGIC   0x59444F53u      /* "SODY" little-endian */
#define STORE_VERSION 2u   /* 2 adds the fault fields */

static store_t g_rec;
static int     g_valid;

/* Sum over every field but the checksum itself. Deliberately trivial: this
 * detects a torn or blank sector, which is what actually happens here. It is
 * not a defence against a determined corruption, and pretending otherwise would
 * be worse than the simple version. */
static uint32_t checksum(const store_t *r)
{
    return r->magic + r->version + r->boots + r->frames +
           r->fault_kind + r->fault_detail + r->fault_epc + r->fault_boot +
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
    return -1;
}

int store_save(void)
{
    g_rec.checksum = checksum(&g_rec);

    if (flash_erase_sector(FLASH_DATA_ADDR) != 0) {
        return -1;
    }
    return flash_write(FLASH_DATA_ADDR, &g_rec, sizeof g_rec);
}

uint32_t store_boots(void)  { return g_rec.boots; }
uint32_t store_frames(void) { return g_rec.frames; }
int      store_valid(void)  { return g_valid; }

void store_set_frames(uint32_t f) { g_rec.frames = f; }

uint32_t store_fault_kind(void)   { return g_rec.fault_kind; }
uint32_t store_fault_detail(void) { return g_rec.fault_detail; }
uint32_t store_fault_epc(void)    { return g_rec.fault_epc; }
uint32_t store_fault_boot(void)   { return g_rec.fault_boot; }

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
