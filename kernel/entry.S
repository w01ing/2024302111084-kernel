/*
 * Lab 1 entry path.
 *
 * QEMU starts the first instruction in M mode at 0x80000000.  Keep
 * interrupts disabled while the machine state is prepared, then use mret
 * to enter S mode at start().  Only hart 0 owns the bootstrap stack in
 * this lab; other harts park until a later lab adds SMP support.
 */

#include "course_sid.h"

.section .text.entry
.globl _entry
.type _entry, @function
_entry:
        csrr    t0, mhartid
        bnez    t0, .Lpark

        la      sp, stack_top

        # No machine interrupt can arrive while the M -> S handoff is open.
        csrw    mie, zero
        csrr    t0, mstatus
        li      t1, (1 << 3)          # mstatus.MIE
        not     t1, t1
        and     t0, t0, t1
        csrw    mstatus, t0

        la      t0, machine_trap
        csrw    mtvec, t0

        la      t0, start
        csrw    mepc, t0
        jr      t0

.Lpark:
        wfi
        j       .Lpark

.balign 4
.type machine_trap, @function
machine_trap:
        # There should be no M-mode trap after interrupts are disabled.
        wfi
        j       machine_trap

.balign 4
.globl supervisor_trap
.type supervisor_trap, @function
supervisor_trap:
        # Lab 1 has no trap recovery path yet.
        wfi
        j       supervisor_trap

.section .bss
.balign 16
.globl stack_bottom
stack_bottom:
        .space  (LAB1_STACK_KB * 1024)
.globl stack_top
stack_top:
