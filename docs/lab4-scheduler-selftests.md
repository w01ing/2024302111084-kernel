# Lab4 调度策略自测归档

学号：2024302111084  
策略：`LAB4_POLICY_ID = 4`，动态彩票调度  
环境：单核 QEMU（`-smp 1`）

## 用例一：固定票数的 CPU 份额

源码：`user/lottery_observe.c`

目的：创建两组纯计算进程，分别设置 `60:20` 和 `20:60` 的票数，使用
`schedstat` 统计观测窗口内的 CPU tick。两组结果应随票数方向反转，且实际
份额与理论值的偏差不超过 20 个百分点。

运行命令：

```text
sh> lottery_observe
```

验收标准：输出两行 `RESULT`，第一组中进程一占优，第二组中进程二占优，
最后输出 `PASS: lottery_observe`。

## 用例二：同一组进程动态改票

源码：`user/lottery_reticket.c`

目的：两个纯计算子进程在第一阶段分别持有 60 和 20 张票；到共同的绝对
时间点后，它们在不退出、不重新创建的情况下调用 `setickets`，将票数改为
20 和 60。父进程用同一 PID 在改票前后各采样一次 CPU tick 增量。

运行命令：

```text
sh> lottery_reticket
```

验收标准：

- `before_reticket_60_20` 中第一个数大于第二个数；
- `after_reticket_20_60` 中第一个数小于第二个数；
- 第一个进程改票前后的份额至少变化 25 个百分点；
- 最后输出 `PASS: lottery_reticket`。

## 实际运行记录

构建命令：

```text
$ make -j4
```

结果：构建成功，新增用户程序被写入 `kernel/userimg.S` 并链接进内核。

用例一实测输出：

```text
sh> lottery_observe
RESULT lottery_case_60_20 = 59:19
RESULT lottery_case_20_60 = 21:57
PASS: lottery_observe
sh>
```

用例二首次实测输出：

```text
sh> lottery_reticket
RESULT before_reticket_60_20 = 145:50
RESULT after_reticket_20_60 = 50:123
PASS: lottery_reticket
sh>
```

为排除彩票随机性造成的偶然通过，同一内核重新启动后又连续运行两次：

```text
RESULT before_reticket_60_20 = 153:43
RESULT after_reticket_20_60 = 39:139
PASS: lottery_reticket

RESULT before_reticket_60_20 = 141:51
RESULT after_reticket_20_60 = 55:120
PASS: lottery_reticket
```

结论：固定票数测试的两组份额方向正确；同进程动态改票测试在 PID 不变的
两个连续阶段中发生明显份额反转。动态用例连续三次通过，`setickets` 的
运行时修改生效。
