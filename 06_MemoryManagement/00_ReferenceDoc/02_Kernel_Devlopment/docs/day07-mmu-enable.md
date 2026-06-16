# Day 07 — Enable MMU & Higher-Half Jump

> **Goal**: Load `MAIR_EL1`/`TCR_EL1`/`TTBR0`/`TTBR1`, enable the MMU (`SCTLR_EL1.M=1`), and jump from physical PC `0x4008_xxxx` to virtual PC `0xFFFF_0000_4008_xxxx`. Then unmap the identity range.
>
> **Why today**: The kernel must run from a fixed high VA so that user processes (later) can keep TTBR0 to themselves. Doing the switch right after table construction (Day 6) keeps the change atomic and debuggable.

---

## 1. Background

### 1.1 The "MMU-on" transition is delicate
- The instruction immediately after `msr SCTLR_EL1, ...; isb` runs through translation.
- That instruction's VA must map to a valid PTE pointing at the same PA where the CPU was about to fetch it (i.e., identity mapping covers `_start..mmu_on_label`).
- After the jump to virtual, we can tear down the identity map.

### 1.2 Required barriers
```
dsb sy   ; complete previous data accesses + TLB writes
tlbi vmalle1is ; invalidate all TLBs inner-shareable
dsb sy
isb      ; synchronize context (mandatory after SCTLR write)
```

### 1.3 Higher-half link
We **link** the kernel at `0xFFFF_0000_4008_0000` (Day 6 mapped TTBR1 L1[1] → PA `0x4000_0000`, so VA `0xFFFF_0000_4008_0000` resolves to PA `0x4008_0000`).

Update `linker.ld`:
```ld
. = 0xFFFF000040080000;
__kernel_virt_base = .;
```
Then the toolchain emits all symbol references at high addresses. The image is still **loaded** at PA `0x4008_0000` because we pass `--image-base=0x40080000` to `ld`, or use a load-address transform.

The simplest robust pattern: keep `_start` and the MMU-enable function PIC (uses `adrp` which is PC-relative), so they work at both PAs. After `eret`/branch to virtual, all subsequent code uses absolute (large model) addresses naturally.

---

## 2. Design

```
mmu_init (PA)
  ├─ build MAIR
  ├─ build TCR
  ├─ msr TTBR0_EL1, &ttbr0_l0
  ├─ msr TTBR1_EL1, &ttbr1_l0
  ├─ dsb/tlbi/dsb/isb
  ├─ msr SCTLR_EL1, (read | M | C | I)
  ├─ isb
  └─ ldr x0, =virt_entry  ; absolute high addr
     br  x0

virt_entry: (VA)
  ├─ adjust SP into high half
  ├─ unmap TTBR0 identity (zero L0[0])
  ├─ tlbi vmalle1is
  └─ bl  kmain_virt
```

---

## 3. Implementation

### 3.1 Updated `linker.ld`
```ld
OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

KERNEL_LMA = 0x40080000;
KERNEL_VMA = 0xFFFF000040080000;

SECTIONS
{
    . = KERNEL_VMA;
    __kernel_start = .;

    .text : AT(KERNEL_LMA) ALIGN(4096) {
        KEEP(*(.text.boot))
        *(.text .text.*)
    }
    .rodata : ALIGN(4096) { *(.rodata .rodata.*) }
    .data   : ALIGN(4096) { *(.data .data.*) }
    .bss    : ALIGN(4096) {
        __bss_start = .;
        *(.bss .bss.*) *(COMMON)
        __bss_end = .;
    }

    . = ALIGN(4096);
    __stack_bottom = .;
    . += 0x4000;
    __stack_top = .;
    __kernel_end = .;
}
```
With `AT(KERNEL_LMA)`, sections **live at PA 0x40080000** but **link at VA 0xFFFF…**. QEMU loads them at LMA; `_start` runs from PA.

### 3.2 Position-independent early boot helpers
Use `adrp`/`add :lo12:` instead of `ldr =symbol` until MMU is on. Provide a helper:
```asm
.macro adr_l, reg, sym
    adrp \reg, \sym
    add  \reg, \reg, :lo12:\sym
.endm
```

### 3.3 `arch/arm64/mm/mmu_enable.S`
```asm
#include "asm-offsets.h"

    .global mmu_enable
mmu_enable:
    /* x0 = MAIR, x1 = TCR, x2 = ttbr0 PA, x3 = ttbr1 PA, x4 = virt_entry */
    msr     mair_el1, x0
    msr     tcr_el1,  x1
    msr     ttbr0_el1, x2
    msr     ttbr1_el1, x3
    dsb     sy
    tlbi    vmalle1is
    dsb     sy
    isb

    mrs     x5, sctlr_el1
    orr     x5, x5, #(1 << 0)     /* M : MMU enable */
    orr     x5, x5, #(1 << 2)     /* C : D-cache    */
    orr     x5, x5, #(1 << 12)    /* I : I-cache    */
    msr     sctlr_el1, x5
    isb

    br      x4                     /* leap to virtual */
```

### 3.4 C glue in `mmu.c`
```c
void mmu_init(void)
{
    mmu_build_tables();          /* Day 6 */
    extern void mmu_enable(u64 mair, u64 tcr, u64 ttbr0, u64 ttbr1, u64 virt);
    extern void kmain_virt(void);

    u64 ttbr0_pa = (u64)ttbr0_l0;  /* still phys before MMU */
    u64 ttbr1_pa = (u64)ttbr1_l0;
    mmu_enable(build_mair(), build_tcr(), ttbr0_pa, ttbr1_pa,
               (u64)&kmain_virt);
    /* unreachable */
}

void kmain_virt(void)
{
    /* Rebase SP into high half */
    extern char __stack_top[];
    asm volatile("mov sp, %0" :: "r"(__stack_top));

    /* Tear down identity map */
    ttbr0_l0[0] = 0;
    asm volatile("dsb sy; tlbi vmalle1is; dsb sy; isb");

    printk(KERN_INFO "MMU on, running at %p\n", &kmain_virt);
    /* continue boot from here: gic_init, timer_init, etc. */
    kmain_post_mmu();
}
```

### 3.5 New early call order in `kmain`
```c
void kmain(u64 dtb_phys)
{
    uart_init();                  /* PA mapping still valid (identity) */
    fdt_init(dtb_phys);
    setup_vbar();                 /* exception table */
    mmu_init();                   /* never returns; jumps to kmain_virt */
}
```

> Note: until MMU on, calls to `printk`/`uart_putc` use identity-mapped PAs. After MMU, the same code at high VA still resolves to the same UART PA because Day 6 mapped `0x0900_0000` (within first 1 GiB) under both TTBR0 and the kernel identity-equivalent. Verify by checking the L1 entry covers `0x0000_0000–0x4000_0000`.

---

## 4. ARM64 Cheat-Sheet (Day 7)

```
SCTLR_EL1.M  bit 0   MMU enable
SCTLR_EL1.A  bit 1   alignment check
SCTLR_EL1.C  bit 2   D-cache enable
SCTLR_EL1.I  bit 12  I-cache enable
SCTLR_EL1.WXN bit 19 write-implies-no-exec
TLBI VMALLE1IS       invalidate stage-1 TLBs, all ASIDs, inner-shareable
TLBI ASIDE1IS, Xn    by ASID
```

---

## 5. Pitfalls

1. **MMU-on instruction not mapped**: classic OSDev gotcha. Ensure your identity map covers `_start..end of mmu_enable`. Easiest: map first 1 GiB as Normal RWX, since kernel image is at 0x40080000.
2. **`br x4` to high VA must use absolute load**: `ldr x4, =kmain_virt` works because linker resolves the constant pool address; `adr x4, kmain_virt` would be PC-relative and stay in low half. Use the `ldr =sym` form.
3. **Stack still pointing to PA `__stack_top`**: must rebase to high VA right after entering virtual.
4. **Cache state crossing MMU enable**: enable D-cache *with* MMU; before that, ensure all early data writes hit memory via `dc cvac` if necessary (rare for our small footprint).
5. **`MAIR/TCR` mismatch between cores (SMP)**: each CPU must execute the same enable sequence — defer to Day 28.

---

## 6. Verification (Phase 2 milestone — partial)

```
make run
# Expect:
# [INFO] mmu: tables built, …
# [INFO] MMU on, running at 0xffff000040080xxx
# [INFO] tick 100 (1s)  ... (timer keeps firing through MMU transition)
```

GDB:
```
(gdb) hb kmain_virt
(gdb) c
(gdb) info reg pc           # 0xffff000040080xxx
(gdb) p/x $sctlr_el1        # bit 0 set
(gdb) x/4xg ttbr0_l0        # entry 0 should be 0 after teardown
```

---

## 7. Stretch

- Use **2 MiB block mappings at L2** so MMIO (e.g. UART page) is Device while surrounding RAM is Normal.
- Add a `phys_to_virt(pa)` macro: `((pa) + 0xffff000000000000UL)` and `virt_to_phys(va)` inverse — used by all later subsystems.
- Set `SCTLR_EL1.WXN` to enforce no W+X mappings (Day 29 hardening prep).

---

## 8. References

- ARM ARM §D5 (VMSAv8-64), §D13.2 (`SCTLR_EL1`).
- Linux `arch/arm64/kernel/head.S` `__cpu_setup`, `__primary_switch`.
- OSDev Wiki: "ARMv8-A Address Translation".
