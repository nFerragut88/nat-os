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

/* [step 242] The handshake's own counters, printed from call0. The windowed
 * side cannot print; these say which message arrived and where it stopped. */
extern uint32_t g_hs_have_pmk, g_hs_state, g_hs_msg1, g_hs_msg3, g_hs_done;
extern uint32_t g_hs_mic_bad, g_hs_unwrap_bad, g_hs_tx_err, g_hs_last_keyinfo;
extern uint32_t g_hs_in4way_calls;

void wpa_hs_report(void);
void wpa_hs_report(void)
{
    uart_puts("  4way pmk=");
    uart_put_dec(g_hs_have_pmk);
    uart_puts(" st=");
    uart_put_dec(g_hs_state);
    uart_puts(" m1=");
    uart_put_dec(g_hs_msg1);
    uart_puts(" m3=");
    uart_put_dec(g_hs_msg3);
    uart_puts(" done=");
    uart_put_dec(g_hs_done);
    uart_puts(" micbad=");
    uart_put_dec(g_hs_mic_bad);
    uart_puts(" unwrap=");
    uart_put_dec(g_hs_unwrap_bad);
    uart_puts(" txerr=");
    uart_put_dec(g_hs_tx_err);
    uart_puts(" ki=");
    uart_put_hex(g_hs_last_keyinfo);
    uart_puts(" poll=");
    uart_put_dec(g_hs_in4way_calls);
}
