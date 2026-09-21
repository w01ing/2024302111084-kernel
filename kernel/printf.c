/*
 * Small freestanding printf used by the lab console.
 */

#include <stdarg.h>

#include "types.h"

extern void putchar(int c);

static void
print_unsigned(uint64 value, int base)
{
  static const char digits[] = "0123456789abcdef";
  char buf[sizeof(uint64) * 8];
  int n = 0;

  if (value == 0) {
    putchar('0');
    return;
  }
  while (value != 0) {
    buf[n++] = digits[value % (uint64)base];
    value /= (uint64)base;
  }
  while (n-- > 0)
    putchar(buf[n]);
}

static void
print_signed(long long value)
{
  uint64 magnitude;

  if (value < 0) {
    putchar('-');
    magnitude = 0 - (uint64)value;
  } else {
    magnitude = (uint64)value;
  }
  print_unsigned(magnitude, 10);
}

static void
print_string(const char *s)
{
  if (s == 0)
    s = "(null)";
  while (*s != '\0')
    putchar((unsigned char)*s++);
}

void
printf(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  for (; *fmt != '\0'; fmt++) {
    if (*fmt != '%') {
      putchar((unsigned char)*fmt);
      continue;
    }

    fmt++;
    if (*fmt == '\0')
      break;
    switch (*fmt) {
    case 'd':
      print_signed((long long)va_arg(ap, int));
      break;
    case 'x':
      print_unsigned((uint64)(uint)va_arg(ap, unsigned int), 16);
      break;
    case 'p':
      putchar('0');
      putchar('x');
      print_unsigned(va_arg(ap, uint64), 16);
      break;
    case 's':
      print_string(va_arg(ap, const char *));
      break;
    case 'c':
      putchar(va_arg(ap, int));
      break;
    case '%':
      putchar('%');
      break;
    default:
      putchar('%');
      putchar((unsigned char)*fmt);
      break;
    }
  }
  va_end(ap);
}
