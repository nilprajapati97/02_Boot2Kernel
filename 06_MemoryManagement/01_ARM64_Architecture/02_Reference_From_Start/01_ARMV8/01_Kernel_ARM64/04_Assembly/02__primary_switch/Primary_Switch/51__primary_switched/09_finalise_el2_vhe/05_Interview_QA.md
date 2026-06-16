# `finalise_el2` and VHE — Interview Q&A

---

## Q1: What does `finalise_el2` do and why is it called in `__primary_switched`?

**A:** `finalise_el2` completes the EL2 hardware configuration that was started
in `init_kernel_el` (very early assembly). It:
1. Detects VHE vs nVHE (reads `HCR_EL2.E2H`)
2. For VHE: configures `SPSR_EL2`, `CNTHCTL_EL2`, timer offsets, and sets the
   CPU into EL2h host mode
3. For nVHE: configures minimal EL2 for KVM stub, sets `SPSR_EL2` for EL1h mode
4. For EL1 boot: returns immediately (nothing to do)

It's called in `__primary_switched` (not earlier) because it requires:
- Virtual memory (MMU on) for symbol address resolution
- A valid stack (needed for the `bl` to C helper functions)
- Installed exception vectors (VBAR_EL1 already configured)

---

## Q2: Explain the difference between VHE and nVHE KVM.

**A:**
**nVHE** (pre-ARMv8.1 or VHE disabled):
- Host Linux runs at EL1, KVM stub at EL2
- On every VM exit: full EL1 context switch (save 30+ registers)
- `finalise_el2_nvhe` sets up EL2 with `HCR_EL2.E2H=0`
- VM exit overhead: ~1000-2000 cycles for register saves

**VHE** (ARMv8.1+, `HCR_EL2.E2H=1`):
- Host Linux runs directly at EL2
- EL1 register accesses aliased to EL2 equivalents (transparent to kernel code)
- On VM exit: host kernel handles directly at EL2 (no EL level switch)
- VM exit overhead: ~100-300 cycles
- `finalise_el2_vhe` sets up `SPSR_EL2 = EL2h` mode

VHE requires hardware support (ARMv8.1-A) and EL2 boot.

---

## Q3: What is `HCR_EL2.E2H` and `HCR_EL2.TGE`?

**A:**
`HCR_EL2.E2H` (bit 34): **EL2 Host bit** — enables VHE. When set:
- EL1 register accesses from EL2 are redirected to EL2 equivalents
- The kernel at EL2 behaves as if it's a normal EL1 OS
- Enables Linux kernel to run unmodified at EL2

`HCR_EL2.TGE` (bit 27): **Trap General Exceptions bit** — when set:
- All EL0 exceptions and interrupts go to EL2 (not EL1)
- EL0 uses EL2 translation tables (TTBR0_EL2 instead of TTBR0_EL1)
- This means user processes are managed by the host at EL2

In VHE host mode: `E2H=1, TGE=1` (host Linux at EL2 fully controls EL0)
In VHE guest mode: `E2H=1, TGE=0` (VM at EL1, not controlled by EL2 host)

---

## Q4: What happens to EL2 configuration if `finalise_el2` is skipped?

**A:** Catastrophic and subtle failures:
1. `SPSR_EL2` may not be set for the correct exception level return
2. Timer configuration (`CNTHCTL_EL2`) may block EL1 timer access → scheduling broken
3. EL2 exception vector (`VBAR_EL2`) may point to firmware vectors → wrong handler
4. `HCR_EL2` in wrong state → exceptions may not be routed correctly
5. Spectre mitigations may be incomplete → security vulnerability

In practice, the kernel would likely crash in `start_kernel` when trying to
initialize the timer subsystem or when the first timer interrupt fires with
incorrect routing.

---

## Q5: How does pKVM (Protected KVM) change the `finalise_el2` model?

**A:** pKVM runs the hypervisor at EL2 as a security boundary that DOESN'T
trust EL1 Linux. In pKVM mode:

- `finalise_el2` installs the pKVM hypervisor code at EL2 (not just a KVM stub)
- EL1 Linux cannot modify EL2 after this point — pKVM "freezes" EL2
- Guest VMs can be protected even from a compromised EL1 Linux kernel
- Used in Android for "Protected Virtual Machines" (sensitive workloads)

The key change: in normal KVM, EL1 can call `hvc` to make EL2 do anything.
In pKVM, the hypercall interface is minimal and verified — EL1 cannot
use EL2 to attack other VMs. `finalise_el2` for pKVM does more: it installs
the full pKVM hypervisor and configures EL2-only memory regions.

---

## Q6: On ARMv8.0 hardware (no VHE), how does KVM achieve reasonable VM performance?

**A:** On ARMv8.0 (nVHE only), KVM uses several optimizations:
1. **Lazy register save**: Don't save all EL1 registers on every exit — only
   save what the exit handler needs
2. **Coalesced exit handling**: Process multiple VM exits before returning
   to EL1 host (fewer EL2→EL1 transitions)
3. **VMID-tagged TLBs**: Avoid full TLB flush on guest switch (ASID-like)
4. **Fast path exits**: Common exits (timer, simple I/O) handled entirely at
   EL2 without going to EL1

Despite these, nVHE is measurably slower than VHE for I/O-intensive workloads.
Modern server SoCs (Neoverse N2, Amazon Graviton3) are ARMv8.4+ with VHE,
largely eliminating nVHE relevance for new hardware.

---

## Q7: Why does `finalise_el2` use `eret` to return, while most functions use `ret`?

**A:** `eret` (Exception Return) performs both a level change and a PC jump:
- It loads `PC = ELR_ELx` (exception link register)
- It loads `PSTATE = SPSR_ELx` (including the new exception level)

`finalise_el2` uses `eret` when it needs to:
1. **Change exception level**: EL2 → EL1h (for nVHE path)
2. **Change processor state**: while keeping the program counter flowing

If `finalise_el2` is called from `__primary_switched` at EL2, it must use
`eret` to return to EL1h mode (for nVHE). For VHE, since the kernel stays at
EL2, `ret` is sufficient (no level change needed).

The `eret` is critical: a simple `ret` from EL2 cannot change the exception
level — it just returns to the call site at the SAME level. Only `eret` can
transition the CPU to a lower (or same) exception level.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
VHE (Virtualization Host Extension, ARMv8.1-A, HCR_EL2.E2H=1) allows the kernel to run at EL2 instead of EL1 in a KVM host scenario. When E2H=1, EL1 system register accesses are re-routed to their EL2 equivalents (e.g., MSR SCTLR_EL1 writes to SCTLR_EL2). PSTATE.M determines the current exception level. Without VHE, the hypervisor must context-switch all EL1 registers on every VM entry/exit; with VHE, the host kernel IS the EL2 code and the context-switch overhead is eliminated. The CPU hardware differentiates VHE from non-VHE mode by the HCR_EL2.E2H bit.

### Kernel Perspective (Linux ARM64)
Linux detects VHE capability in __primary_switched by reading ID_AA64MMFR1_EL1.VH. If VHE is available and KVM is configured, the kernel uses finalise_el2 (arch/arm64/kernel/hyp-stub.S) to switch to EL2 before start_kernel. The boot CPU mode flag (x20 bit 0 in __primary_switch: BOOT_CPU_FLAG_E2H) records whether the boot CPU entered VHE mode. Secondary CPUs must match the primary CPU's VHE mode. The is_hyp_mode_available() and is_kernel_in_hyp_mode() helpers in arch/arm64/include/asm/virt.h let the kernel test VHE mode at runtime.

### Memory Perspective (ARMv8 Memory Model)
In VHE mode (EL2), the CPU uses TTBR0_EL2 and TTBR1_EL2 for translation (with HCR_EL2.E2H=1, the EL2 translation regime gains a TTBR1 equivalent). The memory map is the same conceptually but the table root registers are different. Stage 2 translation (GPA->PA) is also controlled by EL2. For a KVM guest, stage 1 (VA->IPA, managed by the guest OS at EL1) and stage 2 (IPA->PA, managed by KVM at EL2) are both active. The ARMv8 memory model handles two-stage translation transparently: the TLB caches combined stage-1 + stage-2 entries.