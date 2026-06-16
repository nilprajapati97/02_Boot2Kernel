## `__primary_switched` — Simple Explanation

### Flow First

```
MMU is ON → Virtual addresses now work
        ↓
[1] Set init_task as current CPU task (idle process)
        ↓
[2] Install exception vector table (VBAR_EL1)
        ↓
[3] Create a stack frame (for stack unwinding)
        ↓
[4] Save FDT (Device Tree) pointer globally
        ↓
[5] Calculate & save kimage_voffset (virtual - physical offset)
        ↓
[6] Record how we booted (EL1 or EL2)
        ↓
[7] (Optional) KASAN early init (if memory sanitizer enabled)
        ↓
[8] Finalize EL2 / VHE setup
        ↓
[9] → start_kernel()  ← C code begins here
```

---

### Line-by-Line Explanation

**Step 1 — Set up the CPU's current task as `init_task`**
```asm
adr_l   x4, init_task
init_cpu_task x4, x5, x6
```
- `init_task` is the **idle process** (PID 0), the very first task Linux creates.
- `init_cpu_task` macro sets up `sp_el0` (points to current task), sets up the kernel stack, initializes per-CPU offset, etc.
- Basically: *"This CPU now has a proper task context."*

---

**Step 2 — Install the exception vector table**
```asm
adr_l   x8, vectors
msr     vbar_el1, x8    // VBAR = Vector Base Address Register
isb
```
- `vectors` is the ARM64 exception vector table (handles IRQs, data aborts, syscalls, etc.)
- Writing to `VBAR_EL1` tells the CPU: *"when an exception fires, jump to this address."*
- `isb` — flushes the pipeline to make the register write take effect immediately.

---

**Step 3 — Build a stack frame**
```asm
stp     x29, x30, [sp, #-16]!
mov     x29, sp
```
- `x29` = frame pointer, `x30` = link register (return address).
- Pushes them onto the stack and sets `x29` to current `sp`.
- This lets stack unwinders (like `backtrace`) know *"this is the bottom of the call stack."*

---

**Step 4 — Save the FDT pointer**
```asm
str_l   x21, __fdt_pointer, x5
```
- `x21` has been holding the **FDT (Flattened Device Tree) address** since `primary_entry`.
- Now that we have a global variable (`__fdt_pointer`), we save it there so C code can access it later.

---

**Step 5 — Calculate `kimage_voffset`**
```asm
adrp    x4, _text       // virtual address of kernel text
sub     x4, x4, x0      // x0 = physical address of KERNEL_START
str_l   x4, kimage_voffset, x5
```
- The kernel is loaded at a physical address, but runs at a virtual address.
- `kimage_voffset` = virtual - physical offset.
- Used everywhere in the kernel to convert between physical and virtual addresses (`__phys_to_virt`, `__virt_to_phys`).

---

**Step 6 — Record CPU boot mode**
```asm
mov     x0, x20
bl      set_cpu_boot_mode_flag
```
- `x20` = boot mode (`BOOT_CPU_MODE_EL1` or `BOOT_CPU_MODE_EL2`).
- Saves this in `__boot_cpu_mode` so the kernel knows whether it booted from EL1 or EL2.

---

**Step 7 — KASAN early init (conditional)**
```asm
#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)
    bl  kasan_early_init
#endif
```
- KASAN = **Kernel Address Sanitizer** — a memory bug detector.
- Only compiled in if KASAN is configured. Sets up shadow memory early.

---

**Step 8 — Finalize EL2 / VHE**
```asm
mov     x0, x20
bl      finalise_el2        // Prefer VHE if possible
```
- VHE = **Virtualization Host Extensions** — lets the host kernel run at EL2 directly (better performance for KVM).
- This call decides: *"Should we use VHE or fall back to nVHE?"*

---

**Step 9 — Jump to `start_kernel`**
```asm
ldp     x29, x30, [sp], #16
bl      start_kernel
ASM_BUG()
```
- Restore frame/link registers (clean stack frame).
- `bl start_kernel` — this is the **point of no return**. The C kernel (main.c) takes over from here.
- `ASM_BUG()` — a panic/assert macro. If `start_kernel` ever returns (it shouldn't), the kernel crashes intentionally. This should **never** be reached.

---

### One-line Summary for Interview

> `__primary_switched` is the final assembly setup function after the MMU is turned on. It establishes the initial task, exception vectors, saves key boot parameters, then calls `start_kernel()` to hand control to C code — effectively bridging low-level hardware init and the main kernel.You've used 82% of your weekly rate limit.