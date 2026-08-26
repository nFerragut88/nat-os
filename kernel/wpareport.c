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
    {   /* [step 258] aes_unwrap, RFC 3394 4.1 -- the one primitive on the
         * handshake path that step 240 never proved. One line, because iram
         * is 36 bytes from full and a pass/fail IS the measurement. */
        extern uint32_t g_wpat_unwrap_ok;
        uart_puts(g_wpat_unwrap_ok ? "     PASS  aes_unwrap (RFC 3394 4.1)\n"
                                   : "     FAIL  aes_unwrap (RFC 3394 4.1)\n");
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
    {   /* [step 258] step= replaces st=: which STAGE of the message-three
         * tail was last reached. 1 tx m4, 2 sent, 3 pairwise key set,
         * 4 group key set, 5 ptk_init_done, 6 auth_done. */
        extern uint32_t g_hs_step;
        uart_puts(" step=");
        uart_put_dec(g_hs_step);
    }
    uart_puts(" m1=");
    uart_put_dec(g_hs_msg1);
    uart_puts(" m3=");
    uart_put_dec(g_hs_msg3);
    uart_puts(" done=");
    uart_put_dec(g_hs_done);
    uart_puts(" micbad=");
    uart_put_dec(g_hs_mic_bad);
    /* [step 258] unwrap= subsumed by why=: nonzero why IS a failed extract,
     * and it says which of the four causes. iram paid for the difference. */
    uart_puts(" why=");
    {   /* [step 258] why, and the key-data length. */
        extern uint32_t g_hs_uw_why;
        uart_put_dec(g_hs_uw_why);
    }
    /* [step 258] txerr= dropped, 0 since it was added, and iram was four
     * bytes short. It comes back when a transmit actually fails. */
    /* [step 258] ki= dropped for step=: message three's key info has read
     * 0x13ca every run and is confirmed. Which stage stalls is not. */
    /* [step 258] poll= traded for why=: it has read 0 since step 242 and iram
     * is full. The reason extract_gtk failed is worth more than a zero. */
}
