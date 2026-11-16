/* Lab5 综合自测：COW 引用计数、并发回收、写保护、配额和 exec argv。 */
#include "kernel/types.h"
#include "kernel/course_sid.h"
#include "user/user.h"

#define PGSIZE 4096
#define SINGLE_ROUNDS 4
#define CONCURRENT_CHILDREN 6
#define BUDGET_BATCH_PAGES 32
#define BAD_USER_ADDR ((char *)0x50000UL)

static int
test_budget(void)
{
  int rounds = (LAB5_COW_BUDGET + BUDGET_BATCH_PAGES - 1) /
               BUDGET_BATCH_PAGES;
  int bytes = BUDGET_BATCH_PAGES * PGSIZE;
  char *area = sbrk(bytes);
  if (area == SBRK_ERROR) {
    printf("COWTEST-4 FAIL: sbrk budget pages\n");
    return -1;
  }
  for (int i = 0; i < BUDGET_BATCH_PAGES; i++)
    area[i * PGSIZE] = 0x31;

  unsigned long long copies0 = pmc(0);
  int failed = 0;
  for (int round = 0; round < rounds; round++) {
    int pid = fork();
    if (pid < 0) {
      failed = 1;
      break;
    }
    if (pid == 0) {
      /* 保持所有页面被共享，直到父进程完成这一轮逐页 COW 写。 */
      for (;;)
        ;
    }

    for (int i = 0; i < BUDGET_BATCH_PAGES; i++)
      area[i * PGSIZE] = (char)(round + i + 1);
    kill(pid);
    int status = 0;
    if (wait(&status) < 0 || status == 0)
      failed = 1;
  }

  unsigned long long copies1 = pmc(0);
  unsigned long long delta = copies1 - copies0;
  int shared = (int)pmc(1);
  sbrk(-bytes);
  if (!failed && delta >= LAB5_COW_BUDGET && shared == 0) {
    printf("COWTEST-4 PASS: budget=%d copies=%llu (%d pages x %d rounds)\n",
           LAB5_COW_BUDGET, delta, BUDGET_BATCH_PAGES, rounds);
    return 0;
  }
  printf("COWTEST-4 FAIL: failed=%d copies=%llu shared=%d\n",
         failed, delta, shared);
  return -1;
}

static int
test_single_page_rounds(void)
{
  char *page = sbrk(PGSIZE);
  if (page == SBRK_ERROR) {
    printf("COWTEST-1 FAIL: sbrk\n");
    return -1;
  }
  page[0] = 0x11;

  for (int round = 0; round < SINGLE_ROUNDS; round++) {
    unsigned long long shared0 = pmc(1);
    int pid = fork();
    if (pid < 0) {
      printf("COWTEST-1 FAIL: fork round=%d\n", round);
      sbrk(-PGSIZE);
      return -1;
    }
    if (pid == 0) {
      char value = (char)(0x40 + round);
      page[0] = value;
      exit(page[0] == value ? 0 : 1);
    }

    int status = -1;
    wait(&status);
    char parent_value = (char)(0x20 + round);
    page[0] = parent_value;
    if (status != 0 || page[0] != parent_value || pmc(1) != shared0) {
      printf("COWTEST-1 FAIL: round=%d status=%d shared=%llu:%llu\n",
             round, status, shared0, pmc(1));
      sbrk(-PGSIZE);
      return -1;
    }
  }

  sbrk(-PGSIZE);
  printf("COWTEST-1 PASS: single page %d COW rounds, refs conserved\n",
         SINGLE_ROUNDS);
  return 0;
}

static int
test_concurrent_children(void)
{
  char *page = sbrk(PGSIZE);
  if (page == SBRK_ERROR) {
    printf("COWTEST-2 FAIL: sbrk\n");
    return -1;
  }
  page[0] = 0x55;

  int made = 0;
  for (int i = 0; i < CONCURRENT_CHILDREN; i++) {
    int pid = fork();
    if (pid < 0)
      break;
    if (pid == 0) {
      char value = (char)(0x60 + i);
      page[0] = value;
      if (page[0] != value)
        exit(1);
      pause(20);
      exit(0);
    }
    made++;
  }

  unsigned long long peak_shared = pmc(1);
  int failed = made != CONCURRENT_CHILDREN;
  for (int i = 0; i < made; i++) {
    int status = -1;
    if (wait(&status) < 0 || status != 0)
      failed = 1;
  }
  unsigned long long final_shared = pmc(1);
  sbrk(-PGSIZE);

  if (!failed && peak_shared > 0 && final_shared == 0) {
    printf("COWTEST-2 PASS: %d concurrent children, pmc(1)=%llu->0\n",
           made, peak_shared);
    return 0;
  }
  printf("COWTEST-2 FAIL: made=%d peak=%llu final=%llu\n",
         made, peak_shared, final_shared);
  return -1;
}

static int
test_readonly_code(void)
{
  int pid = fork();
  if (pid < 0) {
    printf("COWTEST-3 FAIL: fork\n");
    return -1;
  }
  if (pid == 0) {
    *(volatile char *)0x0 = 0x7f;
    exit(0);
  }

  int status = 0;
  wait(&status);
  if (status != 0) {
    printf("COWTEST-3 PASS: code write killed, kernel continued\n");
    return 0;
  }
  printf("COWTEST-3 FAIL: write to code address 0 survived\n");
  return -1;
}

static int
test_exec(void)
{
  char *normal_argv[] = { "hi", 0 };
  char *bad_string_argv[] = { "hi", BAD_USER_ADDR, 0 };
  if (exec(BAD_USER_ADDR, normal_argv) != -1 ||
      exec("hi", (char **)BAD_USER_ADDR) != -1 ||
      exec("hi", bad_string_argv) != -1) {
    printf("COWTEST-5 FAIL: invalid exec pointer accepted\n");
    return -1;
  }

  int pid = fork();
  if (pid < 0) {
    printf("COWTEST-5 FAIL: fork\n");
    return -1;
  }
  if (pid == 0) {
    char *argv[] = { "echo", "cow", "argv", "ok", 0 };
    exec("echo", argv);
    exit(1);
  }

  int status = -1;
  wait(&status);
  if (status == 0) {
    printf("COWTEST-5 PASS: exec argv and invalid pointers\n");
    return 0;
  }
  printf("COWTEST-5 FAIL: valid exec status=%d\n", status);
  return -1;
}

int
main(void)
{
  int passed = 0;

  if (test_budget() == 0)
    passed++;
  if (test_single_page_rounds() == 0)
    passed++;
  if (test_concurrent_children() == 0)
    passed++;
  if (test_readonly_code() == 0)
    passed++;
  if (test_exec() == 0)
    passed++;

  if (passed == 5) {
    printf("COWTEST PASS (5/5)\n");
    exit(0);
  }
  printf("COWTEST FAIL (%d/5)\n", passed);
  exit(1);
}
