/* nat-os -- a TCP listener, and the smallest useful thing to put behind it.
 * next_moves/08 step 235.
 *
 * lwIP's TCP has been compiled in since step 232 and never once exercised.
 * This is the test: a listening socket on port 80 that serves a status page,
 * so the verdict comes from a browser -- a program this project did not write,
 * on a machine it does not control, speaking a protocol it cannot fake its way
 * through. TCP either completes a three-way handshake, carries a request,
 * carries a response and closes cleanly, or the page does not render.
 *
 * The RAW API, not sockets: NO_SYS=1 means there is no netconn or BSD socket
 * layer, and callbacks are how lwIP is driven in that mode. Everything here
 * runs on the poll task, the same single context that already calls
 * sys_check_timeouts() -- lwIP is not re-entrant and this keeps it entered
 * from exactly one place.
 */

#include "uart.h"
#include "timer.h"
#include <stdint.h>

#include "lwip/tcp.h"
#include "lwip/pbuf.h"

uint32_t g_http_conns, g_http_reqs, g_http_errs;

extern uint32_t g_lwip_rx, g_lwip_tx, g_lwip_rx_drop, g_lwip_tx_err;

static char     g_page[1024];
static uint32_t g_page_len;

/* ---- the page ----------------------------------------------------------
 *
 * Built by hand: there is no snprintf in this kernel. The numbers are real --
 * a page of static text would render identically whether or not anything
 * underneath it worked, and would prove only that bytes moved. */

static void put(const char *s)
{
    while (*s && g_page_len < sizeof g_page - 1u) { g_page[g_page_len++] = *s++; }
}

static void putn(uint32_t v)
{
    char t[12];
    uint32_t n = 0u;
    if (!v) { t[n++] = '0'; }
    while (v) { t[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n && g_page_len < sizeof g_page - 1u) { g_page[g_page_len++] = t[--n]; }
}

static void build_page(void)
{
    uint32_t up = timer_ticks() / 100u;          /* ticks are 10 ms */

    g_page_len = 0u;
    put("HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<!doctype html><html><head><title>nat-os</title></head><body>"
        "<h1>nat-os</h1>"
        "<p>This page was served by a from-scratch operating system over a "
        "Wi-Fi driver it reverse-engineered the interface to.</p><ul>");
    put("<li>uptime: ");        putn(up);              put(" s</li>");
    put("<li>lwIP rx: ");       putn(g_lwip_rx);
    put(" (dropped ");          putn(g_lwip_rx_drop);  put(")</li>");
    put("<li>lwIP tx: ");       putn(g_lwip_tx);
    put(" (errors ");           putn(g_lwip_tx_err);   put(")</li>");
    put("<li>connections: ");   putn(g_http_conns);    put("</li>");
    put("<li>requests: ");      putn(g_http_reqs);     put("</li>");
    put("</ul></body></html>");
}

/* ---- callbacks ---------------------------------------------------------- */

static void on_err(void *arg, err_t err)
{
    /* lwIP has ALREADY freed the pcb when this fires. Touching it here is the
     * classic raw-API use-after-free, so this only counts. */
    (void)arg; (void)err;
    g_http_errs++;
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    /* Close only once the data is ACKED. Closing straight after tcp_write
     * would ask lwIP to send and shut down in the same breath, and the reply
     * can be lost -- which presents as a browser showing an empty page. */
    (void)arg; (void)len;
    tcp_close(pcb);
    return ERR_OK;
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)arg;
    if (err != ERR_OK) { g_http_errs++; if (p) { pbuf_free(p); } return err; }

    if (!p) {                       /* NULL pbuf = the peer closed its side */
        tcp_close(pcb);
        return ERR_OK;
    }

    /* Tell lwIP the window may reopen, then release the request. The request
     * itself is not parsed: any HTTP verb gets the same page, because parsing
     * it would prove nothing extra about TCP. */
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    g_http_reqs++;

    build_page();
    tcp_sent(pcb, on_sent);
    err_t w = tcp_write(pcb, g_page, (u16_t)g_page_len, TCP_WRITE_FLAG_COPY);
    if (w != ERR_OK) {
        g_http_errs++;
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    tcp_output(pcb);
    return ERR_OK;
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || !pcb) { g_http_errs++; return ERR_VAL; }
    g_http_conns++;
    tcp_recv(pcb, on_recv);
    tcp_err(pcb, on_err);
    /* A device with tens of kilobytes of RAM should not hold a connection open
     * because a peer went away mid-request. */
    tcp_poll(pcb, NULL, 8);
    return ERR_OK;
}

/* ---- bring-up ----------------------------------------------------------- */

void tcpsrv_start(void);
void tcpsrv_start(void)
{
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) {
        uart_puts("   http      tcp_new FAILED (out of pcbs)\n");
        return;
    }
    if (tcp_bind(pcb, IP_ADDR_ANY, 80) != ERR_OK) {
        uart_puts("   http      tcp_bind FAILED\n");
        tcp_close(pcb);
        return;
    }
    /* tcp_listen RETURNS A NEW PCB and frees the old one -- assigning it back
     * is not tidiness, it is the documented contract, and using the original
     * afterwards is a use-after-free. */
    struct tcp_pcb *lp = tcp_listen(pcb);
    if (!lp) {
        uart_puts("   http      tcp_listen FAILED\n");
        tcp_close(pcb);
        return;
    }
    tcp_accept(lp, on_accept);
    uart_puts("   http      listening on port 80\n");
}

void tcpsrv_report(void);
void tcpsrv_report(void)
{
    uart_puts("  http conns ");
    uart_put_dec(g_http_conns);
    uart_puts(" reqs ");
    uart_put_dec(g_http_reqs);
    uart_puts(" errs ");
    uart_put_dec(g_http_errs);
}
