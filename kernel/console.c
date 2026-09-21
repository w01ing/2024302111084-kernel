/*
 * Polled 16550-compatible UART output.
 *
 * uartputc_sync() is deliberately raw.  The student-specific output
 * protocol is applied one layer above it by putchar().
 */

#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "course_sid.h"

#define UART_THR 0
#define UART_LSR 5
#define UART_LSR_THRE (1 << 5)

static volatile uint8 *
uart_reg(uint64 offset)
{
  return (volatile uint8 *)(UART0 + offset);
}

void
uartputc_sync(char c)
{
  while ((*uart_reg(UART_LSR) & UART_LSR_THRE) == 0)
    ;
  *uart_reg(UART_THR) = (uint8)c;
  io_fence();
}

#if LAB1_BANNER_PROTOCOL == 2
static uint64 protocol_checksum;
#endif

void
putchar(int c)
{
  char ch = (char)c;

#if LAB1_BANNER_PROTOCOL == 0
  uartputc_sync(ch);
#elif LAB1_BANNER_PROTOCOL == 1
  uartputc_sync(ch);
  uartputc_sync('.');
#elif LAB1_BANNER_PROTOCOL == 2
  uartputc_sync(ch);
  protocol_checksum += (uint8)ch;
#else
#error "unsupported LAB1_BANNER_PROTOCOL"
#endif
}

#if LAB1_BANNER_PROTOCOL == 2
static void
uartputu(uint64 value)
{
  char digits[20];
  int n = 0;

  if (value == 0) {
    uartputc_sync('0');
    return;
  }
  while (value != 0) {
    digits[n++] = (char)('0' + value % 10);
    value /= 10;
  }
  while (n-- > 0)
    uartputc_sync(digits[n]);
}

void
console_protocol_finish(void)
{
  uartputc_sync('[');
  uartputc_sync('c');
  uartputc_sync('h');
  uartputc_sync('k');
  uartputc_sync('=');
  uartputu(protocol_checksum);
  uartputc_sync(']');
}
#else
void
console_protocol_finish(void)
{
}
#endif
