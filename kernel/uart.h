/* nat-os — UART0 console (see uart.c for why this avoids ROM routines). */
#ifndef NATOS_UART_H
#define NATOS_UART_H

void uart_putc(char c);
void uart_puts(const char *s);
void uart_put_hex(unsigned int value);
void uart_put_dec(unsigned int value);

/* Polled receive. uart_getc_nb() returns -1 when nothing is waiting, so a
 * caller can drive a console from an ordinary task without blocking it. */
int  uart_rx_ready(void);
int  uart_getc_nb(void);

#endif /* NATOS_UART_H */
