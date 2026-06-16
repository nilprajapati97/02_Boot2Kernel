# CPU IDs, APIC, and CPU Topology

## x86 APIC Architecture

### Local APIC (LAPIC)
Every x86 CPU core has its own **Local APIC** (Advanced Programmable Interrupt Controller):

```
CPU Package
├── Core 0
│   ├── LAPIC (APIC ID = 0)   ← sends/receives IPIs, handles timer interrupt
│   └── Execution Units
├── Core 1
│   ├── LAPIC (APIC ID = 1)
│   └── Execution Units
...
└── I/O APIC (IOAPIC)         ← routes external device interrupts to LAPICs
```

### xAPIC vs x2APIC
| Feature | xAPIC | x2APIC |
|---------|-------|--------|
| APIC ID width | 8 bits (0-255) | 32 bits |
| Access method | MMIO (mem-mapped regs) | MSR-based |
| Max CPUs | 255 | 4 billion |
| Linux detection | `CPUID.1:EBX[31:24]` | `CPUID.0x1F` |

Linux enables x2APIC early in `setup_arch()` on systems with >255 CPUs (large servers like those at Google/AWS).

---

## ARM64 MPIDR Register

```
MPIDR_EL1 Layout:
Bit 31  = MT (Multi-Threading)
Bits 39:32 = Aff3 (e.g., socket/package)
Bits 23:16 = Aff2 (e.g., cluster)
Bits 15:8  = Aff1 (e.g., core)
Bits 7:0   = Aff0 (e.g., thread within SMT)
```

Example: Qualcomm Snapdragon (big.LITTLE):
```
Cortex-A510 (efficiency core 0): MPIDR = 0x0000_0100 (Aff1=1, Aff0=0)
Cortex-A510 (efficiency core 1): MPIDR = 0x0000_0101
Cortex-X3   (performance core 0): MPIDR = 0x0000_0200 (Aff1=2, Aff0=0)
```

---

## CPU Topology Masks

Linux maintains several bitmasks for CPU state:

```c
// kernel/cpu.c
cpumask_t cpu_possible_mask;    // CPUs that could ever be online
cpumask_t cpu_present_mask;     // CPUs currently present (physical)
cpumask_t cpu_online_mask;      // CPUs currently running tasks
cpumask_t cpu_active_mask;      // CPUs that can run tasks (not isolated)
```

State transitions:
```
possible → present → online → active
  (ACPI/DT)  (hotplug in)  (boot_cpu_init)  (sched)
```

---

## Interview Q&A

### Q1: What is an IPI (Inter-Processor Interrupt) and how is it sent?
**A:** An IPI is an interrupt sent from one CPU to another via the APIC bus. On x86, CPU A sends an IPI to CPU B by writing to its own LAPIC's ICR (Interrupt Command Register) with:
- Destination APIC ID = CPU B's APIC ID
- Vector = the interrupt vector to deliver
- Delivery mode = FIXED or NMI

The hardware delivers the interrupt to CPU B's LAPIC, which raises the interrupt to CPU B's core. IPIs are used for TLB shootdowns, function calls (`smp_call_function`), scheduler reschedules, and CPU hotplug coordination.

### Q2: What is a TLB shootdown and why is it expensive on large SMP systems?
**A:** When a process unmaps a page, the TLB entries on ALL CPUs that have that mm loaded must be invalidated (since TLBs are per-CPU). CPU A sends IPI to all other CPUs → each CPU executes `flush_tlb_func()` → `invlpg` (x86) or `tlbi` (ARM). On 1000-CPU servers (Google Borg), this means 999 IPIs per munmap! This is why Linux uses batched TLB flushes (`arch_tlbbatch_*`) and `mmu_gather` mechanisms to defer and coalesce TLB shootdowns.
