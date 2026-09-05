/* nat-os — fetch a URL over HTTP. DNS, TCP, and the request, for the web view.
 *
 * WHY THERE IS NO TLS HERE, stated plainly because the first question anyone
 * will ask is why google.com does not render:
 *
 * google.com is HTTPS-only. TLS 1.2 needs X.509 parsing, RSA or ECDSA
 * signature verification, an ECDHE key exchange, AES-GCM, SHA-256, and a trust
 * store of root certificates. mbedTLS wants roughly 40-50 KB of heap for a
 * single handshake; this board's whole heap measures 38,648 bytes at boot. It
 * is not a matter of effort — it does not fit.
 *
 * So this fetches over plain HTTP on port 80. Pointed at google.com that
 * returns a 301 to https://www.google.com/, which is Google's servers genuinely
 * answering this board over its own WPA2 link. That is a real result and it is
 * reported as what it is: a redirect, not a rendered page.
 *
 * WHY ITS OWN DNS RESOLVER: lwipopts.h has LWIP_DNS 0, annotated "needs str*
 * this kernel does not have". Rather than pull in lwIP's resolver and the
 * string library it depends on, this asks one A-record question over raw UDP.
 * A DNS query is a header, a name and four bytes of answer; the whole
 * implementation is shorter than the shim would have been.
 */

#ifndef NATOS_WEBFETCH_H
#define NATOS_WEBFETCH_H

#include <stdint.h>

#define WEB_HOST_MAX  64u
#define WEB_BODY_MAX  1536u     /* what is kept of the response, not of the page */

enum {
    WEB_IDLE = 0,
    WEB_RESOLVING,
    WEB_CONNECTING,
    WEB_REQUESTING,
    WEB_DONE,
    WEB_FAILED
};

/* Start a fetch of http://<host>/<path>. Returns 0 if it was started.
 *
 * MUST be called from the net task: the raw lwIP API is not thread safe and
 * NO_SYS=1 means there is no lock to make it so. Everything in this project
 * that touches lwIP runs from one context, and this is that context. */
int         webfetch_start(const char *host, const char *path);

int         webfetch_state(void);       /* one of the WEB_* above */
const char *webfetch_status(void);      /* a short line naming what happened */
const char *webfetch_body(void);        /* what came back, NUL terminated */
uint32_t    webfetch_len(void);
uint32_t    webfetch_code(void);        /* the HTTP status code, or 0 */

/* Driven from the net task each pass: retries the DNS query and times things
 * out. Without this a lost UDP datagram would hang the fetch forever. */
void        webfetch_service(void);

#endif /* NATOS_WEBFETCH_H */
