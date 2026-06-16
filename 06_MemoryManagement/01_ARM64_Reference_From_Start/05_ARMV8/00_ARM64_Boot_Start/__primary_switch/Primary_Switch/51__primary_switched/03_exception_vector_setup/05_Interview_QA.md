# Exception Vector Setup — Interview Q&A

---

## Q1: Why does ARM64 use direct-code vectors instead of function pointer tables?

**A:** Performance and simplicity. With direct-code vectors, the CPU jumps directly
to the handler code at `VBAR + offset` without an additional memory load to fetch
a pointer. Each 128-byte slot has 32 instructions — enough for the critical
save-context prologue before branching to the main handler. This eliminates a
memory access on the critical exception entry path, reducing exception latency.

---

## Q2: What is the purpose of the `.align 11` directive on the `vectors` symbol?

**A:** ARM architecture requires `VBAR_EL1` bits [10:0] to be zero — the vector table
must be aligned to 2048 bytes (2^11). `.align 11` instructs the assembler and linker
to place `vectors` at a 2048-byte boundary. If misaligned, writing the address to
`VBAR_EL1` produces UNPREDICTABLE behavior — the hardware ignores the low bits,
effectively using a different (wrong) base address.

---

## Q3: What is the difference between `dmb`, `dsb`, and `isb`?

**A:**
- `DMB` (Data Memory Barrier): orders memory accesses relative to other memory
  accesses. Does not affect instruction fetch.
- `DSB` (Data Synchronization Barrier): waits for all pending memory accesses to
  complete. Stronger than DMB. Still does not affect instruction fetch.
- `ISB` (Instruction Synchronization Barrier): flushes the instruction pipeline and
  causes all subsequent instructions to be re-fetched. Required after writing system
  registers that affect instruction fetch (like VBAR, SCTLR, TTBR).

For `msr vbar_el1`, only `isb` is correct. `dmb` or `dsb` would not guarantee
that the new VBAR value is visible to the exception dispatch hardware.

---

## Q4: Which bank of the vector table handles Linux syscalls?

**A:** Bank 3, offset `VBAR + 0x400`: "Lower EL, AArch64, Synchronous."

A syscall is an `SVC #0` instruction, which is a synchronous exception. It comes
from user space (lower EL = EL0) and uses AArch64 mode. The CPU jumps to
`VBAR_EL1 + 0x400` which contains the `kernel_ventry 0, t, 64, sync` code, which
quickly branches to `el0t_64_sync` and then `el0_svc`.

---

## Q5: Can you take an exception on the `isb` instruction itself?

**A:** In theory, `isb` is a barrier instruction and does not access memory, so it
cannot cause a data abort or alignment fault. In practice, `isb` is executed with
`PSTATE.I=1` (interrupts masked), so no IRQ can arrive. An `isb` can theoretically
be interrupted by a non-maskable event (like a physical `RESET`), but that would
restart the entire boot process. For all practical purposes, `isb` executes
atomically relative to exception handling.

---

## Q6: What happens if the kernel supports both AArch64 and AArch32 user processes?

**A:** The `vectors` table has entries for both. AArch32 user processes generate
exceptions that land at `VBAR + 0x600–0x780` (Bank 4, AArch32). Linux ARM64 CONFIG
option `CONFIG_COMPAT` enables these handlers. The `kernel_ventry 0, t, 32, sync`
macro installs the 32-bit compat exception entry code. Both 64-bit and 32-bit user
processes use the same `VBAR_EL1` — the hardware selects the correct bank based on
PSTATE.nRW (0=AArch64, 1=AArch32) of the interrupted code.

---

## Q7: How does KPTI (Kernel Page Table Isolation) interact with VBAR?

**A:** With KPTI enabled (`CONFIG_UNMAP_KERNEL_AT_EL0`), the kernel maintains
two page tables:
- User page table (`trampoline`): contains only the bare minimum code needed to
  handle exceptions from user space (including the exception vector stubs)
- Kernel page table: the full kernel page table

When a user-space exception occurs, the CPU jumps to the trampoline page (mapped
in both user and kernel page tables). The trampoline code:
1. Switches `TTBR1_EL1` to the full kernel page table
2. Jumps to the real kernel exception handler

`VBAR_EL1` points to the trampoline vectors (not the regular `vectors`) when KPTI
is active. The `__primary_switched` setup ensures the correct `vectors` address is
loaded, and the KPTI trampoline is handled separately in `entry.S`.

---

## Q8: What is the difference between setting VBAR in `__primary_switched` vs `cpu_init`?

**A:** `__primary_switched` is called only ONCE for the primary CPU during initial
boot. Secondary CPUs (brought up by `cpu_up()`) call `secondary_startup →
secondary_switched → cpu_init()`. `cpu_init()` also calls `cpu_init_vectors()` which
does the equivalent `msr vbar_el1, x0; isb` for each secondary CPU. So the
pattern is: primary CPU sets VBAR in `__primary_switched`, secondary CPUs set VBAR
in `cpu_init`. The `vectors` address is the same for all CPUs — it's a per-CPU
REGISTER write but to the SAME value.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
VBAR_EL1 (Vector Base Address Register, EL1) holds the base address of the EL1 exception vector table. When an exception is taken to EL1 (IRQ, FIQ, SError, Synchronous abort), the CPU computes the vector offset from the exception type and PSTATE.SP, adds it to VBAR_EL1, and jumps to the resulting address (this is the hardware branch, not a software branch). The vector table has 16 entries (4 types x 4 SP variants) each spaced 0x80 bytes apart. VBAR_EL1 must be aligned to a 2 KB boundary. With KASLR, VBAR_EL1 is randomized as part of the kernel image.

### Kernel Perspective (Linux ARM64)
Linux sets VBAR_EL1 in __primary_switched (arch/arm64/kernel/head.S) using:
  adr_l  x8, vectors          // load VA of the vectors table
  msr    vbar_el1, x8         // write to VBAR_EL1
  isb                          // synchronize
This is done after the MMU is enabled and the kernel VA is active. The 'vectors' symbol is defined in arch/arm64/kernel/entry.S and maps the 16 exception handlers. Until this point, any exception would take the CPU to whatever garbage is at VBAR_EL1 (undefined on reset), so the early boot path must not trigger any exceptions.

### Memory Perspective (ARMv8 Memory Model)
VBAR_EL1 stores a VA (in the kernel text mapping). The exception vector table at that VA is Normal Inner-Shareable Read-Only (from an architectural view; the Linux kernel maps it as read-only/execute). When an exception fires, the CPU reads VBAR_EL1, computes the target VA, and fetches the instruction at that VA via the TLB/I-cache. The vector table is part of the kernel text and benefits from I-cache warming; exception entry latency is minimized once the I-cache has been populated with the vector code.