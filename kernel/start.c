/*
 * M 态启动初始化，并完成一次性的 M 态 -> S 态切换。
 *
 * entry.S 先设置最初的栈和 M 态基础环境，再跳到这里；
 * 本函数准备好 S 态运行条件后，使用 mret 跳转到 main()。
 */

/* 自定义的无符号整数类型和 RISC-V CSR 读写函数。 */
#include "types.h"
#include "riscv.h"

/* mstatus.MIE：M 态全局中断使能位。 */
#define MSTATUS_MIE (1L << 3)

/* main() 是切换到 S 态后的第一个 C 函数。 */
extern void main(void);

/* supervisor_trap 在 entry.S 中定义，作为 S 态异常入口。 */
extern void supervisor_trap(void);

/* 告诉编译器：start() 不会正常返回。 */
__attribute__((noreturn)) void
start(void)
{
  /* 暂存修改后的 mstatus 值。 */
  uint64 x;

  /*
   * 将异常委托给 S 态。
   * 0xffff 覆盖常见的同步异常，方便后续由 S 态内核统一处理。
   */
  w_medeleg(0xffff);

  /*
   * 将中断委托给 S 态。
   * 本实验还没有真正开启设备中断，但必须先建立特权级委托关系。
   */
  w_mideleg(0xffff);

  /*
   * 允许低特权级访问 cycle、time、instret 等计数器。
   * 0x7 的三个低位分别对应这三个计数器。
   */
  w_mcounteren(0x7);

  /*
   * 配置 PMP 地址范围。
   * 最大 NAPOT 地址表示覆盖整个物理地址空间，包含内核 RAM 和 UART MMIO。
   */
  w_pmpaddr0(0x3fffffffffffffull);

  /*
   * 配置 PMP 权限：
   * A=NAPOT，R=1，W=1，X=1，L=0，允许 S 态读、写和执行。
   */
  w_pmpcfg0(0xf);

  /* 设置 S 态发生异常或中断时的入口地址。 */
  w_stvec((uint64)supervisor_trap);

  /* 设置 mret 执行后要跳转到的 C 入口 main()。 */
  w_mepc((uint64)main);

  /* 读取当前 mstatus，保留其中与本实验无关的状态位。 */
  x = r_mstatus();

  /* 先清除 MPP 字段，避免旧值影响目标特权级。 */
  x &= ~MSTATUS_MPP_MASK;

  /* 将 MPP 设置为 S，表示 mret 后进入 S 态。 */
  x |= MSTATUS_MPP_S;

  /* 再次确保执行 mret 前 M 态全局中断关闭。 */
  x &= ~MSTATUS_MIE;

  /* 将修改后的 mstatus 写回硬件 CSR。 */
  w_mstatus(x);

  /*
   * 从 M 态返回到 mstatus.MPP 指定的 S 态，
   * 并从 mepc 指定的 main() 开始执行。
   */
  asm volatile("mret" ::: "memory");

  /* 理论上 mret 不会返回；保留死循环作为安全兜底。 */
  for (;;)
    ;
}
