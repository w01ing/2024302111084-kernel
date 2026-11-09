#include "kernel/types.h"
#include "user/user.h"

static void run(char marker)
    __attribute__((noreturn, section(".text.helpers")));

int
main(void)
{
  int first = fork();
  if (first == 0)
    run('A');
  int second = fork();
  if (second == 0)
    run('B');
  if (first < 0 || second < 0) {
    printf("FAIL: alt fork\n");
    exit(1);
  }
  wait(0);
  wait(0);
  printf("\nPASS: alt\n");
  exit(0);
}

static void __attribute__((section(".text.helpers")))
run(char marker)
{
  for (int i = 0; i < 24; i++) {
    for (volatile int delay = 0; delay < 3000000; delay++)
      ;
    write(1, &marker, 1);
  }
  exit(0);
}
