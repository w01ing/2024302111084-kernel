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

## 用例三：ID 4 动态加票与短窗口收敛

源码：`user/lottery_dynamic.c`

目的：同一对进程先以默认票数 13:13 竞争，确认累计份额接近 50%；随后
第一个进程调用 `pause(1)` 主动让出 25 次。其前 19 次让票数从 13 增加到
32，后续 6 次验证上限不再增长；第二个纯计算进程始终保持 13 票。加票
阶段预留 200 tick，确保短窗口采样开始时两个进程都已恢复为纯计算。动态
阶段按 20 tick 短窗口打印瞬时份额和累计份额，最终累计份额应接近
`32/(32+13)`，即约 71%。

运行命令：

```text
sh> lottery_dynamic
```

程序打印 `INSPECT` 后保留 1000 tick 的观测期；此时按 `Ctrl-P`，进程表中
的两个测试子进程应分别显示 32 和 13 票。
彩票调度允许单个窗口随机波动，验收看累计列逐渐稳定以及最终
`PASS: lottery_dynamic`。

实际运行摘要：

```text
PHASE baseline tickets=13:13 expected_share=50
...
PHASE dynamic tickets=32:13 expected_share=71
WINDOW dynamic_1 cpu=15:4 share=78 cumulative=78
WINDOW dynamic_2 cpu=12:8 share=60 cumulative=69
WINDOW dynamic_3 cpu=16:3 share=84 cumulative=74
...
WINDOW dynamic_8 cpu=14:6 share=70 cumulative=73
INSPECT press Ctrl-P now; pid 3 should have 32 tickets, pid 4 should have 13

PID STATE CPU SLICE TICKETS NAME
3 runble 843 1 32 lottery_dynamic
4 run 382 0 13 lottery_dynamic

RESULT baseline_13_13 = 73:78 share=48
RESULT dynamic_32_13 = 113:41 share=73
PASS: lottery_dynamic
```

结论：默认等票阶段的累计占比为 48%，接近理论 50%；主动让出阶段后
`Ctrl-P` 观察到票数为 32:13，证明票数达到且没有超过 32；动态阶段累计
占比为 73%，接近理论值 71%，短窗口累计列稳定在理论值附近。
