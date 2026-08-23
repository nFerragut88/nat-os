/* nat-os -- install the ROM newlib syscall stub table.
 *
 * [step 186] The table itself is vendor/windowed/rom_stubs_w.c and MUST be
 * windowed: ROM calls its entries with CALL8. This file only checks it and
 * publishes its address, which is call0 work -- two stores to fixed addresses.
 *
 * See rom_stubs_w.c for what the table is and why it was missing.
 */

#include <stdint.h>

#include "uart.h"

/* Opaque: the struct is declared on the windowed side and only its address and
 * length are needed here. Same treatment as g_wifi_osi_funcs in
 * wifi_init_cfg.c, and for the same reason. */
extern char     g_rom_stubs;
extern const uint32_t g_rom_stub_words;   /* data, not a call: see rom_stubs_w.c */

/* ROM reads these two words to find the table. esp32.rom.ld names them
 * syscall_table_ptr_pro and _app; nat-os does not link that script's data
 * symbols, so the addresses are written out. */
#define SYSCALL_TABLE_PTR_APP  0x3FFAE020u
#define SYSCALL_TABLE_PTR_PRO  0x3FFAE024u

static int g_installed;

/* Reports, and refuses to install a table with a hole in it.
 *
 * The same guard as the OS adapter's (step 185, wifi_init_cfg.c) and for the
 * same reason: a designated initializer zero-fills what it omits, ROM does not
 * check before calling, and the failure is a jump to address zero a long way
 * from the mistake. That is precisely how the adapter's two missing entries
 * presented, and this table has thirty-six chances to repeat it. */
void rom_stubs_init(void);
void rom_stubs_init(void)
{
    if (g_installed) {
        return;
    }

    const uint32_t *w = (const uint32_t *)&g_rom_stubs;
    uint32_t n   = g_rom_stub_words;
    uint32_t bad = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (w[i] == 0u) { bad++; }
    }

    uart_puts("   rom stubs : ");
    uart_put_dec(n);
    uart_puts(" entries, ");
    if (bad) {
        uart_put_dec(bad);
        uart_puts(" NULL -- NOT INSTALLED, ROM libc left unsafe\n");
        return;
    }
    uart_puts("installed\n");

    *(volatile uint32_t *)SYSCALL_TABLE_PTR_PRO = (uint32_t)&g_rom_stubs;
    *(volatile uint32_t *)SYSCALL_TABLE_PTR_APP = (uint32_t)&g_rom_stubs;
    g_installed = 1;
}
