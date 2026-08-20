/* Feasibility stubs for next_moves/08.
 *
 * Purpose is ONLY to answer "does the vendor transmit path link against a
 * nat-os-shaped host". These are not implementations and several are wrong on
 * purpose -- see the notes. Real versions belong in the kernel if 08 proceeds.
 */

#include <stddef.h>

/* --- trivial: nat-os already has equivalents -------------------------- */
void  free(void *p)                     { (void)p; }
int   puts(const char *s)               { (void)s; return 0; }

char *strtok(char *s, const char *delim)
{
    static char *save;
    if (!s) { s = save; }
    if (!s) { return NULL; }
    while (*s) {
        const char *d = delim;
        int hit = 0;
        while (*d) { if (*s == *d) { hit = 1; break; } d++; }
        if (!hit) { break; }
        s++;
    }
    if (!*s) { save = NULL; return NULL; }
    char *tok = s;
    while (*s) {
        const char *d = delim;
        while (*d) { if (*s == *d) { *s = 0; save = s + 1; return tok; } d++; }
        s++;
    }
    save = NULL;
    return tok;
}

/* hex string -> bytes. Used by net80211 for WPA material. */
int hexstr2bin(const char *hex, unsigned char *buf, size_t len);
int hexstr2bin(const char *hex, unsigned char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        int v = 0;
        for (int n = 0; n < 2; n++) {
            char c = hex[i * 2 + n];
            int d;
            if      (c >= '0' && c <= '9') { d = c - '0'; }
            else if (c >= 'a' && c <= 'f') { d = c - 'a' + 10; }
            else if (c >= 'A' && c <= 'F') { d = c - 'A' + 10; }
            else { return -1; }
            v = (v << 4) | d;
        }
        buf[i] = (unsigned char)v;
    }
    return 0;
}

/* --- diagnostics: discarded here, would go to UART -------------------- */
int net80211_printf(const char *fmt, ...);
int net80211_printf(const char *fmt, ...) { (void)fmt; return 0; }
int mesh_printf(const char *fmt, ...);
int mesh_printf(const char *fmt, ...)     { (void)fmt; return 0; }

/* --- the event system, which is the only genuinely new surface --------
 *
 * net80211 reports asynchronously through esp_event. A direct
 * esp_wifi_80211_tx() call does not need a callback to come back, so stubs
 * that accept a registration and never deliver are enough to LINK and enough
 * to transmit -- but they are NOT enough to associate, scan, or receive, all
 * of which are event-driven. That distinction is the real scope of 08 and is
 * invisible in the symbol count. */
int WIFI_EVENT;      /* esp_event declares this as an event-base object */

int esp_event_handler_register(const char *base, int id, void *h, void *arg);
int esp_event_handler_register(const char *base, int id, void *h, void *arg)
{
    (void)base; (void)id; (void)h; (void)arg;
    return 0;        /* "registered" -- and never called back */
}

int esp_event_handler_unregister(const char *base, int id, void *h);
int esp_event_handler_unregister(const char *base, int id, void *h)
{
    (void)base; (void)id; (void)h;
    return 0;
}

int esp_mesh_send_event_internal(int id, void *data, size_t len);
int esp_mesh_send_event_internal(int id, void *data, size_t len)
{
    (void)id; (void)data; (void)len;
    return 0;
}
