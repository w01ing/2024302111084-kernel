#include "common.h"
#include "kernel/course_sid.h"

#define BASE_TICKETS ((int)(7 + COURSE_SID % 13))
#define TICKET_CAP 32
#define WARMUP_TICKS 10
#define WINDOW_TICKS 20
#define BASE_WINDOWS 8
#define BOOST_YIELDS 25
#define BOOST_TICKS 200
#define DYNAMIC_WINDOWS 8
#define INSPECT_TICKS 1000
#define MARGIN_TICKS 30

static void wait_until(unsigned long deadline)
    __attribute__((section(".text.helpers")));
static void boosted_worker(unsigned long baseline_end,
                           unsigned long stop_at)
    __attribute__((noreturn, section(".text.helpers")));
static void baseline_worker(unsigned long stop_at)
    __attribute__((noreturn, section(".text.helpers")));
static int observe_windows(const char *label, int first, int second,
                           int windows, unsigned long start_at,
                           unsigned long per_tick, int *total_first,
                           int *total_second)
    __attribute__((section(".text.helpers")));

int
main(void)
{
  unsigned long per_tick = tick_cycles();

  // tick_cycles() sleeps, so normalize the value inherited by both children.
  ASSERT(setickets(BASE_TICKETS) == 0, "cannot set baseline tickets");

  unsigned long baseline_start =
      time_rdtime() + WARMUP_TICKS * per_tick;
  unsigned long baseline_end =
      baseline_start + BASE_WINDOWS * WINDOW_TICKS * per_tick;
  unsigned long dynamic_start = baseline_end + BOOST_TICKS * per_tick;
  unsigned long dynamic_end =
      dynamic_start + DYNAMIC_WINDOWS * WINDOW_TICKS * per_tick;
  unsigned long stop_at =
      dynamic_end + (INSPECT_TICKS + MARGIN_TICKS) * per_tick;

  int first = fork();
  if (first == 0)
    boosted_worker(baseline_end, stop_at);
  int second = fork();
  if (second == 0)
    baseline_worker(stop_at);
  ASSERT(first > 0 && second > 0, "lottery_dynamic fork failed");

  // Let the observer run promptly whenever a sample boundary wakes it.
  ASSERT(setickets(500) == 0, "cannot prioritize observer");

  int base_first;
  int base_second;
  printf("PHASE baseline tickets=%d:%d expected_share=50\n",
         BASE_TICKETS, BASE_TICKETS);
  wait_until(baseline_start);
  int base_share = observe_windows("baseline", first, second, BASE_WINDOWS,
                                   baseline_start, per_tick,
                                   &base_first, &base_second);

  printf("PHASE boost pid=%d active_yields=%d cap=%d\n",
         first, BOOST_YIELDS, TICKET_CAP);
  printf("INFO press Ctrl-P during the dynamic phase to see tickets %d:%d\n",
         TICKET_CAP, BASE_TICKETS);
  wait_until(dynamic_start);

  int dynamic_first;
  int dynamic_second;
  printf("PHASE dynamic tickets=%d:%d expected_share=71\n",
         TICKET_CAP, BASE_TICKETS);
  int dynamic_share =
      observe_windows("dynamic", first, second, DYNAMIC_WINDOWS,
                      dynamic_start, per_tick,
                      &dynamic_first, &dynamic_second);

  printf("INSPECT press Ctrl-P now; pid %d should have %d tickets, "
         "pid %d should have %d\n",
         first, TICKET_CAP, second, BASE_TICKETS);
  pause(INSPECT_TICKS);

  kill(first);
  kill(second);
  wait(0);
  wait(0);

  printf("RESULT baseline_%d_%d = %d:%d share=%d\n",
         BASE_TICKETS, BASE_TICKETS, base_first, base_second, base_share);
  printf("RESULT dynamic_%d_%d = %d:%d share=%d\n",
         TICKET_CAP, BASE_TICKETS,
         dynamic_first, dynamic_second, dynamic_share);

  ASSERT(base_first + base_second >
             BASE_WINDOWS * WINDOW_TICKS / 2,
         "baseline sample too small");
  ASSERT(dynamic_first + dynamic_second >
             DYNAMIC_WINDOWS * WINDOW_TICKS / 2,
         "dynamic sample too small");
  ASSERT(base_share >= 35 && base_share <= 65,
         "equal-ticket baseline is outside tolerance");
  ASSERT(dynamic_share >= 56 && dynamic_share <= 86,
         "dynamic share did not converge toward 32:13");
  ASSERT(dynamic_share > base_share,
         "active yields did not materially increase CPU share");

  PASS("lottery_dynamic");
  return 0;
}

static void __attribute__((section(".text.helpers")))
wait_until(unsigned long deadline)
{
  while (time_rdtime() < deadline)
    pause(1);
}

static void __attribute__((noreturn, section(".text.helpers")))
boosted_worker(unsigned long baseline_end, unsigned long stop_at)
{
  burn_until(baseline_end);
  for (int i = 0; i < BOOST_YIELDS; i++)
    pause(1);
  burn_until(stop_at);
  exit(0);
}

static void __attribute__((noreturn, section(".text.helpers")))
baseline_worker(unsigned long stop_at)
{
  burn_until(stop_at);
  exit(0);
}

static int __attribute__((section(".text.helpers")))
observe_windows(const char *label, int first, int second, int windows,
                unsigned long start_at, unsigned long per_tick,
                int *total_first, int *total_second)
{
  int begin_first = cpu_of(first);
  int begin_second = cpu_of(second);
  ASSERT(begin_first >= 0 && begin_second >= 0,
         "lottery_dynamic initial schedstat failed");

  int previous_first = begin_first;
  int previous_second = begin_second;
  for (int i = 0; i < windows; i++) {
    unsigned long end =
        start_at + (unsigned long)(i + 1) * WINDOW_TICKS * per_tick;
    wait_until(end);

    int current_first = cpu_of(first);
    int current_second = cpu_of(second);
    ASSERT(current_first >= previous_first &&
           current_second >= previous_second,
           "lottery_dynamic window schedstat failed");

    int window_first = current_first - previous_first;
    int window_second = current_second - previous_second;
    int cumulative_first = current_first - begin_first;
    int cumulative_second = current_second - begin_second;
    int window_total = window_first + window_second;
    int cumulative_total = cumulative_first + cumulative_second;
    int window_share = window_total ? window_first * 100 / window_total : 0;
    int cumulative_share =
        cumulative_total ? cumulative_first * 100 / cumulative_total : 0;

    printf("WINDOW %s_%d cpu=%d:%d share=%d cumulative=%d\n",
           label, i + 1, window_first, window_second,
           window_share, cumulative_share);
    previous_first = current_first;
    previous_second = current_second;
  }

  *total_first = previous_first - begin_first;
  *total_second = previous_second - begin_second;
  int total = *total_first + *total_second;
  return total ? *total_first * 100 / total : 0;
}
