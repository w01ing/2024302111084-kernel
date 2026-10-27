// LAB3：用户栈 guard page 自测。未映射保护页的读写都必须杀死当前进程。
#include "kernel/types.h"
#include "user/user.h"

#define USER_STACK_GUARD 0x3f000UL

static int
run_guard_child(int write_access)
{
  // 通过参数分别覆盖 load page fault 和 store page fault 两条路径。
  // LAB3：在子进程中故意触发 fault，父进程继续运行并检查退出状态。
  int pid = fork();
  if (pid == 0) {
    volatile char *guard = (volatile char *)USER_STACK_GUARD;
    if (write_access)
      *guard = 'x';
    else
      (void)*guard;
    printf("TEST-GUARD FAIL: access survived (%s)\n",
           write_access ? "write" : "read");
    exit(0);
  }
  return pid;
}

int
main(void)
{
  // 父进程只检查退出状态，正常返回则说明 guard page 错误未被拦截。
  int status;
  int pid = run_guard_child(0);
  if (pid < 0) {
    printf("TEST-GUARD FAIL: fork read\n");
    exit(1);
  }
  wait(&status);
  if (status == 0) {
    printf("TEST-GUARD FAIL: read was not killed\n");
    exit(1);
  }

  pid = run_guard_child(1);
  if (pid < 0) {
    printf("TEST-GUARD FAIL: fork write\n");
    exit(1);
  }
  wait(&status);
  if (status == 0) {
    printf("TEST-GUARD FAIL: write was not killed\n");
    exit(1);
  }

  printf("TEST-GUARD PASS: read/write below user stack killed\n");
  exit(0);
}
