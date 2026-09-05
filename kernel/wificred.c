/* nat-os — saved WiFi credentials. See wificred.h for why this is not in
 * store.c and for what it does not promise about secrecy. */

#include "wificred.h"
#include "flash.h"
#include "uart.h"

/* [step 287] THIS FILE MUST STAY IN IRAM.
 *
 * It drives flash_read/erase/write, and flash.c says what that costs:
 *
 *     "SPI1 shares the flash bus with the cache's SPI0, and these registers
 *      are how the cache issues its own reads. Overwriting them and walking
 *      away leaves the cache unable to read flash AT ALL -- which presented as
 *      every string literal in the kernel returning 0xFF immediately after the
 *      first flash operation."
 *
 * It was placed in irom with wifiapp.c and keyboard.c to save iram, which put
 * flash-resident code in charge of the flash bus. The first tap that reached
 * wificred_get() whited the screen. store.c -- the only other module that
 * touches flash -- has always been in iram, and this broke that precedent
 * without noticing there was one.
 *
 * keyboard.c stays in irom: it draws and it counts ticks, and it never goes
 * near the bus. */
#define CRED_ADDR    0x202000u          /* clear of the record (0x200000) and
                                         * the message sector (0x201000), and
                                         * 120 KB below the blob (0x220000) */
#define CRED_MAGIC   0x7774616Eu        /* "natw" little-endian */
#define CRED_VERSION 1u

typedef struct {
    char ssid[36];                      /* 33 rounded up to a word boundary */
    char pass[68];
} cred_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    cred_t   e[WIFICRED_SLOTS];
    uint32_t checksum;
} rec_t;

_Static_assert(sizeof(rec_t) <= FLASH_SECTOR, "the credential record must fit one sector");

static rec_t g_rec;
static int   g_loaded;

/* The same shape of checksum store.c uses: enough to reject a torn write or an
 * erased sector, and not pretending to be more than that. */
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
    r->magic   = CRED_MAGIC;
    r->version = CRED_VERSION;
}

static void load(void)
{
    if (g_loaded) { return; }
    g_loaded = 1;

    if (flash_read(CRED_ADDR, &g_rec, sizeof g_rec) != 0) { zero(&g_rec); return; }

    /* An erased sector reads as all-ones and fails the magic; a record written
     * by an older version fails too. Both mean "no credentials", which is a
     * correct answer and not an error. */
    if (g_rec.magic != CRED_MAGIC || g_rec.version != CRED_VERSION ||
        g_rec.count > WIFICRED_SLOTS || g_rec.checksum != sum_of(&g_rec)) {
        zero(&g_rec);
    }
}

static int same(const char *a, const char *b)
{
    uint32_t i = 0u;
    while (i < 32u && a[i] && a[i] == b[i]) { i++; }
    return a[i] == b[i];
}

void wificred_prime(void) { load(); }

int wificred_get(const char *ssid, char *pass, uint32_t max)
{
    if (!ssid || !pass || !max) { return 0; }
    load();
    for (uint32_t i = 0u; i < g_rec.count; i++) {
        if (!same(g_rec.e[i].ssid, ssid)) { continue; }
        uint32_t k = 0u;
        for (; k + 1u < max && k < WIFICRED_PASS_MAX - 1u && g_rec.e[i].pass[k]; k++) {
            pass[k] = g_rec.e[i].pass[k];
        }
        pass[k] = 0;
        return 1;
    }
    return 0;
}

int wificred_has(const char *ssid)
{
    if (!ssid) { return 0; }
    load();
    for (uint32_t i = 0u; i < g_rec.count; i++) {
        if (same(g_rec.e[i].ssid, ssid)) { return 1; }
    }
    return 0;
}

uint32_t wificred_count(void)
{
    load();
    return g_rec.count;
}

int wificred_put(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0] || !pass) { return -1; }
    load();

    /* Replace in place if this network is already known, so retyping a changed
     * password does not consume a slot and does not leave the old one behind to
     * be found first. */
    uint32_t at = g_rec.count;
    for (uint32_t i = 0u; i < g_rec.count; i++) {
        if (same(g_rec.e[i].ssid, ssid)) { at = i; break; }
    }
    if (at >= WIFICRED_SLOTS) {
        /* Full. Drop the oldest rather than refusing: a user who is typing a
         * password wants it to work now, and the alternative is an error they
         * cannot act on without a way to browse and delete slots. */
        for (uint32_t i = 1u; i < WIFICRED_SLOTS; i++) { g_rec.e[i - 1u] = g_rec.e[i]; }
        at = WIFICRED_SLOTS - 1u;
        g_rec.count = WIFICRED_SLOTS;
    } else if (at == g_rec.count) {
        g_rec.count++;
    }

    uint32_t k = 0u;
    for (; k < WIFICRED_SSID_MAX - 1u && ssid[k]; k++) { g_rec.e[at].ssid[k] = ssid[k]; }
    g_rec.e[at].ssid[k] = 0;
    for (k = 0u; k < WIFICRED_PASS_MAX - 1u && pass[k]; k++) { g_rec.e[at].pass[k] = pass[k]; }
    g_rec.e[at].pass[k] = 0;

    g_rec.magic    = CRED_MAGIC;
    g_rec.version  = CRED_VERSION;
    g_rec.checksum = sum_of(&g_rec);

    if (flash_erase_sector(CRED_ADDR) != 0) { return -1; }
    if (flash_write(CRED_ADDR, &g_rec, sizeof g_rec) != 0) { return -1; }

    uart_puts("   wificred  saved ");
    uart_puts(ssid);
    uart_puts("\n");
    return 0;
}
