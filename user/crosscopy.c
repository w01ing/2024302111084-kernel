// Lab3 stage4 self-test: copyin must handle a user buffer across two pages.
#include "kernel/types.h"
#include "user/user.h"

#define PGSIZE_TEST 4096

int
main(void)
{
  char *pages = sbrk(2 * PGSIZE_TEST);
  if (pages == (char *)-1) {
    printf("TEST-COPY FAIL: sbrk\n");
    exit(1);
  }

  char *cross = pages + PGSIZE_TEST - 8;
  memcpy(cross, "EDGE1234", 8);
  memcpy(cross + 8, "NEXT5678", 8);

  printf("TEST-COPY BEGIN\n");
  int n = write(1, cross, 16);
  printf("\n");
  if (n != 16) {
    printf("TEST-COPY FAIL: write returned %d, expected 16\n", n);
    exit(1);
  }
  printf("TEST-COPY PASS: cross-page bytes=16\n");
  exit(0);
}
