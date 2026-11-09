#ifndef LAB4_PROC_H
#define LAB4_PROC_H

#include "types.h"
#include "riscv.h"
#include "param.h"
#include "spinlock.h"
#include "trap.h"

struct context {
  uint64 ra;
  uint64 sp;
  uint64 s0;
  uint64 s1;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
};

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

struct proc {
  struct spinlock lock;
  enum procstate state;
  void *chan;
  int killed;
  int xstate;
  int pid;

  struct proc *parent;
  uint64 kstack;
  uint64 sz;
  uint64 base;
  pagetable_t pagetable;
  struct trapframe *trapframe;
  struct context context;
  char name[16];

  int cpu_ticks;
  int last_slice_ticks;
  int tickets;
  int tickets_set;
  uint64 pass;
  int mlfq_level;
  int priority;
  int wait_ticks;
};

struct cpu {
  struct proc *proc;
  struct context context;
  int noff;
  int intena;
};

extern struct cpu cpus[NCPU];
extern struct proc proc[NPROC];
extern uint ticks;

struct cpu *mycpu(void);
struct proc *myproc(void);
int cpuid(void);

void procinit(void);
void userinit(void);
void scheduler(void) __attribute__((noreturn));
void sched(void);
void yield(void);
void sleep(void *chan, struct spinlock *lk);
void wakeup(void *chan);
void clockintr(void);
void timerinit(void);
void procdump(void);

int proc_fork(void);
int proc_wait(uint64 status_addr);
int proc_exec(uint64 name_addr);
int proc_sbrk(int n);
void proc_exit(int status) __attribute__((noreturn));
int proc_kill(int pid);
int proc_pause(int n);
int proc_schedstat(int pid, int *cpu_ticks, int *last_slice_ticks);
int proc_settickets(int tickets);

void forkret(void) __attribute__((noreturn));
void swtch(struct context *old, struct context *new);

#endif
