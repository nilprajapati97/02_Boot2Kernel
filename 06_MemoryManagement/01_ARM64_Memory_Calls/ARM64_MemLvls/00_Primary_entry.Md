## x19, x20, x21 — ARM64 General-Purpose Registers

These are part of the **31 general-purpose 64-bit registers** (`x0`–`x30`) defined by the ARM64 (AArch64) architecture itself — they are hardware registers built into the CPU.

### The ARM64 calling convention (AAPCS64) divides them into roles:

| Registers | Role | Preserved across calls? |
|-----------|------|------------------------|
| `x0`–`x7` | Arguments / return values | No (caller-saved) |
| `x8` | Indirect result register | No |
| `x9`–`x15` | Temporary / scratch | No (caller-saved) |
| `x16`–`x17` | Intra-procedure-call scratch (IP0/IP1) | No |
| `x18` | Platform register (reserved) | No |
| **`x19`–`x28`** | **Callee-saved** | **Yes** |
| `x29` | Frame pointer (FP) | Yes |
| `x30` | Link register (LR) | Yes |

### Why x19–x21 specifically?

`x19`, `x20`, `x21` are the **first three callee-saved registers**. "Callee-saved" means any function you call (`bl some_function`) is **required** to preserve their values — if the function uses them, it must save and restore them.

This makes them ideal for `primary_entry` to store values that must survive across multiple function calls (`bl record_mmu_state`, `bl __cpu_setup`, etc.) during early boot when:
- There's no C runtime yet
- No reliable memory/stack for global variables
- The MMU may be off

### Where each gets its value in this code:

- **`x19`** ← set inside `record_mmu_state` ([line 131](arch/arm64/kernel/head.S#L131)) by reading `sctlr_el1` or `sctlr_el2`
- **`x21`** ← set inside `preserve_boot_args` ([line 161](arch/arm64/kernel/head.S#L161)) by `mov x21, x0` (FDT address from bootloader)
- **`x20`** ← set in `primary_entry` ([line 119](arch/arm64/kernel/head.S#L119)) by `mov x20, x0` (return value from `init_kernel_el`)

They are not special — any callee-saved register could have been used. The kernel authors chose `x19`–`x21` simply because they're the first available callee-saved registers.
===========================================================================================

SYM_CODE_START(primary_entry)
	bl	record_mmu_state
	bl	preserve_boot_args

	adrp	x1, early_init_stack
	mov	sp, x1
	mov	x29, xzr
	adrp	x0, __pi_init_idmap_pg_dir
	mov	x1, xzr
	bl	__pi_create_init_idmap

	/*
	 * If the page tables have been populated with non-cacheable
	 * accesses (MMU disabled), invalidate those tables again to
	 * remove any speculatively loaded cache lines.
	 */
	cbnz	x19, 0f
	dmb     sy
	mov	x1, x0				// end of used region
	adrp    x0, __pi_init_idmap_pg_dir
	adr_l	x2, dcache_inval_poc
	blr	x2
	b	1f

	/*
	 * If we entered with the MMU and caches on, clean the ID mapped part
	 * of the primary boot code to the PoC so we can safely execute it with
	 * the MMU off.
	 */
0:	adrp	x0, __idmap_text_start
	adr_l	x1, __idmap_text_end
	adr_l	x2, dcache_clean_poc
	blr	x2

1:	mov	x0, x19
	bl	init_kernel_el			// w0=cpu_boot_mode
	mov	x20, x0

	/*
	 * The following calls CPU setup code, see arch/arm64/mm/proc.S for
	 * details.
	 * On return, the CPU will be ready for the MMU to be turned on and
	 * the TCR will have been set.
	 */
	bl	__cpu_setup			// initialise processor
	b	__primary_switch
SYM_CODE_END(primary_entry)



## `primary_entry` — Complete Walkthrough at Power-On

When the ARM64 CPU powers on, firmware/bootloader loads the kernel image into RAM and jumps to the kernel's entry point. The image header at head.S contains `b primary_entry` — an unconditional branch that immediately lands here.

At this moment, the CPU state is:
- **MMU is OFF** (typically) — all addresses are physical
- **D-cache is OFF** (typically)
- **`x0`** = physical address of the Flattened Device Tree (FDT) — passed by the bootloader
- **`x1`–`x3`** = bootloader-specific arguments
- No stack, no C runtime, no virtual memory

---

### Step 1: Record MMU State

```asm
bl	record_mmu_state
```

Calls `record_mmu_state` ([line 133](arch/arm64/kernel/head.S#L133)). This determines **whether the MMU was already on** when we entered the kernel.

**Why needed?** Some boot paths (e.g., EFI boot) enter the kernel with the MMU already enabled. The kernel needs to know this because:
- If MMU is **off**: cache may have speculatively loaded stale lines → must **invalidate**
- If MMU is **on**: dirty cache lines exist → must **clean** (flush to RAM) before disabling MMU

**What it does internally:**
1. Reads `CurrentEL` to check if we're at EL1 or EL2
2. Reads the appropriate `SCTLR_ELx` (System Control Register)
3. Checks the **EE bit** (endianness) — if wrong endianness, fixes it immediately and disables MMU, sets `x19 = 0`
4. If endianness is correct, checks **M bit** (MMU enable) AND **C bit** (cache enable):
   - Both on → `x19 = SCTLR_ELx_M` (non-zero, meaning "MMU was on")
   - Either off → `x19 = 0` (treat as "MMU was off")

**Result:** `x19` is now set for the rest of boot.

---

### Step 2: Preserve Boot Arguments

```asm
bl	preserve_boot_args
```

Calls `preserve_boot_args` ([line 161](arch/arm64/kernel/head.S#L161)).

**What it does:**
1. `mov x21, x0` — saves the FDT pointer into callee-saved `x21` (because `x0` will be clobbered by every subsequent call)
2. Stores `x0`–`x3` (all four bootloader arguments) into the `boot_args` array in memory — this is a `.data` variable that C code can inspect later for debugging
3. If MMU was off (`x19 == 0`): issues `dmb sy` (data memory barrier) then invalidates the cache lines covering `boot_args` — because with MMU off, the store went directly to RAM but the cache may hold stale data for that address
4. If MMU was on (`x19 != 0`): stores `x19` into `mmu_enabled_at_boot` variable for later reference

**Why needed?** The bootloader's arguments (especially the FDT address) are the kernel's only way to discover hardware. They must be saved before any function call clobbers `x0`–`x3`.

---

### Step 3: Set Up an Early Stack

```asm
adrp	x1, early_init_stack
mov	sp, x1
mov	x29, xzr
```

- `adrp x1, early_init_stack` — loads the **page-aligned physical address** of a pre-allocated early stack (a small static buffer in the kernel's BSS/data section)
- `mov sp, x1` — sets the stack pointer to this address
- `mov x29, xzr` — zeroes the frame pointer (`x29 = 0`), marking this as the bottom of the call stack (no previous frame)

**Why needed?** The C functions called next (`__pi_create_init_idmap`) need a stack for local variables and function calls. Until now, there was **no stack at all**. This is a minimal temporary stack — just enough to run early C code.

---

### Step 4: Create the Identity-Mapped Page Tables

```asm
adrp	x0, __pi_init_idmap_pg_dir
mov	x1, xzr
bl	__pi_create_init_idmap
```

- `x0` = physical address of `init_idmap_pg_dir` (a pre-allocated page table region). The `__pi_` prefix means "position-independent" — this is a C function compiled to work with physical addresses (no virtual memory yet).
- `x1 = 0` — passed as a parameter (typically flags or start address)
- Calls `__pi_create_init_idmap` — a C function that builds **identity-mapped page tables**

**What is an identity map?** A page table where virtual address == physical address. When the MMU is turned on, the CPU immediately starts translating addresses. If the current code is at physical address `0x4008_0000`, the identity map ensures virtual address `0x4008_0000` also maps to the same place — so the next instruction fetch doesn't fault.

**Why needed?** You can't just flip the MMU on without page tables. The CPU would immediately try to translate the next instruction's address and crash. The identity map provides a seamless transition.

**Return value:** `x0` = end address of the used page table region (used for cache maintenance below).

---

### Step 5: Cache Maintenance on Page Tables

```asm
cbnz	x19, 0f          // if MMU was on, skip to label 0
dmb     sy                // full memory barrier
mov	x1, x0            // x1 = end of page table region
adrp    x0, __pi_init_idmap_pg_dir  // x0 = start of page table region
adr_l	x2, dcache_inval_poc
blr	x2                // invalidate cache for this range
b	1f                // skip the "clean" path

0:	adrp	x0, __idmap_text_start
	adr_l	x1, __idmap_text_end
	adr_l	x2, dcache_clean_poc
	blr	x2            // clean cache for idmap text
```

This is a fork based on `x19`:

**Path A — MMU was OFF (`x19 == 0`):**
- The page tables were written directly to RAM (no cache involved, because D-cache is off)
- But the CPU may have **speculatively** loaded cache lines for those addresses
- These speculative lines contain **stale data** (whatever was there before)
- `dmb sy` ensures all previous stores are visible
- `dcache_inval_poc` **invalidates** (discards) any cached data for the page table region, ensuring the CPU reads the fresh values from RAM when the MMU turns on

**Path B — MMU was ON (`x19 != 0`):**
- The page tables were written through the cache (cache was on)
- But the code in `.idmap.text` is about to be executed with the MMU **off** (after `init_kernel_el` disables it)
- `dcache_clean_poc` **cleans** (writes back to RAM) the cache lines for the identity-mapped code region
- This ensures the CPU can fetch these instructions from RAM after MMU is turned off

**Why needed?** ARM64 has a weak memory model. Caches and RAM can be inconsistent. Before any MMU state transition, you must ensure coherency, or the CPU will execute garbage.

---

### Step 6: Initialize Exception Level

```asm
1:	mov	x0, x19
	bl	init_kernel_el       // w0=cpu_boot_mode
	mov	x20, x0
```

- Passes `x19` (MMU state) to `init_kernel_el` ([line 241](arch/arm64/kernel/head.S#L241))
- This function determines the current exception level and configures it:

**If at EL2 (hypervisor level)** — the common case when booting on bare metal or a hypervisor:
1. If MMU was on, cleans hypervisor code to PoC
2. Disables MMU at EL2 (`INIT_SCTLR_EL2_MMU_OFF`)
3. Configures `HCR_EL2` (Hypervisor Configuration Register) with initial flags
4. Sets up a minimal hypervisor stub (`__hyp_stub_vectors`) so the kernel can make HVC calls later
5. Configures `SCTLR_EL1` for the kernel
6. Checks if VHE (Virtual Host Extensions / E2H) is available — if so, the kernel runs at EL2 directly
7. Sets `SPSR_EL2` and does `ERET` to drop down to EL1 — returning `BOOT_CPU_MODE_EL2` (possibly | `BOOT_CPU_FLAG_E2H`)

**If at EL1** (e.g., running under a hypervisor like KVM):
1. Disables MMU at EL1
2. Does `ERET` back to the caller — returning `BOOT_CPU_MODE_EL1`

**Result:** `x20` = boot mode (EL1 or EL2, with possible VHE flag). The CPU is now at EL1 (or EL2 with VHE), MMU is OFF, and the system control registers are in a known state.

**Why needed?** The kernel needs to run at a consistent exception level with known register state. Different firmware may enter at EL1 or EL2, so this normalizes the environment.

---

### Step 7: CPU Setup

```asm
bl	__cpu_setup
```

Calls `__cpu_setup` in proc.S. This configures the CPU's MMU-related registers **without actually turning the MMU on**:

1. Sets `TCR_EL1` (Translation Control Register) — page size, virtual address width (39/48/52-bit), memory attributes for page table walks
2. Sets `MAIR_EL1` (Memory Attribute Indirection Register) — defines memory types (Normal, Device, Write-Through, etc.) referenced by page table entries
3. Configures other CPU features (e.g., alignment checking, WXN)
4. Returns the desired `SCTLR_EL1` value in `x0` (with MMU enable bit set) — but does **not** write it yet

**Why needed?** The MMU hardware needs to know the translation scheme (page size, address width, cacheability of walks) before it can be enabled. These registers must be configured first.

---

### Step 8: Primary Switch — Enable MMU and Jump to Virtual Addresses

```asm
b	__primary_switch
```

Branches (not `bl` — no return) to `__primary_switch` ([line 330](arch/arm64/kernel/head.S#L330)):

```asm
__primary_switch:
    adrp  x1, reserved_pg_dir
    adrp  x2, __pi_init_idmap_pg_dir
    bl    __enable_mmu              // (1) TURN ON THE MMU

    adrp  x1, early_init_stack     // (2) re-establish stack
    mov   sp, x1
    mov   x29, xzr
    mov   x0, x20                  // (3) pass boot mode
    mov   x1, x21                  // (4) pass FDT pointer
    bl    __pi_early_map_kernel    // (5) create full kernel mapping

    ldr   x8, =__primary_switched  // (6) load VIRTUAL address
    adrp  x0, KERNEL_START
    br    x8                       // (7) JUMP to virtual address!
```

**Step by step:**

**(1) `__enable_mmu`** ([line 308](arch/arm64/kernel/head.S#L308)):
- Validates the CPU supports the configured page granule size
- Loads `TTBR0_EL1` with the identity map page table (`x2`)
- Loads `TTBR1_EL1` with `reserved_pg_dir` (a placeholder — kernel mappings aren't ready yet)
- Writes `SCTLR_EL1` with the MMU-enable bit → **MMU is now ON**
- Execution continues seamlessly because the identity map maps VA == PA

**(2–4)** Re-establishes the stack and passes `x20` (boot mode) and `x21` (FDT) as arguments.

**(5) `__pi_early_map_kernel`**: A C function that creates the **real kernel page tables** — mapping the kernel at its high virtual address (`0xFFFF...`), mapping the FDT, etc. This populates `swapper_pg_dir` (or `init_pg_dir`).

**(6–7)** Loads the **virtual address** of `__primary_switched` (a high kernel address like `0xFFFF800000xx_xxxx`) and branches to it. This is the moment the kernel **leaves the identity map** and starts executing at its proper virtual address.

From `__primary_switched` ([line 206](arch/arm64/kernel/head.S#L206)), the kernel:
- Sets up `init_task` as the current task with a proper kernel stack
- Installs the exception vector table
- Saves `x21` → `__fdt_pointer` (global C variable)
- Saves `x20` → `__boot_cpu_mode` via `set_cpu_boot_mode_flag`
- Calls `finalise_el2` to finalize hypervisor configuration
- Calls **`start_kernel()`** — the first C function of the kernel proper

---

### Summary: The Complete Flow

```
Power On
  │
  ▼
Bootloader loads kernel, sets x0=FDT, jumps to kernel image
  │
  ▼
Image header → b primary_entry
  │
  ├─ record_mmu_state        → x19 = was MMU on?
  ├─ preserve_boot_args      → x21 = FDT, save x0-x3 to memory
  ├─ set up early stack       → sp = early_init_stack
  ├─ create_init_idmap        → build identity page tables
  ├─ cache maintenance        → invalidate or clean depending on x19
  ├─ init_kernel_el           → normalize EL, x20 = boot mode
  ├─ __cpu_setup              → configure TCR, MAIR, etc.
  └─ __primary_switch
       ├─ __enable_mmu        → MMU ON (identity mapped)
       ├─ early_map_kernel    → create real kernel VA mappings
       └─ br __primary_switched  → jump to kernel virtual address
            ├─ init_cpu_task   → set up init_task, stack, per-cpu
            ├─ save FDT, boot mode to globals
            ├─ finalise_el2
            └─ start_kernel()  → C code takes over
```



