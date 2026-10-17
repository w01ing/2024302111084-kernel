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

struct trapframe stage1_tf __attribute__((aligned(16)));
static uint8 kernel_stack[PGSIZE] __attribute__((aligned(PGSIZE)));
static uint64 userret_va(void){return TRAMPOLINE+((uint64)userret-(uint64)trampoline);}

void trap_init(void){
  vm_init();
  stage1_tf.kernel_satp=MAKE_SATP(kernel_pagetable());
  stage1_tf.kernel_sp=(uint64)&kernel_stack[PGSIZE];
  stage1_tf.kernel_trap=(uint64)usertrap;
  stage1_tf.kernel_hartid=0;
  w_stvec(TRAMPOLINE);
}

int trap_map_user_image(uint64 a,uint64 b,uint64 c,uint64 d){(void)a;(void)b;(void)c;(void)d;return -1;}

void usertrap(void){
  uint64 cause=r_scause(); w_stvec((uint64)supervisor_trap);
  if((cause>>63)&&(cause&0xfff)==9){if(devintr()){usertrapret();return;}}
  if(cause==8){stage1_tf.epc=r_sepc()+4;stage1_tf.a0=(uint64)syscall_dispatch(&stage1_tf);usertrapret();}
  printf("user trap fault scause=%lx sepc=%lx stval=%lx\n",cause,r_sepc(),r_stval());
  stage4_fault((int)cause);
}

void kerneltrap(void){uint64 c=r_scause();if((c>>63)&&(c&0xfff)==9&&devintr())return;printf("kernel trap scause=%lx sepc=%lx stval=%lx\n",c,r_sepc(),r_stval());for(;;)asm volatile("wfi");}

void usertrapret(void){uint64 s=r_sstatus();s&=~SSTATUS_SPP;s|=SSTATUS_SPIE;w_sstatus(s);w_stvec(TRAMPOLINE);w_sepc(stage1_tf.epc);stage1_tf.kernel_satp=MAKE_SATP(kernel_pagetable());stage1_tf.kernel_trap=(uint64)usertrap;((void(*)(uint64))userret_va())(MAKE_SATP(user_pagetable()));for(;;);}
