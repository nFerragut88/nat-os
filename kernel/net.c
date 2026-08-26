/* nat-os -- just enough IP to answer a ping. next_moves/08 steps 226-228.
 *
 * This is NOT a network stack and is not trying to be. It is ARP, ICMP echo
 * and the client half of DHCP, written to answer one question with external
 * evidence: can another machine on the network reach nat-os and get a reply?
 * `ping` is the instrument, for the same reason a phone's Wi-Fi list was the
 * instrument at step 209 -- the verdict comes from a device this project does
 * not control.
 *
 * lwIP is the right long-term answer and this does not compete with it. lwIP
 * needs a port layer (sys_arch, a netif driver, threading) and is a drop of
 * forty thousand lines into a kernel with no libc; that deserves its own
 * session. This is a few hundred lines that either produce a reply or do not.
 *
 * ---- structure ------------------------------------------------------------
 *
 * The driver's receive callback is WINDOWED and runs on the driver's own task.
 * It does the least possible: copy the frame into a ring and return, so the
 * driver's buffer can be freed immediately and nothing slow happens in its
 * context. Everything else runs here, in call0, from a polling loop -- which
 * is also why the parsing can be written plainly.
 */

#include "uart.h"
#include "task.h"
#include "blobcall.h"
#include "critical.h"
#include "timer.h"

extern void     netif_wifi_input(const uint8_t *frame, uint32_t len);
extern void     netif_wifi_tick(void);
extern uint32_t netif_wifi_report(void);
extern void     netif_wifi_stats(void);
extern void     tcpsrv_report(void);
extern void     wpa_hs_report(void);
extern void     netif_wifi_start(uint32_t tx_fn, const uint8_t *mac);
#include <stdint.h>

/* [step 231] 512, was 160 -- and 160 is why DHCP never completed.
 *
 * The ring was sized for a ping (74 B from Windows, 98 from Linux) and never
 * checked against the OTHER protocol in this file. DHCP options begin at
 * 14 + 20 + 8 + 240 = 282 bytes into the frame, and the OFFER measured 352, so
 * every reply was truncated BEFORE its options. dhcp_opt then scanned a region
 * that did not exist -- `while (i + 1 < len)` with i = 282 and len = 160 never
 * runs -- returned "no message type", and net_handle discarded a frame that
 * had arrived perfectly intact.
 *
 * It presented as dhcp 0/0: not "the OFFER was rejected" but "no OFFER was
 * ever seen", which sent the search towards the transmit path twice.
 *
 * 512 covers a DHCP message with options; slots drop from 8 to 6 to keep the
 * cost near 3 KB of .bss. */
#define NET_SLOTS 6u
#define NET_MAX   512u

static volatile uint8_t  g_q[NET_SLOTS][NET_MAX];
static volatile uint16_t g_qlen[NET_SLOTS];
static volatile uint32_t g_head, g_tail;
uint32_t g_net_dropped;         /* ring full: counted, never silent */
uint32_t g_net_truncated;       /* frame longer than NET_MAX: same rule */

/* Our identity. */
static uint8_t  g_mac[6];
static uint8_t  g_ip[4];        /* all zero until DHCP says otherwise */
static uint8_t  g_srv[4];
static uint32_t g_have_ip;
static uint32_t g_tx_fn;

/* [step 233] Which stack owns received frames.
 *
 * lwIP and the hand-written ARP/ICMP in this file would BOTH answer the same
 * ARP request if both saw it, and two replies for one address from one MAC is
 * a way to confuse a peer's cache rather than a redundancy. So each frame goes
 * to exactly one of them.
 *
 * lwIP by default -- it is the real stack, it has TCP and UDP, and its DHCP
 * client is code somebody else has already debugged. The hand-written path
 * stays because it is what proved the data path works at all, and because a
 * fallback that has been seen to work is worth keeping while the new thing is
 * being trusted. */
uint32_t g_use_lwip = 1u;

/* Counters -- the report is the result. */
uint32_t g_net_arp_req, g_net_arp_rep, g_net_icmp_req, g_net_icmp_rep;
uint32_t g_net_dhcp_offer, g_net_dhcp_ack, g_net_frames;

static uint8_t g_out[NET_MAX + 160u];

/* ---- helpers ------------------------------------------------------------ */

static void be16w(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint32_t be16r(const volatile uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }

static uint32_t cksum(const uint8_t *p, uint32_t len)
{
    uint32_t s = 0u;
    for (uint32_t i = 0u; i + 1u < len; i += 2u) { s += ((uint32_t)p[i] << 8) | p[i + 1u]; }
    if (len & 1u) { s += (uint32_t)p[len - 1u] << 8; }
    while (s >> 16) { s = (s & 0xFFFFu) + (s >> 16); }
    return (~s) & 0xFFFFu;
}

static int ip_is(const volatile uint8_t *p, const uint8_t *q)
{
    return p[0] == q[0] && p[1] == q[1] && p[2] == q[2] && p[3] == q[3];
}

static void net_send(uint32_t len)
{
    if (!g_tx_fn) { return; }
    (void)blob_call(g_tx_fn, 0u /* WIFI_IF_STA */, (uint32_t)g_out, len, 0u);
}

/* ---- the ring, filled from windowed code -------------------------------- */

void net_rx_enqueue(const uint8_t *f, uint32_t len);
void net_rx_enqueue(const uint8_t *f, uint32_t len)
{
    uint32_t crit = crit_enter();
    uint32_t nxt = (g_head + 1u) % NET_SLOTS;
    if (nxt == g_tail) {
        g_net_dropped++;                 /* full: drop the NEWEST, keep order */
        crit_exit(crit);
        return;
    }
    /* [step 231] Truncation is now COUNTED. A silently shortened frame is how
     * the DHCP bug hid: the parse failed for a reason that never appeared in
     * any output. */
    if (len > NET_MAX) { g_net_truncated++; }
    uint32_t n = len < NET_MAX ? len : NET_MAX;
    for (uint32_t i = 0u; i < n; i++) { g_q[g_head][i] = f[i]; }
    g_qlen[g_head] = (uint16_t)n;
    g_head = nxt;
    crit_exit(crit);
}

/* ---- ARP ---------------------------------------------------------------- */

static void arp_reply(const volatile uint8_t *in)
{
    for (uint32_t i = 0u; i < 6u; i++) { g_out[i] = in[6u + i]; }      /* to sender */
    for (uint32_t i = 0u; i < 6u; i++) { g_out[6u + i] = g_mac[i]; }
    be16w(&g_out[12], 0x0806u);

    uint8_t *a = &g_out[14];
    be16w(&a[0], 1u);            /* ethernet */
    be16w(&a[2], 0x0800u);       /* IPv4 */
    a[4] = 6u; a[5] = 4u;
    be16w(&a[6], 2u);            /* REPLY */
    for (uint32_t i = 0u; i < 6u; i++) { a[8u + i] = g_mac[i]; }
    for (uint32_t i = 0u; i < 4u; i++) { a[14u + i] = g_ip[i]; }
    for (uint32_t i = 0u; i < 6u; i++) { a[18u + i] = in[14u + 8u + i]; }
    for (uint32_t i = 0u; i < 4u; i++) { a[24u + i] = in[14u + 14u + i]; }

    g_net_arp_rep++;
    net_send(42u);
}

/* ---- ICMP echo ---------------------------------------------------------- */

static void icmp_reply(const volatile uint8_t *in, uint32_t len)
{
    if (len > sizeof g_out) { return; }
    for (uint32_t i = 0u; i < len; i++) { g_out[i] = in[i]; }

    for (uint32_t i = 0u; i < 6u; i++) { g_out[i] = in[6u + i]; }       /* swap MAC */
    for (uint32_t i = 0u; i < 6u; i++) { g_out[6u + i] = g_mac[i]; }

    /* [step 230] SWAP, properly. This read:
     *
     *     ip[12+i] = in[14+16+i];    src = the incoming DESTINATION = us  (ok)
     *     ip[16+i] = g_ip[i];        dst = US AGAIN                       (wrong)
     *
     * so every echo reply was addressed to nat-os itself and the machine that
     * sent the ping heard nothing. It presents as "Request timed out", which
     * is indistinguishable from not replying at all -- the reply existed, it
     * was just sent to the wrong place.
     *
     * The sender's address is the incoming SOURCE at +12 of the IP header. */
    uint8_t *ip = &g_out[14];
    for (uint32_t i = 0u; i < 4u; i++) {
        ip[12u + i] = g_ip[i];               /* from us */
        ip[16u + i] = in[14u + 12u + i];     /* to whoever asked */
    }
    be16w(&ip[10], 0u);
    be16w(&ip[10], cksum(ip, 20u));

    uint8_t *ic = &g_out[34];
    ic[0] = 0u;                       /* echo REPLY */
    be16w(&ic[2], 0u);
    uint32_t iclen = len - 34u;
    be16w(&ic[2], cksum(ic, iclen));

    g_net_icmp_rep++;
    net_send(len);
}

/* ---- DHCP --------------------------------------------------------------- */

static uint32_t dhcp_opt(const volatile uint8_t *in, uint32_t len, uint32_t want,
                         uint8_t *out, uint32_t outn)
{
    uint32_t i = 14u + 20u + 8u + 240u;          /* first option */
    while (i + 1u < len) {
        uint32_t code = in[i];
        if (code == 255u) { break; }
        if (code == 0u) { i++; continue; }
        uint32_t l = in[i + 1u];
        if (code == want) {
            uint32_t n = l < outn ? l : outn;
            for (uint32_t k = 0u; k < n; k++) { out[k] = in[i + 2u + k]; }
            return n;
        }
        i += 2u + l;
    }
    return 0u;
}

/* Build a REQUEST from an OFFER: same shape as the DISCOVER, plus option 50
 * (the address being asked for) and 54 (which server offered it). Without
 * those two a server has no idea which offer is being accepted. */
static void dhcp_request(const volatile uint8_t *in, const uint8_t *yi, const uint8_t *sid)
{
    for (uint32_t i = 0u; i < sizeof g_out; i++) { g_out[i] = 0u; }
    for (uint32_t i = 0u; i < 6u; i++) { g_out[i] = 0xFFu; }
    for (uint32_t i = 0u; i < 6u; i++) { g_out[6u + i] = g_mac[i]; }
    be16w(&g_out[12], 0x0800u);

    uint8_t *ip = &g_out[14], *ud = ip + 20, *bp = ud + 8;

    bp[0] = 1u; bp[1] = 1u; bp[2] = 6u;
    for (uint32_t i = 0u; i < 4u; i++) { bp[4u + i] = in[14u + 20u + 8u + 4u + i]; } /* xid */
    be16w(&bp[10], 0x8000u);
    for (uint32_t i = 0u; i < 6u; i++) { bp[28u + i] = g_mac[i]; }
    bp[236] = 99u; bp[237] = 130u; bp[238] = 83u; bp[239] = 99u;
    bp[240] = 53u; bp[241] = 1u; bp[242] = 3u;                 /* REQUEST */
    bp[243] = 50u; bp[244] = 4u;
    for (uint32_t i = 0u; i < 4u; i++) { bp[245u + i] = yi[i]; }
    bp[249] = 54u; bp[250] = 4u;
    for (uint32_t i = 0u; i < 4u; i++) { bp[251u + i] = sid[i]; }
    bp[255] = 255u;
    uint32_t bl = 256u, udl = 8u + bl, ipl = 20u + udl;

    be16w(&ud[0], 68u); be16w(&ud[2], 67u); be16w(&ud[4], udl); be16w(&ud[6], 0u);
    ip[0] = 0x45u; be16w(&ip[2], ipl); ip[8] = 64u; ip[9] = 17u;
    for (uint32_t i = 0u; i < 4u; i++) { ip[16u + i] = 0xFFu; }
    be16w(&ip[10], 0u);
    be16w(&ip[10], cksum(ip, 20u));

    net_send(14u + ipl);
}

/* ---- the poll ----------------------------------------------------------- */

static void net_handle(const volatile uint8_t *f, uint32_t len)
{
    if (len < 14u) { return; }
    uint32_t et = be16r(&f[12]);
    g_net_frames++;

    if (et == 0x0806u && len >= 42u) {
        if (be16r(&f[14u + 6u]) == 1u && g_have_ip && ip_is(&f[14u + 24u], g_ip)) {
            g_net_arp_req++;
            arp_reply(f);
        }
        return;
    }
    if (et != 0x0800u || len < 34u) { return; }

    uint32_t proto = f[23];
    if (proto == 1u && g_have_ip && ip_is(&f[30], g_ip) && f[34] == 8u) {
        g_net_icmp_req++;
        icmp_reply(f, len);
        return;
    }
    if (proto == 17u && be16r(&f[34]) == 67u) {
        uint8_t t[4];
        if (dhcp_opt(f, len, 53u, t, 4u) < 1u) { return; }
        if (t[0] == 2u) {                                  /* OFFER */
            uint8_t yi[4], sid[4];
            for (uint32_t i = 0u; i < 4u; i++) { yi[i] = f[58u + i]; }
            if (dhcp_opt(f, len, 54u, sid, 4u) < 4u) {
                for (uint32_t i = 0u; i < 4u; i++) { sid[i] = f[26u + i]; }
            }
            g_net_dhcp_offer++;
            dhcp_request(f, yi, sid);
        } else if (t[0] == 5u) {                           /* ACK */
            for (uint32_t i = 0u; i < 4u; i++) { g_ip[i] = f[58u + i]; }
            for (uint32_t i = 0u; i < 4u; i++) { g_srv[i] = f[26u + i]; }
            g_have_ip = 1u;
            g_net_dhcp_ack++;
            uart_puts("   net       DHCP ACK -- address ");
            for (uint32_t i = 0u; i < 4u; i++) {
                uart_put_dec(g_ip[i]);
                if (i != 3u) { uart_putc('.'); }
            }
            uart_puts("\n   net       ping it from another machine on this network\n");
        }
    }
}

void net_set_tx(uint32_t tx_fn, const uint8_t *mac);
void net_set_tx(uint32_t tx_fn, const uint8_t *mac)
{
    g_tx_fn = tx_fn;
    for (uint32_t i = 0u; i < 6u; i++) { g_mac[i] = mac[i]; }

    /* [step 229] A STATIC ADDRESS, to take DHCP out of the question.
     *
     * The ping failed with "Destination host unreachable" reported by the
     * PINGING machine's own address -- which means its ARP for us went
     * unanswered and the ICMP was never sent. That is exactly what this code
     * is supposed to do when g_have_ip is 0: nat-os will not answer ARP for an
     * address it does not believe it owns, and the DHCP ACK never arrived.
     *
     * So DHCP and ARP/ICMP fail together and cannot be told apart. Setting the
     * address by hand separates them: if ping works now, ARP and ICMP are
     * correct and only the DHCP exchange is broken; if it still does not, the
     * fault is below DHCP and DHCP was never the problem.
     *
     * .200 is chosen to sit well clear of a hotspot's DHCP pool, which hands
     * out low addresses first. DHCP still runs, and an ACK still overrides
     * this -- the static value is a floor, not a lock. */
    if (g_use_lwip) {
        /* [step 233] lwIP owns addressing now: its DHCP client assigns one,
         * and a hardcoded address here would fight it. */
        netif_wifi_start(tx_fn, mac);
        return;
    }
    if (!g_have_ip) {
        g_ip[0] = 10u; g_ip[1] = 224u; g_ip[2] = 203u; g_ip[3] = 200u;
        g_have_ip = 1u;
        uart_puts("   net       static address 10.224.203.200 (DHCP may replace it)\n");
    }
}

void net_poll_for(uint32_t ticks);
void net_poll_for(uint32_t ticks)
{
    /* [step 228] ELAPSED time, from the timer -- not a count of the sleeps
     * that were asked for. The first version did `spent += 2` per iteration on
     * the assumption that task_sleep(2) costs exactly two ticks; it does not,
     * and a poll advertised as 120 s ran long enough to overrun a 210 s
     * capture and print nothing at all. Same error as the beacon pacing at
     * step 209: assert the duration, then be surprised by it. */
    uint32_t t0 = timer_ticks();
    uint32_t last = 0u;
    while ((timer_ticks() - t0) < ticks) {
        /* [step 262] BOUNDED. This loop was 'drain until empty', and on a
         * busy network it never is: frames arrive as fast as they are taken,
         * so the loop never exits, netif_wifi_tick() below never runs, and
         * the report never prints.
         *
         * That is the whole of step 260's mystery. lwIP kept working, because
         * input is what drives TCP -- a request that arrives is answered
         * inline. What stopped was lwIP's TIMERS: ARP expiry, DHCP renewal
         * and above all TCP RETRANSMISSION. A connection that completes when
         * nothing is lost and hangs forever when anything is, which is
         * precisely how the page behaved: sometimes instant, usually a
         * timeout.
         *
         * Sixteen frames is well above the burst the ring holds between
         * iterations and far below the point where the timers starve. */
        uint32_t drained = 0u;
        while (g_tail != g_head && drained < 16u) {
            drained++;
            if (g_use_lwip) {
                /* The ring holds a volatile copy; lwIP wants a plain pointer.
                 * The cast is safe because the producer has already finished
                 * with this slot -- g_tail only advances past frames the
                 * enqueue side committed. */
                netif_wifi_input((const uint8_t *)g_q[g_tail], g_qlen[g_tail]);
            } else {
                net_handle(g_q[g_tail], g_qlen[g_tail]);
            }
            g_tail = (g_tail + 1u) % NET_SLOTS;
        }
        /* lwIP's timers drive DHCP retries, ARP expiry and every TCP
         * retransmission. Skipping this does not slow the stack down; it stops
         * it recovering from anything. */
        if (g_use_lwip) {
            netif_wifi_tick();
            (void)netif_wifi_report();
        }
        /* A silent minute is indistinguishable from a hang. Say something. */
        uint32_t el = timer_ticks() - t0;
        if (el - last >= 1000u) {
            last = el;
            /* [step 234] In lwIP mode the counters below belong to the
             * hand-written path and stay at zero, which reads as "nothing is
             * happening" when in fact everything is. Report whoever is
             * actually handling the frames. */
            if (g_use_lwip) {
                uart_puts("   lwip      +");
                uart_put_dec(el / 100u);
                uart_puts("s  ");
                netif_wifi_stats();
                tcpsrv_report();
                wpa_hs_report();
                uart_puts("\n");
                task_sleep(2u);
                continue;
            }
            uart_puts("   net       +");
            uart_put_dec(el / 100u);
            uart_puts("s frames ");
            uart_put_dec(g_net_frames);
            uart_puts(" arp ");
            uart_put_dec(g_net_arp_req);
            uart_puts(" icmp ");
            uart_put_dec(g_net_icmp_req);
            /* [step 228] DHCP counts in the live line too. "frames 1" says
             * something arrived; only these say whether it was understood. */
            uart_puts(" dhcp ");
            uart_put_dec(g_net_dhcp_offer);
            uart_puts("/");
            uart_put_dec(g_net_dhcp_ack);
            uart_puts(" drop ");
            uart_put_dec(g_net_dropped);
            uart_puts("/");
            uart_put_dec(g_net_truncated);
            uart_puts(g_have_ip ? " [IP]\n" : " [no IP]\n");
        }
        task_sleep(2u);
    }
}

void net_report(void);
void net_report(void)
{
    uart_puts("   net       frames ");
    uart_put_dec(g_net_frames);
    uart_puts("  dropped ");
    uart_put_dec(g_net_dropped);
    uart_puts("  dhcp offer/ack ");
    uart_put_dec(g_net_dhcp_offer);
    uart_puts("/");
    uart_put_dec(g_net_dhcp_ack);
    uart_puts("  arp ");
    uart_put_dec(g_net_arp_req);
    uart_puts("->");
    uart_put_dec(g_net_arp_rep);
    uart_puts("  icmp ");
    uart_put_dec(g_net_icmp_req);
    uart_puts("->");
    uart_put_dec(g_net_icmp_rep);
    uart_puts(g_have_ip ? "  [have IP]\n" : "  [no IP]\n");
    if (g_use_lwip) { netif_wifi_stats(); }
}
