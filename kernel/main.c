/*
 * Lab1 的 S 态入口和确定性自检输出。
 *
 * start() 中执行 mret 后，CPU 会从这里开始执行；
 * 本函数负责输出个性化 Banner 和 printf 边界测试。
 */

/* 基础类型定义。 */
#include "types.h"

/* 学号和本实验的个性化协议参数。 */
#include "course_sid.h"

/* LAB2-STAGE1: declares trap_init() and usertrapret(). */
#include "trap.h"

/* LAB2-STAGE1/STAGE2：自测会直接填写 trapframe 的 ABI 寄存器字段。 */
#include "syscall.h"

/* 自己实现的最小 printf。 */
extern void printf(const char *fmt, ...);

/* 协议 2 需要收尾；协议 0/1 下该函数为空操作。 */
extern void console_protocol_finish(void);

/* 输出实验要求的 printf 边界测试数据。 */
static void
printf_boundary_tests(void)
{
  /*
   * 连续长字符串，用于验证 printf 不会在较长字符串输出时崩溃。
   * 三段字符串在编译期会自动拼接成一个完整字符串。
   */
  static const char long_text[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

  /* 依次测试 0、负数、最大 32 位整数、空字符串和长字符串。 */
  printf("printf:%d,%d,%d,%s,%s\n",
         0, -1, 2147483647, "", long_text);
}

/* 告诉编译器 main() 不会返回。 */
__attribute__((noreturn)) void
main(void)
{
  /*
   * 输出个性化 Banner。
   * 学号超过 32 位，因此使用 %ld 和 long，避免被截断。
   * 2024302111084 % 97 = 68 = 0x44。
   */
  printf("OSLAB1 sid=%ld mod97=0x%x\n",
         (long)COURSE_SID, (unsigned int)(COURSE_SID % 97));

#if LAB1_BANNER_PROTOCOL != 2
  /* 协议 0/1 继续输出 printf 边界测试；协议 2 需要保留末段校验和。 */
  printf_boundary_tests();
#endif

  /* 协议 2 在这里输出校验和，协议 0/1 下不会产生额外字符。 */
  console_protocol_finish();

  /*
   * LAB2-STAGE1:
   * main() is still running in S mode after start.c executes mret.
   * trap_init() creates the minimal user/kernel address spaces and fills the
   * kernel-side trapframe fields.  main() then fills the user-side starting
   * fields before usertrapret() enters trampoline/userret and performs the
   * first sret into the selected user test entry.
   */
  printf("lab2 stage2: enter user mode\n");
  trap_init();

  /*
   * LAB2-STAGE2 self-test:
   * 这里仍然由 main() 手动构造初始 trapframe，但用户入口已经升级为
   * stage2_user_entry。它按真实 ABI 依次执行 getpid、write、未知调用
   * 和 exit，专门验证“取号 -> 取参数 -> 分发 -> a0 返回”的链路。
   */
  stage1_tf.epc = (uint64)stage2_user_entry;
  stage1_tf.sp = STAGE1_USER_STACK;
  stage1_tf.a0 = 0;
  stage1_tf.a7 = SYS_getpid;

  usertrapret();

  for (;;)
    asm volatile("wfi");
}
