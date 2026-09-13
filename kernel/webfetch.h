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
/* [step 352] 768, was 1536. The view shows nine lines of thirty-nine columns --
 * 351 characters -- and scrolls through what is kept. 768 is two screens of
 * history, which is as much of a raw HTTP response as anyone reads on a
 * 240x320 panel, and 768 bytes back to the heap.
 *
 * [step 391] 2048, and 352's reasoning is what changed rather than being wrong.
 * It sized this to what a person would READ OF A RAW RESPONSE. Since step 390
 * the response is not what is shown: html.c strips it to text, and a page is
 * mostly markup, so the bytes that survive the cut are the ones that decide
 * whether there is anything to read at all.
 *
 * Measured, not guessed. example.com's real response is 1,461 bytes. At 768
 * the board kept 767 of them -- headers, doctype, <head>, and the first third
 * of a CSS block -- which rendered to FOURTEEN BYTES of text and ZERO links:
 *
 *     web  mode=page  text=14B  links=0
 *       0 |Example Domain|
 *
 * The h1, the paragraph and the only <a href> on the page were all past the
 * cut. The reader worked perfectly and had nothing to read.
 *
 * 2048 costs 1,280 bytes of DRAM against 12,872 free, and it is chosen to hold
 * a real response WITH its headers rather than to just clear example.com --
 * 1,536 would have fitted that one page by 75 bytes, which is a number that
 * would stop being true the first time a server added a header. */
#define WEB_BODY_MAX  2048u

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
/* [step 388] How many response bytes did NOT fit in WEB_BODY_MAX and were
 * thrown away. The cap is deliberate -- see the note on WEB_BODY_MAX -- but
 * until now nothing counted it, so a 1,256-byte page and a 767-byte page were
 * indistinguishable to every caller. Zero means webfetch_body() is the whole
 * of what the server sent. */
uint32_t    webfetch_dropped(void);
uint32_t    webfetch_code(void);        /* the HTTP status code, or 0 */

/* [step 390] Put a canned response in, as though it had arrived. A DIAGNOSTIC,
 * and it exists because of 376: the radio takes the USB link with it on this
 * supply, so the browser's rendering could otherwise only be seen on a run
 * nobody can capture. This drives the REAL body through the REAL view -- which
 * is standing rule 3, a diagnostic must use the path an application uses.
 *
 * It never touches the network and cannot be reached by a VM program: the net
 * device has no channel that calls it. */
void        webfetch_inject(const char *response, uint32_t len);

/* Driven from the net task each pass: retries the DNS query and times things
 * out. Without this a lost UDP datagram would hang the fetch forever. */
void        webfetch_service(void);

#endif /* NATOS_WEBFETCH_H */
