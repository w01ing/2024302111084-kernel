/*
 * 基于轮询的 16550 兼容 UART 输出。
 *
 * uartputc_sync() 只负责向硬件发送一个原始字节；
 * 学号对应的个性化协议由上层 putchar() 负责。
 */

/* 基础整数类型。 */
#include "types.h"

/* io_fence() 等底层 RISC-V 指令封装。 */
#include "riscv.h"

/* UART0 的 QEMU virt 地址定义。 */
#include "memlayout.h"

/* 学号、Banner 协议和个性化参数。 */
#include "course_sid.h"

/* UART 发送保持寄存器 THR 的寄存器偏移。 */
#define UART_THR 0

/* UART 线路状态寄存器 LSR 的寄存器偏移。 */
#define UART_LSR 5

/* LSR 的 bit5：THR 为空，可以写入下一个字节。 */
#define UART_LSR_THRE (1 << 5)

/* 每发送 16 + 学号%16 个真实 UART 字节后执行一次节流。 */
#define UART_THROTTLE_PERIOD (16 + (COURSE_SID % 16))

/* 每次节流执行的 nop 次数。 */
#define UART_THROTTLE_NOP_COUNT 16

/* 根据寄存器偏移计算 UART 某个寄存器的 MMIO 地址。 */
static volatile uint8 *
uart_reg(uint64 offset)
{
  /* volatile 防止编译器优化掉对硬件寄存器的读写。 */
  return (volatile uint8 *)(UART0 + offset);
}

/* 已经发送但尚未触发下一次节流的真实字节数。 */
static uint64 uart_bytes_since_throttle;

/* 同步发送一个字符：等待 UART 空闲后再写入 THR。 */
void
uartputc_sync(char c)
{
  /* nop 循环计数器；volatile 防止循环被优化掉。 */
  volatile uint64 i;

  /*
   * 轮询 LSR.bit5，也就是 THRE：
   * 0 表示发送保持寄存器仍忙，1 表示可以发送。
   */
  while ((*uart_reg(UART_LSR) & UART_LSR_THRE) == 0)
    ;

  /* 将一个字节写入发送保持寄存器，交给 UART 硬件发送。 */
  *uart_reg(UART_THR) = (uint8)c;

  /* 保证前面的 MMIO 写入按顺序完成。 */
  io_fence();

  /* 统计实际写入 UART 的字节，协议附加的 '.' 也会被统计。 */
  uart_bytes_since_throttle++;

  /* 达到个性化节流周期后，执行指定的空转。 */
  if (uart_bytes_since_throttle == UART_THROTTLE_PERIOD) {
    /* 清零计数，开始统计下一组字节。 */
    uart_bytes_since_throttle = 0;

    /* 用 nop 延迟发送节奏，但不改变串口输出内容。 */
    for (i = 0; i < UART_THROTTLE_NOP_COUNT; i++)
      asm volatile("nop");
  }
}

#if LAB1_BANNER_PROTOCOL == 2
/* 协议 2 使用的字节校验和；协议 1 不需要该变量。 */
static uint64 protocol_checksum;
#endif

/* 输出一个逻辑字符，并按照个人参数添加协议格式。 */
void
putchar(int c)
{
  /* 先把 int 转成实际要发送的单字节字符。 */
  char ch = (char)c;

#if LAB1_BANNER_PROTOCOL == 0
  /* 协议 0：原样输出字符，换行由调用者直接提供。 */
  uartputc_sync(ch);
#elif LAB1_BANNER_PROTOCOL == 1
  /* 协议 1：输出原字符后固定追加一个句点。 */
  uartputc_sync(ch);
  uartputc_sync('.');
#elif LAB1_BANNER_PROTOCOL == 2
  /* 协议 2：输出原字符，同时累计其无符号字节值。 */
  uartputc_sync(ch);
  protocol_checksum += (uint8)ch;
#else
  /* 参数出现未知协议编号时，让编译阶段直接报错。 */
#error "unsupported LAB1_BANNER_PROTOCOL"
#endif
}

#if LAB1_BANNER_PROTOCOL == 2
/* 仅供协议 2 使用：用十进制输出一个无符号整数。 */
static void
uartputu(uint64 value)
{
  /* uint64 最多需要 20 位十进制数字。 */
  char digits[20];

  /* digits 中当前已经保存的数字个数。 */
  int n = 0;

  /* 特判 0，否则下面的循环不会写入任何数字。 */
  if (value == 0) {
    uartputc_sync('0');
    return;
  }

  /* 从低位到高位取数字，暂时逆序保存在数组中。 */
  while (value != 0) {
    digits[n++] = (char)('0' + value % 10);
    value /= 10;
  }

  /* 从数组末尾倒序发送，恢复正常的高位到低位顺序。 */
  while (n-- > 0)
    uartputc_sync(digits[n]);
}

/* 协议 2 的结尾输出校验和；协议 1 下该函数为空操作。 */
void
console_protocol_finish(void)
{
  /* 输出校验和字段的左边界。 */
  uartputc_sync('[');
  uartputc_sync('c');
  uartputc_sync('h');
  uartputc_sync('k');
  uartputc_sync('=');

  /* 输出整个 Banner 的累计校验和。 */
  uartputu(protocol_checksum);

  /* 输出校验和字段的右边界。 */
  uartputc_sync(']');
}
#else
/* 协议 0/1 不需要额外的结尾校验和。 */
void
console_protocol_finish(void)
{
  /* 空函数：保持 main() 可以统一调用该接口。 */
}
#endif
