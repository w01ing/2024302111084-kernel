#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "trap.h"
#include "syscall.h"
#include "console.h"
#include "proc.h"
#include "spinlock.h"
#include "vm.h"
#include "course_sid.h"

extern char trampoline[], userret[];
extern void supervisor_trap(void);
extern void printf(const char *, ...);

/* Kept for the Lab3 kernel-page-table regression mapping. */
struct trapframe stage1_tf __attribute__((aligned(PGSIZE)));

static uint64
userret_va(void)
{
  return TRAMPOLINE + ((uint64)userret - (uint64)trampoline);
}

void
trap_init(void)
{
  vm_init();
  stage1_tf.kernel_satp = MAKE_SATP(kernel_pagetable());
  stage1_tf.kernel_trap = (uint64)usertrap;
  stage1_tf.kernel_hartid = 0;
  w_stvec((uint64)supervisor_trap);
}

int
trap_map_user_image(uint64 a, uint64 b, uint64 c, uint64 d)
{
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  return -1;
}

static int
is_timer_interrupt(uint64 cause)
{
  return (cause >> 63) && (cause & 0xfff) == 5;
}

static int
is_external_interrupt(uint64 cause)
{
  return (cause >> 63) && (cause & 0xfff) == 9;
}

// 用户态 trap 的统一入口（由 trampoline 跳进来，stvec 此时指向 trampoline）。
// 处理四类事件：外部设备中断、定时器中断、系统调用（ecall）、其他异常。
// 处理完统一走 usertrapret() 返回用户态；若进程已被标记 killed，则退出。
void
usertrap(void)
{
  struct proc *p = myproc();
  if (!p)
    panic("usertrap no proc");   // 用户 trap 时必须有当前进程，否则是内核 bug

  // 读 trap 原因（scause 编码了是哪种中断/异常）
  uint64 cause = r_scause();

  // ---- 关键：把 stvec 从 trampoline 改回 supervisor_trap ----
  // 进入 trap 时 stvec 指向 trampoline（用户态 trap 的固定入口）；
  // 现在已经在内核态处理了，要改回 supervisor_trap，
  // 这样如果内核态再发生 trap（比如中断），能进入 kernelvec 而不是又回 trampoline。
  w_stvec((uint64)supervisor_trap);

  // 保存用户态 PC（sepc）到 trapframe。返回用户态时 usertrapret 会从这里恢复，
  // 所以如果下面改了 epc（比如 syscall 后 +4），返回的用户 PC 就相应变化。
  p->trapframe->epc = r_sepc();

  // ---- 分支一：外部设备中断（scause=9，PLIC 汇聚的设备中断）----
  if (is_external_interrupt(cause)) {
    // devintr 内部会去 PLIC claim IRQ、分发到 UART/磁盘等驱动、再 complete
    devintr();

  // ---- 分支二：定时器中断（scause=5，时间片到期）----
  } else if (is_timer_interrupt(cause)) {
    clockintr();              // 处理时钟中断本身（re-arm stimecmp、ticks++ 等）

    // 统计本进程占用 CPU 的 tick 数，以及本时间片已用 tick 数（MLFQ 判断用）
    p->cpu_ticks++;
    p->last_slice_ticks++;

    // ---- 按策略决定是否抢占/降级 ----
    if (LAB4_POLICY_ID <= 2) {
      // MLFQ 策略：level 越高优先级越低
      // 总级数 = POLICY_ID + 2（策略 0/1/2 分别对应 2/3/4 级）
      int levels = LAB4_POLICY_ID + 2;

      // 当前级的时间片 = 基础 tick 数 << level（level 越低，时间片越短，优先级越高）
      int quantum = LAB2_TICK << p->mlfq_level;

      // 本片用完：降级（若还没到最低级）并让出 CPU
      if (p->last_slice_ticks >= quantum) {
        if (p->mlfq_level + 1 < levels)
          p->mlfq_level++;      // 降到更低优先级队列
        yield();                // 主动让出 CPU，让调度器重新选择
      }
      // 注：如果本片还没用完，就不 yield，继续让用户态跑
    } else {
      // 策略 3/4（彩票/stride）：每个 tick 都 yield，实现"时间片轮转"
      // 保证调度器能按票数/pass 公平地重新分配 CPU
      yield();
    }

  // ---- 分支三：系统调用（scause=8，用户态 ecall）----
  } else if (cause == 8) {
    // 如果进程在进入 trap 前已被标记 killed，不必执行系统调用，直接退出
    if (p->killed)
      proc_exit(-1);

    // 用户态 ecall 是 4 字节指令，返回后要跳过它，否则会无限循环
    p->trapframe->epc += 4;

    // 处理 syscall 期间允许中断（系统调用可能耗时长，如磁盘 I/O）
    intr_on();

    // 分发系统调用，返回值写到 a0（RISC-V 的返回值寄存器）
    p->trapframe->a0 = (uint64)syscall_dispatch(p->trapframe);

  // ---- 分支四：其他异常/缺页 ----
  } else {
    // 打印诊断信息：cause、出错的用户 PC、出错的地址（stval）
    printf("user trap fault scause=%lx sepc=%lx stval=%lx\n",
           cause, r_sepc(), r_stval());
    // 异常通常无法恢复，直接以 scause 作为退出码退出进程
    proc_exit((int)cause);
  }

  // ---- 统一的退出检查 ----
  // 如果处理过程中（或中断到来时）进程被标记 killed（比如别的进程发了 kill），
  // 现在退出，不再返回用户态继续跑
  if (p->killed)
    proc_exit(-1);

  // ---- 返回用户态 ----
  // 构造 sret 现场：恢复 sstatus、sepc、satp、stvec 等，
  // 最后 sret 从 S 态跳回 U 态，用户程序从 trapframe->epc 继续
  usertrapret();
}

void
kerneltrap(void)
{
  uint64 sepc = r_sepc();
  uint64 status = r_sstatus();
  uint64 cause = r_scause();

  if (!(status & SSTATUS_SPP))
    panic("kerneltrap privilege");
  if (is_timer_interrupt(cause)) {
    clockintr();
  } else if (is_external_interrupt(cause)) {
    devintr();
  } else {
    printf("kernel trap scause=%lx sepc=%lx stval=%lx\n",
           cause, sepc, r_stval());
    panic("kerneltrap");
  }
  w_sepc(sepc);
  w_sstatus(status);
}

void
usertrapret(void)
{
  struct proc *p = myproc();
  if (!p)
    panic("usertrapret no proc");
  intr_off();

  p->trapframe->kernel_satp = MAKE_SATP(kernel_pagetable());
  p->trapframe->kernel_sp = p->kstack + PGSIZE;
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();

  uint64 status = r_sstatus();
  status &= ~SSTATUS_SPP;
  status |= SSTATUS_SPIE;
  w_sstatus(status);
  w_stvec(TRAMPOLINE);
  w_sepc(p->trapframe->epc);

  ((void (*)(uint64))userret_va())(MAKE_SATP(p->pagetable));
  panic("userret returned");
}
