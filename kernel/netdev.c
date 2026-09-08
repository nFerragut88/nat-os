/* nat-os — the network, as a device.
 *
 * WHY THIS EXISTS. By step 386 this board had two things it had never joined
 * together: a language a person can write applications in, and a network stack
 * that associates, completes a WPA2 handshake and fetches over HTTP. A
 * NatScript program could read a light sensor, draw on the panel and feel a
 * finger, and had no way at all to reach the network.
 *
 * WHY A DEVICE AND NOT A SYSCALL. device.h has said it since the table was
 * written: "Anything new is a device.h table entry reached through `sys
 * device`, not a thirteenth mnemonic here and a fourteenth case in vm.c." The
 * device model already carries permissions, per-caller grants, bounds-checked
 * transfers through a bounce buffer, and -- since step 358 -- resolution by
 * NAME rather than by index.
 *
 * It also means NatScript needs NO compiler change. `permissions { net }`
 * makes the name available, and net.read / net.xfer_out / net.xfer_in are the
 * generic device methods the language already emits. The whole feature is a
 * table entry and this file.
 *
 * WHY THE HANDOFF. webfetch.h is explicit: its functions MUST be called from
 * the net task, because the raw lwIP API is not thread safe and NO_SYS=1 means
 * there is no lock to make it so. A device callback runs on the CALLER's task,
 * which for a VM program is the app host. So a request is recorded here and
 * picked up by netdev_service() from the net task's own loop -- the same shape
 * job.h uses, for the same reason.
 *
 * THE PROTOCOL, from a program's point of view:
 *
 *   net.xfer_out(0, request, len)   "host/path" -- starts a fetch
 *   net.read(0)   -> state          0 idle, 1 resolving, 2 connecting,
 *                                   3 requesting, 4 done, 5 failed
 *   net.read(1)   -> HTTP status code, or 0
 *   net.read(2)   -> body length in bytes
 *   net.xfer_in(block, buf, len)    body bytes at block * DEVICE_XFER_MAX
 *
 * A fetch is one at a time. A second request while one is in flight is
 * REFUSED rather than queued: a program that cannot tell which reply it is
 * reading is worse off than one told to wait.
 */

#include "netdev.h"
#include "device.h"
#include "webfetch.h"
#include "uart.h"
#include <stdint.h>

#define REQ_MAX  (WEB_HOST_MAX + 64u)

static char     g_req[REQ_MAX];
static uint32_t g_req_len;
static volatile uint32_t g_want;    /* a request is waiting for the net task */
static volatile uint32_t g_busy;    /* a fetch is in flight or finished       */
static uint32_t g_said;             /* the outcome has been reported once     */

/* ---- the device callbacks, on the CALLER's task ------------------------- */

int netdev_read(uint32_t caller, uint32_t chan, uint32_t *out)
{
    (void)caller;
    switch (chan) {
    case 0u: *out = (uint32_t)webfetch_state(); return 1;
    case 1u: *out = webfetch_code();            return 1;
    case 2u: *out = webfetch_len();             return 1;
    default: return 0;
    }
}

int netdev_xfer_out(uint32_t caller, uint32_t chan, const uint8_t *buf,
                        uint32_t len)
{
    (void)caller;
    if (chan != 0u || len == 0u || len >= REQ_MAX) { return 0; }

    /* One at a time. Refused, not queued -- see the header. A fetch that has
     * FINISHED is not in flight, so a program may start the next one as soon
     * as it has read the answer it asked for. */
    int st = webfetch_state();
    if (g_want || (g_busy && st != WEB_DONE && st != WEB_FAILED && st != WEB_IDLE)) {
        return 0;
    }

    for (uint32_t i = 0; i < len; i++) { g_req[i] = (char)buf[i]; }
    g_req[len] = 0;
    g_req_len  = len;
    g_want     = 1u;
    return 1;
}

int netdev_xfer_in(uint32_t caller, uint32_t chan, uint8_t *buf,
                       uint32_t len)
{
    (void)caller;
    if (len == 0u || len > DEVICE_XFER_MAX) { return 0; }

    /* `chan` is a BLOCK index, not a byte offset: a channel is a small number
     * in this table and a body is up to WEB_BODY_MAX bytes, so counting in
     * transfers is the only addressing that fits. */
    uint32_t off  = chan * DEVICE_XFER_MAX;
    uint32_t have = webfetch_len();
    if (off >= have) { return 0; }

    const char *body = webfetch_body();
    if (!body) { return 0; }

    uint32_t n = have - off;
    if (n > len) { n = len; }
    for (uint32_t i = 0; i < n; i++) { buf[i] = (uint8_t)body[off + i]; }
    /* Short reads are zero-filled rather than left holding the bounce
     * buffer's previous tenant -- a program reading the last block would
     * otherwise see somebody else's bytes past the end of the body. */
    for (uint32_t i = n; i < len; i++) { buf[i] = 0u; }
    return 1;
}

/* ---- the net task's half ------------------------------------------------ */

void netdev_service(void)
{
    if (g_want) {
        g_want = 0u;

        /* "host/path" split at the first slash. No slash means the root,
         * which is what a person typing a hostname means. */
        uint32_t i = 0u;
        while (i < g_req_len && g_req[i] != '/') { i++; }
        static char host[WEB_HOST_MAX];
        uint32_t h = 0u;
        for (; h < i && h < WEB_HOST_MAX - 1u; h++) { host[h] = g_req[h]; }
        host[h] = 0;

        const char *path = (i < g_req_len) ? &g_req[i] : "/";
        g_busy = 1u;
        g_said = 0u;
        /* [step 387] Say what was picked up and what came back.
         *
         * The first two attempts sat in state 0 with nothing to read: the
         * request had been accepted, the service was silent, and there was no
         * way to tell "never called" from "called and refused". That is the
         * sixth instrument in this log to know something it did not say. */
        int rc = webfetch_start(host, path);
        uart_puts("   netdev    fetch ");
        uart_puts(host);
        uart_puts(path);
        uart_puts(rc == 0 ? "  started\n" : "  REFUSED by webfetch\n");
    }
    if (g_busy) {
        webfetch_service();

        /* [step 387] Say how it ended, once.
         *
         * webfetch_status() has always returned "a short line naming what
         * happened" and nothing outside the browser view ever read it. A
         * program polling through this device sees only the number 5 --
         * WEB_FAILED -- which says a fetch failed and not one word about why.
         * That is the seventh instrument in this log to know something it did
         * not say, and the cheapest one to fix. */
        int st = webfetch_state();
        if ((st == WEB_DONE || st == WEB_FAILED) && !g_said) {
            g_said = 1u;
            uart_puts("   netdev    ");
            uart_puts(webfetch_status());
            uart_puts("\n");
        }
    }
}

