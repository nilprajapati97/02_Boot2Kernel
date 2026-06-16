# `primary_entry()` — Kernel Entry Point

**Source:** `arch/arm64/kernel/head.S` lines 82–124

**Phase:** Assembly Boot (MMU OFF)

**Memory Allocator:** None

**Called by:** Bootloader (direct jump)

**Calls:** `record_mmu_state()`, `preserve_boot_args()`, `__pi_create_init_idmap()`, `init_kernel_el()`, `__cpu_setup()`, `__primary_switch()`

---

## What This Function Does

`primary_entry` is the **very first instruction** the Linux kernel executes on the boot CPU. When the bootloader (U-Boot, UEFI, etc.) jumps here:

- **MMU is OFF** — all addresses are physical
- **D-cache is OFF** — no data caching
- **x0** contains the physical address of the Device Tree Blob (DTB)
- No stack, no allocator, no C runtime

This function sets up the minimal environment needed to enable the MMU and transition to virtual addressing.

---

## How It Works With Memory

### Memory It Uses (All Static)

| Memory Region | Location | Size | Purpose |
|---------------|----------|------|---------|
| `early_init_stack` | BSS | ~16 KB | Temporary stack for early boot functions |
| `init_idmap_pg_dir` | BSS | 4 KB + PTE pages | Identity-mapped page tables |
| `boot_args[4]` | BSS | 32 bytes | Storage for preserved boot arguments |

**No dynamic allocation.** All memory used is statically allocated within the kernel image's BSS section. The linker script (`vmlinux.lds`) places these symbols at known offsets.

### Why BSS?

BSS (Block Started by Symbol) is the zero-initialized data section. It doesn't consume space in the kernel image file — only in RAM after loading. This is ideal for page tables that need to be zeroed before use.

---

## Step-by-Step Execution

### Step 1: `record_mmu_state()` — Save Boot MMU State

```asm
bl  record_mmu_state
```

**What it does:**
- Reads `SCTLR_EL1` (or `SCTLR_EL2` depending on current EL)
- Stores the MMU-on/off state in register **x19**
- x19 = 0 means bootloader left MMU **off** (normal case)
- x19 ≠ 0 means bootloader left MMU **on** (EFI stub case)

**Why it matters for memory:**
- If MMU was off: data cache is invalid, must be careful with memory accesses
- If MMU was on: caches contain valid data, need to clean (flush) before changing page tables

### Step 2: `preserve_boot_args()` — Save DTB Pointer

```asm
bl  preserve_boot_args
```

**What it does:**
- Saves registers x0-x3 to the `boot_args[]` array in BSS
- **x0** = DTB physical address → saved to x21 (and boot_args[0])
- x1, x2, x3 = reserved by boot protocol

**Memory written:** `boot_args[4]` — 4 × 8 bytes = 32 bytes in BSS

**Why it matters:**
- The DTB pointer is critical — it contains the `/memory` nodes that tell the kernel where RAM exists
- x21 carries this pointer through the entire boot sequence until `setup_machine_fdt()` reads it

### Step 3: Set Up Early Stack

```asm
adrp  x1, early_init_stack
mov   sp, x1
mov   x29, xzr                  // Clear frame pointer (no previous frame)
```

**What it does:**
- Points the stack pointer (SP) to `early_init_stack` in BSS
- This gives subsequent function calls (bl instructions) a place to save return addresses and local variables
- Frame pointer (x29) is zeroed since there's no caller frame

**Memory used:** `early_init_stack` — a small BSS region (~16 KB)

**Why not init_task's stack?**
- `init_task`'s stack is at a virtual address that isn't mapped yet (MMU is off)
- `early_init_stack` is in the identity-mapped region where physical = virtual

### Step 4: `__pi_create_init_idmap()` — Create Identity Page Tables

```asm
adrp  x0, __pi_init_idmap_pg_dir    // Page table base address
mov   x1, xzr                        // Clear mask = 0
bl    __pi_create_init_idmap          // Create identity mapping
```

**See:** [02_create_init_idmap.md](02_create_init_idmap.md) for full details.

**Summary:** Creates page tables where `virtual address == physical address` for the kernel image. This is needed so that when the MMU is turned on, the currently executing code (at physical addresses) is still accessible.

### Step 5: Cache Maintenance

```asm
cbnz  x19, 0f          // If MMU was on, skip to clean path

// MMU was OFF path:
dmb   sy                // Data Memory Barrier
mov   x1, #0x3 << 1     // DC CIVAC (Clean+Invalidate by VA to PoC)
// ... invalidate D-cache by set/way ...
b     1f

0:  // MMU was ON path:
adrp  x0, __idmap_text_start
adr_l x1, __idmap_text_end
adr_l x2, dcache_clean_poc
blr   x2                // Clean cache for identity-mapped code
```

**What it does:**
- **MMU-off boot:** Invalidates the entire D-cache. Data cache is unreliable since bootloader may have left stale data. The newly created page tables in BSS need to be visible to the MMU's page table walker (which reads from memory, not cache).
- **MMU-on boot (EFI):** Cleans the cache for the identity-mapped text region only. Cache is coherent, just need to ensure page table changes are visible.

**Why it matters for memory:**
The ARM64 MMU's **page table walker reads from memory** (or caches, depending on TCR settings). If stale data is in the cache, the walker may see garbage page table entries. This step ensures the identity-mapped page tables created in Step 4 are visible.

### Step 6: `init_kernel_el()` — Configure Exception Level

```asm
mov   x0, x19               // x0 = MMU state from boot
bl    init_kernel_el
mov   x20, x0               // x20 = boot mode (EL1 or EL2)
```

**What it does:**
- Detects current Exception Level (EL2 or EL1)
- If at EL2 (hypervisor level):
  - Configures EL2 registers for KVM
  - Sets up HCR_EL2 for virtualization
  - Drops down to EL1 (kernel level) via `eret`
- If already at EL1: minimal setup

**Return value:** x0 = `BOOT_CPU_MODE_EL1` or `BOOT_CPU_MODE_EL2`

**Memory impact:** None directly. But EL level affects which translation regime (TTBR0_EL1/TTBR1_EL1 vs TTBR0_EL2) is used.

### Step 7: `__cpu_setup()` — Configure MMU Registers

```asm
bl    __cpu_setup
```

**See:** [03_cpu_setup.md](03_cpu_setup.md) for full details.

**Summary:** Configures MAIR_EL1 (memory types), TCR_EL1 (translation control), and prepares SCTLR_EL1 value (returned in x0). No memory allocation — register configuration only.

### Step 8: `__primary_switch()` — Enable MMU

```asm
b     __primary_switch
```

**See:** [04_enable_mmu_primary_switch.md](04_enable_mmu_primary_switch.md) for full details.

**Summary:** Loads TTBR0/TTBR1 with page table base addresses, sets SCTLR_EL1.M=1 to enable the MMU, maps the kernel at its link address, and jumps to `__primary_switched()` at a **virtual address**. From this point, all code runs in virtual address space.

---

## Register State Summary

| Register | After primary_entry | Purpose |
|----------|-------------------|---------|
| x19 | SCTLR boot state | 0 = MMU was off, ≠0 = MMU was on |
| x20 | Boot CPU mode | EL1 or EL2 |
| x21 | DTB physical addr | Device Tree Blob location |
| SP | `early_init_stack` | Temporary boot stack |
| x29 | 0 | Frame pointer (no caller) |

---

## Memory Layout at This Point

```
Physical Memory:
┌─────────────────────────┐ High
│                         │
│    Free RAM (unknown)   │ ← Kernel doesn't know about this yet
│                         │
├─────────────────────────┤
│    Device Tree (DTB)    │ ← x21 points here
├─────────────────────────┤
│                         │
│    Kernel Image         │
│    ├── .text            │ ← Executing from here (physical)
│    ├── .rodata          │
│    ├── .data            │
│    └── .bss             │
│        ├── init_idmap_pg_dir  ← Identity page tables (just created)
│        ├── early_init_stack   ← Current stack
│        ├── boot_args[4]       ← Saved x0-x3
│        └── init_pg_dir        ← Will be used soon
│                         │
├─────────────────────────┤
│    Bootloader / FW      │
└─────────────────────────┘ 0x0
```

---

## Key Takeaways

1. **No memory allocator exists** — everything is statically placed by the linker
2. **Identity mapping is essential** — when MMU turns on, the CPU needs to find the next instruction at the same address
3. **Cache maintenance is critical** — page table walker reads from memory, not CPU cache
4. **DTB pointer (x21) is the key to everything** — without it, the kernel can't discover RAM
5. **BSS is the only "allocator"** — the linker script reserves space for page tables, stacks, and boot args
