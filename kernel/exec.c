/*
 * LAB2-STAGE4：平铺用户程序装载器和最小 Shell 运行时。
 *
 * Makefile.upgrade 会把 sh、hi、spin 编译成“链接基址为 0”的平铺
 * 二进制，并由 kernel/userimg.S 通过 .incbin 嵌入内核镜像。这里不
 * 访问磁盘，也不伪造文件系统，而是直接遍历那张内嵌程序表：
 *
 *   start, end, "name", padding, binary bytes
 *
 * 由于当前实验还没有真正的进程抽象，本文件采用阶段四允许的最小
 * 单进程方案：
 *
 *   1. 内核启动时把 sh 装入当前用户地址空间；
 *   2. sh 调用 fork 时保存 Shell 的代码/数据、用户栈和 trapframe；
 *   3. 当前现场作为“子进程”继续执行，exec 覆盖成 hi 或 spin；
 *   4. hi 调用 exit 时恢复之前保存的 Shell 现场，Shell 接着执行 wait；
 *   5. spin 是有意设计的常驻程序，会一直输出计数，符合测试目的。
 *
 * 这不是后续实验的通用进程实现，但每一步都对应阶段四控制流图上的
 * 一个箭头，并且足以让 sh -> fork -> exec -> exit -> wait 跑通。
 */

#include "types.h"
#include "riscv.h"
#include "trap.h"
#include "console.h"
#include "exec.h"

/* kernel/userimg.S 导出的程序表首地址。 */
extern char _uprog_table[];

/* 阶段四给用户程序准备的固定镜像容量。 */
#define STAGE4_USER_IMAGE_MAX (128 * 1024UL)

/* 用户栈放在程序低地址空间的高处，避免覆盖程序镜像。 */
#define STAGE4_USER_STACK_PAGE 0x40000UL
#define STAGE4_USER_STACK_TOP  (STAGE4_USER_STACK_PAGE + PGSIZE)

/* 每个程序表项的前两个字段是 start/end，name 紧跟在 offset 16 处。 */
struct uprog_entry {
  uint64 start;
  uint64 end;
  char name[1];
};

/*
 * 当前用户程序和被 fork 暂存的 Shell 各有一份物理后备存储。
 * __attribute__((aligned(PGSIZE))) 保证它们可以直接作为页表叶子页
 * 的物理地址使用。
 */
__attribute__((aligned(PGSIZE)))
static uint8 stage4_user_image[STAGE4_USER_IMAGE_MAX];
__attribute__((aligned(PGSIZE)))
static uint8 stage4_parent_image[STAGE4_USER_IMAGE_MAX];
__attribute__((aligned(PGSIZE)))
static uint8 stage4_user_stack[PGSIZE];
__attribute__((aligned(PGSIZE)))
static uint8 stage4_parent_stack[PGSIZE];

/* 当前镜像和父镜像的有效字节数。 */
static uint64 stage4_user_image_size;
static uint64 stage4_parent_image_size;

/*
 * 阶段四的简化进程状态：
 *   1 = Shell 父现场；
 *   2 = fork 后正在运行的子现场。
 */
static int stage4_current_pid = 1;
static int stage4_child_active;
static int stage4_child_done;
static struct trapframe stage4_parent_tf;

/* 内核自己的最小字节复制函数，避免依赖宿主 libc。 */
static void
stage4_memmove(void *dst, const void *src, uint64 n)
{
  uint8 *d = (uint8 *)dst;
  const uint8 *s = (const uint8 *)src;
  uint64 i;

  if (d == s)
    return;

  if (d < s) {
    for (i = 0; i < n; i++)
      d[i] = s[i];
  } else {
    for (i = n; i != 0; i--)
      d[i - 1] = s[i - 1];
  }
}

/* 清空一段存储，确保新程序不会看到上一个程序留下的 .bss 内容。 */
static void
stage4_memzero(void *dst, uint64 n)
{
  uint8 *d = (uint8 *)dst;
  uint64 i;

  for (i = 0; i < n; i++)
    d[i] = 0;
}

/*
 * 比较用户传入的程序名和表项名字。
 *
 * gifts/user/ulib.c 的 gets() 会把回车保存到字符串中，因此这里把
 * '\n' 和 '\r' 视作命令名结束符；否则 Shell 输入 "hi<Enter>" 会
 * 被错误地查成 "hi\n"。
 */
static int
stage4_name_equal(const char *requested, const char *entry_name)
{
  int i;

  if (requested == 0 || entry_name == 0)
    return 0;

  for (i = 0; i < 64; i++) {
    char a = requested[i];
    char b = entry_name[i];

    if (a == '\0' || a == '\n' || a == '\r')
      return b == '\0';
    if (a != b)
      return 0;
  }

  /* 没有在限制长度内遇到字符串结尾，拒绝异常长名字。 */
  return 0;
}

/*
 * 在内嵌程序表中查找名字。
 *
 * 表项后面紧接着是该程序的二进制，因此下一个表项不是
 * sizeof(struct uprog_entry)；必须从 end 向上对齐到 8 字节后继续。
 */
static struct uprog_entry *
stage4_find_program(const char *name)
{
  struct uprog_entry *entry;

  entry = (struct uprog_entry *)_uprog_table;
  while (entry->start != 0 || entry->end != 0) {
    if (stage4_name_equal(name, entry->name))
      return entry;

    entry = (struct uprog_entry *)((entry->end + 7) & ~7UL);
  }

  return 0;
}

/*
 * 把一个已经准备好的镜像绑定到低地址用户页表，并设置用户入口。
 *
 * trap_map_user_image() 会清空旧的低地址叶子映射，然后建立：
 *   VA 0 .. image_size       -> stage4_user_image
 *   VA 0x40000 .. 0x41000   -> stage4_user_stack
 *
 * 用户程序由 user.ld 从地址 0 链接，所以入口 main 的偏移就是 0。
 */
static int
stage4_install_image(const void *source, uint64 size, struct trapframe *tf)
{
  if (source == 0 || size == 0 || size > STAGE4_USER_IMAGE_MAX)
    return -1;

  stage4_memzero(stage4_user_image, STAGE4_USER_IMAGE_MAX);
  stage4_memmove(stage4_user_image, source, size);
  stage4_memzero(stage4_user_stack, PGSIZE);
  stage4_user_image_size = size;

  if (trap_map_user_image((uint64)stage4_user_image,
                          stage4_user_image_size,
                          (uint64)stage4_user_stack,
                          STAGE4_USER_STACK_PAGE) < 0)
    return -1;

  /*
   * exec 覆盖地址空间后，下一条要执行的是新镜像的入口 main。
   * sp 指向栈顶且保持 16 字节对齐，满足 RISC-V ABI。
   */
  tf->epc = 0;
  tf->sp = STAGE4_USER_STACK_TOP;
  tf->ra = 0;
  tf->gp = 0;
  tf->tp = 0;
  return 0;
}

/* 根据程序表项装载一个程序。 */
static int
stage4_install_entry(struct uprog_entry *entry, struct trapframe *tf)
{
  uint64 size;

  if (entry == 0 || entry->end < entry->start)
    return -1;

  size = entry->end - entry->start;
  return stage4_install_image((const void *)entry->start, size, tf);
}

/* 启动时由内核主流程调用：把第一个用户程序固定选择为 sh。 */
void
stage4_bootstrap(struct trapframe *tf)
{
  struct uprog_entry *shell;

  shell = stage4_find_program("sh");
  if (stage4_install_entry(shell, tf) < 0) {
    console_puts_raw("stage4: cannot load sh\n");
    for (;;)
      asm volatile("wfi");
  }
}

/* getpid() 返回当前简化运行时中的父/子编号。 */
int
stage4_getpid(void)
{
  return stage4_current_pid;
}

/*
 * fork()：保存父 Shell 的完整用户现场，然后让当前现场充当子进程。
 *
 * usertrap() 在进入分发器前已经把 epc 从 ecall 指令推进到了下一条
 * 指令，因此保存的父 trapframe 正好会从 fork() 返回后的 Shell 代码
 * 继续执行；a0 改成子 pid，wait() 就能看到一个正常的父返回值。
 */
long
stage4_fork(struct trapframe *tf)
{
  if (stage4_child_active || stage4_current_pid != 1)
    return -1;

  stage4_memmove(stage4_parent_image,
                 stage4_user_image,
                 stage4_user_image_size);
  stage4_parent_image_size = stage4_user_image_size;
  stage4_memmove(stage4_parent_stack, stage4_user_stack, PGSIZE);
  stage4_parent_tf = *tf;
  stage4_parent_tf.a0 = 2;

  stage4_child_active = 1;
  stage4_child_done = 0;
  stage4_current_pid = 2;

  /* 当前 trapframe 将由 syscall_dispatch() 写成 a0=0，进入子分支。 */
  return 0;
}

/*
 * wait()：本阶段没有阻塞式调度器。
 *
 * 子程序 exit 会直接恢复父 trapframe，因此父 Shell 执行到 wait 时，
 * 子程序一定已经结束；返回保存的子 pid 即可让 gifts/user/sh.c 继续
 * 下一轮提示符。
 */
long
stage4_wait(struct trapframe *tf)
{
  (void)tf;

  if (stage4_current_pid != 1)
    return -1;
  if (!stage4_child_active || !stage4_child_done)
    return -1;

  stage4_child_active = 0;
  return 2;
}

/*
 * exec()：从用户 a0 读取程序名，成功后覆盖当前地址空间。
 *
 * argv 在阶段四的 sh 中不会被内核使用，因此只按接口接收而不解析。
 * 必须先完成名字查找，再覆盖 stage4_user_image，否则用户传入的
 * name 指针会随着旧镜像被覆盖而失效。
 */
long
stage4_exec(struct trapframe *tf)
{
  struct uprog_entry *entry;

  entry = stage4_find_program((const char *)tf->a0);
  if (entry == 0)
    return -1;

  if (stage4_install_entry(entry, tf) < 0)
    return -1;
  return 0;
}

/*
 * 子程序退出：恢复 fork 时保存的 Shell。
 *
 * 这是阶段四“伪调度”的关键跳转。恢复后直接调用 usertrapret，
 * 不再返回到已退出子程序的 syscall_dispatch 调用链。
 */
void
stage4_exit(struct trapframe *tf, int status)
{
  (void)tf;
  (void)status;

  if (stage4_current_pid == 2 && stage4_child_active) {
    stage4_memzero(stage4_user_image, STAGE4_USER_IMAGE_MAX);
    stage4_memmove(stage4_user_image,
                   stage4_parent_image,
                   stage4_parent_image_size);
    stage4_user_image_size = stage4_parent_image_size;
    stage4_memmove(stage4_user_stack, stage4_parent_stack, PGSIZE);

    if (trap_map_user_image((uint64)stage4_user_image,
                            stage4_user_image_size,
                            (uint64)stage4_user_stack,
                            STAGE4_USER_STACK_PAGE) < 0) {
      console_puts_raw("stage4: cannot restore sh\n");
      for (;;)
        asm volatile("wfi");
    }

    stage1_tf = stage4_parent_tf;
    stage4_current_pid = 1;
    stage4_child_done = 1;

    /*
     * 子进程不会再返回到自己的用户态；此处恢复父 Shell 的现场，
     * 从 fork() 返回后的下一条指令继续运行。
     */
    usertrapret();
  }

  /* Shell 自己退出时，本阶段没有调度器可切换，安全停机即可。 */
  for (;;)
    asm volatile("wfi");
}
