/* nat-os — the HTML reader's self-test, on the board.
 *
 * WHY IT IS ON THE BOARD. The one path that would exercise this against a real
 * page needs the radio, and on this supply the radio takes the USB link with it
 * (step 376, and again at 388d). A renderer that has only ever been reasoned
 * about is exactly the kind of thing UM-NATOS-059 §7 says goes wrong -- so this
 * runs the REAL html_render() over known inputs on the REAL target, needing no
 * network at all, and says what it got when it disagrees.
 *
 * It is permanent for the same reason `vmargtest` and `wincollide` are: a test
 * that ran once and was deleted is a claim, not a check.
 */

#include "html.h"
#include "uart.h"
#include <stdint.h>

static int streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static void put_num(uint32_t v)
{
    char b[12];
    int n = 0;
    if (!v) { b[n++] = '0'; }
    while (v) { b[n++] = (char)('0' + v % 10u); v /= 10u; }
    while (n) { uart_putc(b[--n]); }
}

/* A rendered buffer is not NUL-safe to hand to uart_puts when it holds a
 * newline, and the newlines are exactly what is being checked -- so they are
 * shown as \n rather than printed as line breaks. */
static void show(const char *s, uint32_t n)
{
    uart_putc('"');
    for (uint32_t i = 0u; i < n; i++) {
        if (s[i] == '\n') { uart_puts("\\n"); }
        else              { uart_putc(s[i]); }
    }
    uart_putc('"');
}

static uint32_t g_pass, g_fail;

static void expect(const char *name, const char *src, const char *want)
{
    uint32_t n = 0u;
    while (src[n]) { n++; }
    html_render(src, n);

    if (streq(html_text(), want)) {
        g_pass++;
        return;
    }
    g_fail++;
    uart_puts("   FAIL ");
    uart_puts(name);
    uart_puts("\n        wanted ");
    uint32_t wn = 0u;
    while (want[wn]) { wn++; }
    show(want, wn);
    uart_puts("\n        got    ");
    show(html_text(), html_len());
    uart_puts("\n");
}

static void expect_link(const char *name, uint32_t idx,
                        const char *href, const char *text)
{
    const html_link_t *L = html_link(idx);
    if (!L) {
        g_fail++;
        uart_puts("   FAIL ");
        uart_puts(name);
        uart_puts(" -- no link ");
        put_num(idx);
        uart_puts(", count=");
        put_num(html_link_count());
        uart_puts("\n");
        return;
    }
    int ok = streq(L->href, href);
    uint32_t tn = 0u;
    while (text[tn]) { tn++; }
    if (ok && (L->end - L->start) == tn) {
        for (uint32_t i = 0u; i < tn; i++) {
            if (html_text()[L->start + i] != text[i]) { ok = 0; break; }
        }
    } else if (ok) {
        ok = 0;
    }
    if (ok) { g_pass++; return; }
    g_fail++;
    uart_puts("   FAIL ");
    uart_puts(name);
    uart_puts(" link ");
    put_num(idx);
    uart_puts("\n        wanted href=");
    uart_puts(href);
    uart_puts(" text=");
    show(text, tn);
    uart_puts("\n        got    href=");
    uart_puts(L->href);
    uart_puts(" text=");
    show(&html_text()[L->start], L->end - L->start);
    uart_puts("\n");
}

/* example.com's actual response, shortened -- and written the way webfetch
 * DELIVERS it, with every CR already mapped to a space (webfetch.c, on_recv).
 * A sample containing real CRs would be testing a shape this code never
 * receives, which is the mistake step 387 made in a different place. */
static const char SAMPLE[] =
    "HTTP/1.1 200 OK \n"
    "Content-Type: text/html; charset=UTF-8 \n"
    "Content-Length: 1256 \n"
    " \n"
    "<!doctype html>\n"
    "<html><head>\n"
    "<title>Example Domain</title>\n"
    "<style>body { margin: 0; } h1 > p { color: #fff; }</style>\n"
    "</head>\n"
    "<body>\n"
    "<div>\n"
    "    <h1>Example Domain</h1>\n"
    "    <p>This domain is for use in illustrative examples &amp; documents.</p>\n"
    "    <p><a href=\"https://www.iana.org/domains/example\">More information...</a></p>\n"
    "</div>\n"
    "<script>var x = 1 < 2 && 3 > 2;</script>\n"
    "</body></html>\n";

void html_selftest(void);
void html_selftest(void)
{
    g_pass = 0u;
    g_fail = 0u;

    /* --- the pieces, one property each ------------------------------- */
    expect("tags removed", "<p>hello</p>", "hello");
    expect("headers skipped", "HTTP/1.1 200 OK \n"
                              "X: y \n \n<p>body</p>", "body");
    expect("no headers, raw html", "<b>bare</b>", "bare");
    expect("entities", "<p>a &amp; b &lt;c&gt; &quot;d&quot;</p>",
           "a & b <c> \"d\"");
    expect("unknown entity kept", "<p>2 &sup2; 3</p>", "2 &sup2; 3");
    expect("script discarded", "<p>a</p><script>var x = 1 < 2;</script><p>b</p>",
           "a\nb");
    expect("style discarded", "<style>p { color: red }</style><p>a</p>", "a");
    /* "ab", not "a\nb": the comment vanishes and nothing between the two
     * letters is a block tag, so there is no break to make. The first version
     * of this case expected "a\nb"; the board said otherwise, and the board was
     * right. The test was wrong, not the reader. */
    expect("comment discarded", "<p>a<!-- not <b>this</b> -->b</p>", "ab");
    expect("doctype discarded", "<!doctype html><p>a</p>", "a");
    expect("whitespace collapsed", "<p>a   \n\t  b</p>", "a b");
    expect("breaks between blocks", "<p>one</p><p>two</p><p>three</p>",
           "one\ntwo\nthree");
    expect("br breaks", "a<br>b", "a\nb");
    expect("inline does not break", "a <b>bold</b> c", "a bold c");
    expect("unterminated tag", "<p>text<span class=", "text");
    expect("empty input", "", "");
    expect("tags only", "<html><head></head><body></body></html>", "");

    /* --- links -------------------------------------------------------- */
    expect("link text shown", "<a href=\"/a\">click</a>", "click");
    expect_link("link text shown", 0u, "/a", "click");

    expect("single quoted href", "<a href='/b'>x</a>", "x");
    expect_link("single quoted href", 0u, "/b", "x");

    expect("bare href", "<a href=/c >x</a>", "x");
    expect_link("bare href", 0u, "/c", "x");

    expect("href among attributes",
           "<a class=\"q\" href=\"/d\" rel=\"nofollow\">x</a>", "x");
    expect_link("href among attributes", 0u, "/d", "x");

    expect("two links", "<a href=\"/1\">one</a> and <a href=\"/2\">two</a>",
           "one and two");
    expect_link("two links", 0u, "/1", "one");
    expect_link("two links", 1u, "/2", "two");

    /* An anchor with no text is not tappable, so it must not be counted --
     * otherwise the link count promises something the screen cannot offer. */
    expect("empty anchor dropped", "<a href=\"/x\"></a>text", "text");
    if (html_link_count() == 0u) { g_pass++; }
    else {
        g_fail++;
        uart_puts("   FAIL empty anchor dropped -- count=");
        put_num(html_link_count());
        uart_puts("\n");
    }

    /* --- the whole thing ---------------------------------------------- */
    expect("example.com", SAMPLE,
           "Example Domain\n"
           "Example Domain\n"
           "This domain is for use in illustrative examples & documents.\n"
           "More information...");
    expect_link("example.com", 0u,
                "https://www.iana.org/domains/example", "More information...");

    uart_puts("   html     : ");
    if (g_fail) {
        put_num(g_fail);
        uart_puts(" FAILED, ");
        put_num(g_pass);
        uart_puts(" passed\n");
    } else {
        uart_puts("PASS  ");
        put_num(g_pass);
        uart_puts(" checks -- tags, entities, script/style, breaks, links\n");
    }
}
