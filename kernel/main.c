/*
 * Lab1 的 S 态入口和确定性自检输出。
 *
 * start() 中执行 mret 后，CPU 会从这里开始执行；
 * 本函数负责输出个性化 Banner、printf 边界测试，并启动 Lab2。
 */

/* 基础类型定义。 */
#include "types.h"
#include "memlayout.h"

/* LAB2-STAGE3：读写 sie，并打开 S 态全局外部中断。 */
#include "riscv.h"

/* 学号和本实验的个性化协议参数。 */
#include "course_sid.h"

/* LAB2-STAGE1：声明 trap_init() 和 usertrapret()。 */
#include "trap.h"
#include "exec.h"      /* LAB2-STAGE4：装载并启动内嵌 Shell。 */

/* LAB2-STAGE1/STAGE2：自测会直接填写 trapframe 中的 ABI 寄存器字段。 */
#include "syscall.h"

/* LAB2-STAGE3：启动 UART 接收中断和 PLIC 外部中断通路。 */
#include "console.h"
#include "vm.h"

/* 本项目自己实现的最小 printf。 */
extern void printf(const char *fmt, ...);

/* 协议二需要输出收尾信息；协议 0/1 下该函数为空操作。 */
extern void console_protocol_finish(void);
extern char kernel_end[];

#ifdef LAB3_ALLOC_SELFTEST
/* Temporary phase-one validation; disabled in the normal build. */
static void
allocator_selftest_main(void)
{
  void *held = 0;
  uint64 count = 0;
  uint64 expected = 0;
  uint64 lo = PGROUNDUP((uint64)kernel_end);
  uint64 hi = PHYSTOP - (uint64)LAB3_MEMCAP_DELTA * PGSIZE;

  if (hi > lo)
    expected = (hi - lo) / PGSIZE;

  for (;;) {
    uint8 *page = kalloc();
    if (!page)
      break;
    if (((uint64)page & (PGSIZE - 1)) != 0)
      for (;;) asm volatile("wfi");
    for (uint64 i = 0; i < PGSIZE; i++)
      if (page[i] != 0)
        for (;;) asm volatile("wfi");
    *(uint64 *)page = (uint64)held;
    held = page;
    count++;
  }

  if (count != expected)
    for (;;) asm volatile("wfi");

  while (held) {
    void *next = (void *)*(uint64 *)held;
    kfree(held);
    held = next;
  }
  printf("lab3 allocator selftest: pass pages=%ld\n", (long)count);
}
#endif

#ifdef LAB3_STAGE2_SELFTEST
static void
stage2_selftest_fail(void)
{
  for (;;) asm volatile("wfi");
}

static void
pagetable_selftest_main(void)
{
  pagetable_t pt = kalloc();
  uint64 pages[4];
  uint64 vas[3] = {0x0000, 0x200000, 0x400000};
  uint64 perms[3] = {PTE_R | PTE_X | PTE_U,
                     PTE_R | PTE_W | PTE_U,
                     PTE_R | PTE_U};

  if (!pt) stage2_selftest_fail();
  for (int i = 0; i < 4; i++) {
    pages[i] = (uint64)kalloc();
    if (!pages[i] || (pages[i] & (PGSIZE - 1))) stage2_selftest_fail();
  }

  for (int i = 0; i < 3; i++)
    if (mappages(pt, vas[i], PGSIZE, pages[i], perms[i]) < 0)
      stage2_selftest_fail();

  for (int i = 0; i < 3; i++) {
    pte_t *p = walk(pt, vas[i], 0);
    if (!p || !(*p & PTE_V) || PTE2PA(*p) != pages[i] ||
        (PTE_FLAGS(*p) & (PTE_R | PTE_W | PTE_X | PTE_U)) != perms[i])
      stage2_selftest_fail();
  }

  if (mappages(pt, vas[0], PGSIZE, pages[3], PTE_R | PTE_U) == 0 ||
      PTE2PA(*walk(pt, vas[0], 0)) != pages[0])
    stage2_selftest_fail();

  for (int i = 0; i < 3; i++) vm_unmap(pt, vas[i], 1, 1);
  kfree((void *)pages[3]);
  kfree(pt);

  /* Consume real free pages, leaving only four for intermediate tables. */
  uint64 baseline = vm_free_count();
  pagetable_t rollback_pt = kalloc();
  uint64 rollback_pa = (uint64)kalloc();
  void *held = 0;
  if (!rollback_pt || !rollback_pa) stage2_selftest_fail();
  while (vm_free_count() > 4) {
    void *page = kalloc();
    if (!page) stage2_selftest_fail();
    *(uint64 *)page = (uint64)held;
    held = page;
  }
  uint64 before_map = vm_free_count();
  if (before_map != 4 ||
      mappages(rollback_pt, 0, 64 * 1024 * 1024UL, rollback_pa,
               PTE_R | PTE_U) == 0 ||
      (rollback_pt[PX(2, 0)] & PTE_V) ||
      vm_free_count() != before_map)
    stage2_selftest_fail();
  while (held) {
    void *next = (void *)*(uint64 *)held;
    kfree(held);
    held = next;
  }
  kfree((void *)rollback_pa);
  kfree(rollback_pt);
  uint64 restored = vm_free_count();
  if (restored != baseline)
    stage2_selftest_fail();
  printf("lab3 pagetable selftest: pass free_before=%ld free_after=%ld restored=%ld\n",
         (long)before_map, (long)before_map, (long)restored);
}
#endif

#ifdef LAB3_STAGE3_SELFTEST
static void
stage3_selftest_fail(void)
{
  for (;;) asm volatile("wfi");
}

static void
kernel_pagetable_selftest_main(void)
{
  pagetable_t pt = kernel_pagetable();
  pte_t *p;

  if (r_satp() != MAKE_SATP(pt)) stage3_selftest_fail();

  /* The RAM direct map uses 2MiB level-1 leaf entries. */
  for (uint64 pa = KERNBASE; pa < PHYSTOP; pa += (1UL << 21)) {
    pte_t root = pt[PX(2, pa)];
    if (!(root & PTE_V) || (PTE_FLAGS(root) & (PTE_R | PTE_W | PTE_X)))
      stage3_selftest_fail();
    pagetable_t l1 = (pagetable_t)PTE2PA(root);
    pte_t leaf = l1[PX(1, pa)];
    if (!(leaf & PTE_V) || PTE2PA(leaf) != pa ||
        (PTE_FLAGS(leaf) & (PTE_R | PTE_W | PTE_X)) !=
          (PTE_R | PTE_W | PTE_X) || (leaf & PTE_U))
      stage3_selftest_fail();
  }

  p = walk(pt, UART0, 0);
  if (!p || PTE2PA(*p) != UART0 ||
      (PTE_FLAGS(*p) & (PTE_R | PTE_W | PTE_U)) != (PTE_R | PTE_W))
    stage3_selftest_fail();
  p = walk(pt, PLIC, 0);
  if (!p || PTE2PA(*p) != PLIC ||
      (PTE_FLAGS(*p) & (PTE_R | PTE_W | PTE_U)) != (PTE_R | PTE_W))
    stage3_selftest_fail();
  p = walk(pt, TRAMPOLINE, 0);
  if (!p || !(PTE_FLAGS(*p) & PTE_X) || (PTE_FLAGS(*p) & PTE_U))
    stage3_selftest_fail();
  p = walk(pt, TRAPFRAME, 0);
  if (!p || !(PTE_FLAGS(*p) & (PTE_R | PTE_W)) || (PTE_FLAGS(*p) & PTE_U))
    stage3_selftest_fail();

  printf("lab3 kernel pagetable selftest: pass satp=%lx\n", r_satp());
}
#endif

#ifdef LAB3_STAGE4_SELFTEST
static void
stage4_selftest_fail(void)
{
  for (;;) asm volatile("wfi");
}

static void
user_pagetable_selftest_main(void)
{
  pagetable_t pt = user_pagetable();
  pte_t *code = walk(pt, 0, 0);
  pte_t *rodata = walk(pt, PGSIZE, 0);
  pte_t *data = walk(pt, 2 * PGSIZE, 0);
  pte_t *stack = walk(pt, 0x40000UL, 0);
  char src[] = "copy-roundtrip";
  char dst[sizeof(src)];

  pte_t *guard = walk(pt, 3 * PGSIZE, 0);
  if (!code || !rodata || !data || !stack || !guard ||
      !(*code & PTE_V) || !(*rodata & PTE_V) || !(*data & PTE_V) ||
      !(*stack & PTE_V) || (*guard & PTE_V) ||
      !(PTE_FLAGS(*code) & PTE_X) || (PTE_FLAGS(*code) & PTE_W) ||
      !(PTE_FLAGS(*rodata) & PTE_R) || (PTE_FLAGS(*rodata) & (PTE_W|PTE_X)) ||
      !(PTE_FLAGS(*data) & (PTE_R|PTE_W)) || (PTE_FLAGS(*data) & PTE_X) ||
      !(PTE_FLAGS(*stack) & (PTE_R|PTE_W|PTE_U)) ||
      copyout(2 * PGSIZE, src, sizeof(src)) < 0 ||
      copyin(dst, 2 * PGSIZE, sizeof(dst)) < 0)
    stage4_selftest_fail();
  for (uint64 i = 0; i < sizeof(src); i++)
    if (dst[i] != src[i]) stage4_selftest_fail();
  if (copyout(PGSIZE, src, sizeof(src)) == 0 ||
      walkaddr(pt, 0, 1) != 0)
    stage4_selftest_fail();
  printf("lab3 user pagetable selftest: pass copyin/copyout perms/guard\n");
}
#endif

/* 输出实验要求的 printf 边界测试数据。 */
static void
printf_boundary_tests(void)
{
  /*
   * 连续长字符串，用于验证 printf 不会在较长字符串输出时崩溃。
   * 三段字符串会在编译期自动拼接成一个完整字符串。
   */
  static const char long_text[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

  /* 依次测试 0、负数、最大 32 位整数、空字符串和长字符串。 */
  printf("printf:%d,%d,%d,%s,%s\n",
         0, -1, 2147483647, "", long_text);
}

/* 告诉编译器 main() 不会返回。 */
__attribute__((noreturn)) void
main(void)
{
  /*
   * 输出个性化 Banner。
   * 学号超过 32 位，因此使用 %ld 和 long，避免被截断。
   * 2024302111084 % 97 = 68 = 0x44。
   */
  printf("OSLAB1 sid=%ld mod97=0x%x\n",
         (long)COURSE_SID, (unsigned int)(COURSE_SID % 97));

#if LAB1_BANNER_PROTOCOL != 2
  /* 协议 0/1 继续输出 printf 边界测试；协议 2 需要保留末段校验和。 */
  printf_boundary_tests();
#endif

  /* 协议 2 在这里输出校验和，协议 0/1 下不会产生额外字符。 */
  console_protocol_finish();

  /*
   * LAB2-STAGE1 / LAB2-STAGE3：
   * start.c 执行 mret 后，main() 仍然运行在 S 态。
   * trap_init() 建立最小的用户/内核地址空间，并填写 trapframe 的
   * 内核侧字段；随后 main() 填写用户侧初始字段，再由 usertrapret()
   * 进入 trampoline/userret。
   */
  printf("lab2 stage4: loading embedded user programs\n");
  /*
   * LAB2-STAGE3：当前仍处于 satp=0 的启动直映阶段，先完成一次
   * UART/PLIC 硬件初始化；随后 trap_init() 切换到带 MMIO 映射的
   * 内核页表，运行期中断处理再通过该页表访问同一组寄存器。
   */
  uart_init();
  plic_init();
#ifdef LAB3_ALLOC_SELFTEST
  /* kinit is repeated by trap_init, restoring the configured free order. */
  kinit();
  allocator_selftest_main();
#endif
#ifdef LAB3_STAGE2_SELFTEST
  kinit();
  pagetable_selftest_main();
#endif
  trap_init();
#ifdef LAB3_STAGE3_SELFTEST
  kernel_pagetable_selftest_main();
#endif

  /*
   * LAB2-STAGE3：
   * satp 已切到内核页表，UART 和 PLIC 的 MMIO 映射现在可用；
   * 打开 S 态外部中断后，第一次 sret 就能进入用户 read。
   */
  w_sie(r_sie() | SIE_SEIE);
  intr_on();

  /*
   * LAB2-STAGE4：
   * 自动化注入脚本仍使用 "$ " 作为启动探针，因此保留一个原始
   * 提示作为“内核已经完成初始化”的标记。真正进入用户态后，
   * gifts/user/sh.c 会输出它自己的 "sh> " 提示。
   */
  console_puts_raw("$ ");
  stage4_bootstrap(&stage1_tf);
#ifdef LAB3_STAGE4_SELFTEST
  user_pagetable_selftest_main();
#endif

  /* stage4_bootstrap() 已填好 epc/sp；usertrapret() 完成第一次 sret。 */
  usertrapret();

  for (;;)
    asm volatile("wfi");
}
