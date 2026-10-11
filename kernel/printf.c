/*
 * 实验使用的最小 freestanding printf。
 *
 * Makefile 使用 -nostdlib，不能依赖标准 C 库中的 printf，
 * 因此这里自己解析格式字符串，并通过 putchar() 输出字符。
 */

/* va_list、va_start、va_arg、va_end 的定义。 */
#include <stdarg.h>

/* 实验使用的 uint64 等基础类型。 */
#include "types.h"

/* putchar() 最终会把字符发送到 UART。 */
extern void putchar(int c);

/* 按指定进制输出一个无符号整数。 */
static void
print_unsigned(uint64 value, int base)
{
  /* 十进制和十六进制共用的数字字符表。 */
  static const char digits[] = "0123456789abcdef";

  /* 暂存逆序数字；uint64 十六进制最多需要 16 位。 */
  char buf[sizeof(uint64) * 8];

  /* 当前已经取出的数字个数。 */
  int n = 0;

  /* 0 需要单独输出，否则取余循环不会执行。 */
  if (value == 0) {
    putchar('0');
    return;
  }

  /* 每次取最低位，并将剩余高位继续留在 value 中。 */
  while (value != 0) {
    buf[n++] = digits[value % (uint64)base];
    value /= (uint64)base;
  }

  /* 取出的数字是低位到高位，倒序输出才能得到正常结果。 */
  while (n-- > 0)
    putchar(buf[n]);
}

/* 输出一个有符号十进制整数。 */
static void
print_signed(long long value)
{
  /* 统一保存正数形式的绝对值，交给无符号函数输出。 */
  uint64 magnitude;

  /* 负数先输出负号，再计算其无符号幅值。 */
  if (value < 0) {
    putchar('-');
    magnitude = 0 - (uint64)value;
  } else {
    /* 非负数直接转换为无符号数。 */
    magnitude = (uint64)value;
  }

  /* 使用十进制基数 10 输出数值部分。 */
  print_unsigned(magnitude, 10);
}

/* 逐字节输出 C 字符串。 */
static void
print_string(const char *s)
{
  /* 空指针不解引用，使用可读的占位文本代替。 */
  if (s == 0)
    s = "(null)";

  /* 直到字符串结尾的 '\0' 为止逐字节输出。 */
  while (*s != '\0')
    putchar((unsigned char)*s++);
}

/* 解析格式字符串，并输出对应的参数。 */
void
printf(const char *fmt, ...)
{
  /* 保存可变参数列表。 */
  va_list ap;

  /* 标记当前转换是否带有 long 修饰符，例如 %ld。 */
  int long_arg;

  /* 初始化可变参数读取位置。 */
  va_start(ap, fmt);

  /* 从左到右扫描格式字符串，直到遇到 '\0'。 */
  for (; *fmt != '\0'; fmt++) {
    /* 普通字符不需要解析，直接输出。 */
    if (*fmt != '%') {
      putchar((unsigned char)*fmt);
      continue;
    }

    /* 跳过 '%'，读取它后面的格式说明符。 */
    fmt++;

    /* 格式字符串以 '%' 结尾时，避免越界读取。 */
    if (*fmt == '\0')
      break;

    /* 默认当前格式不是 long 类型。 */
    long_arg = 0;

    /* 识别 %ld 中的 l，并移动到真正的 d。 */
    if (*fmt == 'l') {
      long_arg = 1;
      fmt++;

      /* 防止格式字符串以 "%l" 结束时继续读取。 */
      if (*fmt == '\0')
        break;
    }

    /* 根据格式字符选择不同的输出路径。 */
    switch (*fmt) {
    case 'd':
      /* %d 读取 int，%ld 读取 long；学号使用 %ld。 */
      if (long_arg)
        print_signed((long long)va_arg(ap, long));
      else
        print_signed((long long)va_arg(ap, int));
      break;
    case 'x':
      /* %x 以小写十六进制输出 unsigned int。 */
      print_unsigned((uint64)(uint)va_arg(ap, unsigned int), 16);
      break;
    case 'p':
      /* %p 输出 0x 前缀，再输出完整指针值。 */
      putchar('0');
      putchar('x');
      print_unsigned(va_arg(ap, uint64), 16);
      break;
    case 's':
      /* %s 读取字符串指针并逐字节输出。 */
      print_string(va_arg(ap, const char *));
      break;
    case 'c':
      /* %c 读取按 ABI 提升后的 int，并输出低字节字符。 */
      putchar(va_arg(ap, int));
      break;
    case '%':
      /* %% 表示输出一个普通的百分号。 */
      putchar('%');
      break;
    default:
      /* 未知格式不丢失信息，原样输出 '%' 和格式字符。 */
      putchar('%');
      putchar((unsigned char)*fmt);
      break;
    }
  }

  /* 释放可变参数列表。 */
  va_end(ap);
}
