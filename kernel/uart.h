/* cyd-os — UART0 console (see uart.c for why this avoids ROM routines). */
#ifndef CYDOS_UART_H
#define CYDOS_UART_H

void uart_putc(char c);
void uart_puts(const char *s);
void uart_put_hex(unsigned int value);
void uart_put_dec(unsigned int value);

#endif /* CYDOS_UART_H */
