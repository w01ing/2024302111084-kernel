/*
 * LAB2-STAGE2：系统调用参数解析与分发。
 *
 * 阶段一已经证明 trampoline.S 能够把用户寄存器完整保存到
 * trapframe，并且能把 trapframe.a0 中的值恢复回用户态。阶段二
 * 不再把 a7 当作一个孤立的测试数字，而是把它解释为系统调用号：
 *
 *   a7       -> 系统调用号
 *   a0..a2   -> 三个调用参数
 *   返回值   -> trapframe.a0
 *
 * 当前还没有正式的进程表、文件表和用户地址空间隔离。因此：
 *   1. getpid() 使用阶段四简化运行时维护的父/子编号；
 *   2. write() 允许标准输出/标准错误，并直接读取当前用户映射中的地址；
 *   3. exit() 在阶段四子程序结束时恢复 fork 暂存的 Shell。
 *
 * 直接读取用户指针是实验接口明确允许的过渡做法，完整的地址检查
 * 和 copyin 会在后续虚拟内存阶段补齐。
 */

#include "types.h"
#include "trap.h"
#include "syscall.h"
#include "exec.h"      /* LAB2-STAGE4：sh/hi/spin 的装载和伪进程状态。 */
#include "vm.h"
/* LAB2-STAGE3：read(0, ...) 连接到控制台输入环形缓冲区。 */
#include "console.h"

/* LAB2-STAGE2：write 允许使用的标准输出文件描述符。 */
#define STDOUT_FD 1
#define STDERR_FD 2

/* LAB2-STAGE2：限制长度，防止错误参数导致无限读取内存。 */
#define STAGE2_WRITE_MAX 4096

/*
 * 从 trapframe 中读取一个整数参数。
 *
 * ecall 进入 uservec 后，用户 a0、a1、a2 已经分别保存到
 * trapframe->a0、a1、a2。系统调用层只读取这些已保存的值，
 * 不直接依赖 C 函数调用时的寄存器状态。
 */
static int
argint(struct trapframe *tf, int index, int *out)
{
  uint64 value;

  if (index == 0)
    value = tf->a0;
  else if (index == 1)
    value = tf->a1;
  else if (index == 2)
    value = tf->a2;
  else
    return -1;

  *out = (int)value;
  return 0;
}

/*
 * 读取一个地址参数。
 *
 * 阶段二只有 write() 使用用户指针。这里先完成 ABI 层面的参数提取；
 * 真正的用户地址合法性检查留给后续页表和 copyin 阶段。
 */
static int
argaddr(struct trapframe *tf, int index, uint64 *out)
{
  if (index == 0)
    *out = tf->a0;
  else if (index == 1)
    *out = tf->a1;
  else if (index == 2)
    *out = tf->a2;
  else
    return -1;

  return 0;
}

/* LAB2-STAGE4：getpid() 返回当前 Shell/子程序的简化进程编号。 */
static long
sys_getpid(void)
{
  return stage4_getpid();
}

/*
 * LAB2-STAGE2：最小 write 实现。
 *
 * 参数约定：
 *   a0 = fd
 *   a1 = 用户缓冲区地址
 *   a2 = 字节数
 *
 * 返回成功写出的字节数；文件描述符、地址或长度非法时返回 -1。
 * 当前阶段的用户页表把用户镜像映射在低地址，因此这里可以直接
 * 解引用用户地址；阶段四的输出改走原始 UART，不再追加 Lab1 句点。
 */
static long
sys_write(struct trapframe *tf)
{
  int fd;
  int count;
  uint64 address;
  char buffer[256];

  if (argint(tf, 0, &fd) < 0 ||
      argaddr(tf, 1, &address) < 0 ||
      argint(tf, 2, &count) < 0)
    return -1;

  if (fd != STDOUT_FD && fd != STDERR_FD)
    return -1;
  if (count < 0 || count > STAGE2_WRITE_MAX)
    return -1;
  if (count == 0)
    return 0;
  if (address == 0)
    return -1;

  if (count > (int)sizeof(buffer) || copyin(buffer, address, count) < 0)
    return -1;
  return console_write_raw(buffer, count);
}

/*
 * LAB2-STAGE3：最小 read 实现。
 *
 * 阶段三只开放标准输入 fd=0。用户缓冲区地址直接传给
 * console_read()，因为当前过渡页表把低地址 RAM 直映给了用户；
 * 后续页表阶段再把这里替换成 copyout/copyin。
 */
static long
sys_read(struct trapframe *tf)
{
  int fd;
  int count;
  uint64 address;

  if (argint(tf, 0, &fd) < 0 ||
      argaddr(tf, 1, &address) < 0 ||
      argint(tf, 2, &count) < 0)
    return -1;

  if (fd != 0 || count < 0 || count > STAGE2_WRITE_MAX)
    return -1;
  if (count == 0)
    return 0;
  if (address == 0)
    return -1;

  char buffer[256];
  if (count > (int)sizeof(buffer)) return -1;
  long n = console_read(buffer, count);
  if (n < 0 || copyout(address, buffer, n) < 0) return -1;
  return n;
}

/*
 * LAB2-STAGE4：exit 的最小语义。
 *
 * 子程序退出时由 stage4_exit() 恢复 fork 保存的 Shell trapframe；
 * 如果没有父现场可恢复，则停在内核安全循环中。重要的是 exit
 * 不会返回用户态，符合用户桩的 noreturn 语义。
 */
__attribute__((noreturn)) static void
sys_exit(struct trapframe *tf)
{
  int status;

  if (argint(tf, 0, &status) < 0)
    status = -1;

  stage4_exit(tf, status);
}

/*
 * LAB2-STAGE2：按照赠送件 syscall.h 中的编号进行系统调用分发。
 *
 * 每个处理函数只负责完成自己的语义；返回值统一由 usertrap()
 * 写入 trapframe->a0，再由 userret 恢复给用户程序。
 */
long
syscall_dispatch(struct trapframe *tf)
{
  switch (tf->a7) {
  case SYS_read:
    return sys_read(tf);
  /* LAB2-STAGE4：Shell 通过这三个调用启动和等待用户程序。 */
  case SYS_fork:
    return stage4_fork(tf);
  case SYS_wait:
    return stage4_wait(tf);
  case SYS_exec:
    return stage4_exec(tf);
  case SYS_sbrk:
    return stage4_sbrk(tf);
  case SYS_getpid:
    return sys_getpid();
  case SYS_write:
    return sys_write(tf);
  case SYS_exit:
    sys_exit(tf);
  case SYS_sync:
    return 0;
  default:
    /* 未知或尚未实现的调用必须稳定返回 -1，不能让内核崩溃。 */
    return -1;
  }
}
