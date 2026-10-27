// LAB3：非法用户地址访问测试，验证用户页表和 page fault 处理没有破坏内核。
// 官方测试 · lab3-test1:非法访问被杀,内核完好
// 断言(与 sid 无关):
//   1. 写地址 0 / 写 MAXVA 附近 → 进程被杀(sbrk 分配后越界一页同样);
//   2. 父进程 wait 正常回收,内核存活(sh 仍在)。
#include "kernel/types.h"
#include "user/user.h"

static volatile int sink;

int
main(void)
{
    // 每个非法访问都放在子进程中，避免测试本身终止整个 Shell。
    int pid = fork();
    if(pid == 0){
        // LAB3：地址 0 没有用户可写映射，U 态写入必须触发 store page fault。
        *(int *)(0) = 1;
        printf("TEST-3a FAIL: write to 0 survived\n");
        exit(0);                     // 不应走到这里
    }
    int st;
    wait(&st);
    if(st != 0)
        printf("TEST-3a PASS: wild write killed (status=%d)\n", st);
    else
        printf("TEST-3a FAIL: child exited normally after wild write\n");

    // 第二组访问验证“已申请范围之外但仍处于用户低地址区”的页错误。
    pid = fork();
    if(pid == 0){
        // LAB3：sbrk 只建立已分配范围的 PTE，越过范围读取必须触发 fault。
        char *p = sbrk(4096);
        sink = p[8192];              // 越界一页读
        printf("TEST-3b FAIL: read beyond sbrk survived\n");
        exit(0);
    }
    wait(&st);
    if(st != 0)
        printf("TEST-3b PASS: beyond-sbrk access killed (status=%d)\n", st);
    else
        printf("TEST-3b FAIL: beyond-sbrk access survived\n");
    exit(0);
}
