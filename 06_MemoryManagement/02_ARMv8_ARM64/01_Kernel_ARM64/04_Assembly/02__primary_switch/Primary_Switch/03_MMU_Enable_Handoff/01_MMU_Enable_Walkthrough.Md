---

# Deep Dive: `__enable_mmu` — ARM64 Linux Kernel Boot

This is one of the most **security-critical, hardware-coupled** functions in the entire kernel. It is called exactly once per CPU core during boot, at the precise moment the CPU transitions from running on raw physical addresses to running under virtual memory. Here is a full decomposition.

---

## 0. The Big Picture — What Is Happening?

Before this function runs:
- The CPU is executing using **physical addresses** directly. Every instruction fetch and data access goes straight to RAM without any translation.
- Page tables have already been built by `__pi_create_init_idmap` and `__cpu_setup`.
- `TTBR0_EL1`, `TTBR1_EL1`, and `TCR_EL1` are not yet activated for the MMU.

After this function returns:
- The **MMU is on**. Every memory access goes through the Translation Lookaside Buffer (TLB) and page table walker.
- Virtual addresses are now live.
- The kernel is running on `TTBR1_EL1` (kernel VA space), userspace would use `TTBR0_EL1`.

---

## 1. ARM64 Address Space Architecture (prerequisite)

ARM64 splits the 64-bit virtual address space into two halves using **TTBR0** and **TTBR1**:

```
0x0000_0000_0000_0000  -->  0x0000_FFFF_FFFF_FFFF   : user space  → TTBR0_EL1
0xFFFF_0000_0000_0000  -->  0xFFFF_FFFF_FFFF_FFFF   : kernel space → TTBR1_EL1
```

The hardware uses bit 63 of the virtual address to decide which TTBR to use. This split is defined by `TCR_EL1.T0SZ` and `TCR_EL1.T1SZ`.

---

## 2. Why Is This Code in `.idmap.text`?

```asm
.section ".idmap.text","a"
```

**Interview answer**: The `.idmap.text` section is the **identity-mapped text** region. Its physical address equals its virtual address. This is essential because:

1. Before the MMU is on, the CPU fetches instructions from **physical** address `X`.
2. The moment `SCTLR_EL1.M` is set (MMU on), the CPU tries to fetch the *next* instruction from a **virtual** address.
3. If the code enabling the MMU is identity-mapped, virtual address `X` == physical address `X`, so the instruction stream is uninterrupted.
4. If it were NOT identity-mapped, the CPU would jump to a random/invalid address the instant the MMU turned on — instant crash.

Both `__enable_mmu` and its **caller** must be in `.idmap.text`. The comment in the code says exactly this: *"This requires the caller to be covered by the .idmap.text section."*

---

## 3. Function Calling Convention — Register Contract

```
x0  = SCTLR_EL1 value (to enable MMU — the bit field pre-computed by __cpu_setup)
x1  = TTBR1_EL1 value (kernel page table physical address = swapper_pg_dir)
x2  = ID map root table physical address (__pi_init_idmap_pg_dir)
x30 = link register (return address)
```

All three arguments were set up by the **caller** (`__primary_switch` or `secondary_startup`). The kernel pre-computes the exact SCTLR value in `__cpu_setup` (proc.S) so this function just writes it atomically.

---

## 4. Line-By-Line Deep Dive

### Line 1: Read the CPU's Memory Model Feature Register

```asm
mrs  x3, ID_AA64MMFR0_EL1
```

**`mrs`** = Move to Register from System register.

`ID_AA64MMFR0_EL1` is a **read-only** identification register (you cannot write to it). It describes the memory model and address translation capabilities of **this specific CPU core**. Key fields:

| Bits | Field | Meaning |
|------|-------|---------|
| [27:24] | TGran64 | 64KB granule support |
| [27:24] | TGran16 | 16KB granule support |
| [31:28] | TGran4 | 4KB granule support |
| [3:0] | PARange | Physical address range |

The encoding is **NOT** a simple boolean. For `TGran4` (4KB pages):
- `0b0000` = supported
- `0b1111` = not supported

For `TGran16` (16KB pages):
- `0b0001` = supported
- `0b0000` = not supported

> **Interview depth**: Why read this *now*? Because the kernel was compiled for a specific page size (`CONFIG_ARM64_4K_PAGES`, `CONFIG_ARM64_16K_PAGES`, or `CONFIG_ARM64_64K_PAGES`), but the CPU it is running on might be a different silicon revision. If you try to turn the MMU on with page tables in 4KB format but the CPU doesn't support 4KB granules, the CPU will fault or produce garbage translations. This is a **mandatory hardware capability check** before committing to MMU-on.

---

### Line 2: Extract the Granule Support Field

```asm
ubfx  x3, x3, #ID_AA64MMFR0_EL1_TGRAN_SHIFT, 4
```

**`ubfx`** = Unsigned Bit Field Extract. It extracts 4 bits starting at bit `ID_AA64MMFR0_EL1_TGRAN_SHIFT` from `x3` (the MMFR0 value) and zero-extends into `x3`.

The shift value is compile-time selected based on your `CONFIG_ARM64_*K_PAGES`:

```c
// From arch/arm64/include/asm/sysreg.h
#if defined(CONFIG_ARM64_4K_PAGES)
#define ID_AA64MMFR0_EL1_TGRAN_SHIFT    ID_AA64MMFR0_EL1_TGRAN4_SHIFT   // bits [31:28]
#define ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MIN  0x0
#define ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MAX  0x7
#elif defined(CONFIG_ARM64_16K_PAGES)
#define ID_AA64MMFR0_EL1_TGRAN_SHIFT    ID_AA64MMFR0_EL1_TGRAN16_SHIFT  // bits [23:20]
#define ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MIN  0x1
#define ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MAX  0xf
#elif defined(CONFIG_ARM64_64K_PAGES)
#define ID_AA64MMFR0_EL1_TGRAN_SHIFT    ID_AA64MMFR0_EL1_TGRAN64_SHIFT  // bits [27:24]
#define ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MIN  0x0
#define ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MAX  0x7
```

So for a 4K-page kernel, after this instruction, `x3` contains the raw 4-bit hardware value for TGran4 support.

---

### Lines 3–6: Range Check — Validate Granule Support

```asm
cmp   x3, #ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MIN
b.lt  __no_granule_support
cmp   x3, #ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MAX
b.gt  __no_granule_support
```

This is a **range check**: the extracted 4-bit value must be within `[MIN, MAX]` to mean "supported".

For 4KB pages (most common case):
- `MIN = 0x0`, `MAX = 0x7`
- `0xF` (`0b1111`) means the CPU **does not support** 4KB granules
- Any value `0x0`–`0x7` means supported (with some values indicating LPA2 capability)

> **Interview depth**: The ARM architecture uses a non-monotonic encoding here. It is not a simple "bit is set = supported". The MIN/MAX range was introduced because:
> 1. Later architecture revisions (ARMv8.2+) added more granularity in the encoding to distinguish LPA2 support (52-bit PA with 4KB granule).
> 2. `0xF` is explicitly "unsupported" for TGran4/TGran64 but for TGran16 `0x0` is unsupported.
> 3. The range check gracefully handles all these variants without per-revision if-else logic.

**What happens if this check fails?**

```asm
SYM_FUNC_START_LOCAL(__no_granule_support)
    update_early_cpu_boot_status \
        CPU_STUCK_IN_KERNEL | CPU_STUCK_REASON_NO_GRAN, x1, x2
1:  wfe
    wfi
    b   1b
SYM_FUNC_END(__no_granule_support)
```

The CPU writes a status code to `__early_cpu_boot_status` (in memory) and then enters an infinite `wfe`/`wfi` loop — it is **parked forever**. This is called "parking" a CPU. The primary CPU continues; the stuck CPU can never proceed. This is safer than a crash or undefined behavior.

---

### Line 7: Encode the ID Map Physical Address for TTBR0

```asm
phys_to_ttbr x2, x2
```

This is a macro expanding (for non-52-bit PA) to simply:
```asm
mov  x2, x2   // identity for 48-bit PA
```

For `CONFIG_ARM64_PA_BITS_52` (52-bit physical address space):
```asm
// From assembler.h
orr  x2, x2, x2, lsr #46
and  x2, x2, #TTBR_BADDR_MASK_52
```

**Why is this needed for 52-bit?**  
TTBR registers are 64-bit, but the base address field in a TTBR is only 48 bits wide in the hardware register layout. For 52-bit PA support (LPA — Large Physical Address extension), the top 4 bits of the PA (bits [51:48]) must be encoded in bits [5:2] of the TTBR register (they are repurposed from the ASID field). The `phys_to_ttbr` macro does this bit-shuffling.

> **Interview depth**: This is a fundamental ABI quirk of ARMv8.2 LPA. A naive `msr ttbr0_el1, x2` would lose the top 4 PA bits on 52-bit systems. The kernel always uses this macro rather than a raw `msr` to ensure correctness across all supported hardware configurations.

---

### Line 8: Load the Identity Map into TTBR0_EL1

```asm
msr  ttbr0_el1, x2    // load TTBR0
```

`TTBR0_EL1` is the **Translation Table Base Register 0 for EL1**. It holds the physical address of the Level 0 (or Level 1 depending on `TCR_EL1.T0SZ`) page table for the lower virtual address range (0x0000... addresses).

**Why do we load the ID map into TTBR0 here (not the user page table)?**  
At boot time, TTBR0 must point to a page table that maps the identity-mapped physical region. This is needed because:
1. Right after the MMU turns on, the PC is still at a physical address (e.g., `0x80001234`).
2. That address must have a valid virtual→physical mapping via TTBR0.
3. The identity map (`__pi_init_idmap_pg_dir`) maps physical addresses directly (VA == PA), so the CPU can keep executing without interruption.

Later, once the kernel is fully up, TTBR0 is replaced with the actual user process page table (or `reserved_pg_dir` when no user process is running).

---

### Line 9: Load the Kernel Page Table into TTBR1_EL1

```asm
load_ttbr1  x1, x1, x3
```

This macro (from assembler.h) expands to:
```asm
phys_to_ttbr  x1, x1          // encode physical addr for TTBR (handles 52-bit PA)
offset_ttbr1  x1, x3          // handle 52-bit VA offset if needed
msr           ttbr1_el1, x1   // write to TTBR1
isb                            // instruction synchronization barrier
```

**`offset_ttbr1`** handles the `CONFIG_ARM64_VA_BITS_52 && !CONFIG_ARM64_LPA2` case: when using 52-bit virtual addresses without LPA2, bits [5:4] of TTBR1 encode the extra VA bits and need an offset of `TTBR1_BADDR_4852_OFFSET` based on TCR_EL1.T1SZ.

**`TTBR1_EL1`** will point to `swapper_pg_dir` — the kernel's permanent page table (for all kernel virtual addresses starting with `0xFFFF...`).

**`isb` after writing TTBR1**: This is architecturally required. The ARM architecture states that a write to a TTBR must be followed by an ISB before the new translation table is guaranteed to be used. Without it, the CPU pipeline might use a stale or partially-updated translation.

> **Interview depth**: Notice that `load_ttbr1` includes an `isb` but the `msr ttbr0_el1, x2` on the previous line does NOT include one. Why? Because immediately after writing TTBR0, we write TTBR1, and eventually the `isb` inside `set_sctlr_el1` will synchronize everything before the MMU is actually turned on. The kernel is careful to minimize unnecessary barriers.

---

### Line 10: Turn the MMU On

```asm
set_sctlr_el1  x0
```

This macro expands to:
```asm
// set_sctlr sreg, reg:
msr   sctlr_el1, x0    // Write the new SCTLR value
isb                     // Pipeline synchronization — makes the write effective
ic    iallu             // Invalidate all instruction caches to PoU (Point of Unification)
dsb   nsh               // Data Synchronization Barrier (non-shareable)
isb                     // Synchronize after cache invalidation
```

**`SCTLR_EL1`** (System Control Register for EL1) is the master control register. The critical bit is:

| Bit | Name | Effect when set |
|-----|------|-----------------|
| 0 | M | **MMU enable** — virtual address translation is ON |
| 2 | C | Data cache enable |
| 12 | I | Instruction cache enable |
| 25 | EE | Endianness of EL1 data accesses |

The value in `x0` was pre-computed in `__cpu_setup` with all necessary bits set, including `M=1`.

**The sequence of events at the hardware level when `msr sctlr_el1, x0` executes:**

1. The write to `SCTLR_EL1` is issued into the CPU's write pipeline.
2. The `isb` forces the CPU to **retire all in-flight instructions** and refetch from the current PC.
3. From the instruction *after* the `isb`, the MMU is ON.
4. The CPU now has to translate every subsequent fetch and data access through the page table walker.
5. The TLB is (architecturally) unpredictable at this point, so the CPU might do a page table walk immediately.
6. The identity map in TTBR0 ensures this walk succeeds.

**Why invalidate the I-cache?**  
Before the MMU is on, instructions may be fetched and cached based on physical addresses. After the MMU turns on, the cache is indexed/tagged differently (VIPT or PIPT depending on the implementation). There may also be **dynamically patched code** (`alternatives`, `static_call`) that was patched at the Point of Unification (PoU) but the instruction cache at this level may have stale copies. The `ic iallu` + `dsb nsh` + `isb` sequence ensures the CPU fetches fresh instructions from the now-coherent cache.

---

### Line 11: Return

```asm
ret
```

`ret` branches to the address in `x30` (the link register). The CPU is now executing with the MMU on, in identity-mapped space, and control returns to the caller (which must itself be in `.idmap.text`). The caller (`__primary_switch`) will then:
1. Set up the real kernel stack.
2. Call `__pi_early_map_kernel` to build the full kernel mapping.
3. Jump to `__primary_switched` at its kernel virtual address (via `ldr x8, =__primary_switched; br x8`).

That final `br x8` is the first instruction executed at a **true kernel virtual address** (`0xFFFF...`), completing the transition from physical to virtual execution.

---

## 5. The Three-Actor Summary

| Actor | Role in this function |
|-------|----------------------|
| **CPU hardware** | Reads `ID_AA64MMFR0_EL1` for capabilities; walks page tables once MMU is on; the `isb` instructions force pipeline synchronization; TLB behavior changes immediately after SCTLR write |
| **MMU hardware** | Dormant until `SCTLR_EL1.M=1`; at that point uses `TTBR0_EL1` for user/low addresses and `TTBR1_EL1` for kernel/high addresses; uses `TCR_EL1` for address size/granule config (set earlier by `__cpu_setup`) |
| **Kernel code** | Builds the page tables *before* this call; pre-computes the correct SCTLR value; carefully orders the register writes; places this code in `.idmap.text` so the identity map covers it; handles unsupported granule by parking the CPU instead of crashing |

---

## 6. Key Interview Talking Points

1. **Why `.idmap.text`?** — VA == PA continuity across the MMU-on moment. The PC value stays valid.

2. **Why write TTBR0 before TTBR1 before SCTLR?** — You must have valid translations in place *before* the MMU is enabled. Writing SCTLR last is the atomic commitment point.

3. **Why `isb` after `msr sctlr_el1`?** — ARM architecture requirement: changes to SCTLR are context-synchronizing only after an ISB. Without it, the CPU may speculatively execute instructions using the old SCTLR state.

4. **Why `ic iallu` when enabling the MMU?** — The kernel uses `alternatives` for patching CPU errata and feature bits at boot. These patches are written to the PoU. Without invalidating the I-cache, the CPU might fetch stale pre-patch instructions.

5. **Why does the granule check use a range, not an equality?** — The ARMv8 ID register encoding uses a range of values for "supported", with additional meanings for LPA2 capability within the range. `0xF` is explicitly "not supported" for 4K/64K granules.

6. **What does "parking" a CPU mean?** — `wfe`/`wfi` loop after writing the failure status. The CPU is alive but permanently suspended. It will never boot. This is better than a data abort or silent memory corruption.

7. **What is `phys_to_ttbr` doing on 52-bit PA systems?** — Encoding the top 4 PA bits (bits [51:48]) into the TTBR register's bits [5:2] as required by the LPA extension register layout.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
The MMU in ARMv8-A is enabled by writing bit 0 (M) of SCTLR_EL1 to 1 via an MSR instruction followed by an ISB. The ISB is the critical barrier: it flushes the instruction pipeline so that all instructions fetched AFTER the ISB use the new memory system configuration. Before the MMU is enabled, the CPU operates in a flat physical address space. After the bit is set, the TLB, page-table walker, TTBR0/TTBR1, TCR_EL1, and MAIR_EL1 all become active simultaneously. There is no intermediate state.

### Kernel Perspective (Linux ARM64)
Linux enables the MMU in __enable_mmu (arch/arm64/kernel/head.S), called from __primary_switch. The sequence is:
  1. Write TTBR0_EL1 (identity map root).
  2. Write TTBR1_EL1 (kernel map root).
  3. ISB to synchronize TTBR writes.
  4. Write SCTLR_EL1 with M=1 (via set_sctlr_el1 macro).
  5. ISB to flush the pipeline.
  6. RET -- the very next instruction is fetched through the new MMU.
The identity map ensures that the physical address of the code after the RET is also mapped at the same VA (PA==VA), so no instruction-fetch fault occurs.

### Memory Perspective (ARMv8 Memory Model)
The moment SCTLR_EL1.M is written to 1 and the ISB completes, the ARMv8 memory model transitions from "flat PA" to "two-stage VA->PA via page tables". The identity map (stored in __idmap_text_start to __idmap_text_end, mapped in the .idmap.text section) covers the physical pages of the MMU-enable code so the VA==PA invariant holds during the critical window. Without the identity map, the instruction fetch for the RET after set_sctlr_el1 would target a VA that has no valid TLB entry, causing a translation fault with no exception handler installed yet.