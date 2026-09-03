/*
 * uart.h - console driver over the raw HAL byte stream.
 *
 * Assembles incoming bytes into lines and provides a small formatter for
 * outgoing text. The formatter is deliberately hand written rather than
 * newlib printf: pulling in the C library's version costs several kilobytes
 * of flash and drags in malloc, neither of which is welcome on a part whose
 * entire job is a management console.
 */
#ifndef OPTILINK_UART_H
#define OPTILINK_UART_H

#include <stdbool.h>
#include <stdint.h>

/* Sized so a full oversize Ethernet frame can be pushed in as hex from the
 * console, which is what the frame parser tests need in order to exercise
 * the reject path. It is the single largest RAM object in the firmware, and
 * it exists for the debug console alone. */
#define UART_LINE_MAX 3200u

void uart_init(uint32_t baud);

/* Drains whatever the HAL has buffered into the line assembler. Call from
 * the console task. */
void uart_poll(void);

bool        uart_line_ready(void);
const char *uart_line(void);
void        uart_consume_line(void);

void uart_puts(const char *s);

/* Supports %c %s %d %u %x, an optional zero flag and a one or two digit
 * width, for example %02x and %5u. Everything else is emitted literally. */
void uart_printf(const char *fmt, ...);

uint32_t uart_lines_dropped(void);

#endif /* OPTILINK_UART_H */
