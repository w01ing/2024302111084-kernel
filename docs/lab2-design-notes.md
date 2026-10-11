# Lab2 设计笔记：系统调用与 UART 输入中断

这两张图是后续实现的接口级施工图。图中的地址、寄存器偏移和
`trapframe` 字段必须与 `lab2/gifts/kernel/trampoline.S` 保持一致。

## ① 一次 `write` 系统调用的完整往返

- [可编辑 DOT 源码](./lab2-write-syscall.dot)

场景：用户程序执行 `write(1, buf, n)`，其中 `buf` 是用户地址，返回值为
实际写出的字节数或 `-1`。

### 关键约定

| 阶段 | 特权级与栈 | 关键寄存器/现场 | 下一步 |
| --- | --- | --- | --- |
| 用户桩 | U；当前用户栈 | `a0=fd`，`a1=buf`，`a2=n`，`a7=SYS_write=16`；`ra` 是用户桩返回地址 | `ecall` |
| 硬件入陷 | U -> S；硬件不替换栈 | `sepc=ecall` 地址，`scause=8`，`sstatus.SPP=0`；`stvec=uservec` | 跳到 trampoline |
| `uservec` 前半段 | S；仍暂时使用用户页表，随后切换 | `sscratch` 暂存用户 `a0`；`a0=TRAPFRAME`；保存 `ra/sp/gp/tp/t0..t6/s0..s11/a1..a7` | 从 trapframe 取 `kernel_sp`、`kernel_satp`、`kernel_trap` |
| `uservec` 后半段 | S；当前进程内核栈 | `satp=kernel_satp`；`sp=kernel_sp`；`tp=kernel_hartid`；跳转 `usertrap` | C 代码处理 trap |
| `usertrap` | S；进程内核栈 | `trapframe->epc=r_sepc()`；`stvec=kernelvec`；识别 `scause=8` 后 `epc += 4`，跳过 `ecall` | `syscall` |
| syscall 分发 | S；进程内核栈 | 从 `trapframe->a7` 取 16；从 `trapframe->a0..a2` 取参数；未知号返回 `-1` | 调用 `sys_write` |
| `sys_write`/console | S；进程内核栈 | 校验 `fd`、`buf`、`n`；从用户地址读取字节；写入 console/UART 输出队列 | 返回 `n` 或 `-1` |
| 返回准备 | S；进程内核栈 | `trapframe->a0=返回值`；`sepc=trapframe->epc`；`sstatus.SPP=0`、`SPIE=1`；`stvec=uservec` | 调 `userret(TRAMPOLINE, user_satp)` |
| `userret`/`sret` | S -> U；恢复用户栈 | 切回用户 `satp`；从 `TRAPFRAME` 恢复除 `a0` 外所有寄存器，最后恢复返回值 `a0`；`sret` 用 `sepc` | 用户代码从 `ecall+4` 继续 |
| 用户桩返回 | U；用户栈 | `ret` 使用仍在 `ra` 中的用户返回地址；C 表达式得到 `a0` 返回值 | 用户程序继续执行 |

### `trapframe` 字段对照

赠送的 trampoline 使用以下偏移，内核结构体必须按相同布局提供字段：

| 偏移 | 字段 | 用途 |
| ---: | --- | --- |
| `0` | `kernel_satp` | `uservec` 切换到内核地址空间 |
| `8` | `kernel_sp` | `uservec` 切换到当前进程内核栈 |
| `16` | `kernel_trap` | `uservec` 间接跳转到 `usertrap` |
| `32` | `kernel_hartid` | 恢复当前 hart 到 `tp` |
| `40` | `ra` | 用户返回地址寄存器 |
| `48` | `sp` | 用户栈指针 |
| `56..104` | `gp/tp/t0..t2/s0/s1` | 用户通用寄存器 |
| `112` | `a0` | 通过 `sscratch` 交换保存的用户参数/返回值 |
| `120..168` | `a1..a7` | syscall 参数和调用号 |
| `176..248` | `s2..s11` | 用户保存寄存器 |
| `256..280` | `t3..t6` | 用户临时寄存器 |

两个容易写错的点：

1. `ecall` 的 `sepc` 必须加 4；普通中断的 `sepc` 不加 4。
2. `trapframe` 保存用户寄存器，进程 `context` 保存调度所需的内核
   `ra/sp/s0..s11`，两者不是同一份现场。

## ② 一次键盘输入中断的完整路径

这里的“键盘”指 QEMU 终端输入经串口送入 UART 的接收路径。场景是
Shell 已经在 `read(0, ...)` 中睡眠，直到收到一整行才被唤醒。

- [可编辑 DOT 源码](./lab2-uart-input-interrupt.dot)

### 关键约定

本人的参数是 `LAB2_BUF_SEMANTICS=0`、`LAB2_BUF_SIZE=32`，因此采用行缓冲：

- `cons.buf[32]` 是环形字节缓冲；
- `cons.r` 指向待读取位置；
- `cons.w` 指向已经提交给读者的位置；
- `cons.e` 指向已接收/编辑位置；
- 普通字符先写入 `buf[e % 32]` 并递增 `e`；
- 收到换行，或缓冲区达到 32 字节时，将 `w=e` 并
  `wakeup(&cons.r)`；
- `consoleread` 只消费 `[r,w)`，读完已提交数据且仍需更多数据时再次
  `sleep(&cons.r, &cons.lock)`；
- 超过容量且尚未被读者消费的字符按约定丢弃，不修改 `r/w`，不能造成
  计数越界或唤醒丢失。

### 中断现场与 Shell 状态

Shell 睡眠后不再占用 CPU。UART 中断发生时，硬件只写入 `sepc/scause`
并跳到 `stvec`，不会自动切换栈或页表。因此中断入口使用的是当时正在
运行的内核代码所在栈，可能是另一个进程的内核栈，也可能是 CPU
scheduler 栈；不能把“唤醒 Shell”误画成“继续使用 Shell 用户栈”。

`wakeup` 只把 Shell 的状态从 `SLEEPING` 改为 `RUNNABLE`。它不会立即
恢复 Shell 寄存器；之后由 scheduler 重新选择 Shell，回到它调用
`sleep` 之后的内核返回点，再继续 `consoleread`。

### UART/PLIC 的寄存器动作

| 动作 | 地址/寄存器 | 作用 |
| --- | --- | --- |
| 开启接收中断 | `UART0 + 1`，IER bit 0 | 允许 UART RX 触发 PLIC 外部中断 |
| 识别接收数据 | UART IIR/LSR | 判断存在接收事件 |
| 取字符 | `UART0 + 0`，RHR | 读取一个输入字节，同时推进 UART 接收状态 |
| 领取外部中断 | `PLIC_SCLAIM(0)` | 得到中断号，确认是 `UART0_IRQ=10` |
| 完成外部中断 | `PLIC_SCLAIM(0)` 写回 10 | 告知 PLIC 本次 UART 中断已处理 |

### 与 `read` 的衔接

1. Shell 的 `consoleread` 持有 `cons.lock`，发现 `cons.r == cons.w`，
   调用 `sleep(&cons.r, &cons.lock)`。
2. `sleep` 必须在释放 `cons.lock` 前持有当前进程锁，并把 Shell 状态设为
   `SLEEPING`，再切换到 scheduler；否则字符可能在检查和入睡之间到达，
   造成永久睡眠。
3. UART 中断处理先获取 `cons.lock`，把字符写入缓冲；换行或满缓冲时
   更新 `cons.w`，调用 `wakeup(&cons.r)`，然后释放锁。
4. scheduler 看到 Shell 为 `RUNNABLE` 后重新运行它；Shell 回到
   `consoleread`，重新检查 `r != w`，复制已提交字节并返回用户态。

## 后续实现顺序

1. 按图实现 trapframe 结构、`uservec` 入口和 `usertrapret` 返回协议。
2. 按图实现 syscall 参数读取、分发和 `sys_write` 返回值写回。
3. 实现 UART 初始化、PLIC 领取/完成和 `kernelvec -> kerneltrap ->
   devintr -> uartintr`。
4. 实现 `cons.buf`、`sleep/wakeup` 的检查-入睡原子性，再接通
   `sys_read`。
