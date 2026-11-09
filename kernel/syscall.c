#include "types.h"
#include "riscv.h"
#include "trap.h"
#include "syscall.h"
#include "console.h"
#include "proc.h"
#include "vm.h"

#define STDOUT_FD 1
#define STDERR_FD 2
#define SYSCALL_IO_MAX 4096

static int
argint(struct trapframe *tf, int index, int *out)
{
  uint64 value;
  if (index == 0)
    value = tf->a0;
  else if (index == 1)
    value = tf->a1;
  else if (index == 2)
    value = tf->a2;
  else
    return -1;
  *out = (int)value;
  return 0;
}

static int
argaddr(struct trapframe *tf, int index, uint64 *out)
{
  if (index == 0)
    *out = tf->a0;
  else if (index == 1)
    *out = tf->a1;
  else if (index == 2)
    *out = tf->a2;
  else
    return -1;
  return 0;
}

static long
sys_write(struct trapframe *tf)
{
  int fd, count;
  uint64 address;
  char buffer[256];
  if (argint(tf, 0, &fd) < 0 || argaddr(tf, 1, &address) < 0 ||
      argint(tf, 2, &count) < 0)
    return -1;
  if ((fd != STDOUT_FD && fd != STDERR_FD) || count < 0 ||
      count > SYSCALL_IO_MAX || count > (int)sizeof(buffer))
    return -1;
  if (count == 0)
    return 0;
  if (!address || copyin(buffer, address, count) < 0)
    return -1;
  return console_write_raw(buffer, count);
}

static long
sys_read(struct trapframe *tf)
{
  int fd, count;
  uint64 address;
  char buffer[256];
  if (argint(tf, 0, &fd) < 0 || argaddr(tf, 1, &address) < 0 ||
      argint(tf, 2, &count) < 0)
    return -1;
  if (fd != 0 || count < 0 || count > (int)sizeof(buffer))
    return -1;
  if (count == 0)
    return 0;
  if (!address)
    return -1;
  int n = console_read(buffer, count);
  if (n < 0 || copyout(address, buffer, n) < 0)
    return -1;
  return n;
}

static long
sys_schedstat(struct trapframe *tf)
{
  int pid, cpu_ticks, last_slice_ticks;
  uint64 cpu_addr, slice_addr;
  if (argint(tf, 0, &pid) < 0 || argaddr(tf, 1, &cpu_addr) < 0 ||
      argaddr(tf, 2, &slice_addr) < 0 || !cpu_addr || !slice_addr)
    return -1;
  if (proc_schedstat(pid, &cpu_ticks, &last_slice_ticks) < 0)
    return -1;
  if (copyout(cpu_addr, &cpu_ticks, sizeof(cpu_ticks)) < 0 ||
      copyout(slice_addr, &last_slice_ticks, sizeof(last_slice_ticks)) < 0)
    return -1;
  return 0;
}

long
syscall_dispatch(struct trapframe *tf)
{
  int value;
  switch (tf->a7) {
  case SYS_fork:
    return proc_fork();
  case SYS_exit:
    if (argint(tf, 0, &value) < 0)
      value = -1;
    proc_exit(value);
  case SYS_wait:
    return proc_wait(tf->a0);
  case SYS_read:
    return sys_read(tf);
  case SYS_kill:
    return argint(tf, 0, &value) < 0 ? -1 : proc_kill(value);
  case SYS_exec:
    return proc_exec(tf->a0);
  case SYS_getpid:
    return myproc()->pid;
  case SYS_sbrk:
    return argint(tf, 0, &value) < 0 ? -1 : proc_sbrk(value);
  case SYS_pause:
    return argint(tf, 0, &value) < 0 ? -1 : proc_pause(value);
  case SYS_uptime:
    return ticks;
  case SYS_write:
    return sys_write(tf);
  case SYS_sync:
    return 0;
  case SYS_schedstat:
    return sys_schedstat(tf);
  case SYS_time_rdtime:
    return (long)r_time();
  case SYS_setickets:
    return argint(tf, 0, &value) < 0 ? -1 : proc_settickets(value);
  default:
    return -1;
  }
}
