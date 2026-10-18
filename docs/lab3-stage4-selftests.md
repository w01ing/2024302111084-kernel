# Lab3 阶段四自测用例归档

## 用例一：用户栈 guard page 异常

### 目的

验证用户态访问用户栈下方的保护页时，硬件产生页故障；内核只终止
当前用户进程，父进程和 Shell 继续运行。读和写两种访问都覆盖。

### 构造

用户栈映射在 `0x40000..0x41000`，下方 `0x3f000..0x40000` 不建立叶子
PTE。`guardtest` 分别 fork 子进程，在用户态读取和写入 `0x3f000`，父进程
通过 `wait` 检查子进程退出状态非零。

### 通过标准

```text
TEST-GUARD PASS: read/write below user stack killed
sh>
```

不能出现 `TEST-GUARD FAIL`、`kernel trap` 或 `panic`。

## 用例二：跨页受控拷贝

### 目的

验证 `copyin` 在用户缓冲区跨越物理页边界时，能够分别检查两个用户页并
完整传输数据，不发生截断、越界或内核崩溃。

### 构造

`crosscopy` 通过 `sbrk(8192)` 申请两页，在第一、第二页交界处构造
`EDGE1234NEXT5678` 共 16 字节的缓冲区，再调用 `write(1, cross, 16)`。

### 通过标准

```text
TEST-COPY BEGIN
EDGE1234NEXT5678
TEST-COPY PASS: cross-page bytes=16
sh>
```

`write` 返回值必须为 16；只返回 8、返回 `-1` 或发生用户/内核异常都算失败。

## 运行方式

```bash
make clean
make kernel/kernel
timeout --kill-after=5s 30s bash -c \
  '(sleep 2; printf "guardtest\\ncrosscopy\\n"; sleep 3) | make qemu' \
  > /tmp/lab3-stage4-selftests.log 2>&1
tr -d '.' < /tmp/lab3-stage4-selftests.log
```

正式构建默认包含两个用户程序；测试完成后应看到两个 `PASS`，并回到
`sh>`。末尾由 `timeout` 产生的 `exit=124` 或 QEMU signal 15 属于主动
结束交互式 QEMU 的正常现象。
