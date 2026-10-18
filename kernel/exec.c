#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "trap.h"
#include "console.h"
#include "exec.h"
#include "vm.h"

extern char _uprog_table[];
#define IMAGE_MAX (128 * 1024UL)
#define USER_STACK_PAGE 0x40000UL
#define USER_STACK_TOP  (USER_STACK_PAGE + PGSIZE)
struct uprog_entry { uint64 start,end; char name[1]; };
struct proc_state { pagetable_t pt; uint64 base, sz; int pid; };
static struct proc_state cur,parent;
static int child_active,child_done;
static int child_status;
static struct trapframe parent_tf;
static struct proc_state nested_parent;
static struct trapframe nested_tf;
static int nested_active,nested_done,nested_status;

static void zero(void *p,uint64 n){uint8 *b=p;while(n--)*b++=0;}
static void copy(void *d,const void *s,uint64 n){uint8 *a=d;const uint8*b=s;if(a<b){while(n--)*a++=*b++;}else{a+=n;b+=n;while(n--)*--a=*--b;}}
static int same(const char*a,const char*b){for(int i=0;i<64;i++){if(a[i]=='\0'||a[i]=='\n'||a[i]=='\r')return b[i]=='\0';if(a[i]!=b[i])return 0;}return 0;}
static struct uprog_entry *find(const char*n){struct uprog_entry*e=(void*)_uprog_table;while(e->start||e->end){if(same(n,e->name))return e;e=(void*)((e->end+7)&~7UL);}return 0;}

static int install(struct uprog_entry*e,struct trapframe*tf){
  if(!e||e->end<e->start||e->end-e->start>IMAGE_MAX)return -1;
  uint64 sz=e->end-e->start,rounded=PGROUNDUP(sz),spa=(uint64)kalloc();pagetable_t pt=vm_new_user();
  if(!pt||!spa){if(pt)vm_free_user(pt,0);if(spa)kfree((void*)spa);return -1;}
  for(uint64 va=0;va<rounded;va+=PGSIZE){uint64 pa=(uint64)kalloc();if(!pa){vm_free_user(pt,va);return -1;}zero((void*)pa,PGSIZE);uint64 n=sz-va;if(n>PGSIZE)n=PGSIZE;copy((void*)pa,(void*)(e->start+va),n);uint64 perm=PTE_U|PTE_R;if(va==0)perm|=PTE_X;if(va>=2*PGSIZE)perm|=PTE_W;if(vm_map(pt,va,PGSIZE,pa,perm)<0){kfree((void*)pa);vm_free_user(pt,va);return -1;}}
  zero((void*)spa,PGSIZE);if(vm_map(pt,USER_STACK_PAGE,PGSIZE,spa,PTE_U|PTE_R|PTE_W)<0){vm_free_user(pt,rounded);kfree((void*)spa);return -1;}
  vm_set_user_pagetable(pt);cur.pt=pt;cur.base=rounded;cur.sz=rounded;tf->epc=0;tf->sp=USER_STACK_TOP;tf->ra=0;tf->gp=0;tf->tp=0;return 0;
}

void stage4_bootstrap(struct trapframe*tf){if(install(find("sh"),tf)<0){console_puts_raw("cannot load sh\n");for(;;)asm volatile("wfi");}cur.pid=1;dump_pagetable(cur.pt);}
int stage4_getpid(void){return cur.pid;}
long stage4_fork(struct trapframe*tf){
  struct proc_state *save; struct trapframe *saved_tf; int *active,*done,*status,childpid;
  if(cur.pid==1&&!child_active){save=&parent;saved_tf=&parent_tf;active=&child_active;done=&child_done;status=&child_status;childpid=2;}
  else if(cur.pid==2&&child_active&&!nested_active){save=&nested_parent;saved_tf=&nested_tf;active=&nested_active;done=&nested_done;status=&nested_status;childpid=3;}
  else return -1;
  save->pt=vm_new_user();
  if(!save->pt) return -1;
  /* vm_copy consumes and rolls back dst on failure. */
  if(vm_copy(save->pt,cur.pt,PGROUNDUP(cur.sz))<0){save->pt=0;return -1;}
  save->base=cur.base;save->sz=cur.sz;save->pid=cur.pid;*saved_tf=*tf;saved_tf->a0=childpid;*active=1;*done=0;*status=0;cur.pid=childpid;return 0;
}
long stage4_wait(struct trapframe*tf){
  if(cur.pid==1&&child_active&&child_done){if(tf->a0&&copyout(tf->a0,&child_status,sizeof(child_status))<0)return -1;child_active=0;return 2;}
  if(cur.pid==2&&nested_active&&nested_done){if(tf->a0&&copyout(tf->a0,&nested_status,sizeof(nested_status))<0)return -1;nested_active=0;return 3;}
  return -1;
}
long stage4_exec(struct trapframe*tf){char name[64];if(copyinstr(name,tf->a0,sizeof(name))<0)return -1;struct uprog_entry*e=find(name);if(!e)return -1;pagetable_t old=cur.pt;uint64 oldsz=cur.sz;if(install(e,tf)<0)return -1;vm_free_user(old,oldsz);return 0;}
long stage4_sbrk(struct trapframe*tf){int n=(int)tf->a0;uint64 old=cur.sz;if(n<0){uint64 nn=(uint64)(-n);if(nn>old||old-nn<PGSIZE)return -1;vm_unmap(cur.pt,PGROUNDUP(old-nn),(PGROUNDUP(old)-PGROUNDUP(old-nn))/PGSIZE,1);cur.sz=old-nn;return old;}uint64 ns=old+(uint64)n;if(ns<old||ns>USER_STACK_PAGE-PGSIZE)return -1;for(uint64 va=PGROUNDUP(old);va<PGROUNDUP(ns);va+=PGSIZE){uint64 pa=(uint64)kalloc();if(!pa||vm_map(cur.pt,va,PGSIZE,pa,PTE_U|PTE_R|PTE_W)<0){if(pa)kfree((void*)pa);vm_unmap(cur.pt,PGROUNDUP(old),(va-PGROUNDUP(old))/PGSIZE,1);return -1;}}cur.sz=ns;return old;}
void stage4_exit(struct trapframe*tf,int status){
  (void)tf;
  if(cur.pid==3&&nested_active){nested_status=status;vm_free_user(cur.pt,cur.sz);cur=nested_parent;vm_set_user_pagetable(cur.pt);stage1_tf=nested_tf;nested_done=1;usertrapret();}
  if(cur.pid==2&&child_active){child_status=status;vm_free_user(cur.pt,cur.sz);cur=parent;vm_set_user_pagetable(cur.pt);stage1_tf=parent_tf;child_done=1;usertrapret();}
  for(;;)asm volatile("wfi");
}
void stage4_fault(int cause){if(cur.pid==3&&nested_active)stage4_exit(&stage1_tf,cause);if(cur.pid==2&&child_active)stage4_exit(&stage1_tf,cause);for(;;)asm volatile("wfi");}
pagetable_t stage4_current_pagetable(void){return cur.pt;}
