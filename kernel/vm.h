/*
 * LAB3 虚拟内存接口。
 *
 * vm.c 同时负责四类工作：物理页分配、三级页表操作、用户地址空间
 * 生命周期管理，以及内核访问用户地址时的权限检查和跨页复制。
 * kalloc 返回物理地址；内核页表建立了 RAM 直接映射，所以内核可以
 * 通过这个地址读写页内容。
 */
#ifndef LAB3_VM_H
#define LAB3_VM_H

#include "types.h"
#include "riscv.h"

struct trapframe;

#define SBRK_EAGER 1
#define SBRK_LAZY  2

/* 初始化空闲物理页链表和课程参数指定的可分配范围。 */
void kinit(void);
/* 建立内核页表、写入 satp，并开启 Sv39 分页。 */
void vm_init(void);
/* 申请一个清零的物理页；没有空闲页时返回 0。 */
void *kalloc(void);
/* 释放一个物理页，并把它放回空闲链表。 */
void kfree(void *pa);
/* 返回空闲页数量，供自测和 OOM 回滚测试使用。 */
uint64 vm_free_count(void);

/* 当前内核页表和当前简化运行时的用户页表。 */
pagetable_t kernel_pagetable(void);
pagetable_t user_pagetable(void);
void vm_set_user_pagetable(pagetable_t pt);
/* 创建或销毁用户地址空间。 */
pagetable_t vm_new_user(struct trapframe *trapframe);
void vm_free_user(pagetable_t pt, uint64 sz);
/* 查找虚拟地址对应的最后一级 PTE；alloc 控制是否创建中间页表。 */
pte_t *walk(pagetable_t pt, uint64 va, int alloc);
/* 建立连续映射；失败时回滚本次创建的页表结构。 */
int mappages(pagetable_t pt, uint64 va, uint64 size, uint64 pa, uint64 perm);
int vm_map(pagetable_t pt, uint64 va, uint64 size, uint64 pa, uint64 perm);
/* 删除叶子映射；do_free 控制是否释放映射的物理页。 */
int vm_unmap(pagetable_t pt, uint64 va, uint64 npages, int do_free);
/* 将源用户空间逐页深拷贝到目标页表。 */
int vm_copy(pagetable_t dst, pagetable_t src, uint64 sz);
/* 检查用户权限并将用户 VA 转换为内核可访问的 PA。 */
uint64 walkaddr(pagetable_t pt, uint64 va, int write);
/* 在内核缓冲区和用户地址空间之间进行跨页安全复制。 */
int copyin(void *dst, uint64 srcva, uint64 len);
int copyout(uint64 dstva, const void *src, uint64 len);
/* 安全复制 NUL 结尾的用户字符串。 */
int copyinstr(char *dst, uint64 srcva, uint64 max);
/* 递归打印页表层级、映射地址和 R/W/X/U 权限。 */
void dump_pagetable(pagetable_t pt);

#endif
