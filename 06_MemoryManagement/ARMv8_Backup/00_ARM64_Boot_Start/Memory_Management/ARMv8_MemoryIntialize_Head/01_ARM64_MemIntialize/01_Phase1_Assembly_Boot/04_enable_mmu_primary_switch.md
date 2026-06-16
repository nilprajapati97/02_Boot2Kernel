# `__primary_switch()` & `__enable_mmu()` — MMU Enable and Virtual Address Transition

**Source:** `arch/arm64/kernel/head.S` lines 508–524
**Phase:** Assembly Boot (MMU OFF → MMU ON)
**Memory Allocator:** None
**Called by:** `primary_entry()` (tail call via `b __primary_switch`)
**Calls:** `__enable_mmu()`, `__pi_early_map_kernel()`, jumps to `__primary_switched()`

---

## What This Function Does

This is the **moment the MMU turns on**. The function:

1. Loads page table base addresses into TTBR0/TTBR1
2. Writes SCTLR_EL1.M=1 to enable the MMU
3. Maps the kernel image at its link (virtual) address
4. Jumps from an identity-mapped (phys==virt) address to a **virtual address**

After this function, the CPU is permanently in virtual address mode.

---

## How It Works With Memory

### Page Tables Used

| Register | Page Table | Contains | Used For |
|----------|-----------|----------|----------|
| TTBR0_EL1 | `init_idmap_pg_dir` | Identity mapping (phys==virt) | Current code execution |
| TTBR1_EL1 | `reserved_pg_dir` → then `init_pg_dir` | Kernel mapping (virt≠phys) | Kernel virtual addresses |

### Memory Created

| What | Where | How |
|------|-------|-----|
| Kernel virtual mapping | `init_pg_dir` | `__pi_early_map_kernel()` creates PGD→PTE entries |

---

## Step-by-Step Execution

### Step 1: `__primary_switch()` Entry

```asm
SYM_FUNC_START_LOCAL(__primary_switch)
    adrp  x1, reserved_pg_dir       // Empty page directory (all zeros)
    adrp  x2, __pi_init_idmap_pg_dir // Identity-mapped page tables
    bl    __enable_mmu               // >>> ENABLE MMU <<<
```

**x1** = `reserved_pg_dir` — a zeroed-out page directory. Loaded into TTBR1 temporarily. Since it has no valid entries, any access to kernel virtual addresses (0xFFFF...) will fault — this is intentional as a safety measure until the real kernel mapping is created.

**x2** = `init_idmap_pg_dir` — the identity mapping created by `create_init_idmap()`. Loaded into TTBR0 so the currently executing code remains accessible.

---

### Step 2: `__enable_mmu()` — The Critical Moment

```asm
SYM_FUNC_START(__enable_mmu)
    mrs   x3, ID_AA64MMFR0_EL1
    ubfx  x3, x3, #ID_AA64MMFR0_EL1_TGRAN_SHIFT, 4
    cmp   x3, #ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MIN
    b.lt  __no_granule_support      // Panic if page granule not supported

    phys_to_ttbr x2, x2            // Convert physical addr to TTBR format
    msr   ttbr0_el1, x2            // TTBR0 = identity map page tables
    phys_to_ttbr x1, x1
    msr   ttbr1_el1, x1            // TTBR1 = reserved (empty) page tables

    isb                             // Instruction Synchronization Barrier
    msr   sctlr_el1, x0            // >>> SCTLR.M = 1: MMU IS NOW ON <<<
    isb                             // Ensure MMU is active before next instruction

    ret                             // Return to __primary_switch
SYM_FUNC_END(__enable_mmu)
```

### The Critical Sequence Explained

```
1. msr ttbr0_el1, x2     ← Load identity map into TTBR0
                             (Lower VA range: 0x0000_xxxx_xxxx_xxxx)
                             Now MMU can translate identity-mapped addresses

2. msr ttbr1_el1, x1     ← Load reserved_pg_dir into TTBR1
                             (Upper VA range: 0xFFFF_xxxx_xxxx_xxxx)
                             Kernel VAs will fault — this is temporary

3. isb                    ← Synchronization barrier
                             Ensures TTBR writes complete before MMU enable

4. msr sctlr_el1, x0     ← WRITE SCTLR_EL1 WITH M=1
                             ┌─────────────────────────────────────┐
                             │  THIS INSTRUCTION ENABLES THE MMU   │
                             │  Next instruction fetch goes through │
                             │  the translation tables              │
                             └─────────────────────────────────────┘

5. isb                    ← Ensure all subsequent instructions see
                             the new MMU configuration
```

### Why This Doesn't Crash

After `msr sctlr_el1, x0`, the next instruction is at a physical address (e.g., 0x4080_1234). The MMU translates this using TTBR0 (identity map), which maps 0x4080_1234 → 0x4080_1234. Since phys==virt in the identity map, the fetch succeeds.

```
Before MMU ON:  PC = 0x4080_1234 → physical fetch → OK
After MMU ON:   PC = 0x4080_1234 → MMU: TTBR0 → PTE[0x4080_1234] → 0x4080_1234 → OK
```

---

### Step 3: Map Kernel at Link Address

```asm
    // After __enable_mmu returns (MMU is now ON, running at identity-mapped VA)
    bl    __pi_early_map_kernel    // Create kernel mapping in init_pg_dir
```

**`__pi_early_map_kernel()`** creates a mapping of the kernel image at its **link address** (the virtual address the kernel was compiled for). This is different from the identity map:

```
Identity map:  VA 0x4080_0000 → PA 0x4080_0000  (TTBR0)
Kernel map:    VA 0xFFFF_FF80_0800_0000 → PA 0x4080_0000  (TTBR1)
```

The function:
1. Calculates the offset between physical load address and virtual link address
2. Uses `map_range()` to create entries in `init_pg_dir`
3. Maps kernel text as RO+X, kernel data as RW+NX
4. The kernel's TTBR1 is then switched from `reserved_pg_dir` to `init_pg_dir`

### TTBR1 Switch

```asm
    // Update TTBR1 to point to init_pg_dir (with kernel mapping)
    adrp  x1, init_pg_dir
    phys_to_ttbr x1, x1
    msr   ttbr1_el1, x1            // Now kernel VAs resolve correctly
    isb
```

---

### Step 4: Jump to Virtual Address

```asm
    ldr   x8, =__primary_switched  // x8 = VIRTUAL address of __primary_switched
    br    x8                        // Jump to KERNEL virtual address!
```

**This is the point of no return.** The `ldr x8, =__primary_switched` loads the **link address** (virtual) of `__primary_switched`, not its physical address. The `br x8` jumps there.

```
Before: PC = 0x4080_XXXX (identity-mapped via TTBR0)
After:  PC = 0xFFFF_FF80_08XX_XXXX (kernel-mapped via TTBR1)
```

From this point, all code executes at kernel virtual addresses via TTBR1.

---

## Visual: The Three-Step Address Transition

```
Step 1: MMU OFF                   Step 2: MMU ON (identity)         Step 3: MMU ON (kernel VA)
┌──────────────────┐             ┌──────────────────┐             ┌──────────────────┐
│ PC = 0x4080_0000 │             │ PC = 0x4080_0000 │             │ PC = 0xFFFF_...  │
│ Physical fetch   │   enable    │ TTBR0 translates │    br x8    │ TTBR1 translates │
│ No translation   │ ──────────► │ 0x4080 → 0x4080  │ ──────────► │ 0xFFFF → 0x4080  │
│                  │   (Step 2)  │ Identity mapping  │   (Step 4)  │ Kernel mapping   │
└──────────────────┘             └──────────────────┘             └──────────────────┘
```

---

## TTBR Format

The TTBR (Translation Table Base Register) holds the physical address of the root page table plus an optional ASID:

```
TTBR0_EL1 / TTBR1_EL1:
┌──────────┬──────────────────────────────┬─────┐
│ [63:48]  │ [47:1]                       │ [0] │
│ ASID     │ BADDR (page table phys addr) │ CnP │
└──────────┴──────────────────────────────┴─────┘

BADDR = Physical address of PGD table, aligned to 4KB
ASID  = Address Space Identifier (for TLB tagging)
CnP   = Common-not-Private (multiprocessor TLB sharing)
```

---

## Memory State After This Function

```
TTBR0_EL1 = init_idmap_pg_dir    (identity map, will be replaced later)
TTBR1_EL1 = init_pg_dir          (kernel mapping)

Virtual Address Space:
┌──────────────────────────────┐ 0xFFFF_FFFF_FFFF_FFFF
│ Kernel image (text + data)   │ ← Mapped via init_pg_dir
│ at link address              │
├──────────────────────────────┤ 0xFFFF_0000_0000_0000
│ (gap)                        │
├──────────────────────────────┤ 0x0001_0000_0000_0000
│ Identity map of kernel       │ ← Mapped via init_idmap_pg_dir
│ (phys == virt)               │
└──────────────────────────────┘ 0x0000_0000_0000_0000
```

---

## Key Takeaways

1. **`isb` barriers are critical** — TTBR/SCTLR writes are not visible to instruction fetch until `isb`
2. **Two-phase mapping** — identity map first (for survival), kernel map second (for operation)
3. **`reserved_pg_dir` as safety net** — TTBR1 initially points to an empty table, preventing accidental kernel VA access
4. **The `br x8` is a one-way door** — after jumping to the kernel virtual address, the CPU never returns to identity-mapped execution (except during CPU hotplug/suspend)
5. **`init_pg_dir` is temporary** — it will be replaced by `swapper_pg_dir` during `paging_init()` in Phase 2
