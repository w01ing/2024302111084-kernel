/* LAB3：trap 路径负责在用户页表、内核页表和 trampoline 之间协调切换。 */
#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "trap.h"
#include "syscall.h"
#include "console.h"
#include "exec.h"
#include "vm.h"

extern char trampoline[], userret[];
extern void supervisor_trap(void);
extern void printf(const char *, ...);

/*
 * 当前简化运行时只有一个活动用户进程，所以共享一个 trapframe。
 * trampoline 保存用户寄存器，C 代码修改返回 PC/系统调用返回值，随后
 * userret 再从同一块内存恢复寄存器。
 */
struct trapframe stage1_tf __attribute__((aligned(16)));
static uint8 kernel_stack[PGSIZE] __attribute__((aligned(PGSIZE)));
/*
 * userret 在链接时是内核镜像中的符号；运行时却必须从高地址
 * TRAMPOLINE 执行。用两个符号的差值保留它在 trampoline 页内的偏移。
 */
static uint64 userret_va(void){return TRAMPOLINE+((uint64)userret-(uint64)trampoline);}

/*
 * LAB3：初始化虚拟内存和第一次用户 trap 所需的内核侧现场。
 * vm_init() 会开启内核页表；随后 trampoline 才能在用户 trap 时切回它。
 */
void trap_init(void){
  /* 先建立内核页表并开启分页，后续 trap 才能稳定访问内核代码。 */
  vm_init();
  /* uservec 切入 C 代码前需要知道内核页表、内核栈和 usertrap 地址。 */
  stage1_tf.kernel_satp=MAKE_SATP(kernel_pagetable());
  stage1_tf.kernel_sp=(uint64)&kernel_stack[PGSIZE];
  stage1_tf.kernel_trap=(uint64)usertrap;
  stage1_tf.kernel_hartid=0;
  w_stvec(TRAMPOLINE);
}

int trap_map_user_image(uint64 a,uint64 b,uint64 c,uint64 d){(void)a;(void)b;(void)c;(void)d;return -1;}

/* LAB3：在内核页表/内核栈上处理用户 ecall、设备中断和用户页错误。 */
void usertrap(void){
  /* C 处理阶段使用内核 trap 入口，避免递归回到用户 trampoline。 */
  uint64 cause=r_scause(); w_stvec((uint64)supervisor_trap);
  /* 外部中断交给 UART/PLIC 处理；处理完成后恢复原用户现场。 */
  if((cause>>63)&&(cause&0xfff)==9){if(devintr()){usertrapret();return;}}
  /* ecall 的 sepc 指向 ecall 指令本身，返回时必须跳过一条指令。 */
  if(cause==8){stage1_tf.epc=r_sepc()+4;stage1_tf.a0=(uint64)syscall_dispatch(&stage1_tf);usertrapret();}
  /* 其他原因通常是用户页错误，交给统一的进程终止路径。 */
  printf("user trap fault scause=%lx sepc=%lx stval=%lx\n",cause,r_sepc(),r_stval());
  stage4_fault((int)cause);
}

/* LAB3：处理已经发生在 S 态内核中的外部中断或异常。 */
void kerneltrap(void){uint64 c=r_scause();if((c>>63)&&(c&0xfff)==9&&devintr())return;printf("kernel trap scause=%lx sepc=%lx stval=%lx\n",c,r_sepc(),r_stval());for(;;)asm volatile("wfi");}

/*
 * LAB3：准备从 S 态返回 U 态。
 * 设置用户返回特权级、用户 PC、用户 trap 入口，并通过 trampoline 的
 * userret 切换到当前用户页表；不能在 C 函数中直接写 satp 后继续执行，
 * 因为当前 C 代码的映射会随页表切换而失效。
 */
void usertrapret(void){
  /* 清除 SPP，使 sret 返回 U 态；SPIE 控制返回后的中断使能状态。 */
  uint64 s=r_sstatus();
  s&=~SSTATUS_SPP;
  s|=SSTATUS_SPIE;
  w_sstatus(s);
  /* 下一次用户 trap 仍从用户页表中的高地址 trampoline 进入。 */
  w_stvec(TRAMPOLINE);
  w_sepc(stage1_tf.epc);
  /* 为下一次 uservec 重新填写内核页表和 C 入口。 */
  stage1_tf.kernel_satp=MAKE_SATP(kernel_pagetable());
  stage1_tf.kernel_trap=(uint64)usertrap;
  /* 切换 satp 后 C 代码的地址可能失效，因此必须交给 userret 汇编。 */
  ((void(*)(uint64))userret_va())(MAKE_SATP(user_pagetable()));
  for(;;);
}
