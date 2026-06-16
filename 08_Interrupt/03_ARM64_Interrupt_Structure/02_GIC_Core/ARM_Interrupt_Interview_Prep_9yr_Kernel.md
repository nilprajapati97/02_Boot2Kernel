# ARM Interrupt Driver — Interview Prep (Senior Linux Kernel Engineer, ~9 yrs)

> Format: **topic section → 2–4 paragraph technical explanation → "Likely Interview Questions" with concise model answers.**
> Scope: ARMv8/ARM64 exception model, GICv2/v3/v4 architecture, Linux IRQ subsystem, driver-author patterns, PREEMPT_RT, KVM/vGIC, debugging.
> Source: original `.docx` reference could not be read in this session; this document was generated from scratch.

---

## Table of Contents

1. [ARMv8 Exception Model & IRQ Routing](#1-armv8-exception-model--irq-routing)
2. [Vector Table, `SP_ELx`, and the Per-CPU IRQ Stack](#2-vector-table-sp_elx-and-the-per-cpu-irq-stack)
3. [GIC Architecture: v2 / v3 / v4 / v4.1](#3-gic-architecture-v2--v3--v4--v41)
4. [GICv3 ITS and LPIs (MSI/MSI-X)](#4-gicv3-its-and-lpis-msimsi-x)
5. [IPIs (SGIs) and SMP Cross-Calls](#5-ipis-sgis-and-smp-cross-calls)
6. [Linux IRQ Subsystem Core (`irq_desc` / `irq_chip` / `irqaction`)](#6-linux-irq-subsystem-core)
7. [IRQ Domains (Hierarchical)](#7-irq-domains-hierarchical)
8. [`request_irq` / `request_threaded_irq` and `IRQF_*` Flags](#8-request_irq--request_threaded_irq-and-irqf_-flags)
9. [Threaded IRQs and Top/Bottom Halves](#9-threaded-irqs-and-topbottom-halves)
10. [Softirq vs Tasklet vs Workqueue](#10-softirq-vs-tasklet-vs-workqueue)
11. [Affinity, `smp_affinity`, Managed IRQs](#11-affinity-smp_affinity-managed-irqs)
12. [PREEMPT_RT Considerations](#12-preempt_rt-considerations)
13. [MSI/MSI-X on ARM64](#13-msimsi-x-on-arm64)
14. [Devicetree Bindings for Interrupts](#14-devicetree-bindings-for-interrupts)
15. [Chained vs Hierarchical Interrupt Controllers](#15-chained-vs-hierarchical-interrupt-controllers)
16. [Nested IRQs and Interrupt Storms](#16-nested-irqs-and-interrupt-storms)
17. [EL2 / VHE / vGIC (KVM)](#17-el2--vhe--vgic-kvm)
18. [Debugging Interrupts](#18-debugging-interrupts)
19. [Suspend/Resume & Wakeup IRQs](#19-suspendresume--wakeup-irqs)
20. [Common Pitfalls & Gotchas](#20-common-pitfalls--gotchas)
21. [Mock Interview Script (30 min)](#21-mock-interview-script-30-min)

---

## 1. ARMv8 Exception Model & IRQ Routing

ARMv8-A defines four **Exception Levels**: `EL0` (user), `EL1` (kernel), `EL2` (hypervisor), `EL3` (secure monitor). Each EL (except EL0) has banked system registers (`SP_ELx`, `ELR_ELx`, `SPSR_ELx`, `VBAR_ELx`, `SCTLR_ELx`, `TTBRx_ELy`). The CPU maintains four asynchronous-exception masks in `PSTATE.DAIF` — **D** (debug), **A** (SError), **I** (IRQ), **F** (FIQ). Setting a bit *masks* that class.

Where an IRQ/FIQ is **taken** depends on routing config: `SCR_EL3.IRQ/FIQ` controls whether non-secure IRQ/FIQ traps to EL3; `HCR_EL2.IMO/FMO/AMO` controls whether IRQ/FIQ/SError taken while in a guest (EL0/EL1) trap to EL2. On a typical Linux-on-ARM64 system without virtualization, IRQs are taken at EL1 (or EL2 when running with **VHE** as a KVM host). FIQ is historically a separate "fast" interrupt line; mainline Linux on arm64 mostly treats FIQ as a secure-side or pseudo-NMI vehicle (since 5.8+: `arm64_pseudo_nmi`).

On exception entry, the CPU automatically: saves `PSTATE → SPSR_ELx`, saves return PC → `ELR_ELx`, masks `DAIF` (all four), sets `PSTATE.SP=1` (handler uses `SP_ELx`), and jumps to `VBAR_ELx + offset`. `ERET` reverses this. Linux unmasks `I` (and others as appropriate) at controlled points using `local_irq_enable()` → `msr daifclr, #2`.

### Likely Interview Questions

**Q1.** Walk me through what the hardware does the instant an IRQ is taken from EL0.
**A.** PC of next instruction → `ELR_EL1`; `PSTATE` → `SPSR_EL1`; `DAIF` all set (mask further async); `SPSel` forced to 1 (use `SP_EL1`); EL becomes 1; PC ← `VBAR_EL1 + 0x480` (Lower-EL AArch64 / IRQ slot). No registers are saved automatically — that's software's job.

**Q2.** What is `DAIF` and when does Linux clear `I`?
**A.** Four PSTATE mask bits (Debug, SError, IRQ, FIQ). Linux clears `I` (`msr daifclr, #2`) after it has saved `pt_regs`, established a valid stack, and is ready to take nested exceptions — typically inside `el0t_64_irq_handler`/`el1_interrupt_handler` after `enter_from_user_mode`/`enter_from_kernel_mode` bookkeeping.

**Q3.** How is IRQ routing to EL2 vs EL1 controlled?
**A.** `HCR_EL2.IMO` (IRQ Mask Override) — when 1, physical IRQs taken in EL0/EL1 are routed to EL2 instead of EL1. KVM sets this so the host (EL2 with VHE, or low-vis hyp stub without VHE) intercepts guest IRQs. `SCR_EL3.IRQ` does the analogous thing for EL3.

**Q4.** Why is FIQ special on ARM64 Linux?
**A.** Architecturally FIQ has its own banked regs (in AArch32) and lower latency potential. On ARM64 Linux it's usually owned by secure firmware (TrustZone). With GICv3 + `CONFIG_ARM64_PSEUDO_NMI`, Linux re-purposes FIQ-like priority masking via `ICC_PMR_EL1` to deliver **pseudo-NMIs** for hard-lockup detection, perf, etc.

**Q5.** What's the difference between `EL1t` and `EL1h`?
**A.** The vector-table grouping for exceptions taken **while already at EL1**: `EL1t` = `SPSel=0` (was using `SP_EL0`) — rare in modern kernels; `EL1h` = `SPSel=1` (was using `SP_EL1`) — the normal kernel case. Each has its own 4 vectors (Sync, IRQ, FIQ, SError).

---

## 2. Vector Table, `SP_ELx`, and the Per-CPU IRQ Stack

`VBAR_EL1` points to a 2 KB table of 16 entries × 128 bytes, grouped by the source of the exception: **Current EL with SP_EL0** (`EL1t`), **Current EL with SP_ELx** (`EL1h`), **Lower EL using AArch64** (from EL0_64), **Lower EL using AArch32**. Each group has four 128-byte slots: Synchronous, IRQ, FIQ, SError. The slots are aligned so the CPU branches by simple offset arithmetic without a software dispatch.

ARM64 banks the SP per EL (`SP_EL0/1/2/3`), and within each EL≥1 the `PSTATE.SP` bit selects between `SP_EL0` and `SP_ELx`. On exception entry to ELx the CPU forces `SPSel=1`, so the handler starts on `SP_ELx`. Linux keeps `SP_EL1` always pointing at the **interrupted task's kernel stack** (`task->stack`, `THREAD_SIZE` = 16 KB by default; `CONFIG_VMAP_STACK` allocates it in vmalloc space with guard pages). `SP_EL0` is repurposed in kernel mode to hold `current` (the `task_struct *`), enabling `get_current()` to be a single `mrs` with no memory access.

Linux uses a separate **per-CPU IRQ stack** for hardirq handler bodies. `kernel_entry` first builds `pt_regs` on the task's kernel stack (so unwinders, page faults, schedule-on-return all work). Then `call_on_irq_stack` saves SP into `x29`, switches SP to `__this_cpu_read(irq_stack_ptr)`, calls `handle_arch_irq`, and restores SP. This bounds worst-case stack usage (task + IRQ depths don't compound) and improves cache locality. `pt_regs` stays on the task stack; the unwinder uses the frame record saved in `x29` to cross the task↔IRQ boundary.

### Likely Interview Questions

**Q1.** Why doesn't Linux just set `SP_EL1` directly to the IRQ stack?
**A.** `SP_EL1` is used for **every** EL1 entry — sync exceptions, syscalls, page faults, SError — not only IRQ. Those need the task's kernel stack to build `pt_regs`, to be allowed to sleep (e.g., page-fault → IO), and to be preempted. The IRQ stack is for the transient, non-sleeping handler body only.

**Q2.** How does `current` work on ARM64?
**A.** `SP_EL0` is unused while executing at EL1, so Linux stores `task_struct *current` there on every kernel entry. `current` macro expands to `mrs %0, sp_el0` — single instruction, no cache miss.

**Q3.** What is `VMAP_STACK` and how does it interact with overflow?
**A.** Kernel stack allocated from vmalloc with an unmapped guard page beneath. Overflow → fault on the guard. Arm64 needs a special **overflow stack** (per-CPU) because the fault handler can't run on the broken stack. See `__bad_stack` and the `overflow_stack` array in [arch/arm64/kernel/traps.c](https://github.com/torvalds/linux/blob/master/arch/arm64/kernel/traps.c).

**Q4.** How does the arm64 unwinder cross the IRQ-stack boundary?
**A.** When entry code switches to the IRQ stack it pushes a frame record (`x29`, `x30`) pointing back to the task-stack frame. The unwinder (`unwind_frame`) recognizes the boundary by checking that `fp` leaves the current stack's range, then validates it lies inside another known stack (task / IRQ / overflow / SDEI).

**Q5.** Where exactly does the IRQ vector live?
**A.** `VBAR_EL1 + 0x480` for IRQ from Lower EL AArch64 (EL0 user task), and `VBAR_EL1 + 0x280` for IRQ from current EL (EL1h, kernel was running). See `SYM_CODE_START(vectors)` in [arch/arm64/kernel/entry.S](https://github.com/torvalds/linux/blob/master/arch/arm64/kernel/entry.S).

---

## 3. GIC Architecture: v2 / v3 / v4 / v4.1

The **Generic Interrupt Controller** is ARM's standard interrupt controller. **GICv2** has a memory-mapped **Distributor** (`GICD_*`, global) and a per-CPU **CPU Interface** (`GICC_*`). It supports 8 SGIs, 16 PPIs, up to 988 SPIs, and at most 8 CPUs. Priority is 8 bits (typically 4 or 5 implemented); a CPU acknowledges via `GICC_IAR` and signals end-of-interrupt via `GICC_EOIR`.

**GICv3** is a redesign for >8 CPUs and MSI scalability. Two main changes: (a) the CPU interface moves into the core as **system registers** (`ICC_*_EL1`) accessed via `mrs`/`msr` — much faster than MMIO; (b) per-CPU **Redistributor** (`GICR_*`) handles per-CPU state (PPIs, SGIs, LPI config/pending tables). Distributor still handles SPIs globally. New interrupt class: **LPIs** (Locality-specific Peripheral Interrupts) — message-signaled, edge-only, configured via the **ITS** (Interrupt Translation Service). GICv3 supports up to 16M LPIs and uses **Affinity Routing** (`GICD_IROUTER`) to target CPUs by `MPIDR` affinity hierarchy rather than 8-bit bitmask.

**GICv4** adds **direct injection of virtual LPIs**: the ITS can deliver an LPI directly into a guest's vCPU `vPE` (virtual PE) table without exiting to the hypervisor. **GICv4.1** extends this to **vSGIs** — direct virtual SGI injection, eliminating the hypervisor round-trip for guest IPIs. Critical for KVM PCIe passthrough performance.

Interrupt classes summary:

| Class | Range (GICv3) | Scope | Triggering |
|-------|---------------|-------|------------|
| **SGI** | 0–15 | Per-CPU; software-generated | Edge |
| **PPI** | 16–31 | Per-CPU (timer, PMU, vGIC maint.) | Level or edge |
| **SPI** | 32–1019 | Shared / routable | Level or edge |
| **Extended PPI** | 1056–1119 | Per-CPU (GICv3.1+) | Level or edge |
| **Extended SPI** | 4096–5119 | Shared (GICv3.1+) | Level or edge |
| **LPI** | 8192–N | Routed via ITS | Edge, message-based |

### Likely Interview Questions

**Q1.** Why did GICv3 move the CPU interface to system registers?
**A.** MMIO accesses cost dozens of cycles and don't scale across many CPUs (one APB-ish path). System-register access is 1–2 cycles, per-CPU by definition, and enables `ICC_PMR_EL1`-based pseudo-NMI. Software must opt-in by setting `ICC_SRE_EL1.SRE=1`.

**Q2.** Difference between PPI and SPI?
**A.** PPI = Private Peripheral Interrupt: one per CPU, same INTID means different signal on each CPU (e.g., per-CPU arch timer is PPI 30/27/26). SPI = Shared: one global instance, routable to any CPU via `GICD_IROUTER<n>` (GICv3) or `GICD_ITARGETSR<n>` (GICv2).

**Q3.** How does GICv3 affinity routing work?
**A.** `GICD_IROUTER<n>` is 64-bit per SPI. Two modes: `Interrupt_Routing_Mode=0` selects a specific PE by Aff3.Aff2.Aff1.Aff0 (matching `MPIDR_EL1`); mode 1 = "1-of-N" — GIC picks any participating CPU. Linux normally uses specific-PE mode for predictability.

**Q4.** What's an LPI and why do we need one?
**A.** Locality-specific Peripheral Interrupt — edge-triggered, message-signaled, INTID ≥ 8192, configured via in-memory tables (no per-line state in the Distributor). Needed because GICv3 supports millions of MSIs from PCIe devices — can't have a dedicated wire/register per source.

**Q5.** What did GICv4.1 add over v4?
**A.** Direct injection of virtual SGIs. In v4, the hypervisor still had to emulate guest `ICC_SGI1R_EL1` writes; in v4.1 the ITS can deliver vSGIs straight to a guest vPE table, dramatically reducing IPI cost in SMP guests.

---

## 4. GICv3 ITS and LPIs (MSI/MSI-X)

The **ITS** is an in-SoC engine that translates `(DeviceID, EventID)` from an incoming MSI write into `(LPI INTID, target CPU)` and forwards it to the appropriate Redistributor. The translation uses three programmable in-memory tables: **Device Table** (`MAPD` command — maps DeviceID to an Interrupt Translation Table), per-device **ITT** (`MAPTI` — maps EventID to LPI INTID + Collection), and **Collection Table** (`MAPC` — maps Collection ID to a Redistributor / CPU). LPI **config table** (1 byte per LPI: enable + priority) and **pending table** (1 bit per LPI) live per-Redistributor in normal memory.

A device issues an MSI by writing its EventID to the ITS translation register `GITS_TRANSLATER` (address advertised in PCI MSI capability). The ITS looks up DeviceID → ITT → EventID → LPI + Collection → Redistributor, then sets the LPI pending bit at the target Redistributor, which raises the IRQ to its CPU. The CPU reads `ICC_IAR1_EL1`, runs the handler, writes `ICC_EOIR1_EL1`.

Programming the ITS is done by writing commands to an in-memory **command queue** and ringing the `GITS_CWRITER` doorbell; the ITS consumes commands asynchronously. Common commands: `MAPD`, `MAPC`, `MAPTI`, `INV` (invalidate cached LPI config), `INVALL` (invalidate all for a collection), `SYNC` (barrier), `MOVI` (move LPI to new collection — used for affinity changes), `DISCARD` (free EventID).

Linux's ITS driver: [drivers/irqchip/irq-gic-v3-its.c](https://github.com/torvalds/linux/blob/master/drivers/irqchip/irq-gic-v3-its.c) — implements an `msi_domain` parent for PCIe and platform MSI, manages LPI allocation, command queue, collections per CPU.

### Likely Interview Questions

**Q1.** Trace an MSI from a PCIe NIC to the handler.
**A.** NIC writes EventID to its programmed MSI address (= `GITS_TRANSLATER`). ITS translates `(DeviceID=PCI RID, EventID)` via Device Table → ITT → LPI INTID + Collection. ITS sends "Set pending" to the Collection's Redistributor. Redistributor checks LPI config-table enable + priority, raises IRQ to its CPU. Linux vector reads `ICC_IAR1_EL1` → returns LPI INTID → `handle_domain_irq` → `irq_desc->handle_irq` (`handle_fasteoi_irq`) → driver handler → `ICC_EOIR1_EL1`.

**Q2.** What happens on affinity change for an LPI?
**A.** Linux issues an ITS `MOVI` command remapping the LPI's EventID to a new Collection (which points to the new CPU's Redistributor), followed by `SYNC`. No per-line MMIO needed — scales to millions of MSIs.

**Q3.** Why are LPIs edge-only?
**A.** They are message-signaled — there is no physical wire whose level the GIC can sample. The "edge" is the write event to the ITS. Devices must re-fire if they want repeated notification.

**Q4.** How big are the ITS tables and where are they?
**A.** Allocated by Linux in normal RAM at boot — sizes derived from `GITS_BASER<n>` (device, collection, vPE). Device Table can be flat or two-level; Linux picks two-level if flat would exceed `MAX_ORDER`. Pending/config tables sized for `nr_lpis` (Linux caps at `lpi_id_bits`, typically 14–16).

**Q5.** What's the role of the `INV`/`INVALL` commands?
**A.** When Linux modifies an LPI's config-table byte (e.g., mask/unmask, change priority), the ITS / Redistributor may have cached it. `INV <DeviceID, EventID>` invalidates a single LPI's cached config; `INVALL <Collection>` invalidates all LPIs for a CPU. Followed by `SYNC` to wait for completion.

---

## 5. IPIs (SGIs) and SMP Cross-Calls

**SGIs** (INTIDs 0–15) are software-generated per-CPU interrupts used for inter-processor communication. On GICv3 a CPU sends an SGI by writing `ICC_SGI1R_EL1` (or `ICC_ASGI1R_EL1` for alternate-security): the write encodes target affinity (Aff3/2/1, target-list of up to 16 PEs sharing Aff0 hi-nibble) and INTID 0–15. The targeted Redistributors raise the SGI to their CPUs.

Linux arm64 defines a small fixed set of IPI types in [arch/arm64/kernel/smp.c](https://github.com/torvalds/linux/blob/master/arch/arm64/kernel/smp.c) — `IPI_RESCHEDULE`, `IPI_CALL_FUNC`, `IPI_CPU_STOP`, `IPI_CPU_CRASH_STOP`, `IPI_TIMER`, `IPI_IRQ_WORK`, `IPI_WAKEUP` (and `IPI_CPU_BACKTRACE`/`IPI_KGDB_ROUNDUP` depending on config). All multiplexed onto SGIs allocated dynamically from the GIC driver via `set_smp_ipi_range()`. Generic kernel users send via `smp_call_function_single()`, `smp_call_function_many()`, or higher-level `kick_all_cpus_sync()`.

Reception: SGIs go through the normal IRQ path (`ICC_IAR1_EL1` ack → handler → `ICC_EOIR1_EL1`) — there's no separate vector. Linux dispatches by INTID inside the IRQ handler. SGIs are **edge** on GICv3 (level on GICv2 SGI behavior is implementation-defined). With pseudo-NMI, certain IPIs (e.g., `IPI_CPU_BACKTRACE`) can be promoted to NMI-priority for hard-lockup escape.

### Likely Interview Questions

**Q1.** How does an `smp_call_function_single()` actually reach the target CPU?
**A.** Caller enqueues a `call_single_data` on the target's per-CPU `call_single_queue`, then issues `arch_send_call_function_single_ipi(cpu)` → maps to `IPI_CALL_FUNC` SGI via `ipi_send_mask` → GIC driver writes `ICC_SGI1R_EL1`. Target CPU's IRQ handler dispatches to `generic_smp_call_function_interrupt` → drains queue → invokes callbacks.

**Q2.** Why does GICv3 SGI delivery use affinity routing instead of CPU bitmask?
**A.** Because GICv3 supports >8 CPUs (up to 2^32 logical PEs by affinity). `ICC_SGI1R_EL1` encodes Aff3.Aff2.Aff1 explicitly and a 16-bit target list within that Aff0 cluster nibble — efficient broadcast-within-cluster. Linux loops by affinity cluster when sending to an arbitrary mask.

**Q3.** What is `IPI_CPU_STOP`?
**A.** Sent during panic/shutdown to halt other CPUs. Each CPU on receipt disables IRQs and either spins or enters cpuidle. `IPI_CPU_CRASH_STOP` is the kdump variant — saves register state into per-CPU `crash_notes` before halting so kdump can capture full SMP state.

**Q4.** Can you send an SGI from userspace?
**A.** No — `ICC_SGI1R_EL1` is EL1-only. Userspace can only trigger IPIs indirectly via syscalls (`sched_setaffinity`, `membarrier`, etc.).

**Q5.** How does pseudo-NMI promote certain IPIs?
**A.** `CONFIG_ARM64_PSEUDO_NMI` programs `ICC_PMR_EL1` so that normal IRQs are masked by lowering PMR, but NMI-priority interrupts (configured at a higher priority than PMR's normal mask level) can still preempt. Selected IPIs (`IPI_CPU_BACKTRACE`, `IPI_CPU_STOP`) are configured with NMI priority via `irq_set_irqchip_state(... IRQCHIP_STATE_PENDING)` plus priority programming, enabling stack traces from a hung CPU.

---

## 6. Linux IRQ Subsystem Core

The kernel maps every interrupt to a **Linux IRQ number** (`virq`) — distinct from the hardware INTID. Each `virq` is described by a `struct irq_desc` (per-IRQ state: counters, action list, lock, status flags) containing an `irq_data` (chip pointers, hwirq, affinity, domain). The controller is abstracted as `struct irq_chip` with method pointers: `irq_mask`, `irq_unmask`, `irq_ack`, `irq_eoi`, `irq_set_affinity`, `irq_set_type`, `irq_set_wake`, etc. The handler **flow** is selected by `irq_set_handler(virq, flow)` — common flows: `handle_level_irq`, `handle_edge_irq`, `handle_fasteoi_irq`, `handle_percpu_devid_irq`, `handle_simple_irq`. Per-driver handlers are kept on an `irqaction` list rooted at `irq_desc->action`.

Top-level entry from `entry.S` lands in `handle_arch_irq` (`gic_handle_irq` for GIC). The chip driver reads the INTID (`ICC_IAR1_EL1`), translates hwirq → virq via the IRQ domain (`irq_resolve_mapping` / `generic_handle_domain_irq`), and invokes `irq_desc->handle_irq` (the flow). The flow handler enforces the right masking/EOI dance (e.g., `handle_fasteoi_irq`: ack-by-read already done, call action, then EOI; `handle_level_irq`: mask, ack, unmask after handler — to prevent reassertion storm).

`irqaction` carries `handler`, `thread_fn`, `flags`, `dev_id`, etc. Shared IRQs chain multiple `irqaction`s on one `irq_desc`; the flow calls each in turn until one returns `IRQ_HANDLED`. Driver writers rarely touch `irq_desc` directly — they use `request_irq` / `free_irq` / `disable_irq` / `enable_irq` / `irq_set_affinity_hint`.

### Likely Interview Questions

**Q1.** Why is there a distinction between `hwirq` and Linux `virq`?
**A.** Multiple controllers can coexist (GIC + GPIO controllers + PCIe MSI domains); hwirq numbers collide across them and may not be densely allocated. `virq` is a flat, kernel-global namespace allocated lazily by IRQ domains. Drivers always see `virq`; chip code translates.

**Q2.** Walk through `handle_fasteoi_irq`.
**A.** Used for GIC where reading `IAR` both acknowledges and provides INTID. Flow: `raw_spin_lock(desc->lock)`; if `IRQS_PENDING` or no action → mask + EOI early; else mark `IRQD_IRQ_INPROGRESS`, drop lock, call `handle_irq_event` (walk action list), reacquire lock, `chip->irq_eoi()` (writes `EOIR`), clear in-progress.

**Q3.** What is `handle_percpu_devid_irq`?
**A.** Optimized flow for per-CPU PPIs where each CPU has its own `dev_id` (e.g., arch timer, PMU). Avoids the global desc lock — uses per-CPU `irqaction` storage (`request_percpu_irq`). No sharing, no affinity, EOI inline.

**Q4.** How are shared IRQs handled and what's the contract for the handler?
**A.** Driver passes `IRQF_SHARED` and a unique `dev_id`. Flow walks `desc->action`; each handler must quickly determine ownership (read a status reg), return `IRQ_HANDLED` if it handled, `IRQ_NONE` otherwise. Too many consecutive `IRQ_NONE` → kernel disables the IRQ as spurious (`note_interrupt`).

**Q5.** What does `IRQ_WAKE_THREAD` return value mean?
**A.** Returned from a primary (hardirq) handler that registered a `thread_fn` via `request_threaded_irq`. Tells core to wake the IRQ kthread to run `thread_fn` in process context. Primary typically masks the source, then returns `IRQ_WAKE_THREAD`; thread does heavy work and unmasks (with `IRQF_ONESHOT` keeping the line masked between primary return and thread completion).

---

## 7. IRQ Domains (Hierarchical)

An **IRQ domain** (`struct irq_domain`) is the translation layer between a controller's hwirq space and the global virq space. Each chip driver registers a domain via `irq_domain_create_linear/_tree` with `irq_domain_ops` containing `.translate` (decode DT/ACPI fwspec → hwirq + type), `.alloc`, `.free`, `.activate`, `.deactivate`. DT lookup uses `interrupt-parent` + `interrupts` cells; `of_irq_parse_and_map` walks parents until it reaches a domain.

**Hierarchical domains** stack controllers as parent/child. Example: a PCIe MSI → `pci-msi` domain → `its-msi` domain → `gic-v3` domain. Allocating a virq at the leaf cascades `alloc` up the chain; each level fills its slice of `irq_data` (its own chip pointer, chip_data, hwirq). At runtime, `irq_chip_*_parent` helpers forward operations (e.g., `irq_chip_mask_parent`) so a leaf chip can implement mask-by-asking-the-parent semantics. This avoids the older "chained" pattern where every level had to demultiplex in software.

`generic_handle_domain_irq(domain, hwirq)` is the modern entry point chip drivers call from their ISR. It looks up the virq, sets up `irq_enter` accounting, and invokes the flow.

### Likely Interview Questions

**Q1.** Chained vs hierarchical domain — when do you choose which?
**A.** Chained: parent IRQ runs a demux handler that calls `generic_handle_irq` on the child's virq. Used for simple GPIO-style controllers where children are pure passthrough on one parent line. Hierarchical: when each level needs to participate in per-IRQ ops (mask/affinity/MSI compose) — MSI stacks always need hierarchical.

**Q2.** What goes in `irq_domain_ops.translate`?
**A.** Parses a `struct irq_fwspec` (DT cells or ACPI GSI/triggers) into `(hwirq, type)`. For GICv3: 3 cells = type (SPI/PPI/extended) + number + flags. Returns `-EINVAL` on bad spec.

**Q3.** What does `irq_domain_alloc_irqs` do internally for a hierarchical stack?
**A.** Allocates virq from `irq_desc` pool, then walks the domain hierarchy top-down calling each `.alloc`: each level fills its `irq_data` slot, sets chip, may program HW (e.g., ITS `MAPTI`). On failure it unwinds with `.free`.

**Q4.** Give an example where `irq_chip_mask_parent` is used.
**A.** PCIe MSI leaf chip: masking an MSI vector means clearing the LPI's enable bit in the LPI config table — which is the ITS/Redistributor's job. The leaf chip's `irq_mask` is just `irq_chip_mask_parent`, forwarding to the ITS chip's mask.

**Q5.** How are MSI domains different?
**A.** Built on hierarchical domains with `msi_domain_info` describing capabilities (`MSI_FLAG_*`), an `msi_domain_ops` with `.msi_init`/`.msi_prepare`/`.set_desc`. PCI uses `pci_msi_create_irq_domain` that stacks a generic `pci-msi` layer on top of the platform's MSI parent (ITS on GICv3).

---

## 8. `request_irq` / `request_threaded_irq` and `IRQF_*` Flags

`request_irq(irq, handler, flags, name, dev)` is the workhorse for hardirq-only drivers. `request_threaded_irq(irq, handler, thread_fn, flags, name, dev)` adds a kthread for bottom-half work. If `handler` is NULL, the core supplies `irq_default_primary_handler` which simply returns `IRQ_WAKE_THREAD` — useful when there is no quick check needed at hardirq level. `free_irq` removes the action and synchronizes against in-flight handlers (waits for hardirq + thread to complete).

Important `IRQF_*` flags:

| Flag | Meaning |
|------|---------|
| `IRQF_SHARED` | Line is shared; require unique `dev_id`; handler must identify ownership |
| `IRQF_ONESHOT` | Keep line masked from hardirq return through thread completion (mandatory with `thread_fn` for level-triggered) |
| `IRQF_TRIGGER_*` | Edge rising/falling/both, level high/low — only honored at first request |
| `IRQF_NO_SUSPEND` | Don't disable across suspend (timers, wakeup paths) |
| `IRQF_NO_AUTOEN` | Don't auto-enable after registration; driver must `enable_irq` later |
| `IRQF_PERCPU` | Per-CPU IRQ (use with `request_percpu_irq`) |
| `IRQF_NO_THREAD` | On PREEMPT_RT, prevent forced-threading; the handler is truly hardirq |
| `IRQF_EARLY_RESUME` | Resume IRQ in `syscore_resume` (very early) |
| `IRQF_COND_SUSPEND` | Suspend only if the action is not the wakeup source on shared line |
| `IRQF_NOBALANCING` | Exclude from irqbalance |

### Likely Interview Questions

**Q1.** Why is `IRQF_ONESHOT` required for `request_threaded_irq` on level interrupts?
**A.** Level IRQs reassert continuously while the source is active. Without `ONESHOT` the core unmasks immediately after the primary returns, causing storm before the thread can quiesce the device. `ONESHOT` keeps masked until `thread_fn` completes and then unmasks.

**Q2.** What can/can't you do inside a primary IRQ handler?
**A.** Can't sleep, can't take mutexes, can't call functions that may sleep (kmalloc with GFP_KERNEL, copy_from_user). Can use spinlocks (`spin_lock_irqsave` only for nested locking with other contexts — the IRQ is already disabled locally), wake_up, complete, schedule softirq/work.

**Q3.** Difference between `disable_irq` and `disable_irq_nosync`?
**A.** `disable_irq` masks the line **and** waits for any in-flight handler (including the thread) to complete on all CPUs — sleeps. `disable_irq_nosync` just masks and returns immediately. Use nosync from atomic contexts where you don't care about in-flight; sync when you need to safely free device resources.

**Q4.** What happens if two drivers request the same line but only one passes `IRQF_SHARED`?
**A.** Second `request_irq` returns `-EBUSY`. All sharers must agree on `IRQF_SHARED` and on trigger type.

**Q5.** When would you use `IRQF_NO_AUTOEN`?
**A.** When you must complete device configuration before allowing IRQs to fire — e.g., set up DMA descriptors, then `enable_irq`. Avoids racing a spurious early IRQ that hits an unprepared driver.

---

## 9. Threaded IRQs and Top/Bottom Halves

A threaded IRQ splits work between a small **primary (hardirq) handler** and a **kthread** (`thread_fn`). The primary runs in true hardirq context with the line masked at the GIC; it should: read+ack a status register, decide if this is "our" IRQ, mask the source if level-triggered, return `IRQ_WAKE_THREAD`. The thread runs as a kernel thread (FIFO50 by default) and can sleep, take mutexes, do I/O. Use `request_threaded_irq` with `IRQF_ONESHOT` for level-triggered sources.

The traditional alternatives to threaded IRQs are **softirqs**, **tasklets**, and **workqueues**. Softirqs run from `irq_exit` in softirq context (still atomic, but interrupts re-enabled) — limited to compiled-in users (NET_TX/RX, BLOCK, TIMER, SCHED, RCU, TASKLET, HI). Tasklets are softirqs with per-instance serialization; **largely deprecated** in favor of threaded IRQs or workqueues for new code. Workqueues run in kthreads (`kworker`), can sleep, support concurrency-managed pools, ordered queues, delayed work.

Decision matrix for new drivers: need to sleep / take mutex / long work → **threaded IRQ** or **workqueue**; latency-critical, must run before scheduler tick → **softirq** (usually NAPI for networking); legacy code with tasklet — convert to threaded IRQ if possible.

### Likely Interview Questions

**Q1.** Latency comparison: hardirq vs softirq vs tasklet vs threaded IRQ vs workqueue.
**A.** Hardirq: lowest, ns-scale, preempts everything (except higher-pri IRQs on GIC). Softirq: runs at `irq_exit` or `ksoftirqd`, low-µs typical. Tasklet: same context as softirq, with serialization. Threaded IRQ: scheduling delay (µs-ms depending on load and policy). Workqueue: similar to threaded IRQ but uses generic kworker pool with possibly longer queue.

**Q2.** Why are tasklets discouraged?
**A.** They run in softirq context (atomic, can't sleep), don't scale (per-tasklet serialization across CPUs), and have nasty disable/enable semantics. Modern guidance: threaded IRQs for IRQ deferral, workqueues for general deferred work. New tasklet use is rejected on linux-next.

**Q3.** What priority do IRQ threads run at?
**A.** `SCHED_FIFO`, prio 50 by default. Tunable per-IRQ via `/proc/irq/N/threaded` info or `sched_setscheduler` from a privileged setup tool. On PREEMPT_RT all IRQs are forced-threaded with the same default; admins re-tune for RT workloads.

**Q4.** What's the cost of `IRQF_ONESHOT`?
**A.** The line stays masked between primary return and thread completion → some added latency, no nested deliveries, slightly higher per-IRQ overhead in the chip mask/unmask. Acceptable; required for correctness on level IRQs with threaded handlers.

**Q5.** How is the IRQ thread woken and where does it run?
**A.** On `IRQ_WAKE_THREAD`, `__irq_wake_thread` sets `IRQTF_RUNTHREAD` and `wake_up_process` on `desc->action->thread`. It runs on whatever CPU the scheduler picks (subject to affinity hints from `irq_set_affinity`).

---

## 10. Softirq vs Tasklet vs Workqueue

**Softirqs** are statically allocated bottom halves (`HI`, `TIMER`, `NET_TX`, `NET_RX`, `BLOCK`, `IRQ_POLL`, `TASKLET`, `SCHED`, `HRTIMER`, `RCU`). They run from `irq_exit` (after a hardirq) or from `ksoftirqd/N` when load grows. Pros: lowest-latency deferral, runs on the same CPU as the originating hardirq (cache-hot). Cons: atomic context (no sleep), can't be added by modules, requires careful re-entrancy design (the same softirq vector can run concurrently on multiple CPUs).

**Tasklets** are dynamic wrappers around the `TASKLET` softirq. Each tasklet is single-execution at a time across the whole system (`TASKLET_STATE_RUN` bit serializes). API: `tasklet_init`/`tasklet_schedule`/`tasklet_disable`/`tasklet_kill`. Largely deprecated; subsystem maintainers actively reject new tasklet users. Replace with `threaded IRQ`, `workqueue`, or for true atomic deferred work, dedicated softirq if you're a core subsystem.

**Workqueues** run in `kworker` kthreads. `system_wq` (concurrency-managed, shared), `system_long_wq`, `system_unbound_wq` (not CPU-bound, good for CPU-intensive work), `system_freezable_wq`. Drivers create dedicated WQs with `alloc_workqueue("name", flags, max_active)` — `WQ_UNBOUND`, `WQ_HIGHPRI`, `WQ_CPU_INTENSIVE`, `WQ_MEM_RECLAIM`, `WQ_ORDERED` (serialize). API: `INIT_WORK` / `queue_work` / `cancel_work_sync`; delayed: `INIT_DELAYED_WORK` / `queue_delayed_work` / `cancel_delayed_work_sync`.

### Likely Interview Questions

**Q1.** Same softirq running on two CPUs at once — is that possible?
**A.** Yes, softirq handlers (e.g., NET_RX) can execute concurrently on multiple CPUs. Handlers must be reentrant; per-CPU data is the usual pattern. Tasklets within `TASKLET` softirq are serialized by `TASKLET_STATE_RUN`.

**Q2.** When does `ksoftirqd` run vs inline at `irq_exit`?
**A.** After a hardirq, `irq_exit` runs pending softirqs up to `MAX_SOFTIRQ_RESTART` (10) or `MAX_SOFTIRQ_TIME` (2 ms). If still pending, wakes `ksoftirqd/N` to handle the rest, avoiding starvation of user processes.

**Q3.** What is `WQ_MEM_RECLAIM` and why does it matter?
**A.** Guarantees forward progress under memory pressure by reserving a rescuer thread that the workqueue can fall back to if normal kworker allocation fails. Required for workqueues on the writeback / swap path; recommended for storage drivers' WQs.

**Q4.** Is `queue_work_on(cpu, ...)` strict about CPU?
**A.** Yes for bound (non-unbound) workqueues — runs on that CPU. For `WQ_UNBOUND` it's a hint; the work runs on any CPU in the WQ's numa node pool.

**Q5.** `cancel_work_sync` from within the same work — deadlock?
**A.** Yes — classic deadlock. Use `cancel_delayed_work` (no sync) or restructure. `flush_work` from within itself also deadlocks. Lockdep usually catches this.

---

## 11. Affinity, `smp_affinity`, Managed IRQs

`irq_set_affinity(virq, mask)` requests the chip to route the IRQ to one of the CPUs in `mask`. Userspace exposes `/proc/irq/N/smp_affinity` (hex bitmask) and `_list` (CPU list). Default affinity is `/proc/irq/default_smp_affinity`. For GICv3 SPI, chip driver writes `GICD_IROUTER<n>` with the chosen MPIDR. For LPIs, ITS `MOVI` command.

**`irqbalance`** (userspace daemon) rebalances periodically based on rate, NUMA, power policies. Many drivers set an **affinity hint** (`irq_set_affinity_hint`) to suggest preferred CPUs (e.g., NIC RX queue N → CPU N); irqbalance respects hints.

**Managed IRQs** (`IRQD_AFFINITY_MANAGED`) — kernel-owned affinity, irqbalance must not touch them. Used by **blk-mq** and high-queue-count NICs: `pci_alloc_irq_vectors_affinity` builds an affinity mask per vector spreading across CPUs, the matching block/MSI queue is bound to that CPU set, and CPU hotplug **shuts the IRQ down** when the last CPU in its mask goes offline (rather than migrating). This preserves per-queue locality and avoids interleaving.

### Likely Interview Questions

**Q1.** What's the difference between affinity, affinity hint, and managed affinity?
**A.** Real affinity: actual routing mask (chip programmed). Hint: advisory, irqbalance reads it. Managed: kernel exclusively manages, irqbalance excluded, hotplug shuts down rather than migrates.

**Q2.** What happens to a managed IRQ when all its CPUs go offline?
**A.** It is **shutdown** (`irq_shutdown`) — chip masks and core marks inactive. The owning queue (blk-mq HW queue) is quiesced. When a CPU in the mask comes back, IRQ is `startup`'d again.

**Q3.** Can userspace change a managed IRQ's affinity?
**A.** No — writes to `/proc/irq/N/smp_affinity` for `IRQD_AFFINITY_MANAGED` return `-EIO` (or are ignored depending on kernel version). Only the kernel/driver can reconfigure.

**Q4.** Why is irqbalance often disabled on real-time / DPDK systems?
**A.** It introduces jitter (mask changes → MOVI commands, cache invalidations). RT/DPDK systems pin IRQs statically (housekeeping CPUs only) and disable the daemon entirely.

**Q5.** How does `pci_alloc_irq_vectors_affinity` compute masks?
**A.** Takes `pre_vectors` (admin, no affinity) and `post_vectors`, spreads the remaining across online CPUs respecting NUMA distance — algorithm in `lib/group_cpus.c` (`group_cpus_evenly`). Returned `cpumask` per vector becomes the managed affinity.

---

## 12. PREEMPT_RT Considerations

`PREEMPT_RT` transforms the kernel into a fully preemptible RT kernel. Key changes affecting interrupts:

- **All IRQs are forced-threaded** by default. Even drivers using `request_irq` get a kthread under the hood; the "primary" runs only to ack/mask. Drivers opting out must pass `IRQF_NO_THREAD` and be prepared to run with hard IRQs disabled — only for very short, lockless handlers.
- **`spinlock_t` becomes a sleeping `rt_mutex`**. Code that needed truly-atomic locking must use **`raw_spinlock_t`**. The hardirq paths in chip drivers (GIC, ITS) use `raw_spinlock_t` because they execute with IRQs disabled in true atomic context. Holding a regular `spinlock_t` no longer disables preemption / IRQs.
- **Softirqs run in a per-CPU `ksoftirqd` thread** (`local_bh_disable` no longer disables migration; `bh_lock_sock` is real lock). Networking and timers gain predictable latency at the cost of throughput.
- **High-resolution timers can run in hardirq context** (`HRTIMER_MODE_*_HARD`) for the very small set needing it (e.g., posix-cpu-timer expiration on RT).

Driver authors targeting RT: review every IRQ handler — is it short enough to keep as `IRQF_NO_THREAD`? Are locks `raw_spinlock_t` where shared with hardirq? Avoid `GFP_KERNEL` allocations on hot paths. Use `IRQF_ONESHOT` correctly. Be aware that `mdelay`/`udelay` in handlers is forbidden — busy-wait kills RT determinism.

### Likely Interview Questions

**Q1.** What's the difference between `spinlock_t` and `raw_spinlock_t` on PREEMPT_RT?
**A.** `spinlock_t` → `rt_mutex` (sleeping, priority-inherited). `raw_spinlock_t` → a real spinning lock that disables preemption. Use raw for: hardirq handlers, scheduler internals, code where sleeping is impossible.

**Q2.** When should a driver use `IRQF_NO_THREAD`?
**A.** Only when the handler is provably bounded-time, doesn't take any `spinlock_t` (only raw), doesn't allocate memory, and doesn't call into subsystems that may sleep on RT. Examples: arch timer IRQ, perf PMU IRQ, IPI handlers.

**Q3.** What's the latency cost of forced threading?
**A.** A few µs of scheduler latency on top of hardirq delivery (worst case ~10–50 µs on a typical arm64 server, depending on load and RT priorities). Trade-off: predictability + ability to preempt long handlers vs. raw throughput.

**Q4.** How does GIC priority masking help RT?
**A.** With pseudo-NMI, high-priority IRQs (e.g., timer for RT scheduler) can preempt other IRQs masked by PMR — bypassing the global `DAIF.I` mask. Reduces worst-case IRQ-off windows.

**Q5.** Why must `raw_spin_lock_irqsave` still be used in chip drivers on RT?
**A.** GIC chip ops execute in true atomic context (during hardirq dispatch). The `desc->lock` (a `raw_spinlock_t`) protects state across CPUs. Using `spinlock_t` would mean acquiring an rt_mutex inside hardirq — illegal.

---

## 13. MSI/MSI-X on ARM64

On ARM64, MSI/MSI-X are delivered via the **GICv3 ITS** (or platform MSI controllers like Marvell ICU, Freescale MC). The PCI subsystem sees a standard `msi_domain` parented by the platform MSI parent. `pci_alloc_irq_vectors` / `pci_alloc_irq_vectors_affinity` allocates `N` MSI-X vectors, each backed by a virq from the MSI domain. Per-vector enable/mask uses MSI-X table writes in the device; per-LPI mask uses LPI config table writes through the ITS chip.

The MSI address/data written into the device's MSI capability is computed by the chip stack: the ITS chip's `irq_compose_msi_msg` returns `(addr=GITS_TRANSLATER, data=EventID)`. PCIe writes propagate through the SMMU (which may translate the address for IOMMU/SVA) to the ITS. **SMMU + ITS integration** is critical: MSIs from a device whose stream is under SMMU must hit the SMMU-translated address; Linux relies on `iommu_dma_prepare_msi`.

PCIe **PASID/SVA** doesn't change MSI delivery directly, but virtualization does: with **GICv4** + SMMU stage-2 + ITS, MSIs from passthrough devices land directly in the guest vPE without exit. **vSVA + vITS** (still evolving) extends this.

### Likely Interview Questions

**Q1.** Why is `iommu_dma_prepare_msi` needed?
**A.** When a device is behind an SMMU, all its DMA (including MSI writes to `GITS_TRANSLATER`) goes through IOVA translation. Linux must reserve a per-device IOVA that maps to the physical `GITS_TRANSLATER` and program that IOVA into the MSI address field; otherwise the MSI write faults at the SMMU.

**Q2.** Difference between MSI and MSI-X from the driver's view?
**A.** MSI: up to 32 vectors but a single contiguous range, single address+data pair, devices may bundle. MSI-X: up to 2048 vectors, separate table entry per vector with independent address+data, per-vector mask, much better for multi-queue devices. Modern drivers use MSI-X exclusively.

**Q3.** How are MSI-X vectors masked?
**A.** Per-vector mask bit in the device's MSI-X table (set by leaf chip's `irq_mask` → which usually calls `pci_msi_mask_irq`). On GICv3 ITS, masking can additionally clear the LPI config-table enable bit via `INV` command.

**Q4.** Can MSI-X vectors be reallocated dynamically?
**A.** Historically no (alloc at probe). Since 6.x `pci_msix_can_alloc_dyn` + `pci_msix_alloc_irq_at` allow per-vector dynamic allocation — useful for VFIO / hot-add scenarios.

**Q5.** What happens if a passthrough device sends an MSI to a guest in a non-GICv4 system?
**A.** MSI traps to the host (LPI delivered to host CPU), KVM looks up the virtual ITS state, injects a virtual interrupt to the vCPU via list registers (LRs) in `ICH_LR<n>_EL2`. With GICv4 the ITS does this directly via vPE table — no host CPU cycle.

---

## 14. Devicetree Bindings for Interrupts

A node consumes interrupts via `interrupts` (and optionally `interrupts-extended` for multi-parent). The parent is `interrupt-parent` (inherited if not specified). The parent's `#interrupt-cells` determines how many cells each entry occupies.

For **GICv3** (`#interrupt-cells = <3>`): cells = `<type number flags>` where type = `0` (SPI), `1` (PPI), `2`/`3` (extended), number = INTID offset within that range, flags = trigger type / affinity (low nibble = `IRQ_TYPE_EDGE_RISING`/`LEVEL_HIGH`/etc., upper bits for PPI CPU affinity).

```dts
gic: interrupt-controller@2f000000 {
    compatible = "arm,gic-v3";
    #interrupt-cells = <3>;
    interrupt-controller;
    reg = <0x0 0x2f000000 0x0 0x10000>,    // GICD
          <0x0 0x2f100000 0x0 0x200000>;   // GICR
};

uart0: serial@9000000 {
    compatible = "arm,pl011", "arm,primecell";
    interrupts = <GIC_SPI 1 IRQ_TYPE_LEVEL_HIGH>;
    interrupt-parent = <&gic>;
};
```

For **nested controllers** (GPIO expander downstream of a SoC GPIO bank downstream of GIC), `interrupt-map` translates a child IRQ specifier into a parent specifier; `interrupt-map-mask` says which bits of the child specifier participate in the lookup.

**MSI parents** use `msi-parent = <&its>;` on PCIe root complex nodes (or `msi-map` for ITS DeviceID mapping from PCI RID).

### Likely Interview Questions

**Q1.** What does `interrupts-extended` give you over `interrupts`?
**A.** Lets each entry specify its own parent: `interrupts-extended = <&gic 0 5 4>, <&gpio0 7 1>;`. Useful for devices wired to multiple interrupt controllers (rare but real — e.g., wakeup vs runtime IRQ on different controllers).

**Q2.** Decode `interrupts = <GIC_PPI 13 (GIC_CPU_MASK_SIMPLE(4) | IRQ_TYPE_LEVEL_LOW)>`.
**A.** PPI 13 (== arch timer virtual), 4 CPUs in affinity mask (legacy v2 style; ignored by GICv3), level-low trigger. The CPU mask part is a legacy GICv2 cell artifact — GICv3 routes PPIs per-CPU automatically.

**Q3.** What's `msi-map` for?
**A.** Translates PCI Requester IDs into ITS DeviceIDs (and IOMMU stream IDs). Format: `msi-map = <rid-base &its its-base length>;` — allows multiple RIDs to share or remap to different DeviceIDs.

**Q4.** How does `interrupt-map` work?
**A.** Tuple of `<child-spec> <parent-phandle> <parent-spec>`. The OF code masks `child-spec` with `interrupt-map-mask`, finds matching row, returns parent spec for further translation. Used in PCIe IRQ swizzling (INTA/B/C/D → SoC IRQ lines).

**Q5.** What if `interrupt-parent` is missing on a leaf node?
**A.** Inherited from the closest ancestor that defines it. Root DT usually sets `interrupt-parent = <&gic>` so most nodes need not repeat it.

---

## 15. Chained vs Hierarchical Interrupt Controllers

**Chained** approach: the parent IRQ is owned by a "demux" handler installed via `irq_set_chained_handler_and_data`. When it fires, the handler reads the child controller's status register, derives the child hwirq, calls `generic_handle_domain_irq(child_domain, hwirq)`. Child IRQs share the parent's slot in `/proc/interrupts` (no separate count for parent). Simple, low overhead; no per-child masking through the parent.

**Hierarchical** approach: each level has its own `irq_data` and `irq_chip`. The child IRQ's virq stacks through both levels' `irq_data`. Mask/unmask/affinity operations at the leaf can be forwarded to the parent via `irq_chip_*_parent`. Required for MSI (because the parent needs to participate in `compose_msi_msg`), and preferred when you need per-vector affinity or wakeup at the leaf.

Conversion of legacy chained GPIO drivers to hierarchical is an ongoing kernel effort. New GPIO controllers should be hierarchical (`gpiochip_irqchip_add_domain` and related helpers).

### Likely Interview Questions

**Q1.** Can a chained handler call `request_irq`?
**A.** The chained handler itself is not registered via `request_irq` — it's set with `irq_set_chained_handler_and_data` and runs in true hardirq context for the parent IRQ. Child IRQs registered through `irq_create_mapping` + `request_irq` work normally.

**Q2.** Why doesn't `/proc/interrupts` show counts for a chained parent?
**A.** The parent IRQ is "owned" by the chain handler; its count is suppressed (`IRQ_NOPROBE | IRQ_DISABLE_UNLAZY`) to avoid double-counting — the child counts are what the user cares about.

**Q3.** When *must* you go hierarchical?
**A.** MSI (parent composes message), per-child affinity through parent, per-child wakeup configuration, situations where a leaf op cannot be implemented without parent action.

**Q4.** Hierarchical leaf chip with no real HW state — what does it look like?
**A.** `irq_mask = irq_chip_mask_parent`, `irq_unmask = irq_chip_unmask_parent`, `irq_eoi = irq_chip_eoi_parent`, `irq_set_affinity = irq_chip_set_affinity_parent`. The leaf exists mainly to provide a stable identity in the IRQ namespace.

**Q5.** What's the trade-off in terms of overhead?
**A.** Chained: one indirect call + one domain lookup per child IRQ. Hierarchical: per-op walk of the hierarchy (small fixed depth ≤4). Both negligible at handler dispatch; hierarchical wins on flexibility.

---

## 16. Nested IRQs and Interrupt Storms

ARM64 with GIC supports interrupt **priority preemption** (`ICC_BPR1_EL1` controls preemption granularity). Linux historically runs with preemption masked at GIC (all normal IRQs same priority), so a hardirq is **not** nested by another normal IRQ. With pseudo-NMI, a higher-priority NMI can preempt an in-progress IRQ.

**Storms** happen when: (a) a level-triggered source isn't quiesced by the handler before unmask, (b) a shared IRQ has many spurious deliveries, (c) a device floods MSI under load. Symptoms: 100% CPU in `si` time, `ksoftirqd` saturating, dropped device throughput. Mitigations:

- **NAPI** for networking: on IRQ, mask the line/queue and poll the device until empty or budget exhausted; only re-enable when idle. The driver becomes pull-based at high load.
- **irq_poll** for block: similar idea for storage controllers.
- **Spurious detection**: `note_interrupt` tracks `irqs_unhandled`; >99 900 unhandled out of 100 000 → disable the IRQ (`IRQ_SPURIOUS_DISABLED`), log warning. Tunable via `irqfixup`/`irqpoll` boot params.
- **Coalescing** at device level (ethtool `-C`) — let the device batch.
- **Rate limit** at chip via priority drop or queue depth.

### Likely Interview Questions

**Q1.** Why does Linux on arm64 disable GIC priority preemption by default?
**A.** Simplicity and determinism: all normal IRQs share priority, so a single per-CPU IRQ-disabled critical section is enough; no nested re-entry concerns for chip drivers. Pseudo-NMI opt-in is the only normally-active higher priority.

**Q2.** How does NAPI eliminate the storm?
**A.** On first IRQ the driver schedules NAPI and **masks** the queue's IRQ. `napi_poll` runs in softirq, pulls packets up to `budget`. If less than budget consumed → re-enable IRQ; else reschedule, keep masked. Under sustained load the device delivers zero IRQs.

**Q3.** What does "spurious IRQ" mean for a shared line?
**A.** All registered handlers returned `IRQ_NONE` — none claimed it. Tracked via `note_interrupt`; threshold reached → kernel disables to prevent CPU lockup. Log: `irq N: nobody cared (try booting with the "irqpoll" option)`.

**Q4.** Can you take an IRQ while in an IRQ handler?
**A.** Generally no on arm64 Linux (PSTATE.I set on entry, kept set throughout). With pseudo-NMI, an NMI-priority IRQ can preempt; the NMI handler must be reentrancy-safe. Synchronous exceptions (page fault on bad pointer in handler) can still nest.

**Q5.** How do you diagnose a storm in production?
**A.** `/proc/interrupts` deltas (find the runaway line), `perf top` (CPU in IRQ handler), `ftrace function_graph` filtered to the chip's IRQ entry, check `dmesg` for spurious warnings, `cat /proc/irq/N/spurious`.

---

## 17. EL2 / VHE / vGIC (KVM)

Without **VHE** (Virtualization Host Extensions), the host kernel runs at EL1 with a thin "hyp stub" at EL2. With VHE (ARMv8.1+), the host kernel runs **directly at EL2** — same code, but `TGE`/`E2H` bits in `HCR_EL2` redirect EL1 register accesses to EL2 transparently. Linux on modern arm64 servers prefers VHE — `kvm-arm` runs as a thin EL2 layer inside the host kernel.

The **vGIC** is KVM's virtualization of GICv2/v3 for guests. GICv3 hardware provides architectural support: a guest's CPU interface is the **virtual** CPU interface (`ICV_*_EL1`) backed by **List Registers** (`ICH_LR<n>_EL2`) — KVM populates LRs with pending vIRQs before `ERET`-ing into the guest; the hardware delivers them to the guest's vGIC and notifies the host via the **maintenance IRQ** when LRs drain. KVM emulates the distributor (`GICD_*` accesses trap to EL2).

With **GICv4**, the ITS delivers physical LPIs directly into the guest's vPE pending table (one per vCPU) without trapping to the host — the host only programs the mapping. With **GICv4.1**, vSGIs are also direct: guest writes to `ICC_SGI1R_EL1` hit a trap-free virtual SGI path.

### Likely Interview Questions

**Q1.** What is VHE and why is it a big deal?
**A.** VHE lets the host kernel run at EL2 while keeping its EL1-style register access semantics via `HCR_EL2.{E2H,TGE}` aliasing. Eliminates the world-switch cost between host EL1 and hyp EL2 — KVM context switches become much cheaper.

**Q2.** Walk through a guest IRQ injection on GICv3 (non-v4).
**A.** Device IRQ → host LPI → host handler in KVM ISR identifies vCPU target → KVM marks vIRQ pending in software vGIC state. On next vCPU entry, KVM scans pending vIRQs, writes them to `ICH_LR<n>_EL2`. ERET to guest; guest's vCPU interface delivers vIRQ; guest handles, writes `ICV_EOIR1_EL1` (deactivates LR). Maintenance IRQ on LR underflow lets KVM refill.

**Q3.** Why does the maintenance IRQ exist?
**A.** LRs are a small fixed set (typically 4–16). Guest may have more pending vIRQs than LRs. Maintenance fires when an LR completes; KVM uses it to refill from the software pending queue without polling.

**Q4.** What does GICv4 change?
**A.** Direct injection of physical LPIs to guest vPE — no host wakeup on every interrupt for passthrough devices. ITS maintains per-vPE tables (vPENDBASER, vPROPBASER) selected via `GICR_VPENDBASER` for the currently-resident vCPU.

**Q5.** Can a host run with `CONFIG_KVM=n` on a VHE CPU?
**A.** Yes. VHE is independent of KVM; arm64 Linux uses it for everything-at-EL2 even without virtualization, simplifying the kernel. KVM just additionally takes advantage of running at EL2.

---

## 18. Debugging Interrupts

Core tools:

- **`/proc/interrupts`** — per-CPU count per virq; rate-of-change reveals storms or unbalanced distribution. `watch -d -n1 cat /proc/interrupts`.
- **`/proc/irq/N/`** — `smp_affinity`, `smp_affinity_list`, `node`, `spurious`, `<driver-name>`. Read `spurious` to see unhandled/last_unhandled counters.
- **`ftrace`** — `events/irq/*` (`irq_handler_entry`, `irq_handler_exit`, `softirq_entry/exit`), or function_graph with filter on chip driver entry points (`gic_handle_irq`, `its_irq_handler`).
- **`perf`** — `perf record -e irq:irq_handler_entry`, `perf top -g` while problem reproduces.
- **`irqsoff` / `preemptoff` / `irqsoff+preemptoff` tracers** — capture longest IRQ-off / preempt-off latency windows with stack trace.
- **`lockdep`** — catches IRQ-context lock-order violations (`spinlock_t` taken in hardirq vs process context without `_irqsave`).
- **`sysrq-l`** — backtrace all CPUs via NMI/IPI (needs `CONFIG_ARM64_PSEUDO_NMI` for hung-CPU escape).
- **GIC state dumps** — for hangs, dump `GICD_ISPENDR`/`ISACTIVER`/`IROUTER` and per-CPU `ICC_HPPIR1_EL1` from a debugger / kdump.

### Likely Interview Questions

**Q1.** A device IRQ count isn't incrementing — how do you diagnose?
**A.** Check `/proc/interrupts` for the virq presence; check `/proc/irq/N/smp_affinity` (is the target CPU online?); read `GICD_ISENABLER` to confirm enabled; check trigger type matches device (level vs edge); enable `irq_handler_entry` tracepoint to see if it fires at all; if fires but driver doesn't see it, look for masked/stale state in the device.

**Q2.** `cat /proc/interrupts` shows only CPU0 incrementing — why?
**A.** Default affinity is CPU0, or `irqbalance` isn't running, or it's a managed IRQ pinned to CPU0's queue, or the GIC has affinity routing broken to that CPU (check `GICD_IROUTER<n>` value).

**Q3.** Hard lockup detector fired — how do you find the culprit?
**A.** With pseudo-NMI: hard lockup detector uses an NMI-priority IRQ to interrupt a hung CPU and dump its stack — that stack is the culprit (long IRQ-disabled critical section, infinite loop with IRQ off). Without pseudo-NMI: only a watchdog reset gives you anything; rely on `irqsoff` tracer to find sub-threshold cases.

**Q4.** Stuck IRQ — handler keeps firing forever.
**A.** Level-triggered source not quiesced. Driver bug: handler must ack/clear the source register. Symptom: IRQ count climbs at line rate, system unresponsive. Use ftrace + device datasheet to confirm clearing sequence.

**Q5.** How do you confirm a particular vIRQ in a guest came from the expected host source?
**A.** KVM trace events (`kvm_inject_virq`, `kvm_irq_routing`), match by GSI; on host correlate with `/proc/interrupts` delta on the passthrough device's vector; for GICv4 direct injection, check ITS event counters and vPE state.

---

## 19. Suspend/Resume & Wakeup IRQs

Across system suspend, the IRQ core suspends "normal" IRQs in `suspend_device_irqs` (called late in suspend). IRQs flagged `IRQF_NO_SUSPEND` (timers, IPIs) remain active. IRQs marked as **wakeup sources** via `irq_set_wake(virq, 1)` (or `enable_irq_wake`) are kept enabled — typically with the chip programmed to wake the SoC out of low-power state.

`IRQF_NO_SUSPEND` vs wakeup: the former says "don't bother suspending this — it must work during the suspend transition" (e.g., arch timer). Wakeup says "this IRQ should trigger system resume" (e.g., power button). They are independent flags; many wakeup IRQs are *also* `NO_SUSPEND` during the transition but the canonical wakeup path is `irq_set_wake`.

On resume, `resume_device_irqs` re-enables suspended IRQs in reverse order. `IRQF_EARLY_RESUME` resumes during `syscore_resume` (very early — before noirq → before devices) for cases like the GIC itself or clocksource controllers that the resume path depends on.

### Likely Interview Questions

**Q1.** Difference between `IRQF_NO_SUSPEND` and `enable_irq_wake`?
**A.** `IRQF_NO_SUSPEND` keeps the IRQ enabled across `suspend_device_irqs` — for IRQs needed during the suspend/resume sequence itself. `enable_irq_wake` arms the IRQ as a wakeup source so it can resume the system from a sleep state; usually programs the chip's wakeup logic.

**Q2.** Does the GIC itself need special suspend handling?
**A.** Yes — GIC distributor state (enabled, routing, priority, pending) is lost on full SoC suspend. The GIC driver saves/restores via `syscore_ops`. GICv3 ITS tables in RAM survive (if RAM is retained); pending state is rebuilt by re-enabling.

**Q3.** Why might an IRQ need `IRQF_EARLY_RESUME`?
**A.** It must be ready before device resume callbacks run — e.g., the system tick / clocksource IRQ, the GIC's own dependencies. Resumes in syscore_resume phase.

**Q4.** A wakeup IRQ doesn't wake the system — what do you check?
**A.** Was `enable_irq_wake` called and succeeded? Is the device's wakeup source enabled in its config? Does the SoC PMIC/wakeup controller route this IRQ to the wakeup mux? Check `/sys/kernel/debug/wakeup_sources` and `dmesg` for `wakeup` events.

**Q5.** `IRQF_NO_SUSPEND` on a shared IRQ — any concern?
**A.** Yes — `IRQF_COND_SUSPEND` exists for this. If one sharer is `NO_SUSPEND` and another isn't, the line stays enabled but the suspended sharer's handler returns immediately. `COND_SUSPEND` lets the core call only the wakeup-relevant handler during suspend.

---

## 20. Common Pitfalls & Gotchas

1. **Sleeping in hardirq** — calling `mutex_lock`, `msleep`, `kmalloc(GFP_KERNEL)`, or any "may sleep" API will deadlock or BUG. Use only `raw_spinlock_t`-style ops, `GFP_ATOMIC` if absolutely needed.
2. **Shared IRQ + same `dev_id`** — `free_irq` uses `dev_id` to identify which sharer to remove; identical `dev_id` between sharers leads to wrong-handler removal. Always use a unique driver-specific pointer.
3. **Level IRQ + non-`ONESHOT` threaded handler** — re-fires immediately on unmask before thread runs; storm. Always combine threaded + level with `IRQF_ONESHOT`.
4. **EOI before handler done** — calling `chip->irq_eoi` early can let the same IRQ re-deliver while you're still in the handler. Trust the flow handler.
5. **Forgetting `disable_irq` vs `disable_irq_nosync`** — `disable_irq` from a callback that itself runs in the IRQ's thread context will deadlock (waits for itself). Use `disable_irq_nosync` from such contexts.
6. **Wrong `IRQF_TRIGGER_*`** — only the first requester's trigger sticks. Conflicting later requests succeed but the chip remains at first trigger. Confirm via `cat /sys/kernel/debug/irq/irqs/N`.
7. **Affinity to offline CPU** — silently fails or pins to one online CPU. Always validate mask against `cpu_online_mask`.
8. **Managed IRQ + manual affinity** — userspace writes silently ignored. Document this in driver.
9. **MSI alloc count mismatch** — `pci_alloc_irq_vectors(min, max, ...)` may return fewer than requested. Always check return and adapt queue count.
10. **Holding `desc->lock` recursion** — calling back into IRQ core from within a chip callback re-enters and deadlocks. Use deferred work / per-cpu lockless designs.
11. **DT trigger flags on PPIs** — GICv2-era CPU mask cell is ignored on GICv3 but must still be `0xFF` or 0 for parsing. Wrong value → trigger parse error.
12. **Spurious "nobody cared"** — usually a missed init-time mask of the device. Initialize device IRQ-clean before `request_irq`, or use `IRQF_NO_AUTOEN`.

### Likely Interview Questions

**Q1.** I see `WARNING: at kernel/irq/handle.c... IRQ N: nobody cared` — what now?
**A.** Means the IRQ fired but no handler claimed it (`IRQ_NONE` from all). Common causes: device left an IRQ asserted at probe (forgot to mask/clear before requesting); shared IRQ where one driver doesn't decode its source correctly; wrong trigger type. Boot with `irqpoll` as a workaround while debugging.

**Q2.** Module load registers IRQ; module unload — what's the right teardown order?
**A.** `disable_irq` (or stop device producing IRQs first), `free_irq`, then release device resources. `free_irq` synchronizes — it returns only after no handler runs anywhere.

**Q3.** Driver A and Driver B share an IRQ. A's handler runs slow and prevents B from getting service — how to fix?
**A.** Convert A to threaded IRQ with `IRQF_SHARED | IRQF_ONESHOT`. Primary just quickly identifies if it's A's. Or use MSI-X if hardware supports — eliminates sharing entirely.

**Q4.** Hot-plug CPU0 — what happens to IRQs?
**A.** Non-managed IRQs are migrated to remaining online CPUs (`irq_migrate_all_off_this_cpu`). Managed IRQs whose mask becomes empty are shut down. Per-CPU IRQs (PPIs) on CPU0 are stopped. Reverse on hot-add.

**Q5.** You're chasing 50 µs latency spikes. Where do you start?
**A.** `irqsoff` tracer with 50 µs threshold; cross-reference with `perf sched`; look for `raw_spin_lock_irqsave` held long; check for runaway softirq cycles (`bh_time`). Validate `CONFIG_PREEMPT_RT` if hard RT is needed; tune IRQ thread priorities and isolate housekeeping CPUs.

---

## 21. Mock Interview Script (30 min)

> **Panel:** Senior kernel maintainer + platform architect.
> Style: technical, follows the thread the candidate pulls on.

**[0:00] Warm-up — exception model**
Q: "Tell me how an IRQ from a user task on ARM64 arrives at a Linux handler."
*(Expect: EL0 → vector @ VBAR_EL1+0x480, SP_EL1 task stack, kernel_entry builds pt_regs, switch to per-CPU IRQ stack, gic_handle_irq → domain → flow → driver. Bonus: mention SP_EL0=current.)*

**[0:05] GIC architecture**
Q: "Why does GICv3 exist if GICv2 was working?"
*(Expect: scale beyond 8 CPUs, sysreg CPU interface, LPIs via ITS for PCIe MSI, affinity routing. Follow-up: walk an LPI from device to handler.)*

**[0:10] Driver patterns**
Q: "I'm writing a driver for a PCIe NIC with 16 RX queues — how do you set up IRQs?"
*(Expect: pci_alloc_irq_vectors_affinity with managed affinity, MSI-X, per-queue request_irq, NAPI for storm control. Bonus: discuss IRQF_NO_THREAD on RT.)*

**[0:15] Concurrency / locking**
Q: "Your handler shares a list with a process-context updater. What lock?"
*(Expect: spinlock_t with spin_lock_irqsave (or process side, spin_lock_bh if softirq is the other side). On RT: raw_spinlock_t only if truly hardirq, else spinlock_t becomes rt_mutex. Discuss IRQF_ONESHOT for threaded variants.)*

**[0:20] Debugging scenario**
Q: "System hangs under load. dmesg shows no oops but one CPU appears stuck. How do you proceed?"
*(Expect: enable pseudo-NMI hard lockup detector, sysrq-l, irqsoff tracer, check /proc/interrupts for runaway, inspect GIC state via debugger / kdump. Discuss spurious detection.)*

**[0:25] RT / virtualization corner**
Q: "How does GICv4 change passthrough device IRQ handling?"
*(Expect: direct LPI injection to guest vPE table, no host trap on every IRQ, ITS commands MAPV/VMAPP/VMOVI for vPE management. Bonus: GICv4.1 vSGIs.)*

**[0:28] Closing trap**
Q: "Why is calling `disable_irq` from inside the IRQ's threaded handler a bug?"
*(Expect: `disable_irq` is sync — waits for in-flight handler, including the calling thread → deadlock with itself. Use `disable_irq_nosync` or restructure.)*

---

*End of document.*
