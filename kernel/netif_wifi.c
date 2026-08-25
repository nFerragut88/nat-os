/* nat-os -- the lwIP network interface, on top of the Wi-Fi driver.
 * next_moves/08 step 233.
 *
 * This is the whole port layer. With NO_SYS=1 there is no sys_arch.c, no
 * threading abstraction and no sockets; what lwIP needs from nat-os is:
 *
 *   sys_now()            a millisecond clock          -- kernel/kstring.c
 *   linkoutput()         send one Ethernet frame      -- here
 *   netif_input()        deliver one received frame   -- here
 *   sys_check_timeouts() called periodically          -- the poll loop
 *
 * The receive side reuses the ring that kernel/net.c already fills from the
 * driver's WINDOWED callback. That ring exists because the callback runs on
 * the driver's own task and must return immediately; lwIP is emphatically not
 * safe to call from there, and NO_SYS=1 means it is not safe to call from two
 * contexts at all. One producer, one consumer, one place lwIP is entered.
 *
 * kernel/net.c is NOT deleted. It answers ARP and ICMP with hand-written code
 * and a hand-rolled DHCP client, and it is what proved the data path works at
 * all. Running both at once would have two stacks answering the same ARP, so
 * the poll loop hands each frame to ONE of them -- see net_use_lwip().
 */

#include "uart.h"
#include "task.h"
#include "timer.h"
#include "blobcall.h"
#include <stdint.h>

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

static struct netif g_netif;
static uint32_t     g_tx_fn;
static uint32_t     g_up;

uint32_t g_lwip_tx, g_lwip_tx_err, g_lwip_rx, g_lwip_rx_drop;

/* ---- transmit ----------------------------------------------------------
 *
 * lwIP hands over a pbuf chain. esp_wifi_internal_tx wants one flat buffer, so
 * the chain is flattened into a static frame. Static rather than stack: a
 * 1514-byte local would be most of a nat-os task stack, and this runs on the
 * poll task where the depth is already committed elsewhere. */
static uint8_t g_txbuf[1600];

static err_t nat_linkoutput(struct netif *nif, struct pbuf *p)
{
    (void)nif;
    if (!g_tx_fn) { return ERR_IF; }
    if (p->tot_len > sizeof g_txbuf) {
        g_lwip_tx_err++;
        return ERR_BUF;                 /* refuse rather than truncate */
    }
    uint16_t n = pbuf_copy_partial(p, g_txbuf, p->tot_len, 0);
    if (n != p->tot_len) {
        g_lwip_tx_err++;
        return ERR_BUF;
    }
    uint32_t rc = blob_call(g_tx_fn, 0u /* WIFI_IF_STA */, (uint32_t)g_txbuf,
                            (uint32_t)n, 0u);
    if (rc != 0u) {
        /* 0x3006 is ESP_ERR_WIFI_STATE: not associated. Counted, not fatal --
         * the association can come back and lwIP will retry. */
        g_lwip_tx_err++;
        return ERR_IF;
    }
    g_lwip_tx++;
    return ERR_OK;
}

/* ---- the interface ------------------------------------------------------ */

static err_t nat_netif_init(struct netif *nif)
{
    nif->name[0] = 'w';
    nif->name[1] = 'l';
    nif->output     = etharp_output;      /* IP -> ARP -> linkoutput */
    nif->linkoutput = nat_linkoutput;
    nif->mtu        = 1500;
    nif->hwaddr_len = 6;
    nif->flags      = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP
                    | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

/* ---- receive ------------------------------------------------------------
 *
 * Called from the poll loop with one frame from the ring. The pbuf is
 * allocated from the POOL and freed by lwIP once it is done -- except when
 * netif->input returns an error, which means lwIP did not take ownership and
 * this must free it. Getting that wrong leaks the pool and reception stops
 * dead a dozen frames later, which would look like the radio going quiet. */
void netif_wifi_input(const uint8_t *frame, uint32_t len);
void netif_wifi_input(const uint8_t *frame, uint32_t len)
{
    if (!g_up || !len) { return; }

    struct pbuf *p = pbuf_alloc(PBUF_RAW, (uint16_t)len, PBUF_POOL);
    if (!p) {
        g_lwip_rx_drop++;              /* pool exhausted: counted, never silent */
        return;
    }
    if (pbuf_take(p, frame, (uint16_t)len) != ERR_OK) {
        pbuf_free(p);
        g_lwip_rx_drop++;
        return;
    }
    g_lwip_rx++;
    if (g_netif.input(p, &g_netif) != ERR_OK) {
        pbuf_free(p);
        g_lwip_rx_drop++;
    }
}

/* ---- bring-up ----------------------------------------------------------- */

void netif_wifi_start(uint32_t tx_fn, const uint8_t *mac);
void netif_wifi_start(uint32_t tx_fn, const uint8_t *mac)
{
    ip4_addr_t any;
    IP4_ADDR(&any, 0, 0, 0, 0);

    g_tx_fn = tx_fn;
    lwip_init();

    if (!netif_add(&g_netif, &any, &any, &any, NULL,
                   nat_netif_init, ethernet_input)) {
        uart_puts("   lwip      netif_add FAILED\n");
        return;
    }
    for (uint32_t i = 0u; i < 6u; i++) { g_netif.hwaddr[i] = mac[i]; }
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);
    netif_set_link_up(&g_netif);
    g_up = 1u;

    uart_puts("   lwip      netif up, mtu 1500, starting DHCP\n");
    if (dhcp_start(&g_netif) != ERR_OK) {
        uart_puts("   lwip      dhcp_start FAILED\n");
    }
}

/* Called from the poll loop. lwIP's timers drive DHCP retries, ARP expiry and
 * every TCP retransmission, so this is not optional housekeeping -- without it
 * DHCP never retries and TCP never recovers from a lost segment. */
void netif_wifi_tick(void);
void netif_wifi_tick(void)
{
    if (g_up) { sys_check_timeouts(); }
}

/* Has DHCP produced an address yet? Reported rather than assumed. */
uint32_t netif_wifi_report(void);
uint32_t netif_wifi_report(void)
{
    static uint32_t announced;
    if (!g_up) { return 0u; }
    const ip4_addr_t *a = netif_ip4_addr(&g_netif);
    uint32_t ip = ip4_addr_get_u32(a);
    if (ip && !announced) {
        announced = 1u;
        uart_puts("   lwip      DHCP bound -- address ");
        uart_put_dec((ip >>  0) & 0xFFu); uart_putc('.');
        uart_put_dec((ip >>  8) & 0xFFu); uart_putc('.');
        uart_put_dec((ip >> 16) & 0xFFu); uart_putc('.');
        uart_put_dec((ip >> 24) & 0xFFu);
        uart_puts("\n   lwip      ping it -- this is lwIP answering now\n");
    }
    return ip;
}

void netif_wifi_stats(void);
void netif_wifi_stats(void)
{
    uart_puts("   lwip      rx ");
    uart_put_dec(g_lwip_rx);
    uart_puts(" drop ");
    uart_put_dec(g_lwip_rx_drop);
    uart_puts("  tx ");
    uart_put_dec(g_lwip_tx);
    uart_puts(" err ");
    uart_put_dec(g_lwip_tx_err);
    uart_puts("\n");
}
