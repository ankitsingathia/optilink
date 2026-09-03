#include "optilink/uart.h"

#include <stdarg.h>
#include <string.h>

#include "optilink/hal.h"

static char     s_line[UART_LINE_MAX];
static uint16_t s_len;
static bool     s_ready;
static bool     s_overflowed;
static uint32_t s_dropped;

void uart_init(uint32_t baud)
{
    hal_uart_init(baud);
    s_len        = 0u;
    s_ready      = false;
    s_overflowed = false;
    s_dropped    = 0u;
}

void uart_poll(void)
{
    int c;

    /* Stop as soon as a complete line is pending. Leaving the rest in the
     * HAL ring keeps this loop bounded and means one console command is
     * handled per task run, which is what keeps the 1 ms task honest. */
    while (!s_ready && (c = hal_uart_read_byte()) >= 0) {
        char ch = (char)c;

        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            if (s_overflowed) {
                /* The line was already too long to be meaningful, so drop
                 * the whole thing rather than act on a truncated command. */
                s_dropped++;
                s_overflowed = false;
                s_len        = 0u;
                continue;
            }
            s_line[s_len] = '\0';
            s_ready       = true;
            continue;
        }
        if (s_len >= (UART_LINE_MAX - 1u)) {
            s_overflowed = true;
            continue;
        }
        s_line[s_len++] = ch;
    }
}

bool uart_line_ready(void)
{
    return s_ready;
}

const char *uart_line(void)
{
    return s_line;
}

void uart_consume_line(void)
{
    s_len   = 0u;
    s_ready = false;
}

void uart_puts(const char *s)
{
    hal_uart_write((const uint8_t *)s, strlen(s));
}

static void emit(char c)
{
    hal_uart_write((const uint8_t *)&c, 1u);
}

static void emit_pad(uint8_t width, uint8_t written, bool zero)
{
    while (written < width) {
        emit(zero ? '0' : ' ');
        written++;
    }
}

static uint8_t u32_digits(uint32_t v, uint8_t base)
{
    uint8_t n = 1u;

    while (v >= base) {
        v /= base;
        n++;
    }
    return n;
}

static void emit_u32(uint32_t v, uint8_t base, bool upper)
{
    char    buf[10];
    uint8_t n = 0u;

    do {
        uint32_t d = v % base;

        buf[n++] = (char)((d < 10u) ? ('0' + d)
                                    : ((upper ? 'A' : 'a') + (d - 10u)));
        v /= base;
    } while (v != 0u && n < sizeof buf);

    while (n > 0u) {
        emit(buf[--n]);
    }
}

void uart_printf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    while (*fmt != '\0') {
        bool    zero  = false;
        uint8_t width = 0u;

        if (*fmt != '%') {
            emit(*fmt++);
            continue;
        }
        fmt++;

        if (*fmt == '0') {
            zero = true;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = (uint8_t)((width * 10u) + (uint8_t)(*fmt - '0'));
            fmt++;
        }

        switch (*fmt) {
        case 'c':
            emit((char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);

            if (s == NULL) {
                s = "(null)";
            }
            uart_puts(s);
            break;
        }
        case 'd': {
            int32_t  v = va_arg(ap, int32_t);
            uint32_t m;
            uint8_t  n;

            /* Negating INT32_MIN is undefined, so the magnitude is built
             * without ever holding it as a signed value. This also keeps
             * 64 bit arithmetic, and the library helpers it drags in, out
             * of the image entirely. */
            m = (v < 0) ? ((uint32_t)(-(v + 1)) + 1u) : (uint32_t)v;
            n = u32_digits(m, 10u);
            if (v < 0) {
                n++;
            }
            /* A zero padded negative number needs its sign before the
             * padding, so the sign is emitted first in that case. */
            if (v < 0 && zero) {
                emit('-');
                emit_pad(width, n, true);
                emit_u32(m, 10u, false);
            } else {
                emit_pad(width, n, zero);
                if (v < 0) {
                    emit('-');
                }
                emit_u32(m, 10u, false);
            }
            break;
        }
        case 'u': {
            uint32_t v = va_arg(ap, uint32_t);

            emit_pad(width, u32_digits(v, 10u), zero);
            emit_u32(v, 10u, false);
            break;
        }
        case 'x':
        case 'X': {
            uint32_t v = va_arg(ap, uint32_t);

            emit_pad(width, u32_digits(v, 16u), zero);
            emit_u32(v, 16u, *fmt == 'X');
            break;
        }
        case '%':
            emit('%');
            break;
        case '\0':
            va_end(ap);
            return;
        default:
            emit('%');
            emit(*fmt);
            break;
        }
        fmt++;
    }
    va_end(ap);
}

uint32_t uart_lines_dropped(void)
{
    return s_dropped;
}
