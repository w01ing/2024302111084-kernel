# Lab4 设计笔记

学号：2024302111084；`LAB4_POLICY_ID = 4`；默认票数
`7 + sid % 13 = 13`。验收基线为单核。

## 起点与总体结构

Lab3 起点只有一个活动用户现场，`fork` 通过保存父现场并让当前执行流充当
子进程，最多支持两层，控制台读取使用关中断加 `wfi`。Lab4 将其替换为
`proc[NPROC]`、每进程锁、每进程内核栈和 trapframe，以及每 CPU 的
`context/proc/noff/intena`。父子关系由 `wait_lock` 保护，tick 条件由
`ticks_lock` 保护。

调度机制与策略分离：`sched/yield/sleep/wakeup` 只负责状态转换和切换；
`pick_next` 按 `LAB4_POLICY_ID` 选择进程。本学号走动态彩票分支，LCG 产生
随机数，初值混合 `r_time()` 与学号。`setickets` 覆盖默认票数；阻塞休眠
属于主动让出，每次增加一票，但只在票数小于 32 时增加。时钟抢占不加票。

## context、trapframe 与 swtch

`swtch(old, new)` 的两个参数位于 `a0/a1`。它保存并恢复 `ra`、`sp` 和
`s0-s11` 共 14 个 callee-saved 寄存器。`ra` 相当于内核执行流的恢复 PC：
恢复后 `ret` 跳到该 context 上次调用 `swtch` 的返回点。caller-saved
寄存器由 C 调用约定允许调用者自行保存，因此不属于 `swtch` 的责任。

新进程在 `allocproc` 中把 `context.sp` 设为该进程内核栈顶，把
`context.ra` 设为 `forkret`。第一次被选中后，`swtch` 的 `ret` 进入
`forkret`；它先释放由调度器跨切换传递的 `p->lock`，再进入
`usertrapret`。

trapframe 保存用户态完整寄存器，用于 U/S 特权级切换；context 只保存
内核调用约定要求长期保留的寄存器，用于内核执行流切换。从用户进程 A
到 B 的栈顺序是：A 用户栈，经 trampoline 切到 A 内核栈，经 `swtch`
切到调度器启动栈，再经 `swtch` 切到 B 内核栈，最后 trampoline 恢复
B 用户栈。不能直接在 A 内核栈上切到 B，因为 A 可能正在退出并等待回收。

## 状态机与锁不变式

```text
UNUSED -> USED -> RUNNABLE -> RUNNING -> RUNNABLE
                     ^           |  \-> SLEEPING -> RUNNABLE
                     |           \----> ZOMBIE -> UNUSED
```

`allocproc` 完成 `UNUSED -> USED`；创建或唤醒完成到 `RUNNABLE`；调度器完成
`RUNNABLE -> RUNNING`；`yield` 完成 `RUNNING -> RUNNABLE`；`sleep/wakeup`
完成睡眠往返；`exit/wait` 完成 `ZOMBIE -> UNUSED`。

进入 `sched` 时逐项检查：当前进程锁已持有、`noff == 1`、状态不是
`RUNNING`、中断关闭。进程锁必须跨越 `swtch`，防止进程刚标成
`RUNNABLE`、尚未离开其内核栈时又被调度。调度器和进程在切换两侧交接
这把锁。

## 时钟、抢占与统计

Sstc 由 M 态开放，S 态用 `stimecmp` 产生中断。一个 tick 定义为一次
定时器中断，间隔为 100000 个 `time` CSR 计数；`LAB2_TICK=2` 表示
MLFQ 最高层时间片为 2 个 tick，后续层逐级乘 2。只在用户态时钟中断
中累加 `cpu_ticks/last_slice_ticks` 并抢占；内核态中断只推进全局 tick
和唤醒睡眠者，不在内核中途切换。

调度器每轮先开中断，再选择一个 `RUNNABLE` 进程；没有就绪进程时执行
`wfi`。彩票分支先汇总就绪票数，再用随机落点选择；扫描起点使用轮转
游标，避免固定低槽位偏置。被分发前将 `last_slice_ticks` 清零。

## 无丢失唤醒

错误交错是：A 检查条件为空，先释放条件锁；B 取得条件锁、生产并调用
`wakeup`，但此时 A 尚非 `SLEEPING`；B 的唤醒落空；A 随后睡眠并永久
等待。正确 `sleep(chan, lk)` 的顺序是先持有 `lk` 获取 `p->lock`，再
释放 `lk`，随后填写 `chan/SLEEPING` 并 `sched`。唤醒方修改目标状态前
也必须持目标 `p->lock`，因此条件检查与进入睡眠之间不存在可见空窗。

`wait/exit` 固定按 `wait_lock -> p->lock` 的顺序。`wait` 在没有僵尸
子进程时睡在父进程地址上；`exit` 在同一父子关系临界区唤醒父进程，
再把自己置为 `ZOMBIE`。资源由父进程 `wait` 回收，退出进程不能释放
自己仍在使用的内核栈。

复现记录：曾临时把顺序改为先 `release(lk)`，并在获取 `p->lock` 前强制
`yield()` 来稳定放大竞态。运行 `bench` 后 10 秒没有第一条 RESULT；Ctrl-P
显示父测试进程 PID 2 为 `SLEEPING`，子进程 PID 3 已为 `ZOMBIE`。子进程
的 `wakeup` 发生时父进程尚未标记为睡眠，随后父进程入睡，现场与上述交错
完全一致。测试后已恢复正确顺序，`bench` 再次通过。

`kill` 只设置 `killed`；若目标睡眠则改为 `RUNNABLE`。目标在返回用户态
前自行 `exit`，避免从其他执行流中释放它正在使用的页表、trapframe 或栈。

## 策略元数据与接口

调度元数据放在 `struct proc`，与状态一起受 `p->lock` 保护：票数、pass、
MLFQ 层级、优先级、等待 tick 和统计计数。新进程先取策略默认值；fork
继承父进程显式票数和优先级，但 MLFQ 从最高层开始，stride pass 对齐当前
最小值。按名称分票的 ID 5/7 在 exec 成功时更新，显式 `setickets` 始终
优先。

`schedstat` 持目标进程锁快照计数，释放锁后才 `copyout`；已退出或不存在
的 PID 返回 -1。`time_rdtime` 直接返回 `r_time()`；`pause` 在
`ticks_lock` 下睡眠到目标 tick；`wait` 用 `copyout` 写回退出码。

## 自测与验收命令

- `bench`：fork/exit/wait 往返和 8 子进程并发回收。
- `lottery_test`：课程测试的 60:20 份额检查。
- `lottery_observe`：自写的 60:20 与 20:60 两组反向改票检查。
- `lottery_reticket`：同一对子进程存活期间从 60:20 改为 20:60，分别采样
  改票前后 CPU 份额；完整用例与实测归档见 `docs/lab4-scheduler-selftests.md`。
- `alt`：两个计算进程的抢占交错检查。
- Ctrl-P：打印 PID、状态、CPU tick、当前片长、票数和名称。
- `badaccess`、`guardtest`、`crosscopy`、`memcap`：Lab3 回归；启动时保留
  `DUMP-PAGETABLE begin/end`。

完整保存 31 个通用寄存器也能实现上下文切换，但会扩大 context、增加每次
切换的访存，并重复保存 ABI 已允许破坏的 caller-saved 寄存器；14 个
callee-saved 寄存器已经满足 C 调用约定。
