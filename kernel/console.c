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

/* LAB2-STAGE3：控制台中断和 read() 的公开接口。 */
#include "console.h"

/* UART 发送保持寄存器 THR 的寄存器偏移。 */
#define UART_THR 0

/* UART 接收保持寄存器与发送保持寄存器共用偏移 0。 */
#define UART_RHR 0

/* UART 中断使能寄存器。 */
#define UART_IER 1

/* UART 中断标识寄存器。 */
#define UART_IIR 2

/* UART FIFO 控制寄存器。 */
#define UART_FCR 2

/* UART 线路控制寄存器。 */
#define UART_LCR 3

/* UART 线路状态寄存器 LSR 的寄存器偏移。 */
#define UART_LSR 5

/* LSR 的 bit5：THR 为空，可以写入下一个字节。 */
#define UART_LSR_THRE (1 << 5)

/* IER bit0：允许接收数据就绪中断。 */
#define UART_IER_RX_ENABLE (1 << 0)

/* IIR bit0：1 表示没有待处理的 UART 中断。 */
#define UART_IIR_NO_INTERRUPT (1 << 0)

/* LCR bit7：访问波特率分频器 DLL/DLM。 */
#define UART_LCR_BAUD_LATCH (1 << 7)

/* UART 接收 FIFO 使能，并清空收发 FIFO。 */
#define UART_FCR_ENABLE_FIFO 0x01
#define UART_FCR_CLEAR_RX   0x02
#define UART_FCR_CLEAR_TX   0x04

/* 16550 的 8 位数据、无校验、1 个停止位。 */
#define UART_LCR_8BIT 0x03

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

/*
 * LAB2-STAGE3：输入环形缓冲区。
 *
 * r = 已经被 read() 消费的位置；
 * w = 已经对读者提交的位置；
 * e = UART 中断已经写入的位置。
 *
 * 三个下标都使用单调递增计数，真正访问数组时才对
 * LAB2_BUF_SIZE 取模。这样 w-r 始终表示当前未消费数据量。
 */
static char console_buf[LAB2_BUF_SIZE];
static uint64 console_r;
static uint64 console_w;
static uint64 console_e;

/* 向 UART 写一个原始字节，键盘回显不叠加 Lab1 的句点协议。 */
static void
uart_echo_raw(char c)
{
  volatile uint64 i;

  while ((*uart_reg(UART_LSR) & UART_LSR_THRE) == 0)
    ;
  *uart_reg(UART_THR) = (uint8)c;
  io_fence();

  /* 回显也遵守已有发送节流，避免风暴输入撑爆模拟器输出。 */
  uart_bytes_since_throttle++;
  if (uart_bytes_since_throttle == UART_THROTTLE_PERIOD) {
    uart_bytes_since_throttle = 0;
    for (i = 0; i < UART_THROTTLE_NOP_COUNT; i++)
      asm volatile("nop");
  }
}

/*
 * LAB2-STAGE3：
 * 向串口发送不经过 Lab1 个性化协议的原始字符串。
 *
 * inject_uart.py 的启动探针硬编码等待 "$ "。如果使用 printf() 输出，
 * 当前 Lab1 协议会把它变成 "$ . ."，脚本就无法识别。因此阶段三
 * 自测提示必须走这条原始输出路径。
 */
void
console_puts_raw(const char *s)
{
  while (*s != '\0')
    uart_echo_raw(*s++);
}

/*
 * LAB2-STAGE4：把用户 write(fd, buf, n) 的 n 个字节原样送到 UART。
 *
 * Lab1 的 putchar() 会根据课程协议附加句点；Shell 的命令行和用户
 * 程序输出必须保持普通终端语义，所以阶段四单独走这条原始路径。
 */
int
console_write_raw(const char *s, int count)
{
  int i;

  if (s == 0 || count < 0)
    return -1;

  for (i = 0; i < count; i++)
    uart_echo_raw(s[i]);
  return count;
}

/*
 * LAB2-STAGE3：初始化 16550 UART 的接收路径。
 *
 * 先关闭 UART 中断，再设置 8N1、波特率分频器和 FIFO，最后只打开
 * 接收数据就绪中断。PLIC 的线路使能在 plic_init() 中完成。
 */
void
uart_init(void)
{
  *uart_reg(UART_IER) = 0;
  *uart_reg(UART_LCR) = UART_LCR_BAUD_LATCH;
  *uart_reg(0) = 0x03; /* DLL：QEMU 16550 的常用分频值。 */
  *uart_reg(1) = 0x00; /* DLM：分频器高字节。 */
  *uart_reg(UART_LCR) = UART_LCR_8BIT;
  *uart_reg(UART_FCR) =
      UART_FCR_ENABLE_FIFO | UART_FCR_CLEAR_RX | UART_FCR_CLEAR_TX;
  *uart_reg(UART_IER) = UART_IER_RX_ENABLE;
}

/*
 * LAB2-STAGE3：打开 hart 0 的 PLIC 外部中断通路。
 *
 * UART0 的硬件 IRQ 号是 10。PLIC 需要同时满足三层条件：
 *   1. IRQ 优先级非零；
 *   2. 当前 hart 的 S 态 enable 位打开；
 *   3. 当前 hart 的 threshold 低于该 IRQ 优先级。
 */
void
plic_init(void)
{
  volatile uint32 *priority =
      (volatile uint32 *)(PLIC_PRIORITY + UART0_IRQ * 4);
  volatile uint32 *enable =
      (volatile uint32 *)PLIC_SENABLE(0);
  volatile uint32 *threshold =
      (volatile uint32 *)PLIC_SPRIORITY(0);

  *priority = 1;
  *enable |= (1U << UART0_IRQ);
  *threshold = 0;
}

/*
 * LAB2-STAGE3：把一个字符提交给输入缓冲区。
 *
 * LAB2_BUF_SEMANTICS=0 是行缓冲：字符先进入 [w,e)，遇到 '\n'
 * 才把 w 推到 e，read() 才能看见这一整行。
 *
 * 缓冲满时直接丢弃后续字符，这是增量包允许的文档化行为。遇到
 * 换行时，即使换行本身放不下，也提交已经收下的前缀。
 */
static void
console_input_char(char c)
{
  uint64 used;

  /* 退格删除尚未提交的最后一个字符。 */
  if (c == '\b' || c == 0x7f) {
    if (console_e != console_w) {
      console_e--;
      uart_echo_raw('\b');
      uart_echo_raw(' ');
      uart_echo_raw('\b');
    }
    return;
  }

  /* 统一终端常见的回车和换行表示。 */
  if (c == '\r')
    c = '\n';

  /* 每个字符都重新计算占用量，正确处理一次 FIFO 批量到达。 */
  used = console_e - console_r;
  if (used < LAB2_BUF_SIZE) {
    console_buf[console_e % LAB2_BUF_SIZE] = c;
    console_e++;
    uart_echo_raw(c);
  } else if (c == '\n') {
    /* 满时无法再存换行，但仍提交已经收下的前缀。 */
    console_w = console_e;
    uart_echo_raw(c);
    return;
  } else {
    /* 满缓冲的非换行字符丢弃，但继续回显用户输入。 */
    uart_echo_raw(c);
    return;
  }

#if LAB2_BUF_SEMANTICS == 0
  if (c == '\n')
    console_w = console_e;
#else
  console_w = console_e;
#endif
}

/*
 * LAB2-STAGE3：UART 接收中断服务程序。
 *
 * IIR bit0 为 0 表示仍有待处理中断；循环读取 RHR，直到 UART
 * 接收 FIFO 清空。每次读 RHR 都会推进硬件接收队列。
 */
static void
uart_intr(void)
{
  while ((*uart_reg(UART_IIR) & UART_IIR_NO_INTERRUPT) == 0) {
    char c = (char)*uart_reg(UART_RHR);
    console_input_char(c);
  }
}

/*
 * LAB2-STAGE3：统一的外部设备中断入口。
 *
 * PLIC claim 返回设备 IRQ 号；目前只有 UART0 IRQ=10。处理完成后
 * 必须把相同 IRQ 写回 claim/complete 寄存器。
 */
int
devintr(void)
{
  volatile uint32 *claim = (volatile uint32 *)PLIC_SCLAIM(0);
  uint32 irq = *claim;

  if (irq == UART0_IRQ) {
    uart_intr();
    *claim = irq;
    return 1;
  }

  if (irq != 0)
    *claim = irq;
  return 0;
}

/*
 * LAB2-STAGE3：从输入环形缓冲区读取字符。
 *
 * 这里还没有 lab4 的 sleep/wakeup，因此采用“关中断检查、开中断
 * WFI”的最小阻塞方案。这样不会在检查和等待之间丢失 UART 中断。
 */
int
console_read(char *dst, int count)
{
  int copied = 0;

  if (dst == 0 || count <= 0)
    return count == 0 ? 0 : -1;

  for (;;) {
    intr_off();

    while (console_r != console_w && copied < count) {
      char c = console_buf[console_r % LAB2_BUF_SIZE];
      console_r++;
      dst[copied++] = c;

#if LAB2_BUF_SEMANTICS == 0
      if (c == '\n')
        break;
#endif
    }

    if (copied != 0) {
      intr_on();
      return copied;
    }

    intr_on();
    asm volatile("wfi");
  }
}

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
