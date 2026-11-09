#include "common.h"

#define WARMUP_TICKS 20
#define PHASE_TICKS 180
#define MARGIN_TICKS 40

static void worker(int first_tickets, int second_tickets,
                   unsigned long switch_at, unsigned long stop_at)
    __attribute__((noreturn, section(".text.helpers")));

int
main(void)
{
  unsigned long per_tick = tick_cycles();
  unsigned long start_at = time_rdtime() + WARMUP_TICKS * per_tick;
  unsigned long switch_at = start_at + PHASE_TICKS * per_tick;
  unsigned long sample_end = switch_at + PHASE_TICKS * per_tick;
  unsigned long stop_at = sample_end + MARGIN_TICKS * per_tick;

  setickets(500);
  int first = fork();
  if (first == 0)
    worker(60, 20, switch_at, stop_at);
  int second = fork();
  if (second == 0)
    worker(20, 60, switch_at, stop_at);
  ASSERT(first > 0 && second > 0, "lottery_reticket fork failed");

  int begin_first = cpu_of(first);
  int begin_second = cpu_of(second);
  pause(WARMUP_TICKS + PHASE_TICKS);
  int middle_first = cpu_of(first);
  int middle_second = cpu_of(second);
  pause(PHASE_TICKS);
  int end_first = cpu_of(first);
  int end_second = cpu_of(second);

  kill(first);
  kill(second);
  wait(0);
  wait(0);

  ASSERT(begin_first >= 0 && begin_second >= 0 &&
         middle_first >= begin_first && middle_second >= begin_second &&
         end_first >= middle_first && end_second >= middle_second,
         "lottery_reticket schedstat failed");

  int before_first = middle_first - begin_first;
  int before_second = middle_second - begin_second;
  int after_first = end_first - middle_first;
  int after_second = end_second - middle_second;
  printf("RESULT before_reticket_60_20 = %d:%d\n",
         before_first, before_second);
  printf("RESULT after_reticket_20_60 = %d:%d\n",
         after_first, after_second);

  ASSERT(before_first + before_second > PHASE_TICKS / 2 &&
         after_first + after_second > PHASE_TICKS / 2,
         "lottery_reticket sample too small");
  ASSERT(before_first > before_second,
         "60:20 phase did not favor the first process");
  ASSERT(after_first < after_second,
         "20:60 phase did not favor the second process");

  int before_share = before_first * 100 / (before_first + before_second);
  int after_share = after_first * 100 / (after_first + after_second);
  ASSERT(before_share > 55 && before_share < 95,
         "60:20 phase is outside the lottery tolerance");
  ASSERT(after_share > 5 && after_share < 45,
         "20:60 phase is outside the lottery tolerance");
  ASSERT(before_share - after_share >= 25,
         "setickets did not materially reverse the CPU share");

  PASS("lottery_reticket");
  return 0;
}

static void __attribute__((noreturn, section(".text.helpers")))
worker(int first_tickets, int second_tickets,
       unsigned long switch_at, unsigned long stop_at)
{
  if (setickets(first_tickets) < 0)
    exit(2);
  burn_until(switch_at);
  if (setickets(second_tickets) < 0)
    exit(3);
  burn_until(stop_at);
  exit(0);
}
