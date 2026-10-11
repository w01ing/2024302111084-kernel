#ifndef LAB2_CONSOLE_H
#define LAB2_CONSOLE_H

#include "types.h"

/*
 * LAB2-STAGE3：
 * 控制台模块同时提供三条路径：
 *   1. uart_init()：配置 16550 接收中断；
 *   2. plic_init()：打开 hart 0 的 UART 外部中断通路；
 *   3. devintr()：处理中断入口从 PLIC 取出的 UART 外部中断；
 *   4. console_read()：read(0, ...) 从环形缓冲区取字符。
 */
void uart_init(void);
void plic_init(void);
void console_puts_raw(const char *s);
/* LAB2-STAGE4：用户 write() 使用的原始字节输出，不附加 Lab1 句点。 */
int console_write_raw(const char *s, int count);
int devintr(void);
int console_read(char *dst, int count);

#endif
