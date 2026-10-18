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

struct run { struct run *next; };
static struct run *freelist;
static uint64 alloc_lo, alloc_hi;
static uint64 free_pages;
static uint64 kernel_pt_store[512] __attribute__((aligned(PGSIZE)));
static pagetable_t active_user_pt;

static void zero_page(void *p) {
  uint64 *w = p;
  for (int i = 0; i < PGSIZE / 8; i++) w[i] = 0;
}

static void junk_page(void *p) {
  uint8 *b = p;
  for (uint64 i = 0; i < PGSIZE; i++) b[i] = 0x01;
}

static void free_push(uint64 p) {
  struct run *r = (struct run *)p;
  r->next = freelist;
  freelist = r;
  free_pages++;
}

void kinit(void) {
  uint64 p, npages, i;
  alloc_lo = PGROUNDUP((uint64)kernel_end);
  alloc_hi = PHYSTOP - (uint64)LAB3_MEMCAP_DELTA * PGSIZE;
  freelist = 0;
  free_pages = 0;
  if (alloc_hi <= alloc_lo) return;
  npages = (alloc_hi - alloc_lo) / PGSIZE;

  /* Build the list in reverse of the requested allocation order. */
  if (LAB3_ALLOC_ORDER == 0) {
    /* Prepending high-to-low makes the lowest page the first allocation. */
    for (p = alloc_hi; p != alloc_lo;) {
      p -= PGSIZE;
      junk_page((void *)p);
      free_push(p);
    }
  } else if (LAB3_ALLOC_ORDER == 2) {
    /* midpoint, midpoint-1, midpoint+1, midpoint-2, ... */
    uint64 center = (npages - 1) / 2;
    for (i = npages; i != 0; i--) {
      uint64 k = i - 1;
      uint64 index = (k & 1) ? center + (k + 1) / 2 : center - k / 2;
      p = alloc_lo + index * PGSIZE;
      junk_page((void *)p);
      free_push(p);
    }
  } else {
    /* Prepending low-to-high makes the highest page the first allocation. */
    for (p = alloc_lo; p < alloc_hi; p += PGSIZE) {
      junk_page((void *)p);
      free_push(p);
    }
  }
}

void *kalloc(void) {
  struct run *r = freelist;
  if (r) { freelist = r->next; free_pages--; zero_page(r); }
  return r;
}

void kfree(void *pa) {
  uint64 p = (uint64)pa;
  if (p < alloc_lo || p >= alloc_hi || (p & (PGSIZE - 1))) return;
  junk_page(pa);
  free_push(p);
}

uint64 vm_free_count(void) { return free_pages; }

pte_t *walk(pagetable_t pt, uint64 va, int alloc) {
  if (va >= MAXVA) return 0;
  for (int level = 2; level > 0; level--) {
    pte_t *p = &pt[PX(level, va)];
    if (*p & PTE_V) {
      if (PTE_FLAGS(*p) & (PTE_R | PTE_W | PTE_X)) return 0;
      pt = (pagetable_t)PTE2PA(*p);
    } else {
      if (!alloc) return 0;
      pagetable_t child = kalloc();
      if (!child) return 0;
      *p = PA2PTE(child) | PTE_V;
      pt = child;
    }
  }
  return &pt[PX(0, va)];
}

static int map_one(pagetable_t pt, uint64 va, uint64 pa, uint64 perm) {
  pte_t *p = walk(pt, va, 1);
  if (!p || (*p & PTE_V)) return -1;
  *p = PA2PTE(pa) | perm | PTE_V | PTE_A;
  if (perm & PTE_W) *p |= PTE_D;
  return 0;
}

static int table_empty(pagetable_t pt) {
  for (int i = 0; i < 512; i++)
    if (pt[i] & PTE_V) return 0;
  return 1;
}

/* Remove empty intermediate tables below one root path. */
static void prune_path(pagetable_t pt, uint64 va, int level) {
  pte_t *p = &pt[PX(level, va)];
  if (!(*p & PTE_V) || (PTE_FLAGS(*p) & (PTE_R | PTE_W | PTE_X))) return;
  pagetable_t child = (pagetable_t)PTE2PA(*p);
  if (level > 0) prune_path(child, va, level - 1);
  if (level > 0 && table_empty(child)) {
    *p = 0;
    kfree(child);
  }
}

int mappages(pagetable_t pt, uint64 va, uint64 size, uint64 pa, uint64 perm) {
  if (size == 0 || va + size < va || va + size > MAXVA) return -1;
  uint64 a = PGROUNDDOWN(va), last = PGROUNDDOWN(va + size - 1);

  /* Preflight prevents rollback from deleting mappings that predate this call. */
  for (uint64 check = a;; check += PGSIZE) {
    pte_t *p = walk(pt, check, 0);
    if (p && (*p & PTE_V) && (PTE_FLAGS(*p) & (PTE_R | PTE_W | PTE_X)))
      return -1;
    if (check == last) break;
  }

  for (;;) {
    if (map_one(pt, a, pa + (a - PGROUNDDOWN(va)), perm) < 0) {
      /* Leaf pages remain caller-owned; only PTEs and empty tables roll back. */
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

int vm_map(pagetable_t pt, uint64 va, uint64 size, uint64 pa, uint64 perm) {
  return mappages(pt, va, size, pa, perm);
}

int vm_unmap(pagetable_t pt, uint64 va, uint64 npages, int do_free) {
  for (uint64 i = 0; i < npages; i++) {
    pte_t *p = walk(pt, va + i * PGSIZE, 0);
    if (!p || !(*p & PTE_V) || !(PTE_FLAGS(*p) & (PTE_R|PTE_W|PTE_X))) continue;
    uint64 pa = PTE2PA(*p); *p = 0; if (do_free) kfree((void *)pa);
    prune_path(pt, va + i * PGSIZE, 2);
  }
  return 0;
}

static void map_kernel(void) {
  for (uint64 pa = KERNBASE; pa < PHYSTOP; pa += KERNEL_MAP_2M) {
    uint64 va = pa;
    uint64 l2 = PX(2, va), l1 = PX(1, va);
    if (!(kernel_pt_store[l2] & PTE_V)) {
      pagetable_t p = kalloc();
      kernel_pt_store[l2] = PA2PTE(p) | PTE_V;
    }
    pagetable_t l1pt = (pagetable_t)PTE2PA(kernel_pt_store[l2]);
    l1pt[l1] = PA2PTE(pa) | PTE_R | PTE_W | PTE_X | PTE_V | PTE_A | PTE_D;
  }
  vm_map(kernel_pt_store, UART0, PGSIZE, UART0, PTE_R|PTE_W);
  for (uint64 pa = PLIC; pa < PLIC + 0x204000; pa += PGSIZE)
    vm_map(kernel_pt_store, pa, PGSIZE, pa, PTE_R|PTE_W);
  extern char trampoline[];
  vm_map(kernel_pt_store, TRAMPOLINE, PGSIZE, PGROUNDDOWN((uint64)trampoline), PTE_R|PTE_X);
  vm_map(kernel_pt_store, TRAPFRAME, PGSIZE, PGROUNDDOWN((uint64)&stage1_tf), PTE_R|PTE_W);
}

pagetable_t kernel_pagetable(void) { return kernel_pt_store; }
pagetable_t user_pagetable(void) { return active_user_pt; }
void vm_set_user_pagetable(pagetable_t pt) { active_user_pt = pt; }

pagetable_t vm_new_user(void) {
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

void vm_free_user(pagetable_t pt, uint64 sz) {
  if (!pt) return;
  vm_unmap(pt, 0, PGROUNDUP(sz)/PGSIZE, 1);
  vm_unmap(pt, 0x40000, 1, 1);
  vm_unmap(pt, TRAMPOLINE, 1, 0); vm_unmap(pt, TRAPFRAME, 1, 0);
  kfree(pt);
}

int vm_copy(pagetable_t dst, pagetable_t src, uint64 sz) {
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

uint64 walkaddr(pagetable_t pt, uint64 va, int write) {
  pte_t *p = walk(pt, va, 0);
  if (!p || !(*p & PTE_V) || !(*p & PTE_U) || (!(PTE_FLAGS(*p)&(PTE_R|PTE_X)) && !write) || (write && !(*p&PTE_W))) return 0;
  return PTE2PA(*p) + (va & (PGSIZE-1));
}

int copyin(void *dst, uint64 srcva, uint64 len) {
  uint8 *d=dst; while (len) { uint64 pa=walkaddr(active_user_pt,srcva,0); if(!pa)return -1; uint64 n=PGSIZE-(srcva&(PGSIZE-1)); if(n>len)n=len; for(uint64 i=0;i<n;i++)d[i]=((uint8*)pa)[i]; d+=n;srcva+=n;len-=n; } return 0;
}
int copyout(uint64 dstva, const void *src, uint64 len) {
  const uint8 *s=src; while(len){uint64 pa=walkaddr(active_user_pt,dstva,1);if(!pa)return -1;uint64 n=PGSIZE-(dstva&(PGSIZE-1));if(n>len)n=len;for(uint64 i=0;i<n;i++)((uint8*)pa)[i]=s[i];s+=n;dstva+=n;len-=n;}return 0;
}
int copyinstr(char *dst, uint64 srcva, uint64 max) {
  for(uint64 i=0;i<max;i++){if(copyin(&dst[i],srcva+i,1)<0)return -1;if(dst[i]=='\0')return 0;}return -1;
}

static void dump_level(pagetable_t pt, int level, uint64 base) {
  for (int i=0;i<512;i++) if (pt[i]&PTE_V) { uint64 va=base+((uint64)i<<PXSHIFT(level)),pa=PTE2PA(pt[i]); uint64 f=PTE_FLAGS(pt[i]); if(f&(PTE_R|PTE_W|PTE_X)){printf("LEAF va=0x%lx pa=0x%lx perm=%c%c%c%c\n",va,pa,(f&PTE_R)?'r':'-',(f&PTE_W)?'w':'-',(f&PTE_X)?'x':'-',(f&PTE_U)?'u':'-');} else {printf("L%d va=0x%lx pa=0x%lx perm=----\n",level,va,pa);dump_level((pagetable_t)pa,level-1,va);} }
}
void dump_pagetable(pagetable_t pt){printf("DUMP-PAGETABLE begin\n");dump_level(pt,2,0);printf("DUMP-PAGETABLE end\n");}

void vm_init(void) {
  zero_page(kernel_pt_store); kinit();
  map_kernel(); w_satp(MAKE_SATP(kernel_pt_store)); sfence_vma();
}
