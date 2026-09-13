/* nat-os — the web view.
 *
 * A URL bar, a fetch, and the response on screen. It replaces "draw", which was
 * a VM program; this cannot be one, for the same reason the wifi view could not
 * be: reaching the network means calling lwIP, and a bytecode application has
 * no path to it by design.
 *
 * WHAT IT IS, as of step 390: the response is run through html.c, so the page
 * is shown as TEXT -- tags removed, entities decoded, script and style
 * discarded, paragraphs broken at block boundaries and wrapped at word
 * boundaries. Links are drawn in cyan, selected by a tap, and FOLLOWED by a
 * second tap on the same one. Two taps rather than one because this panel's
 * default calibration puts the reported point some way from the finger, and a
 * single tap that navigates would send the reader somewhere they did not
 * choose. The header toggles back to the raw response, which is what this view
 * used to show and is still the better answer to "what actually came back".
 *
 * WHAT IT IS NOT, said here so the icon does not overpromise: there is no
 * layout, no CSS, no tables, no images and no JavaScript -- html.h is explicit
 * about that. It is a reader, not a rendering engine.
 *
 * AND THERE IS STILL NO TLS, which decides what can be visited more than
 * anything above. webfetch.h explains why (a handshake wants more heap than
 * this board has in total), and the consequence is concrete: an https link is
 * refused out loud rather than attempted, and any site that redirects HTTP to
 * HTTPS -- which is most of them, Wikipedia and Google included -- can only
 * ever show its 301. That redirect is now readable prose with the target as a
 * link, instead of raw headers, and it is still a redirect.
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

/* [step 390] What the last paint actually laid out: one line per visible row,
 * with the link runs marked, printed from the SAME arrays the tap handler reads
 * to decide what was tapped.
 *
 * It exists because `fbdump` cannot see this view -- it dumps the raycast
 * framebuffer, not the panel -- and MISO is held low by GPIO12's strapping
 * resistor, so the panel cannot be read back either (05, answered). Without
 * this the layout and the hit-testing could only be argued about. */
void browser_dump_rows(void);

/* [step 394] Point the view at a URL and fetch it, from the shell.
 *
 * Accepts "host", "host/path", and either with http:// or https:// on the
 * front -- the scheme is stripped and the fetch is plain HTTP either way,
 * because there is no TLS and refusing every URL a person copies out of a real
 * browser is a worse answer than fetching what can be fetched.
 *
 * Same splitter and same fetch the go button uses; browser_where() reports the
 * result so a caller can print where the view actually ended up. */
void        browser_goto(const char *url);
const char *browser_where(void);

#endif /* NATOS_BROWSER_H */
