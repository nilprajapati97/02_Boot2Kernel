Read [](file:///c%3A/My_Projects/Kernel_Repo/linux/arch/arm64/include/asm/assembler.h#473-473), lines 473 to 500

Read [](file:///c%3A/My_Projects/Kernel_Repo/linux/arch/arm64/include/asm/assembler.h#608-608), lines 608 to 650

Read [](file:///c%3A/My_Projects/Kernel_Repo/linux/arch/arm64/include/asm/assembler.h#742-742), lines 742 to 768

Read [](file:///c%3A/My_Projects/Kernel_Repo/linux/arch/arm64/kernel/head.S#370-370), lines 370 to 400

Searched for regex `TTBR_BADDR_MASK_52|TCR_EL1_T1SZ_MASK|TCR_T1SZ|TTBR1_BADDR_4852` (`**/arch/arm64/include/asm/pgtable-hwdef.h`), 5 results

Searched for text `reserved_pg_dir` (`**/arch/arm64/kernel/head.S`), 1 result

Read [](file:///c%3A/My_Projects/Kernel_Repo/linux/arch/arm64/include/asm/pgtable-hwdef.h#215-215), lines 215 to 297

Read [](file:///c%3A/My_Projects/Kernel_Repo/linux/arch/arm64/kernel/head.S#505-505), lines 505 to 524

Searched for regex `TCR_T0SZ|T1SZ|IDMAP_VA_BITS|VA_BITS_MIN|TCR_SHARED|TCR_CACHE` (`**/arch/arm64/mm/proc.S`), 9 results

Now I have everything. Here is the complete, three-perspective deep dive.

---

# `__enable_mmu` — The 5 Instructions After Granule Validation

## The State of the World When We Arrive Here

The granule check passed. This means:
- The CPU **does** support the page size this kernel was compiled for (4K/16K/64K).
- `x0` = `INIT_SCTLR_EL1_MMU_ON` value (pre-computed by `__cpu_setup` in proc.S)
- `x1` = physical address of `swapper_pg_dir` (kernel page table) OR `reserved_pg_dir` (primary boot path)
- `x2` = physical address of `__pi_init_idmap_pg_dir` (identity map page table)
- `x3` = the 4-bit raw TGran hardware field (still live from the granule check — will be reused)
- MMU = **OFF**. Every memory access is a raw physical address.
- TLB = empty/unpredictable.
- D-cache = off. I-cache = on or off.
- `TCR_EL1`, `MAIR_EL1` already written by `__cpu_setup`.

---

## Instruction 1: `phys_to_ttbr x2, x2`

### The Kernel Side

This is a **compile-time conditional macro** from assembler.h:

```asm
.macro  phys_to_ttbr, ttbr, phys
#ifdef CONFIG_ARM64_PA_BITS_52
    orr  \ttbr, \phys, \phys, lsr #46
    and  \ttbr, \ttbr, #TTBR_BADDR_MASK_52   // GENMASK_ULL(47, 2)
#else
    mov  \ttbr, \phys                          // trivial: x2 = x2
#endif
.endm
```

For the **common 48-bit PA case** this is a `mov x2, x2` — a no-op. The kernel includes it anyway as a **hardware-compatibility wrapper** so the same call site works on all ARM64 systems without `#ifdef` scattered in head.S.

### The CPU Side

For 48-bit PA: The CPU executes one `mov` instruction. Nothing architecturally significant.

For **52-bit PA** (e.g., server-grade systems): The CPU executes the `orr`+`and` pair. Why these exact operations?

A 52-bit physical address looks like:

```
Bit positions:  63      51    47          11   0
                [zeros][EXTRA][normal 48-bit PA][page offset]
                        ^^^^
                     bits [51:48] — 4 extra bits
```

The TTBR register has this layout (standard, pre-LPA2):

```
TTBR_EL1:  [63:48]=ASID  [47:12]=base_addr_bits[47:12]  [11:2]=RES0  [1:0]=flags
                                                           ^^^^
                                              bits [5:2] repurposed for PA[51:48]
```

The `orr  x2, x2, x2, lsr #46` operation:
```
x2          = 0x000F_xxxx_xxxx_x000   (example 52-bit PA)
x2 >> 46    = 0x0000_0000_0000_003C   (PA[51:48] shifted down to bits [5:2])
OR result   = 0x000F_xxxx_xxxx_x03C   (both positions now have the extra bits)
```

Then `and x2, x2, #GENMASK_ULL(47,2)` = `0x0000_FFFF_FFFF_FFFC` clears:
- Bits [63:48] — removes the original high PA bits (ASID field must be zero here)
- Bits [1:0] — clears CnP and reserved bit

Result: a legal TTBR value with PA[47:12] in bits [47:12] and PA[51:48] in bits [5:2].

### The Memory Point of View

At this stage nothing has been written to any register yet. `x2` now contains a **TTBR-formatted value** — not a plain physical address. If you tried to use `x2` directly as a pointer to read memory, it would be wrong on 52-bit PA systems. The reformatting is for the MMU's consumption only, not for the CPU's load/store units.

---

## Instruction 2: `msr ttbr0_el1, x2`

### The Kernel Side

This loads the **identity map** (`__pi_init_idmap_pg_dir`) as the TTBR0 page table. TTBR0 governs translations for virtual addresses with bit [63] = **0** (the `0x0000...` range — traditionally user space, but here used for the boot identity map).

**Why the identity map in TTBR0?**

Look at the two callers in head.S:

```asm
// Primary path (__primary_switch):
adrp  x1, reserved_pg_dir       // ← NOT swapper_pg_dir!
adrp  x2, __pi_init_idmap_pg_dir
bl    __enable_mmu

// Secondary path (secondary_startup):
adrp  x1, swapper_pg_dir
adrp  x2, idmap_pg_dir
bl    __enable_mmu
```

`x2` is always the **identity map root**. The identity map is built by `__pi_create_init_idmap` (primary) or was already built before secondary CPUs boot. It maps:

```
VA 0x80001000  →  PA 0x80001000   (example: where the kernel was loaded)
VA 0x80002000  →  PA 0x80002000
...
```

VA exactly equals PA across the entire `.idmap.text` region.

### The CPU Side

`msr ttbr0_el1, x2` writes to `TTBR0_EL1`. This is a **system register write** — it enters the CPU's system register write pipeline. It does NOT take effect immediately in the instruction stream. The CPU may pipeline this write alongside the next instruction (`load_ttbr1`). There is no ISB here by design — the ISB inside `load_ttbr1` (next instruction) will synchronize both writes simultaneously.

Architecturally, the ARM reference manual states:
> *"A write to a TTBR register is not guaranteed to be visible to subsequent memory translations until a Context Synchronization Event."*

The CSE (ISB) that comes inside `load_ttbr1` is that event.

### The Memory Point of View

TTBR0 now **logically** points to the identity map page table base. The MMU is still OFF. No translation is happening yet. However, the table walker hardware notes: *"when the MMU next turns on, TTBR0 base = physical address of `__pi_init_idmap_pg_dir`."*

This page table was built as:

```
Level 0 entry → Level 1 table
Level 1 entry → Level 2 table (or block mapping)
Level 2 entry → 2MB block → PA = VA  (identity)
```

The critical invariant: the physical pages where `__enable_mmu` itself lives are covered by this identity map. When the MMU turns on (instruction 4), the PC is at some physical address like `0x80001234`. TTBR0's identity map must contain a valid entry for `0x80001234` — and it does.

---

## Instruction 3: `load_ttbr1 x1, x1, x3`

This is the most architecturally complex of the five. It expands to **4 instructions**:

```asm
// Expansion of: load_ttbr1  x1, x1, x3
phys_to_ttbr   x1, x1       // Step A: encode x1 for TTBR format
offset_ttbr1   x1, x3       // Step B: apply 52-bit VA offset if needed
msr            ttbr1_el1, x1 // Step C: write kernel page table to TTBR1
isb                          // Step D: Context Synchronization Event
```

### Step A: `phys_to_ttbr x1, x1` — Same bit-shuffle as instruction 1

Applied to `swapper_pg_dir` / `reserved_pg_dir` physical address. On 48-bit PA systems: `mov x1, x1` (no-op). On 52-bit PA: the same `orr`+`and` encoding.

### Step B: `offset_ttbr1 x1, x3` — The 52-bit VA twist

This macro from assembler.h:

```asm
.macro  offset_ttbr1, ttbr, tmp
#if defined(CONFIG_ARM64_VA_BITS_52) && !defined(CONFIG_ARM64_LPA2)
    mrs   \tmp, tcr_el1
    and   \tmp, \tmp, #TCR_EL1_T1SZ_MASK
    cmp   \tmp, #TCR_T1SZ(VA_BITS_MIN)
    orr   \tmp, \ttbr, #TTBR1_BADDR_4852_OFFSET
    csel  \ttbr, \tmp, \ttbr, eq
#endif
.endm
```

**Kernel side**: On a system with `CONFIG_ARM64_VA_BITS_52` but **without** `CONFIG_ARM64_LPA2`, the kernel uses 52-bit virtual addresses by repurposing TCR_EL1.T1SZ. The page global directory (`swapper_pg_dir`) needs 4× more top-level entries to cover the extra 4 VA bits.

The `TTBR1_BADDR_4852_OFFSET` from pgtable-hwdef.h:

```c
#define PTRS_PER_PGD_52_VA      (1 << (52 - PGDIR_SHIFT))   // entries for 52-bit VA
#define PTRS_PER_PGD_48_VA      (1 << (48 - PGDIR_SHIFT))   // entries for 48-bit VA
#define PTRS_PER_PGD_EXTRA      (PTRS_PER_PGD_52_VA - PTRS_PER_PGD_48_VA)
#define TTBR1_BADDR_4852_OFFSET (PTRS_PER_PGD_EXTRA << PTDESC_ORDER)
```

The MMU's table walker for TTBR1 starts reading from `TTBR1 base address`. If TTBR1 points to the *start* of the PGD, but the 52-bit VA indexing starts *before* that (because of the extra entries), you must tell the walker to start reading from `base - OFFSET`. By subtracting the offset in the TTBR register value, the walker begins at the correct position.

**CPU side**: The `csel` (conditional select) means this offset is only applied if TCR_EL1.T1SZ was actually set to the 52-bit value. This allows the same binary to boot on CPUs that advertise 52-bit VA support (`alternative_if ARM64_HAS_VA52` in `secondary_startup`) and those that don't.

**Note**: `x3` is used here as the `tmp` register. This is why `x3` — which held the raw TGran field from the granule check — is passed to `load_ttbr1` as the third argument. It gets **overwritten** by `offset_ttbr1` (specifically by the `mrs tcr_el1` and subsequent instructions).

### Step C: `msr ttbr1_el1, x1` — Load kernel page table

TTBR1 governs translations for virtual addresses with bit [63] = **1** (the `0xFFFF...` range — kernel space).

For the **primary boot path**, x1 was `reserved_pg_dir` — a single all-zeros page. This is intentional: the primary CPU doesn't need the full kernel page table through TTBR1 yet. It will call `__pi_early_map_kernel` after `__enable_mmu` returns to build `swapper_pg_dir` and switch to it.

For the **secondary boot path**, x1 is `swapper_pg_dir` — the full kernel page table already built by the primary CPU.

### Step D: `isb` — The synchronization point for TTBR0 AND TTBR1

This single `isb` synchronizes **both** the `msr ttbr0_el1` (instruction 2) and the `msr ttbr1_el1` (step C). The ARM architecture ISB is cumulative — it drains all preceding system register writes. The kernel intentionally placed only one ISB here rather than one after each TTBR write.

**CPU side effect of `isb`**: The pipeline is flushed. All speculative work based on old TTBR values is discarded. The CPU restarts from the next instruction (`set_sctlr_el1`) knowing TTBR0 and TTBR1 are definitively set.

**Memory side effect**: The MMU's page table base registers are now architecturally updated. Any subsequent hardware table walk (once MMU turns on) will use these physical addresses as the starting point.

### The Memory Point of View at This Stage

```
TTBR0_EL1 → __pi_init_idmap_pg_dir
               ├── maps PA 0x8000_0000 → VA 0x8000_0000  (identity)
               ├── maps PA 0x8000_1000 → VA 0x8000_1000  (identity)
               └── ... (covers all of .idmap.text)

TTBR1_EL1 → reserved_pg_dir (primary) or swapper_pg_dir (secondary)
               └── (primary) all zeros — no kernel VA mappings yet
               └── (secondary) full kernel VA → PA mappings
```

The MMU is still **OFF**. Both TTBRs are programmed. The hardware is in the correct state. The system is ready.

---

## Instruction 4: `set_sctlr_el1 x0`

This is **the point of no return**. It expands to 5 micro-operations:

```asm
// Expansion of: set_sctlr_el1 x0  →  set_sctlr sctlr_el1, x0
msr   sctlr_el1, x0   // (A) write new SCTLR
isb                    // (B) pipeline flush — MMU IS NOW ON
ic    iallu            // (C) invalidate all I-cache to PoU
dsb   nsh              // (D) wait for invalidation to complete
isb                    // (E) restart fetch with clean I-cache
```

### What is in `x0`? (`INIT_SCTLR_EL1_MMU_ON`)

From sysreg.h:

```c
#define INIT_SCTLR_EL1_MMU_ON \
    (SCTLR_ELx_M      |   // bit  0: MMU ENABLE ← the critical one
     SCTLR_ELx_C      |   // bit  2: data cache enable
     SCTLR_ELx_SA     |   // bit  3: stack alignment check at EL1
     SCTLR_EL1_SA0    |   // bit  4: stack alignment check at EL0
     SCTLR_EL1_SED    |   // bit  8: SETEND instruction disable
     SCTLR_ELx_I      |   // bit 12: I-cache enable
     SCTLR_EL1_DZE    |   // bit 14: DC ZVA accessible at EL0
     SCTLR_EL1_UCT    |   // bit 15: CTR_EL0 accessible at EL0
     SCTLR_EL1_nTWE   |   // bit 18: WFE not trapped to EL1
     SCTLR_ELx_WXN    |   // bit 19: Write implies XN (W^X enforcement)
     SCTLR_ELx_IESB   |   // bit 21: Implicit Error Synchronization Barrier
     SCTLR_EL1_SPAN   |   // bit 23: Set Privileged Access Never
     SCTLR_EL1_UCI    |   // bit 26: cache instructions at EL0
     SCTLR_EL1_EPAN   |   // bit 57: Enhanced PAN
     SCTLR_EL1_EOS    |   // bit 11: Exception exit is CSE
     ...)
```

All 20+ bits activate simultaneously with the ISB.

### Sub-step A: `msr sctlr_el1, x0`

**Kernel side**: Enqueues the SCTLR write into the system register pipeline. At this CPU clock cycle, the MMU is still technically OFF — the write is pending.

**CPU side**: The CPU may have instructions already in the pipeline past this `msr`. Those in-flight instructions are still executing against the **old** SCTLR (MMU off). The write buffer holds the new SCTLR value waiting for an ISB.

**Memory side**: No change to physical memory. No TLB activity. The page tables are sitting in RAM untouched.

### Sub-step B: `isb` — THE ATOMIC MOMENT

**This is the exact instruction at which MMU turns ON.**

**CPU side — what happens in the pipeline:**

```
Clock N:   msr sctlr_el1, x0    ← enters system reg write buffer
Clock N+1: isb begins

ISB processing:
  1. All instructions before ISB are retired and committed to architectural state
  2. The reorder buffer (ROB) is completely drained
  3. The system register write buffer commits: SCTLR_EL1.M=1 is now architecturally active
  4. Branch predictor state from the pre-ISB era is flushed
  5. The instruction fetch unit is restarted

Clock N+K: First instruction AFTER ISB is fetched
  → CPU issues: "Fetch instruction at PC = 0x80001238" (example)
  → This fetch goes through the MMU
  → MMU checks bit 63 of 0x80001238: it is 0, so use TTBR0
  → MMU reads TTBR0_EL1: base = __pi_init_idmap_pg_dir
  → MMU walks the page table: L0 → L1 → L2 → 2MB block → PA = 0x8000_0000
  → Physical address = 0x80001238, fetch succeeds
  → ic iallu begins executing with MMU ON
```

**Memory side — the first translation walk:**

The hardware page table walker performs:
1. Read TTBR0_EL1 base → physical address of L0 table.
2. Extract VA[47:39] (9 bits for 4K granule, 48-bit VA) → L0 index.
3. Load L0 descriptor from `__pi_init_idmap_pg_dir[index]` — a **physical memory access** even though the MMU is on (table walks always use physical addresses).
4. Follow the descriptor to the L1 table.
5. Extract VA[38:30] → L1 index, load L1 descriptor.
6. If L1 is a block entry (1GB), translate directly. If table, continue to L2.
7. Extract VA[29:21] → L2 index, load L2 descriptor (likely a 2MB block entry for the identity map).
8. PA = block_base + VA[20:0]. Since identity: PA = VA.
9. Cache the result in the TLB.

All these table walk reads happen against **physical memory** — they do not go through the MMU recursively.

**SCTLR bits that activate simultaneously:**

| Bit | What immediately changes |
|-----|--------------------------|
| `M=1` | All subsequent VA accesses are translated |
| `C=1` | Data cache is enabled — subsequent loads/stores use cache |
| `I=1` | Instruction cache enabled for translated VAs |
| `WXN=1` | Any page that is writable is automatically non-executable |
| `SPAN=1` | If PSTATE.PAN is supported, privileged code cannot access user mappings |
| `IESB=1` | All SError exceptions are synchronized at exception boundaries |
| `EPAN=1` | Enhanced PAN: strengthens SPAN behavior |

### Sub-step C: `ic iallu` — Invalidate All I-cache to PoU

**Why at this exact moment, not before?**

Before the ISB (before MMU-on): the CPU was fetching instructions using physical addresses. The I-cache may contain lines tagged with physical addresses corresponding to the identity-mapped boot code.

After the ISB (MMU-on): the CPU now fetches using **virtual addresses through the MMU**. On VIPT (Virtually Indexed, Physically Tagged) I-caches — the most common ARM implementation — the cache index comes from the VA and the tag comes from the PA. If old lines from physical address `0x80001000` are in the cache tagged as physical `0x80001000` but the new virtual index for the same physical address comes out differently, you get cache pollution.

More critically: the **`alternatives` framework** has already patched the kernel text. These patches:
1. Were written via the D-cache (data writes).
2. Were cleaned to the PoU (Point of Unification, typically L2/L3) via `DC CVAU`.
3. But the I-cache (usually private L1) may still hold the **pre-patch** version of those instructions.

`ic iallu` = "Instruction Cache Invalidate All to PoU":
- Invalidates all lines in the L1 I-cache.
- Forces the CPU to reload instructions from L2/L3 (the PoU) where the patched versions live.
- Applies to the current CPU only (no broadcast — that is `ic ialluis`).

**Memory side**: No data in RAM changes. Cache lines in L1-I are simply marked invalid. The next instruction fetch that misses L1-I will fetch from L2/L3, which has the patched instructions.

### Sub-step D: `dsb nsh` — Data Synchronization Barrier

**CPU side**: Stalls the pipeline until all cache maintenance operations from `ic iallu` have completed to the inner-shareable domain boundary. Without this, the subsequent `isb` could restart instruction fetch before the I-cache invalidation is fully committed — and the CPU might fetch from a partially invalidated cache state.

`nsh` = Non-Shareable scope. This barrier only needs to be visible to the local CPU, not broadcast to other cores. Cheaper than `ish` (inner-shareable) and `sy` (full system).

**Memory side**: The I-cache invalidation is fully visible. No data movement in RAM.

### Sub-step E: Final `isb` — Restart with Clean I-cache

**CPU side**: A second pipeline flush. The CPU restarts instruction fetch from the now-**invalidated** I-cache. The next instruction (`ret`) is fetched from L2/L3, guaranteed to be the post-`alternatives` patched version.

**Memory side**: The CPU's fetch unit will issue a cache fill request to L2/L3 for the instruction containing `ret`. This is the first instruction fetched from the **unified** cache hierarchy post-MMU-on, post-patch. It is the last instruction of `__enable_mmu`.

---

## Instruction 5: `ret`

### The Kernel Side

`ret` branches to the address in `x30` (link register). `x30` was set by the `bl __enable_mmu` in the caller. The return address is inside either `__primary_switch` or `secondary_startup` — both of which are in `.idmap.text`.

For the **primary path** (head.S):
```asm
// Returns here (still in .idmap.text, still identity-mapped):
adrp  x1, early_init_stack
mov   sp, x1                    // set up the early kernel stack
mov   x29, xzr
mov   x0, x20                   // pass boot status
mov   x1, x21                   // pass FDT pointer
bl    __pi_early_map_kernel      // BUILD swapper_pg_dir, apply KASLR
ldr   x8, =__primary_switched   // load KERNEL VIRTUAL address
adrp  x0, KERNEL_START
br    x8                         // ← FIRST JUMP TO 0xFFFF... KERNEL VA
```

The `ret` lands in `.idmap.text` where everything still works via TTBR0 identity map. Only when `br x8` executes does execution move to the `0xFFFF...` kernel virtual address space (TTBR1).

### The CPU Side

`ret` is equivalent to `br x30`. After the final `isb` from `set_sctlr_el1`, the CPU fetches `ret` through the MMU (TTBR0 identity map, VA == PA). It decodes it as an indirect branch to `x30`. The branch target (return address) is also in `.idmap.text` — identity-mapped — so the fetch of the first instruction after `ret` also succeeds through TTBR0.

### The Memory Point of View

At the instant `ret` completes:

```
CPU State:
  PC       → inside __primary_switch or secondary_startup (.idmap.text)
  MMU      → ON (SCTLR_EL1.M = 1)
  TTBR0    → __pi_init_idmap_pg_dir  (identity map: VA = PA for .idmap.text)
  TTBR1    → reserved_pg_dir (primary) / swapper_pg_dir (secondary)
  TLB      → contains the identity map entry for the current PC
  I-cache  → freshly invalidated, being re-filled from L2/L3
  D-cache  → enabled (SCTLR.C=1), coherent

Virtual address space:
  0x0000_0000_8000_0000  →  0x8000_0000  (identity map, TTBR0, .idmap.text)
  0xFFFF_0000_0000_0000  →  not yet mapped (primary: reserved_pg_dir = all zeros)
  0xFFFF_0000_0000_0000  →  kernel text/data (secondary: swapper_pg_dir)
```

---

## Complete Flow Diagram

```
GRANULE CHECK PASSED (x0=SCTLR, x1=swapper/reserved, x2=idmap, x3=TGran)
│
├─[1] phys_to_ttbr x2, x2
│     KERNEL: encode idmap physical addr for TTBR register format
│     CPU:    mov or bit-shuffle (52-bit PA only)
│     MEMORY: x2 now holds hardware-formatted TTBR value, not a pointer
│
├─[2] msr ttbr0_el1, x2
│     KERNEL: install identity map as TTBR0 (low VA range)
│     CPU:    system reg write queued, NOT yet visible to translations
│     MEMORY: TTBR0 register updated, page walker will use this base
│
├─[3] load_ttbr1 x1, x1, x3
│     ├── phys_to_ttbr  x1, x1     encode swapper/reserved addr
│     ├── offset_ttbr1  x1, x3     apply 52-bit VA offset if needed
│     ├── msr ttbr1_el1, x1        install kernel page table as TTBR1
│     └── isb                      ← CSE: BOTH ttbr0 AND ttbr1 writes committed
│         CPU: pipeline flush, TTBR0+TTBR1 architecturally visible
│         MEMORY: page walker now has valid base addresses for both VA halves
│
├─[4] set_sctlr_el1 x0  (5 sub-steps)
│     ├── msr sctlr_el1, x0        SCTLR write queued (M=1 pending)
│     ├── isb                      ← CSE: MMU ON — every access now translated
│     │   CPU: ROB drained, fetch restarted, first translated fetch via TTBR0
│     │   MEMORY: TLB fill from identity map, PA=VA, execution continues
│     ├── ic iallu                 invalidate L1 I-cache to PoU
│     │   MEMORY: L1-I lines invalidated, next fetch from L2/L3 (patched code)
│     ├── dsb nsh                  wait for ic iallu to complete
│     └── isb                      restart fetch from clean I-cache
│         CPU: guaranteed to execute patched (alternatives) code
│
└─[5] ret
      KERNEL: return to caller in .idmap.text
      CPU: branch to x30 via MMU (TTBR0 identity map)
      MEMORY: TLB hit for return address, seamless execution
      
      NEXT: __pi_early_map_kernel → build swapper_pg_dir
            br x8 → FIRST JUMP TO 0xFFFF... KERNEL VIRTUAL ADDRESS
```

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