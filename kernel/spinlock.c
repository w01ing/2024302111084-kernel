#include "types.h"
#include "riscv.h"
#include "proc.h"
#include "spinlock.h"
#include "console.h"

void
panic(const char *message)
{
  intr_off();
  console_puts_raw("panic: ");
  console_puts_raw(message);
  console_puts_raw("\n");
  for (;;)
    asm volatile("wfi");
}

void
initlock(struct spinlock *lk, const char *name)
{
  lk->name = name;   // 人类可读的名字，仅用于调试/panic 输出
  lk->locked = 0;    // 真正的锁标志位：0 = 未持有，1 = 已持有
  lk->cpu = 0;       // 指向当前持有该锁的 cpu 结构体
}

void
push_off(void)
{
  int old = intr_get();
  intr_off();
  struct cpu *c = mycpu();
  if (c->noff == 0)
    c->intena = old;
  c->noff++;
}

void
pop_off(void)
{
  struct cpu *c = mycpu();
  if (intr_get())
    panic("pop_off interruptible");
  if (c->noff < 1)
    panic("pop_off");
  c->noff--;
  if (c->noff == 0 && c->intena)
    intr_on();
}

int
holding(struct spinlock *lk)
{
  return lk->locked && lk->cpu == mycpu();
}

void
acquire(struct spinlock *lk)
{
  push_off();
  if (holding(lk))
    panic("acquire");
  while (__sync_lock_test_and_set(&lk->locked, 1) != 0)
    ;
  __sync_synchronize();
  lk->cpu = mycpu();
}

void
release(struct spinlock *lk)
{
  if (!holding(lk))
    panic("release");
  lk->cpu = 0;
  __sync_synchronize();
  __sync_lock_release(&lk->locked);
  pop_off();
}
