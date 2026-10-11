#ifndef LAB2_SYSCALL_H
#define LAB2_SYSCALL_H

/*
 * LAB2-STAGE2:
 * 系统调用号必须与 lab2/gifts/kernel/syscall.h 完全一致。
 *
 * 用户态 usys.pl 会把例如 write() 翻译成：
 *   li a7, SYS_write
 *   ecall
 *
 * 因此这里不是随意编号，而是用户桩与内核分发器共同遵守的 ABI。
 * 阶段 2 只实现 getpid、write、exit 和 sync；其余编号先保留，
 * 对应的未实现调用统一返回 -1。
 */
#define SYS_fork   1
#define SYS_exit   2
#define SYS_wait   3
#define SYS_pipe   4
#define SYS_read   5
#define SYS_kill   6
#define SYS_exec   7
#define SYS_fstat  8
#define SYS_chdir  9
#define SYS_dup    10
#define SYS_getpid 11
#define SYS_sbrk   12
#define SYS_pause  13
#define SYS_uptime 14
#define SYS_open   15
#define SYS_write  16
#define SYS_mknod  17
#define SYS_unlink 18
#define SYS_link   19
#define SYS_mkdir  20
#define SYS_close  21
#define SYS_sync   22

/* LAB2-STAGE2：trap.c 把当前 trapframe 交给这里做系统调用分发。 */
struct trapframe;
long syscall_dispatch(struct trapframe *tf);

#endif
