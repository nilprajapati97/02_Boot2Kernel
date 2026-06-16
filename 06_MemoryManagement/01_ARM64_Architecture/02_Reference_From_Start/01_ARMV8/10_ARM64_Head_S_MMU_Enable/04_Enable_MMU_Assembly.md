# `__enable_mmu` — Assembly Deep Dive

## 1. Where It Lives

```
File:     arch/arm64/kernel/head.S
Function: __enable_mmu
Called by: __primary_entry (after page tables and system registers are set up)
```

---

## 2. Complete Call Chain in `head.S`

```
_start / primary_entry
    │
    ├── preserve_boot_args()       // save x0-x3 (DTB pointer etc.)
    │
    ├── init_kernel_el()           // set up EL1 if booted at EL2/EL3
    │     └── el2_setup()
    │
    ├── set_cpu_boot_mode_flag()
    │
    ├── __create_page_tables()     // build init_pg_dir
    │
    ├── __cpu_setup()              // configure MAIR_EL1, TCR_EL1
    │     └── proc.S: cpu_do_idle, MAIR write, TCR write
    │
    └── __primary_switch()
          │
          ├── adrp  x0, init_pg_dir    // TTBR1 = kernel page table
          ├── adrp  x1, idmap_pg_dir   // TTBR0 = identity map
          │
          └── __enable_mmu()           ◄── MMU ENABLE HERE
                └── br __primary_switched  // jump to virtual address
```

---

## 3. `__cpu_setup` (proc.S) — Before MMU Enable

`__cpu_setup` runs just before `__enable_mmu`. It configures all
system registers **except** the TTBRs and SCTLR.

```asm
// arch/arm64/mm/proc.S
SYM_FUNC_START(__cpu_setup)
    tlbi    vmalle1             // invalidate all TLB entries at EL1
    dsb     nsh                 // data sync barrier (non-shareable)

    mov     x0, #3 << 20       // RES1 bits in SCTLR_EL1
    msr     SCTLR_EL1, x0      // minimal init of SCTLR (MMU still off)
    isb

    // Set MAIR_EL1
    mov_q   x5, MAIR_EL1_SET
    msr     MAIR_EL1, x5

    // Set TCR_EL1
    mrs     x10, ID_AA64MMFR0_EL1    // read PA size support
    ...                               // compute IPS bits
    mov_q   x9,  TCR_T0SZ(VA_BITS)  | TCR_T1SZ(VA_BITS) | \
                 TCR_TG0_4K          | TCR_TG1_4K         | \
                 TCR_IRGN0_WBWA      | TCR_IRGN1_WBWA     | \
                 TCR_ORGN0_WBWA      | TCR_ORGN1_WBWA     | \
                 TCR_SH0_INNER       | TCR_SH1_INNER
    orr     x9, x9, x10             // merge IPS
    msr     TCR_EL1, x9
    isb

    ret                              // return to caller (head.S)
SYM_FUNC_END(__cpu_setup)
```

---

## 4. `__enable_mmu` — Annotated Assembly

```asm
// arch/arm64/kernel/head.S
SYM_FUNC_START_LOCAL(__enable_mmu)
    /*
     * x0 = TTBR1 value (init_pg_dir physical address)
     * x1 = TTBR0 value (idmap_pg_dir physical address)
     */

    // Step 1: Load TTBR0 and TTBR1
    msr     TTBR0_EL1, x1       // identity map → lower VA
    msr     TTBR1_EL1, x0       // kernel map   → upper VA
    isb                          // ensure TTBRs are committed

    // Step 2: Read current SCTLR_EL1
    mrs     x0, SCTLR_EL1

    // Step 3: Clear any undesired bits (implementation-defined bits)
    mov_q   x1, SCTLR_EL1_SET   // mask of bits to set
    orr     x0, x0, x1

    // Step 4: Actually enable the MMU
    // SCTLR_ELx_M  = bit 0  → MMU enable
    // SCTLR_ELx_C  = bit 2  → Data cache enable
    // SCTLR_ELx_I  = bit 12 → Instruction cache enable
    msr     SCTLR_EL1, x0

    // Step 5: MANDATORY instruction synchronization barrier
    isb

    // At this point:
    // - MMU is ON
    // - PC is still at a physical/identity-mapped address
    // - The identity map covers this code, so no fault
    // - Next we must jump to the high VA kernel

    ret                          // return to __primary_switch
SYM_FUNC_END(__enable_mmu)
```

### After `__enable_mmu` Returns

The return address in `lr` (link register) is a **virtual address** in
the upper range (`0xFFFF...`), placed there by the caller.

```asm
// In __primary_switch (caller of __enable_mmu):
adr_l   lr, __primary_switched   // lr = absolute VA of __primary_switched
b       __enable_mmu              // branch (lr is now the VA return address)

// When __enable_mmu does `ret`, it jumps to __primary_switched at 0xFFFF...
```

---

## 5. The MMU-On Instant — Cycle by Cycle

```
Cycle N:   msr SCTLR_EL1, x0   ← write to SCTLR
Cycle N+1: isb                  ← pipeline flush; SCTLR change takes effect
Cycle N+2: ret                  ← fetch from lr (0xFFFF...); MMU is now ON
                                   TLB lookup → hits init_pg_dir → translates to PA
```

If the `isb` were missing:
```
Cycle N+1: ret   ← CPU might still be using old translation state → UNPREDICTABLE
```

---

## 6. Why `ret` Works After MMU Enable

The `lr` (link register / x30) was loaded with the virtual address of
`__primary_switched` **before** `__enable_mmu` was called.

When `ret` executes (after `isb`), the MMU is on and translates:
```
VA 0xFFFF000000082xxx  →  init_pg_dir  →  PA of __primary_switched code
```

This works because `__create_page_tables` mapped the entire kernel image
in `init_pg_dir` before we ever called `__enable_mmu`.

---

## 7. `__primary_switched` — First Code at Virtual Address

```asm
SYM_FUNC_START_LOCAL(__primary_switched)
    /*
     * We are now running at the virtual address of the kernel.
     * The CPU is using init_pg_dir for translation.
     * TTBR0_EL1 still points to idmap_pg_dir.
     */

    adrp    x0, init_thread_union    // set up initial stack pointer
    add     sp, x0, #THREAD_SIZE

    adrp    x1, __bss_start          // clear BSS
    adrp    x2, __bss_stop
    bl      __pi_memset

    mov_q   x0, INIT_SCTLR_EL1_MMU_ON    // set remaining SCTLR bits
    msr     SCTLR_EL1, x0
    isb

    bl      start_kernel             // ← enter C code
SYM_FUNC_END(__primary_switched)
```

---

## 8. TLB Invalidation Before MMU Enable

Before loading `TTBR0_EL1` and `TTBR1_EL1`, the kernel invalidates
all existing TLB entries:

```asm
// In __cpu_setup:
tlbi    vmalle1             // invalidate all non-hyp EL1 TLB entries
dsb     nsh                 // ensure tlbi completes before continuing
isb
```

This is essential because:
1. If CPU was previously running (e.g., after a warm reset), stale TLB entries could cause wrong translations
2. The new page tables must not be overridden by cached old entries

---

## 9. Secondary CPU MMU Enable (SMP)

Each secondary CPU (non-boot CPUs) follows a similar path through
`secondary_entry` in `head.S`:

```
secondary_entry
    │
    ├── __cpu_setup()          // configure MAIR, TCR (same as primary)
    │
    └── __secondary_switched()
          │
          ├── __enable_mmu()   // enable MMU
          │     └── uses swapper_pg_dir (not init_pg_dir)
          │
          └── secondary_start_kernel()  // C code for secondary
```

Secondary CPUs use `swapper_pg_dir` directly (permanent tables are
already in place by the time secondary CPUs start).

---

## 10. Boot-Time MMU Enable Checklist

```
✓ idmap_pg_dir built — covers __enable_mmu code region (PA == VA)
✓ init_pg_dir built  — covers kernel image at KIMAGE_VADDR
✓ MAIR_EL1 loaded    — memory type indices defined
✓ TCR_EL1 loaded     — VA sizes, granule, cache policy set
✓ TLB invalidated    — no stale entries
✓ TTBR0_EL1 = idmap_pg_dir
✓ TTBR1_EL1 = init_pg_dir
✓ SCTLR_EL1.M = 1   — MMU ON
✓ isb                — pipeline flush
✓ ret to VA          — jump to __primary_switched at 0xFFFF...
```
