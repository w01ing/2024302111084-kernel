        #
        # Lab3 trampoline 是用户页表和内核页表共同映射的跳板页。
        # 用户 trap 发生时 CPU 仍然使用用户页表，所以入口 uservec 必须
        # 在用户页表中有效；切换 satp 后，返回路径 userret 又必须在内核
        # 页表中继续有效。两套页表把这个页面映射到相同的 TRAMPOLINE VA。
        #
        # 用户 trap 发生时 CPU 仍使用用户页表，因此入口和返回代码必须
        # 在两套页表中拥有相同的虚拟地址，才能安全完成 satp 切换。
        #
        # low-level code to handle traps from user space into
        # the kernel, and returns from kernel to user.
        #
        # the kernel maps the page holding this code
        # at the same virtual address (TRAMPOLINE)
        # in user and kernel space so that it continues
        # to work when it switches page tables.
        # kernel.ld causes this code to start at 
        # a page boundary.
        #

#include "riscv.h"
#include "memlayout.h"

# [lab3 增量包 v2 修复覆盖] 段声明由 .section trampsec 改为携带 "ax"(可分配
# 可执行)属性,并在段首增加 .align 12(4KB 对齐)。原声明生成的段无
# SHF_ALLOC/SHF_EXECINSTR 标志:链接后不进入任何可加载段(符号地址为 0),
# lab3 启用 uservec/userret 时首次陷入将跳转 0 地址。参考库 xv6 依靠其
# kernel.ld 的 *(trampsec) 收集规则规避,本课程预置 kernel.ld 无该规则,
# 故在此修复。段对齐保证跳板页满足页表整页映射前提。
.section trampsec, "ax"
.align 12
.globl trampoline
.globl usertrap
trampoline:
.align 4
.globl uservec
uservec:
        # 进入时 CPU 已从 U 态切到 S 态，但尚未切换页表，也尚未切换栈。
        # sscratch 由 usertrapret 预先放入 trapframe VA，先用它找到保存区。
	#
        # trap.c sets stvec to point here, so
        # traps from user space start here,
        # in supervisor mode, but with a
        # user page table.
        #

        # 先交换 a0 和 sscratch：暂存用户 a0，同时让 a0 指向 trapframe。
        # save user a0 in sscratch so
        # a0 can be used to get at TRAPFRAME.
        csrw sscratch, a0

        # trapframe 虽然位于高地址，但其物理页由当前实现的
        # stage1_tf 提供；每套用户页表把 TRAPFRAME 映射到该现场区域。
        # each process has a separate p->trapframe memory area,
        # but it's mapped to the same virtual address
        # (TRAPFRAME) in every process's user page table.
        li a0, TRAPFRAME
        
        # 按 kernel/trap.h 中 struct trapframe 的固定偏移保存用户寄存器。
        # 这些 sd 指令的偏移必须与 C 结构体字段顺序完全一致。
        # 这些偏移是 C 和汇编之间的 ABI，不能随意调整。
        # save the user registers in TRAPFRAME
        sd ra, 40(a0)
        sd sp, 48(a0)
        sd gp, 56(a0)
        sd tp, 64(a0)
        sd t0, 72(a0)
        sd t1, 80(a0)
        sd t2, 88(a0)
        sd s0, 96(a0)
        sd s1, 104(a0)
        sd a1, 120(a0)
        sd a2, 128(a0)
        sd a3, 136(a0)
        sd a4, 144(a0)
        sd a5, 152(a0)
        sd a6, 160(a0)
        sd a7, 168(a0)
        sd s2, 176(a0)
        sd s3, 184(a0)
        sd s4, 192(a0)
        sd s5, 200(a0)
        sd s6, 208(a0)
        sd s7, 216(a0)
        sd s8, 224(a0)
        sd s9, 232(a0)
        sd s10, 240(a0)
        sd s11, 248(a0)
        sd t3, 256(a0)
        sd t4, 264(a0)
        sd t5, 272(a0)
        sd t6, 280(a0)

	# save the user a0 in p->trapframe->a0
        csrr t0, sscratch
        sd t0, 112(a0)

        # 从 trapframe 取出内核栈、hart 编号、usertrap 地址和内核 satp。
        # 在切换 satp 前先把这些值放入寄存器，避免切换后无法访问用户页表。
        # 当前仍是 S 态，但此时暂时还在用户页表下执行。
        # initialize kernel stack pointer, from p->trapframe->kernel_sp
        ld sp, 8(a0)

        # make tp hold the current hartid, from p->trapframe->kernel_hartid
        ld tp, 32(a0)

        # load the address of usertrap(), from p->trapframe->kernel_trap
        ld t0, 16(a0)

        # fetch the kernel page table address, from p->trapframe->kernel_satp.
        ld t1, 0(a0)

        # 切换 satp 前后都刷新 TLB，防止旧用户页表翻译残留。
        # wait for any previous memory operations to complete, so that
        # they use the user page table.
        sfence.vma zero, zero

        # install the kernel page table.
        csrw satp, t1

        # flush now-stale user entries from the TLB.
        sfence.vma zero, zero

        # 现在已经使用内核页表和内核栈，可以安全进入 C 语言 trap 分发逻辑。
        # call usertrap()
        jalr t0

.globl userret
userret:
        # a0 传入目标用户页表的 satp 值；此时仍在内核页表和内核栈上。
        # usertrap() 返回到这里时，a0 保存目标用户页表的 satp 值。
        # return from kernel to user.

        # flush icache, in case this is the first time
        # we're running this proc on this hart.
        fence.i

        # 切回用户页表并刷新 TLB；之后恢复的用户 PC/栈必须能被查到。
        # switch to the user page table.
        sfence.vma zero, zero
        csrw satp, a0
        sfence.vma zero, zero

        li a0, TRAPFRAME

        # 按与 uservec 相反的布局恢复用户寄存器，a0 最后恢复。
        # a0 最后恢复是因为它在前面的步骤中暂时承载 satp/trapframe 信息。
        # restore all but a0 from TRAPFRAME
        ld ra, 40(a0)
        ld sp, 48(a0)
        ld gp, 56(a0)
        ld tp, 64(a0)
        ld t0, 72(a0)
        ld t1, 80(a0)
        ld t2, 88(a0)
        ld s0, 96(a0)
        ld s1, 104(a0)
        ld a1, 120(a0)
        ld a2, 128(a0)
        ld a3, 136(a0)
        ld a4, 144(a0)
        ld a5, 152(a0)
        ld a6, 160(a0)
        ld a7, 168(a0)
        ld s2, 176(a0)
        ld s3, 184(a0)
        ld s4, 192(a0)
        ld s5, 200(a0)
        ld s6, 208(a0)
        ld s7, 216(a0)
        ld s8, 224(a0)
        ld s9, 232(a0)
        ld s10, 240(a0)
        ld s11, 248(a0)
        ld t3, 256(a0)
        ld t4, 264(a0)
        ld t5, 272(a0)
        ld t6, 280(a0)

	# restore user a0
        ld a0, 112(a0)
        
        # sepc 和 SPP/SPIE 已由 usertrapret 设置，sret 返回用户 PC/U 态。
        # return to user mode and user pc.
        # prepare_return() sets up sstatus and sepc.
        sret
