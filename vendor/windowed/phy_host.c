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

/* NESTING-SAFE, and it has to be.
 *
 * The first version kept a single saved PS. register_chipv7_phy() nests these,
 * so the inner enter overwrote the outer's saved value and the final exit
 * restored the MASKED level instead of the original. Interrupts stayed off, the
 * tick stopped, no task switched, and the hang detector reset the board exactly
 * 3000 ms later -- rst:0x7 TG0WDT_SYS_RESET.
 *
 * The PHY returned 0 through all of that. The bring-up had SUCCEEDED; the
 * reset came from this shim afterwards, which is a good illustration of how far
 * a wrong answer can travel when the thing it breaks is somewhere else.
 *
 * Only the outermost pair touches PS. Inner pairs raise the level again, which
 * is already raised, and change nothing. */
static uint32_t g_crit_saved;
static int      g_crit_depth;

void phy_enter_critical(void)
{
    uint32_t ps;
    __asm__ volatile ("rsil %0, 3" : "=a"(ps));
    if (g_crit_depth++ == 0) {
        g_crit_saved = ps;
    }
}

void phy_exit_critical(void)
{
    if (g_crit_depth > 0 && --g_crit_depth == 0) {
        /* Interrupt level only, for the reason given in wifi_osi_stubs.c's
         * osi_s_wifi_int_restore: the rest of PS is the kernel's execution
         * mode, and restoring a whole saved word can put WOE back as it was at
         * capture time rather than as it must be now. */
        uint32_t ps;
        __asm__ volatile ("rsr.ps %0" : "=a"(ps));
        ps = (ps & ~0xFu) | (g_crit_saved & 0xFu);
        __asm__ volatile ("wsr.ps %0; rsync" :: "a"(ps));
    }
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
char     phy_host_log_buf[PHY_LOG_MAX];
uint32_t phy_host_log_len;

static void put(char c)
{
    if (phy_host_log_len + 1u < PHY_LOG_MAX) {
        phy_host_log_buf[phy_host_log_len++] = c;
        phy_host_log_buf[phy_host_log_len]   = 0;
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

/* Exported as DATA, not as accessor functions, and that distinction is the
 * whole point.
 *
 * The first version exported phy_host_log() and phy_host_log_clear() and the
 * kernel called them. Both live in this file, so both are WINDOWED, and the
 * kernel is call0: the callee executed ENTRY without the caller having rotated
 * the window, then returned with RETW into a frame that was never established.
 * It fell off the end of the function into padding and panicked with
 * IllegalInstruction at 0x4008a810.
 *
 * The comment at the top of this file describes exactly that hazard, in the
 * other direction, and I walked into it anyway. A symbol is not the problem —
 * a CALL is. Reading a buffer needs no calling convention at all. */

/* ---- renamed symbols the blob needs ------------------------------------
 *
 * libphy.a originally referenced memcpy, sprintf and five libgcc soft-float
 * helpers. Those were renamed in the archive with objcopy, for one reason:
 *
 *   memcpy = 0x4000c0bc;                    <- ROM script, STRONG assignment
 *
 * Linking Espressif's newlib/libgcc ROM scripts to satisfy them redirected the
 * KERNEL's own call0 memcpy to that windowed ROM routine and panicked the board
 * on boot. PROVIDE would have deferred to ours; a bare assignment does not.
 *
 * So the blob's references are renamed instead and answered here, in windowed
 * code, where they belong. Nothing the kernel defines can be displaced, because
 * no script defines anything any more: the only one linked is esp32.rom.ld,
 * every entry of which is PROVIDE.
 *
 * The float helpers are ROM routines reached through function pointers rather
 * than through a linker script. Same code, no symbol table to collide with. */
#define ROM_EXTENDSFDF2  0x40002C34u
#define ROM_DIVDF3       0x40002954u
#define ROM_TRUNCDFSF2   0x40002B90u
#define ROM_DIVDI3       0x4000CA84u
#define ROM_FLOATUNSIDF  0x4000C938u

typedef double   (*fn_ext)(float);
typedef double   (*fn_dd)(double, double);
typedef float    (*fn_trunc)(double);
typedef long long (*fn_lldiv)(long long, long long);
typedef double   (*fn_uf)(unsigned int);

double phy_divdf3(double a, double b)
{
    return ((fn_dd)ROM_DIVDF3)(a, b);
}

float phy_truncdfsf2(double a)
{
    return ((fn_trunc)ROM_TRUNCDFSF2)(a);
}

long long phy_divdi3(long long a, long long b)
{
    return ((fn_lldiv)ROM_DIVDI3)(a, b);
}

double phy_floatunsidf(unsigned int a)
{
    return ((fn_uf)ROM_FLOATUNSIDF)(a);
}

/* Not in ROM, so it is composed from ones that are: widen, divide, narrow. */
float phy_divsf3(float a, float b)
{
    double da = ((fn_ext)ROM_EXTENDSFDF2)(a);
    double db = ((fn_ext)ROM_EXTENDSFDF2)(b);
    return ((fn_trunc)ROM_TRUNCDFSF2)(((fn_dd)ROM_DIVDF3)(da, db));
}

void *phy_memcpy(void *dst, const void *src, unsigned int n)
{
    unsigned char       *d = dst;
    const unsigned char *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

/* Enough of sprintf for what the blob does with it — version strings. */
int phy_sprintf(char *out, const char *fmt, ...)
{
    va_list ap;
    char *o = out;
    va_start(ap, fmt);
    for (const char *p = fmt; p && *p; p++) {
        if (*p != '%') { *o++ = *p; continue; }
        p++;
        if (*p == 'd') {
            int v = va_arg(ap, int);
            char t[12]; int n = 0;
            unsigned u = (v < 0) ? (*o++ = '-', (unsigned)(-v)) : (unsigned)v;
            if (!u) { *o++ = '0'; }
            while (u) { t[n++] = (char)('0' + (u % 10u)); u /= 10u; }
            while (n) { *o++ = t[--n]; }
        } else if (*p == 's') {
            const char *sv = va_arg(ap, const char *);
            while (sv && *sv) { *o++ = *sv++; }
        } else if (*p == 'c') {
            *o++ = (char)va_arg(ap, int);
        } else if (*p) {
            *o++ = *p;
        }
    }
    va_end(ap);
    *o = 0;
    return (int)(o - out);
}
