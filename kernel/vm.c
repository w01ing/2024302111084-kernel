/*
 * LAB3：虚拟内存和物理页管理实现。
 *
 * Lab2 使用 satp=0 的裸地址模式，用户地址基本等同于物理地址；
 * Lab3 改为 Sv39 三级页表后，所有用户代码、数据、栈和页表页都必须
 * 从物理页分配器取得，再通过 PTE 建立 VA -> PA 的映射。
 */
#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "course_sid.h"
#include "trap.h"
#include "vm.h"

extern char kernel_end[];
extern void printf(const char *, ...);

#define PTE_A (1L << 6)
#define PTE_D (1L << 7)
#define KERNEL_MAP_2M (1UL << 21)

/* LAB3：空闲物理页链表节点。节点本身放在空闲页的起始位置。 */
struct run { struct run *next; };
/* freelist 指向第一个空闲页；free_pages 与链表中的节点数量保持一致。 */
static struct run *freelist;
static uint64 alloc_lo, alloc_hi;
static uint64 free_pages;
/* 内核根页表放在静态数组中，避免建立内核页表时依赖自身的映射。 */
static uint64 kernel_pt_store[512] __attribute__((aligned(PGSIZE)));
/* copyin/copyout 使用的当前用户页表；单进程简化运行时只需要一个。 */
static pagetable_t active_user_pt;

/* LAB3：新分配页必须清零，避免新用户页看到旧数据。 */
static void zero_page(void *p) {
  uint64 *w = p;
  for (int i = 0; i < PGSIZE / 8; i++) w[i] = 0;
}

/* LAB3：释放页填充固定字节，帮助发现释放后继续使用等错误。 */
static void junk_page(void *p) {
  uint8 *b = p;
  for (uint64 i = 0; i < PGSIZE; i++) b[i] = 0x01;
}

/* LAB3：将一个已经验证过的物理页压入空闲链表。 */
static void free_push(uint64 p) {
  struct run *r = (struct run *)p;
  r->next = freelist;
  freelist = r;
  free_pages++;
}

/*
 * LAB3：初始化物理页分配器。
 * 可用区间从内核镜像结束处开始，到按课程要求扣除保留页后的 PHYSTOP
 * 结束。LAB3_ALLOC_ORDER 只改变分配顺序，不改变可用物理页集合。
 */
void kinit(void) {
  uint64 p, npages, i;
  //计算可分配物理页的范围
  alloc_lo = PGROUNDUP((uint64)kernel_end);//kernel_end 是链接脚本导出的符号，表示内核镜像结束位置。
  alloc_hi = PHYSTOP - (uint64)LAB3_MEMCAP_DELTA * PGSIZE;//PHYSTOP 是 QEMU RAM 的结束位置：
  freelist = 0;
  free_pages = 0;
  if (alloc_hi <= alloc_lo) return;
  npages = (alloc_hi - alloc_lo) / PGSIZE;

  /*
   * 链表采用头插法，因此初始化时要反向遍历，才能得到配置要求的
   * 首次分配顺序。每个页先填充 junk，后续 kalloc 会清零它。
   */
  if (LAB3_ALLOC_ORDER == 0) {//升序分配
    /* Prepending high-to-low makes the lowest page the first allocation. */
    for (p = alloc_hi; p != alloc_lo;) {
      p -= PGSIZE;
      junk_page((void *)p);
      free_push(p);
    }
  } else if (LAB3_ALLOC_ORDER == 2) {//从中点附近开始交替分配
    /* midpoint, midpoint-1, midpoint+1, midpoint-2, ... */
    uint64 center = (npages - 1) / 2;
    for (i = npages; i != 0; i--) {
      uint64 k = i - 1;
      uint64 index = (k & 1) ? center + (k + 1) / 2 : center - k / 2;
      p = alloc_lo + index * PGSIZE;
      junk_page((void *)p);
      free_push(p);
    }
  } else {//降序分配
    /* Prepending low-to-high makes the highest page the first allocation. */
    for (p = alloc_lo; p < alloc_hi; p += PGSIZE) {
      junk_page((void *)p);
      free_push(p);
    }
  }//kinit把所有可用物理页串成一个单向空闲链表
}

/* LAB3：取出一个空闲页并清零；失败时返回 0。 */
void *kalloc(void) {
  /* 取出一个空闲页；清零动作既初始化新页，也隔离上一个拥有者的数据。 */
  struct run *r = freelist;
  if (r) { freelist = r->next; free_pages--; zero_page(r); }
  return r;
}

/* LAB3：释放页并重新加入空闲链表；非法、未对齐地址直接忽略。 */
void kfree(void *pa) {
  uint64 p = (uint64)pa;
  if (p < alloc_lo || p >= alloc_hi || (p & (PGSIZE - 1))) return;
  /* 先填充垃圾数据，再重新挂入链表，便于暴露释放后继续使用。 */
  junk_page(pa);
  free_push(p);
}

uint64 vm_free_count(void) { return free_pages; }

/*
 * LAB3：沿 Sv39 的 L2 -> L1 -> L0 三级索引查找最终 PTE。
 * 中间项没有 R/W/X 时表示页表指针；alloc 非零时，缺失的中间页表
 * 由 kalloc() 创建。最终返回的是 PTE 的地址，而不是物理页地址。
 */
pte_t *walk(pagetable_t pt, uint64 va, int alloc) {
  /* Sv39 只接受低 39 位形成的有效虚拟地址。 */
  if (va >= MAXVA) return 0;
  for (int level = 2; level > 0; level--) {
    /* 每次循环向下走一级：L2 -> L1 -> L0。 */
    pte_t *p = &pt[PX(level, va)];
    if (*p & PTE_V) {
      if (PTE_FLAGS(*p) & (PTE_R | PTE_W | PTE_X)) return 0;
      pt = (pagetable_t)PTE2PA(*p);
    } else {
      if (!alloc) return 0;
      /* 缺失的中间页表由物理页分配器提供，并作为非叶子 PTE 挂入。 */
      pagetable_t child = kalloc();
      if (!child) return 0;
      *p = PA2PTE(child) | PTE_V;
      pt = child;
    }
  }
  return &pt[PX(0, va)];
}

/* LAB3：建立一个 4 KiB 叶子映射，不负责释放调用者拥有的物理页。 */
static int map_one(pagetable_t pt, uint64 va, uint64 pa, uint64 perm) {
  /* map_one 只填写叶子 PTE，不拥有 pa，因此失败时不能替调用者释放 pa。 */
  pte_t *p = walk(pt, va, 1);
  if (!p || (*p & PTE_V)) return -1;
  *p = PA2PTE(pa) | perm | PTE_V | PTE_A;
  if (perm & PTE_W) *p |= PTE_D;
  return 0;
}

/* LAB3：判断中间页表是否已经没有有效子项。 */
static int table_empty(pagetable_t pt) {
  for (int i = 0; i < 512; i++)
    if (pt[i] & PTE_V) return 0;
  return 1;
}

/* LAB3：解除映射后向上回收已经空的中间页表。 */
static void prune_path(pagetable_t pt, uint64 va, int level) {
  /* 递归返回时检查子表是否为空；只有空表才能安全回收。 */
  pte_t *p = &pt[PX(level, va)];
  if (!(*p & PTE_V) || (PTE_FLAGS(*p) & (PTE_R | PTE_W | PTE_X))) return;
  pagetable_t child = (pagetable_t)PTE2PA(*p);
  if (level > 0) prune_path(child, va, level - 1);
  if (level > 0 && table_empty(child)) {
    *p = 0;
    kfree(child);
  }
}

/*
 * LAB3：批量建立连续虚拟区间的映射。
 * 先预检查重叠，再逐页建立映射；中途分配中间页表失败时，清除本次
 * 已写入的 PTE 并回收空中间页表，保证失败不会留下半成品页表。
 */
int mappages(pagetable_t pt, uint64 va, uint64 size, uint64 pa, uint64 perm) {
  if (size == 0 || va + size < va || va + size > MAXVA) return -1;
  uint64 a = PGROUNDDOWN(va), last = PGROUNDDOWN(va + size - 1);

  /*
   * 先预检查重叠。这样失败回滚时只会删除本次调用新建的叶子映射，
   * 不会误删调用前已经存在的映射。
   */
  for (uint64 check = a;; check += PGSIZE) {
    pte_t *p = walk(pt, check, 0);
    if (p && (*p & PTE_V) && (PTE_FLAGS(*p) & (PTE_R | PTE_W | PTE_X)))
      return -1;
    if (check == last) break;
  }

  for (;;) {
    if (map_one(pt, a, pa + (a - PGROUNDDOWN(va)), perm) < 0) {
      /* 叶子物理页仍归调用者所有；这里只回滚 PTE 和空的中间页表。 */
      for (uint64 rollback = PGROUNDDOWN(va); rollback < a; rollback += PGSIZE) {
        pte_t *p = walk(pt, rollback, 0);
        if (p) *p = 0;
        prune_path(pt, rollback, 2);
      }
      prune_path(pt, a, 2);
      return -1;
    }
    if (a == last) break;
    a += PGSIZE;
  }
  return 0;
}

/* LAB3：对外提供的映射接口，统一转到带回滚保证的 mappages。 */
int vm_map(pagetable_t pt, uint64 va, uint64 size, uint64 pa, uint64 perm) {
  int r = mappages(pt, va, size, pa, perm);
  if (r == 0)
    sfence_vma();
  return r;
}

/*
 * LAB3：解除一段叶子映射。do_free 为真时同时释放叶子物理页；
 * trampoline/trapframe 等共享页只解除当前页表中的映射，不释放物理页。
 */
int vm_unmap(pagetable_t pt, uint64 va, uint64 npages, int do_free) {
  int changed = 0;
  for (uint64 i = 0; i < npages; i++) {
    pte_t *p = walk(pt, va + i * PGSIZE, 0);
    if (!p || !(*p & PTE_V) || !(PTE_FLAGS(*p) & (PTE_R|PTE_W|PTE_X))) continue;
    uint64 pa = PTE2PA(*p); *p = 0; if (do_free) kfree((void *)pa);
    prune_path(pt, va + i * PGSIZE, 2);
    changed = 1;
  }
  if (changed)
    sfence_vma();
  return 0;
}

/*
 * LAB3：构造内核页表。
 * RAM 使用 2 MiB 叶子映射；UART、PLIC、trampoline 和 trapframe 使用
 * 4 KiB 映射。内核页表必须覆盖内核运行代码和所有内核直接访问的 MMIO。
 */
static int map_kernel(void) {
  /*
   * 内核 RAM 使用 2 MiB L1 叶子映射，减少内核页表页数量；设备和高地址
   * 共享页则通过 vm_map 使用普通 4 KiB 叶子映射。
   */
  for (uint64 pa = KERNBASE; pa < PHYSTOP; pa += KERNEL_MAP_2M) {//KERNBASE=0x80000000L 
    uint64 va = pa;//直接映射：虚拟地址 VA = 物理地址 PA
    uint64 l2 = PX(2, va), l1 = PX(1, va);//取 VPN[2]，作为 L2 索引；取 VPN[1]，作为 L1 索引
    if (!(kernel_pt_store[l2] & PTE_V)) {//如果L2项还没有建立
      pagetable_t p = kalloc();
      if (!p)
        return -1;
      kernel_pt_store[l2] = PA2PTE(p) | PTE_V;//PA2PTE(p)：把 L1 页表物理地址转换成 PTE 中的 PPN；
    }
    pagetable_t l1pt = (pagetable_t)PTE2PA(kernel_pt_store[l2]);
    l1pt[l1] = PA2PTE(pa) | PTE_R | PTE_W | PTE_X | PTE_V | PTE_A | PTE_D;
  }
  if (vm_map(kernel_pt_store, UART0, PGSIZE, UART0, PTE_R|PTE_W) < 0)
    return -1;
  for (uint64 pa = PLIC; pa < PLIC + 0x204000; pa += PGSIZE)
    if (vm_map(kernel_pt_store, pa, PGSIZE, pa, PTE_R|PTE_W) < 0)
      return -1;
  extern char trampoline[];
  if (vm_map(kernel_pt_store, TRAMPOLINE, PGSIZE,
            PGROUNDDOWN((uint64)trampoline), PTE_R|PTE_X) < 0)
    return -1;
  if (vm_map(kernel_pt_store, TRAPFRAME, PGSIZE,
            PGROUNDDOWN((uint64)&stage1_tf), PTE_R|PTE_W) < 0)
    return -1;
  return 0;
}

/* LAB3：返回当前内核页表根，供 satp 和用户 trap 返回路径使用。 */
pagetable_t kernel_pagetable(void) { return kernel_pt_store; }
pagetable_t user_pagetable(void) { return active_user_pt; }
void vm_set_user_pagetable(pagetable_t pt) { active_user_pt = pt; }

/*
 * LAB3：新建用户页表根，并预先映射两个公共高地址页面：
 * TRAMPOLINE 供用户 trap 入口/返回使用，TRAPFRAME 供 trampoline 保存现场。
 * 用户代码、数据和栈由 exec/install 或 sbrk 后续加入。
 */
pagetable_t vm_new_user(void) {
  /* 用户页表根本身也是一个物理页，先申请根，再建立两个高地址共享页。 */
  pagetable_t pt = kalloc();
  if (!pt) return 0;
  extern char trampoline[];
  if (vm_map(pt, TRAMPOLINE, PGSIZE, PGROUNDDOWN((uint64)trampoline), PTE_R|PTE_X) < 0 ||
      vm_map(pt, TRAPFRAME, PGSIZE, PGROUNDDOWN((uint64)&stage1_tf), PTE_R|PTE_W) < 0) {
    /* vm_free_user also releases intermediate tables allocated by vm_map. */
    vm_free_user(pt, 0);
    return 0;
  }
  return pt;
}

/* LAB3：销毁用户页表、用户叶子页以及所有空的中间页表。 */
void vm_free_user(pagetable_t pt, uint64 sz) {
  if (!pt) return;
  /* 先释放低地址用户页，再释放固定栈页，最后解除两个共享高地址页。 */
  vm_unmap(pt, 0, PGROUNDUP(sz)/PGSIZE, 1);
  vm_unmap(pt, 0x40000, 1, 1);
  vm_unmap(pt, TRAMPOLINE, 1, 0); vm_unmap(pt, TRAPFRAME, 1, 0);
  kfree(pt);
}

/*
 * LAB3：复制用户地址空间。
 * fork 不共享用户物理页，而是为每个源页分配独立目标页并复制内容；
 * 任意一步失败都释放目标地址空间，保证父地址空间不被破坏。
 */
int vm_copy(pagetable_t dst, pagetable_t src, uint64 sz) {
  /*
   * fork 使用真正的深拷贝：子页表拥有自己的物理页，因此父子进程的
   * 后续写入互不影响。目标空间的中间页和叶子页由失败路径统一清理。
   */
  for (uint64 va = 0; va < PGROUNDUP(sz); va += PGSIZE) {
    pte_t *sp = walk(src, va, 0);
    if (!sp || !(*sp & PTE_V)) {
      vm_free_user(dst, va);
      return -1;
    }
    uint64 pa = (uint64)kalloc();
    if (!pa) {
      vm_free_user(dst, va);
      return -1;
    }
    uint8 *d = (uint8 *)pa, *s = (uint8 *)PTE2PA(*sp);
    for (int i=0;i<PGSIZE;i++) d[i]=s[i];
    if (map_one(dst, va, pa, PTE_FLAGS(*sp) & (PTE_R|PTE_W|PTE_X|PTE_U)) < 0) {
      kfree((void*)pa);
      vm_free_user(dst, va);
      return -1;
    }
  }
  pte_t *stack = walk(src, 0x40000, 0);
  if (stack && (*stack & PTE_V)) {
    uint64 pa = (uint64)kalloc();
    if (!pa) {
      vm_free_user(dst, sz);
      return -1;
    }
    uint8 *d=(uint8 *)pa, *s=(uint8 *)PTE2PA(*stack);
    for (int i=0;i<PGSIZE;i++) d[i]=s[i];
    if (map_one(dst, 0x40000, pa, PTE_FLAGS(*stack)&(PTE_R|PTE_W|PTE_X|PTE_U))<0) {
      kfree((void *)pa);
      vm_free_user(dst, sz);
      return -1;
    }
  }
  return 0;
}

/*
 * LAB3：把用户虚拟地址转换成可访问的物理地址，并检查 PTE_U 及读写权限。
 * copyin/copyout 只能通过这个函数访问用户指针，不能直接解引用用户 VA。
 */
uint64 walkaddr(pagetable_t pt, uint64 va, int write) {
  pte_t *p = walk(pt, va, 0);
  if (!p || !(*p & PTE_V) || !(*p & PTE_U) ||
      (!write && !(*p & PTE_R)) || (write && !(*p & PTE_W)))
    return 0;
  /* PTE2PA 得到页首物理地址，再加上用户 VA 的页内偏移。 */
  return PTE2PA(*p) + (va & (PGSIZE-1));
}

/* LAB3：按页边界把用户地址空间数据复制到内核缓冲区。 */
int copyin(void *dst, uint64 srcva, uint64 len) {
  /* 每次只处理当前页剩余部分，跨页后重新检查下一页的 PTE 权限。 */
  uint8 *d=dst; while (len) { uint64 pa=walkaddr(active_user_pt,srcva,0); if(!pa)return -1; uint64 n=PGSIZE-(srcva&(PGSIZE-1)); if(n>len)n=len; for(uint64 i=0;i<n;i++)d[i]=((uint8*)pa)[i]; d+=n;srcva+=n;len-=n; } return 0;
}
/* LAB3：按页边界把内核数据复制到用户地址空间。 */
int copyout(uint64 dstva, const void *src, uint64 len) {
  /* 与 copyin 对称，但目标页必须具有用户可写权限。 */
  const uint8 *s=src; while(len){uint64 pa=walkaddr(active_user_pt,dstva,1);if(!pa)return -1;uint64 n=PGSIZE-(dstva&(PGSIZE-1));if(n>len)n=len;for(uint64 i=0;i<n;i++)((uint8*)pa)[i]=s[i];s+=n;dstva+=n;len-=n;}return 0;
}
/* LAB3：安全复制用户字符串，遇到 NUL 成功，越界或超长则失败。 */
int copyinstr(char *dst, uint64 srcva, uint64 max) {
  /* 逐字节复制可以正确处理字符串跨页，并在遇到非法页时立即失败。 */
  for(uint64 i=0;i<max;i++){if(copyin(&dst[i],srcva+i,1)<0)return -1;if(dst[i]=='\0')return 0;}return -1;
}

/* LAB3：递归打印页表层级、叶子 VA/PA 和 R/W/X/U 权限。 */
static void dump_level(pagetable_t pt, int level, uint64 base) {
  /* 非叶子项递归展开，叶子项输出其覆盖区间起始 VA 和目标 PA。 */
  for (int i=0;i<512;i++) if (pt[i]&PTE_V) { uint64 va=base+((uint64)i<<PXSHIFT(level)),pa=PTE2PA(pt[i]); uint64 f=PTE_FLAGS(pt[i]); if(f&(PTE_R|PTE_W|PTE_X)){printf("LEAF va=0x%lx pa=0x%lx perm=%c%c%c%c\n",va,pa,(f&PTE_R)?'r':'-',(f&PTE_W)?'w':'-',(f&PTE_X)?'x':'-',(f&PTE_U)?'u':'-');} else {printf("L%d va=0x%lx pa=0x%lx perm=----\n",level,va,pa);dump_level((pagetable_t)pa,level-1,va);} }
}
/* LAB3：按实验规定的格式输出完整页表，用于验收映射和权限。 */
void dump_pagetable(pagetable_t pt){printf("DUMP-PAGETABLE begin\n");dump_level(pt,2,0);printf("DUMP-PAGETABLE end\n");}

/*
 * LAB3：开启分页的总入口。先准备分配器和内核页表，再写 satp；
 * satp 改变后立即 sfence.vma，避免 CPU 继续使用旧的 TLB 翻译。
 */
void vm_init(void) {
  /* 开启分页前，所有内核仍通过物理地址直接访问这些初始化结构。 */
  //清零
  zero_page(kernel_pt_store); kinit();
  if (map_kernel() < 0)
    for (;;) asm volatile("wfi");
  w_satp(MAKE_SATP(kernel_pt_store)); sfence_vma();
}
