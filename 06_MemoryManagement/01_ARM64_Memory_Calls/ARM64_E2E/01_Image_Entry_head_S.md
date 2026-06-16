# 01 — Image Entry: `head.S` Code Walkthrough

[← Index](00_Index.md) | **Doc 01** | [Doc 02: Identity Map & Kernel Map →](02_Identity_Map_and_Kernel_Map.md)

---

## The Very First Byte

The bootloader (U-Boot, UEFI, etc.) loads the kernel `Image` into DRAM and jumps to
its start address. The CPU enters with:

```
Register state at kernel entry:
  x0  = physical address of the Device Tree Blob (DTB)
  x1  = 0 (reserved)
  x2  = 0 (reserved)
  x3  = 0 (reserved)
  MMU = OFF (typically) or ON (UEFI)
  EL  = EL2 (most bootloaders) or EL1
  D-cache = OFF or ON
  I-cache = may be on
```

---

## 1. `_text` — The Image Header

**Source**: `arch/arm64/kernel/head.S` — `.head.text` section

```asm
	__HEAD                              // places code in .head.text section
	efi_signature_nop                   // Byte 0-3: encodes "MZ" for PE/COFF
	b	primary_entry               // Byte 4-7: branch to real entry point
	.quad	0                           // Byte 8-15: Image load offset (0)
	le64sym	_kernel_size_le             // Byte 16-23: kernel image size
	le64sym	_kernel_flags_le            // Byte 24-31: flags (LE, 4K pages, etc.)
	.quad	0                           // Byte 32-39: reserved
	.quad	0                           // Byte 40-47: reserved
	.quad	0                           // Byte 48-55: reserved
	.ascii	ARM64_IMAGE_MAGIC           // Byte 56-59: "ARM\x64"
	.long	.Lpe_header_offset          // Byte 60-63: PE header offset (for UEFI)
```

The `efi_signature_nop` macro:
```asm
// With CONFIG_EFI:
	ccmp  x18, #0, #0xd, pl     // opcode bytes = 0x4D, 0x5A = "MZ" (DOS magic)

// Without CONFIG_EFI:
	nop
```

**Why**: UEFI firmware looks for "MZ" at byte 0 to identify PE/COFF executables. The
`ccmp` instruction's encoding happens to spell "MZ" — a harmless conditional compare
that has no side effects at boot.

**Instruction 0**: `ccmp` (or `nop`) — just passes through  
**Instruction 1**: `b primary_entry` — the real start

---

## 2. `primary_entry` — The Real Boot Entry

**Source**: `arch/arm64/kernel/head.S`

### Register Convention (maintained throughout boot)

| Register | Lifetime | Contains |
|----------|----------|----------|
| `x19` | `primary_entry` → `start_kernel` | MMU state at entry (0=off, `SCTLR_ELx_M`=on) |
| `x20` | `primary_entry` → `__primary_switch` | CPU boot mode (`BOOT_CPU_MODE_EL1` or `EL2`) |
| `x21` | `primary_entry` → `start_kernel` | FDT physical address (original `x0`) |

```asm
SYM_CODE_START(primary_entry)
	bl	record_mmu_state           //─┐ Step 1: was MMU on?
	                                   //  └→ x19 = result
	bl	preserve_boot_args         //─┐ Step 2: save FDT ptr + boot regs
	                                   //  └→ x21 = FDT, boot_args[] filled

	adrp	x1, early_init_stack   //─┐ Step 3: set up temporary stack
	mov	sp, x1                 //  │ (one page, enough for early C calls)
	mov	x29, xzr               //  └ zero frame pointer

	adrp	x0, __pi_init_idmap_pg_dir //─┐ Step 4: build identity map
	mov	x1, xzr                        //  │ x0 = pg_dir base, x1 = 0 (clrmask)
	bl	__pi_create_init_idmap         //  └→ returns ptep in x0

	/* Cache maintenance after identity map creation */
	cbnz	x19, 0f                //─ if MMU was on, skip invalidation
	dmb	sy                     //  barrier
	mov	x1, x0                 //  x1 = end of pg_dir used region
	adrp	x0, __pi_init_idmap_pg_dir
	adr_l	x2, dcache_inval_poc   //  invalidate D-cache [pg_dir_start, ptep_end)
	blr	x2
	b	1f

0:	adrp	x0, __idmap_text_start //─ MMU was on: clean idmap text to PoC
	adr_l	x1, __idmap_text_end
	adr_l	x2, dcache_clean_poc
	blr	x2

1:	mov	x0, x19                //─┐ Step 5: configure exception level
	bl	init_kernel_el         //  └→ w0 = boot mode, drop EL2→EL1

	mov	x20, x0                //─ save boot mode in x20

	bl	__cpu_setup            //─┐ Step 6: configure MAIR, TCR, SCTLR
	                               //  └→ x0 = SCTLR_EL1 value (MMU ON config)

	b	__primary_switch       //─ Step 7: enable MMU and enter kernel VA
SYM_CODE_END(primary_entry)
```

---

## 3. `record_mmu_state()` — Detect MMU State

**Source**: `arch/arm64/kernel/head.S`  
**Called by**: `primary_entry`  
**Returns**: `x19` = 0 (MMU off) or `SCTLR_ELx_M` (MMU on)

```asm
SYM_CODE_START_LOCAL(record_mmu_state)
	mrs	x19, CurrentEL              // Read current exception level
	cmp	x19, #CurrentEL_EL2        // Are we at EL2?
	mrs	x19, sctlr_el1              // Default: read SCTLR_EL1
	b.ne	0f                          // If not EL2, skip
	mrs	x19, sctlr_el2              // At EL2: read SCTLR_EL2 instead
0:
	/* Check endianness matches compile-time config */
CPU_LE( tbnz	x19, #SCTLR_ELx_EE_SHIFT, 1f )  // Little-endian build, big-endian CPU → fix
CPU_BE( tbz	x19, #SCTLR_ELx_EE_SHIFT, 1f )   // Big-endian build, little-endian CPU → fix

	tst	x19, #SCTLR_ELx_C          // Test: are D-caches enabled?
	and	x19, x19, #SCTLR_ELx_M     // Isolate MMU enable bit
	csel	x19, xzr, x19, eq          // If caches off → x19 = 0 (treat as MMU-off)
	ret

1:	/* Endianness mismatch — fix it, disable MMU */
	// ... fixes EE bit, clears M bit, returns x19 = 0 ...
SYM_CODE_END(record_mmu_state)
```

**Logic**: Read `SCTLR_ELx` for the current EL. Check if endianness matches. If MMU
and caches are both on, return `SCTLR_ELx_M` in `x19`. Otherwise return 0.

---

## 4. `preserve_boot_args()` — Save Bootloader Registers

**Source**: `arch/arm64/kernel/head.S`  
**Called by**: `primary_entry`  
**Side effects**: `x21` = FDT pointer, `boot_args[0..3]` = original x0-x3

```asm
SYM_CODE_START_LOCAL(preserve_boot_args)
	mov	x21, x0                    // x21 = FDT physical address (saved forever)

	adr_l	x0, boot_args              // x0 = &boot_args[0] (PA, since MMU may be off)
	stp	x21, x1, [x0]              // boot_args[0] = FDT, boot_args[1] = x1
	stp	x2, x3, [x0, #16]          // boot_args[2] = x2, boot_args[3] = x3

	cbnz	x19, 0f                    // If MMU on → skip cache invalidation
	dmb	sy                         // Ensure stores complete before invalidation
	add	x1, x0, #0x20             // x1 = x0 + 32 (end of boot_args)
	b	dcache_inval_poc           // Tail call: invalidate [x0, x1) from D-cache

0:	str_l	x19, mmu_enabled_at_boot, x0  // Store MMU flag (for later reference)
	ret
SYM_CODE_END(preserve_boot_args)
```

**Data structure**: `boot_args` is defined as:
```c
u64 __cacheline_aligned boot_args[4];   // in arch/arm64/kernel/setup.c
```

After this function:
- `x21` = physical address of DTB (preserved across entire boot)
- `boot_args[]` = {FDT_addr, x1_orig, x2_orig, x3_orig}

---

## 5. `init_kernel_el()` — Exception Level Setup

**Source**: `arch/arm64/kernel/head.S`  
**Called by**: `primary_entry` with `x0 = x19` (MMU state)  
**Returns**: `w0` = `BOOT_CPU_MODE_EL1` (0xe11) or `BOOT_CPU_MODE_EL2` (0xe12)

```asm
SYM_FUNC_START(init_kernel_el)
	mrs	x1, CurrentEL
	cmp	x1, #CurrentEL_EL2
	b.eq	init_el2                    // At EL2 → configure and drop

init_el1:
	/* Already at EL1 — just set safe SCTLR value */
	mov_q	x0, INIT_SCTLR_EL1_MMU_OFF
	pre_disable_mmu_workaround
	msr	sctlr_el1, x0
	isb
	mov_q	x0, INIT_PSTATE_EL1
	msr	spsr_el1, x0
	msr	elr_el1, lr
	mov	w0, #BOOT_CPU_MODE_EL1        // Return 0xe11
	eret

init_el2:
	/* Configure EL2 → EL1 transition */
	msr	elr_el2, lr                   // Return address for ERET

	/* If MMU was on (UEFI), clean hypervisor text to PoC */
	cbz	x0, 0f
	adrp	x0, __hyp_idmap_text_start
	adr_l	x1, __hyp_text_end
	adr_l	x2, dcache_clean_poc
	blr	x2

	mov_q	x0, INIT_SCTLR_EL2_MMU_OFF
	pre_disable_mmu_workaround
	msr	sctlr_el2, x0                // Disable EL2 MMU
	isb
0:
	init_el2_hcr  HCR_HOST_NVHE_FLAGS | HCR_ATA   // Configure HCR_EL2
	init_el2_state                                  // Timer, debug, PMU

	adr_l	x0, __hyp_stub_vectors
	msr	vbar_el2, x0                  // Install hypervisor stub vectors
	isb

	mov_q	x1, INIT_SCTLR_EL1_MMU_OFF

	/* Check for VHE (Virtualization Host Extensions) */
	mrs	x0, hcr_el2
	and	x0, x0, #HCR_E2H             // E2H bit → VHE mode
	cbz	x0, 2f

	msr_s	SYS_SCTLR_EL12, x1           // VHE path: write via EL12 alias
	mov	x2, #BOOT_CPU_FLAG_E2H
	b	3f
2:	msr	sctlr_el1, x1                 // non-VHE path: write directly
	mov	x2, xzr
3:	mov	x0, #INIT_PSTATE_EL1
	msr	spsr_el2, x0
	mov	w0, #BOOT_CPU_MODE_EL2        // Return 0xe12
	orr	x0, x0, x2                    // OR in E2H flag if VHE
	eret                                  // ★ DROP FROM EL2 → EL1
SYM_FUNC_END(init_kernel_el)
```

**What happens**: If at EL2, configure hypervisor registers (HCR, timer, debug),
install stub vectors, set up SCTLR_EL1 with MMU off, then `eret` to EL1.
The kernel runs at EL1 from this point forward.

---

## 6. `__cpu_setup()` — CPU System Register Configuration

**Source**: `arch/arm64/mm/proc.S`  
**Called by**: `primary_entry`  
**Returns**: `x0` = `INIT_SCTLR_EL1_MMU_ON` (SCTLR value to enable MMU)

### 6.1 TLB and Feature Reset

```asm
SYM_FUNC_START(__cpu_setup)
	tlbi	vmalle1                  // Invalidate entire local TLB
	dsb	nsh                      // Ensure TLB invalidation completes

	msr	cpacr_el1, xzr           // Reset CPACR (disable FP/SIMD traps)
	mov	x1, MDSCR_EL1_TDCC
	msr	mdscr_el1, x1            // Disable DCC access from EL0
	reset_pmuserenr_el0 x1        // Disable PMU access from EL0
	reset_amuserenr_el0 x1        // Disable AMU access from EL0
```

### 6.2 MAIR_EL1 — Memory Attribute Indirection Register

```asm
	mov_q	mair, MAIR_EL1_SET       // mair is alias for x17
```

Where `MAIR_EL1_SET` defines 5 memory attribute slots:

| AttrIdx | Type | Value | Used For |
|---------|------|-------|----------|
| 0 | `MT_DEVICE_nGnRnE` | `0x00` | Strongly-ordered device memory |
| 1 | `MT_DEVICE_nGnRE` | `0x04` | Device memory (early write ack) |
| 2 | `MT_NORMAL_NC` | `0x44` | Normal non-cacheable |
| 3 | `MT_NORMAL` | `0xFF` | Normal cacheable (WB, RW-Alloc) ← **most RAM** |
| 4 | `MT_NORMAL_TAGGED` | `0xFF` | Normal + MTE tagging (initial = same as 3) |

### 6.3 TCR_EL1 — Translation Control Register

```asm
	mov_q	tcr, TCR_T0SZ(IDMAP_VA_BITS) | TCR_T1SZ(VA_BITS_MIN) | \
	             TCR_CACHE_FLAGS | TCR_SHARED | TCR_TG_FLAGS | \
	             TCR_KASLR_FLAGS | TCR_EL1_AS | TCR_EL1_TBI0 | \
	             TCR_EL1_A1 | TCR_KASAN_SW_FLAGS | TCR_MTE_FLAGS
```

Key fields configured:

| Field | Value | Meaning |
|-------|-------|---------|
| `T0SZ` | `64 - IDMAP_VA_BITS` | TTBR0 VA size for identity map |
| `T1SZ` | `64 - VA_BITS_MIN` | TTBR1 VA size for kernel (48-bit) |
| `IRGN0/1` | WBWA | Page table walks: Write-Back, Write-Allocate |
| `ORGN0/1` | WBWA | Outer cacheability: Write-Back, Write-Allocate |
| `SH0/1` | ISH | Inner Shareable (for SMP coherency) |
| `TG0/TG1` | 4KB | Translation granule |
| `A1` | 1 | ASID is in TTBR1 (kernel selects ASID) |
| `TBI0` | 1 | Top Byte Ignore for TTBR0 (user pointers) |

Dynamic adjustments:
```asm
	tcr_compute_pa_size tcr, #TCR_EL1_IPS_SHIFT, x5, x6
	                                  // Read PA size from ID_AA64MMFR0_EL1.PARange
	                                  // Set IPS field (e.g., 48-bit PA = 0b101)

	/* Hardware Access Flag if supported */
	mrs	x9, ID_AA64MMFR1_EL1
	ubfx	x9, x9, ID_AA64MMFR1_EL1_HAFDBS_SHIFT, #4
	cbz	x9, 1f
	orr	tcr, tcr, #TCR_EL1_HA    // Enable hardware Access flag updates
```

### 6.4 Write Registers and Return

```asm
	msr	mair_el1, mair               // x17 → MAIR_EL1
	msr	tcr_el1, tcr                 // x16 → TCR_EL1

	/* Permission Indirection Extension (if supported) */
	// ... PIE configuration ...

	mov_q	x0, INIT_SCTLR_EL1_MMU_ON   // Return value
	ret
SYM_FUNC_END(__cpu_setup)
```

`INIT_SCTLR_EL1_MMU_ON` contains:
- **M** (bit 0): MMU enable
- **C** (bit 2): D-cache enable
- **I** (bit 12): I-cache enable
- **SA** (bit 3): Stack alignment check
- Plus many security/feature bits (SPAN, EIS, EOS, LSMAOE, etc.)

---

## 7. `__enable_mmu()` — The MMU Enable Sequence

**Source**: `arch/arm64/kernel/head.S`  
**Called by**: `__primary_switch`  
**Arguments**: `x0` = SCTLR value, `x1` = TTBR1 phys, `x2` = TTBR0 phys

```asm
SYM_FUNC_START(__enable_mmu)
	/* Verify page granule is supported by hardware */
	mrs	x3, ID_AA64MMFR0_EL1
	ubfx	x3, x3, #ID_AA64MMFR0_EL1_TGRAN_SHIFT, 4
	cmp	x3, #ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MIN
	b.lt	__no_granule_support            // Panic if not supported
	cmp	x3, #ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MAX
	b.gt	__no_granule_support

	phys_to_ttbr x2, x2                    // Convert PA to TTBR format
	msr	ttbr0_el1, x2                  // ★ LOAD TTBR0 (identity map)

	load_ttbr1 x1, x1, x3                  // ★ LOAD TTBR1 (reserved/kernel)
	//   phys_to_ttbr tmp1, x1
	//   msr ttbr1_el1, tmp1
	//   isb

	set_sctlr_el1 x0                       // ★ ENABLE MMU
	//   msr sctlr_el1, x0      ← M bit set → MMU ON
	//   isb                     ← pipeline flush
	//   ic  iallu               ← invalidate I-cache
	//   dsb nsh                 ← ensure invalidation completes
	//   isb                     ← synchronize

	ret                                     // Return to __primary_switch
SYM_FUNC_END(__enable_mmu)
```

**At this point**:
- TTBR0 = `init_idmap_pg_dir` (identity map: VA == PA)
- TTBR1 = `reserved_pg_dir` (empty — no kernel mappings yet)
- MMU is **ON**
- CPU fetches next instruction using identity map (same PA as before)

---

## 8. `__primary_switch()` — MMU On → Kernel Virtual Address

**Source**: `arch/arm64/kernel/head.S`  
**Called by**: `primary_entry` (tail branch)

```asm
SYM_FUNC_START_LOCAL(__primary_switch)
	adrp	x1, reserved_pg_dir            // x1 = phys of empty reserved page dir
	adrp	x2, __pi_init_idmap_pg_dir     // x2 = phys of identity map page dir
	bl	__enable_mmu                   // ★ MMU ON (explained above)

	/* Now running with MMU on, but still at identity-mapped PA */

	adrp	x1, early_init_stack           // Re-establish stack (adrp gives PA=VA here)
	mov	sp, x1
	mov	x29, xzr                      // Zero frame pointer

	mov	x0, x20                        // x0 = boot_status (from init_kernel_el)
	mov	x1, x21                        // x1 = FDT physical address
	bl	__pi_early_map_kernel          // ★ Build kernel VA mapping (C function)
	                                       //   → maps kernel at 0xFFFF_8xxx...
	                                       //   → switches TTBR1 to swapper_pg_dir

	/* Now TTBR1 points to swapper_pg_dir with kernel mapped at high VA */

	ldr	x8, =__primary_switched        // x8 = VIRTUAL address of __primary_switched
	adrp	x0, KERNEL_START               // x0 = PA of kernel start
	br	x8                             // ★ JUMP TO KERNEL VA
	                                       //   CPU now fetches from TTBR1 high VA
SYM_FUNC_END(__primary_switch)
```

**The critical transition**: After `__pi_early_map_kernel` builds the kernel mapping
and switches TTBR1 to `swapper_pg_dir`, the `ldr x8, =__primary_switched` loads
the **virtual** (link) address of `__primary_switched`. The `br x8` jumps to that
virtual address — the CPU now fetches instructions through TTBR1 at the high kernel
VA (`0xFFFF_xxxx...`).

---

## 9. `__primary_switched()` — Running at Kernel VA

**Source**: `arch/arm64/kernel/head.S`  
**Called by**: `__primary_switch` (branch to kernel VA)  
**Arguments**: `x0` = `__pa(KERNEL_START)`, `x20` = boot mode, `x21` = FDT phys

```asm
SYM_FUNC_START_LOCAL(__primary_switched)
	/* 1. Set up init_task as current and configure kernel stack */
	adr_l	x4, init_task                  // x4 = &init_task (VA)
	init_cpu_task x4, x5, x6
	//   msr  sp_el0, x4                   → current = &init_task
	//   ldr  tmp, [x4, #TSK_STACK]        → tmp = init_task.stack
	//   add  sp, tmp, #THREAD_SIZE        → SP = stack top
	//   sub  sp, sp, #PT_REGS_SIZE        → reserve space for pt_regs
	//   stp  xzr, xzr, [sp, #S_STACKFRAME] → zero final frame record
	//   add  x29, sp, #S_STACKFRAME       → FP = final frame
	//   ldr per-cpu offset, set TPIDR_EL1

	/* 2. Install kernel exception vectors */
	adr_l	x8, vectors                    // x8 = &vectors (VA)
	msr	vbar_el1, x8                   // VBAR_EL1 = exception vector table
	isb

	/* 3. Save frame */
	stp	x29, x30, [sp, #-16]!
	mov	x29, sp

	/* 4. Store FDT pointer in global variable */
	str_l	x21, __fdt_pointer, x5         // __fdt_pointer = x21 (FDT phys addr)

	/* 5. Compute and save VA-PA offset */
	adrp	x4, _text                      // x4 = VA of _text
	sub	x4, x4, x0                    // x4 = VA(_text) - PA(_text)
	str_l	x4, kimage_voffset, x5         // kimage_voffset = VA-PA offset

	/* 6. Record boot CPU mode */
	mov	x0, x20                        // x0 = boot mode (EL1 or EL2)
	bl	set_cpu_boot_mode_flag         // Store to __boot_cpu_mode[]

	/* 7. KASAN early init (if configured) */
#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)
	bl	kasan_early_init
#endif

	/* 8. Finalize EL2 state */
	mov	x0, x20
	bl	finalise_el2

	/* 9. ★ ENTER C CODE — NEVER RETURNS */
	ldp	x29, x30, [sp], #16
	bl	start_kernel                   // → init/main.c start_kernel()
	ASM_BUG()                              // Should never reach here
SYM_FUNC_END(__primary_switched)
```

**Key global variables set**:

| Variable | Value | Defined In |
|----------|-------|-----------|
| `__fdt_pointer` | Physical address of DTB | `arch/arm64/kernel/setup.c` |
| `kimage_voffset` | VA(_text) - PA(_text) | `arch/arm64/kernel/setup.c` |
| `__boot_cpu_mode[0]` | `BOOT_CPU_MODE_EL1` or `EL2` | `arch/arm64/kernel/head.S` |

**After `bl start_kernel`**: We are in C code. The kernel stack is `init_task.stack`,
`current` points to `init_task`, exception vectors are installed, and the MMU is on
with the kernel mapped at its link address via `swapper_pg_dir`.

---

## State After Document 01

```
┌──────────────────────────────────────────────────────────┐
│  CPU State:                                              │
│    EL1, MMU ON, D-cache ON, I-cache ON                   │
│    SP = init_task kernel stack                           │
│    current = &init_task                                  │
│    VBAR_EL1 = kernel exception vectors                   │
│                                                          │
│  Translation Tables:                                     │
│    TTBR0 = init_idmap_pg_dir (identity map, VA=PA)       │
│    TTBR1 = swapper_pg_dir (kernel at 0xFFFF_xxxx)        │
│                                                          │
│  System Registers:                                       │
│    MAIR_EL1 = 5 attribute slots configured               │
│    TCR_EL1 = 4K granule, 48-bit VA, ISH, WBWA            │
│    SCTLR_EL1 = MMU+cache+alignment checks ON             │
│                                                          │
│  Global Variables:                                       │
│    __fdt_pointer = PA of DTB                             │
│    kimage_voffset = VA - PA offset                       │
│    boot_args[0..3] = original x0-x3                      │
│                                                          │
│  Memory Subsystem:                                       │
│    ✗ memblock NOT initialized                            │
│    ✗ No knowledge of RAM size/layout                     │
│    ✗ Linear map NOT created                              │
│    ✗ No allocator available                              │
│                                                          │
│  Next: start_kernel() → C code begins                    │
└──────────────────────────────────────────────────────────┘
```

---

[← Index](00_Index.md) | **Doc 01** | [Doc 02: Identity Map & Kernel Map →](02_Identity_Map_and_Kernel_Map.md)
