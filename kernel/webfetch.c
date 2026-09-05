/* nat-os — DNS and HTTP for the web view. See webfetch.h for why there is no
 * TLS and why this carries its own resolver. */

#include "webfetch.h"
#include "uart.h"
#include "timer.h"

#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"

extern uint32_t netif_wifi_ip(void);
extern uint32_t netif_wifi_gw(void);

/* ---- state --------------------------------------------------------------- */

static int      g_state;
static char     g_host[WEB_HOST_MAX];
static char     g_path[WEB_HOST_MAX];
static char     g_body[WEB_BODY_MAX];
static uint32_t g_len;
static uint32_t g_code;
static const char *g_status = "";

static ip_addr_t      g_addr;
static struct udp_pcb *g_dns;
static struct tcp_pcb *g_tcp;
static uint32_t g_t0;           /* when the current phase started */
static uint32_t g_tries;
static uint16_t g_id;

static void fail(const char *why)
{
    g_status = why;
    g_state  = WEB_FAILED;
    if (g_dns) { udp_remove(g_dns); g_dns = 0; }
    /* g_tcp is closed by its own callbacks; abort only if still open. */
    if (g_tcp) { tcp_abort(g_tcp); g_tcp = 0; }
}

int         webfetch_state(void)  { return g_state; }
const char *webfetch_status(void) { return g_status; }
const char *webfetch_body(void)   { return g_body; }
uint32_t    webfetch_len(void)    { return g_len; }
uint32_t    webfetch_code(void)   { return g_code; }

/* ---- HTTP ---------------------------------------------------------------- */

/* The response arrives in pieces and only the first WEB_BODY_MAX bytes are
 * kept. That is deliberate: a page is not being rendered, it is being shown,
 * and a screen holds about nine lines. Keeping the head of the response also
 * keeps the part that matters — the status line and the headers. */
static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)arg;
    if (err != ERR_OK) { fail("recv error"); return err; }

    if (!p) {                       /* the server closed: that is the end */
        g_body[g_len] = 0;
        if (g_state == WEB_REQUESTING) {
            g_state  = WEB_DONE;
            g_status = "done";
        }
        tcp_close(pcb);
        g_tcp = 0;
        return ERR_OK;
    }

    struct pbuf *q = p;
    while (q) {
        const char *s = (const char *)q->payload;
        for (uint16_t i = 0u; i < q->len && g_len + 1u < WEB_BODY_MAX; i++) {
            char c = s[i];
            /* Keep it printable. A raw response carries CR and stray bytes that
             * would otherwise land in the font as blocks. */
            g_body[g_len++] = (c == '\n' || (c >= 32 && c < 127)) ? c : ' ';
        }
        q = q->next;
    }
    g_body[g_len] = 0;

    /* "HTTP/1.1 301 Moved" -> 301. Parsed once, from the head. */
    if (!g_code && g_len > 12u && g_body[0] == 'H') {
        uint32_t i = 0u;
        while (i < g_len && g_body[i] != ' ') { i++; }
        while (i < g_len && g_body[i] == ' ') { i++; }
        uint32_t v = 0u;
        while (i < g_len && g_body[i] >= '0' && g_body[i] <= '9') {
            v = v * 10u + (uint32_t)(g_body[i++] - '0');
        }
        g_code = v;
    }

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void on_err(void *arg, err_t err)
{
    (void)arg; (void)err;
    g_tcp = 0;                      /* lwIP has already freed the pcb */
    if (g_state == WEB_CONNECTING || g_state == WEB_REQUESTING) {
        g_status = "connection failed";
        g_state  = WEB_FAILED;
    }
}

static err_t on_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK) { fail("connect refused"); return err; }

    /* HTTP/1.0 with an explicit Host, which is the smallest request a modern
     * server will answer. 1.0 rather than 1.1 so the server closes when it is
     * done and the close IS the end of the body -- no chunked decoding, no
     * Content-Length parsing. */
    static char req[256];
    uint32_t n = 0u;
    const char *p;
    p = "GET ";            while (*p) { req[n++] = *p++; }
    p = g_path;            while (*p && n < 180u) { req[n++] = *p++; }
    p = " HTTP/1.0\r\nHost: "; while (*p) { req[n++] = *p++; }
    p = g_host;            while (*p && n < 240u) { req[n++] = *p++; }
    p = "\r\nUser-Agent: nat-os\r\nConnection: close\r\n\r\n";
    while (*p) { req[n++] = *p++; }

    if (tcp_write(pcb, req, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) {
        fail("write failed");
        return ERR_MEM;
    }
    tcp_output(pcb);
    g_state  = WEB_REQUESTING;
    g_status = "waiting for reply";
    g_t0     = timer_ticks();
    return ERR_OK;
}

static void http_go(void)
{
    g_tcp = tcp_new();
    if (!g_tcp) { fail("no tcp pcb"); return; }
    tcp_recv(g_tcp, on_recv);
    tcp_err(g_tcp, on_err);
    g_state  = WEB_CONNECTING;
    g_status = "connecting";
    g_t0     = timer_ticks();
    if (tcp_connect(g_tcp, &g_addr, 80, on_connected) != ERR_OK) {
        fail("connect failed");
    }
}

/* ---- DNS ----------------------------------------------------------------- */

/* One A-record question. Names are length-prefixed labels: "google.com"
 * becomes 6 g o o g l e 3 c o m 0. */
static uint32_t dns_query(uint8_t *b)
{
    uint32_t n = 0u;
    b[n++] = (uint8_t)(g_id >> 8); b[n++] = (uint8_t)g_id;
    b[n++] = 0x01; b[n++] = 0x00;       /* standard query, recursion desired */
    b[n++] = 0x00; b[n++] = 0x01;       /* one question */
    b[n++] = 0x00; b[n++] = 0x00;
    b[n++] = 0x00; b[n++] = 0x00;
    b[n++] = 0x00; b[n++] = 0x00;

    uint32_t i = 0u;
    while (g_host[i]) {
        uint32_t start = i, len = 0u;
        while (g_host[i] && g_host[i] != '.') { i++; len++; }
        b[n++] = (uint8_t)len;
        for (uint32_t k = 0u; k < len; k++) { b[n++] = (uint8_t)g_host[start + k]; }
        if (g_host[i] == '.') { i++; }
    }
    b[n++] = 0x00;                      /* root label ends the name */
    b[n++] = 0x00; b[n++] = 0x01;       /* QTYPE  A */
    b[n++] = 0x00; b[n++] = 0x01;       /* QCLASS IN */
    return n;
}

static void on_dns(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                   const ip_addr_t *addr, u16_t port)
{
    (void)arg; (void)pcb; (void)addr; (void)port;
    if (!p) { return; }

    uint8_t b[512];
    uint16_t n = p->tot_len > sizeof b ? (uint16_t)sizeof b : p->tot_len;
    pbuf_copy_partial(p, b, n, 0);
    pbuf_free(p);

    if (g_state != WEB_RESOLVING || n < 12u) { return; }
    if (((uint16_t)b[0] << 8 | b[1]) != g_id) { return; }   /* not our question */

    uint16_t qd = (uint16_t)(b[4] << 8 | b[5]);
    uint16_t an = (uint16_t)(b[6] << 8 | b[7]);
    if ((b[3] & 0x0Fu) != 0u) { fail("host not found"); return; }
    if (an == 0u)             { fail("no address"); return; }

    uint32_t i = 12u;
    /* Skip the questions: each is a name then four bytes. */
    for (uint16_t q = 0u; q < qd && i < n; q++) {
        while (i < n && b[i]) {
            if ((b[i] & 0xC0u) == 0xC0u) { i += 2u; break; }    /* compressed */
            i += (uint32_t)b[i] + 1u;
        }
        if (i < n && !b[i]) { i++; }
        i += 4u;
    }

    /* Walk the answers for the first A record. */
    for (uint16_t a = 0u; a < an && i + 12u <= n; a++) {
        if ((b[i] & 0xC0u) == 0xC0u) { i += 2u; }
        else {
            while (i < n && b[i]) { i += (uint32_t)b[i] + 1u; }
            if (i < n) { i++; }
        }
        if (i + 10u > n) { break; }
        uint16_t type = (uint16_t)(b[i] << 8 | b[i + 1u]);
        uint16_t rdl  = (uint16_t)(b[i + 8u] << 8 | b[i + 9u]);
        i += 10u;
        if (type == 1u && rdl == 4u && i + 4u <= n) {
            IP4_ADDR(&g_addr, b[i], b[i + 1u], b[i + 2u], b[i + 3u]);
            udp_remove(g_dns);
            g_dns = 0;
            http_go();
            return;
        }
        i += rdl;
    }
    fail("no A record");
}

static void dns_send(void)
{
    static uint8_t q[256];
    uint32_t n = dns_query(q);

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)n, PBUF_RAM);
    if (!p) { fail("out of buffers"); return; }
    for (uint32_t i = 0u; i < n; i++) { ((uint8_t *)p->payload)[i] = q[i]; }

    /* The gateway. A home router forwards DNS, and using it keeps this board
     * off any name server the user did not already choose by joining their
     * network -- which is the right default for something that is not going to
     * ask permission. */
    ip_addr_t srv;
    uint32_t gw = netif_wifi_gw();
    IP4_ADDR(&srv, gw & 0xFFu, (gw >> 8) & 0xFFu, (gw >> 16) & 0xFFu, (gw >> 24) & 0xFFu);

    udp_sendto(g_dns, p, &srv, 53);
    pbuf_free(p);
    g_t0 = timer_ticks();
    g_tries++;
}

/* ---- the front door ------------------------------------------------------ */

int webfetch_start(const char *host, const char *path)
{
    if (!host || !host[0]) { return -1; }
    if (!netif_wifi_ip())  { g_status = "no network"; g_state = WEB_FAILED; return -1; }

    if (g_dns) { udp_remove(g_dns); g_dns = 0; }
    if (g_tcp) { tcp_abort(g_tcp);  g_tcp = 0; }

    uint32_t i = 0u;
    for (; i + 1u < WEB_HOST_MAX && host[i]; i++) { g_host[i] = host[i]; }
    g_host[i] = 0;
    i = 0u;
    if (path && path[0]) {
        for (; i + 1u < WEB_HOST_MAX && path[i]; i++) { g_path[i] = path[i]; }
    } else {
        g_path[i++] = '/';
    }
    g_path[i] = 0;

    g_len = 0u; g_body[0] = 0; g_code = 0u; g_tries = 0u;
    g_id = (uint16_t)(timer_ticks() & 0xFFFFu) | 1u;

    g_dns = udp_new();
    if (!g_dns) { fail("no udp pcb"); return -1; }
    udp_recv(g_dns, on_dns, 0);
    if (udp_bind(g_dns, IP_ADDR_ANY, 0) != ERR_OK) { fail("bind failed"); return -1; }

    g_state  = WEB_RESOLVING;
    g_status = "resolving";
    dns_send();
    return 0;
}

void webfetch_service(void)
{
    if (g_state != WEB_RESOLVING && g_state != WEB_CONNECTING &&
        g_state != WEB_REQUESTING) {
        return;
    }

    uint32_t el = timer_ticks() - g_t0;

    /* A lost datagram must not hang the fetch. Two retries at two seconds, then
     * the question is answered: nothing is listening. */
    if (g_state == WEB_RESOLVING && el > 200u) {
        if (g_tries < 3u) { dns_send(); }
        else              { fail("dns timeout"); }
        return;
    }
    if (g_state == WEB_CONNECTING && el > 800u) { fail("connect timeout"); return; }
    if (g_state == WEB_REQUESTING && el > 1200u) {
        /* Whatever arrived is what there is. A truncated answer is still an
         * answer, and reporting it beats reporting nothing. */
        g_body[g_len] = 0;
        if (g_len) { g_state = WEB_DONE; g_status = "done (timed out)"; }
        else       { fail("no reply"); }
        if (g_tcp) { tcp_abort(g_tcp); g_tcp = 0; }
    }
}
