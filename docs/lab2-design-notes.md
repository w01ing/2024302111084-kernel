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

## ③ Lab3 虚拟内存设计约束

本节是后续 SV39、物理页分配和进程地址空间实现的资源管理合同。实现中
如果函数名使用 `vm_map`，其语义等价于实验要求中的 `mappages`。

### 1. 映射失败的回滚与所有权

一次多页映射是事务操作：

- 成功返回前，所有目标叶子 PTE 都必须已经有效；
- 第 N 页或任何中间级页表分配失败时，撤销本次调用已经建立的前 N-1
  个叶子映射；
- 本次调用新申请、且不再被其他 PTE 引用的中间级页表页必须释放；
- 回滚不能破坏调用前已经存在的映射，也不能释放调用方在 `pa` 参数中
  传入的物理页。

物理页所有权由调用方负责：`mappages/vm_map` 只写入 PTE，不接管叶子
物理页，也不在失败时替调用方 `kfree(pa)`。因此调用方必须遵循：

1. 先分配待映射物理页；
2. 调用映射函数；
3. 映射失败时由调用方释放尚未转移到地址空间的叶子页；
4. 映射成功后由地址空间销毁路径统一释放这些页。

中间页表页是映射函数内部申请的资源。只有本次调用新建、回滚后变成
空表且没有其他引用的中间页表页由映射函数释放。后续实现应记录本次
新建的页表页，或在回滚后检查空表，避免误释放共享页表。

### 2. Guard page 与用户异常

guard page 不分配、也不映射实际物理页；它只保留一段未设置 `PTE_V` 的
虚拟地址。这样既不消耗物理页，也不会给用户提供任何访问权限。

用户访问 guard page 时由硬件产生页故障：取指是 instruction page fault
（通常 `scause=12`），读是 load page fault（`scause=13`），写是 store/
AMO page fault（`scause=15`）。`stval` 保存触发故障的虚拟地址。

`usertrap` 根据当前来自 U 态、异常原因和 `stval` 查验当前进程页表；
确认是用户地址未映射、权限不满足或越过用户栈 guard page 后，将当前
进程标记为 killed，并从进程退出/回收路径结束它。该路径不能把用户
故障当成内核页表损坏，也不能进入内核 panic 路径。若故障发生在 S
态，才进入 `kerneltrap` 的内核故障处理路径。

### 3. 物理内存配额与错误传播

物理页分配范围为 `[PGROUNDUP(kernel_end), PHYSTOP -
LAB3_MEMCAP_DELTA * PGSIZE)`；保留区不进入 freelist。`kalloc()` 在
freelist 为空时返回 `0`，不 panic。

所有可能触发分配的函数使用统一的失败约定：底层分配失败返回 `0`，
映射、地址空间创建、复制和 `sbrk` 等返回 `-1`。错误必须逐层向上传递：

`kalloc -> walk/map -> uvm/exec/sbrk -> syscall`。

每一层在返回错误前都要回滚本层已经取得的资源：已建立的叶子映射、
本层新建的中间页表、已经复制的用户页以及尚未提交的物理页。成功的
旧地址空间不能因一次新的分配失败而被破坏；例如 `sbrk` 只有在全部
新增页面映射成功后才更新 `proc->sz`。

### 4. copyin/copyout 的拷贝策略

采用“按页切分、每页先检查 PTE、页内按字节复制”的策略。每轮计算当前
页剩余长度，使用 `walkaddr` 检查：PTE 有效、包含 `PTE_U`，并满足读或
写权限；然后最多复制到当前页尾，再进入下一页。

三种策略的取舍如下：

| 策略 | 优点 | 风险/代价 |
| --- | --- | --- |
| 逐字节先查权限 | 边界最直观 | 页表遍历开销极大，吞吐差 |
| 按字对齐直接批量复制 | 常数开销低 | 跨页、未对齐和最后不足一个字时容易越界；不能绕过权限检查 |
| 按页检查后批量复制 | 每页只检查一次，边界明确，跨页安全 | 仍需处理页首/页尾，代码略复杂 |

本实验选择第三种策略；页内复制可以使用简单字节循环，先保证边界和
权限语义正确，再考虑字对齐优化。`copyinstr` 复用单字节长度检查，
遇到 `NUL` 成功，达到最大长度仍未结束或任一页无权限则返回 `-1`。

### 5. `struct proc` 的内存字段与生命周期

正式进程结构至少包含以下字段：

```c
struct proc {
  pagetable_t pagetable;       // 用户根页表
  uint64 sz;                   // 用户可用地址空间顶端/堆边界
  uint64 stack_bottom;         // 用户栈有效区下界
  uint64 stack_top;            // 用户栈顶
  struct trapframe *trapframe; // 固定映射到 TRAPFRAME
  uint8 *kstack;               // 内核栈及其 guard page
  struct context context;      // 调度器保存的内核现场
  struct proc *parent;
  enum procstate state;
};
```

生命周期约定：

- 创建进程时由 `allocproc/uvmcreate` 分配根页表、trapframe、内核栈和
  用户页表页；建立 trampoline、trapframe、代码、数据、栈和 guard page
  映射后，才把 `pagetable` 提交给进程；
- `fork` 由 `uvmcopy` 创建子页表和用户页副本，子进程获得独立的
  `pagetable`，父子进程的页表不得共享可写用户页；
- 上下文切换由 scheduler/proc 代码把 `MAKE_SATP(p->pagetable)` 交给
  `usertrapret`，并用 `p->trapframe` 恢复用户现场；内核页表由
  `trapframe->kernel_satp` 固定指向；
- `exec` 在新地址空间完整构建成功前保留旧 `pagetable`。成功后切换
  `p->pagetable`、`p->sz` 和入口现场，再释放旧地址空间；失败时旧地址
  空间继续有效；
- `exit/wait` 回收进程时先解除用户叶子映射并释放用户物理页，再释放
  中间页表、根页表、trapframe 和内核栈。内核共享页表、trampoline 和
  调用方仍持有的物理页不得被进程销毁路径释放。

当前简化运行时中的 `cur/parent/nested_parent` 是上述 `struct proc`
生命周期的过渡实现；后续新增内存管理功能必须遵循同样的所有权、提交
和回滚规则，不能再通过全局静态镜像绕过页表生命周期。

## ④ 阶段一：物理页分配器

### 地址范围与配额

`kinit` 使用链接脚本导出的 `kernel_end` 作为内核镜像之后的第一个可用
地址，并向上按 `PGSIZE` 对齐。可用上界为：

```text
alloc_hi = PHYSTOP - LAB3_MEMCAP_DELTA * PGSIZE
```

因此管理范围是半开区间 `[PGROUNDUP(kernel_end), alloc_hi)`；尾部保留
页永远不进入空闲链表。这里必须使用 `kernel_end`，不能使用传统 xv6
示例中的 `end` 符号。

### 空闲链表与分配顺序

每个空闲页的首个字长复用为 `struct run.next`，不额外建立每页元数据。
这使得 128MiB 物理内存只需要一个空闲链表头，页级元数据开销为常数级。

- `LAB3_ALLOC_ORDER=0`：低地址页优先；
- `LAB3_ALLOC_ORDER=1`：高地址页优先；
- `LAB3_ALLOC_ORDER=2`：以中间页为起点，向两侧交替扩展。

链表初始化按目标分配顺序的逆序头插，`kalloc` 和 `kfree` 都是 `O(1)`。
其中顺序链表的空间复杂度为 `O(1)`；若采用“按地址有序插入”的回收
方案，单次 `kfree` 为 `O(n)`，批量释放 `n` 页会退化到 `O(n^2)`，在
`memcap` 测试中不可接受，因此不采用有序插入。

### 垃圾填充与清零

`kfree` 在把页面挂入链表前填充固定垃圾字节 `0x01`，用于隔离旧页内容，
并帮助发现释放后继续使用（use-after-free）的问题。`kalloc` 从链表取页
后整页清零，防止新拥有者读取前一个拥有者留下的数据。两者目的不同：

- 释放时填充是调试和数据隔离措施，不能替代分配时清零；
- 分配时清零是新页可见数据的安全边界，不能依赖释放者一定执行过填充。

`kinit` 建链时也执行垃圾填充，因此临时验证可以循环分配全部页、检查
4KB 对齐和清零结果，再释放后继续分配到 `kalloc()==0`。该验证代码应由
条件编译宏隔离，正式构建不改变启动输出。

## ⑤ 阶段二：页表遍历与回滚映射

### Sv39 索引与 `walk`

39 位虚拟地址按 `VPN[2] | VPN[1] | VPN[0] | page offset` 划分。索引
通过右移 `12 + 9 * level` 位并与 `0x1ff` 掩码得到，统一封装为：

```c
PX(level, va) = (va >> (PGSHIFT + 9 * level)) & PXMASK
```

`walk(pt, va, alloc)` 从二级根页表依次访问 VPN[2]、VPN[1] 和 VPN[0]。
中间级 PTE 只有 `PTE_V` 而没有 R/W/X，表示下一级页表；叶子 PTE
具有 R/W/X 中至少一位，表示最终物理页。遍历到第零级后返回叶子 PTE
指针，而不是返回物理地址，原因是调用方还需要读取或修改 PTE 的物理页号
和权限位。

### `mappages` 语义

`mappages` 在真正分配前先检查目标范围内是否已有叶子映射；重复映射直接
返回 `-1`，不覆盖旧 PTE，也不释放调用方物理页。映射成功时设置：

- `PTE_V` 和调用方指定的 R/W/X/U 权限；
- `PTE_A`，避免硬件访问位更新导致异常；
- 可写页的 `PTE_D`。

### 失败回滚

映射推进到中途失败时，先撤销本次调用已经建立的叶子 PTE，再沿对应路径
清理空的零级、一级页表。根页表本身不释放。叶子物理页始终由调用方
负责：`mappages` 只清除 PTE 和内部新建的中间页表，不释放 `pa` 参数
指向的页面。

为避免回滚误删调用前已有的映射，映射前置检查整个目标范围；因此真正
开始写入后，目标范围内的叶子映射全部属于当前事务。阶段二的临时自检
使用 `LAB3_STAGE2_SELFTEST` 条件编译，验证：

1. 三个不同页表层级路径的映射和 `walk` 返回值；
2. R/W/X/U 权限标志；
3. 重复映射拒绝覆盖；
4. 通过循环 `kalloc` 将真实空闲页消耗到 4 页，对 64MiB 区间发起需要
   大量中间页表的映射，使分配在中途失败；随后检查根页表项为空、失败
   前后的空闲页数相同，并释放暂存页验证总空闲页数恢复。
