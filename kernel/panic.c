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
#include "flash.h"

/* Shared ending for both entry points. Spins rather than resetting so the
 * evidence stays on the terminal.
 *
 * The watchdog is disarmed first, and that is a deliberate reversal. It is armed
 * to recover a system that has stopped making progress, and a halted panic looks
 * exactly like one — so without this, the board resets a few seconds in and
 * scrolls away the report this handler exists to produce. A hang the kernel
 * cannot explain should be recovered automatically; a fault it CAN explain
 * should be left on screen for someone to read. */
static int g_record_rc = -99;

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
