/* nat-os -- WPA crypto self-test, CALL0 half: the reporting.
 * next_moves/08 step 240.
 *
 * The vectors run in vendor/windowed/wpatest.c, which cannot print: uart_puts
 * is call0 and that file is windowed. Results cross as data in globals, which
 * is the same discipline as "only the address crosses" pointed the other way.
 */

#include "uart.h"
#include <stdint.h>

extern uint32_t g_wpat_pass, g_wpat_fail, g_wpat_prf_ok;
extern uint8_t  g_wpat_got[3][32];
extern uint8_t  g_wpat_want[3][32];
extern uint32_t g_wpat_ok[3];

static void hex32(const uint8_t *p)
{
    static const char h[] = "0123456789abcdef";
    for (uint32_t i = 0u; i < 32u; i++) {
        uart_putc(h[(p[i] >> 4) & 15]);
        uart_putc(h[p[i] & 15]);
    }
}

void wpa_selftest_report(void);
void wpa_selftest_report(void)
{
    uart_puts("   wpa       crypto self-test (IEEE 802.11i vectors)\n");
    for (uint32_t i = 0u; i < 3u; i++) {
        uart_puts(g_wpat_ok[i] ? "     PASS  pbkdf2 #" : "     FAIL  pbkdf2 #");
        uart_put_dec(i);
        if (!g_wpat_ok[i]) {
            uart_puts("\n            got  "); hex32(g_wpat_got[i]);
            uart_puts("\n            want "); hex32(g_wpat_want[i]);
        }
        uart_puts("\n");
    }
    uart_puts(g_wpat_prf_ok ? "     ok    sha1_prf (weak check: nonzero, label-sensitive)\n"
                            : "     FAIL  sha1_prf\n");
    uart_puts("   wpa       ");
    uart_put_dec(g_wpat_pass);
    uart_puts(" passed, ");
    uart_put_dec(g_wpat_fail);
    uart_puts(g_wpat_fail ? " FAILED\n" : " failed\n");
}
