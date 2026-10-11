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
void stage4_bootstrap(struct trapframe *tf);
long stage4_fork(struct trapframe *tf);
long stage4_wait(struct trapframe *tf);
long stage4_exec(struct trapframe *tf);
int stage4_getpid(void);
void stage4_exit(struct trapframe *tf, int status)
    __attribute__((noreturn));

#endif
