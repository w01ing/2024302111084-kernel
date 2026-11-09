#ifndef LAB4_SPINLOCK_H
#define LAB4_SPINLOCK_H

#include "types.h"

struct cpu;

struct spinlock {
  uint locked;       // 锁是否被持有
  char *name;        // 锁的名字（调试用）
  struct cpu *cpu;   // 持有该锁的 CPU
};

void initlock(struct spinlock *lk, const char *name);
void acquire(struct spinlock *lk);
void release(struct spinlock *lk);
int holding(struct spinlock *lk);
void push_off(void);
void pop_off(void);
void panic(const char *message) __attribute__((noreturn));

#endif
