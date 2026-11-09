#include "common.h"

#define OBSERVE_TICKS 80
#define MARGIN_TICKS 40

static void run_case(int t1, int t2)
    __attribute__((section(".text.helpers")));

int
main(void)
{
  setickets(500);
  run_case(60, 20);
  run_case(20, 60);
  PASS("lottery_observe");
  return 0;
}

static void __attribute__((section(".text.helpers")))
run_case(int t1, int t2)
{
  unsigned long deadline = deadline_after(OBSERVE_TICKS + MARGIN_TICKS);
  int p1 = fork();
  if (p1 == 0) {
    setickets(t1);
    burn_until(deadline);
    exit(0);
  }
  int p2 = fork();
  if (p2 == 0) {
    setickets(t2);
    burn_until(deadline);
    exit(0);
  }
  ASSERT(p1 > 0 && p2 > 0, "lottery_observe fork failed");

  int b1 = cpu_of(p1);
  int b2 = cpu_of(p2);
  pause(OBSERVE_TICKS);
  int c1 = cpu_of(p1);
  int c2 = cpu_of(p2);
  kill(p1);
  kill(p2);
  wait(0);
  wait(0);

  ASSERT(b1 >= 0 && b2 >= 0 && c1 >= b1 && c2 >= b2,
         "lottery_observe schedstat failed");
  c1 -= b1;
  c2 -= b2;
  printf("RESULT lottery_case_%d_%d = %d:%d\n", t1, t2, c1, c2);
  ASSERT(c1 + c2 > OBSERVE_TICKS / 2, "lottery_observe sample too small");
  int actual = c1 * 100 / (c1 + c2);
  int expected = t1 * 100 / (t1 + t2);
  ASSERT(actual > expected - 20 && actual < expected + 20,
         "setickets did not change lottery share");
}
