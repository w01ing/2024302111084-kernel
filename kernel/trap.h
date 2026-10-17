#ifndef LAB2_TRAP_H
#define LAB2_TRAP_H

/*
 * LAB2-STAGE1
 *
 * 本头文件描述 lab2/gifts/kernel/trampoline.S 使用的内存块在 C 语言
 * 侧的布局。汇编文件不知道这些字段名，只能通过固定字节偏移访问，
 * 例如 "sd ra, 40(a0)" 和 "ld sp, 8(a0)"。
 */

#include "types.h"

/*
 * LAB2-STAGE1：trapframe 由两部分组成：保存的用户态寄存器现场，
 * 以及 uservec 调用 usertrap() 前需要的内核侧运行环境。
 *
 * 前五个字不是用户寄存器：
 *   0   kernel_satp   内核页表值
 *   8   kernel_sp     当前运行现场的内核栈顶
 *   16  kernel_trap   usertrap() 的地址
 *   24  epc           由 C 代码维护的用户 PC
 *   32  kernel_hartid 要恢复到 tp 的 hart 编号
 *
 * 从偏移 40 开始，字段顺序必须严格对应 uservec 中的保存顺序。
 * 不能调整字段顺序或插入填充，因为汇编使用的是数字偏移。
 */
struct trapframe {
  uint64 kernel_satp;       /* 0：uservec 要写入 satp 的内核页表值 */
  uint64 kernel_sp;         /* 8：uservec 切换到的内核栈顶 */
  uint64 kernel_trap;       /* 16：jalr 前装入 t0 的 usertrap 地址 */
  uint64 epc;               /* 24：C 代码维护的用户返回 PC */
  uint64 kernel_hartid;     /* 32：恢复到 tp 的 hart 编号 */

  uint64 ra;                /* 40：用户返回地址寄存器 */
  uint64 sp;                /* 48：用户栈指针 */
  uint64 gp;                /* 56：用户全局指针 */
  uint64 tp;                /* 64：用户线程指针 */
  uint64 t0;                /* 72：用户临时寄存器 */
  uint64 t1;                /* 80：用户临时寄存器 */
  uint64 t2;                /* 88：用户临时寄存器 */
  uint64 s0;                /* 96：用户保存寄存器 */
  uint64 s1;                /* 104：用户保存寄存器 */
  uint64 a0;                /* 112：系统调用参数/返回值 */
  uint64 a1;                /* 120：系统调用第二个参数 */
  uint64 a2;                /* 128：系统调用第三个参数 */
  uint64 a3;                /* 136：用户参数寄存器 */
  uint64 a4;                /* 144：用户参数寄存器 */
  uint64 a5;                /* 152：用户参数寄存器 */
  uint64 a6;                /* 160：用户参数寄存器 */
  uint64 a7;                /* 168：系统调用号 */
  uint64 s2;                /* 176：用户保存寄存器 */
  uint64 s3;                /* 184：用户保存寄存器 */
  uint64 s4;                /* 192：用户保存寄存器 */
  uint64 s5;                /* 200：用户保存寄存器 */
  uint64 s6;                /* 208：用户保存寄存器 */
  uint64 s7;                /* 216：用户保存寄存器 */
  uint64 s8;                /* 224：用户保存寄存器 */
  uint64 s9;                /* 232：用户保存寄存器 */
  uint64 s10;               /* 240：用户保存寄存器 */
  uint64 s11;               /* 248：用户保存寄存器 */
  uint64 t3;                /* 256：用户临时寄存器 */
  uint64 t4;                /* 264：用户临时寄存器 */
  uint64 t5;                /* 272：用户临时寄存器 */
  uint64 t6;                /* 280：用户临时寄存器 */
};

/*
 * LAB2-STAGE1 自测对象。
 *
 * main() 在第一次调用 usertrapret() 前手工填写这个 trapframe；
 * trampoline 通过固定虚拟地址 TRAPFRAME 访问它。
 */
extern struct trapframe stage1_tf;
/* LAB2-STAGE1：原阶段 1的 ecall 往返自测入口，保留用于排障回归。 */
extern char stage1_user_entry[];

/* LAB2-STAGE2：getpid/write/exit 系统调用自测入口。 */
extern char stage2_user_entry[];

/* LAB2-STAGE3：UART read/write 自测使用的用户入口和缓冲区。 */
extern char stage3_user_entry[];
extern char stage3_buffer[];

/* 阶段一手工构造的用户地址空间使用的一页用户栈。 */
#define STAGE1_USER_STACK 0x4000UL

/* LAB2-STAGE1：建立第一个用户地址空间和 trap 入口。 */
void trap_init(void);

/*
 * LAB2-STAGE4：把当前内嵌用户程序和它的用户栈映射到低地址空间。
 *
 * image_pa/image_size 指向内核中保存的平铺程序镜像；
 * stack_pa/stack_va 指向一页已经分配好的用户栈后备存储。
 * 函数会先清空旧的低地址叶子映射，因此 exec 覆盖程序后不会继续
 * 看见上一个用户程序的旧映射。
 */
int trap_map_user_image(uint64 image_pa, uint64 image_size,
                        uint64 stack_pa, uint64 stack_va);

/* LAB2-STAGE1：uservec 切换到 S 态环境后，由此函数处理一次 trap。 */
void usertrap(void);

/*
 * LAB2-STAGE3：S 态正在执行内核代码时的外部中断入口。
 * entry.S 会保存完整寄存器现场后调用 kerneltrap()。
 */
void kerneltrap(void);

/*
 * LAB2-STAGE1：准备 CSR 和 trapframe 状态，然后进入跳板中的 userret。
 * 本函数不直接执行 sret，真正的寄存器恢复和 sret 由汇编完成。
 */
void usertrapret(void) __attribute__((noreturn));

#endif
