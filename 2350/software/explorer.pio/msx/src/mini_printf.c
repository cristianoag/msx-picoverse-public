// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// mini_printf.c - Minimal printf/sprintf for the Explorer menu ROM
//
// Replaces the SDCC printf_large formatter (about 3 KB), which does not fit
// comfortably below the 0xB900 page-buffer window. Only what the menu uses is
// supported: %s, %d, %u (optional 'l' for 32-bit), %%, and the '-' and '0'
// flags, a decimal width and a '.' precision (strings only). Any other
// conversion is echoed as written. Output goes through putchar() (printf) or a
// buffer (sprintf).
//
// License: Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdarg.h>

int putchar(int character);

// Where the current call writes; 0 means putchar().
static char *out_ptr;
static int out_count;

static void emit(char c)
{
    if (out_ptr) {
        *out_ptr++ = c;
    } else {
        putchar(c);
    }
    out_count++;
}

static void pad(unsigned char width, unsigned char used, char c)
{
    while (width > used) {
        emit(c);
        used++;
    }
}

static const unsigned long powers_of_ten[] = {
    1000000000UL, 100000000UL, 10000000UL, 1000000UL, 100000UL,
    10000UL, 1000UL, 100UL, 10UL, 1UL
};

static int format(const char *fmt, va_list ap)
{
    out_count = 0;
    for (; *fmt; fmt++) {
        unsigned char left = 0;
        unsigned char zero = 0;
        unsigned char width = 0;
        unsigned char is_long = 0;
        unsigned char negative = 0;
        unsigned char len;
        unsigned char i;
        unsigned int precision = 0xFFFFu;
        char digits[10];
        const char *text;

        if (*fmt != '%') {
            emit(*fmt);
            continue;
        }
        fmt++;
        if (*fmt == '-') {
            left = 1;
            fmt++;
        }
        if (*fmt == '0') {
            zero = !left;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = (unsigned char)(width * 10 + (*fmt++ - '0'));
        }
        if (*fmt == '.') {
            precision = 0;
            for (fmt++; *fmt >= '0' && *fmt <= '9'; fmt++) {
                precision = precision * 10 + (*fmt - '0');
            }
        }
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
        }
        if (*fmt == 's') {
            zero = 0;
            text = va_arg(ap, const char *);
            for (len = 0; text[len] && len < precision; len++) {
            }
        } else if (*fmt == 'd' || *fmt == 'u') {
            unsigned long value;

            if (is_long) {
                value = va_arg(ap, unsigned long);
            } else if (*fmt == 'd') {
                int number = va_arg(ap, int);
                negative = number < 0;
                value = negative ? (unsigned int)-number : (unsigned int)number;
            } else {
                value = va_arg(ap, unsigned int);
            }
            len = 0;
            for (i = 0; i < 10; i++) {
                char digit = '0';
                while (value >= powers_of_ten[i]) {
                    value -= powers_of_ten[i];
                    digit++;
                }
                if (len || digit != '0' || i == 9) {
                    digits[len++] = digit;
                }
            }
            text = digits;
        } else {
            if (*fmt == '\0') {
                break;
            }
            emit(*fmt);
            continue;
        }
        if (!left && !zero) {
            pad(width, (unsigned char)(len + negative), ' ');
        }
        if (negative) {
            emit('-');
        }
        if (zero) {
            pad(width, (unsigned char)(len + negative), '0');
        }
        for (i = 0; i < len; i++) {
            emit(text[i]);
        }
        if (left) {
            pad(width, (unsigned char)(len + negative), ' ');
        }
    }
    return out_count;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    out_ptr = 0;
    return format(fmt, ap);
}

int sprintf(char *buffer, const char *fmt, ...)
{
    va_list ap;
    int count;
    va_start(ap, fmt);
    out_ptr = buffer;
    count = format(fmt, ap);
    *out_ptr = '\0';
    return count;
}
