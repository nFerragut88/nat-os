/* nat-os — HTML to text and links. See html.h for what this is and is not. */

#include "html.h"

static char        g_text[HTML_TEXT_MAX];
static uint32_t    g_len;
static uint32_t    g_dropped;
static html_link_t g_links[HTML_LINKS_MAX];
static uint32_t    g_nlinks;
static uint32_t    g_links_dropped;

const char        *html_text(void)          { return g_text; }
uint32_t           html_len(void)           { return g_len; }
uint32_t           html_dropped(void)       { return g_dropped; }
uint32_t           html_link_count(void)    { return g_nlinks; }
uint32_t           html_links_dropped(void) { return g_links_dropped; }

const html_link_t *html_link(uint32_t i)
{
    return i < g_nlinks ? &g_links[i] : 0;
}

int html_link_at(uint32_t off)
{
    for (uint32_t i = 0u; i < g_nlinks; i++) {
        if (off >= g_links[i].start && off < g_links[i].end) { return (int)i; }
    }
    return -1;
}

/* ---- small helpers, local on purpose ------------------------------------- */

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* Case-insensitive compare of an n-byte tag name against a lowercase literal. */
static int tag_is(const char *s, uint32_t n, const char *want)
{
    uint32_t i = 0u;
    for (; i < n && want[i]; i++) {
        if (lower(s[i]) != want[i]) { return 0; }
    }
    return i == n && !want[i];
}

/* "Refuse rather than clamp" is the standing rule, and a DISPLAY buffer is the
 * one place it does not apply: showing the first part of a page beats showing
 * nothing. So this truncates AND COUNTS, which is the shape step 388 settled
 * on for exactly the same choice in webfetch. */
static void put_ch(char c)
{
    if (g_len + 1u < HTML_TEXT_MAX) { g_text[g_len++] = c; }
    else                            { g_dropped++; }
}

/* Collapse runs of whitespace, and never start a line with one. */
static void put_space(void)
{
    if (g_len == 0u) { return; }
    char last = g_text[g_len - 1u];
    if (last == ' ' || last == '\n') { return; }
    put_ch(' ');
}

static void put_break(void)
{
    if (g_len == 0u) { return; }
    /* Trailing spaces before a break are invisible and cost a column. */
    while (g_len && g_text[g_len - 1u] == ' ') { g_len--; }
    if (g_len && g_text[g_len - 1u] != '\n') { put_ch('\n'); }
}

/* The tags that mean "a new line starts here". Not a complete list of block
 * elements -- that would be most of HTML -- but the ones whose absence makes a
 * page read as one run-on paragraph. */
static int is_break_tag(const char *s, uint32_t n)
{
    static const char *const B[] = {
        "p", "br", "div", "tr", "li", "h1", "h2", "h3", "h4", "h5", "h6",
        "ul", "ol", "table", "section", "article", "header", "footer", "nav",
        "blockquote", "pre", "hr", "form", "figure", "dd", "dt", "dl", "title",
        0
    };
    for (uint32_t i = 0u; B[i]; i++) {
        if (tag_is(s, n, B[i])) { return 1; }
    }
    return 0;
}

/* ---- entities ------------------------------------------------------------
 *
 * The handful that actually appear in prose. An unknown entity is emitted
 * VERBATIM, ampersand and all, rather than dropped: a visible "&sup2;" says
 * something was not understood, and a silent deletion would not. */
static uint32_t entity(const char *s, uint32_t n)
{
    static const struct { const char *name; char out; } E[] = {
        { "amp", '&' },   { "lt", '<' },     { "gt", '>' },    { "quot", '"' },
        { "apos", 0x27 }, { "nbsp", ' ' },   { "#39", 0x27 },  { "#34", '"' },
        { "#38", '&' },   { "#60", '<' },    { "#62", '>' },   { "#160", ' ' },
        { "mdash", '-' }, { "ndash", '-' },  { "hellip", '.' },
        { "middot", '.' }, { "times", 'x' }, { "copy", 'c' },
        { "reg", 'r' },   { "deg", 'o' },    { 0, 0 }
    };
    for (uint32_t i = 0u; E[i].name; i++) {
        uint32_t k = 0u;
        while (E[i].name[k] && k < n && E[i].name[k] == s[k]) { k++; }
        if (!E[i].name[k] && k == n) { put_ch(E[i].out); return 1u; }
    }
    return 0u;
}

/* ---- headers -------------------------------------------------------------
 *
 * An HTTP response is a status line, headers, a BLANK LINE, then the body.
 * Finding that blank line belongs here rather than in the caller because
 * webfetch maps every non-printable byte to a space (webfetch.c, on_recv), so
 * the CRLFCRLF that separates them arrives as newline, space, newline -- and a
 * caller looking for a literal CRLFCRLF would never find it and would render
 * the headers as prose. */
static uint32_t skip_headers(const char *s, uint32_t n)
{
    for (uint32_t i = 0u; i < n; i++) {
        if (s[i] != '\n') { continue; }
        uint32_t j = i + 1u;
        while (j < n && (s[j] == ' ' || s[j] == '\r')) { j++; }
        if (j < n && s[j] == '\n') { return j + 1u; }
    }
    return 0u;          /* no blank line: treat the whole thing as content */
}

/* ---- the pass ------------------------------------------------------------ */

void html_render(const char *src, uint32_t len)
{
    g_len = 0u;
    g_dropped = 0u;
    g_nlinks = 0u;
    g_links_dropped = 0u;
    g_text[0] = 0;

    if (!src || !len) { return; }

    uint32_t i = skip_headers(src, len);
    int open_link = -1;                 /* index into g_links, or -1 */

    while (i < len) {
        char c = src[i];

        /* --- a comment, or a doctype ---------------------------------- */
        if (c == '<' && i + 3u < len && src[i + 1u] == '!') {
            if (src[i + 2u] == '-' && src[i + 3u] == '-') {
                i += 4u;
                while (i + 2u < len &&
                       !(src[i] == '-' && src[i + 1u] == '-' && src[i + 2u] == '>')) {
                    i++;
                }
                i = (i + 3u < len) ? i + 3u : len;
            } else {
                while (i < len && src[i] != '>') { i++; }
                if (i < len) { i++; }
            }
            continue;
        }

        /* --- a tag ---------------------------------------------------- */
        if (c == '<') {
            uint32_t t = i + 1u;
            int closing = 0;
            if (t < len && src[t] == '/') { closing = 1; t++; }

            uint32_t ns = t;
            while (t < len && !space(src[t]) && src[t] != '>' && src[t] != '/') { t++; }
            uint32_t nn = t - ns;

            /* The attributes as a SPAN, so href can be found without copying
             * them anywhere and without a second scan of the document. */
            uint32_t as = t;
            while (t < len && src[t] != '>') { t++; }
            uint32_t ae = t;
            if (t < len) { t++; }                   /* step past '>' */

            /* An unterminated tag: the document ended inside it. Everything
             * from '<' onward was markup, so there is nothing left to show. */
            if (ae >= len && ns >= len) { i = len; continue; }

            /* <script> and <style> hold source code, not prose. Skip to the
             * matching close IN THE SOURCE -- their contents contain '<' and
             * would otherwise be read as markup. */
            if (!closing && (tag_is(&src[ns], nn, "script") ||
                             tag_is(&src[ns], nn, "style"))) {
                const char *what = tag_is(&src[ns], nn, "script") ? "script" : "style";
                while (t < len) {
                    if (src[t] == '<' && t + 1u < len && src[t + 1u] == '/') {
                        uint32_t k = t + 2u, s2 = k;
                        while (k < len && !space(src[k]) && src[k] != '>') { k++; }
                        if (tag_is(&src[s2], k - s2, what)) {
                            while (k < len && src[k] != '>') { k++; }
                            t = (k < len) ? k + 1u : len;
                            break;
                        }
                    }
                    t++;
                }
                i = t;
                put_break();
                continue;
            }

            if (!closing && tag_is(&src[ns], nn, "a")) {
                /* href="..." or href='...' or href=bare */
                for (uint32_t k = as; k + 4u < ae; k++) {
                    if (lower(src[k]) != 'h' || lower(src[k + 1u]) != 'r' ||
                        lower(src[k + 2u]) != 'e' || lower(src[k + 3u]) != 'f') {
                        continue;
                    }
                    uint32_t v = k + 4u;
                    while (v < ae && space(src[v])) { v++; }
                    if (v >= ae || src[v] != '=') { continue; }
                    v++;
                    while (v < ae && space(src[v])) { v++; }
                    char quote = 0;
                    if (v < ae && (src[v] == '"' || src[v] == 0x27)) {
                        quote = src[v];
                        v++;
                    }
                    uint32_t he = v;
                    while (he < ae && (quote ? src[he] != quote : !space(src[he]))) {
                        he++;
                    }

                    if (g_nlinks < HTML_LINKS_MAX) {
                        html_link_t *L = &g_links[g_nlinks];
                        uint32_t w = 0u;
                        for (uint32_t p = v; p < he && w + 1u < HTML_HREF_MAX; p++) {
                            L->href[w++] = src[p];
                        }
                        L->href[w] = 0;
                        L->start = g_len;
                        L->end   = g_len;
                        open_link = (int)g_nlinks;
                        g_nlinks++;
                    } else {
                        g_links_dropped++;
                    }
                    break;
                }
                i = t;
                continue;
            }

            if (closing && tag_is(&src[ns], nn, "a")) {
                if (open_link >= 0) {
                    g_links[open_link].end = g_len;
                    /* A link with no text cannot be tapped and is not a link
                     * anybody can use. Drop it, so the count means what the
                     * screen shows. */
                    if (g_links[open_link].end == g_links[open_link].start &&
                        (uint32_t)open_link + 1u == g_nlinks) {
                        g_nlinks--;
                    }
                    open_link = -1;
                }
                i = t;
                continue;
            }

            if (is_break_tag(&src[ns], nn)) { put_break(); }
            else if (nn)                    { put_space(); }
            i = t;
            continue;
        }

        /* --- an entity ------------------------------------------------ */
        if (c == '&') {
            uint32_t e = i + 1u;
            while (e < len && e < i + 12u && src[e] != ';' && !space(src[e])) { e++; }
            if (e < len && src[e] == ';' && entity(&src[i + 1u], e - i - 1u)) {
                i = e + 1u;
                continue;
            }
            put_ch('&');
            i++;
            continue;
        }

        /* --- text ----------------------------------------------------- */
        if (space(c)) { put_space(); }
        else          { put_ch(c); }
        i++;
    }

    if (open_link >= 0) { g_links[open_link].end = g_len; }
    while (g_len && (g_text[g_len - 1u] == ' ' || g_text[g_len - 1u] == '\n')) {
        g_len--;
    }
    g_text[g_len] = 0;
}
