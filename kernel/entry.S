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
        # LAB2-STAGE1:
        # Stage 1 only tests traps originating from user mode.  A trap taken
        # while already in S mode has no kernelvec implementation yet, so
        # park the hart instead of pretending that it was handled.
        wfi
        j       supervisor_trap

# LAB2-STAGE1:
# 这是阶段 1保留的最小 ecall 往返程序，专门验证：
#   用户态 ecall -> uservec -> usertrap -> usertrapret -> userret -> sret。
# 阶段 2的入口在下面另行定义，避免排障时丢失阶段 1基线。
.section .text.stage1_user
.align 2
.globl stage1_user_entry
.type stage1_user_entry, @function
stage1_user_entry:
        # LAB2-STAGE1：调用约定中的已知调用 SYS_sync=22。
        li      a7, 22
        ecall

        # LAB2-STAGE1：未知号必须返回 -1，不能重复执行同一条 ecall。
        li      a7, 99
        ecall

.Lstage1_user_wait:
        # 阶段 1返回用户态后停在普通跳转循环中。
        j       .Lstage1_user_wait

# LAB2-STAGE2：
# 阶段 2用户态自测按真实 RISC-V syscall ABI 依次执行：
#   getpid() -> write(1, message, length) -> unknown syscall -> exit(0)
#
# 每次 ecall 之前，调用号写入 a7，参数写入 a0-a2；uservec 会把它们
# 保存到 trapframe，syscall.c 再从 trapframe 取出并写回返回值 a0。
.section .text.stage2_user
.align 2
# 消息 "stage2: write says hi\n" 不包含结尾 '\0' 时正好是 22 字节。
.equ STAGE2_MESSAGE_LEN, 22
.globl stage2_user_entry
.type stage2_user_entry, @function
stage2_user_entry:
        # LAB2-STAGE2：getpid()，无参数，返回值由内核写回 a0。
        li      a7, 11
        ecall

        # LAB2-STAGE2：write(1, stage2_message, STAGE2_MESSAGE_LEN)。
        # a0=标准输出 fd，a1=用户缓冲区地址，a2=有效字节数。
        li      a0, 1
        la      a1, stage2_message
        li      a2, STAGE2_MESSAGE_LEN
        li      a7, 16
        ecall

        # LAB2-STAGE2：非法调用仍必须返回 -1，作为容错回归检查。
        li      a7, 99
        ecall

        # LAB2-STAGE2：exit(0) 不返回用户态，内核停在自测终点。
        li      a0, 0
        li      a7, 2
        ecall

        # 理论上 exit 不返回；若实现错误，进入安全死循环而非乱跑。
.Lstage2_user_wait:
        j       .Lstage2_user_wait

.section .rodata.stage2_user
.align 2
stage2_message:
        .asciz  "stage2: write says hi\n"

.section .bss
.balign 16
.globl stack_bottom
stack_bottom:
        .space  (LAB1_STACK_KB * 1024)
.globl stack_top
stack_top:
