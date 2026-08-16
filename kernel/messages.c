/* nat-os — saved messages in flash. See messages.h. */

#include "messages.h"
#include "flash.h"

#define MSG_ADDR   (FLASH_DATA_ADDR + FLASH_SECTOR)   /* the sector after the record */
#define MSG_MAGIC  0x4753454Du                        /* "MSEG" little-endian */
#define MSG_VERSION 1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    char     text[MSG_MAX][MSG_LEN + 1u];
    uint32_t checksum;
} msg_store_t;

static msg_store_t g_store;

/* Sum over the header and every stored byte. Trivial on purpose, like the boot
 * record's: it detects a torn or blank sector, which is what actually happens,
 * and pretending to more than that would be worse than the simple version. */
static uint32_t checksum(const msg_store_t *st)
{
    uint32_t sum = st->magic + st->version + st->count + 0x5BD1E995u;
    for (uint32_t m = 0; m < MSG_MAX; m++) {
        for (uint32_t i = 0; i <= MSG_LEN; i++) {
            sum += (uint32_t)(uint8_t)st->text[m][i] * (i + 1u);
        }
    }
    return sum;
}

static void reset(void)
{
    g_store.magic   = MSG_MAGIC;
    g_store.version = MSG_VERSION;
    g_store.count   = 0;
    for (uint32_t m = 0; m < MSG_MAX; m++) {
        for (uint32_t i = 0; i <= MSG_LEN; i++) {
            g_store.text[m][i] = 0;
        }
    }
}

int msg_load(void)
{
#if FLASH_ENABLE
    msg_store_t st;
    if (flash_read(MSG_ADDR, &st, sizeof st) != 0) {
        reset();
        return -1;
    }
    if (st.magic != MSG_MAGIC || st.version != MSG_VERSION ||
        st.count > MSG_MAX || st.checksum != checksum(&st)) {
        reset();
        return -1;
    }
    g_store = st;
    return 0;
#else
    reset();
    return -1;
#endif
}

static int commit(void)
{
#if FLASH_ENABLE
    g_store.checksum = checksum(&g_store);
    if (flash_erase_sector(MSG_ADDR) != 0) {
        return -1;
    }
    return flash_write(MSG_ADDR, &g_store, sizeof g_store);
#else
    return -1;
#endif
}

int msg_save(const char *text)
{
    if (!text || !text[0]) {
        return -1;                  /* nothing to save is not a save */
    }

    if (g_store.count >= MSG_MAX) {
        /* Drop the oldest by sliding the rest down. A ring buffer would avoid
         * the copy, but this store is read far more often than written and the
         * copy is 1.3 KB against a flash erase measured in tens of
         * milliseconds — the memmove is not what costs. */
        for (uint32_t m = 0; m + 1u < MSG_MAX; m++) {
            for (uint32_t i = 0; i <= MSG_LEN; i++) {
                g_store.text[m][i] = g_store.text[m + 1u][i];
            }
        }
        g_store.count = MSG_MAX - 1u;
    }

    uint32_t slot = g_store.count;
    uint32_t i = 0;
    for (; i < MSG_LEN && text[i]; i++) {
        g_store.text[slot][i] = text[i];
    }
    g_store.text[slot][i] = 0;
    g_store.count++;

    if (commit() != 0) {
        g_store.count--;            /* not written: do not claim it was */
        return -1;
    }
    return 0;
}

int msg_clear(void)
{
    reset();
    return commit();
}

uint32_t msg_count(void) { return g_store.count; }

const char *msg_get(uint32_t i)
{
    return (i < g_store.count) ? g_store.text[i] : 0;
}
