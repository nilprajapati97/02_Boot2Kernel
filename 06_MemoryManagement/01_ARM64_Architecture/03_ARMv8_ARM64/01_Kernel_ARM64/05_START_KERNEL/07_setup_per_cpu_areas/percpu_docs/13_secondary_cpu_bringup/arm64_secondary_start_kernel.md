# ARM64 `secondary_start_kernel()` — Secondary CPU Per-CPU Init

## Source Reference
- `arch/arm64/kernel/smp.c:203` — `secondary_start_kernel()` definition
- `arch/arm64/include/asm/percpu.h:15` — `set_my_cpu_offset()` definition
- `arch/arm64/kernel/alternative.c` — `apply_alternatives_all()`

---

## Context: ARM64 Secondary CPU Bring-Up

```
Boot CPU (CPU 0):                     Secondary CPU N (ARM64):

start_kernel()
  setup_per_cpu_areas()
  smp_prepare_boot_cpu()
  smp_init()
    cpu_up(N)
      __cpu_up()
        boot_secondary()              [Send PSCI CPU_ON or SGI wake]
          psci_cpu_on(cpu_id)         
                                      [EL2 or EL1 entry]
                                      secondary_entry [head.S]
                                        el2_setup()      ← configure EL2/VHE
                                        __enable_mmu()   ← enable MMU
                                        __primary_switched() variant
                                        secondary_start_kernel()  ← HERE
```

---

## `secondary_start_kernel()` — Full ARM64 Analysis

```c
/* arch/arm64/kernel/smp.c:203 */
asmlinkage notrace void secondary_start_kernel(void)
{
    u64 mpidr = read_cpuid_mpidr();  /* read MPIDR_EL1 for this CPU */
    struct mm_struct *mm = &init_mm;
    const struct cpu_operations *ops;
    unsigned int cpu;

    /* Get logical CPU number from MPIDR */
    cpu = task_cpu(current);

    pr_debug("CPU%u: Booted secondary processor [%010lx]\n",
             cpu, (unsigned long)read_cpuid_id());

    /* ═══════════════════════════════════════════════════════════════ */
    /* CRITICAL: Write per-CPU offset to tpidr_el1 (or tpidr_el2)    */
    /* ═══════════════════════════════════════════════════════════════ */
    set_my_cpu_offset(per_cpu_offset(cpu));
    /*
     * ALTERNATIVE("msr tpidr_el1, %0",
     *             "msr tpidr_el2, %0",
     *             ARM64_HAS_VIRT_HOST_EXTN)
     *
     * After this: this_cpu_*() works on this CPU.
     */

    /*
     * Apply CPU-specific alternatives for this CPU.
     * Each CPU must independently apply alternatives because some
     * alternatives are per-CPU (e.g., errata workarounds for specific
     * CPU revisions in a heterogeneous system like big.LITTLE).
     */
    apply_alternatives_all();

    /*
     * Update the secondary CPU's page tables and TLB state.
     * flush_icache_range needed after apply_alternatives_all() since
     * we modified executable code.
     */

    /* ARM64 CPU initialization */
    cpu_die_early = false;

    /*
     * Initialize the CPU:
     * - CPU features enable/disable
     * - PSTATE setup
     * - Vector table setup
     * - GIC (interrupt controller) per-CPU initialization
     */
    notify_cpu_starting(cpu);

    /* ... scheduler and hotplug state ... */

    /*
     * Signal boot CPU this CPU is alive.
     */
    complete(&cpu_running);

    /* Enter idle loop */
    cpu_startup_entry(CPUHP_AP_ONLINE_IDLE);
}
```

---

## Key Difference from ARM32: `apply_alternatives_all()`

On ARM64, after `set_my_cpu_offset()`, each secondary CPU calls:

```c
/* arch/arm64/kernel/alternative.c */
void apply_alternatives_all(void)
{
    pr_info("applying system-wide alternatives\n");

    __apply_alternatives_multi_stop(all_alternatives,
                                    ARRAY_SIZE(all_alternatives));
}
```

**Why is this needed on secondary CPUs?**

Each physical CPU core must have consistent instruction cache state. Alternatives
patching modifies executable `.text` section bytes. After patching:
- The write is to the data cache
- The instruction cache on another core may still have the old instructions

`apply_alternatives_all()` on each secondary CPU ensures:
1. The alternatives are applied (or re-verified) on this CPU
2. I-cache is flushed/invalidated for modified instruction ranges
3. ISB (Instruction Synchronization Barrier) is executed

Additionally, in heterogeneous systems (e.g., big.LITTLE with Cortex-A73 + Cortex-A53):
- Different CPU types may need different errata patches
- `apply_alternatives_all()` applies only the patches relevant to this CPU's features

---

## ARM64 vs ARM32: Secondary Bring-Up Differences

| Aspect | ARM32 `secondary_start_kernel` | ARM64 `secondary_start_kernel` |
|---|---|---|
| File | `arch/arm/kernel/smp.c:410` | `arch/arm64/kernel/smp.c:203` |
| First per-CPU action | `set_my_cpu_offset(per_cpu_offset(cpu))` | `set_my_cpu_offset(per_cpu_offset(cpu))` |
| Write instruction | `mcr p15, 0, Rn, c13, c0, 4` | `msr tpidr_el1, Xn` (or `tpidr_el2`) |
| Alternatives patching | Not done here (done at boot) | `apply_alternatives_all()` called here |
| Heterogeneous CPU support | Limited | Full (big.LITTLE, DynamIQ) |
| PSCI support | Optional | Standard (ARM64 mandates PSCI) |

---

## Ordering: `set_my_cpu_offset` Before `apply_alternatives_all`

The order matters:

```
1. set_my_cpu_offset(...)     ← MUST be first
2. apply_alternatives_all()  ← uses this_cpu_*() internally

Why:
  apply_alternatives_all() may call kernel functions that use this_cpu_*()
  These require tpidr_el1 to be valid
  If apply_alternatives_all() ran before set_my_cpu_offset(), those accesses
  would read from a garbage offset → kernel crash
```

---

## ARM64 Specific: PSCI and CPU Entry

ARM64 secondary CPUs typically use PSCI (Power State Coordination Interface):

```
Boot CPU calls:              Secondary CPU firmware response:
  psci_cpu_on(                 [EL3 firmware (TF-A/UEFI) receives PSCI call]
    cpu_id,                    [Powers on the CPU, sets PC to secondary_entry]
    __pa(secondary_entry))     [Starts executing at secondary_entry in head.S]
```

The secondary CPU's entry point:
```asm
/* arch/arm64/kernel/head.S: secondary_entry */
ENTRY(secondary_entry)
    bl  el2_setup           /* EL2 / VHE configuration (if applicable) */
    bl  set_cpu_boot_mode_flag
    b   secondary_startup
END(secondary_entry)

secondary_startup:
    /* ... */
    bl  secondary_init_el2   /* Set EL2 registers including MDCR_EL2 etc. */
    /* ... */
    b   secondary_start_kernel  /* Jump to C code */
```

---

## CPU Hotplug on ARM64

When a CPU is hotplugged off and back on:

```
CPU offline:
  cpu_die()
  → PSCI CPU_OFF call
  → CPU powers down (tpidr_el1 register lost)

CPU back online:
  boot CPU calls psci_cpu_on() again
  → secondary_entry again
  → secondary_start_kernel() again
  → set_my_cpu_offset() re-writes tpidr_el1/el2
  → apply_alternatives_all() re-applies patches
```

---

## Interview Quick Facts

| Question | Answer |
|---|---|
| Where is secondary_start_kernel defined (ARM64)? | `arch/arm64/kernel/smp.c:203` |
| First per-CPU operation? | `set_my_cpu_offset(per_cpu_offset(cpu))` |
| What instruction executes? | `msr tpidr_el1, Xn` (or `tpidr_el2` on VHE) |
| Additional call not in ARM32? | `apply_alternatives_all()` — re-applies patches per CPU |
| Why apply_alternatives on secondary? | I-cache coherency; heterogeneous CPU errata |
| Must set_my_cpu_offset come before apply_alternatives? | Yes — apply_alternatives uses this_cpu_*() |
| What protocol wakes ARM64 secondary CPUs? | PSCI (Power State Coordination Interface) |
| Is tpidr_el1 restored after hotplug? | Yes — secondary_start_kernel() runs again → set_my_cpu_offset() |
