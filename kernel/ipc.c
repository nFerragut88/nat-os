/* nat-os — inter-application messaging. See ipc.h for why messages are copied
 * rather than shared. */

#include "ipc.h"
#include "app.h"
#include "critical.h"

typedef struct {
    uint8_t  data[IPC_MSG_MAX];
    uint32_t len;                   /* 0 means empty */
    int      from;
} mailbox_t;

static mailbox_t g_box[APP_MAX];
static uint32_t  g_sent, g_delivered, g_refused;

void ipc_init(void)
{
    for (int i = 0; i < APP_MAX; i++) {
        g_box[i].len = 0;
        g_box[i].from = -1;
    }
    g_sent = g_delivered = g_refused = 0;
}

int ipc_send(int from, int dst, const uint8_t *data, uint32_t len)
{
    if (dst < 0 || dst >= APP_MAX || len == 0u || len > IPC_MSG_MAX) {
        g_refused++;
        return -1;
    }

    /* Delivering to a slot that is not running would leave a message for
     * whoever occupies it next, which is a channel between two applications
     * that never agreed to talk. */
    if (app_state(dst) != APP_RUNNING) {
        g_refused++;
        return -1;
    }

    /* The mailbox is shared between the sending and receiving applications,
     * which run in the same task but at different times, and with the reporter
     * reading the counters. Short enough for a critical section. */
    uint32_t crit = crit_enter();

    if (g_box[dst].len != 0u) {
        g_refused++;                /* occupied; sender must retry */
        crit_exit(crit);
        return -1;
    }

    for (uint32_t i = 0; i < len; i++) {
        g_box[dst].data[i] = data[i];
    }
    g_box[dst].len  = len;
    g_box[dst].from = from;
    g_sent++;

    crit_exit(crit);
    return 0;
}

uint32_t ipc_recv(int id, uint8_t *out, uint32_t max, int *from)
{
    if (id < 0 || id >= APP_MAX) {
        return 0;
    }

    uint32_t crit = crit_enter();

    uint32_t len = g_box[id].len;
    if (len == 0u) {
        crit_exit(crit);
        return 0;
    }

    uint32_t take = (len < max) ? len : max;
    for (uint32_t i = 0; i < take; i++) {
        out[i] = g_box[id].data[i];
    }
    if (from) {
        *from = g_box[id].from;
    }

    g_box[id].len  = 0;
    g_box[id].from = -1;
    g_delivered++;

    crit_exit(crit);
    return take;
}

void ipc_clear(int id)
{
    if (id < 0 || id >= APP_MAX) {
        return;
    }
    uint32_t crit = crit_enter();
    g_box[id].len  = 0;
    g_box[id].from = -1;
    crit_exit(crit);
}

uint32_t ipc_sent(void)      { return g_sent; }
uint32_t ipc_delivered(void) { return g_delivered; }
uint32_t ipc_refused(void)   { return g_refused; }
