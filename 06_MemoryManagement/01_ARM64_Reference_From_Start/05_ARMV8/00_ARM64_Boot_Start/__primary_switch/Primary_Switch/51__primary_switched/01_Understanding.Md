This is a fragment of **ARM64 Linux kernel boot code**, written in assembly. It runs **very early during kernel startup**, right after the MMU (Memory Management Unit) is enabled and the system has switched to virtual addressing.

Let’s walk through it in a clear, structured way.

---

## 🔹 What is `__primary_switched`?

`SYM_FUNC_START_LOCAL(__primary_switched)` defines a **local symbol/function** in the Linux kernel.

This function is executed:

* After early boot setup
* After enabling the MMU
* On the **primary CPU (boot CPU)**

At this point, the system is transitioning into the full kernel environment.

---

## 🔹 Step-by-step Explanation

### 1. Initialize the initial task (`init_task`)

```asm
adr_l x4, init_task
init_cpu_task x4, x5, x6
```

* `adr_l` loads the **address of `init_task`** (the first process, PID 0).
* `init_cpu_task` sets up the **task structure for the boot CPU**.

👉 This establishes the **initial process context**.

---

### 2. Set exception vector table

```asm
adr_l x8, vectors
msr vbar_el1, x8
isb
```

* Loads address of `vectors` (exception handlers).
* Writes it to `VBAR_EL1` (Vector Base Address Register).
* `isb` ensures the change takes effect immediately.

👉 This tells the CPU:
“Use this table when handling interrupts/exceptions.”

---

### 3. Set up stack frame

```asm
stp x29, x30, [sp, #-16]!
mov x29, sp
```

* Saves frame pointer (`x29`) and return address (`x30`) on stack.
* Sets up a **new stack frame**.

👉 Standard function prologue.

---

### 4. Save Device Tree pointer

```asm
str_l x21, __fdt_pointer, x5
```

* Stores `x21` into `__fdt_pointer`.
* `x21` contains the **FDT (Flattened Device Tree) pointer** passed by the bootloader.

👉 The kernel uses this to discover hardware.

---

### 5. Compute virtual–physical offset

```asm
adrp x4, _text
sub x4, x4, x0
str_l x4, kimage_voffset, x5
```

* `_text` = virtual address of kernel code
* `x0` = physical address of kernel start (`__pa(KERNEL_START)`)

So:

```
voffset = virtual_address - physical_address
```

👉 This offset is used for translating between:

* physical memory
* kernel virtual addresses

---

### 6. Set CPU boot mode flag

```asm
mov x0, x20
bl set_cpu_boot_mode_flag
```

* Passes boot mode info (in `x20`)
* Calls a function to record how the CPU was started

👉 Helps kernel know if it booted in EL1, EL2, etc.

---

### 7. KASAN early init (optional)

```asm
bl kasan_early_init
```

Only if enabled:

* KASAN initializes early memory debugging

👉 Detects memory bugs like buffer overflows.

---

### 8. Finalize EL2 (Hypervisor level)

```asm
mov x0, x20
bl finalise_el2
```

* Configures **Exception Level 2 (EL2)** if present
* Enables VHE (Virtualization Host Extensions) if possible

👉 Prepares system for virtualization support.

---

### 9. Restore stack frame

```asm
ldp x29, x30, [sp], #16
```

* Restores saved registers
* Cleans up stack

---

### 10. Jump to main kernel

```asm
bl start_kernel
```

* Transfers control to:
  👉 start_kernel

This is the **main C entry point of the Linux kernel**.

---

### 11. Safety fallback

```asm
ASM_BUG()
```

* If execution ever returns here → crash intentionally

👉 Because `start_kernel()` should **never return**.

---

## 🔹 Big Picture

This function is the **final bridge between early assembly boot and the full Linux kernel**.

### It does:

* Setup CPU task
* Install exception handlers
* Save bootloader data (FDT)
* Establish memory mapping offsets
* Initialize debugging features (KASAN)
* Configure privilege levels (EL2)
* Jump into C kernel (`start_kernel`)

---

## 🔹 Simplified Flow

```
Bootloader → MMU ON → __primary_switched →
    setup CPU + stack
    setup exception vectors
    save device tree
    compute address offsets
    init debug + EL2
    → start_kernel() (C world begins)
```

---

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
This document describes a stage in the ARMv8-A Linux ARM64 boot path. ARMv8-A is the 64-bit ARM architecture (AArch64 execution state) introduced with the ARM Cortex-A53/A57 generation. Key architectural features relevant to boot:
- Exception levels: EL0 (user), EL1 (OS kernel), EL2 (hypervisor), EL3 (secure monitor).
- Two-stage translation: TTBR0_EL1 (user/low VA) and TTBR1_EL1 (kernel/high VA).
- System registers accessed via MRS/MSR instructions (not memory-mapped).
- PSTATE: condition flags + CPU mode + interrupt mask bits.
- Mandatory ISB after system register writes that affect instruction fetch.

### Kernel Perspective (Linux ARM64)
The Linux ARM64 boot path follows this sequence:
  stext (head.S) -> __primary_switch -> __pi_early_map_kernel -> __enable_mmu
  -> __primary_switched -> start_kernel -> setup_arch -> paging_init
Each stage initializes one more layer of the memory system. Before start_kernel, all memory management is done with physical addresses or the early identity/kernel maps. After paging_init(), the full kernel virtual memory map is active.

### Memory Perspective (ARMv8 Memory Model)
The ARMv8 memory model (based on the ARM ARM's "Arm Memory Model" chapter) defines:
- Normal memory: cacheable, reorderable, speculatable. Used for DRAM (kernel code, data, stack, heap).
- Device memory: non-cacheable, strictly ordered. Used for MMIO (UART, GIC, etc.).
- Barriers: DSB (Data Synchronization Barrier), DMB (Data Memory Barrier), ISB (Instruction Synchronization Barrier) enforce ordering guarantees.
At boot, the kernel transitions from a world where every address is physical (pre-MMU) to the full ARMv8 virtual memory model where TTBR0 and TTBR1 map the user and kernel address spaces respectively.