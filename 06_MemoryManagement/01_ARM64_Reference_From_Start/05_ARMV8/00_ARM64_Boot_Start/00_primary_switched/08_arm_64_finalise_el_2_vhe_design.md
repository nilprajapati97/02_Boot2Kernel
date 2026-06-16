# ARM64 Early Boot Deep Design: `finalise_el2` (VHE Enable Path)

## Code Snippet

```asm
bl finalise_el2   // Prefer VHE if possible
```

---

## One-Line Summary

Attempts to transition the Linux kernel from EL1 to EL2 using **VHE (Virtualization Host Extensions)** so the kernel runs in hypervisor mode for better virtualization performance.

---

# 1. Boot Context (Critical for Interview)

This runs inside `__primary_switched`:

At this moment:

- MMU is ON
- Kernel is running in virtual address space
- CPU is currently executing at **EL1**
- Bootloader may have started CPU at **EL2**

Goal:

👉 If system supports it → **upgrade kernel to EL2 (VHE mode)**

---

# 2. Why This Exists

Modern systems (NVIDIA / AMD / ARM servers) heavily use virtualization.

Two modes exist:

## Without VHE (nVHE)

- Kernel runs at EL1
- Hypervisor runs at EL2
- Every VM entry/exit requires EL switch

❌ High overhead

## With VHE

- Kernel runs at EL2
- No EL switching required for KVM

✅ Faster virtualization

---

# 3. High-Level Flow

```text
__primary_switched (EL1)
   │
   ├── bl finalise_el2
   │       │
   │       ├── Check boot mode (EL2 required)
   │       ├── Check CPU supports VHE
   │       ├── Trigger HVC → switch to EL2
   │       │
   │       └── __finalise_el2 (runs at EL2)
   │               │
   │               ├── Configure HCR_EL2 (E2H + TGE)
   │               ├── Copy EL1 state → EL2
   │               ├── Enable MMU at EL2
   │               └── Return at EL2
   │
   └── Continue boot (now at EL2 if success)
```

---

# 4. Step-by-Step Deep Dive

## Step 1: Call from EL1

```asm
bl finalise_el2
```

- Standard function call
- `x30` stores return address

---

## Step 2: Boot Mode Check

Kernel checks:

```text
Did we boot in EL2?
```

If booted in EL1 → cannot upgrade → return immediately

---

## Step 3: Trigger Hypercall

```asm
hvc #0
```

- Causes exception → switches CPU to EL2
- Handled by **hyp stub vectors**

---

## Step 4: Execution at EL2 (`__finalise_el2`)

Now CPU is running at EL2.

---

## Step 5: Check VHE Support

Checks CPU register:

```text
ID_AA64MMFR1_EL1.VH bit
```

- VH = 1 → VHE supported
- VH = 0 → fallback (stay EL1)

---

## Step 6: Configure HCR_EL2 (Critical)

```text
HCR_EL2 = E2H + TGE + RW + interrupts
```

Key bits:

| Bit | Meaning |
|-----|--------|
| E2H | EL2 behaves like EL1 (host mode) |
| TGE | All EL0 exceptions go to EL2 |
| RW  | 64-bit execution |

👉 This makes EL2 behave like normal kernel mode

---

## Step 7: State Migration (EL1 → EL2)

Copies critical registers:

- TTBR0/TTBR1 (page tables)
- TCR (translation control)
- MAIR (memory attributes)
- VBAR (exception vectors)
- SP (stack pointer)

👉 Ensures kernel continues seamlessly at EL2

---

## Step 8: MMU Handling

- Flush TLB
- Enable MMU at EL2
- Disable EL1 MMU (no longer needed)

---

## Step 9: Return Using `eret`

```text
Return to EL2h mode (not EL1)
```

👉 Kernel now permanently runs at EL2

---

# 5. Final System State

## Before

```text
EL2 → unused
EL1 → kernel
```

## After

```text
EL2 → kernel (VHE)
EL1 → not used
```

---

# 6. Why NVIDIA / AMD Care

This is very important in real systems:

### NVIDIA

- GPU virtualization
- Jetson / embedded hypervisors
- Memory isolation

### AMD / Server Systems

- Cloud virtualization (KVM)
- High VM density
- Low latency VM switching

👉 VHE reduces VM entry/exit overhead significantly

---

# 7. Failure Path

If any condition fails:

- Not booted in EL2
- No VHE support
- Disabled via kernel flag

Then:

```text
Return → stay at EL1
```

Kernel still works, but slower virtualization

---

# 8. CPU Perspective

- `bl` → function call
- `hvc` → exception to EL2
- `eret` → return at higher privilege

---

# 9. Key Insight (Interview Gold)

This is NOT just a function call.

👉 It is a **privilege-level upgrade mechanism**.

It transforms:

```text
Normal OS → Hypervisor-level OS
```

---

# 10. Interview Explanation

> The instruction `bl finalise_el2` attempts to upgrade the kernel from EL1 to EL2 using ARM Virtualization Host Extensions. If the system booted in EL2 and the CPU supports VHE, the kernel triggers an HVC call to switch to EL2, configures HCR_EL2 with E2H and TGE bits, migrates EL1 state to EL2, and resumes execution at EL2. This eliminates the need for privilege switching during virtualization, significantly improving performance for KVM workloads. If VHE is not supported, the kernel continues running at EL1.

---

# 11. Short Answer

> It upgrades the kernel to run at EL2 using VHE for better virtualization performance.


Compacted conversation## `finalise_el2` — Deep Technical Explanation for an NVIDIA Interview

---

### 1. The Problem It Solves: Why VHE Exists

ARM's original exception level model was designed when hypervisors and OS kernels were written as separate software layers. The assumption was: **the kernel lives at EL1, the hypervisor lives at EL2, and they are fundamentally different pieces of software**.

This created a serious performance problem when Linux was used as a KVM host. Every time a virtual machine needed to enter or exit, the CPU had to transition between EL1 (where the Linux host kernel lives) and EL2 (where the KVM hypervisor code lives). That transition is not free — it requires saving and restoring hundreds of system registers, flushing pipeline state, and issuing ISBs. On a workload with thousands of VM exits per second (which is normal — every I/O, every interrupt, every privileged instruction in the guest causes one), this was measurably expensive.

**VHE (Virtualization Host Extensions)**, introduced in ARMv8.1-A, solves this by allowing the kernel itself to run at EL2 permanently, while still behaving as if it were at EL1 through a register aliasing trick. `finalise_el2` is the boot-time function that performs this transition from the initial EL1 execution context to permanent EL2 execution.

---

### 2. The Boot State Before `finalise_el2` Is Called

To understand what `finalise_el2` does, you need to understand the exact state of the CPU when it is called.

The primary boot path is: `primary_entry` → `init_kernel_el` → `__primary_switch` → `__primary_switched` → `finalise_el2`.

By the time `finalise_el2` is called, the MMU is **on**, the kernel is running with virtual addresses, and the CPU is executing at **EL1** — even if the bootloader delivered the CPU at EL2. This is because `init_kernel_el` (called much earlier) performed an `eret` from EL2 back to EL1 to set up a clean, known EL1 execution environment. The boot mode is stored in register `x20` — either `BOOT_CPU_MODE_EL1` (`0xe11`) or `BOOT_CPU_MODE_EL2` (`0xe12`).

So at the point `finalise_el2` is called, the kernel is at EL1 with a fully working MMU, page tables, and stack. The `x20` register tells it whether the bootloader originally came from EL2 (meaning EL2 is available and we can upgrade) or from EL1 (meaning there is no hypervisor support and we stay at EL1 forever).

---

### 3. The Two-Level Structure of `finalise_el2`

The function has a two-level design that is worth understanding precisely:

**Level 1 (EL1 wrapper):** Checks two guard conditions. First, was the bootloader at EL2? If the boot came from EL1, there is no hypervisor layer available at all, and no HVC will reach EL2 — so it returns immediately with no effect. Second, is the CPU currently at EL1? This is a safety check for re-entrancy. If both guards pass, it issues an `hvc #0` with the `HVC_FINALISE_EL2` selector value of 3 in `x0`.

**Level 2 (EL2 handler):** The `hvc #0` traps through `VBAR_EL2` — which was set to `__hyp_stub_vectors` by `init_kernel_el` — into the stub's synchronous exception handler. This handler dispatches on the `x0` value. When it sees 3, it calls `__finalise_el2`, which is the actual VHE upgrade logic running at EL2.

This split is architecturally forced: the CPU register `HCR_EL2` is only writable from EL2. You cannot set `HCR_EL2.E2H` from EL1. So the kernel must use a hypercall to get the hypervisor stub to do it on its behalf.

---

### 4. The VHE Capability Check

Before committing to the upgrade, `__finalise_el2` performs three critical checks:

**Check 1 — MMU state:** The EL2 MMU must be off at this point. This is checked by reading `SCTLR_EL2.M`. If the EL2 MMU were on with its own page tables, switching `HCR_EL2.E2H` would invalidate the meaning of all EL2 system register accesses mid-flight, causing a guaranteed crash. The invariant here is that `init_kernel_el` disabled the EL2 MMU before dropping to EL1, so this check should always pass on a correctly booted system.

**Check 2 — CPU feature:** The CPU must report `ID_AA64MMFR1_EL1.VH = 1`. Not all ARMv8 implementations support VHE. The A53 and A55 do not. The A55 and earlier Cortex cores that are common in embedded designs lack this feature. An interviewer might ask: why is this a runtime check and not a compile-time Kconfig option? The answer is that Linux must produce a single kernel binary that boots on both VHE and non-VHE hardware — it is the same `Image` file. The decision is deferred to runtime based on what the CPU advertises in its ID registers.

**Check 3 — Software override:** The kernel command line can pass `arm64.nohvhe`, which sets a software feature override bit. This is a debugging/compatibility escape hatch. Some early hypervisor implementations had bugs in their VHE paths, and this flag lets a developer disable VHE even on capable hardware.

---

### 5. What `HCR_EL2` Setting Actually Does

When all three checks pass, `HCR_EL2` is written with `HCR_HOST_VHE_FLAGS`. This is the single most important write of the entire function. Understanding each bit is essential:

**`E2H` (bit 34) — The core VHE bit.** When set, it activates the system register aliasing: accesses to EL1-named registers (`SCTLR_EL1`, `TCR_EL1`, `TTBR0_EL1`, `TTBR1_EL1`, etc.) from code running at EL2 are transparently redirected to the EL2 equivalents (`SCTLR_EL2`, `TCR_EL2`, etc., or new `*_EL12` shadow registers). This is how the kernel binary, which was compiled to write `SCTLR_EL1`, can run at EL2 without modification — every such write now programs the EL2 control register through the alias.

**`TGE` (bit 27) — Trap General Exceptions.** When `E2H=1` and `TGE=1`, all EL0 exceptions are routed directly to EL2 rather than EL1. Without `TGE`, a syscall from userspace would go to EL1 first, requiring an expensive re-routing. With `TGE`, userspace exceptions land directly at EL2 where the kernel now lives — eliminating one level of indirection.

**`RW` (bit 31) — Register Width.** Sets the execution state of EL1 below the hypervisor to AArch64. Required for compatibility.

**`AMO/IMO/FMO` (bits 5/4/3) — Asynchronous/IRQ/FIQ Mask Override.** These cause physical SError, IRQ, and FIQ signals to be taken to EL2 rather than being potentially masked at EL1. This is critical for the interrupt model: with the kernel at EL2, it must receive all interrupts directly. If these were not set, interrupts targeting EL1 would require a round-trip through the EL1→EL2 exception path even though the kernel is running at EL2.

---

### 6. The State Transfer Problem

After setting `HCR_EL2`, there is a subtle problem: the kernel has been running at EL1 with its MMU on and its system registers configured. Page tables are loaded in `TTBR0_EL1` and `TTBR1_EL1`. Exception vectors are in `VBAR_EL1`. Stack pointer was set up. But with `E2H` set, those EL1-named registers now refer to EL2 physical registers — the contents of the actual EL2 hardware registers are **different** (they were configured for the hypervisor stub, not for the kernel).

The solution is to explicitly copy the EL1 state into EL2. This is done through the `*_EL12` shadow register namespace that `E2H` exposes. When `E2H=1`, `SYS_TTBR0_EL12` refers to what was `TTBR0_EL1` before VHE, and `TTBR0_EL1` now refers to `TTBR0_EL2`. So the code reads the shadow copies (`*_EL12`) and writes them to the EL1-named registers (which now program EL2 hardware). The transfers cover: page table bases, memory attribute indirection register, translation control register, VBAR (exception vectors), and the stack pointer.

On ARMv8.9 and newer hardware with Permission Indirection Extensions (PIE), there are additional registers (`TCR2`, `PIRE0`, `PIR`) that must also be transferred.

---

### 7. The `spsr_el1` Patch — The Most Elegant Detail

The current execution is about to `eret` from EL2 back to wherever `spsr_el1` says to return. Before the transfer, `spsr_el1` was set to return to EL1 (as part of the original `init_kernel_el` setup). After VHE is active, returning to EL1 would be wrong — the kernel should be at EL2.

So `__finalise_el2` modifies `spsr_el1` in-place: it clears the `MODE` field and writes `PSR_MODE_EL2h`. When `eret` executes, the CPU reads this patched SPSR and returns to EL2 instead of EL1. This is the mechanism by which the kernel's execution level is permanently changed — not by a direct privilege escalation instruction, but by patching the saved processor state that the `eret` restores.

The `h` suffix in `EL2h` means the stack pointer used is `SP_EL2` (the EL2-dedicated stack pointer), not `SP_EL0`. This distinction matters because the kernel's stack was set up as `SP_EL2` during the state transfer step.

---

### 8. The `enter_vhe` Trampoline and Why It Lives in `.idmap.text`

After all state is prepared, execution jumps to `enter_vhe`. This function is placed in `.idmap.text` — the identity-mapped memory section where virtual addresses equal physical addresses. This is non-obvious at first, but the reason is the TLB flush.

`enter_vhe` issues a `TLBI VMALLE1` to flush all EL1/EL0 TLB entries. This is necessary because switching `HCR_EL2.E2H` changes the interpretation of ASID/VMID tagging for TLB entries. Any cached translations from the EL1 phase could be misinterpreted after VHE is fully committed. After the TLB flush, the EL2 MMU is enabled by writing `SCTLR_EL1` (which, through the E2H alias, now programs `SCTLR_EL2`). Between the TLB flush and the MMU enable, the CPU is briefly executing with no valid TLB and no MMU. If the code were not identity-mapped, the instruction fetch would fail because virtual addresses would not resolve to physical addresses without page table walks.

The `eret` at the end of `enter_vhe` reads the patched `spsr_el1` and `elr_el1`, returning execution to the EL2 kernel with the MMU on.

---

### 9. The Before/After State Summary

**Before `finalise_el2`:**
- CPU execution level: EL1
- `HCR_EL2`: `HCR_HOST_NVHE_FLAGS` (E2H=0, TGE=0)
- Kernel system registers: EL1 physical hardware
- KVM model: nVHE — every VM entry/exit crosses EL1↔EL2 boundary
- `is_kernel_in_hyp_mode()`: returns false

**After `finalise_el2`:**
- CPU execution level: EL2
- `HCR_EL2`: `HCR_HOST_VHE_FLAGS` (E2H=1, TGE=1, RW=1, AMO/IMO/FMO=1)
- Kernel system registers: EL1-named registers alias to EL2 hardware
- KVM model: VHE — VM entry/exit only requires VMID/VTTBR switch, no privilege level change
- `is_kernel_in_hyp_mode()`: returns true (`CurrentEL == CurrentEL_EL2`)

---

### 10. What an Interviewer Is Really Testing

If asked this in an NVIDIA interview, the depth they are probing for typically covers:

**ARM architecture depth:** Do you understand exception levels not just as names but as privilege hardware boundaries? Do you know what `HCR_EL2` is and why it is the key register?

**Boot sequence knowledge:** Can you trace exactly where in the boot flow this runs, what state the CPU is in, and why the sequence of operations in `__finalise_el2` is ordered the way it is? (MMU state check → feature check → HCR write → state transfer → SPSR patch → enter_vhe with TLB flush → eret is the only safe ordering.)

**Performance reasoning:** Why does VHE matter for KVM? The answer is not just "fewer privilege level crossings" — the deeper answer is that without VHE, every VM exit requires saving all EL1 guest system registers (dozens of registers) and restoring all EL1 host registers, because EL1 is shared between guest and host. With VHE, the host kernel is permanently at EL2 and has its own dedicated register state; only the guest EL1 registers need save/restore on VM entry/exit.

**Edge case awareness:** What happens if `finalise_el2` is not called for a secondary CPU, or called on a CPU that booted at EL1 while the primary booted at EL2? This would cause `__boot_cpu_mode` mismatch, and `is_hyp_mode_mismatched()` would return true, causing KVM to refuse to initialize. The kernel reports this condition clearly during boot.

**System register aliasing:** The E2H register aliasing is often a tripping point. The key insight is that `EL1` register names, when read/written from EL2 with `E2H=1`, silently redirect to EL2 hardware registers — while the old EL2 state is accessible only through the `*_EL12` shadow register names. This is the mechanism that makes the kernel binary work at EL2 without recompilation. 


## `ldp x29, x30, [sp], #16` — Deep Technical Explanation for an NVIDIA Interview

---

### 1. What the Instruction Does, Precisely

`ldp` is **Load Pair** — a single instruction that performs two 64-bit loads atomically from consecutive memory addresses. Breaking down each field:

- **`x29`** — destination register 1, loaded from `[sp + 0]`
- **`x30`** — destination register 2, loaded from `[sp + 8]`
- **`[sp], #16`** — **post-index addressing**: the memory access uses the current `sp` value, then `sp` is incremented by 16 after the load completes

The post-index form is important. The sequence is strictly:
1. Read 8 bytes at address `sp` → write to `x29`
2. Read 8 bytes at address `sp+8` → write to `x30`
3. Add 16 to `sp` (the increment happens after, not before the load)

This is the inverse of the prologue instruction `stp x29, x30, [sp, #-16]!`, which uses **pre-index** addressing (decrement first, then store).

---

### 2. Why `x29` and `x30` Specifically

These two registers are architecturally special in the ARM64 AAPCS64 calling convention:

**`x29` — Frame Pointer (FP):** Points to the base of the current stack frame. The AArch64 ABI mandates that `x29` always points to a valid `{x29, x30}` pair on the stack (the frame record), enabling linked-list traversal of the call stack for unwinding. It is callee-saved, meaning the function is responsible for preserving its value across the call.

**`x30` — Link Register (LR):** Holds the return address. When `bl` (Branch with Link) is used to call a function, the processor automatically writes the return address (the instruction after the `bl`) into `x30`. It is also callee-saved when a function itself calls other functions — because the nested `bl` calls would overwrite it.

In `__primary_switched`, both were saved at the top of the function with `stp x29, x30, [sp, #-16]!`, and this `ldp` restores them, closing that stack frame.

---

### 3. The Full Frame Lifecycle in `__primary_switched`

The prologue at the top of `__primary_switched`:

```
stp  x29, x30, [sp, #-16]!   // push: sp -= 16, store x29 at [sp], x30 at [sp+8]
mov  x29, sp                  // x29 now points to the frame base (the saved {x29, x30})
```

This creates a **standard frame record** — the entry `{previous_fp, return_address}` at the address held by `x29`. Any stack unwinder (kernel's `dump_backtrace`, `perf`, crash dump tools) walks the chain by repeatedly reading `{x29, x30}` at the address in `x29`, then jumping to `x29[0]` (the saved prior frame pointer).

The epilogue at line 245:

```
ldp  x29, x30, [sp], #16     // pop: load x29, x30 from [sp], then sp += 16
```

This precisely reverses the prologue — `x29` is restored to the caller's frame pointer value, `x30` is restored to the return address into whoever called `__primary_switched`, and `sp` returns to where it was before the `stp`. The frame record is gone.

---

### 4. The Critical Nuance: Why Teardown Here, Before `start_kernel`?

This is the detail that separates a surface-level answer from a deep one.

After the `ldp`, the very next instruction is `bl start_kernel`. This means:

- The frame for `__primary_switched` is torn down **before** calling `start_kernel`
- `start_kernel` is being called as if `__primary_switched` were doing a near-tail-call
- `x30` just got restored from the stack — but `bl start_kernel` **immediately overwrites `x30`** with the return address pointing to `ASM_BUG()`

So the restored `x30` value is never used as a return address. It is discarded in the very next instruction. Why restore it at all?

There are two reasons:

**Reason 1 — Stack Integrity for the Unwinder:** Even though `start_kernel` never returns under normal operation, the kernel's stack unwinder can be invoked at any time during `start_kernel`'s execution — via a panic, an oops, or a debug trap. If `__primary_switched` had left a dangling, partially-wound stack frame (sp pointing into the middle of its own allocation, x29 pointing at stale data), the unwinder would produce garbage or crash when it tried to walk past it. By tearing down the frame cleanly before handing off, `__primary_switched` ensures that the `start_kernel` frame sits on a clean stack with a correct `x29` chain above it.

**Reason 2 — ABI Discipline in Assembly Code:** The `__primary_switched` function is declared with `SYM_FUNC_START_LOCAL`, which means it has full function semantics. Assembly functions that call C code must follow the ABI even in the boot path. Leaving the function frame open across a `bl` to a C function would corrupt the CFI (Call Frame Information) annotations, breaking `perf` stack sampling, `kgdb`, and kernel stack sanitizers that rely on frame pointer or DWARF unwinding.

---

### 5. The `finalise_el2` Interaction — The Subtle Architectural Point

The instruction immediately before this `ldp` is `bl finalise_el2`. If VHE was successfully activated, `finalise_el2` returned from EL2 — but the kernel is now executing at **EL2** instead of EL1. The stack pointer value itself is unchanged (because `finalise_el2`'s `__finalise_el2` explicitly transfers `sp_el1` to the EL2 stack pointer before the `eret` back to the caller), but the hardware privilege level is different.

Does this affect the `ldp`? No, because:

- The stack is in the kernel's normal virtual address space, which is mapped identically at EL1 and EL2 (via the page tables transferred during VHE activation)
- `sp` at EL2h (stack register is `SP_EL2`) holds the same address as `sp` at EL1 held before, because `__finalise_el2` copied it explicitly
- The load is a standard data memory access — it does not care what exception level issued it, as long as the address is in a valid mapped region

So the `ldp` executes correctly whether the CPU is at EL1 (nVHE path) or at EL2 (VHE path). This is an example of the VHE design principle: kernel code after `finalise_el2` must be exception-level agnostic, and this instruction is.

---

### 6. The Stack Layout at This Exact Point

When `ldp x29, x30, [sp], #16` executes, the stack looks like this:

```
Higher addresses (stack grows down on AArch64)
  ...
  [previous frame of whoever called __primary_switched]
  ...
  [sp + 8] = saved x30 (return address into __primary_switched's caller)
  [sp + 0] = saved x29 (previous frame pointer)    <-- sp points here
  ...
Lower addresses
```

After the load:
- `x29` = value of caller's frame pointer (restoring the frame chain)
- `x30` = return address into caller (irrelevant, as `bl start_kernel` overwrites it)
- `sp` = `sp + 16` (points to the caller's frame, deallocating the 16-byte save area)

---

### 7. What an NVIDIA Interviewer Is Testing With This Line

This single instruction touches several layers of knowledge that are relevant to NVIDIA's kernel and hypervisor work:

**ARM64 ABI depth:** Do you know the difference between pre-index, post-index, and offset addressing modes? Do you know that `ldp`/`stp` are not just convenience — they are required by the AAPCS64 frame record layout?

**Stack unwinding in production systems:** NVIDIA's DRIVE OS includes crash dump analysis tools (similar to `kdump`) that walk the kernel stack to produce backtraces after a fault. These tools depend entirely on correct frame pointer chain maintenance. A boot path function that left a corrupted frame chain would cause incorrect crash reports in production automotive systems.

**Control flow reasoning:** Can you explain why `x30` is restored even though it will be immediately overwritten? The answer requires understanding that the restore is about the *stack pointer* and *frame pointer* state, not about actually using the return address.

**EL transition resilience:** Can you explain why this load works regardless of whether the preceding `finalise_el2` changed the exception level? This requires understanding how `finalise_el2` transfers stack pointer state during the VHE upgrade.

**Non-returning functions and tail calls:** Why is `bl start_kernel` used rather than `b start_kernel`? Because the frame was already torn down — there is no return address on the stack to pass as a tail call. Using `bl` means `x30` is set to point to `ASM_BUG()`, providing a last-resort fault signal if `start_kernel` ever incorrectly returns. 



