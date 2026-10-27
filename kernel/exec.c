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
static int install(struct uprog_entry*e,struct trapframe*tf){
  /*
   * install 的核心不变量：旧地址空间还没有被释放前，新地址空间必须
   * 已经完成所有用户页、栈页和高地址共享页的建立。中途失败统一释放
   * 新空间，调用者可以继续使用旧空间。
   */
  if(!e||e->end<e->start||e->end-e->start>IMAGE_MAX)return -1;
  uint64 sz=e->end-e->start,rounded=PGROUNDUP(sz),spa=(uint64)kalloc();pagetable_t pt=vm_new_user();
  if(!pt||!spa){if(pt)vm_free_user(pt,0);if(spa)kfree((void*)spa);return -1;}
  for(uint64 va=0;va<rounded;va+=PGSIZE){
    /* 每个用户虚拟页拥有独立物理页，避免不同程序页互相覆盖。 */
    uint64 pa=(uint64)kalloc();
    if(!pa){vm_free_user(pt,va);return -1;}
    zero((void*)pa,PGSIZE);
    uint64 n=sz-va;if(n>PGSIZE)n=PGSIZE;
    copy((void*)pa,(void*)(e->start+va),n);
    /* flat binary 没有 ELF 权限元数据，这里按实验约定推导权限。 */
    uint64 perm=PTE_U|PTE_R;if(va==0)perm|=PTE_X;if(va>=2*PGSIZE)perm|=PTE_W;
    if(vm_map(pt,va,PGSIZE,pa,perm)<0){kfree((void*)pa);vm_free_user(pt,va);return -1;}
  }
  /* 栈页单独分配；紧邻栈下方的页不建立 PTE，形成 guard page。 */
  zero((void*)spa,PGSIZE);if(vm_map(pt,USER_STACK_PAGE,PGSIZE,spa,PTE_U|PTE_R|PTE_W)<0){vm_free_user(pt,rounded);kfree((void*)spa);return -1;}
  vm_set_user_pagetable(pt);cur.pt=pt;cur.base=rounded;cur.sz=rounded;tf->epc=0;tf->sp=USER_STACK_TOP;tf->ra=0;tf->gp=0;tf->tp=0;return 0;
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
long stage4_fork(struct trapframe*tf){
  /*
   * 本实验用保存的父状态模拟真正 fork：当前现场继续作为子进程运行，
   * parent_tf 保存父进程将来 wait 后恢复的现场。
   */
  struct proc_state *save; struct trapframe *saved_tf; int *active,*done,*status,childpid;
  if(cur.pid==1&&!child_active){save=&parent;saved_tf=&parent_tf;active=&child_active;done=&child_done;status=&child_status;childpid=2;}
  else if(cur.pid==2&&child_active&&!nested_active){save=&nested_parent;saved_tf=&nested_tf;active=&nested_active;done=&nested_done;status=&nested_status;childpid=3;}
  else return -1;
  save->pt=vm_new_user();
  if(!save->pt) return -1;
  /* vm_copy 失败时会释放目标空间，父地址空间不会被修改。 */
  if(vm_copy(save->pt,cur.pt,PGROUNDUP(cur.sz))<0){save->pt=0;return -1;}
  save->base=cur.base;save->sz=cur.sz;save->pid=cur.pid;*saved_tf=*tf;saved_tf->a0=childpid;*active=1;*done=0;*status=0;cur.pid=childpid;return 0;
}
/* LAB3：简化 wait；子进程退出后把状态通过 copyout 写回用户地址。 */
long stage4_wait(struct trapframe*tf){
  /* 子进程退出时已经把状态写入全局槽；wait 再通过 copyout 返回给用户。 */
  if(cur.pid==1&&child_active&&child_done){if(tf->a0&&copyout(tf->a0,&child_status,sizeof(child_status))<0)return -1;child_active=0;return 2;}
  if(cur.pid==2&&nested_active&&nested_done){if(tf->a0&&copyout(tf->a0,&nested_status,sizeof(nested_status))<0)return -1;nested_active=0;return 3;}
  return -1;
}
/* LAB3：先安全复制程序名，再创建新地址空间；成功后才回收旧地址空间。 */
long stage4_exec(struct trapframe*tf){
  /* 程序名来自用户空间，必须先通过 copyinstr 验证后再查表。 */
  char name[64];
  if(copyinstr(name,tf->a0,sizeof(name))<0)return -1;
  struct uprog_entry*e=find(name);if(!e)return -1;
  /* 只有 install 成功后才释放 old，保证 exec 失败不会破坏原进程。 */
  pagetable_t old=cur.pt;uint64 oldsz=cur.sz;
  if(install(e,tf)<0)return -1;
  vm_free_user(old,oldsz);return 0;
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
