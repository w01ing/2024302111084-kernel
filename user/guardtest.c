// Lab3 stage4 self-test: access the user stack guard page from U mode.
#include "kernel/types.h"
#include "user/user.h"

#define USER_STACK_GUARD 0x3f000UL

static int
run_guard_child(int write_access)
{
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
