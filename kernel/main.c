/*
 * Lab 1 supervisor-mode entry and deterministic self-check output.
 */

#include "types.h"
#include "course_sid.h"

extern void printf(const char *fmt, ...);
extern void console_protocol_finish(void);

static void
printf_boundary_tests(void)
{
  static const char long_text[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

  printf("printf:%d,%d,%d,%s,%s\n",
         0, -1, 2147483647, "", long_text);
}

__attribute__((noreturn)) void
main(void)
{
  printf("OSLAB1 sid=%d mod97=0x%x\n",
         (int)COURSE_SID, (unsigned int)(COURSE_SID % 97));

#if LAB1_BANNER_PROTOCOL != 2
  printf_boundary_tests();
#endif

  console_protocol_finish();
  for (;;)
    asm volatile("wfi");
}
