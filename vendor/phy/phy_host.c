/* The complete host environment libphy.a asks for, minus what the ROM provides.
 * Written to find out whether the surface is really this small. */
#include <stdint.h>
#include <stddef.h>

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

/* Single core: the dual-core DPORT hazard workaround is unnecessary. */
uint32_t esp_dport_access_reg_read(uint32_t reg)
{
    return *(volatile uint32_t *)reg;
}

/* The CYD runs a 40 MHz crystal. */
int rtc_get_xtal(void) { return 40; }

int phy_printf(const char *fmt, ...) { (void)fmt; return 0; }

int temprature_sens_read(void) { return 0; }

int phy_tx_pwr_track_en = 0;
