/* The host environment libphy.a asks for. Compiled -mabi=windowed.
 *
 * These are called BY the blob, which is windowed, so they must be windowed
 * too: a windowed caller issues CALL8, and a call0 callee neither executes
 * ENTRY nor returns with RETW, so the window would rotate forward and never
 * come back. The ABI has to match on both sides of every edge.
 *
 * For the same reason nothing here calls into the kernel. phy_printf() formats
 * into a static buffer that call0 code reads afterwards, rather than calling
 * uart_puts() — which would be exactly the broken edge described above.
 *
 * Measured surface: 16 external symbols, 10 already in ROM. These are the six
 * that remain. See vendor/phy/README.md.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

static uint32_t g_crit_saved;

void phy_enter_critical(void)
{
    uint32_t ps;
    __asm__ volatile ("rsil %0, 3" : "=a"(ps));
    g_crit_saved = ps;
}

void phy_exit_critical(void)
{
    __asm__ volatile ("wsr.ps %0; rsync" :: "a"(g_crit_saved));
}

/* Single core, so the dual-core DPORT read hazard does not apply. */
uint32_t esp_dport_access_reg_read(uint32_t reg)
{
    return *(volatile uint32_t *)reg;
}

int rtc_get_xtal(void) { return 40; }          /* the CYD's crystal, in MHz */

int temprature_sens_read(void) { return 0; }   /* spelling is Espressif's */

int phy_tx_pwr_track_en = 0;

/* ---- captured output ----------------------------------------------------
 *
 * The blob's own printf. Deliberately self-contained: no kernel calls, no
 * ROM calls, nothing that crosses an ABI boundary while the blob owns the
 * window. Just bytes into .bss for call0 code to collect later.
 */
#define PHY_LOG_MAX 512
static char     g_log[PHY_LOG_MAX];
static uint32_t g_log_len;

static void put(char c)
{
    if (g_log_len + 1u < PHY_LOG_MAX) {
        g_log[g_log_len++] = c;
        g_log[g_log_len]   = 0;
    }
}

static void put_dec(int v)
{
    char t[12];
    int n = 0;
    unsigned u = (v < 0) ? (put('-'), (unsigned)(-v)) : (unsigned)v;
    if (u == 0u) { put('0'); return; }
    while (u) { t[n++] = (char)('0' + (u % 10u)); u /= 10u; }
    while (n) { put(t[--n]); }
}

int phy_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (const char *p = fmt; p && *p; p++) {
        if (*p != '%') { put(*p); continue; }
        p++;
        switch (*p) {
        case 'd': put_dec(va_arg(ap, int)); break;
        case 's': { const char *s = va_arg(ap, const char *);
                    while (s && *s) { put(*s++); } } break;
        case 'c': put((char)va_arg(ap, int)); break;
        case '%': put('%'); break;
        default:  put('%'); if (*p) { put(*p); } break;
        }
    }
    va_end(ap);
    return 0;
}

/* Read by the kernel, which is call0 — a plain data pointer crosses the ABI
 * boundary safely where a call would not. */
const char *phy_host_log(void)   { return g_log; }
void        phy_host_log_clear(void) { g_log_len = 0; g_log[0] = 0; }
