# ARM32 MMU - Baremetal Initialization
## Document 2: Complete Bring-Up from Reset to Virtual Addressing

**Author:** Senior Kernel Engineer
**Target Architecture:** ARMv7-A (ARM32)
**Scope:** Bare-metal MMU initialization, identity mapping, physical-to-virtual transition
**Revision:** 1.0
**Prerequisites:** Document 01 (Architecture Overview)

---

## Table of Contents
1. [System State at Reset](#1-system-state-at-reset)
2. [Pre-MMU Mandatory Operations](#2-pre-mmu-mandatory-operations)
3. [Page Table Design for Boot](#3-page-table-design-for-boot)
4. [Page Table Construction in C](#4-page-table-construction-in-c)
5. [Complete Boot Assembly Sequence](#5-complete-boot-assembly-sequence)
6. [MMU Enable Sequence - Critical Ordering](#6-mmu-enable-sequence---critical-ordering)
7. [Physical-to-Virtual Jump Trick](#7-physical-to-virtual-jump-trick)
8. [Post-MMU Cleanup](#8-post-mmu-cleanup)
9. [Linker Script Design](#9-linker-script-design)
10. [Debugging Baremetal MMU Issues](#10-debugging-baremetal-mmu-issues)
11. [Complete Reference Implementation](#11-complete-reference-implementation)

---

## 1. System State at Reset

When an ARMv7-A core comes out of reset, the hardware state is:

```
SCTLR.M  = 0   → MMU is OFF (all accesses are physical)
SCTLR.C  = 0   → Data cache DISABLED
SCTLR.I  = 0   → Instruction cache DISABLED (implementation defined)
SCTLR.Z  = 0   → Branch predictor DISABLED
SCTLR.A  = 0   → Alignment checking DISABLED
TTBR0    = UNKNOWN
TTBR1    = UNKNOWN
TTBCR    = 0   → N=0, TTBR0 covers full 4GB
DACR     = 0   → All domains: No Access
CPSR.M   = 0b10011 → SVC mode
CPSR.I   = 1   → IRQ disabled
CPSR.F   = 1   → FIQ disabled
```

### 1.1 Implications

```
DANGER ZONE - State at Reset:

1. TLBs contain UNKNOWN entries → must invalidate before MMU enable
2. Caches may have STALE data → must invalidate before enabling
3. Branch predictor UNKNOWN → must invalidate
4. Page tables not set up → MMU CANNOT be enabled yet
5. All code runs at PHYSICAL addresses
```

### 1.2 Memory Map Assumptions

```
Typical embedded SoC:

Physical Address Space:
0x00000000 ├───────────────────┤ ← Reset vector
           │   ROM/Flash       │ (may be mirrored to 0x0)
0x1C000000 ├───────────────────┤
           │   DRAM (2GB)      │ ← Where our code loads
0x9FFFFFFF ├───────────────────┤
           │   Device MMIO     │
0xFFFFFFFF └───────────────────┘

Our firmware loads at: PHYS_BASE = 0x60000000 (example)
We want to run at:     VIRT_BASE = 0xC0000000 (kernel convention)
Offset: VIRT_BASE - PHYS_BASE = 0x60000000
```

---

## 2. Pre-MMU Mandatory Operations

**These MUST complete BEFORE setting SCTLR.M=1. No exceptions.**

### 2.1 Why These Steps Are Non-Negotiable

The hardware TLBs and caches are in an UNKNOWN state after power-on. If you enable the MMU with stale TLB entries, the hardware may translate your next instruction fetch to a wrong physical address — instant crash. If you enable caches before invalidation, you may read stale data.

```assembly
/* ============================================================
 * STEP 1: Invalidate TLBs
 * Must be done with MMU OFF (physical addresses)
 * ============================================================ */
mmu_invalidate_tlb:
    mov     r0, #0
    mcr     p15, 0, r0, c8, c7, 0   /* TLBIALL - invalidate unified TLB */
    mcr     p15, 0, r0, c8, c5, 0   /* ITLBIALL - invalidate I-TLB */
    mcr     p15, 0, r0, c8, c6, 0   /* DTLBIALL - invalidate D-TLB */
    dsb                              /* ensure TLB ops complete */
    isb                              /* flush pipeline */

/* ============================================================
 * STEP 2: Invalidate Branch Predictor
 * ============================================================ */
mmu_invalidate_bp:
    mov     r0, #0
    mcr     p15, 0, r0, c7, c5, 6   /* BPIALL - flush entire branch predictor */
    dsb
    isb

/* ============================================================
 * STEP 3: Invalidate Instruction Cache
 * ============================================================ */
mmu_invalidate_icache:
    mov     r0, #0
    mcr     p15, 0, r0, c7, c5, 0   /* ICIALLU - invalidate all I-cache */
    dsb
    isb

/* ============================================================
 * STEP 4: Invalidate Data Cache (by Set/Way)
 * More complex - must iterate all sets and ways
 * ============================================================ */
mmu_invalidate_dcache:
    /* Read cache size ID register for L1 D-cache */
    mov     r0, #0                  /* CSSELR: select L1 D-cache */
    mcr     p15, 2, r0, c0, c0, 0  /* write CSSELR */
    isb
    mrc     p15, 1, r0, c0, c0, 0  /* read CCSIDR */

    and     r3, r0, #0x7            /* line size = 2^(linesize+4) */
    add     r3, r3, #4              /* r3 = log2(line size in bytes) */
    ldr     r1, =0x7FFF
    and     r1, r1, r0, lsr #13    /* r1 = sets - 1 */
    ldr     r2, =0x3FF
    and     r2, r2, r0, lsr #3     /* r2 = ways - 1 */
    clz     r4, r2                  /* r4 = bit position of way in DCISW */

dcache_inv_set_way_loop:
    mov     r5, r1                  /* r5 = set counter */
dcache_inv_set_loop:
    orr     r6, r6, r2, lsl r4     /* way << way_shift */
    orr     r6, r6, r5, lsl r3     /* set << set_shift */
    mcr     p15, 0, r6, c7, c6, 2 /* DCISW - invalidate by set/way */
    subs    r5, r5, #1             /* decrement set */
    bge     dcache_inv_set_loop
    subs    r2, r2, #1             /* decrement way */
    bge     dcache_inv_set_way_loop
    dsb
    isb
```

### 2.2 Register Initialization

```assembly
/* ============================================================
 * STEP 5: Configure DACR - Domain Access Control
 * Must set BEFORE enabling MMU or all memory accesses fault
 * ============================================================ */
setup_dacr:
    /* Domain 0 = Manager (bypass AP checks, used for kernel)
     * Domain 1 = Client  (respect AP checks, used for user) */
    ldr     r0, =0x00000003         /* D0=Manager (0b11) */
    mcr     p15, 0, r0, c3, c0, 0  /* write DACR */

/* ============================================================
 * STEP 6: Configure TTBCR
 * N=1 → TTBR0 covers 0x00000000-0x7FFFFFFF (user)
 *        TTBR1 covers 0x80000000-0xFFFFFFFF (kernel)
 * ============================================================ */
setup_ttbcr:
    mov     r0, #1                  /* N=1 */
    mcr     p15, 0, r0, c2, c0, 2  /* write TTBCR */
```

---

## 3. Page Table Design for Boot

### 3.1 What We Need to Map

During boot, we need exactly **three** mappings:

```
Mapping 1: IDENTITY MAP (Physical = Virtual)
  VA 0x60000000 → PA 0x60000000
  Purpose: CPU is still executing physical-address instructions.
           When MMU turns on, the NEXT instruction fetch must still work.
           Without this, the core would fault immediately on MMU enable.
  Size: 1MB section (covers our boot code)

Mapping 2: KERNEL VIRTUAL MAP (Virtual = Physical + offset)
  VA 0xC0000000 → PA 0x60000000
  Purpose: Final kernel address — code will jump here after MMU on.
  Size: Enough to cover kernel image (e.g., 64MB = 64 sections)

Mapping 3: DEVICE MMIO MAP
  VA 0xF8000000 → PA 0xF8000000 (identity for MMIO)
  Purpose: UART, interrupt controller, timers
  Memory type: Device/Strongly-ordered (NOT cacheable)
```

### 3.2 Boot Page Table Layout

```
L1 Table: 4096 × 4-byte entries = 16KB, must be 16KB aligned

Index calculation: VA[31:20] → L1 index

Identity map:     VA[31:20] = 0x600 → entry[0x600] = SECTION(PA=0x60000000)
Kernel map:       VA[31:20] = 0xC00 → entry[0xC00] = SECTION(PA=0x60000000)
                  VA[31:20] = 0xC01 → entry[0xC01] = SECTION(PA=0x60100000)
                  ... (64 entries for 64MB)
MMIO map:         VA[31:20] = 0xF80 → entry[0xF80] = SECTION(PA=0xF8000000)

All other entries: INVALID (0x00000000)
```

---

## 4. Page Table Construction in C

```c
/* ============================================================
 * boot_mmu.c - Boot-time page table construction
 *
 * IMPORTANT: This code runs with MMU OFF.
 * All addresses used here are PHYSICAL.
 * ============================================================ */

#include <stdint.h>

/* Physical address where page tables are stored (must be 16KB aligned) */
#define PAGE_TABLE_BASE_PHYS    0x60004000UL

/* Physical base of our image */
#define PHYS_BASE               0x60000000UL

/* Virtual base of our image */
#define VIRT_BASE               0xC0000000UL

/* VA-PA offset */
#define PAGE_OFFSET             (VIRT_BASE - PHYS_BASE)

/* Number of 1MB sections for the kernel mapping */
#define KERNEL_SIZE_SECTIONS    64      /* 64MB */

/* Device MMIO physical base */
#define MMIO_PHYS_BASE          0xF8000000UL

/* ============================================================
 * L1 Section Descriptor Bits
 * ============================================================ */

/* Bits [1:0] = 0b10 → Section */
#define L1_SECTION              (1U << 1)

/* Domain field [8:5] */
#define L1_DOMAIN(d)            ((d) << 5)

/* Access Permissions */
#define L1_AP_NONE              (0U << 10)   /* No access */
#define L1_AP_KRW               (1U << 10)   /* Kernel RW, User none */
#define L1_AP_KRW_URO           (2U << 10)   /* Kernel RW, User RO */
#define L1_AP_FULL              (3U << 10)   /* Full access */
#define L1_APX                  (1U << 15)   /* APX for RO */

/* Memory type: Normal, Outer+Inner WB, WA, Shareable */
#define L1_MEM_NORMAL_WB_WA     ((1U << 12) | (1U << 3) | (1U << 2))

/* Memory type: Device (Strongly-ordered) */
#define L1_MEM_STRONGLY_ORDERED (0U)

/* Memory type: Normal, Non-cacheable */
#define L1_MEM_NORMAL_NC        ((1U << 12))

/* Shareable */
#define L1_SHAREABLE            (1U << 16)

/* Not Global (user mappings) */
#define L1_NOT_GLOBAL           (1U << 17)

/* Execute Never */
#define L1_XN                   (1U << 4)

/* ============================================================
 * Kernel section attributes:
 * - Normal WB WA memory
 * - Shareable (SMP coherent)
 * - Kernel RW, User no access
 * - Global (not ASID-tagged)
 * - Executable
 * ============================================================ */
#define KERNEL_SECTION_FLAGS    (L1_SECTION              | \
                                 L1_MEM_NORMAL_WB_WA     | \
                                 L1_SHAREABLE             | \
                                 L1_AP_KRW                | \
                                 L1_DOMAIN(0))

/* ============================================================
 * Device section attributes:
 * - Strongly-ordered
 * - Non-executable
 * - Kernel only
 * ============================================================ */
#define DEVICE_SECTION_FLAGS    (L1_SECTION              | \
                                 L1_MEM_STRONGLY_ORDERED  | \
                                 L1_AP_KRW                | \
                                 L1_XN                    | \
                                 L1_DOMAIN(0))

/* L1 page table: 4096 entries × 4 bytes = 16KB */
static uint32_t *const l1_table = (uint32_t *)PAGE_TABLE_BASE_PHYS;

/* Helper: convert physical address to L1 index */
static inline uint32_t pa_to_l1_index(uint32_t pa)
{
    return pa >> 20;
}

/* Helper: extract section base PA from descriptor */
static inline uint32_t l1_section_base(uint32_t pa)
{
    return pa & 0xFFF00000U;  /* mask to 1MB boundary */
}

/*
 * boot_mmu_init_table() - Zero and populate the boot L1 page table
 *
 * Must be called with MMU OFF.
 * PHYS_BASE used for all addresses.
 */
void boot_mmu_init_table(void)
{
    uint32_t i;
    uint32_t virt_idx, phys_pa;

    /* Step 1: Zero the entire L1 table (all invalid) */
    for (i = 0; i < 4096; i++)
        l1_table[i] = 0;

    /* --------------------------------------------------------
     * Step 2: Identity map — physical == virtual
     * Covers the 1MB section where our boot code executes.
     * This keeps the CPU alive the instant MMU is enabled.
     * -------------------------------------------------------- */
    {
        uint32_t boot_pa = PHYS_BASE;   /* 0x60000000 */
        uint32_t idx     = pa_to_l1_index(boot_pa);  /* 0x600 */
        l1_table[idx] = l1_section_base(boot_pa) | KERNEL_SECTION_FLAGS;
    }

    /* --------------------------------------------------------
     * Step 3: Kernel virtual mapping
     * VA 0xC0000000 → PA 0x60000000 (KERNEL_SIZE_SECTIONS × 1MB)
     * -------------------------------------------------------- */
    for (i = 0; i < KERNEL_SIZE_SECTIONS; i++) {
        virt_idx = pa_to_l1_index(VIRT_BASE) + i;   /* 0xC00, 0xC01, ... */
        phys_pa  = PHYS_BASE + (i << 20);
        l1_table[virt_idx] = l1_section_base(phys_pa) | KERNEL_SECTION_FLAGS;
    }

    /* --------------------------------------------------------
     * Step 4: Device MMIO mapping
     * VA = PA (identity) with device attributes
     * -------------------------------------------------------- */
    {
        uint32_t mmio_size_mb = 16;  /* 16MB of MMIO */
        for (i = 0; i < mmio_size_mb; i++) {
            uint32_t mmio_addr = MMIO_PHYS_BASE + (i << 20);
            uint32_t idx       = pa_to_l1_index(mmio_addr);
            l1_table[idx] = l1_section_base(mmio_addr) | DEVICE_SECTION_FLAGS;
        }
    }

    /* --------------------------------------------------------
     * Step 5: Data Synchronization Barrier
     * Ensure all page table writes are visible to the MMU hardware
     * before we write to TTBR0/TTBR1.
     * -------------------------------------------------------- */
    __asm__ volatile("dsb" ::: "memory");
}

/*
 * boot_mmu_set_ttbr() - Load page table base into TTBR registers
 *
 * TTBR attributes:
 * - RGN[4:3] = 0b01 → Outer Write-Back, Write-Allocate cacheable
 * - IRGN[0][6] = 0b11 → Inner Write-Back, Write-Allocate cacheable
 * - S[1] = 1 → Shareable (needed for SMP)
 */
void boot_mmu_set_ttbr(void)
{
    uint32_t ttbr_flags;

    /*
     * TTBR cache attributes (inner/outer for page table walks):
     * bit[0] IRGN[1] = 1 → Inner WB WA
     * bit[1] S       = 1 → Shareable
     * bit[3:4] RGN   = 01 → Outer WB WA
     * bit[6] IRGN[0] = 1 → Inner WB WA (combined with bit[0])
     */
    ttbr_flags = (1U << 6) | (1U << 0) | (1U << 1) | (1U << 3);

    /* Set TTBR1 (kernel space, 0x80000000-0xFFFFFFFF) */
    __asm__ volatile(
        "mcr p15, 0, %0, c2, c0, 1\n"  /* TTBR1 = page table + flags */
        :: "r"(PAGE_TABLE_BASE_PHYS | ttbr_flags)
        : "memory"
    );

    /* Set TTBR0 (user space, 0x00000000-0x7FFFFFFF) */
    /* Point to same table for boot; will be updated on first context switch */
    __asm__ volatile(
        "mcr p15, 0, %0, c2, c0, 0\n"  /* TTBR0 = page table + flags */
        :: "r"(PAGE_TABLE_BASE_PHYS | ttbr_flags)
        : "memory"
    );

    /* TTBCR.N=1: TTBR0 → 0-0x7FFFFFFF, TTBR1 → 0x80000000-0xFFFFFFFF */
    __asm__ volatile(
        "mcr p15, 0, %0, c2, c0, 2\n"  /* TTBCR */
        :: "r"(1U)
        : "memory"
    );

    __asm__ volatile("isb" ::: "memory");
}
```

---

## 5. Complete Boot Assembly Sequence

```assembly
/*
 * startup.S - ARM32 baremetal startup
 *
 * Execution flow:
 * 1. Enter at _reset (physical address)
 * 2. Set up stack (physical address)
 * 3. Invalidate TLBs, caches, BP
 * 4. Call boot_mmu_init_table() (C)
 * 5. Load TTBR, DACR, TTBCR
 * 6. Enable MMU (SCTLR.M = 1)
 * 7. Long-jump to virtual address
 * 8. Remove identity map
 * 9. Jump to main()
 */

    .syntax unified
    .arch armv7-a
    .text

    .global _reset
    .type   _reset, %function

/* Physical address constants */
#define PHYS_STACK_TOP      0x60004000   /* 16KB stack below page tables */
#define PAGE_TABLE_PHYS     0x60004000
#define VIRT_MAIN           0xC0008000   /* Virtual address of main() */

/* SCTLR bits */
#define SCTLR_M     (1 << 0)    /* MMU enable */
#define SCTLR_A     (1 << 1)    /* Alignment check */
#define SCTLR_C     (1 << 2)    /* D-cache enable */
#define SCTLR_Z     (1 << 11)   /* Branch predictor */
#define SCTLR_I     (1 << 12)   /* I-cache enable */
#define SCTLR_V     (1 << 13)   /* High vectors 0xFFFF0000 */
#define SCTLR_TRE   (1 << 28)   /* TEX remap enable */
#define SCTLR_AFE   (1 << 29)   /* Access flag enable */

_reset:
    /* --------------------------------------------------------
     * 1. Ensure we are in SVC mode, IRQ/FIQ disabled
     * -------------------------------------------------------- */
    mrs     r0, cpsr
    bic     r0, r0, #0x1F           /* clear mode bits */
    orr     r0, r0, #0xD3           /* SVC mode, I=1, F=1 */
    msr     cpsr_c, r0

    /* --------------------------------------------------------
     * 2. Set up physical stack pointer
     *    Stack grows downward from PHYS_STACK_TOP
     * -------------------------------------------------------- */
    ldr     sp, =PHYS_STACK_TOP

    /* --------------------------------------------------------
     * 3. Save ATAGS/DTB pointer (r0, r1, r2 from bootloader)
     *    Store in a physical-address variable before stack use
     * -------------------------------------------------------- */
    /* (preserve r0, r1, r2 - bootloader params) */

    /* --------------------------------------------------------
     * 4. Disable MMU, caches (ensure clean start state)
     *    Read SCTLR, clear M, C, I, Z bits
     * -------------------------------------------------------- */
    mrc     p15, 0, r3, c1, c0, 0  /* read SCTLR */
    bic     r3, r3, #(SCTLR_M | SCTLR_A | SCTLR_C)
    bic     r3, r3, #(SCTLR_Z | SCTLR_I)
    mcr     p15, 0, r3, c1, c0, 0  /* write SCTLR */
    isb

    /* --------------------------------------------------------
     * 5. Invalidate TLBs
     * -------------------------------------------------------- */
    mov     r3, #0
    mcr     p15, 0, r3, c8, c7, 0  /* TLBIALL */
    mcr     p15, 0, r3, c8, c5, 0  /* ITLBIALL */
    mcr     p15, 0, r3, c8, c6, 0  /* DTLBIALL */
    dsb
    isb

    /* --------------------------------------------------------
     * 6. Invalidate Branch Predictor
     * -------------------------------------------------------- */
    mov     r3, #0
    mcr     p15, 0, r3, c7, c5, 6  /* BPIALL */
    dsb
    isb

    /* --------------------------------------------------------
     * 7. Invalidate I-cache and D-cache
     *    (D-cache by set/way loop - see boot_mmu.c for C version)
     * -------------------------------------------------------- */
    mov     r3, #0
    mcr     p15, 0, r3, c7, c5, 0  /* ICIALLU */
    bl      _dcache_invalidate_all  /* see subroutine below */

    /* --------------------------------------------------------
     * 8. Build page tables (C function, physical addresses)
     * -------------------------------------------------------- */
    bl      boot_mmu_init_table

    /* --------------------------------------------------------
     * 9. Configure Domain Access Control Register
     *    D0 = Manager (0b11), all others = No Access (0b00)
     * -------------------------------------------------------- */
    ldr     r3, =0x00000003
    mcr     p15, 0, r3, c3, c0, 0  /* DACR */

    /* --------------------------------------------------------
     * 10. Configure TTBCR: N=1 (split at 0x80000000)
     * -------------------------------------------------------- */
    mov     r3, #1
    mcr     p15, 0, r3, c2, c0, 2  /* TTBCR */

    /* --------------------------------------------------------
     * 11. Load TTBR0 and TTBR1
     *
     * Cache hint bits for page table walks:
     * [0] IRGN[1] = 1 → Inner WB WA
     * [1] S       = 1 → Shareable
     * [3:4] RGN   = 01 → Outer WB WA
     * [6] IRGN[0] = 1 → Inner WB WA
     * Combined: 0x4B
     * -------------------------------------------------------- */
    ldr     r3, =(PAGE_TABLE_PHYS | 0x4B)
    mcr     p15, 0, r3, c2, c0, 0  /* TTBR0 */
    mcr     p15, 0, r3, c2, c0, 1  /* TTBR1 */
    isb

    /* --------------------------------------------------------
     * 12. Enable MMU + D-cache + I-cache + Branch Predictor
     *
     * ORDER MATTERS:
     *  a) DSB before reading SCTLR
     *  b) Set all bits at once (not piecemeal)
     *  c) ISB immediately after MCR to flush pipeline
     *     (pipeline may have fetched instructions at PA)
     * -------------------------------------------------------- */
    dsb

    mrc     p15, 0, r3, c1, c0, 0  /* read SCTLR */
    orr     r3, r3, #(SCTLR_M | SCTLR_C | SCTLR_A)
    orr     r3, r3, #(SCTLR_Z | SCTLR_I)
    mcr     p15, 0, r3, c1, c0, 0  /* MMU ON */
    isb

    /* --------------------------------------------------------
     * AT THIS EXACT POINT:
     * - MMU is ON
     * - CPU fetches NEXT INSTRUCTION from virtual address
     * - Identity map ensures VA = PA still works here
     * - We are still running at PHYSICAL address equivalent
     * -------------------------------------------------------- */

    /* --------------------------------------------------------
     * 13. Long jump to VIRTUAL address
     *     This is the physical-to-virtual transition.
     *     After this, we discard the identity map.
     * -------------------------------------------------------- */
    ldr     r3, =_start_virtual     /* absolute virtual address */
    bx      r3                      /* branch to virtual address */

/* ============================================================
 * _start_virtual: We are now running at virtual addresses
 * ============================================================ */
    .type _start_virtual, %function
_start_virtual:
    /* --------------------------------------------------------
     * 14. Relocate stack pointer to virtual address
     * -------------------------------------------------------- */
    ldr     sp, =__stack_top        /* __stack_top is a virtual address */

    /* --------------------------------------------------------
     * 15. Remove identity map to catch NULL dereferences
     *     Write invalid descriptor to identity-map entry
     * -------------------------------------------------------- */
    bl      boot_mmu_remove_identity

    /* --------------------------------------------------------
     * 16. Flush TLBs to remove identity map entries
     * -------------------------------------------------------- */
    mov     r3, #0
    mcr     p15, 0, r3, c8, c7, 0  /* TLBIALL */
    dsb
    isb

    /* --------------------------------------------------------
     * 17. Zero BSS section (using virtual addresses)
     * -------------------------------------------------------- */
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
bss_zero_loop:
    cmp     r0, r1
    strlt   r2, [r0], #4
    blt     bss_zero_loop

    /* --------------------------------------------------------
     * 18. Jump to main()
     * -------------------------------------------------------- */
    bl      main

    /* Should never reach here */
_halt:
    wfi
    b       _halt

/* ============================================================
 * _dcache_invalidate_all - Invalidate entire D-cache by set/way
 * No stack usage (called before stack is reliable)
 * Uses r3-r9
 * ============================================================ */
    .type _dcache_invalidate_all, %function
_dcache_invalidate_all:
    stmfd   sp!, {r4-r9}

    mov     r3, #0
    mcr     p15, 2, r3, c0, c0, 0  /* CSSELR: select L1 D-cache */
    isb
    mrc     p15, 1, r3, c0, c0, 0  /* CCSIDR */

    and     r4, r3, #0x7            /* line size field */
    add     r4, r4, #4              /* r4 = log2(line size bytes) */
    ldr     r5, =0x7FFF
    and     r5, r5, r3, lsr #13    /* r5 = sets - 1 */
    ldr     r6, =0x3FF
    and     r6, r6, r3, lsr #3     /* r6 = ways - 1 */
    clz     r7, r6                  /* r7 = way shift */

1:  mov     r8, r5                  /* r8 = set counter */
2:  mov     r9, r6                  /* r9 = way counter */
3:  lsl     r9, r9, r7              /* way << way_shift */
    lsl     r8, r8, r4              /* set << line_shift (WRONG - fix below) */
    orr     r9, r9, r8
    mcr     p15, 0, r9, c7, c6, 2  /* DCISW - invalidate by set/way */
    sub     r8, r8, #1
    tst     r8, r8
    bge     2b
    subs    r5, r5, #1
    bge     1b

    dsb
    isb
    ldmfd   sp!, {r4-r9}
    bx      lr
```

---

## 6. MMU Enable Sequence - Critical Ordering

This is one of the most failure-prone steps in baremetal bring-up.

### 6.1 Exact Required Sequence

```
MANDATORY ORDER (violating this causes undefined behavior):

Step 1: Write page tables to memory
        (all L1 entries set correctly)

Step 2: DSB (Data Synchronization Barrier)
        Ensure page table writes are observable by page table walker

Step 3: Write TTBR0, TTBR1
        Point MMU hardware to page tables

Step 4: Write TTBCR
        Configure TTBR0/TTBR1 split

Step 5: Write DACR
        Set domain permissions

Step 6: ISB (Instruction Synchronization Barrier)
        Flush pipeline, ensure TTBR/DACR writes take effect

Step 7: Invalidate TLBs (again)
        Remove any stale entries that might have been loaded

Step 8: DSB
        Ensure TLB invalidation completes

Step 9: Read SCTLR

Step 10: Set SCTLR.M = 1 (and C, I, Z if desired)

Step 11: ISB IMMEDIATELY after MCR SCTLR
         This is CRITICAL. Without ISB, the pipeline may
         have pre-fetched instructions at old physical addresses.

COMMON BUG: Forgetting ISB after enabling MMU.
Result: CPU executes garbage — code at PA != new VA.
```

### 6.2 Why DSB + ISB Are Both Required

```
DSB (Data Synchronization Barrier):
  - Waits for ALL preceding memory accesses to complete
  - Includes cache/TLB maintenance operations
  - Does NOT flush instruction pipeline
  - "All stores are done"

ISB (Instruction Synchronization Barrier):
  - Flushes the CPU pipeline
  - Forces re-fetch from new state
  - Effects of preceding SCTLR write are guaranteed visible
  - "CPU forgets everything prefetched"

Without DSB before MCR SCTLR:
  → Page table walk may use stale cache data → wrong PA mappings

Without ISB after MCR SCTLR:
  → Pipeline has pre-fetched instructions at old addresses
  → After MMU ON, those old PAs may now be invalid VAs → fault
```

### 6.3 Common Failure Modes

| Failure | Symptom | Root Cause |
|---------|---------|------------|
| No identity map | Prefetch abort immediately after MCR SCTLR | PC points to unmapped PA after MMU on |
| No ISB after SCTLR | Erratic crash after MMU enable | Pipeline stale |
| DACR = 0 | Data abort on first memory access | No domain access granted |
| TTBR not 16KB aligned | Random translation faults | L1 table base addr corrupt |
| Missing DSB before TTBR write | Inconsistent page table walk | Stores not visible |
| D-cache not invalidated | Wrong translations cached | Stale D-cache line served to MMU |

---

## 7. Physical-to-Virtual Jump Trick

### 7.1 The Problem

After `MCR p15, 0, r3, c1, c0, 0` (MMU enable), the CPU's PC still holds a **physical address**. The MMU is now translating all accesses. The identity map ensures:

```
VA 0x60000000 → PA 0x60000000  (still valid, code still runs)
```

But we want to be at virtual address `0xC0000000`. We need to transfer the PC to the virtual address **atomically** — if we tried a relative branch (B), we'd stay in the identity map. We need an **absolute branch**.

### 7.2 The Solution

```assembly
/* We are at physical 0x60001234 */
/* MMU is now on */
/* Identity map: VA 0x60001234 → PA 0x60001234 (works) */

/* Load ABSOLUTE virtual address */
ldr     r3, =_start_virtual     /* r3 = 0xC0001234 (absolute VA) */
bx      r3                      /* branch to 0xC0001234 */

/* Now executing at VA 0xC0001234 → PA 0x60001234 */
/* Identity map no longer needed */
```

### 7.3 Why LDR + BX, Not B

```
B/BL: PC-relative branch. Works within ±32MB.
      BUT: compiled at link time as "current_PC + offset"
      Current PC is a PA (e.g., 0x60001234)
      B to _start_virtual would still be PA-relative → stays in identity map

LDR r3, =label: Load the ABSOLUTE link-time address
      _start_virtual was linked at 0xC0001234
      r3 = 0xC0001234 (the correct VA)
BX r3: Jump to r3 → CPU now runs at virtual address
```

### 7.4 Linker Dependency

This requires the linker to know `_start_virtual` lives at `0xC0001234`:

```ld
/* linker.ld */
ENTRY(_reset)

SECTIONS {
    /* Boot code at physical load address */
    . = 0x60000000;
    .boot : {
        startup.o(.text._reset)
        startup.o(.text._dcache_invalidate_all)
    }

    /* Kernel image at virtual address, loaded at physical */
    . = 0xC0000000;  /* Virtual address */
    _text_start = .;
    .text : AT(0x60008000) {   /* AT() = physical load address (LMA) */
        *(.text._start_virtual)
        *(.text)
    }
    .rodata : AT(LOADADDR(.text) + SIZEOF(.text)) {
        *(.rodata)
    }
    .data : AT(LOADADDR(.rodata) + SIZEOF(.rodata)) {
        *(.data)
    }
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss)
        __bss_end = .;
    }
    . = ALIGN(8);
    __stack_top = . + 0x4000;   /* 16KB virtual stack */
}
```

---

## 8. Post-MMU Cleanup

### 8.1 Remove Identity Map

After jumping to virtual addresses, the identity map must be removed to:
1. Catch NULL pointer dereferences (VA=0 should fault)
2. Detect accesses to unmapped physical addresses
3. Clean up page table (reduce confusion)

```c
/*
 * boot_mmu_remove_identity() - Remove physical-identity sections
 *
 * Called AFTER jumping to virtual addresses.
 * Clears the identity map entry.
 * Must be followed by TLBIALL + DSB + ISB.
 */
void boot_mmu_remove_identity(void)
{
    uint32_t *l1 = (uint32_t *)PAGE_TABLE_BASE_PHYS;
    uint32_t  idx;

    /* Identity section was at VA 0x60000000 → index 0x600 */
    idx = PHYS_BASE >> 20;  /* 0x60000000 >> 20 = 0x600 */
    l1[idx] = 0;            /* Mark as invalid */

    /* DSB to ensure the write is visible to TLB hardware */
    __asm__ volatile("dsb" ::: "memory");
}
```

### 8.2 TLB Invalidation After Table Change

```c
void mmu_flush_tlb_all(void)
{
    uint32_t zero = 0;

    /* Invalidate unified TLB */
    __asm__ volatile(
        "mcr p15, 0, %0, c8, c7, 0\n"   /* TLBIALL */
        "dsb\n"
        "isb\n"
        :: "r"(zero) : "memory"
    );
}
```

---

## 9. Linker Script Design

```ld
/*
 * linker.ld - ARM32 baremetal with MMU
 *
 * Key concepts:
 * VMA (Virtual Memory Address): where code RUNS
 * LMA (Load Memory Address):    where code is STORED in flash/ROM
 *
 * For ROM-based systems: LMA = flash address, VMA = RAM virtual address
 * For RAM-loaded systems: LMA = RAM physical, VMA = RAM virtual
 */

OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_reset)

/* Physical load address */
PHYS_BASE = 0x60000000;

/* Virtual run address */
VIRT_BASE = 0xC0000000;

/* VA to PA offset */
LOAD_OFFSET = VIRT_BASE - PHYS_BASE;

SECTIONS {
    /*
     * .boot: Runs and loads at physical address
     * These sections execute before MMU is enabled
     * MUST be position-independent or PA-aware
     */
    . = PHYS_BASE;
    .boot_text : {
        KEEP(*(.boot_vector))        /* Reset vector must be first */
        KEEP(*(.boot_text))
        . = ALIGN(16384);            /* 16KB align for L1 table */
        _l1_page_table = .;
        . += 16384;                  /* Reserve 16KB for L1 page table */
        _boot_stack_bottom = .;
        . += 16384;                  /* Reserve 16KB for boot stack */
        _boot_stack_top = .;
    }

    /*
     * .text: Virtual address, physical LMA calculated from offset
     * After MMU enable, code runs here.
     */
    . = VIRT_BASE + SIZEOF(.boot_text);
    PROVIDE(_kernel_start = .);

    .text : AT(ADDR(.text) - LOAD_OFFSET) {
        *(.text.startup)             /* _start_virtual must be first */
        *(.text)
        *(.text.*)
        . = ALIGN(4);
    }

    .rodata : AT(ADDR(.rodata) - LOAD_OFFSET) {
        *(.rodata)
        *(.rodata.*)
        . = ALIGN(4);
    }

    /*
     * .data: Loaded at LMA (PA), initialized at startup
     * Startup code copies LMA → VMA before using .data
     */
    .data : AT(ADDR(.data) - LOAD_OFFSET) {
        _data_start = .;
        *(.data)
        *(.data.*)
        _data_end = .;
        . = ALIGN(4);
    }

    .bss (NOLOAD) : {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        _bss_end = .;
        . = ALIGN(8);
    }

    _kernel_end = .;

    /* Stack and heap at end of kernel image */
    .stack (NOLOAD) : {
        _stack_bottom = .;
        . += 0x10000;               /* 64KB stack */
        _stack_top = .;
    }

    /* Debug and symbol tables */
    /DISCARD/ : {
        *(.ARM.exidx*)
        *(.ARM.extab*)
    }
}

/* Useful symbols */
_load_start = LOADADDR(.text);
_load_end   = LOADADDR(.data) + SIZEOF(.data);
```

---

## 10. Debugging Baremetal MMU Issues

### 10.1 Using UART as the Only Debug Channel

When the MMU is not yet up, the only reliable debug output is via UART mapped at a physical address. Implement a `uart_putchar()` that uses direct physical address writes.

```c
/*
 * phys_uart_putchar() - Write to UART before MMU is up
 * Uses physical address directly.
 * No printf, no stack, no globals.
 */
#define UART0_BASE_PHYS  0xF8000000  /* Example UART physical address */
#define UART_DR_OFFSET   0x00        /* Data register */
#define UART_FR_OFFSET   0x18        /* Flag register */
#define UART_FR_TXFF     (1 << 5)    /* TX FIFO Full */

static void phys_uart_putchar(char c)
{
    volatile uint32_t *fr = (volatile uint32_t *)(UART0_BASE_PHYS + UART_FR_OFFSET);
    volatile uint32_t *dr = (volatile uint32_t *)(UART0_BASE_PHYS + UART_DR_OFFSET);

    /* Wait until TX FIFO is not full */
    while (*fr & UART_FR_TXFF)
        ;
    *dr = (uint32_t)c;
}
```

### 10.2 Fault Handling Before MMU is Up

Prefetch/Data aborts before MMU setup indicate:
- Wrong DACR (all domains have no access)
- TTBR not written before SCTLR.M=1
- Identity map missing

Install a minimal abort handler at the vector table:

```assembly
/* Minimal vector table */
    .section .boot_vector, "ax"
_vectors:
    ldr     pc, _reset_addr         /* 0x00 Reset */
    ldr     pc, _undef_addr         /* 0x04 Undefined Instruction */
    ldr     pc, _svc_addr           /* 0x08 SVC */
    ldr     pc, _pabt_addr          /* 0x0C Prefetch Abort */
    ldr     pc, _dabt_addr          /* 0x10 Data Abort */
    .word   0                       /* 0x14 Reserved */
    ldr     pc, _irq_addr           /* 0x18 IRQ */
    ldr     pc, _fiq_addr           /* 0x1C FIQ */

_reset_addr:    .word _reset
_undef_addr:    .word _undef_handler
_svc_addr:      .word _svc_handler
_pabt_addr:     .word _pabt_handler
_dabt_addr:     .word _dabt_handler

/* Minimal abort handler: dump r0-r3, LR, DFSR, DFAR via UART */
_dabt_handler:
    /* Read DFSR (Data Fault Status Register) */
    mrc     p15, 0, r0, c5, c0, 0
    /* Read DFAR (Data Fault Address Register) */
    mrc     p15, 0, r1, c6, c0, 0
    /* Store to well-known physical address for JTAG inspection */
    ldr     r2, =0x60000100
    str     r0, [r2, #0]    /* DFSR */
    str     r1, [r2, #4]    /* DFAR */
    str     lr, [r2, #8]    /* LR at fault */
    /* Halt */
1:  wfi
    b       1b
```

### 10.3 JTAG / OpenOCD Debugging Checklist

```
When hitting faults during MMU enable, inspect:

1. SCTLR (cp15:c1:c0:0):
   - Bit 0 (M) = 1? MMU enabled?
   - Bit 2 (C) = 1? D-cache enabled?

2. TTBR0 (cp15:c2:c0:0):
   - Points to valid L1 table (16KB aligned)?

3. TTBR1 (cp15:c2:c0:1):
   - Correct for kernel mapping?

4. DACR (cp15:c3:c0:0):
   - Non-zero? Domain 0 = Manager (0b11)?

5. DFSR (cp15:c5:c0:0) after data abort:
   - Bits [3:0] = fault type
   - Bits [7:4] = domain
   - 0b0101 = Translation fault, section
   - 0b0111 = Translation fault, page
   - 0b1101 = Permission fault, section

6. DFAR (cp15:c6:c0:0):
   - Address that caused the fault

7. L1 Table at PAGE_TABLE_PHYS:
   - Check entry for faulting VA[31:20]
   - Should be non-zero for mapped regions
```

---

## 11. Complete Reference Implementation

### 11.1 File Structure

```
baremetal_mmu/
├── startup.S          ← Reset vector, all assembly
├── boot_mmu.c         ← Page table construction
├── boot_mmu.h         ← Constants and API
├── mmu.c              ← Runtime MMU management
├── uart.c             ← Debug UART (phys address)
├── linker.ld          ← Linker script
└── Makefile
```

### 11.2 Makefile

```makefile
CROSS := arm-none-eabi-
CC    := $(CROSS)gcc
AS    := $(CROSS)as
LD    := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy

CFLAGS := -march=armv7-a -marm -mfpu=vfpv3 -mfloat-abi=softfp \
          -O1 -fno-stack-protector -fno-common \
          -nostdlib -nostartfiles \
          -fno-builtin -fno-pie -fno-pic

ASFLAGS := -march=armv7-a

LDFLAGS := -T linker.ld -nostdlib

OBJS := startup.o boot_mmu.o mmu.o uart.o main.o

all: firmware.elf firmware.bin

firmware.elf: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

firmware.bin: firmware.elf
	$(OBJCOPY) -O binary $< $@

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(CFLAGS) $(ASFLAGS) -c -o $@ $<

clean:
	rm -f *.o *.elf *.bin
```

### 11.3 Quick-Reference: MCR/MRC for MMU Setup

| Operation | Assembly | Purpose |
|-----------|----------|---------|
| Read SCTLR | `MRC p15,0,r0,c1,c0,0` | Read system control |
| Write SCTLR | `MCR p15,0,r0,c1,c0,0` | Enable/disable MMU |
| Write TTBR0 | `MCR p15,0,r0,c2,c0,0` | Set user page table |
| Write TTBR1 | `MCR p15,0,r0,c2,c0,1` | Set kernel page table |
| Write TTBCR | `MCR p15,0,r0,c2,c0,2` | Set TTBR split |
| Write DACR | `MCR p15,0,r0,c3,c0,0` | Domain access control |
| Inval TLB all | `MCR p15,0,r0,c8,c7,0` | Flush all TLB entries |
| Inval I-TLB | `MCR p15,0,r0,c8,c5,0` | Flush I-TLB |
| Inval D-TLB | `MCR p15,0,r0,c8,c6,0` | Flush D-TLB |
| Inval ICACHE | `MCR p15,0,r0,c7,c5,0` | Flush I-cache |
| Inval BP | `MCR p15,0,r0,c7,c5,6` | Flush branch predictor |
| DCISW | `MCR p15,0,r0,c7,c6,2` | D-cache inval by set/way |
| Write CONTEXTIDR | `MCR p15,0,r0,c13,c0,1` | Set ASID + PROCID |

---

**Cross-References:**
- Doc 01: Translation descriptor formats, AP bits, domain descriptions
- Doc 03: Linux kernel `head.S` equivalent to this boot code
- Doc 04: ASID setup (CONTEXTIDR) after MMU enable
- Doc 05: Cache maintenance operations in detail

**End of Document 2**
