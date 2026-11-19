/* Kernel printing: everything goes to the screen and to the modem port. */
#include <stdarg.h>
#include "kernel.h"

static void (*divert)(char);

/* While a function is given, it gets what would go to the text screen
 * (connect.c); NULL puts the text screen back. */
void kputc_divert(void (*to)(char))
{
    divert = to;
}

void kputc(char c)
{
    if (divert)
        divert(c);
    else
        con_putc(c);
    if (c == '\n')
        serial_putc('\r');
    serial_putc(c);
}

void kputs(const char *s)
{
    while (*s)
        kputc(*s++);
}

/* Print an unsigned number in the given base, padded on the left to `width`
 * with `pad`. */
static void print_number(u32 value, unsigned base, int width, char pad, int negative)
{
    static const char digits[] = "0123456789ABCDEF";
    char buf[12];
    int n = 0;

    do {
        buf[n++] = digits[value % base];
        value /= base;
    } while (value);
    if (negative)
        buf[n++] = '-';
    while (width-- > n)
        kputc(pad);
    while (n)
        kputc(buf[--n]);
}

/* Supports %c %s %d %u %x %X %p and %%, an optional 'l' (ignored: int and
 * long are both 32 bits), a field width, and a leading 0 for zero padding. */
void kprintf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        char pad = ' ';
        int width = 0;

        if (*fmt != '%') {
            kputc(*fmt);
            continue;
        }
        fmt++;
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        if (*fmt == 'l')
            fmt++;

        switch (*fmt) {
        case 'c':
            kputc((char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            int len = (int)strlen(s);

            kputs(s);
            while (width-- > len)
                kputc(' ');
            break;
        }
        case 'd': {
            s32 v = va_arg(ap, s32);

            if (v < 0)
                print_number((u32)-v, 10, width, pad, 1);
            else
                print_number((u32)v, 10, width, pad, 0);
            break;
        }
        case 'u':
            print_number(va_arg(ap, u32), 10, width, pad, 0);
            break;
        case 'x':
        case 'X':
            print_number(va_arg(ap, u32), 16, width, pad, 0);
            break;
        case 'p':
            print_number(va_arg(ap, u32), 16, 8, '0', 0);
            break;
        case '%':
            kputc('%');
            break;
        case '\0':
            va_end(ap);
            return;
        default:
            kputc('%');
            kputc(*fmt);
            break;
        }
    }
    va_end(ap);
}
