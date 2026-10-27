/*
 * LAB3 用户地址空间和简化进程接口。
 *
 * 当前实验还没有完整进程表和调度器，因此 stage4_* 使用 exec.c 中
 * 的少量全局状态模拟父子进程切换；但它们实际创建、复制和释放的都
 * 是 Lab3 的真实用户页表和物理页。
 */
#ifndef LAB2_EXEC_H
#define LAB2_EXEC_H

#include "types.h"

struct trapframe;

/*
 * LAB2-STAGE4：阶段四的最小用户程序运行时接口。
 *
 * 本实验还没有正式的进程表和调度器，因此这些接口围绕一个
 * “当前用户现场”工作：启动时装载 sh；Shell fork 后暂存父现场，
 * 让子现场 exec 运行 hi 或 spin；子程序 exit 后恢复父现场。
 */
/* 创建第一个用户地址空间，并把 sh 设置为首次运行的程序。 */
void stage4_bootstrap(struct trapframe *tf);
/* 深拷贝当前地址空间，同时保存父进程的 trapframe 和运行状态。 */
long stage4_fork(struct trapframe *tf);
/* 等待简化子进程结束，并把状态写回用户地址。 */
long stage4_wait(struct trapframe *tf);
/* 装载新程序；只有新地址空间完全成功后才释放旧空间。 */
long stage4_exec(struct trapframe *tf);
/* 按当前实验的 EAGER 策略扩展或收缩用户堆。 */
long stage4_sbrk(struct trapframe *tf);
/* 返回当前简化进程编号。 */
int stage4_getpid(void);
/* 用户页错误的统一处理入口。 */
void stage4_fault(int cause) __attribute__((noreturn));
/* 释放子进程空间并恢复保存的父进程现场。 */
void stage4_exit(struct trapframe *tf, int status)
    __attribute__((noreturn));

#endif
