/*
 * LAB2-STAGE1: C half of the user-mode trap path.
 *
 * The supplied trampoline.S performs the register-level work:
 *   uservec: save user registers, switch to the kernel environment, call us
 *   userret: switch back to the user page table, restore registers, sret
 *
 * This file supplies the trapframe data and the C decisions between those
 * two assembly entry points. Stage 1 uses one static address space and one
 * static trapframe; later process stages will make them per-process.
 */

#include "types.h"       /* uint64 and freestanding integer types. */
#include "riscv.h"       /* CSR accessors, page-table macros, and PTE flags. */
#include "memlayout.h"   /* UART0, KERNBASE, TRAPFRAME, TRAMPOLINE. */
#include "course_sid.h"  /* lab parameters used by the course contract. */
#include "trap.h"        /* struct trapframe and trap declarations. */
#include "syscall.h"     /* SYS_sync=22 for the stage-1 self-test. */

/* Lab1 printf is the only output facility available at this stage. */
extern void printf(const char *fmt, ...);

/*
 * Symbols supplied by trampoline.S and the linker script.
 * trampoline is the linked address of the trampoline code page.
 * userret is the linked address of the return entry inside that page.
 */
extern char trampoline[];
extern char userret[];

/* Linker marker retained for the later physical page allocator. */
extern char kernel_end[];

/*
 * LAB2-STAGE1 address spaces:
 *
 * kernel_pt and user_pt are the roots passed to satp. The *_l1 and *_l0
 * arrays are the lower-level Sv39 page-table pages referenced by those roots.
 *
 * This is deliberately a static bootstrap arrangement. It is enough to
 * execute one user test and return through the trampoline; it is not yet the
 * general allocator/process/page-table design required by later labs.
 */
/* Kernel direct-map page-table roots and lower levels. */
__attribute__((aligned(PGSIZE)))
static uint64 kernel_pt[512];
__attribute__((aligned(PGSIZE)))
static uint64 user_pt[512];

__attribute__((aligned(PGSIZE)))
static uint64 kernel_l1[512];
__attribute__((aligned(PGSIZE)))
static uint64 kernel_l0[512];
__attribute__((aligned(PGSIZE)))
static uint64 kernel_mmio_l0[512];
__attribute__((aligned(PGSIZE)))
static uint64 kernel_hi_l1[512];
__attribute__((aligned(PGSIZE)))
static uint64 kernel_hi_l0[512];

/* User page-table roots and lower levels. */
__attribute__((aligned(PGSIZE)))
static uint64 user_l1[512];
__attribute__((aligned(PGSIZE)))
static uint64 user_l0[512];
__attribute__((aligned(PGSIZE)))
static uint64 user_hi_l1[512];
__attribute__((aligned(PGSIZE)))
static uint64 user_hi_l0[512];
__attribute__((aligned(PGSIZE)))
static uint64 user_low_l1[512];
__attribute__((aligned(PGSIZE)))
static uint64 user_low_l0[512];

/*
 * LAB2-STAGE1 storage:
 *   stage1_kernel_stack is selected by uservec before usertrap().
 *   stage1_user_stack is mapped at virtual address 0x4000.
 *   stage1_tf is the single trapframe addressed as TRAPFRAME.
 */
__attribute__((aligned(PGSIZE)))
static uint8 stage1_kernel_stack[PGSIZE];
__attribute__((aligned(PGSIZE)))
static uint8 stage1_user_stack[PGSIZE];
/*
 * Exported only for the stage-1 self-test: main() deliberately constructs
 * the initial trapframe instead of hiding that important setup in a helper.
 */
__attribute__((aligned(16)))
struct trapframe stage1_tf;

/* Map the first 2 MiB of physical RAM for the bootstrap address spaces. */
#define STAGE1_KERNEL_MAP_SIZE (2 * 1024 * 1024UL)

/* RISC-V scause value for an environment call originating in U mode. */
#define SCAUSE_ECALL_U 8

/*
 * Pre-set the architectural accessed/dirty bits. This avoids an A/D update
 * fault while stage 1 is using its hand-built page tables.
 */
#define PTE_A_STAGE1 (1L << 6)
#define PTE_D_STAGE1 (1L << 7)

/*
 * uservec is the first byte of the trampoline page. stvec needs the high
 * virtual address shared by the user and kernel page tables.
 */
static uint64
uservec_va(void)
{
  return TRAMPOLINE;
}

/*
 * userret is inside the same page. Preserve its page offset while translating
 * its linked kernel address to the high virtual trampoline address.
 */
static uint64
userret_va(void)
{
  return TRAMPOLINE + ((uint64)userret - (uint64)trampoline);
}

static void
map_page(uint64 *pt, uint64 *l1, uint64 *l0,
         uint64 va, uint64 pa, uint64 perm)
{
  /*
   * Install one 4 KiB leaf mapping in a three-level Sv39 page table.
   *
   * pt  = level-2 root
   * l1  = level-1 page
   * l0  = level-0 page containing the final PTE
   * va  = virtual page to map
   * pa  = physical page to expose
   * perm = R/W/X/U permission bits
   */
  uint64 l2_index = PX(2, va);
  uint64 l1_index = PX(1, va);
  uint64 l0_index = PX(0, va);

  /* Create root -> level-1 link if this root slot is unused. */
  if (pt[l2_index] == 0)
    pt[l2_index] = PA2PTE(l1) | PTE_V;

  /* Create level-1 -> level-0 link if this level-1 slot is unused. */
  if (l1[l1_index] == 0)
    l1[l1_index] = PA2PTE(l0) | PTE_V;

  /*
   * Install the leaf PTE. PTE_A is set for every mapping; writable pages
   * also receive PTE_D. PA2PTE places the physical page number in PTE bits.
   */
  l0[l0_index] = PA2PTE(pa) | perm | PTE_V | PTE_A_STAGE1;
  if (perm & PTE_W)
    l0[l0_index] |= PTE_D_STAGE1;
}

/*
 * Map the kernel's low linear address range.
 *
 * The user table also receives a temporary U-accessible direct map in this
 * stage. This is intentionally permissive bootstrap code; proper user/kernel
 * isolation belongs to later virtual-memory work.
 */
static void
map_kernel_region(void)
{
  uint64 pa;

  /* Map RAM pages at the same virtual and physical addresses. */
  for (pa = KERNBASE; pa < KERNBASE + STAGE1_KERNEL_MAP_SIZE;
       pa += PGSIZE) {
    map_page(kernel_pt, kernel_l1, kernel_l0,
             pa, pa, PTE_R | PTE_W | PTE_X);
    map_page(user_pt, user_l1, user_l0,
             pa, pa, PTE_R | PTE_W | PTE_X | PTE_U);
  }

  /*
   * UART0 is outside RAM, so it needs a separate level-0 page under the
   * appropriate level-1 slot instead of being placed in the RAM L0 page.
   */
  map_page(kernel_pt, kernel_l1, kernel_mmio_l0,
           UART0, UART0, PTE_R | PTE_W);
}

/*
 * Map the special regions required by trampoline.S:
 *
 *   TRAMPOLINE -> physical trampoline code page
 *   TRAPFRAME  -> physical stage1_tf page
 *   0x4000     -> physical stage1_user_stack page
 *
 * The trampoline and trapframe are supervisor-only. The user program and
 * user stack receive PTE_U through their ordinary user mappings.
 */
static void
map_shared_special_pages(void)
{
  /* Physical/linear addresses of the backing objects. */
  uint64 tramp_pa = (uint64)trampoline;
  uint64 trapframe_pa = (uint64)&stage1_tf;
  uint64 user_stack_pa = (uint64)stage1_user_stack;

  /* Kernel page-table mappings for the high transition addresses. */
  map_page(kernel_pt, kernel_hi_l1, kernel_hi_l0,
           TRAMPOLINE, tramp_pa, PTE_R | PTE_X);
  map_page(kernel_pt, kernel_hi_l1, kernel_hi_l0,
           TRAPFRAME, trapframe_pa, PTE_R | PTE_W);

  /* User page-table mappings needed while uservec/userret run. */
  map_page(user_pt, user_hi_l1, user_hi_l0,
           TRAMPOLINE, tramp_pa, PTE_R | PTE_X);
  map_page(user_pt, user_hi_l1, user_hi_l0,
           TRAPFRAME, trapframe_pa, PTE_R | PTE_W);
  map_page(user_pt, user_low_l1, user_low_l0,
           STAGE1_USER_STACK, user_stack_pa, PTE_R | PTE_W | PTE_U);
}

/* Encode the kernel page-table root into the Sv39 satp format. */
static uint64
kernel_satp(void)
{
  return MAKE_SATP(kernel_pt);
}

/* Encode the stage-1 user page-table root into the Sv39 satp format. */
static uint64
user_satp(void)
{
  return MAKE_SATP(user_pt);
}

/*
 * LAB2-STAGE1 syscall dispatcher.
 *
 * Only SYS_sync is intentionally implemented here. The default path is
 * required by the ABI: an unknown number must return deterministic -1 rather
 * than panic or re-enter the same instruction forever.
 */
static long
dispatch_syscall(uint64 number)
{
  switch (number) {
  case SYS_sync:
    return 0;
  default:
    return -1;
  }
}

void
usertrap(void)
{
  /*
   * At this point uservec has already saved user registers into stage1_tf,
   * loaded stage1_kernel_stack into sp, changed satp to kernel_pt, and
   * jumped through trapframe->kernel_trap to this C function.
   */
  uint64 cause = r_scause();
  long result;

  /*
   * Stage 1 has no device-interrupt or page-fault handler yet. Print the
   * diagnostic CSR values and stop if this milestone receives another cause.
   */
  if (cause != SCAUSE_ECALL_U) {
    printf("lab2 trap fault scause=%lx sepc=%lx stval=%lx\n",
           cause, r_sepc(), r_stval());
    for (;;)
      asm volatile("wfi");
  }

  /*
   * Hardware left sepc pointing at the ecall instruction itself. Save it in
   * the C-visible trapframe and advance by one 32-bit instruction so sret
   * resumes at the instruction after ecall.
   */
  stage1_tf.epc = r_sepc();
  stage1_tf.epc += 4;

  /*
   * uservec saved the user's a7 at offset 168. Dispatch that number and
   * write the ABI return value into saved a0 at offset 112. userret restores
   * that saved a0 as the user's syscall return value.
   */
  result = dispatch_syscall(stage1_tf.a7);
  stage1_tf.a0 = (uint64)result;

  /* Visible trace for the stage-1 round-trip self-test. */
  printf("lab2 ecall num=%ld ret=%ld epc=%lx\n",
         (long)stage1_tf.a7, (long)result, stage1_tf.epc);

  /* Prepare CSR state and transfer control to assembly userret. */
  usertrapret();
}

/*
 * Prepare the supervisor CSRs and trapframe state required by userret.
 *
 * This is the C-side "return preparation" half. It does not restore general
 * registers and does not execute sret; userret does both in trampoline.S.
 */
void
usertrapret(void)
{
  uint64 status;
  void (*return_to_user)(uint64);

  /* The next trap originating in U mode must enter uservec again. */
  w_stvec(uservec_va());

  /* Preserve unrelated sstatus bits while changing only return semantics. */
  status = r_sstatus();

  /* SPP=0 makes the eventual sret return to U mode. */
  status &= ~SSTATUS_SPP;

  /* SPIE=1 restores interrupt-enable state when sret completes. */
  status |= SSTATUS_SPIE;
  w_sstatus(status);

  /* sret takes the next user PC from sepc. */
  w_sepc(stage1_tf.epc);

  /*
   * userret receives user satp in a0. It then changes a0 to TRAPFRAME,
   * restores all saved user registers, restores saved user a0, and executes
   * sret.
   */
  return_to_user = (void (*)(uint64))userret_va();
  return_to_user(user_satp());

  /* The assembly return path should never return to this C function. */
  for (;;)
    ;
}

void
trap_init(void)
{
  uint64 kernel_stack_top;

  /*
   * Keep the linker symbol referenced; later labs use kernel_end as the
   * starting address for physical page allocation.
   */
  (void)kernel_end;

  /* Fill the static page tables before enabling address translation. */
  map_kernel_region();
  map_shared_special_pages();

  /* Flush old translations, enter the kernel page table, and flush again. */
  sfence_vma();
  w_satp(kernel_satp());
  sfence_vma();

  /*
   * Fill the first trapframe words read by uservec before it can call
   * usertrap. These values describe the kernel environment to enter.
   */
  stage1_tf.kernel_satp = kernel_satp();
  kernel_stack_top = (uint64)&stage1_kernel_stack[PGSIZE];
  stage1_tf.kernel_sp = kernel_stack_top;
  stage1_tf.kernel_trap = (uint64)usertrap;
/*
 * mhartid is an M-mode CSR and cannot be read after mret. Lab1 deliberately
 * parks every hart except hart 0, so the bootstrap can record the known value.
 */
  stage1_tf.kernel_hartid = 0;

  /* Install uservec before the first user-mode sret. */
  w_stvec(uservec_va());
}
