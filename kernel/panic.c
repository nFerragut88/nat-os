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

    uart_puts("\n  halted. reset the board to continue.\n");

    for (;;) {
        /* Deliberately spin rather than reset: a reset loop would scroll the
         * evidence off the terminal. */
    }
}
