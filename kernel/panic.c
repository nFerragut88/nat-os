/* cyd-os — fault reporting.
 *
 * Entered from the exception vectors when something the kernel did not plan
 * for happens. Until a JTAG probe is available this is the only way a fault
 * says anything at all; without it, a bad pointer or illegal instruction
 * produces a silent reset and no evidence.
 *
 * Runs on a dedicated stack (see vectors.S) because the faulting stack may be
 * the reason we are here. Uses only uart_*, which touch nothing but hardware
 * registers and are safe in this state.
 *
 * Never returns.
 */

#include "panic.h"
#include "uart.h"
#include "watchdog.h"
#include "store.h"
#include "display.h"
#include "flash.h"

static int g_record_rc = -99;

/* What to put on the panel, filled in by whichever entry point ran. Statics
 * rather than parameters because halt_forever() is shared and the two entry
 * points describe a fault differently. */
static const char *g_panic_what = "unknown";
static unsigned int g_panic_a, g_panic_b;
static int g_panic_has_b;

/* Puts the fault on the panel.
 *
 * The device is standalone. Everything else in this file assumes a serial cable
 * that is usually not attached, and UM-CYDOS-018's record only answers the
 * question on the NEXT boot. Between the fault and that reboot the user is
 * looking at a frozen screen with no indication anything is wrong — which is
 * indistinguishable from the renderer having simply stopped.
 *
 * Drawn AFTER the UART report, deliberately. The panel is the more elaborate of
 * the two paths: it needs the SPI controller, the flash-mapped font, and a
 * peripheral whose state nobody has verified. If drawing wedges despite the
 * bounds in the driver, the serial report and the flash record have both
 * already happened, and only the least reliable of the three is lost.
 *
 * See below the helper. */

/* Eight hex digits into a caller-supplied buffer. Local to this file because
 * the panic path must not depend on anything it does not have to: no printf,
 * no heap, no shared scratch buffer that something else might be using. */
static void hex8(char *out, unsigned int v)
{
    static const char DIGITS[] = "0123456789abcdef";
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < 8; i++) {
        out[2 + i] = DIGITS[(v >> ((7 - i) * 4)) & 0xFu];
    }
    out[10] = 0;
}

static void panic_screen(void)
{
    char buf[12];

    if (!display_ready()) {
        return;
    }
    display_enter_panic_mode();

    display_clear(COLOR_BLUE);
    display_fill_rect(0, 0, 320, 20, COLOR_WHITE);
    display_text(6, 6, "KERNEL PANIC", COLOR_BLUE, COLOR_WHITE, 1);

    display_text(6, 36, g_panic_what, COLOR_WHITE, COLOR_BLUE, 1);

    /* Numbers as hex without labels: at this size a label costs more width
     * than it buys, and the two values are positional in the same order the
     * UART report prints them. */
    display_text(6, 56, "cause", COLOR_YELLOW, COLOR_BLUE, 1);
    hex8(buf, g_panic_a);
    display_text(70, 56, buf, COLOR_WHITE, COLOR_BLUE, 1);
    if (g_panic_has_b) {
        display_text(6, 72, "pc", COLOR_YELLOW, COLOR_BLUE, 1);
        hex8(buf, g_panic_b);
        display_text(70, 72, buf, COLOR_WHITE, COLOR_BLUE, 1);
    }

    display_text(6, 104, "halted - reset the board", COLOR_WHITE, COLOR_BLUE, 1);
    display_text(6, 120, "reason is saved; next boot", COLOR_GREY, COLOR_BLUE, 1);
    display_text(6, 136, "will report it over serial", COLOR_GREY, COLOR_BLUE, 1);
}

/* Shared ending for both entry points. Spins rather than resetting so the
 * evidence stays put.
 *
 * The watchdog is disarmed first, and that is a deliberate reversal. It is armed
 * to recover a system that has stopped making progress, and a halted panic looks
 * exactly like one — so without this, the board resets a few seconds in and
 * scrolls away the report this handler exists to produce. A hang the kernel
 * cannot explain should be recovered automatically; a fault it CAN explain
 * should be left for someone to read.
 *
 * Order is by decreasing reliability: flash record (already written by the
 * caller), then UART, then the panel. Each step can only cost the steps after
 * it. */
static void halt_forever(void)
{
    watchdog_disarm();

#if FLASH_ENABLE
    /* The record was already written by the caller. Confirm it on the terminal
     * so the two reports can be compared: if the next boot disagrees with what
     * was printed here, the persistence path is what is wrong, not the fault. */
    uart_puts(g_record_rc == 0 ? "  recorded : yes, the next boot will report this\n"
                               : "  recorded : NO — the fault will be lost\n");
#endif

    uart_puts("\n  halted. reset the board to continue.\n");

    /* Report how many bytes the panel actually took.
     *
     * Nobody can query a halted board, and "the screen looks wrong" and "the
     * driver never ran" are indistinguishable by eye. A byte count crossing the
     * wire separates them: roughly 150 KB means a full repaint happened and the
     * question is what was drawn; a number near zero means the panic never
     * reached the panel at all. */
    uint32_t before = display_bytes_written();
    panic_screen();
    uart_puts("  panel    : ");
    uart_put_dec(display_bytes_written() - before);
    uart_puts(" bytes drawn\n");

    for (;;) {
    }
}

void kernel_panic_msg(const char *why, unsigned int detail)
{
#if FLASH_ENABLE
    /* Written BEFORE anything is printed. A handler that reports first and
     * records second loses the record if it dies while reporting, and that is
     * not far-fetched — the UART is the more complicated of the two paths and
     * the system is already in an unknown state. */
    g_record_rc = store_record_fault(STORE_FAULT_GUARD, detail, 0);
#endif

    g_panic_what  = why;
    g_panic_a     = detail;
    g_panic_has_b = 0;

    uart_puts("\n\n*** KERNEL PANIC ***\n\n");
    uart_puts("  reason   : ");
    uart_puts(why);
    uart_puts("\n  detail   : ");
    uart_put_dec(detail);
    uart_puts("\n");
    halt_forever();
}

/* Xtensa EXCCAUSE values worth naming. The rest print as a bare number rather
 * than carrying a table that would be mostly dead weight. */
static const char *cause_name(unsigned int cause)
{
    switch (cause) {
    case 0:  return "IllegalInstruction";
    case 1:  return "Syscall";
    case 2:  return "InstructionFetchError";
    case 3:  return "LoadStoreError";
    case 4:  return "Level1Interrupt";
    case 5:  return "Alloca";
    case 6:  return "IntegerDivideByZero";
    case 8:  return "Privileged";
    case 9:  return "LoadStoreAlignment";
    case 20: return "InstFetchProhibited";
    case 28: return "LoadProhibited";
    case 29: return "StoreProhibited";
    default: return "unknown";
    }
}

void kernel_panic(unsigned int exccause, unsigned int epc, unsigned int ps)
{
#if FLASH_ENABLE
    /* Recorded first, for the reason given in kernel_panic_msg(). */
    g_record_rc = store_record_fault(STORE_FAULT_EXCEPTION, exccause, epc);
#endif

    g_panic_what  = cause_name(exccause);
    g_panic_a     = exccause;
    g_panic_b     = epc;
    g_panic_has_b = 1;

    uart_puts("\n\n*** KERNEL PANIC ***\n");

    uart_puts("  exccause : ");
    uart_put_dec(exccause);
    uart_puts("  (");
    uart_puts(cause_name(exccause));
    uart_puts(")\n");

    uart_puts("  epc      : ");
    uart_put_hex(epc);
    uart_puts("   <- faulting instruction\n");

    uart_puts("  ps       : ");
    uart_put_hex(ps);
    uart_puts("\n");

    halt_forever();
}
