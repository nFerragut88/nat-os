/* nat-os — cached pairwise master keys. See pmkcache.h.
 *
 * THIS FILE MUST STAY IN IRAM. It drives flash_read/erase/write, and flash.c is
 * explicit about what that costs: a flash operation leaves the cache unable to
 * read flash at all. Step 292 paid for that rule with a white screen; the
 * linker script keeps wificred.c out of irom for the same reason and this file
 * belongs on the same side of it.
 */

#include "pmkcache.h"
#include "flash.h"
#include "uart.h"

#define PMK_ADDR    0x203000u       /* clear of the record (0x200000), the
                                     * message sector (0x201000) and the
                                     * credentials (0x202000); 116 KB below
                                     * the blob at 0x220000 */
#define PMK_MAGIC   0x6B6D7061u     /* "apmk" little-endian */
#define PMK_VERSION 1u

typedef struct {
    char          ssid[36];
    unsigned char pmk[PMK_LEN];
} entry_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    entry_t  e[PMK_SLOTS];
    uint32_t checksum;
} rec_t;

_Static_assert(sizeof(rec_t) <= FLASH_SECTOR, "the pmk record must fit one sector");

static rec_t g_rec;
static int   g_loaded;

static uint32_t sum_of(const rec_t *r)
{
    const uint32_t *w = (const uint32_t *)r;
    uint32_t n = (sizeof *r - sizeof r->checksum) / 4u;
    uint32_t s = 0u;
    for (uint32_t i = 0u; i < n; i++) { s += w[i] ^ (i * 2654435761u); }
    return s;
}

static void zero(rec_t *r)
{
    unsigned char *p = (unsigned char *)r;
    for (uint32_t i = 0u; i < sizeof *r; i++) { p[i] = 0u; }
    r->magic   = PMK_MAGIC;
    r->version = PMK_VERSION;
}

static void load(void)
{
    if (g_loaded) { return; }
    g_loaded = 1;

    if (flash_read(PMK_ADDR, &g_rec, sizeof g_rec) != 0) { zero(&g_rec); return; }

    /* Every failure here means "derive it again", which costs fifteen seconds
     * and nothing else. That is why this record can afford to be strict. */
    if (g_rec.magic != PMK_MAGIC || g_rec.version != PMK_VERSION ||
        g_rec.count > PMK_SLOTS || g_rec.checksum != sum_of(&g_rec)) {
        zero(&g_rec);
    }
}

void pmkcache_prime(void) { load(); }

static int same(const char *a, const char *b)
{
    uint32_t i = 0u;
    while (i < 32u && a[i] && a[i] == b[i]) { i++; }
    return a[i] == b[i];
}

int pmkcache_get(const char *ssid, unsigned char *out)
{
    if (!ssid || !out) { return 0; }
    load();
    for (uint32_t i = 0u; i < g_rec.count; i++) {
        if (!same(g_rec.e[i].ssid, ssid)) { continue; }
        for (uint32_t k = 0u; k < PMK_LEN; k++) { out[k] = g_rec.e[i].pmk[k]; }
        return 1;
    }
    return 0;
}

static int write_back(void)
{
    g_rec.magic    = PMK_MAGIC;
    g_rec.version  = PMK_VERSION;
    g_rec.checksum = sum_of(&g_rec);
    if (flash_erase_sector(PMK_ADDR) != 0) { return -1; }
    if (flash_write(PMK_ADDR, &g_rec, sizeof g_rec) != 0) { return -1; }
    return 0;
}

int pmkcache_put(const char *ssid, const unsigned char *pmk)
{
    if (!ssid || !ssid[0] || !pmk) { return -1; }
    load();

    uint32_t at = g_rec.count;
    for (uint32_t i = 0u; i < g_rec.count; i++) {
        if (same(g_rec.e[i].ssid, ssid)) { at = i; break; }
    }
    if (at >= PMK_SLOTS) {
        for (uint32_t i = 1u; i < PMK_SLOTS; i++) { g_rec.e[i - 1u] = g_rec.e[i]; }
        at = PMK_SLOTS - 1u;
        g_rec.count = PMK_SLOTS;
    } else if (at == g_rec.count) {
        g_rec.count++;
    }

    uint32_t k = 0u;
    for (; k < 32u && ssid[k]; k++) { g_rec.e[at].ssid[k] = ssid[k]; }
    g_rec.e[at].ssid[k] = 0;
    for (k = 0u; k < PMK_LEN; k++) { g_rec.e[at].pmk[k] = pmk[k]; }

    if (write_back() != 0) { return -1; }
    uart_puts("   pmkcache  stored for ");
    uart_puts(ssid);
    uart_puts("\n");
    return 0;
}

void pmkcache_forget(const char *ssid)
{
    if (!ssid) { return; }
    load();
    for (uint32_t i = 0u; i < g_rec.count; i++) {
        if (!same(g_rec.e[i].ssid, ssid)) { continue; }
        for (uint32_t j = i + 1u; j < g_rec.count; j++) { g_rec.e[j - 1u] = g_rec.e[j]; }
        g_rec.count--;
        /* Wipe the vacated slot: a PMK is as good as the passphrase, and
         * "forget" has to mean gone. */
        unsigned char *p = (unsigned char *)&g_rec.e[g_rec.count];
        for (uint32_t b = 0u; b < sizeof g_rec.e[0]; b++) { p[b] = 0u; }
        (void)write_back();
        return;
    }
}
