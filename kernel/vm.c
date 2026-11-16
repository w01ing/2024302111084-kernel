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
#include "proc.h"
#include "spinlock.h"
#include "trap.h"
#include "vm.h"

extern char kernel_end[];
extern void printf(const char *, ...);

#define PTE_A (1L << 6)
#define PTE_D (1L << 7)
#define KERNEL_MAP_2M (1UL << 21)
#define TOTAL_PAGES ((PHYSTOP - KERNBASE) / PGSIZE)

/* LAB3：空闲物理页链表节点。节点本身放在空闲页的起始位置。 */
struct run { struct run *next; };
/* LAB5：空闲链表和全局引用计数由同一把锁保护。 */
static struct {
  struct spinlock lock;
  struct run *freelist;
  uint16 refcnt[TOTAL_PAGES];
  uint64 free_pages;
  uint64 shared_pages;
  uint64 cow_copies;
} kmem;
static struct spinlock cow_lock;
static uint64 alloc_lo, alloc_hi;
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
  for (uint64 i = 0; i < PGSIZE; i++) b[i] = 0x5a;
}

static uint64
page_index(uint64 pa)
{
  return (pa - KERNBASE) / PGSIZE;
}

static int
valid_alloc_page(uint64 pa)
{
  return pa >= alloc_lo && pa < alloc_hi &&
         (pa & (PGSIZE - 1)) == 0;
}

/* 调用者必须持有 kmem.lock。 */
static void free_push_locked(uint64 p) {
  struct run *r = (struct run *)p;
  r->next = kmem.freelist;
  kmem.freelist = r;
  kmem.free_pages++;
}

/*
 * LAB3：初始化物理页分配器。
 * 可用区间从内核镜像结束处开始，到按课程要求扣除保留页后的 PHYSTOP
 * 结束。LAB3_ALLOC_ORDER 只改变分配顺序，不改变可用物理页集合。
 */
void kinit(void) {
  uint64 p, npages, i;
  initlock(&kmem.lock, "kmem");
  initlock(&cow_lock, "cow");
  //计算可分配物理页的范围
  alloc_lo = PGROUNDUP((uint64)kernel_end);//kernel_end 是链接脚本导出的符号，表示内核镜像结束位置。
  alloc_hi = PHYSTOP - (uint64)LAB3_MEMCAP_DELTA * PGSIZE;//PHYSTOP 是 QEMU RAM 的结束位置：
  kmem.freelist = 0;
  kmem.free_pages = 0;
  kmem.shared_pages = 0;
  kmem.cow_copies = 0;
  for (i = 0; i < TOTAL_PAGES; i++)
    kmem.refcnt[i] = 0;
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
      free_push_locked(p);
    }
  } else if (LAB3_ALLOC_ORDER == 2) {//从中点附近开始交替分配
    /* midpoint, midpoint-1, midpoint+1, midpoint-2, ... */
    uint64 center = (npages - 1) / 2;
    for (i = npages; i != 0; i--) {
      uint64 k = i - 1;
      uint64 index = (k & 1) ? center + (k + 1) / 2 : center - k / 2;
      p = alloc_lo + index * PGSIZE;
      junk_page((void *)p);
      free_push_locked(p);
    }
  } else {//降序分配
    /* Prepending low-to-high makes the highest page the first allocation. */
    for (p = alloc_lo; p < alloc_hi; p += PGSIZE) {
      junk_page((void *)p);
      free_push_locked(p);
    }
  }//kinit把所有可用物理页串成一个单向空闲链表
}

/* LAB3：取出一个空闲页并清零；失败时返回 0。 */
void *kalloc(void) {
  /* 取出一个空闲页；清零动作既初始化新页，也隔离上一个拥有者的数据。 */
  acquire(&kmem.lock);
  struct run *r = kmem.freelist;
  if (r) {
    kmem.freelist = r->next;
    kmem.free_pages--;
    kmem.refcnt[page_index((uint64)r)] = 1;
  }
  release(&kmem.lock);
  if (r)
    zero_page(r);
  return r;
}

/* LAB5：仅最后一个引用释放时才毒化页面并归还空闲链表。 */
void kfree(void *pa) {
  uint64 p = (uint64)pa;
  if (!valid_alloc_page(p))
    return;

  acquire(&kmem.lock);
  uint64 idx = page_index(p);
  if (kmem.refcnt[idx] < 1)
    panic("kfree: refcnt underflow");
  kmem.refcnt[idx]--;
  if (kmem.refcnt[idx] == 1)
    kmem.shared_pages--;
  if (kmem.refcnt[idx] == 0) {
    junk_page(pa);
    free_push_locked(p);
  }
  release(&kmem.lock);
}

void
kref_inc(uint64 pa)
{
  if (!valid_alloc_page(pa))
    panic("kref_inc: invalid pa");
  acquire(&kmem.lock);
  uint64 idx = page_index(pa);
  if (kmem.refcnt[idx] < 1)
    panic("kref_inc: free page");
  if (kmem.refcnt[idx] == (uint16)-1)
    panic("kref_inc: overflow");
  if (kmem.refcnt[idx] == 1)
    kmem.shared_pages++;
  kmem.refcnt[idx]++;
  release(&kmem.lock);
}

uint16
kref_get(uint64 pa)
{
  if (!valid_alloc_page(pa))
    return 0;
  acquire(&kmem.lock);
  uint16 count = kmem.refcnt[page_index(pa)];
  release(&kmem.lock);
  return count;
}

uint64
kshared_count(void)
{
  acquire(&kmem.lock);
  uint64 count = kmem.shared_pages;
  release(&kmem.lock);
  return count;
}

uint64
kcow_copy_count(void)
{
  acquire(&kmem.lock);
  uint64 count = kmem.cow_copies;
  release(&kmem.lock);
  return count;
}

uint64
kfree_count(void)
{
  acquire(&kmem.lock);
  uint64 count = kmem.free_pages;
  release(&kmem.lock);
  return count;
}

uint64
vm_free_count(void)
{
  return kfree_count();
}

void
kref_selftest(void)
{
  uint64 free0 = kfree_count();
  uint64 shared0 = kshared_count();
  uint8 *p = kalloc();
  if (!p || kref_get((uint64)p) != 1)
    panic("kref selftest: alloc");

  for (uint64 i = 0; i < PGSIZE; i++)
    p[i] = 0xa5;
  kref_inc((uint64)p);
  kref_inc((uint64)p);
  if (kref_get((uint64)p) != 3 || kshared_count() != shared0 + 1)
    panic("kref selftest: share");

  kfree(p);
  if (kref_get((uint64)p) != 2 || p[0] != 0xa5 ||
      p[PGSIZE - 1] != 0xa5)
    panic("kref selftest: release 2");
  kfree(p);
  if (kref_get((uint64)p) != 1 || p[0] != 0xa5 ||
      p[PGSIZE - 1] != 0xa5)
    panic("kref selftest: release 1");
  kfree(p);
  if (kref_get((uint64)p) != 0 || p[PGSIZE - 1] != 0x5a ||
      kfree_count() != free0 ||
      kshared_count() != shared0 || kcow_copy_count() != 0)
    panic("kref selftest: restore");

  printf("lab5 kref selftest: pass free=%ld shared=%ld copies=%ld\n",
         (long)free0, (long)shared0, (long)kcow_copy_count());
}

/*
 * LAB3：沿 Sv39 的 L2 -> L1 -> L0 三级索引查找最终 PTE。
 * 中间项没有 R/W/X 时表示页表指针；alloc 非零时，缺失的中间页表
 * 由 kalloc() 创建。最终返回的是 PTE 的地址，而不是物理页地址。
 */
/**
 * walk: 遍历页表，找到指定虚拟地址对应的最低级（叶子层）页表项指针
 * 
 * @param pt    当前页表基址（第一次调用时是 L2 根页表的基址）
 * @param va    要查找的虚拟地址
 * @param alloc 是否允许在遍历过程中自动分配缺失的中间级页表页
 * @return      成功返回指向 L0 页表项的指针；失败返回 0
 */
pte_t *walk(pagetable_t pt, uint64 va, int alloc) {
  /* 
   * Sv39 架构要求虚拟地址必须有效。
   * MAXVA 通常是 1 << 38（即 0x4000000000），超过此范围的地址无法被映射。
   */
  if (va >= MAXVA) return 0;

  /*
   * Sv39 采用三级页表结构：L2（根） -> L1（中间） -> L0（叶子）。
   * PX(level, va) 宏负责从 va 中提取对应级别的 9 位索引（0~511）。
   * 循环从 L2 开始，走到 L1 结束，目的是把 pt 更新到 L0 页表的基址。
   */
  for (int level = 2; level > 0; level--) {
    // 获取当前级别的页表项（PTE）指针
    pte_t *p = &pt[PX(level, va)];

    if (*p & PTE_V) {
      /*
       * 当前 PTE 是有效的（Valid）。
       * 安全检查：如果这个 PTE 居然带有 R/W/X 权限，说明它是一个叶子节点（指向实际数据），
       * 而不是指向下一级页表的节点，这在遍历中间层时是非法的，直接返回 0。
       */
      if (PTE_FLAGS(*p) & (PTE_R | PTE_W | PTE_X)) return 0;
      
      /* 
       * 将 PTE 中的物理页号 (PPN) 转换为物理地址 (PA)，
       * 因为内核通常直接映射物理内存，所以物理地址可以直接当作指针使用，
       * 更新 pt 指针，准备进入下一级循环。
       */
      pt = (pagetable_t)PTE2PA(*p);
    } else {
      /*
       * 当前 PTE 无效（不存在），说明下一级页表还没有被创建。
       */
      if (!alloc) return 0; // 如果 alloc 为 0（如仅查询），直接失败返回
      
      /* 
       * 如果 alloc 为 1，则分配一个新的物理页作为下一级页表。
       * kalloc() 返回的是物理页的起始虚拟地址（因为是直接映射）。
       */
      pagetable_t child = kalloc();
      if (!child) return 0; // 物理内存耗尽，分配失败
      
      /* 
       * 将新分配的页表页地址（转回物理地址 PPN），加上有效位 PTE_V，
       * 写入当前 PTE 中。注意：这里没有设置 R/W/X，说明它只是中间页表。
       */
      *p = PA2PTE(child) | PTE_V;
      
      // 进入新分配的下一级页表
      pt = child;
    }
  }
  
  /* 
   * 循环结束后，pt 指向的是 L0 页表（叶子页表）的基址。
   * 返回 va 对应的 L0 页表项的指针。
   * 注意：此时返回的 PTE 可能有效也可能无效，需要调用者自行检查 *pte & PTE_V。
   */
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
/**
 * mappages: 将一段连续的物理地址空间映射到指定的虚拟地址范围
 * 
 * @param pt    目标页表（通常为当前进程的用户页表）
 * @param va    起始虚拟地址
 * @param size  要映射的内存大小（字节）
 * @param pa    起始物理地址
 * @param perm  页面权限（如 PTE_R | PTE_W | PTE_X | PTE_U）
 * @return      成功返回 0，失败返回 -1
 */
int mappages(pagetable_t pt, uint64 va, uint64 size, uint64 pa, uint64 perm) {
  // 1. 边界与合法性检查
  // size == 0 无意义；
  // va + size < va 说明发生了无符号整数溢出；
  // va + size > MAXVA 说明超出了允许的最大虚拟地址空间
  if (size == 0 || va + size < va || va + size > MAXVA) return -1;
  
  // 2. 计算需要映射的首页和末页的页对齐基地址
  // PGROUNDDOWN 将地址向下按页大小（如 4KB）对齐
  // (va + size - 1) 是为了正确处理恰好页对齐的情况（例如 va=0, size=4096，最后一页应该是 0 而不是 4096）
  uint64 a = PGROUNDDOWN(va), last = PGROUNDDOWN(va + size - 1);

  /*
   * 3. 预检查阶段（防重叠检查）
   * 先遍历整个目标虚拟地址范围，检查是否已经存在有效的叶子映射。
   * 这样做的好处是：如果发生冲突，直接返回失败，此时还没有建立任何新映射，
   * 不需要执行复杂的回滚逻辑，也不会误删调用前已经存在的映射。
   */
  for (uint64 check = a;; check += PGSIZE) {
    // 查找该虚拟地址对应的叶子页表项 (PTE)
    pte_t *p = walk(pt, check, 0); // 0 表示只查找，不分配中间页表页
    
    // 如果 PTE 存在且有效 (PTE_V)，并且具有 R/W/X 权限（说明它是一个叶子映射）
    // 则说明该地址已经被映射过了，直接报错
    if (p && (*p & PTE_V) && (PTE_FLAGS(*p) & (PTE_R | PTE_W | PTE_X)))
      return -1;
      
    if (check == last) break; // 检查完毕
  }

  // 4. 实际映射阶段（逐页建立映射）
  for (;;) {
    // 计算当前页对应的物理地址：
    // pa + (a - PGROUNDDOWN(va)) 保证物理地址的页内偏移与虚拟地址的页内偏移保持一致。
    // map_one 负责在页表中创建/修改 PTE，指向该物理页。
    if (map_one(pt, a, pa + (a - PGROUNDDOWN(va)), perm) < 0) {
      /* 
       * 5. 失败回滚逻辑
       * 如果 map_one 失败（例如内存耗尽，无法分配中间页表页），
       * 需要撤销本次调用已经建立的所有映射。
       * 注意：叶子物理页（pa）仍归调用者所有，这里绝不释放物理页本身，
       * 只负责清理 PTE 项和因映射而动态分配的空的中间页表页。
       */
      
      // 回滚 [PGROUNDDOWN(va), a) 范围内已经成功建立的映射
      for (uint64 rollback = PGROUNDDOWN(va); rollback < a; rollback += PGSIZE) {
        pte_t *p = walk(pt, rollback, 0);
        if (p) *p = 0; // 清除叶子 PTE，解除映射
        
        // 清理因为刚才的映射而分配的、现在已变空的中间级页表页
        // 这里的 2 代表清理该虚拟地址路径上的层级（具体取决于你的页表级数实现）
        prune_path(pt, rollback, 2);
      }
      
      // 对于当前失败的页 a，虽然映射没成功，但 map_one 可能已经分配了中间页表页
      // 所以也需要清理它的中间路径
      prune_path(pt, a, 2);
      return -1;
    }
    
    // 如果已经处理到了最后一页，映射完成，退出循环
    if (a == last) break;
    a += PGSIZE; // 继续映射下一页
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
    if (!(kernel_pt_store[l2] & PTE_V)) {//如果L2项还没有建立(kernel_pt_store[l2] 是根页表（L2）的 PTE，指向下一级 L1 页表。)
      pagetable_t p = kalloc();
      if (!p)
        return -1;
      kernel_pt_store[l2] = PA2PTE(p) | PTE_V;//PA2PTE(p)：把 L1 页表物理地址转换成 PTE 中的 PPN；
    }
    pagetable_t l1pt = (pagetable_t)PTE2PA(kernel_pt_store[l2]);
    l1pt[l1] = PA2PTE(pa) | PTE_R | PTE_W | PTE_X | PTE_V | PTE_A | PTE_D;//直接在 L1 写叶子 PTE
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
pagetable_t vm_new_user(struct trapframe *trapframe) {
  /* 用户页表根本身也是一个物理页，先申请根，再建立两个高地址共享页。 */
  pagetable_t pt = kalloc();
  if (!pt) return 0;
  extern char trampoline[];
  if (!trapframe ||
      vm_map(pt, TRAMPOLINE, PGSIZE, PGROUNDDOWN((uint64)trampoline), PTE_R|PTE_X) < 0 ||
      vm_map(pt, TRAPFRAME, PGSIZE, PGROUNDDOWN((uint64)trapframe), PTE_R|PTE_W) < 0) {
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

static int
share_user_page(pagetable_t dst, pagetable_t src, uint64 va,
                int *parent_changed)
{
  pte_t *sp = walk(src, va, 0);
  if (!sp || !(*sp & PTE_V))
    return 0;

  uint64 pa = PTE2PA(*sp);
  uint64 flags = PTE_FLAGS(*sp);
  if (flags & PTE_W) {
    flags = (flags & ~PTE_W) | PTE_COW;
    *sp = PA2PTE(pa) | flags;
    *parent_changed = 1;
  }

  if (mappages(dst, va, PGSIZE, pa, flags & ~PTE_V) < 0)
    return -1;
  kref_inc(pa);
  return 0;
}

/* LAB5：fork 只共享物理页；原可写页在父子两侧都标记为 COW。 */
int
vm_copy(pagetable_t dst, pagetable_t src, uint64 sz)
{
  int parent_changed = 0;

  for (uint64 va = 0; va < PGROUNDUP(sz); va += PGSIZE) {
    if (share_user_page(dst, src, va, &parent_changed) < 0)
      goto bad;
  }
  if (share_user_page(dst, src, 0x40000UL, &parent_changed) < 0)
    goto bad;

  if (parent_changed)
    sfence_vma();
  return 0;

bad:
  vm_free_user(dst, sz);
  if (parent_changed)
    sfence_vma();
  return -1;
}

/*
 * LAB5：分离一个 COW 页。cow_lock 保护从检查旧页引用数到递减旧页
 * 引用数的完整事务，避免双核同时把 refcnt==2 的页面复制两次。
 */
int
cow_alloc(pagetable_t pt, uint64 va)
{
  if (va >= MAXVA)
    return -1;
  va = PGROUNDDOWN(va);

  int warn_budget = 0;
  int warn_pid = 0;
  uint64 warn_copies = 0;
  acquire(&cow_lock);

  pte_t *pte = walk(pt, va, 0);
  if (!pte || !(*pte & PTE_V) || !(*pte & PTE_U)) {
    release(&cow_lock);
    return -1;
  }
  if (*pte & PTE_W) {
    release(&cow_lock);
    return 0;
  }
  if (!(*pte & PTE_COW)) {
    release(&cow_lock);
    return -1;
  }

  uint64 old_pa = PTE2PA(*pte);
  uint64 flags = PTE_FLAGS(*pte);
  uint16 refs = kref_get(old_pa);
  if (refs < 1) {
    release(&cow_lock);
    return -1;
  }

  if (refs == 1) {
    *pte = PA2PTE(old_pa) | ((flags & ~PTE_COW) | PTE_W);
    sfence_vma();
    release(&cow_lock);
    return 0;
  }

  uint8 *new_page = kalloc();
  if (!new_page) {
    release(&cow_lock);
    return -1;
  }
  uint8 *old_page = (uint8 *)old_pa;
  for (uint64 i = 0; i < PGSIZE; i++)
    new_page[i] = old_page[i];

  *pte = PA2PTE(new_page) | ((flags & ~PTE_COW) | PTE_W);
  acquire(&kmem.lock);
  kmem.cow_copies++;
  release(&kmem.lock);

  struct proc *p = myproc();
  if (p) {
    p->cow_copies++;
    if (!p->cow_budget_warned && p->cow_copies == LAB5_COW_BUDGET) {
      p->cow_budget_warned = 1;
      warn_budget = 1;
      warn_pid = p->pid;
      warn_copies = p->cow_copies;
    }
  }

  kfree((void *)old_pa);
  sfence_vma();
  release(&cow_lock);

  if (warn_budget)
    printf("[cow] pid=%d reached LAB5_COW_BUDGET=%d at copies=%ld; continuing\n",
           warn_pid, LAB5_COW_BUDGET, (long)warn_copies);
  return 0;
}

int
uvm_check_range(pagetable_t pt, uint64 va, uint64 len, uint64 want_perm)
{
  if (len == 0)
    return 0;
  if (va >= MAXVA || va + len < va || va + len > MAXVA)
    return -1;

  uint64 page = PGROUNDDOWN(va);
  uint64 last = PGROUNDDOWN(va + len - 1);
  for (;;) {
    pte_t *pte = walk(pt, page, 0);
    if (!pte || !(*pte & PTE_V) || !(*pte & PTE_U))
      return -1;
    if ((want_perm & PTE_W) && !(*pte & PTE_W)) {
      if (!(*pte & PTE_COW) || cow_alloc(pt, page) < 0)
        return -1;
      pte = walk(pt, page, 0);
    }
    if (!pte || ((*pte & want_perm) != want_perm))
      return -1;
    if (page == last)
      break;
    page += PGSIZE;
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
/* LAB3：按页边界把内核数据复制到指定用户地址空间。 */
int copyout_pt(pagetable_t pt, uint64 dstva, const void *src, uint64 len) {
  /* 内核写入用户页前也必须触发 COW，避免 read/wait 等调用误报只读。 */
  if (uvm_check_range(pt, dstva, len, PTE_W) < 0)
    return -1;
  const uint8 *s = src;
  while (len) {
    uint64 pa = walkaddr(pt, dstva, 1);
    if (!pa)
      return -1;
    uint64 n = PGSIZE - (dstva & (PGSIZE - 1));
    if (n > len)
      n = len;
    for (uint64 i = 0; i < n; i++)
      ((uint8 *)pa)[i] = s[i];
    s += n;
    dstva += n;
    len -= n;
  }
  return 0;
}
/* 向当前进程地址空间复制的兼容入口。 */
int copyout(uint64 dstva, const void *src, uint64 len) {
  return copyout_pt(active_user_pt, dstva, src, len);
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
    //写入satp，CPU 开始使用 kernel_pt_store 作为根页表。
  w_satp(MAKE_SATP(kernel_pt_store)); sfence_vma();
}
