/*
 * Machine-mode bootstrap and the one-way transition to supervisor mode.
 */

#include "types.h"
#include "riscv.h"

#define MSTATUS_MIE (1L << 3)

extern void main(void);
extern void supervisor_trap(void);

__attribute__((noreturn)) void
start(void)
{
  uint64 x;

  /*
   * Delegate ordinary traps and interrupts before mret.  The lab does not
   * enable device interrupts yet, but this establishes the S-mode contract
   * used by later labs.
   */
  w_medeleg(0xffff);
  w_mideleg(0xffff);
  w_mcounteren(0x7);

  /*
   * QEMU's virt RAM and MMIO devices must be executable/readable/writable
   * from S mode.  NAPOT with the maximal address covers the whole physical
   * address space.
   */
  w_pmpaddr0(0x3fffffffffffffull);
  w_pmpcfg0(0xf);

  w_stvec((uint64)supervisor_trap);
  w_mepc((uint64)main);

  x = r_mstatus();
  x &= ~MSTATUS_MPP_MASK;
  x |= MSTATUS_MPP_S;
  x &= ~MSTATUS_MIE;
  w_mstatus(x);

  asm volatile("mret" ::: "memory");
  for (;;)
    ;
}
