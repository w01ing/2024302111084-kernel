#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "course_sid.h"
#include "console.h"
#include "exec.h"
#include "proc.h"
#include "spinlock.h"
#include "trap.h"
#include "vm.h"

#define IMAGE_MAX (128 * 1024UL)
#define USER_STACK_PAGE 0x40000UL
#define USER_STACK_TOP  (USER_STACK_PAGE + PGSIZE)
#define STRIDE_BIG 1000000UL
#define TIMER_INTERVAL 100000UL

struct uprog_entry { uint64 start, end; char name[1]; };

extern char _uprog_table[];
extern void printf(const char *, ...);

struct cpu cpus[NCPU];
struct proc proc[NPROC];
uint ticks;

static struct proc *initproc;
static struct spinlock wait_lock;
static struct spinlock pid_lock;
static struct spinlock ticks_lock;
static int nextpid = 1;
static int rr_idx = -1;
static uint64 lottery_state;
static uint8 proc_stacks[NPROC][PGSIZE] __attribute__((aligned(PGSIZE)));

static void
memzero(void *ptr, uint64 n)
{
  uint8 *p = ptr;
  while (n--)
    *p++ = 0;
}

static void
memcopy(void *dst, const void *src, uint64 n)
{
  uint8 *d = dst;
  const uint8 *s = src;
  while (n--)
    *d++ = *s++;
}

static int
same_name(const char *a, const char *b)
{
  for (int i = 0; i < 64; i++) {
    if (a[i] == '\0' || a[i] == '\n' || a[i] == '\r')
      return b[i] == '\0';
    if (a[i] != b[i])
      return 0;
  }
  return 0;
}

static void
set_name(struct proc *p, const char *name)
{
  int i = 0;
  while (i + 1 < (int)sizeof(p->name) && name[i] &&
         name[i] != '\n' && name[i] != '\r') {
    p->name[i] = name[i];
    i++;
  }
  p->name[i] = 0;

  if (!p->tickets_set && (LAB4_POLICY_ID == 5 || LAB4_POLICY_ID == 7)) {
    if (same_name(p->name, "hi"))
      p->tickets = 1;
    else if (same_name(p->name, "spin"))
      p->tickets = 2;
    else if (same_name(p->name, "sh"))
      p->tickets = 4;
  }
}

static struct uprog_entry *
find_program(const char *name)
{
  struct uprog_entry *e = (void *)_uprog_table;
  while (e->start || e->end) {
    if (same_name(name, e->name))
      return e;
    e = (void *)((e->end + 7) & ~7UL);
  }
  return 0;
}

int
cpuid(void)
{
  return (int)r_tp();
}

struct cpu *
mycpu(void)
{
  int id = cpuid();
  if (id < 0 || id >= NCPU)
    panic("cpuid");
  return &cpus[id];
}

struct proc *
myproc(void)
{
  push_off();
  struct proc *p = mycpu()->proc;
  pop_off();
  return p;
}

static int
allocpid(void)
{
  acquire(&pid_lock);
  int pid = nextpid++;
  release(&pid_lock);
  return pid;
}

static uint64
minimum_pass_excluding(struct proc *skip)
{
  uint64 minimum = ~0UL;
  for (int i = 0; i < NPROC; i++) {
    struct proc *p = &proc[i];
    if (p == skip)
      continue;
    acquire(&p->lock);
    if ((p->state == RUNNABLE || p->state == RUNNING) && p->pass < minimum)
      minimum = p->pass;
    release(&p->lock);
  }
  return minimum == ~0UL ? 0 : minimum;
}

static void
freeproc(struct proc *p)
{
  if (p->pagetable)
    vm_free_user(p->pagetable, p->sz);
  if (p->trapframe)
    kfree(p->trapframe);
  p->pagetable = 0;
  p->trapframe = 0;
  p->sz = 0;
  p->base = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->cpu_ticks = 0;
  p->last_slice_ticks = 0;
  p->state = UNUSED;
}

// 从进程表中分配一个空闲进程槽位，做好运行前的内核态初始化。
// 成功返回时：p->state == USED，且 p->lock 仍被持有（调用方负责 release）。
// 失败返回 0（不持有任何锁）。
static struct proc *
allocproc(void)
{
  struct proc *p;

  // 线性扫描进程表，找第一个 state == UNUSED 的槽位。
  // 每个槽位在检查前先 acquire 自己的锁，避免和别的 CPU 同时抢同一个槽。
  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state == UNUSED)
      goto found;          // 找到空闲槽：此时持有 p->lock，直接跳出去
    release(&p->lock);     // 不是空闲：释放锁，继续找下一个
  }
  return 0;                // 进程表满，分配失败

found:
  // 走到这里时 p->lock 已持有，且 state 是 UNUSED。
  // 先把它标记为 USED，防止其他 CPU 再抢到这个槽。
  p->state = USED;

  // 分配一个全局唯一的 pid（内部用 pid_lock 保护 nextpid）
  p->pid = allocpid();

  // 分配 trapframe 所在的一页：保存用户态寄存器、内核栈指针等，
  // trap 进入/返回时靠它保存/恢复现场
  p->trapframe = kalloc();
  if (!p->trapframe) {     // 内存不足：回滚已分配的资源
    freeproc(p);           // freeproc 会把 state 改回 UNUSED、释放已分配页
    release(&p->lock);
    return 0;
  }

  // 创建该进程的用户页表（vm_new_user 内部通常会映射 trampoline、
  // trapframe 等内核需要的页，并返回 satp 可用的根页表指针）
  p->pagetable = vm_new_user(p->trapframe);
  if (!p->pagetable) {     // 页表创建失败：同样回滚
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // ---- 初始化内核上下文（context）----
  // context 保存的是"内核态"的 callee-saved 寄存器，
  // 第一次被调度器 swtch 切进来时，就从这里恢复执行。
  memzero(&p->context, sizeof(p->context));
  p->context.ra = (uint64)forkret;         // 首次被调度时跳到 forkret（trap 返回用户态前的收尾）
  p->context.sp = p->kstack + PGSIZE;      // 内核栈顶：栈向下增长，从栈顶开始用

  // 彩票调度：每个进程初始彩票数 = 7 + (学号 % 13)，范围 [7, 19]，
  // 用 COURSE_SID 区分不同课程/学生，便于实验和复现
  p->tickets = 7 + COURSE_SID % 13;
  p->tickets_set = 0;                       // 标记"用户是否显式设置过 tickets"，0 表示还没设置

  // stride 调度用的 pass 值：取当前所有可运行进程中（排除自己）最小的 pass，
  // 保证新进程不会因为 pass 过大而长期得不到调度
  p->pass = minimum_pass_excluding(p);

  // MLFQ（多级反馈队列）初始状态：最高优先级队列 0、priority 0
  p->mlfq_level = 0;
  p->priority = 0;

  // 统计/调度辅助字段清零：
  p->wait_ticks = 0;        // 在就绪队列里等待的 tick 数（用于优先级提升/老化）
  p->cpu_ticks = 0;         // 累计占用 CPU 的 tick 数
  p->last_slice_ticks = 0;  // 上一个时间片用了多少 tick（MLFQ 降级判断用）

  // 注意：这里没有 release(&p->lock)！
  // 返回时仍持有锁，调用方（userinit / fork / exec 等）负责在
  // 设置完 state（如 RUNNABLE）后 release。
  return p;
}

static int
load_program(pagetable_t pt, struct trapframe *tf,
             struct uprog_entry *e, uint64 *size_out)
{
  if (!e || e->end < e->start || e->end - e->start > IMAGE_MAX)
    return -1;
  uint64 size = e->end - e->start;
  uint64 rounded = PGROUNDUP(size);

  for (uint64 va = 0; va < rounded; va += PGSIZE) {
    uint64 pa = (uint64)kalloc();
    if (!pa)
      goto bad;
    uint64 n = size - va;
    if (n > PGSIZE)
      n = PGSIZE;
    memcopy((void *)pa, (void *)(e->start + va), n);
    uint64 perm = PTE_U | PTE_R;
    if (va == 0)
      perm |= PTE_X;
    if (va >= 2 * PGSIZE)
      perm |= PTE_W;
    if (vm_map(pt, va, PGSIZE, pa, perm) < 0) {
      kfree((void *)pa);
      goto bad;
    }
  }

  uint64 stack_pa = (uint64)kalloc();
  if (!stack_pa)
    goto bad;
  if (vm_map(pt, USER_STACK_PAGE, PGSIZE, stack_pa,
             PTE_U | PTE_R | PTE_W) < 0) {
    kfree((void *)stack_pa);
    goto bad;
  }

  memzero(tf, sizeof(*tf));
  tf->epc = 0;
  tf->sp = USER_STACK_TOP;
  *size_out = rounded;
  return 0;

bad:
  vm_unmap(pt, 0, rounded / PGSIZE, 1);
  vm_unmap(pt, USER_STACK_PAGE, 1, 1);
  return -1;
}

void
procinit(void)
{
  initlock(&pid_lock, "pid");
  initlock(&wait_lock, "wait");
  initlock(&ticks_lock, "ticks");
  // 彩票调度状态用"读时间戳 ^ 课程号"做随机种子。
  // 这里在单线程启动阶段执行，无竞争，普通写即可；此后由 lottery 相关逻辑更新。
  lottery_state = r_time() ^ COURSE_SID;

  // 遍历进程表，逐个初始化
  for (int i = 0; i < NPROC; i++) {
    // 每个进程一把自己的锁：保护 proc[i] 各字段（state、chan、kstack 等）
    // 注意：必须每把锁单独 init，不能靠 BSS 全零"碰巧"可用
    initlock(&proc[i].lock, "proc");

    // 初始状态为未使用，表示这个槽位可以分配给新进程
    proc[i].state = UNUSED;

    // 记录该进程的内核栈基址。发生 trap 时，硬件/汇编切换到这里，
    // 后续由 usertrapret / kernelvec 等使用
    proc[i].kstack = (uint64)&proc_stacks[i][0];
  }
}

void
userinit(void)
{
  // 从进程表里分配一个进程槽位（allocproc 内部会初始化
  // trapframe、pagetable、kstack 等，并把 state 设为 USED，
  // 最后仍持有 p->lock 返回，需要调用方自己 release）
  struct proc *p = allocproc();
  if (!p)
    panic("userinit allocproc");

  // 在程序表里查找名为 "sh" 的可执行程序
  // （用户程序放在 fs/内存镜像里，用名字索引）
  struct uprog_entry *e = find_program("sh");

  // 把 "sh" 加载到该进程的页表，写入 trapframe 的入口/栈指针，
  // 并通过 p->sz 返回程序占用的虚拟地址大小（即 break 位置）
  // load_program 会把用户栈等安排好，返回 <0 表示加载失败
  if (load_program(p->pagetable, p->trapframe, e, &p->sz) < 0)
    panic("userinit load sh");

  // 记录程序的初始大小：后续 sbrk() 只会在这个基址之上扩展。
  // 也就是说 base 是"堆起点"，sz 会随 sbrk 增长
  p->base = p->sz;

  // 设置进程名字为 "sh"，方便 ps 等调试工具显示
  set_name(p, "sh");

  // 记录全局 initproc 指针（内核可据此找到第一个用户进程，
  // 例如在退出时等待所有子进程清理完）
  initproc = p;

  // 把进程状态设为"可运行"，之后调度器（scheduler）就会把它调度到 CPU 上
  p->state = RUNNABLE;

  // 将该进程的用户页表安装为当前 CPU 的活动页表？
  // 让后续 trap 返回用户态时能直接用上（或者只是建立一个全局映射关系）
  vm_set_user_pagetable(p->pagetable);

  // 把页表内容打印出来，方便确认 sh 被正确加载
  dump_pagetable(p->pagetable);

  // allocproc() 返回时持有 p->lock，这里必须释放，
  // 否则该进程永远不会被调度，且任何想访问它的代码都会阻塞
  release(&p->lock);
}

// 新进程第一次被调度器 swtch 进来时的落脚点。
// 它做两件事：① 释放调度器一直持有的 p->lock；
//             ② 调用 usertrapret() 走标准 trap 返回路径，sret 回到用户态。
// 之后这个函数不会再被进入（每个进程只走一次），
// 进程再次被调度时是从它上次 swtch 出去的地方恢复，而不是 forkret。
void
forkret(void)
{
  struct proc *p = myproc();   // 取当前 CPU 上正在跑的进程（即刚被 swtch 进来的那个）
  release(&p->lock);           // 释放 allocproc/pick_next 一路持有到现在的进程锁
  usertrapret();               // 构造 sret 现场，跳到用户态：trapframe 里已设好用户 PC/SP
}

static uint64
lottery_next(void)
{
  lottery_state = lottery_state * 6364136223846793005UL +
                  1442695040888963407UL;
  return lottery_state >> 16;
}

static struct proc *
pick_rr(void)
{
  for (int n = 1; n <= NPROC; n++) {
    int i = (rr_idx + n) % NPROC;
    struct proc *p = &proc[i];
    acquire(&p->lock);
    if (p->state == RUNNABLE) {
      rr_idx = i;
      return p;
    }
    release(&p->lock);
  }
  return 0;
}

static struct proc *
pick_mlfq(void)
{
  int levels = LAB4_POLICY_ID + 2;
  for (int level = 0; level < levels; level++) {
    for (int n = 1; n <= NPROC; n++) {
      int i = (rr_idx + n) % NPROC;
      struct proc *p = &proc[i];
      acquire(&p->lock);
      if (p->state == RUNNABLE && p->mlfq_level == level) {
        rr_idx = i;
        return p;
      }
      release(&p->lock);
    }
  }
  return 0;
}

// 按彩票调度策略挑选下一个可运行进程。
// 核心思想：每个 RUNNABLE 且 tickets>0 的进程按票数占比例被选中。
// 返回时持有该进程的 p->lock（scheduler 负责后续 release）；无可选进程时退化为轮转。
static struct proc *
pick_lottery(void)
{
  uint64 total = 0;

  // ---- 第一遍：统计所有"可运行且有票"进程的总票数 ----
  // 只统计 state==RUNNABLE 且 tickets>0 的进程：
  //   - 非 RUNNABLE（USED/SLEEPING/ZOMBIE）不参与抽签
  //   - tickets==0 的进程相当于"放弃被调度"，不计入总票数
  for (int i = 0; i < NPROC; i++) {
    acquire(&proc[i].lock);
    if (proc[i].state == RUNNABLE && proc[i].tickets > 0)
      total += (uint64)proc[i].tickets;
    release(&proc[i].lock);
  }

  // 没有任何进程持有有效彩票：彩票调度无从谈起，
  // 退化到轮转调度，保证系统不会因此卡住
  if (total == 0)
    return pick_rr();

  // ---- 抽签：在 [0, total) 里随机取一张票 ----
  // lottery_next() 返回伪随机数，% total 把它映射到总票数区间内。
  // winner 表示"第几张票中奖"，后面用它去各进程的票段里定位
  uint64 winner = lottery_next() % total;

  // ---- 第二遍：找出 winner 落在哪个进程的票段里 ----
  // 用 (rr_idx + n) % NPROC 从 rr_idx 的下一个开始遍历，
  // 让起点轮转，避免每次都从 0 开始导致低索引进程有微小偏置。
  for (int n = 1; n <= NPROC; n++) {
    int i = (rr_idx + n) % NPROC;
    struct proc *p = &proc[i];
    acquire(&p->lock);

    if (p->state == RUNNABLE && p->tickets > 0) {
      // 把每个进程看作长度为 tickets 的区间：
      //   进程 A 占 [0, tA)
      //   进程 B 占 [tA, tA+tB)
      //   ...
      // winner 落在谁的区间里就选谁。
      if (winner < (uint64)p->tickets) {
        rr_idx = i;              // 记住本次选中的位置，作为下次轮转起点
        return p;                // 返回时仍持有 p->lock，符合调度器约定
      }
      // 不在本进程区间：从 winner 中减去本进程的票数，继续找下一个
      winner -= (uint64)p->tickets;
    }
    release(&p->lock);
  }

  // 理论上走不到这里：total 已经保证至少有一个有票进程。
  // 但两次遍历之间进程状态可能变化（被唤醒/睡眠/退出），
  // 导致第二遍没匹配上；兜底退化为轮转，保证一定能返回一个进程或 0。
  return pick_rr();
}
static struct proc *
pick_stride(void)
{
  struct proc *best = 0;
  int best_index = -1;
  uint64 best_pass = ~0UL;
  for (int n = 1; n <= NPROC; n++) {
    int i = (rr_idx + n) % NPROC;
    struct proc *p = &proc[i];
    acquire(&p->lock);
    if (p->state == RUNNABLE && p->pass < best_pass) {
      if (best)
        release(&best->lock);
      best = p;
      best_index = i;
      best_pass = p->pass;
    } else {
      release(&p->lock);
    }
  }
  if (best) {
    rr_idx = best_index;
    int tickets = best->tickets > 0 ? best->tickets : 1;
    best->pass += STRIDE_BIG / (uint64)tickets;
  }
  return best;
}

static struct proc *
pick_priority(void)
{
  struct proc *best = 0;
  int best_index = -1;
  int best_priority = -1;
  for (int n = 1; n <= NPROC; n++) {
    int i = (rr_idx + n) % NPROC;
    struct proc *p = &proc[i];
    acquire(&p->lock);
    int priority = p->priority;
    if (LAB4_POLICY_ID == 9)
      priority += p->wait_ticks / 8;
    if (p->state == RUNNABLE && priority > best_priority) {
      if (best)
        release(&best->lock);
      best = p;
      best_index = i;
      best_priority = priority;
    } else {
      release(&p->lock);
    }
  }
  if (best) {
    rr_idx = best_index;
    best->wait_ticks = 0;
  }
  return best;
}

static struct proc *
pick_next(void)
{
  if (LAB4_POLICY_ID <= 2)
    return pick_mlfq();
  if (LAB4_POLICY_ID <= 5)
    return pick_lottery();
  if (LAB4_POLICY_ID <= 7)
    return pick_stride();
  return pick_priority();
}

// 每个 CPU 核的调度器主循环，永不返回。
// 职责：不断从就绪进程里挑一个，切到它运行；它让出或被抢占后回到这里，继续挑。
void
scheduler(void)
{
  struct cpu *c = mycpu();     // 取当前 CPU 的 cpu 结构体（每核一份，存 proc 指针和 context）
  c->proc = 0;                 // 初始化：当前没有进程在这个核上跑

  for (;;) {
    // ---- 开中断 ----
    // 调度器本身要允许中断：如果关着中断，定时器中断进不来，
    // 就没有"抢占"和"从 wfi 唤醒"，整个系统会卡死。
    // 注意：下面 swtch() 之后会进入进程，进程里可能再关中断；回到这里时重新打开。
    intr_on();

    // ---- 按调度策略挑下一个可运行进程 ----
    // pick_next 内部会实现彩票/stride/MLFQ 等策略，返回时：
    //   - 成功：p->state 已被置为 RUNNABLE（或即将置 RUNNING），且持有 p->lock
    //   - 失败：返回 0（没有可运行进程）
    struct proc *p = pick_next();
    if (!p) {
      // 没有可运行进程：执行 wfi（wait for interrupt）让 CPU 休眠省电，
      // 直到有中断（定时器/外部）到来才继续循环。
      // 不能在这里忙等空转，否则白耗 CPU。
      asm volatile("wfi");
      continue;
    }

    // ---- 准备切换到进程 p ----
    p->state = RUNNING;          // 状态改为"正在运行"（持有 p->lock，安全）
    p->last_slice_ticks = 0;     // 新时间片开始，清零"本片已用 tick"（MLFQ 降级判断用）
    c->proc = p;                 // 记录当前核正在跑哪个进程（mycpu()->proc 查询用）

    // 把这个进程的用户页表设为本核的活动页表。
    // 课程 VM 设计里，这可能是设置 satp、或记录"当前用户页表"供内核访问用户内存用。
    vm_set_user_pagetable(p->pagetable);

    // ---- 上下文切换：从调度器的 context 切到进程的内核 context ----
    // swtch 只保存/恢复 callee-saved 寄存器，不碰页表、不碰 trapframe。
    // 第一次切进新进程时，会从 p->context.ra（= forkret）开始执行；
    // 之后每次进程让出（sleep/yield/exit）都会从它上次 swtch 的地方恢复。
    // 关键：swtch 返回时，说明"某个进程让出了 CPU，回到了调度器"。
    swtch(&c->context, &p->context);

    // ---- 从进程切回调度器之后 ----
    // 此时可能是一个进程调用了 sched()/yield()/sleep()/exit() 主动让出，
    // 也可能是它被抢占后（trap 里调用 yield）间接回到这里。
    c->proc = 0;                 // 当前核不再有运行中的进程
    vm_set_user_pagetable(0);    // 清除活动用户页表（回到内核页表或"无用户页表"状态）

    // 释放刚才那个进程的锁。
    // pick_next 返回时持有 p->lock，swtch 期间一直持有（防止别人改它的状态），
    // 现在它已经让出 CPU，可以安全释放，让其他 CPU 或中断处理能操作它。
    release(&p->lock);
  }
}

// 从当前进程切回调度器：保存进程的内核现场到 p->context，
// 恢复调度器的现场，从 scheduler() 里上次 swtch 的下一句继续。
//
// 调用约定（下面 4 个 panic 就是在检查这些）：
//   1. 必须持有 p->lock
//   2. 当前核的 noff 必须为 1（只有 p->lock 这一层，没嵌套别的锁）
//   3. p->state 必须已改为 RUNNING 以外的值（RUNNABLE/SLEEPING/ZOMBIE）
//   4. 中断必须已关（不能是可中断状态）
// 满足这些后调用 swtch 切到调度器；被再次调度回来时从 swtch 下一行继续。
void
sched(void)
{
  struct proc *p = myproc();
  struct cpu *c = mycpu();

  // ---- 四个前置条件断言，任何一条不满足都是内核 bug ----

  // ① 必须持有 p->lock：保护 state/chan/context 的修改不被其他核并发干扰
  if (!holding(&p->lock))
    panic("sched p->lock");

  // ② noff == 1：表示只持有一把锁（p->lock）且只关了一层中断。
  //    - 如果 noff > 1，说明还持有别的锁；在持有多把锁的情况下让出 CPU，
  //      那些锁会被"锁住"直到进程再次被调度，可能造成死锁/长延迟；
  //    - 如果 noff == 0，说明没关中断，切出去后可能被中断打断导致状态不一致。
  //    xv6 要求 sched 时只持有 p->lock 这一把锁。
  if (c->noff != 1)
    panic("sched locks");

  // ③ state 不能还是 RUNNING：调用方（sleep/yield/exit）必须先把
  //    state 改成 SLEEPING / RUNNABLE / ZOMBIE，否则调度器会误以为
  //    这个进程还在跑，可能又把它调度出去，造成"两个 CPU 同时跑同一进程"。
  if (p->state == RUNNING)
    panic("sched running");

  // ④ 中断必须关着：如果开着中断就切出去，
  //    可能在 swtch 恢复调度器现场的过程中被中断打断，
  //    此时栈/寄存器处于"半切换"状态，极易崩溃。
  if (intr_get())
    panic("sched interruptible");

  // ---- 保存/恢复中断使能状态，然后切换 ----
  // intena 记录"切进调度器之前，中断本来是开的还是关的"。
  // 因为调度器内部要 intr_on()，会改变中断状态；
  // 等本进程再次被调度、从 swtch 返回时，需要用 intena 恢复成原来的状态，
  // 保证"从进程视角看，中断状态前后一致"。
  int intena = c->intena;

  // 核心：保存当前进程内核现场到 p->context，
  // 恢复调度器现场 c->context，跳回 scheduler() 里 swtch 的下一句。
  // 等本进程将来再次被调度，会从这里"返回"（swtch 另一侧执行）。
  swtch(&p->context, &c->context);

  // ---- 再次被调度、切回来之后才执行到这里 ----
  // 恢复中断使能状态：切出去前如果中断是开的，这里就打开；
  // 如果是关的，保持关。保证 sched 返回后，调用方看到的中断状态与调用前一致。
  c->intena = intena;
}

void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// 让当前进程在通道 chan 上睡眠，直到被 wakeup(chan) 唤醒。
// 调用约定：
//   - 调用者必须持有 lk（通常是保护"睡眠条件"的那把锁）；
//   - sleep 内部会先拿 p->lock、再放 lk，然后进入睡眠；
//   - 返回时重新持有 lk，且调用者醒来后要重新检查条件（可能被虚假唤醒/条件已变）。
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  // 防御性检查：没有当前进程或没传锁，属于内核 bug
  if (!p || !lk)
    panic("sleep");

  // ---- 关键顺序：先 acquire(p->lock)，再 release(lk) ----
  // 这两步的顺序不能反！原因见下面"lost wakeup"分析：
  //   - 拿 p->lock 是为了让"设置 SLEEPING"和"sched() 让出 CPU"之间
  //     不被其他核的 wakeup 抢先（wakeup 改 state 也要拿 p->lock）；
  //   - 放 lk 是因为一旦进入 SLEEPING、即将让出 CPU，
  //     就不该再占着保护条件的那把锁，否则别人无法改变条件、无法 wakeup。
  acquire(&p->lock);
  release(lk);

  // 记录在哪个通道上睡，供 wakeup 匹配
  p->chan = chan;

  // 标记为 SLEEPING：调度器从此不会再挑它，直到有人把它改回 RUNNABLE
  p->state = SLEEPING;

  // ---- 彩票调度的补偿 ----
  // 策略 4 下，进程睡眠时给它"加票"（上限 32），
  // 目的是补偿 I/O 密集/频繁睡眠的进程，避免它们票数被 CPU 密集进程越拉越远，
  // 从而在彩票调度里获得更公平的 CPU 份额。
  if (LAB4_POLICY_ID == 4 && p->tickets < 32)
    p->tickets++;

  // ---- 让出 CPU ----
  // sched() 内部会：检查持有 p->lock、检查 state != RUNNING、
  // 调用 swtch 切回调度器。等本进程被唤醒、再次被调度时，
  // 从 sched() 的下一行继续执行（此时仍持有 p->lock）。
  sched();

  // ---- 被唤醒后从这里继续 ----
  // 清理通道记录：已经不在睡眠了
  p->chan = 0;

  // 释放 p->lock：睡眠期间一直持有它（保护 state/chan），现在可以放了
  release(&p->lock);

  // 重新拿回调用者传进来的 lk，保持"返回时持有 lk"的约定，
  // 让调用者能在锁保护下重新检查睡眠条件（经典 while 循环模式）
  acquire(lk);
}

void
wakeup(void *chan)
{
  struct proc *current = myproc();
  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    if (p == current)
      continue;
    acquire(&p->lock);
    if (p->state == SLEEPING && p->chan == chan) {
      p->state = RUNNABLE;
      if (LAB4_POLICY_ID == 6 || LAB4_POLICY_ID == 7)
        p->pass = minimum_pass_excluding(p);
    }
    release(&p->lock);
  }
}

int
proc_fork(void)
{
  struct proc *p = myproc();
  struct proc *np = allocproc();
  if (!np)
    return -1;
  if (vm_copy(np->pagetable, p->pagetable, p->sz) < 0) {
    np->pagetable = 0;
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;
  np->base = p->base;
  *np->trapframe = *p->trapframe;
  np->trapframe->a0 = 0;
  np->tickets = p->tickets;
  np->tickets_set = p->tickets_set;
  np->priority = p->priority;
  np->mlfq_level = 0;
  np->pass = minimum_pass_excluding(np);
  for (int i = 0; i < (int)sizeof(np->name); i++)
    np->name[i] = p->name[i];

  int pid = np->pid;
  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);
  np->state = RUNNABLE;
  release(&np->lock);
  return pid;
}

int
proc_wait(uint64 status_addr)
{
  struct proc *parent = myproc();
  acquire(&wait_lock);
  for (;;) {
    int have_children = 0;
    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
      if (p->parent != parent)
        continue;
      acquire(&p->lock);
      have_children = 1;
      if (p->state == ZOMBIE) {
        int pid = p->pid;
        int status = p->xstate;
        if (status_addr && copyout(status_addr, &status, sizeof(status)) < 0) {
          release(&p->lock);
          release(&wait_lock);
          return -1;
        }
        freeproc(p);
        release(&p->lock);
        release(&wait_lock);
        return pid;
      }
      release(&p->lock);
    }
    if (!have_children || parent->killed) {
      release(&wait_lock);
      return -1;
    }
    sleep(parent, &wait_lock);
  }
}

void
proc_exit(int status)
{
  struct proc *p = myproc();
  if (p == initproc)
    panic("init exiting");
  acquire(&wait_lock);
  for (struct proc *child = proc; child < &proc[NPROC]; child++) {
    if (child->parent == p) {
      child->parent = initproc;
      wakeup(initproc);
    }
  }
  wakeup(p->parent);
  acquire(&p->lock);
  p->xstate = status;
  p->state = ZOMBIE;
  release(&wait_lock);
  sched();
  panic("zombie exit");
}

int
proc_exec(uint64 name_addr)
{
  char name[64];
  if (copyinstr(name, name_addr, sizeof(name)) < 0)
    return -1;
  struct uprog_entry *e = find_program(name);
  if (!e)
    return -1;

  struct proc *p = myproc();
  pagetable_t newpt = vm_new_user(p->trapframe);
  if (!newpt)
    return -1;
  struct trapframe newtf;
  uint64 newsz = 0;
  if (load_program(newpt, &newtf, e, &newsz) < 0) {
    vm_free_user(newpt, newsz);
    return -1;
  }
  pagetable_t oldpt = p->pagetable;
  uint64 oldsz = p->sz;
  p->pagetable = newpt;
  p->sz = newsz;
  p->base = newsz;
  *p->trapframe = newtf;
  set_name(p, name);
  vm_set_user_pagetable(newpt);
  vm_free_user(oldpt, oldsz);
  return 0;
}

int
proc_sbrk(int n)
{
  struct proc *p = myproc();
  uint64 old = p->sz;
  if (n < 0) {
    uint64 amount = (uint64)(-(long)n);
    if (amount > old || old - amount < PGSIZE)
      return -1;
    uint64 new_size = old - amount;
    vm_unmap(p->pagetable, PGROUNDUP(new_size),
             (PGROUNDUP(old) - PGROUNDUP(new_size)) / PGSIZE, 1);
    p->sz = new_size;
    return (int)old;
  }
  uint64 new_size = old + (uint64)n;
  if (new_size < old || new_size > USER_STACK_PAGE - PGSIZE)
    return -1;
  uint64 va;
  for (va = PGROUNDUP(old); va < PGROUNDUP(new_size); va += PGSIZE) {
    uint64 pa = (uint64)kalloc();
    if (!pa || vm_map(p->pagetable, va, PGSIZE, pa,
                      PTE_U | PTE_R | PTE_W) < 0) {
      if (pa)
        kfree((void *)pa);
      vm_unmap(p->pagetable, PGROUNDUP(old),
               (va - PGROUNDUP(old)) / PGSIZE, 1);
      return -1;
    }
  }
  p->sz = new_size;
  return (int)old;
}

int
proc_kill(int pid)
{
  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->pid == pid && p->state != UNUSED && p->state != ZOMBIE) {
      p->killed = 1;
      if (p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

int
proc_pause(int n)
{
  if (n < 0)
    return -1;
  acquire(&ticks_lock);
  uint start = ticks;
  while (ticks - start < (uint)n) {
    if (myproc()->killed) {
      release(&ticks_lock);
      return -1;
    }
    sleep(&ticks, &ticks_lock);
  }
  release(&ticks_lock);
  return 0;
}

int
proc_schedstat(int pid, int *cpu_ticks, int *last_slice_ticks)
{
  struct proc *self = myproc();
  int wanted = pid == -1 ? self->pid : pid;
  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->pid == wanted && p->state != UNUSED && p->state != ZOMBIE) {
      *cpu_ticks = p->cpu_ticks;
      *last_slice_ticks = p->last_slice_ticks;
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

int
proc_settickets(int tickets_value)
{
  if (tickets_value < 1)
    return -1;
  struct proc *p = myproc();
  acquire(&p->lock);
  p->tickets = tickets_value;
  p->tickets_set = 1;
  p->priority = tickets_value;
  if (LAB4_POLICY_ID == 6 || LAB4_POLICY_ID == 7)
    p->pass = minimum_pass_excluding(p);
  release(&p->lock);
  return 0;
}

// 时钟中断处理函数：每个 TIMER_INTERVAL 触发一次（由 stimecmp 到期引起）。
// 职责：
//   ① re-arm stimecmp：清 STIP、设定下一次闹钟（不做会中断风暴）；
//   ② ticks++ 并 wakeup(&ticks)：推进全局时钟、唤醒所有基于 ticks 睡眠的进程；
//   ③ 策略 9 下统计就绪进程的 wait_ticks：用于老化（aging），避免饥饿。
// 注意：本函数在关中断状态下运行（trap 里进来时中断已被硬件关），
// 并在持有 ticks_lock 期间完成所有操作。
void
clockintr(void)
{
  // ---- ① 重新上发条 ----
  // 把 stimecmp 推到"现在 + 一个间隔"。因为硬件是单次触发（one-shot），
  // 不 re-arm 的话 time >= stimecmp 永远成立，STIP 一直为 1，
  // 一返回用户态就立刻再次进 trap → 中断风暴。
  // 写一个大于当前 time 的值，硬件比较结果变成 time < stimecmp，
  // 从而自动把 sip.STIP 清 0。
  w_stimecmp(r_time() + TIMER_INTERVAL);

  // ---- ② 加锁保护 ticks 和相关操作 ----
  // ticks_lock 保护 ticks 计数器，也保护下面策略 9 对 proc[] 的遍历统计
  // （虽然遍历时对每个 p 还会单独 acquire p->lock，这里主要保护 ticks 本身）。
  acquire(&ticks_lock);

  ticks++;              // 全局时钟 tick 加一（所有进程共享）
  wakeup(&ticks);       // 唤醒所有 chan == &ticks 的睡眠进程
                        // 典型用途：nanosleep/sleep(n) 的实现基于 ticks 变化

  // ---- ③ LAB4 策略 9：统计就绪进程的等待时间（用于老化）----
  // 目的：为长时间处于 RUNNABLE 但一直没被选中的进程累积 wait_ticks，
  // 调度器据此提升其优先级/增加票数，避免饥饿。
  // 只统计其他 RUNNABLE 进程（跳过自己：current 正在跑，没在"等待"）。
  if (LAB4_POLICY_ID == 9) {
    struct proc *current = myproc();
    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
      if (p == current)          // 跳过自己：自己正在 CPU 上跑，不算"等待中"
        continue;
      acquire(&p->lock);         // 逐个进程加锁：state/wait_ticks 是共享可变字段
      if (p->state == RUNNABLE)  // 只统计就绪队列里的进程
        p->wait_ticks++;         // 每 tick 累加一次等待时间
      release(&p->lock);
    }
  }

  release(&ticks_lock);
}

void
timerinit(void)
{
  // 设置"下一次时钟中断触发时刻"：当前时间 + TIMER_INTERVAL。
  // stimecmp 是 Sstc 扩展提供的 supervisor 级定时比较寄存器；
  // 当 time 寄存器的值 >= stimecmp 时，CPU 会触发一次 supervisor 定时器中断（STIP）。
  w_stimecmp(r_time() + TIMER_INTERVAL);

  // 打开 supervisor 定时器中断使能位：
  // r_sie() 读当前 supervisor 中断使能寄存器（sie），
  // SIE_STIE 是其中"supervisor timer interrupt enable"那一位。
  // 只有该位置 1，且 sstatus.SIE（全局中断使能）也为 1 时，
  // 上面设定的定时中断才会真正递送到 S 态（进入 trap / kernelvec）。
  w_sie(r_sie() | SIE_STIE);
}

void
procdump(void)
{
  static const char *states[] = {
    "unused", "used", "sleep", "runble", "run", "zombie"
  };
  printf("\nPID STATE CPU SLICE TICKETS NAME\n");
  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state != UNUSED)
      printf("%d %s %d %d %d %s\n", p->pid, states[p->state],
             p->cpu_ticks, p->last_slice_ticks, p->tickets, p->name);
    release(&p->lock);
  }
}

pagetable_t
stage4_current_pagetable(void)
{
  struct proc *p = myproc();
  return p ? p->pagetable : 0;
}
