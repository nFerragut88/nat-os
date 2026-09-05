/* nat-os — the web view.
 *
 * A URL bar, a fetch, and the response on screen. It replaces "draw", which was
 * a VM program; this cannot be one, for the same reason the wifi view could not
 * be: reaching the network means calling lwIP, and a bytecode application has
 * no path to it by design.
 *
 * WHAT IT IS NOT, said here so the icon does not overpromise: this is not a
 * rendering engine. It fetches over HTTP and shows what came back — the status
 * line, the headers, the beginning of the body. There is no HTML parser, no
 * layout, no CSS, and there is no TLS (webfetch.h explains why: the handshake
 * wants more heap than this board has in total).
 *
 * Pointed at google.com it shows Google's 301 redirect to HTTPS. That is a real
 * answer from Google's servers, reached over a link this board negotiated
 * itself, and it is displayed as what it is rather than dressed up as a page.
 */

#ifndef NATOS_BROWSER_H
#define NATOS_BROWSER_H

#include <stdint.h>

void browser_open(void);
void browser_frame(void);
void browser_touch(uint32_t x, uint32_t y, int down);

/* Runs the fetch. Called from the NET task, never from the task that reads the
 * glass — the same rule the wifi view learned at step 281. */
void browser_service(void);

#endif /* NATOS_BROWSER_H */
