#ifndef LAB2_TRAP_H
#define LAB2_TRAP_H

/*
 * LAB2-STAGE1
 *
 * This header describes the C-side view of the memory block used by
 * lab2/gifts/kernel/trampoline.S.  The assembly file does not know these
 * field names; it accesses the block only through fixed byte offsets such as
 * "sd ra, 40(a0)" and "ld sp, 8(a0)".
 */

#include "types.h"

/*
 * LAB2-STAGE1: trapframe is the saved user-mode register image plus the
 * kernel-side values that uservec needs before it can call usertrap().
 *
 * The first five words are not user registers:
 *   0   kernel_satp   kernel page-table value
 *   8   kernel_sp     current process's kernel-stack top
 *   16  kernel_trap   address of usertrap()
 *   24  epc           saved user PC, maintained by C code
 *   32  kernel_hartid hart id to restore into tp
 *
 * Starting at offset 40, the order follows the stores in uservec exactly.
 * Do not reorder fields or insert padding: the assembly uses numeric offsets.
 */
struct trapframe {
  uint64 kernel_satp;       /* 0: value loaded into satp by uservec */
  uint64 kernel_sp;         /* 8: value loaded into sp by uservec */
  uint64 kernel_trap;       /* 16: target loaded into t0 before jalr */
  uint64 epc;               /* 24: C-side saved sepc / return PC */
  uint64 kernel_hartid;     /* 32: value loaded into tp by uservec */

  uint64 ra;                /* 40: user return-address register */
  uint64 sp;                /* 48: user stack pointer */
  uint64 gp;                /* 56: user global pointer */
  uint64 tp;                /* 64: user thread pointer */
  uint64 t0;                /* 72: user temporary register */
  uint64 t1;                /* 80: user temporary register */
  uint64 t2;                /* 88: user temporary register */
  uint64 s0;                /* 96: user saved register */
  uint64 s1;                /* 104: user saved register */
  uint64 a0;                /* 112: syscall argument/return value */
  uint64 a1;                /* 120: syscall argument 2 */
  uint64 a2;                /* 128: syscall argument 3 */
  uint64 a3;                /* 136: user argument register */
  uint64 a4;                /* 144: user argument register */
  uint64 a5;                /* 152: user argument register */
  uint64 a6;                /* 160: user argument register */
  uint64 a7;                /* 168: syscall number */
  uint64 s2;                /* 176: user saved register */
  uint64 s3;                /* 184: user saved register */
  uint64 s4;                /* 192: user saved register */
  uint64 s5;                /* 200: user saved register */
  uint64 s6;                /* 208: user saved register */
  uint64 s7;                /* 216: user saved register */
  uint64 s8;                /* 224: user saved register */
  uint64 s9;                /* 232: user saved register */
  uint64 s10;               /* 240: user saved register */
  uint64 s11;               /* 248: user saved register */
  uint64 t3;                /* 256: user temporary register */
  uint64 t4;                /* 264: user temporary register */
  uint64 t5;                /* 272: user temporary register */
  uint64 t6;                /* 280: user temporary register */
};

/*
 * LAB2-STAGE1 self-test objects.
 *
 * main() fills this trapframe by hand before the first usertrapret().
 * The trampoline reaches it through the fixed virtual address TRAPFRAME.
 */
extern struct trapframe stage1_tf;
extern char stage1_user_entry[];

/* The one-page user stack used by the stage-1 hand-built address space. */
#define STAGE1_USER_STACK 0x4000UL

/* LAB2-STAGE1: build the first user address space and trap entry. */
void trap_init(void);

/* LAB2-STAGE1: handle one trap after uservec switches to S-mode context. */
void usertrap(void);

/*
 * LAB2-STAGE1: prepare CSR/trapframe state, then enter trampoline userret.
 * This function does not itself execute sret; userret does that in assembly.
 */
void usertrapret(void) __attribute__((noreturn));

#endif
