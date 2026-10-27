// LAB3：跨页 copyin 自测。系统调用参数缓冲区不能假设落在单个页面内。
#include "kernel/types.h"
#include "user/user.h"

#define PGSIZE_TEST 4096

int
main(void)
{
  // 申请两个完整页面，确保待传输字符串可以精确跨过页边界。
  char *pages = sbrk(2 * PGSIZE_TEST);
  if (pages == (char *)-1) {
    printf("TEST-COPY FAIL: sbrk\n");
    exit(1);
  }

  // LAB3：让字符串前 8 字节位于第一页末尾，后 8 字节位于下一页开头。
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
