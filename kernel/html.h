/* nat-os — HTML to text and links.
 *
 * WHAT THIS IS. Enough of an HTML reader to show a page as words rather than
 * as markup: tags removed, entities decoded, block boundaries turned into line
 * breaks, and <a href> recorded so a link can be tapped and followed.
 *
 * WHAT IT IS NOT, said here so browser.h does not have to overpromise twice:
 * there is no layout, no CSS, no tables, no images, no JavaScript. <script> and
 * <style> bodies are DISCARDED rather than shown, which is most of what makes
 * the difference between markup and reading.
 *
 * IT IS NOT A PARSER EITHER, in the sense a validator would mean. It is a
 * single pass over bytes with four states, and malformed input produces wrong
 * text rather than a diagnosis. The input is untrusted, so the only properties
 * it actually guarantees are the ones that matter on this board: it never
 * writes past its own buffers, it always terminates, and everything that did
 * not fit is COUNTED and reportable (html_dropped, html_links_dropped) rather
 * than silently lost -- which is the correction UM-NATOS-060 prescribes and
 * step 388 had to apply to webfetch for exactly this reason.
 */

#ifndef NATOS_HTML_H
#define NATOS_HTML_H

#include <stdint.h>

/* The rendered text. Larger than WEB_BODY_MAX would suggest is needed, because
 * entity decoding can expand and because dropping the tags usually shrinks a
 * page by more than half -- so this is sized to hold what a full body renders
 * to, not to match the body byte for byte. */
#define HTML_TEXT_MAX   1536u
#define HTML_LINKS_MAX  16u
#define HTML_HREF_MAX   96u

typedef struct {
    uint32_t start;                 /* byte offset into html_text() */
    uint32_t end;                   /* one past the last byte */
    char     href[HTML_HREF_MAX];
} html_link_t;

/* Render `len` bytes of an HTTP RESPONSE -- status line, headers and all. The
 * headers are found and skipped here rather than by the caller, because the
 * blank line that separates them is the only reliable marker and a caller that
 * guessed would show half a header as a paragraph. */
void html_render(const char *src, uint32_t len);

const char        *html_text(void);
uint32_t           html_len(void);
uint32_t           html_dropped(void);        /* text bytes that did not fit */
uint32_t           html_link_count(void);
uint32_t           html_links_dropped(void);  /* links past HTML_LINKS_MAX */
const html_link_t *html_link(uint32_t i);

/* Which link, if any, covers byte `off` of the rendered text. -1 for none. */
int html_link_at(uint32_t off);

#endif /* NATOS_HTML_H */
