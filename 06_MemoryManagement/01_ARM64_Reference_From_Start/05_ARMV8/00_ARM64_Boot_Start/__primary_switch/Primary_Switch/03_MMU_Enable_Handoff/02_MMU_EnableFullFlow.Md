Read [](file:///c%3A/My_Projects/Kernel_Repo/linux/arch/arm64/kernel/head.S#76-76), lines 76 to 115

Read [](file:///c%3A/My_Projects/Kernel_Repo/linux/arch/arm64/mm/proc.S#483-483), lines 483 to 571

Read [](file:///c%3A/My_Projects/Kernel_Repo/linux/arch/arm64/kernel/head.S#505-505), lines 505 to 524

Searched for regex `early_map_kernel|create_init_idmap|swapper_pg_dir|map_kernel` (`**/arch/arm64/kernel/pi/map_kernel.c`), 7 results

Read [](file:///c%3A/My_Projects/Kernel_Repo/linux/arch/arm64/kernel/pi/map_kernel.c#120-120), lines 120 to 290

Read [](file:///c%3A/My_Projects/Kernel_Repo/linux/arch/arm64/kernel/pi/map_kernel.c#241-241), lines 241 to 290

Read [](file:///c%3A/My_Projects/Kernel_Repo/linux/arch/arm64/kernel/head.S#185-185), lines 185 to 215

Searched for regex `SCTLR_EL1_WXN|SCTLR_EL1_SPAN|SCTLR_EL1_EPAN|SCTLR_ELx_C\b|SCTLR_ELx_I\b` (`**/arch/arm64/include/asm/sysreg.h`), 16 results

Now I have the complete picture. Here is the full flow explanation:

---

# Complete Flow: `__enable_mmu` — From Power-On to Virtual Memory

---

## STAGE 0 — Who Set Everything Up Before `__enable_mmu` Was Called?

To understand the function itself, you must understand the **chain of initialization** that precedes it. Nothing inside `__enable_mmu` works without this preparation.

```
BOOTLOADER
    │
    │  Loads kernel image to some physical address (e.g. 0x8000_0000)
    │  Sets x0 = FDT blob physical address
    │  Jumps to kernel entry (physical address)
    │
    ▼
primary_entry()           ← .idmap.text, raw physical execution
    │
    ├── record_mmu_state()       save x19 = was MMU on when we entered?
    ├── preserve_boot_args()     save x0..x3 into boot_args[]
    │
    ├── early_init_stack         set up a minimal assembly stack
    │
    ├── __pi_create_init_idmap() ← BUILD IDENTITY MAP PAGE TABLE
    │       Maps PA 0x8000_0000 → VA 0x8000_0000  (VA == PA)
    │       Covers entire .idmap.text section
    │       Result stored in __pi_init_idmap_pg_dir
    │
    ├── dcache_inval_poc()       flush cache lines of the new page table
    │                            so the page walker can see them from RAM
    │
    ├── init_kernel_el()         configure EL1/EL2, set PSTATE
    │
    └── __cpu_setup()            ← CONFIGURE ALL MMU SYSTEM REGISTERS
            │
            ├── tlbi vmalle1           Invalidate local TLB (start clean)
            ├── dsb nsh
            ├── msr mair_el1           Set memory attribute types:
            │                          index 0 = Device nGnRnE
            │                          index 1 = Device nGnRE
            │                          index 4 = Normal (write-back cache)
            │                          (used by page table entry attributes)
            ├── msr tcr_el1            Set Translation Control Register:
            │                          T0SZ  = size of TTBR0 VA range
            │                          T1SZ  = size of TTBR1 VA range
            │                          TG0/1 = granule size (4K/16K/64K)
            │                          IRGN/ORGN = cache policy for walks
            │                          SH    = shareability for walks
            │                          IPS   = intermediate physical addr size
            ├── (optionally) msr tcr2_el1
            └── mov_q x0, INIT_SCTLR_EL1_MMU_ON
                ret              ← returns SCTLR value in x0 to caller
```

Then `primary_entry` branches to `__primary_switch`:

```asm
SYM_FUNC_START_LOCAL(__primary_switch)
    adrp  x1, reserved_pg_dir       // x1 = kernel page table (empty for now)
    adrp  x2, __pi_init_idmap_pg_dir // x2 = identity map page table
    bl    __enable_mmu               // ← THE FUNCTION WE ARE ANALYZING
```

So when `__enable_mmu` is entered, the register contract is:

| Register | Value | Who Set It |
|----------|-------|-----------|
| `x0` | `INIT_SCTLR_EL1_MMU_ON` bit pattern | `__cpu_setup` |
| `x1` | physical address of `reserved_pg_dir` | `__primary_switch` |
| `x2` | physical address of `__pi_init_idmap_pg_dir` | `__primary_switch` |
| `x30` | return address inside `__primary_switch` | `bl __enable_mmu` |
| `MAIR_EL1` | fully programmed | `__cpu_setup` |
| `TCR_EL1` | fully programmed | `__cpu_setup` |
| TLB | empty | `__cpu_setup` tlbi |
| MMU | **OFF** | — |
| D-cache | **OFF** | — |

---

## STAGE 1 — Granule Check (Lines 2–7)

```asm
mrs   x3, ID_AA64MMFR0_EL1
ubfx  x3, x3, #ID_AA64MMFR0_EL1_TGRAN_SHIFT, 4
cmp   x3, #ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MIN
b.lt  __no_granule_support
cmp   x3, #ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MAX
b.gt  __no_granule_support
```

**What is being checked:** The CPU reads its own hardware capability register to verify it supports the page granule size the kernel was **compiled** for. The compile-time choice (`CONFIG_ARM64_4K_PAGES`, `16K`, `64K`) is baked into the constants at build time.

**Why it can fail:** The bootloader may have loaded a 4K-page kernel onto a CPU that only supports 64K pages (rare embedded SoCs). Without this check, writing a 4K-format page table to TTBR and enabling the MMU would produce silent garbage translations.

**What happens on failure:**

```asm
__no_granule_support:
    update_early_cpu_boot_status \
        CPU_STUCK_IN_KERNEL | CPU_STUCK_REASON_NO_GRAN, x1, x2
        // writes status to __early_cpu_boot_status in RAM
        // dc ivac → pushes it to main memory for debuggers to see
1:  wfe         // sleep until event
    wfi         // sleep until interrupt
    b 1b        // loop forever — CPU is parked permanently
```

The CPU is permanently halted. It will never boot. This is intentional — a parked CPU is far safer than a CPU executing with broken translations.

**After this check passes**, `x3` still holds the raw 4-bit TGran hardware value. It will be **reused** by `load_ttbr1` later.

---

## STAGE 2 — `phys_to_ttbr x2, x2` — Encode the Identity Map Address

```asm
phys_to_ttbr x2, x2
```

### What Is `x2` at this point?

`x2` holds the **physical address** of `__pi_init_idmap_pg_dir` — the root of the identity map page table that was built by `__pi_create_init_idmap`.

### What does this macro do?

For **48-bit PA** systems (standard):
```asm
mov  x2, x2    // no-op — physical address is already in the right format
```

For **52-bit PA** systems (`CONFIG_ARM64_PA_BITS_52`):
```asm
orr  x2, x2, x2, lsr #46   // copy PA bits [51:48] down to bits [5:2]
and  x2, x2, #GENMASK_ULL(47,2)  // clear bits[63:48] and bits[1:0]
```

**Why this encoding is needed for 52-bit PA:**

The TTBR register field for the base address is only 48 bits wide in hardware. For 52-bit PA (LPA extension), the top 4 PA bits cannot go in their natural positions — bits [63:48] are occupied by the ASID field. The ARM architecture spec requires those 4 extra bits to be encoded in TTBR bits [5:2]. The `phys_to_ttbr` macro does that bit-shuffle.

```
52-bit PA example:   0x000F_0000_0001_0000
After macro:         0x0000_0000_0001_003C
                                     ^^^^── bits[5:2] = PA[51:48] = 0xF
                          ^^^^^^^^────────── bits[47:6]  = PA[47:6]
```

After this instruction, `x2` is a properly-formatted **TTBR register value**, not just a plain physical address.

---

## STAGE 3 — `msr ttbr0_el1, x2` — Install the Identity Map

```asm
msr  ttbr0_el1, x2    // load TTBR0
```

### What is TTBR0_EL1?

`TTBR0_EL1` is the **Translation Table Base Register 0**, active at Exception Level 1. It holds the physical address of the **root page table** for the **lower** virtual address range (bit 63 = 0, addresses `0x0000_0000_0000_0000` to `0x0000_FFFF_FFFF_FFFF`).

### Why the identity map?

When the MMU turns on, the CPU's program counter still holds a **physical-looking address** (e.g., `0x8000_1234`). The CPU will try to interpret that as a virtual address through TTBR0. If TTBR0's page table doesn't have an entry for `0x8000_1234`, the result is an immediate **Translation Fault** — with no exception handler yet installed, the machine dies.

The identity map (`__pi_init_idmap_pg_dir`) provides:
```
VA 0x8000_0000 → PA 0x8000_0000  (2MB block, covers all .idmap.text)
VA 0x8000_2000 → PA 0x8000_2000  (...)
...
```

Since VA == PA, the CPU's instruction stream continues uninterrupted across the MMU-on boundary.

### CPU timing

At this clock cycle, `SCTLR_EL1.M` is still 0. The MMU is **still off**. The `msr` instruction enqueues the TTBR0 write but the change is not architecturally visible yet. There is **no ISB here** — the ISB inside `load_ttbr1` (next instruction) will synchronize both TTBR writes at once.

---

## STAGE 4 — `load_ttbr1 x1, x1, x3` — Install the Kernel Page Table

```asm
load_ttbr1  x1, x1, x3
```

This macro expands to **4 instructions**:

```asm
// Step 4a
phys_to_ttbr   x1, x1       // encode x1 (swapper/reserved) for TTBR format

// Step 4b
offset_ttbr1   x1, x3       // apply 52-bit VA offset into x1 if needed

// Step 4c
msr            ttbr1_el1, x1 // write kernel page table base to TTBR1

// Step 4d
isb                          // ← CSE: synchronize BOTH ttbr0 AND ttbr1
```

### Step 4a: `phys_to_ttbr x1, x1`

Same 52-bit PA encoding as Stage 2, now applied to `reserved_pg_dir` (primary path) or `swapper_pg_dir` (secondary path).

### Step 4b: `offset_ttbr1 x1, x3`

This handles the **52-bit VA** case specifically for TTBR1. The issue is subtly different from 52-bit PA:

```asm
.macro  offset_ttbr1, ttbr, tmp
#if defined(CONFIG_ARM64_VA_BITS_52) && !defined(CONFIG_ARM64_LPA2)
    mrs   \tmp, tcr_el1
    and   \tmp, \tmp, #TCR_EL1_T1SZ_MASK      // read current T1SZ
    cmp   \tmp, #TCR_T1SZ(VA_BITS_MIN)         // is it set for 52-bit VA?
    orr   \tmp, \ttbr, #TTBR1_BADDR_4852_OFFSET
    csel  \ttbr, \tmp, \ttbr, eq               // conditionally apply offset
#endif
.endm
```

With 52-bit virtual addresses, the Page Global Directory needs more top-level entries (to index the extra 4 VA bits). `TTBR1` must point to an address that is **before the actual page table start** by the size of those extra PGD entries. The offset bakes this backward shift into the TTBR value so the hardware page table walker reads from the correct position.

Note: `x3` is the `tmp` register here — it gets **overwritten** in this step. The TGran value it held after the granule check is no longer needed after `load_ttbr1` completes.

### Step 4c: `msr ttbr1_el1, x1`

`TTBR1_EL1` governs translations for the **upper** virtual address range (bit 63 = 1, addresses `0xFFFF_0000_0000_0000` to `0xFFFF_FFFF_FFFF_FFFF`) — the kernel address space.

**Primary path:** `x1` = `reserved_pg_dir` (a single zero-filled page). This means after MMU-on, **no kernel VA mappings exist yet**. The kernel runs entirely through TTBR0 identity map for a brief period until `__pi_early_map_kernel` builds `swapper_pg_dir`.

**Secondary path:** `x1` = `swapper_pg_dir` (the full kernel page table built by the primary CPU). Secondary CPUs can immediately access all kernel virtual addresses after MMU-on.

### Step 4d: `isb` — The First Critical Barrier

This single `isb` is a **Context Synchronization Event** that:
1. Commits the `msr ttbr0_el1` write from Stage 3.
2. Commits the `msr ttbr1_el1` write from Step 4c.
3. Flushes the CPU pipeline.
4. Ensures all subsequent memory translations see the new TTBR values.

Without this ISB, the CPU could begin using the TTBRs before they are architecturally visible — the ARM spec is explicit that a TTBR write requires a subsequent ISB before the new table is used.

**System state after this ISB:**

```
TTBR0_EL1 → __pi_init_idmap_pg_dir   (identity: VA == PA for .idmap.text)
TTBR1_EL1 → reserved_pg_dir          (all zeros, no kernel VA mappings yet)
TCR_EL1   → programmed (T0SZ, T1SZ, TG0/1, IRGN, ORGN, IPS...)
MAIR_EL1  → programmed (memory type attributes)
SCTLR_EL1 → MMU still OFF (M bit = 0)
```

Everything is ready. The page tables are in RAM. The hardware registers point to them. The only remaining step is to flip the switch.

---

## STAGE 5 — `set_sctlr_el1 x0` — THE SWITCH IS FLIPPED

```asm
set_sctlr_el1  x0
```

This macro expands to **5 sub-instructions**:

```asm
msr   sctlr_el1, x0    // Sub-step A: enqueue SCTLR write
isb                    // Sub-step B: ← MMU IS NOW ON
ic    iallu            // Sub-step C: invalidate all I-cache to PoU
dsb   nsh              // Sub-step D: wait for invalidation
isb                    // Sub-step E: restart fetch from clean I-cache
```

### What is in `x0`? — The Full SCTLR Bit Map

`INIT_SCTLR_EL1_MMU_ON` from sysreg.h:

```
Bit  0: M      = 1  ← MMU ENABLE — the entire point of this function
Bit  2: C      = 1  ← Data cache enable
Bit  3: SA     = 1  ← SP alignment check at EL1
Bit  4: SA0    = 1  ← SP alignment check at EL0
Bit  8: SED    = 1  ← SETEND instruction disable
Bit 12: I      = 1  ← Instruction cache enable
Bit 14: DZE    = 1  ← DC ZVA accessible at EL0
Bit 15: UCT    = 1  ← CTR_EL0 accessible at EL0
Bit 18: nTWE   = 1  ← WFE not trapped to EL1
Bit 19: WXN    = 1  ← Write implies XN (no writable+executable pages)
Bit 21: IESB   = 1  ← Implicit Error Synchronization Barrier
Bit 23: SPAN   = 1  ← Set Privileged Access Never
Bit 26: UCI    = 1  ← cache instr accessible at EL0
Bit 57: EPAN   = 1  ← Enhanced Privileged Access Never
```

### Sub-step A: `msr sctlr_el1, x0`

The new SCTLR value enters the system register write buffer. The CPU may still be executing subsequent instructions using the **old** SCTLR (M=0). The write is pending. The MMU is still technically off.

### Sub-step B: `isb` — THE EXACT MOMENT THE MMU TURNS ON

```
Before ISB:  CPU executes with SCTLR_EL1.M = 0 (MMU off)
   ISB:      Reorder buffer drained, SCTLR write committed
After ISB:   CPU executes with SCTLR_EL1.M = 1 (MMU ON)
```

**What the CPU hardware does at this exact moment:**

```
Clock cycle T:   isb begins processing
Clock cycle T+1: reorder buffer (ROB) fully drains — all in-flight instructions retire
Clock cycle T+2: system register commit — SCTLR_EL1.M=1 is now architectural state
Clock cycle T+3: branch predictor state from M=0 era is flushed
Clock cycle T+4: instruction fetch unit restarts
Clock cycle T+5: CPU issues: "Fetch instruction at PC = 0x8000_123C"
                  → MMU intercepts: bit 63 of 0x8000_123C is 0 → use TTBR0
                  → Page table walk begins:
                     Read TTBR0_EL1 base = __pi_init_idmap_pg_dir
                     VA[47:39] = index into L0 table
                     Load L0 descriptor from RAM (physical address)
                     VA[38:30] = index into L1 table
                     Load L1 descriptor from RAM
                     VA[29:21] = index into L2 table (2MB block)
                     PA = block_base + VA[20:0] = 0x8000_123C  ← identity
                  → TLB filled: VA 0x8000_1000 → PA 0x8000_1000
                  → Instruction fetched from PA 0x8000_123C
                  → ic iallu begins executing
```

All 20+ SCTLR bits activate simultaneously. From this instruction forward:
- Every memory access is translated through the page table walker or TLB
- Write+Execute pages are blocked (WXN)
- Privileged-mode access to user pages is blocked (SPAN, EPAN)
- Data cache is on (C=1) — stores go to cache, not directly to RAM

### Sub-step C: `ic iallu` — Invalidate All I-cache

This is executed **with the MMU already on**.

**Why is this needed?**

Before the MMU turned on, `alternatives` patching may have already run — writing new instructions directly into kernel `.text` sections in memory. Those writes went through the **data cache** (D-cache), not the instruction cache (I-cache). The I-cache may still hold **pre-patch** (stale) instruction lines from when the CPU was speculatively prefetching code.

`ic iallu` = Instruction Cache Invalidate All to Point of Unification:
- Invalidates every line in the L1 instruction cache.
- Forces future instruction fetches to reload from L2/L3 (the PoU), where the patched instructions live.
- Does NOT affect data caches or main memory.

**Examples of what would be wrong without this:**
- Spectre-v2 mitigations (`IBRS`, `CSV2`) not active because the patched `nop → csv2_instruction` was not fetched.
- MTE tag checking not operational.
- BTI (Branch Target Identification) enforcement missing.

### Sub-step D: `dsb nsh` — Wait for Cache Invalidation

`dsb nsh` = Data Synchronization Barrier, non-shareable scope.

This stalls the pipeline until the `ic iallu` has **fully completed**. Without this, the following `isb` could restart instruction fetch before the I-cache invalidation has propagated through the cache hierarchy — potentially fetching from a half-invalidated state.

`nsh` (non-shareable) is intentional: at this point during primary boot, no other CPUs are online yet. A `sy` (full system) barrier would be unnecessary overhead.

### Sub-step E: Final `isb` — Restart with Clean I-cache

The second `isb` flushes the pipeline again. The CPU restarts instruction fetch with:
- MMU fully active.
- I-cache fully invalidated.
- Any further instruction fetch guaranteed to come from L2/L3 (the patched, coherent copy).

---

## STAGE 6 — `ret` — Return to Identity-Mapped Caller

```asm
ret    // branches to x30
```

**What x30 contains:** The return address inside `__primary_switch` — inside `.idmap.text` — physical address e.g. `0x8000_0ABC`.

**Translation at ret:** The CPU fetches from VA `0x8000_0ABC`. MMU is on. TTBR0 identity map has this VA mapped. TLB may already have it cached from the walk at Sub-step B. Execution continues seamlessly.

---

## STAGE 7 — What Happens After `ret` Returns?

Back in `__primary_switch`:

```asm
// MMU is ON. Still running in .idmap.text (identity mapped via TTBR0)
// TTBR1 = reserved_pg_dir (all zeros — no 0xFFFF... mappings yet)

adrp  x1, early_init_stack
mov   sp, x1               // set up proper stack
mov   x29, xzr
mov   x0, x20              // boot_status
mov   x1, x21              // FDT physical address
bl    __pi_early_map_kernel // ← BUILD THE FULL KERNEL PAGE TABLE
                            //   calls early_map_kernel() in map_kernel.c:
                            //   - clears BSS
                            //   - applies KASLR offset
                            //   - calls map_kernel() → fills swapper_pg_dir
                            //   - calls idmap_cpu_replace_ttbr1(swapper_pg_dir)
                            //     → switches TTBR1 to swapper_pg_dir
                            //   - kernel VA space 0xFFFF... is now LIVE

ldr   x8, =__primary_switched  // load KERNEL VIRTUAL address (0xFFFF...)
adrp  x0, KERNEL_START
br    x8               // ← FIRST JUMP TO 0xFFFF... KERNEL VIRTUAL ADDRESS
                       //   This is the point of no return to physical execution
```

The `br x8` is the **architectural boundary** where the kernel stops using the TTBR0 identity map and switches to the TTBR1 kernel virtual address space permanently.

---

## Complete Initialization Flow — One Picture

```
POWER ON
    │
    ▼
BOOTLOADER → loads kernel to PA 0x8000_0000, jumps to it
    │
    ▼ physical execution begins
primary_entry()
    │
    ├─ record_mmu_state()         x19 = was MMU on at entry?
    ├─ preserve_boot_args()       save FDT pointer in x21
    ├─ setup early_init_stack     sp = early stack
    ├─ __pi_create_init_idmap()   BUILD identity map in RAM
    │       __pi_init_idmap_pg_dir: PA 0x8000_0000 → VA 0x8000_0000
    ├─ dcache_inval_poc()         flush page table to RAM
    ├─ init_kernel_el()           configure EL1/EL2, PSTATE
    └─ __cpu_setup()              INITIALIZE ALL MMU REGISTERS
            ├─ tlbi vmalle1       empty TLB
            ├─ msr mair_el1       memory types (Device, Normal, NC...)
            ├─ msr tcr_el1        VA size, granule, cache policy, IPS
            └─ returns x0 = INIT_SCTLR_EL1_MMU_ON
    │
    ▼ branches to __primary_switch
__primary_switch()
    ├─ x1 = reserved_pg_dir (empty kernel page table)
    ├─ x2 = __pi_init_idmap_pg_dir
    └─ bl __enable_mmu
            │
            ├─[1] mrs x3, ID_AA64MMFR0_EL1      read CPU capabilities
            ├─[2] ubfx x3, ...                    extract TGran field
            ├─[3] cmp + b.lt                      check MIN support
            ├─[4] cmp + b.gt                      check MAX support
            │         └── FAIL → park CPU forever (wfe/wfi loop)
            │
            ├─[5] phys_to_ttbr x2, x2            encode PA for TTBR format
            ├─[6] msr ttbr0_el1, x2              load identity map → TTBR0
            ├─[7] load_ttbr1 x1, x1, x3
            │       ├─ phys_to_ttbr x1, x1       encode kernel pg table
            │       ├─ offset_ttbr1 x1, x3       52-bit VA adjustment
            │       ├─ msr ttbr1_el1, x1          load kernel pg table → TTBR1
            │       └─ isb                        ← synchronize BOTH TTBRs
            │
            ├─[8] set_sctlr_el1 x0
            │       ├─ msr sctlr_el1, x0          write new SCTLR (M=1 pending)
            │       ├─ isb           ←══════════ MMU IS NOW ON ══════════════╗
            │       │                             Page walker active         ║
            │       │                             TLB fills from idmap       ║
            │       ├─ ic iallu                   invalidate I-cache         ║
            │       ├─ dsb nsh                    wait for invalidation      ║
            │       └─ isb                        fetch from clean I-cache   ║
            │                                                                ║
            └─[9] ret                             return to __primary_switch ║
                                                  (still identity mapped)    ║
    │                                                                        ║
    ├─ __pi_early_map_kernel()    BUILD swapper_pg_dir (kernel VA mappings)  ║
    │       └─ idmap_cpu_replace_ttbr1(swapper_pg_dir)                       ║
    │              ← TTBR1 now points to full kernel page table              ║
    │                                                                         ║
    └─ br x8  (=__primary_switched at 0xFFFF...)  ══════════════════════════╝
                  FIRST INSTRUCTION AT KERNEL VIRTUAL ADDRESS
                  TTBR0 identity map no longer needed
                  
__primary_switched()
    ├─ init_cpu_task()            set up init_task stack
    ├─ msr vbar_el1, vectors      install exception vector table
    ├─ str kimage_voffset         save VA-PA offset
    ├─ set_cpu_boot_mode_flag()
    ├─ finalise_el2()
    └─ bl start_kernel()          ← FULL C KERNEL STARTS
```

---

## System Register State Timeline

```
                    MAIR_EL1  TCR_EL1  TTBR0_EL1  TTBR1_EL1  SCTLR.M
                    ────────  ───────  ─────────  ─────────  ───────
After power-on:     unknown   unknown  unknown    unknown    0 (off)
After __cpu_setup:  set       set      unknown    unknown    0 (off)
After msr ttbr0:    set       set      idmap      unknown    0 (off)
After load_ttbr1:   set       set      idmap      reserved   0 (off)
After isb(TTBR):    set       set      idmap✓     reserved✓  0 (off)
After isb(SCTLR):   set       set      idmap✓     reserved✓  1 (ON!) ←
After early_map:    set       set      idmap✓     swapper✓   1 (ON)
After br x8:        set       set      idmap✓     swapper✓   1 (ON)
After start_kernel: fully managed by the running kernel
```

The `✓` marks the moment each register became architecturally committed. The MMU doesn't use a register until both it is written AND an ISB has passed.

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