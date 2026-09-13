# `echo hi` 全链条图纸材料

这三份材料以完整可运行的 xv6-riscv 参考内核为对象，用来反推你当前 lab1 baseline 后续要补齐的内核能力。你的仓库目前还是 lab1 启动骨架，只有 `memlayout.h` / `riscv.h` 里的硬件常量已经接近 xv6 风格；因此下面的 `fork/exec/wait/syscall/scheduler` 链路是参考内核级图纸，不是当前骨架已经具备的功能。

可渲染图源：

- [material-1-control-flow.dot](/home/wly/2024302111084-kernel/docs/material-1-control-flow.dot)
- [material-2-data-snapshot.dot](/home/wly/2024302111084-kernel/docs/material-2-data-snapshot.dot)
- [material-3-timer-interrupt.dot](/home/wly/2024302111084-kernel/docs/material-3-timer-interrupt.dot)

校对依据：

- [xv6-book-riscv-rev5.pdf](/home/wly/2024302111084-kernel/docs/xv6-book-riscv-rev5.pdf)
- Chapter 1 `Operating system interfaces`：用来校对 shell 作为普通用户程序、系统调用接口、`fork/exec/wait/read/write/exit` 语义、文件描述符继承。
- Chapter 4 `Traps and system calls`：用来校对 `ecall`、`uservec -> usertrap -> syscall/device driver -> userret`、`trapframe`、`trampoline`、`sepc/scause/sstatus/sscratch`、`sret`。

若本机安装了 Graphviz，可执行：

```sh
dot -Tsvg docs/material-1-control-flow.dot -o docs/material-1-control-flow.svg
dot -Tsvg docs/material-2-data-snapshot.dot -o docs/material-2-data-snapshot.svg
dot -Tsvg docs/material-3-timer-interrupt.dot -o docs/material-3-timer-interrupt.svg
```

## 材料一：全系统控制流图

入口事件是用户在 shell 中输入 `echo hi` 并敲回车。主链路如下：

1. `sh` 在 U 态调用 `read(0, buf, n)`，由 `ecall` 进入 `uservec -> usertrap -> syscall -> sys_read -> fileread -> consoleread`。
2. 若还没有完整行，`consoleread` 持有 `cons.lock` 检查输入缓冲，随后通过 `sleep(&cons.r, &cons.lock)` 原子释放 `cons.lock`、持有 `p->lock` 并把 shell 置为 `SLEEPING`，再经 `sched -> swtch` 切到 CPU 的 scheduler 栈。
3. UART 输入中断到来后，内核经 `kernelvec/kerneltrap -> devintr -> uartintr -> consoleintr` 将字符放入 `cons.buf`；遇到换行时 `wakeup(&cons.r)` 唤醒 shell。
4. 调度器重新选择 shell，`read` 返回命令行。shell 解析后调用 `fork()`，父进程进入 `wait()`，子进程在 fork 返回值为 0 的分支调用 `exec("echo", argv)`。
5. `exec` 装入 echo ELF，替换子进程用户页表，设置 `trapframe->epc` 为 echo 入口。`usertrapret -> userret -> sret` 后，echo 第一条用户指令开始运行。
6. echo 的 `main` 调用 `write(1, "hi\n", 3)`，内核经 `sys_write -> filewrite -> consolewrite -> uartputc/uartstart` 输出到 UART，屏幕显示 `hi`。
7. echo 调用 `exit(0)`，内核关闭文件、唤醒父进程并把 echo 置为 `ZOMBIE`。shell 的 `wait()` 回收子进程后返回。

图中每个节点都标注了栈、特权级和锁状态。重点记忆三种栈：

- 用户栈：执行 shell/echo 用户代码时使用。
- 进程内核栈：执行该进程系统调用、中断返回路径、`yield/sched` 前半段时使用。
- CPU scheduler 栈：`swtch(&p->context, &cpu->context)` 之后，调度器循环运行时使用。
- M 态说明：参考内核启动时会先在 M 态配置 PMP 与中断/异常委托，然后 `mret` 进入 S 态；`echo hi` 运行期的 syscall、UART 中断、timer 中断均在 U/S 态之间切换。

自主批注已在图中标出三处：`read` 的真实阻塞点、`fork/exec` 的返回语义、UART 输出的中断续传边界。

### 与 xv6 book 校对

第 1 章的 Figure 1.1 把系统分成 `user space` 与 `kernel space`，用户程序通过 `system call` 进入内核。图 1 中所有 `read/fork/exec/write/exit/wait` 都画成 U 态用户调用，经 `ecall/usertrap/syscall` 到 S 态内核处理，和 Figure 1.1 的边界一致。

第 1 章 Figure 1.2 列出 xv6 系统调用：`fork` 创建进程，`exec` 装载文件并执行，`write/read` 通过文件描述符 I/O，`exit` 终止当前进程，`wait` 等待子进程退出。图 1 的 C/D/E 阶段按这个接口语义串联：shell 先 `fork`，父进程 `wait`，子进程 `exec("echo")`，echo 再 `write` 和 `exit`。

第 1 章还说明 shell 是普通用户程序，`exec` 成功后子进程执行 echo 而不是继续执行旧 shell 代码；同时文件描述符表在 `fork` 后复制，`exec` 后保留。这校对了图 1 中“父 shell 等待、子进程 exec echo、stdout 仍为 fd=1”的逻辑。

## 材料二：核心数据结构快照

截面时刻：`exec("echo", argv)` 已经成功返回到内核路径，`echo` 的第一条用户指令尚未执行。

### 进程表快照

| 进程 | 典型状态 | parent | pagetable | sz | 关键 fd |
| --- | --- | --- | --- | --- | --- |
| `init` | `SLEEPING` 或 `RUNNABLE` | `0` | `init_pt` | init 用户地址空间大小 | `fd0/fd1/fd2 -> console` |
| `sh` | 常见为 `SLEEPING(wait)` | `init` | `sh_pt` | shell 程序大小、堆、栈 | 继承 console 标准输入输出错误 |
| `echo` | `RUNNABLE`，或正在 `usertrapret` 前为 `RUNNING` | `sh` | `echo_pt` | `PGROUNDUP(text+data)+2*PGSIZE` 左右 | `fd0/fd1/fd2` 从 shell 继承 |

关键点：`exec` 替换的是子进程的用户地址空间和用户入口，不替换 `struct proc` 本身，所以 pid、parent、打开文件表仍来自 fork 后的子进程。

### echo 页表快照

本仓库 [memlayout.h](/home/wly/2024302111084-kernel/kernel/memlayout.h:52) 定义：

- `MAXVA = 1 << 38 = 0x4000000000`
- `PGSIZE = 4096`
- `TRAMPOLINE = MAXVA - PGSIZE = 0x3ffffff000`
- `TRAPFRAME = TRAMPOLINE - PGSIZE = 0x3fffffe000`

echo 的用户页表包含：

| 虚拟地址区间 | 内容 | PTE 权限 |
| --- | --- | --- |
| `0x0 .. text_end` | ELF 代码段 | `PTE_V | PTE_R | PTE_X | PTE_U` |
| `text_end .. data_end` | rodata/data/bss | rodata 常见 `R|U`，data/bss 为 `R|W|U` |
| `data_end .. stack_guard` | heap，可由 `sbrk` 扩展 | `PTE_V | PTE_R | PTE_W | PTE_U` |
| `sz-2*PGSIZE .. sz-PGSIZE` | guard page | 无效，或清除 `PTE_U` |
| `sz-PGSIZE .. sz` | 用户栈，含 argv | `PTE_V | PTE_R | PTE_W | PTE_U` |
| `0x3fffffe000 .. 0x3ffffff000` | `TRAPFRAME` | `PTE_V | PTE_R | PTE_W`，无 `PTE_U` |
| `0x3ffffff000 .. 0x4000000000` | `TRAMPOLINE` | `PTE_V | PTE_R | PTE_X`，无 `PTE_U` |

### 文件表快照

标准输出链路是：

`echo->ofile[1] -> struct file(type=FD_DEVICE, writable=1, major=CONSOLE, ip=&inode_console) -> struct inode(type=T_DEVICE, major=CONSOLE) -> devsw[CONSOLE].write -> consolewrite -> uartputc`

`fd0` 是标准输入，走 `devsw[CONSOLE].read -> consoleread`；`fd1` 和 `fd2` 分别是标准输出与标准错误，通常都指向 console 设备文件对象或由 `dup` 得到的同源引用。

自主批注已在图中标出三处：`exec` 不替换 `proc`、高地址陷阱页无 `PTE_U`、stdout 的 `file -> inode -> devsw` 链路。

### 与 xv6 book 校对

第 1 章 `I/O and File descriptors` 说明：文件描述符是进程持有的小整数，指向内核管理对象；`0` 是输入，`1` 是输出；`fork` 会复制文件描述符表，`exec` 替换内存但保留文件表。因此图 2 把 `echo.ofile[1]` 继续指向 stdout，并连到 `file -> inode -> devsw[CONSOLE]`，和书中接口语义一致。

第 4 章解释 `TRAMPOLINE` 与 `TRAPFRAME` 是用户态陷入内核和返回用户态的支点；用户页表必须映射 trampoline，trapframe 保存用户寄存器。图 2 的高地址页表快照保留 `0x3fffffe000..0x3ffffff000` 的 `TRAPFRAME` 与 `0x3ffffff000..0x4000000000` 的 `TRAMPOLINE`，并标注无 `PTE_U`，与第 4 章 trap 机制吻合。

第 4 章也说明内核页表和用户页表不同，内核通过安全拷贝函数访问用户指针；所以图 2 没有把 echo 的用户内存直接画成内核可任意访问，而是通过 `pagetable`、PTE 权限和 trapframe/trampoline 边界来表达隔离。

## 材料三：一次时钟中断的微观旅程

选择任意正在 U 态运行的进程 P。时钟中断到来时：

1. 硬件写入 `scause=0x8000000000000005`、`sepc=被中断用户 PC`，并把 `sstatus.SPP` 记录为 U 态，随后跳到 `stvec` 指向的 `uservec`。
2. `uservec` 在 trampoline 页执行，先用 `sscratch` 找到 trapframe，保存全部用户寄存器；随后加载 `kernel_sp`、`kernel_satp`、`kernel_trap`、`kernel_hartid`，切换到该进程内核栈和内核页表。
3. `usertrap` 设置 `stvec=kernelvec`，保存 `p->trapframe->epc=r_sepc()`。因为这是中断，不是 `ecall`，所以 `sepc` 不加 4。
4. `devintr` 识别 supervisor timer interrupt，更新 `ticks`、唤醒等待 tick 的进程，并设置下一次 timer compare。此处会短暂持有 `tickslock`。
5. 若当前进程可被抢占，`usertrap` 调用 `yield()`。`yield` 获取 `p->lock`，把 P 从 `RUNNING` 改为 `RUNNABLE`，进入 `sched()`。
6. `swtch(&p->context, &cpu->context)` 保存内核上下文 `ra/sp/s0..s11`，从进程内核栈切到 CPU scheduler 栈。
7. `scheduler()` 释放旧进程锁，扫描进程表，选择一个 `RUNNABLE` 进程。它可能重新选中 P，也可能先运行 Q。
8. 当 P 被再次选中时，`swtch` 回到 P 的 `sched()` 返回点，`yield` 返回，`usertrap` 继续执行 `usertrapret()`。
9. `usertrapret` 关闭中断窗口，恢复用户陷阱入口 `stvec=uservec`，填好 trapframe 中的内核字段，设置 `sstatus.SPP=0`、`SPIE=1`、`sepc=p->trapframe->epc`。
10. `userret` 切换 `satp` 到用户页表，执行 `sfence.vma`，从 trapframe 恢复用户寄存器，最后 `sret` 回到 U 态。

自主批注已在图中标出三处：中断路径 `sepc` 不加 4、`swtch` 与 trapframe 分工、`yield` 不保证一定换到其他进程。

### 与 xv6 book 校对

第 4 章 Figure 4.1 给出的用户态 trap 总路径是：`User code -> trampoline/uservec -> usertrap -> syscall or device driver -> trampoline/userret -> User code`。图 3 正是把这条骨架展开到 timer interrupt：`A1-A4` 对应用户态到 `uservec/usertrap`，`B1-B3` 对应设备中断处理，`D2-D4` 对应 `usertrapret/userret/sret` 回到用户态。

第 4 章 `RISC-V trap machinery` 说明 `stvec` 决定 trap 入口，`sepc` 保存被中断 PC，`scause` 记录原因，`sscratch` 帮助在覆盖用户寄存器前找到保存位置，`sstatus.SPP` 决定 `sret` 返回 U 态还是 S 态。图 3 表格逐项标出了这些寄存器，所以和书上的硬件机制相符。

第 4 章强调硬件不会自动切换内核页表、不会自动切内核栈、也不会保存除 PC 外的全部寄存器，这些必须由内核软件完成。图 3 因此把 `uservec` 单独标出，说明它保存用户寄存器到 `trapframe`、切换到内核页表和进程内核栈；再由 `userret` 恢复寄存器并执行 `sret`。

## 验收清单对照

- 控制流覆盖 `read -> fork -> exec -> write -> exit`：见材料一主链路。
- 栈与特权级转换：材料一、三每个关键节点均标注；M 态只出现在启动/委托说明中，运行期不回到 M 态。
- 数据结构字段：材料二包含 `state/parent/pagetable/sz/ofile[]`、`file -> inode -> devsw`。
- `TRAMPOLINE/TRAPFRAME` 边界与权限：材料二明确列出 `0x3ffffff000` 和 `0x3fffffe000`。
- 时钟中断现场保护、调度交接、`sret` 恢复：材料三完整覆盖。
- 至少 3 处自主批注：每张图各 3 处，总计 9 处。
- 已对照 xv6 book 第 1 章与第 4 章：材料一校对系统调用接口和 shell 语义；材料二校对 fd/exec 保留文件表和 trap 高地址页；材料三校对 Figure 4.1 的 trap 返回骨架与 RISC-V trap CSR。
