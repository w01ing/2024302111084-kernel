#ifndef LAB3_VM_H
#define LAB3_VM_H

#include "types.h"
#include "riscv.h"

#define SBRK_EAGER 1
#define SBRK_LAZY  2

void kinit(void);
void vm_init(void);
void *kalloc(void);
void kfree(void *pa);

pagetable_t kernel_pagetable(void);
pagetable_t user_pagetable(void);
void vm_set_user_pagetable(pagetable_t pt);
pagetable_t vm_new_user(void);
void vm_free_user(pagetable_t pt, uint64 sz);
int vm_map(pagetable_t pt, uint64 va, uint64 size, uint64 pa, uint64 perm);
int vm_unmap(pagetable_t pt, uint64 va, uint64 npages, int do_free);
int vm_copy(pagetable_t dst, pagetable_t src, uint64 sz);
uint64 walkaddr(pagetable_t pt, uint64 va, int write);
int copyin(void *dst, uint64 srcva, uint64 len);
int copyout(uint64 dstva, const void *src, uint64 len);
int copyinstr(char *dst, uint64 srcva, uint64 max);
void dump_pagetable(pagetable_t pt);

#endif
