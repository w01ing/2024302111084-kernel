/*
 * LAB3：用户地址空间装载和简化进程操作。
 * 与 Lab2 的静态镜像缓冲区不同，本文件现在为代码、数据、栈和页表
 * 逐页分配物理内存，并通过 vm.c 建立独立的用户虚拟地址空间。
 */
#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "trap.h"
#include "console.h"
#include "exec.h"
#include "vm.h"

extern char _uprog_table[];
/* 用户栈固定放在低地址程序空间上方；其下方保留一页 guard page。 */
#define IMAGE_MAX (128 * 1024UL)
#define USER_STACK_PAGE 0x40000UL
#define USER_STACK_TOP  (USER_STACK_PAGE + PGSIZE)
struct uprog_entry { uint64 start,end; char name[1]; };
/* LAB3：当前简化进程的页表、已用地址空间大小和实验用 PID。 */
struct proc_state { pagetable_t pt; uint64 base, sz; int pid; };
/* cur 是当前活动地址空间；parent/nested_parent 保存 fork 前的父现场。 */
static struct proc_state cur,parent;
static int child_active,child_done;
static int child_status;
static struct trapframe parent_tf;
static struct proc_state nested_parent;
static struct trapframe nested_tf;
static int nested_active,nested_done,nested_status;

/* 用户镜像装载到新物理页前清零，保证镜像末尾的 BSS 区域为零。 */
static void zero(void *p,uint64 n){uint8 *b=p;while(n--)*b++=0;}
/* 复制内嵌 flat binary；这里允许源和目标重叠，便于复用通用复制逻辑。 */
static void copy(void *d,const void *s,uint64 n){uint8 *a=d;const uint8*b=s;if(a<b){while(n--)*a++=*b++;}else{a+=n;b+=n;while(n--)*--a=*--b;}}
/* 比较用户传入的程序名和内嵌程序表名字，忽略命令行换行符。 */
static int same(const char*a,const char*b){for(int i=0;i<64;i++){if(a[i]=='\0'||a[i]=='\n'||a[i]=='\r')return b[i]=='\0';if(a[i]!=b[i])return 0;}return 0;}
/* 按 {start,end,name} ABI 顺序扫描 Makefile 生成的内嵌程序表。 */
static struct uprog_entry *find(const char*n){struct uprog_entry*e=(void*)_uprog_table;while(e->start||e->end){if(same(n,e->name))return e;e=(void*)((e->end+7)&~7UL);}return 0;}

/*
 * LAB3：把一个内嵌 flat binary 安装到全新的用户地址空间。
 * 每个程序页单独 kalloc，按代码/只读数据/可写数据设置 PTE 权限；
 * 只有整个新地址空间准备成功后，调用者才应释放旧页表。
 */
/*
 * install 函数：将指定的用户程序（e）加载到一个全新的地址空间中，
 * 并初始化好对应的陷阱帧（tf），以便随后能通过 __switch 和 sret 切入用户态。
 */
static int install(struct uprog_entry*e,struct trapframe*tf){
  /*
   * install 的核心不变量：旧地址空间还没有被释放前，新地址空间必须
   * 已经完成所有用户页、栈页和高地址共享页的建立。中途失败统一释放
   * 新空间，调用者可以继续使用旧空间。
   * （这意味着这里分配的任何资源如果失败，都要严格回滚，不能影响当前正在运行的进程）
   */

  // 1. 检查参数合法性：e不能为空，结束地址不能小于起始地址，程序大小不能超过规定的最大值(IMAGE_MAX)
  if(!e||e->end<e->start||e->end-e->start>IMAGE_MAX)return -1;

  // 2. 计算所需大小：
  // sz 是程序实际的字节大小
  uint64 sz=e->end-e->start,
  // rounded 是将 sz 按 4KB (PGSIZE) 向上取整后的总大小（因为内存分配和映射必须是页对齐的）
         rounded=PGROUNDUP(sz),
  // 3. 提前为用户栈分配一个物理页（物理地址 spa）。栈也是一页 4KB。用户栈 VA：0x40000 ~ 0x40fff
         spa=(uint64)kalloc();

  // 4. 为用户程序创建一个全新的空用户页表（pt）。
  // 这一步通常也会在底层把 trampoline 和 trapframe 的高地址映射（共享页）建好。
  pagetable_t pt=vm_new_user();

  // 5. 如果页表或栈物理页分配失败，必须进行资源清理，防止内存泄漏。
  if(!pt||!spa){
    if(pt)vm_free_user(pt,0);      // 如果页表建好了但栈失败，释放页表及其内部所有映射
    if(spa)kfree((void*)spa);      // 如果栈分配了但页表失败，释放栈物理页
    return -1;                     // 返回失败状态
  }

  // 6. 遍历并加载程序的每一个虚拟页（从 VA 0 开始，到 rounded 结束）
  for(uint64 va=0;va<rounded;va+=PGSIZE){
    /* 每个用户虚拟页拥有独立物理页，避免不同程序页互相覆盖。 */
    // 6.1 为当前虚拟页 va 分配一个物理页 pa
    uint64 pa=(uint64)kalloc();
    
    // 6.2 如果分配失败，清理新页表中已经建立的部分映射，返回失败（调用者可以继续用旧空间）
    if(!pa){vm_free_user(pt,va);return -1;}
    
    // 6.3 将新分配的物理页清空（填 0），防止把内核旧数据泄露给用户
    zero((void*)pa,PGSIZE);
    
    // 6.4 计算这一页需要拷贝的字节数。
    // 默认拷贝一整页，但如果是最后一页（不足 4KB），则只拷贝剩余的部分。
    uint64 n=sz-va;if(n>PGSIZE)n=PGSIZE;
    
    // 6.5 将用户程序镜像（位于内核态的物理地址 e->start + va）拷贝到新分配的物理页中
    copy((void*)pa,(void*)(e->start+va),n);
    
    /* flat binary 没有 ELF 权限元数据，这里按实验约定推导权限。 */
    // 6.6 设置页表项（PTE）的权限：
    // 默认权限：用户态可访问 (PTE_U)，可读 (PTE_R)
    uint64 perm=PTE_U|PTE_R;
    
    // 如果是第 0 页（即代码段起始处），赋予可执行权限 (PTE_X)
    if(va==0)perm|=PTE_X;
    
    // 如果是从第 2 页（2 * 4KB = 8KB）开始，赋予可写权限 (PTE_W)（通常对应数据段/堆区）
    if(va>=2*PGSIZE)perm|=PTE_W;
    
    // 6.7 将虚拟地址 va 映射到物理地址 pa，设置对应的权限。
    // 如果映射失败，释放刚才分配的物理页 pa，清空已建好的页表，返回失败。
    if(vm_map(pt,va,PGSIZE,pa,perm)<0){kfree((void*)pa);vm_free_user(pt,va);return -1;}
  }

  /* 栈页单独分配；紧邻栈下方的页不建立 PTE，形成 guard page。 */
  // 7. 处理用户栈：
  // 7.1 将栈的物理页清零
  zero((void*)spa,PGSIZE);
  
  // 7.2 将固定的栈虚拟地址（USER_STACK_PAGE）映射到刚才分配的物理页 spa。
  // 权限设置为：用户态可访问 (PTE_U)、可读 (PTE_R)、可写 (PTE_W)。
  // 注意：这里的栈地址是紧贴着高地址 TRAPFRAME 下方的，栈更下方的页没有映射（Guard Page），防止栈溢出覆盖代码。
  if(vm_map(pt,USER_STACK_PAGE,PGSIZE,spa,PTE_U|PTE_R|PTE_W)<0){
    // 如果映射失败，清理页表（传入 rounded 表示连同刚才映射的代码段一起释放）及栈物理页
    vm_free_user(pt,rounded);kfree((void*)spa);return -1;
  }

  // 8. 安装完成，开始设置运行环境：
  // 8.1 将该进程的页表记录到当前进程的控制块中，切换到该地址空间
  vm_set_user_pagetable(pt);
  cur.pt=pt;              // 记录当前页表指针
  cur.base=rounded;       // 记录程序加载的基址/大小
  cur.sz=rounded;         // 记录程序大小
  
  // 8.2 初始化陷阱帧（trapframe），设置程序第一次运行的寄存器状态：
  tf->epc=0;              // 异常程序计数器（PC）：程序从虚拟地址 0 开始执行
  tf->sp=USER_STACK_TOP;  // 栈指针：指向用户栈的最高地址（栈是向下生长的）
  tf->ra=0;               // 返回地址清零
  tf->gp=0;               // 全局指针清零
  tf->tp=0;               // 线程指针清零
  
  // 9. 安装成功，返回 0
  return 0;
}

/* LAB3：创建第一个用户页表并装载 sh，填写第一次 sret 使用的 epc/sp。 */
void stage4_bootstrap(struct trapframe*tf){
  /* 第一个用户进程固定加载 sh；成功后打印一次完整用户页表供验收。 */
  if(install(find("sh"),tf)<0){console_puts_raw("cannot load sh\n");for(;;)asm volatile("wfi");}
  cur.pid=1;
  dump_pagetable(cur.pt);
}

int stage4_getpid(void){return cur.pid;}
/* LAB3：fork 为子进程创建新页表，并使用 vm_copy 复制独立物理页。 */
long stage4_fork(struct trapframe *tf)
{
    /*
     * 本实验用“保存父状态”的方式模拟真正的 fork：
     *   - 当前现场不切换，继续作为子进程运行；
     *   - 把父进程将来从 wait 返回后需要恢复的现场保存到 parent/parent_tf
     *     或 nested_parent/nested_tf 中。
     *
     * 注意：这里的“父”不是当前继续运行的执行流，而是被保存起来、
     * 将来 wait 后恢复的那个进程状态。
     */

    struct proc_state *save;      // 指向本次要保存的父进程状态结构
    struct trapframe *saved_tf;   // 指向父进程将来恢复时使用的 trapframe
    int *active, *done, *status;  // 分别指向子进程的活跃/完成/退出状态
    int childpid;                 // 本次要创建的子进程 pid

    /*
     * 第一次 fork：
     * 当前进程是 pid=1，并且还没有活跃子进程。
     * 此时创建 pid=2 的子进程，父进程状态保存到 parent。
     */
    if (cur.pid == 1 && !child_active) {
        save = &parent;           // 父进程状态保存到 parent
        saved_tf = &parent_tf;    // 父进程 trapframe 保存到 parent_tf
        active = &child_active;   // 指向子进程活跃标志
        done = &child_done;       // 指向子进程完成标志
        status = &child_status;   // 指向子进程退出状态
        childpid = 2;             // 新子进程 pid 为 2
    }
    /*
     * 第二次 fork：
     * 当前进程是 pid=2 的子进程，它已经活跃，
     * 并且还没有创建过嵌套子进程。
     * 此时创建 pid=3 的嵌套子进程，父状态保存到 nested_parent。
     */
    else if (cur.pid == 2 && child_active && !nested_active) {
        save = &nested_parent;    // 父进程状态保存到 nested_parent
        saved_tf = &nested_tf;    // 父进程 trapframe 保存到 nested_tf
        active = &nested_active;  // 指向嵌套子进程活跃标志
        done = &nested_done;      // 指向嵌套子进程完成标志
        status = &nested_status;  // 指向嵌套子进程退出状态
        childpid = 3;             // 新子进程 pid 为 3
    }
    else {
        /*
         * 当前状态不允许 fork：
         *   - pid 不是 1 或 2；
         *   - 或者对应子进程已经创建过；
         *   - 或者重复创建嵌套子进程。
         */
        return -1;
    }

    /*
     * 为“将来要恢复的父进程”新建一个用户页表。
     * 注意：这个页表不是给当前继续运行的子进程用的，
     * 而是保存到 save->pt，等父进程恢复时使用。
     */
    save->pt = vm_new_user();
    if (!save->pt)
        return -1;                // 新用户页表分配失败

    /*
     * 把当前进程 cur 的用户地址空间复制到 save->pt 中。
     *
     * 复制范围是 [0, PGROUNDUP(cur.sz))：
     *   - cur.sz 是当前用户空间大小；
     *   - PGROUNDUP 把它向上按页取整，保证最后一页也被复制；
     *   - vm_copy 的复制方向是：目标 save->pt <- 源 cur.pt。
     *
     * 这样父进程将来恢复时，拥有和 fork 时相同的用户地址空间快照。
     * 当前 cur.pt 没有被修改，仍留给当前执行流作为子进程继续使用。
     *
     * 如果 vm_copy 失败，它会释放目标空间；
     * 同时父地址空间 cur.pt 不会被修改。
     */
    if (vm_copy(save->pt, cur.pt, PGROUNDUP(cur.sz)) < 0) {
        save->pt = 0;             // 防止 save->pt 变成悬空指针
        return -1;
    }

    /*
     * 保存父进程的用户空间元数据和 pid。
     * 当前执行流随后会变成子进程，所以这里保存的是“父进程身份”。
     */
    save->base = cur.base;        // 保存父进程用户空间基址
    save->sz = cur.sz;            // 保存父进程用户空间大小
    save->pid = cur.pid;          // 保存父进程 pid

    /*
     * 复制当前 trapframe，作为父进程将来恢复时的现场。
     * 当前现场继续作为子进程运行，而 saved_tf 是父进程 wait 后恢复用的现场。
     */
    *saved_tf = *tf;

    /*
     * 模拟 fork 在父进程中的返回值：
     * 父进程将来恢复后，fork 应该返回子进程 pid。
     * 在 RISC-V 中 a0 通常保存系统调用返回值/参数，
     * 所以这里把保存的父现场中的 a0 设置为 childpid。
     */
    saved_tf->a0 = childpid;

    /*
     * 初始化新子进程的模拟状态：
     *   active=1 表示子进程已活跃；
     *   done=0   表示子进程尚未结束；
     *   status=0 表示退出状态清零。
     */
    *active = 1;
    *done = 0;
    *status = 0;

    /*
     * 关键一步：
     * 当前执行流改为子进程身份继续运行。
     * 原 cur.pt 保持不变，因此当前子进程继续使用原来的地址空间；
     * 父进程将来恢复时则使用 save->pt 中复制出来的地址空间。
     */
    cur.pid = childpid;

    /*
     * 返回 0 表示模拟 fork 成功。
     * 当前执行流作为子进程继续；父进程将来从 saved_tf 恢复后
     * 会看到 fork 返回 childpid。
     */
    return 0;
}
/* LAB3：简化 wait；子进程退出后把状态通过 copyout 写回用户地址。 */
long stage4_wait(struct trapframe*tf){
  /* 子进程退出时已经把状态写入全局槽；wait 再通过 copyout 返回给用户。 */
  if(cur.pid==1&&child_active&&child_done){if(tf->a0&&copyout(tf->a0,&child_status,sizeof(child_status))<0)return -1;child_active=0;return 2;}
  if(cur.pid==2&&nested_active&&nested_done){if(tf->a0&&copyout(tf->a0,&nested_status,sizeof(nested_status))<0)return -1;nested_active=0;return 3;}
  return -1;
}
/* LAB3：先安全复制程序名，再创建新地址空间；成功后才回收旧地址空间。 */
long stage4_exec(struct trapframe *tf)
{
    /*
     * 模拟 exec 系统调用：把当前进程的用户空间映像
     * 替换成指定用户程序（由 tf->a0 传入的程序名决定）的映像。
     *
     * 关键原则：
     *   - 参数来自用户空间，必须先安全拷贝到内核并验证；
     *   - 新映像安装成功之前，绝不释放旧映像；
     *   - install 失败时直接返回 -1，原进程保持可用。
     */

    char name[64];

    /*
     * 程序名来自用户空间（tf->a0 是用户虚拟地址）。
     * 用户指针不可信，必须用 copyinstr 做：
     *   - 地址合法性检查（是否落在用户可访问范围）；
     *   - 字符串长度限制（最多 sizeof(name) 字节，含结尾 '\0'）；
     *   - 逐字节拷入内核缓冲区 name。
     *
     * copyinstr 失败可能原因：用户地址非法、字符串过长、
     * 或没有以 '\0' 结尾。此时不能继续查表，直接返回失败。
     */
    if (copyinstr(name, tf->a0, sizeof(name)) < 0)
        return -1;

    /*
     * 在内核维护的用户程序表中按名字查找目标程序。
     * find 返回 NULL 表示程序不存在或名字不匹配，
     * 这时同样直接失败，不触碰当前进程的任何状态。
     */
    struct uprog_entry *e = find(name);
    if (!e)
        return -1;

    /*
     * 保存当前进程的旧用户页表和旧用户空间大小。
     *
     * 为什么要先保存？
     *   exec 的语义是“成功才替换，失败保留原进程”。
     *   如果 install 失败，我们必须让进程还能继续使用旧映像运行。
     *   因此旧页表不能提前释放，只能等 install 成功后再释放。
     */
    pagetable_t old = cur.pt;
    uint64 oldsz = cur.sz;

    /*
     * 安装新程序映像：
     *   - 根据 e 中的程序信息建立新的用户页表/映射；
     *   - 设置新程序的入口地址、栈等；
     *   - 修改 tf，使将来返回用户态时从新程序入口开始执行。
     *
     * install 内部失败时应自行清理它已经建立的部分，
     * 保证不会留下半成品；同时不能破坏 old 页表。
     *
     * 如果 install 失败，这里直接返回 -1：
     *   - cur.pt 仍然是 old；
     *   - cur.sz 仍然是 oldsz；
     *   - 原进程现场未被破坏，可以继续运行。
     */
    if (install(e, tf) < 0)
        return -1;

    /*
     * 走到这里说明新映像已经安装成功，当前进程已经切换到新程序。
     * 此时旧用户页表 old 不再被任何执行流使用，可以安全释放。
     *
     * 注意释放范围是 oldsz，即旧用户空间的大小，
     * 不能把新页表 cur.pt 的页误释放。
     */
    vm_free_user(old, oldsz);

    /*
     * 返回 0 表示 exec 成功。
     * 由于 tf 已被 install 改写，将来 trap 返回用户态时
     * 会从新程序的入口开始执行，旧程序映像不再存在。
     */
    return 0;
}
/* LAB3：sbrk 真实扩展/收缩用户页表；分配中途失败时撤销本次全部修改。 */
long stage4_sbrk(struct trapframe*tf){
  /* 当前课程统一采用 EAGER：边界增长时立即申请并建立每个新页。 */
  int n=(int)tf->a0;uint64 old=cur.sz;
  if(n<0){
    /* 收缩只解除完全离开新边界的页，保留仍被部分使用的最后一页。 */
    uint64 nn=(uint64)(-n);if(nn>old||old-nn<PGSIZE)return -1;
    vm_unmap(cur.pt,PGROUNDUP(old-nn),(PGROUNDUP(old)-PGROUNDUP(old-nn))/PGSIZE,1);
    cur.sz=old-nn;return old;
  }
  uint64 ns=old+(uint64)n;
  /* 堆不能越过栈下方的 guard page。 */
  if(ns<old||ns>USER_STACK_PAGE-PGSIZE)return -1;
  for(uint64 va=PGROUNDUP(old);va<PGROUNDUP(ns);va+=PGSIZE){
    uint64 pa=(uint64)kalloc();
    if(!pa||vm_map(cur.pt,va,PGSIZE,pa,PTE_U|PTE_R|PTE_W)<0){
      /* 既释放当前未映射页，也撤销本次已经成功的扩展。 */
      if(pa)kfree((void*)pa);
      vm_unmap(cur.pt,PGROUNDUP(old),(va-PGROUNDUP(old))/PGSIZE,1);return -1;
    }
  }
  cur.sz=ns;return old;
}
/* LAB3：释放子地址空间并恢复父页表、父 trapframe，直接返回父用户现场。 */
void stage4_exit(struct trapframe*tf,int status){
  /* exit 不返回用户态；先回收当前子空间，再切回父进程页表和现场。 */
  (void)tf;
  if(cur.pid==3&&nested_active){nested_status=status;vm_free_user(cur.pt,cur.sz);cur=nested_parent;vm_set_user_pagetable(cur.pt);stage1_tf=nested_tf;nested_done=1;usertrapret();}
  if(cur.pid==2&&child_active){child_status=status;vm_free_user(cur.pt,cur.sz);cur=parent;vm_set_user_pagetable(cur.pt);stage1_tf=parent_tf;child_done=1;usertrapret();}
  for(;;)asm volatile("wfi");
}
void stage4_fault(int cause){
  /* 用户非法访问不能进入内核死循环；有父进程时按异常状态退出子进程。 */
  if(cur.pid==3&&nested_active)stage4_exit(&stage1_tf,cause);
  if(cur.pid==2&&child_active)stage4_exit(&stage1_tf,cause);
  for(;;)asm volatile("wfi");
}
pagetable_t stage4_current_pagetable(void){return cur.pt;}
