/* cyd-os — raw UART0 output.
 *
 * Register-level, no ROM calls (ROM routines are windowed-ABI and this kernel
 * is call0). The ROM bootloader already configured UART0 at 115200 8N1 for its
 * own boot messages, so Milestone 0 inherits that and only pushes bytes into
 * the TX FIFO. Baud/pin configuration comes later, when the kernel stops
 * relying on the ROM having been there first.
 *
 * Until JTAG arrives this is the only window into a running kernel, so it is
 * deliberately dependency-free: it works before the heap, before the
 * scheduler, and inside an exception handler.
 */

#include "uart.h"

#define UART0_BASE        0x3FF40000u
#define UART_FIFO_REG     (UART0_BASE + 0x00)  /* write: push a byte to TX   */
#define UART_STATUS_REG   (UART0_BASE + 0x1C)  /* bits 16..23: TX FIFO count */

#define UART_TXFIFO_CNT_SHIFT  16
#define UART_TXFIFO_CNT_MASK   0xFFu
#define UART_TXFIFO_DEPTH      127u

#define REG(addr) (*(volatile unsigned int *)(addr))

static unsigned int tx_fifo_used(void)
{
    return (REG(UART_STATUS_REG) >> UART_TXFIFO_CNT_SHIFT) & UART_TXFIFO_CNT_MASK;
}

void uart_putc(char c)
{
    /* Spin rather than drop. Boot output that silently truncates is worse than
     * slow boot output — a lost line looks identical to a crash. */
    while (tx_fifo_used() >= UART_TXFIFO_DEPTH) {
        /* busy wait */
    }
    REG(UART_FIFO_REG) = (unsigned int)(unsigned char)c;
}

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            uart_putc('\r');  /* terminals expect CRLF */
        }
        uart_putc(*s++);
    }
}

void uart_put_hex(unsigned int value)
{
    static const char digits[] = "0123456789abcdef";
    uart_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        uart_putc(digits[(value >> shift) & 0xF]);
    }
}

void uart_put_dec(unsigned int value)
{
    char buf[11];
    int i = 0;
    if (value == 0) {
        uart_putc('0');
        return;
    }
    while (value > 0 && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (i-- > 0) {
        uart_putc(buf[i]);
    }
}
