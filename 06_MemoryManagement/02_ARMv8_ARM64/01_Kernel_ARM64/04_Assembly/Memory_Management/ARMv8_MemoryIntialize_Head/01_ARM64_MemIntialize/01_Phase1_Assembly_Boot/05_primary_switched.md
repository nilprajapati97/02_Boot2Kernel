# `__primary_switched()` — Transition to C Code

**Source:** `arch/arm64/kernel/head.S` (after `__primary_switch`)
**Phase:** Assembly Boot (MMU ON, Virtual Addresses)
**Memory Allocator:** None
**Called by:** `__primary_switch()` via `br x8`
**Calls:** `kasan_early_init()`, `start_kernel()`

---

## What This Function Does

This is the **last assembly function** before the kernel enters C code. With the MMU now on and the CPU running at kernel virtual addresses, this function:

1. Sets up the **init_task kernel stack** (the first process's stack)
2. Installs the **exception vector table** (VBAR_EL1)
3. Records the **kernel image virtual-to-physical offset**
4. Calls `start_kernel()` — the first C function

---

## How It Works With Memory

### Memory Used (All Pre-Allocated)

| Memory | Source | Purpose |
|--------|--------|---------|
| `init_task` stack | Statically allocated in `init/init_task.c` | First kernel thread's stack |
| `vectors` | `.text` section | Exception vector table |
| `kimage_voffset` | `.data` section | Virtual-to-physical offset variable |

**No dynamic allocation.** Everything is statically placed by the linker.

---

## Step-by-Step Execution

### Step 1: Set Up init_task Stack

```asm
    adr_l  x4, init_task              // x4 = &init_task (virtual address)
    ldr    w5, [x4, #TSK_TI_FLAGS]    // Load thread_info flags
    and    w5, w5, #_TIF_NEED_RESCHED // Check flags

    ldr    x8, [x4, #TSK_STACK]       // x8 = init_task.stack
    add    sp, x8, #THREAD_SIZE       // SP = top of stack
    mov    x29, xzr                   // Clear frame pointer (no caller)
```

**What happens:**
- `init_task` is the first process (PID 0, the idle/swapper process)
- Its stack is a `THREAD_SIZE` region (typically 16 KB on arm64)
- Stack grows downward, so SP is set to the **top** (highest address)
- This replaces the temporary `early_init_stack` used during Phase 1

```
init_task.stack:
┌────────────────────┐ SP = stack + THREAD_SIZE (high)
│                    │
│  Available stack   │ ← Stack grows downward
│     space          │
│                    │
├────────────────────┤ stack + sizeof(thread_info)
│  struct thread_info│ ← At bottom of stack
└────────────────────┘ stack (low)
```

### `init_task` — The First Process

`init_task` is statically defined in `init/init_task.c`:

```c
struct task_struct init_task = {
    .thread_info = INIT_THREAD_INFO(init_task),
    .stack       = init_stack,          // Points to init_stack BSS region
    .state       = 0,                   // TASK_RUNNING
    .prio        = MAX_PRIO - 20,
    .comm        = INIT_TASK_COMM,      // "swapper"
    .mm          = NULL,                // Kernel thread, no user mm
    .active_mm   = &init_mm,            // Kernel address space
    ...
};
```

- This is NOT dynamically allocated — it's a global variable in `.data`
- Its stack (`init_stack`) is a global array in BSS: `THREAD_SIZE` bytes

---

### Step 2: Install Exception Vector Table

```asm
    adr_l  x5, vectors
    msr    vbar_el1, x5              // VBAR_EL1 = exception vector table
```

**What it does:**
- `vectors` is the exception vector table defined in `arch/arm64/kernel/entry.S`
- VBAR_EL1 (Vector Base Address Register) tells the CPU where to jump on exceptions
- After this, any exception (page fault, interrupt, syscall, etc.) is handled by the kernel

**Memory relevance:** Once VBAR is set, page faults during boot will be caught and reported (instead of hanging silently). This is essential for debugging memory issues during `setup_arch()`.

### Exception Vector Table Layout

```
VBAR_EL1 → vectors:
┌──────────────────────────────────┐ Offset
│ Synchronous (current EL, SP_EL0)│ 0x000
│ IRQ          (current EL, SP_EL0)│ 0x080
│ FIQ          (current EL, SP_EL0)│ 0x100
│ SError       (current EL, SP_EL0)│ 0x180
├──────────────────────────────────┤
│ Synchronous (current EL, SP_ELx)│ 0x200  ← Kernel uses this
│ IRQ          (current EL, SP_ELx)│ 0x280
│ FIQ          (current EL, SP_ELx)│ 0x300
│ SError       (current EL, SP_ELx)│ 0x380
├──────────────────────────────────┤
│ Synchronous (lower EL, AArch64) │ 0x400  ← User syscalls/faults
│ IRQ          (lower EL, AArch64) │ 0x480
│ FIQ          (lower EL, AArch64) │ 0x500
│ SError       (lower EL, AArch64) │ 0x580
├──────────────────────────────────┤
│ (lower EL, AArch32 — if compat) │ 0x600-0x780
└──────────────────────────────────┘
```

Each vector entry is 128 bytes (32 instructions), enough for initial dispatch.

---

### Step 3: Record Kernel Image Offset

```asm
    adrp  x4, _text                  // x4 = virtual address of _text
    ldr   x5, =_text                 // x5 = link address of _text
    sub   x9, x4, x5                // x9 = actual VA - link VA (KASLR offset)

    adrp  x4, kimage_voffset
    str   x9, [x4, :lo12:kimage_voffset]  // Store the offset
```

**`kimage_voffset`** = virtual_address - physical_address for the kernel image.

This is used later by:
- `__pa_symbol(x)` — convert kernel symbol virtual address to physical: `x - kimage_voffset`
- `__va(x)` — convert physical to virtual (for linear map, different from kimage_voffset)

**With KASLR (Kernel Address Space Layout Randomization):**
- The kernel may be loaded at a random physical address
- `kimage_voffset` captures this random offset so translations work correctly

---

### Step 4: KASAN Early Init (if enabled)

```asm
#ifdef CONFIG_KASAN_GENERIC
    bl    kasan_early_init           // Shadow memory setup for KASAN
#endif
```

**KASAN (Kernel Address Sanitizer):**
- A runtime memory error detector (out-of-bounds, use-after-free)
- Requires a **shadow memory** region: 1/8th of the kernel VA space
- `kasan_early_init()` creates page table entries for the shadow region
- Maps the entire shadow to a single zero page initially (lazy allocation)

**Memory impact:** Allocates page table entries from memblock (or static BSS) to map the shadow region. Actual shadow pages are allocated later on demand.

---

### Step 5: Enter C Code — `start_kernel()`

```asm
    bl    start_kernel               // >>> ENTER C CODE <<<

    // Should never return. If it does:
    ASM_BUG()                        // Trigger a BUG (crash with diagnostic)
```

**This is the handoff.** From here, the kernel is in C code, executing `start_kernel()` in `init/main.c`.

---

## State at Handoff to start_kernel()

### CPU State

| Register/State | Value | Notes |
|---------------|-------|-------|
| SP | Top of `init_task` stack | 16 KB stack, grows down |
| PC | `start_kernel` (virtual) | C function in init/main.c |
| SCTLR_EL1.M | 1 | MMU enabled |
| SCTLR_EL1.C | 1 | D-cache enabled |
| SCTLR_EL1.I | 1 | I-cache enabled |
| VBAR_EL1 | `vectors` | Exception handling ready |
| TTBR0_EL1 | `init_idmap_pg_dir` | Identity map (temporary) |
| TTBR1_EL1 | `init_pg_dir` | Kernel image mapping |
| MAIR_EL1 | Configured | 8 memory types defined |
| TCR_EL1 | Configured | Translation parameters set |

### Memory State

```
Virtual Address Space:

0xFFFF_FFFF_FFFF_FFFF ┐
                       │  Kernel image mapped (init_pg_dir)
                       │  .text (RO+X), .data (RW+NX), .bss (RW+NX)
                       │
0xFFFF_FF80_0000_0000 ┤
                       │
                       │  REST OF KERNEL VA SPACE: UNMAPPED
                       │  (no linear map yet — that's paging_init's job)
                       │
0xFFFF_0000_0000_0000 ┘ PAGE_OFFSET (nothing mapped here yet)

- - - - - - - - - - - - - - - - - - - -

0x0000_FFFF_FFFF_FFFF ┐
                       │  Identity map (init_idmap_pg_dir)
                       │  Kernel image at physical addresses
0x0000_0000_4080_0000 ┤  (only ~30MB mapped)
                       │
0x0000_0000_0000_0000 ┘
```

### What's NOT Available Yet

| Feature | Status | When Available |
|---------|--------|---------------|
| Linear map (all RAM) | Not created | Phase 2: `paging_init()` |
| Memory allocator | None | Phase 2: `memblock` after DTB parse |
| Page fault handling | Basic (BUG) | Phase 2: proper handlers |
| Per-CPU areas | Not set up | Phase 2: `setup_per_cpu_areas()` |
| Interrupts | Masked | Phase 2: `local_irq_enable()` |

---

## Transition Summary

```
Phase 1 Complete:
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  Bootloader  ──►  primary_entry()  ──►  __cpu_setup()
  (MMU OFF)        (MMU OFF, BSS)       (Register config)
      │                                       │
      │                                       ▼
      │                              __primary_switch()
      │                                       │
      │                              __enable_mmu()
      │                              SCTLR.M=1 ──► MMU ON
      │                                       │
      │                              __pi_early_map_kernel()
      │                              init_pg_dir populated
      │                                       │
      │                              br x8 ──► VIRTUAL ADDRESS
      │                                       │
      │                              __primary_switched()
      │                              SP = init_task stack
      │                              VBAR = vectors
      │                                       │
      └──────────────────────────────────────► start_kernel()
                                              (Phase 2 begins)
```

---

## Key Takeaways

1. **`init_task` is the ancestor** — every kernel thread and user process ultimately descends from PID 0
2. **Exception vectors enable diagnostics** — without VBAR, any boot crash is silent
3. **`kimage_voffset` enables VA↔PA translation** — critical for all subsequent memory operations
4. **KASLR offset is captured here** — if the kernel was randomly placed, this records where
5. **The linear map doesn't exist yet** — only the kernel image is mapped. `paging_init()` in Phase 2 will map all RAM
