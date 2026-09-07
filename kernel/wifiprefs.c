/* nat-os — the radio's remembered settings. See wifiprefs.h.
 *
 * THIS FILE MUST STAY IN IRAM: it drives flash_read/erase/write, and step 316
 * narrowed that rule to the one claim that survives -- the code executing while
 * the flash bus is taken must not be fetching through the cache that bus feeds.
 */

#include "wifiprefs.h"
#include "flash.h"
#include "uart.h"

#define PREF_ADDR    0x204000u      /* record 0x200000, message 0x201000,
                                     * credentials 0x202000, pmk 0x203000;
                                     * the blob starts at 0x220000 */
#define PREF_MAGIC   0x66727077u    /* "wprf" little-endian */
#define PREF_VERSION 2u   /* [step 347] adds `chosen` */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t enabled;
    uint32_t chosen;        /* [step 347] the user has expressed a preference */
    char     ssid[36];
    uint32_t checksum;
} rec_t;

_Static_assert(sizeof(rec_t) <= FLASH_SECTOR, "the preference record must fit one sector");

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

static void defaults(rec_t *r)
{
    unsigned char *p = (unsigned char *)r;
    for (uint32_t i = 0u; i < sizeof *r; i++) { p[i] = 0u; }
    r->magic   = PREF_MAGIC;
    r->version = PREF_VERSION;
    /* Enabled, on a board that has never been told. A wifi view whose radio is
     * off by default is a wifi view that looks broken. */
    r->enabled = 1u;
}

static void load(void)
{
    if (g_loaded) { return; }
    g_loaded = 1;

    if (flash_read(PREF_ADDR, &g_rec, sizeof g_rec) != 0) { defaults(&g_rec); return; }

    if (g_rec.magic != PREF_MAGIC || g_rec.version != PREF_VERSION ||
        g_rec.checksum != sum_of(&g_rec)) {
        defaults(&g_rec);
    }
}

static void save(void);      /* defined below; the setters above call it */

void wifiprefs_prime(void) { load(); }

int wifiprefs_enabled(void)
{
    load();
    return g_rec.enabled ? 1 : 0;
}

int wifiprefs_chosen(void)
{
    load();
    return g_rec.chosen ? 1 : 0;
}

void wifiprefs_set_chosen(void)
{
    load();
    if (g_rec.chosen) { return; }        /* no erase for a no-op */
    g_rec.chosen = 1u;
    save();
    uart_puts("   wifiprefs user choice recorded\n");
}

const char *wifiprefs_network(void)
{
    load();
    return g_rec.ssid;
}

static void save(void)
{
    g_rec.magic    = PREF_MAGIC;
    g_rec.version  = PREF_VERSION;
    g_rec.checksum = sum_of(&g_rec);
    if (flash_erase_sector(PREF_ADDR) != 0) { return; }
    (void)flash_write(PREF_ADDR, &g_rec, sizeof g_rec);
}

void wifiprefs_set_enabled(int on)
{
    load();
    uint32_t want = on ? 1u : 0u;
    if (g_rec.enabled == want) { return; }   /* no erase for a no-op */
    g_rec.enabled = want;
    save();
    uart_puts(on ? "   wifiprefs enabled\n" : "   wifiprefs disabled\n");
}

void wifiprefs_set_network(const char *ssid)
{
    if (!ssid) { return; }
    load();

    /* Only write when it actually changes. A join that reconnects to the
     * network already preferred must not erase a flash sector every time. */
    uint32_t i = 0u;
    while (i < 32u && g_rec.ssid[i] && g_rec.ssid[i] == ssid[i]) { i++; }
    if (g_rec.ssid[i] == ssid[i]) { return; }

    for (i = 0u; i < 32u && ssid[i]; i++) { g_rec.ssid[i] = ssid[i]; }
    g_rec.ssid[i] = 0;
    g_rec.chosen  = 1u;     /* [step 347] setting a network is choosing */
    save();
    uart_puts("   wifiprefs network ");
    uart_puts(g_rec.ssid);
    uart_puts("\n");
}
