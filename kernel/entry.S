/*
 * Lab1 启动入口路径。
 *
 * QEMU 从 0x80000000 开始执行内核的第一条指令，初始处于 M 态。
 * 准备机器状态期间保持中断关闭，然后通过 mret 在 start() 处进入
 * S 态。本实验只有 hart 0 使用启动栈，其他 hart 暂时停车，直到
 * 后续实验加入 SMP 支持。
 */

#include "course_sid.h"

.section .text.entry
.globl _entry
.type _entry, @function
_entry:
        csrr    t0, mhartid
        bnez    t0, .Lpark

        la      sp, stack_top

        # M 态到 S 态的切换尚未完成时，不允许机器中断进入。
        csrw    mie, zero
        csrr    t0, mstatus
        li      t1, (1 << 3)          # mstatus.MIE 位
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
        # 关闭中断后不应再发生 M 态 trap。
        wfi
        j       machine_trap

.balign 4
.globl supervisor_trap
.type supervisor_trap, @function
supervisor_trap:
        # LAB2-STAGE3：
        # 这里处理 S 态内核代码运行期间到来的外部中断，例如
        # sys_read() 在 WFI 中等待键盘输入时收到的 UART 中断。
        #
        # 硬件只自动保存 sepc/scause/sstatus，不会保存通用寄存器；
        # 先在当前内核栈上保存完整现场，再调用 kerneltrap()。
        addi    sp, sp, -256
        sd      ra, 0(sp)
        sd      gp, 8(sp)
        sd      tp, 16(sp)
        sd      t0, 24(sp)
        sd      t1, 32(sp)
        sd      t2, 40(sp)
        sd      s0, 48(sp)
        sd      s1, 56(sp)
        sd      a0, 64(sp)
        sd      a1, 72(sp)
        sd      a2, 80(sp)
        sd      a3, 88(sp)
        sd      a4, 96(sp)
        sd      a5, 104(sp)
        sd      a6, 112(sp)
        sd      a7, 120(sp)
        sd      s2, 128(sp)
        sd      s3, 136(sp)
        sd      s4, 144(sp)
        sd      s5, 152(sp)
        sd      s6, 160(sp)
        sd      s7, 168(sp)
        sd      s8, 176(sp)
        sd      s9, 184(sp)
        sd      s10, 192(sp)
        sd      s11, 200(sp)
        sd      t3, 208(sp)
        sd      t4, 216(sp)
        sd      t5, 224(sp)
        sd      t6, 232(sp)

        call    kerneltrap

        ld      ra, 0(sp)
        ld      gp, 8(sp)
        ld      tp, 16(sp)
        ld      t0, 24(sp)
        ld      t1, 32(sp)
        ld      t2, 40(sp)
        ld      s0, 48(sp)
        ld      s1, 56(sp)
        ld      a0, 64(sp)
        ld      a1, 72(sp)
        ld      a2, 80(sp)
        ld      a3, 88(sp)
        ld      a4, 96(sp)
        ld      a5, 104(sp)
        ld      a6, 112(sp)
        ld      a7, 120(sp)
        ld      s2, 128(sp)
        ld      s3, 136(sp)
        ld      s4, 144(sp)
        ld      s5, 152(sp)
        ld      s6, 160(sp)
        ld      s7, 168(sp)
        ld      s8, 176(sp)
        ld      s9, 184(sp)
        ld      s10, 192(sp)
        ld      s11, 200(sp)
        ld      t3, 208(sp)
        ld      t4, 216(sp)
        ld      t5, 224(sp)
        ld      t6, 232(sp)
        addi    sp, sp, 256
        sret

# LAB2-STAGE1：
# 这是阶段一保留的最小 ecall 往返程序，专门验证：
#   用户态 ecall -> uservec -> usertrap -> usertrapret -> userret -> sret。
# 阶段二的入口在下面另行定义，避免排障时丢失阶段一基线。
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
        # 阶段一返回用户态后停在普通跳转循环中。
        j       .Lstage1_user_wait

# LAB2-STAGE2：
# 阶段二用户态自测按真实 RISC-V syscall ABI 依次执行：
#   getpid() -> write(1, message, length) -> 未知系统调用 -> exit(0)
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

# LAB2-STAGE3：
# 阶段三用户态自测调用 read(0, buf, 64)，从 UART 行缓冲读到一行后，
# 再调用 write(1, buf, n) 原样回显，最后 exit(0)。
.section .text.stage3_user
.align 2
.globl stage3_user_entry
.type stage3_user_entry, @function
stage3_user_entry:
        # read(0, stage3_buffer, 64)
        li      a0, 0
        la      a1, stage3_buffer
        li      a2, 64
        li      a7, 5
        ecall

        # 将 read 返回的字节数暂存到 t0，准备组成 write 参数。
        mv      t0, a0
        blez    t0, .Lstage3_exit
        li      a0, 1
        la      a1, stage3_buffer
        mv      a2, t0
        li      a7, 16
        ecall

.Lstage3_exit:
        li      a0, 0
        li      a7, 2
        ecall

.Lstage3_user_wait:
        j       .Lstage3_user_wait

.section .rodata.stage2_user
.align 2
stage2_message:
        .asciz  "stage2: write says hi\n"

.section .bss
.balign 16
# LAB2-STAGE3：read/write 自测共享的用户缓冲区。
.globl stage3_buffer
stage3_buffer:
        .space 64

.balign 16
.globl stack_bottom
stack_bottom:
        .space  (LAB1_STACK_KB * 1024)
.globl stack_top
stack_top:
