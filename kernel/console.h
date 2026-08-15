/* cyd-os — console arbitration.
 *
 * UART0 has several writers: the reporter task, the shell, and the application
 * host announcing terminations. Without arbitration a report lands in the
 * middle of a typed line, which is cosmetic but makes the console unusable for
 * anything requiring attention.
 *
 * Locking is at MESSAGE granularity, not per character. uart_putc() stays
 * lock-free, and a writer takes the console around a whole line or block. Two
 * reasons:
 *
 *   - A per-character lock would be acquired hundreds of times per line for no
 *     benefit; interleaving happens between messages, not within a byte.
 *   - The panic handler prints. It runs after a fault, possibly with the lock
 *     already held by the task that faulted, and must never block or wait. It
 *     therefore ignores this layer entirely and writes through uart_putc()
 *     directly. A panic that deadlocks trying to report a panic is the worst
 *     possible failure mode for a kernel with no debugger.
 */

#ifndef CYDOS_CONSOLE_H
#define CYDOS_CONSOLE_H

void console_init(void);

/* Recursive, so a locked block may call a helper that also locks. */
void console_lock(void);
void console_unlock(void);

unsigned int console_contentions(void);
int console_owner(void);

#endif /* CYDOS_CONSOLE_H */
