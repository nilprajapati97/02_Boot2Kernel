# smp_setup_block — System Design Approach and Q&A

## 1. The SMP Boot Sequence Overview

The SMP setup block in `setup_arch()` is only the **discovery and preparation** phase. Actual secondary CPU startup happens much later:

```
setup_arch() — early boot (BSP only):
  smp_set_ops()        → select mechanism
  smp_init_cpus()      → discover CPUs, mark possible/present
  smp_build_mpidr_hash() → build hash for fast lookup

start_kernel() → rest_init() → kernel_init():
  smp_prepare_cpus()   → allocate idle threads, page tables per-CPU
  smp_init()           → actually call cpu_up() for each possible CPU
    cpu_up(1)          → smp_boot_secondary(1) → PSCI CPU_ON → CPU1 online
    cpu_up(2)          → smp_boot_secondary(2) → PSCI CPU_ON → CPU2 online
    cpu_up(3)          → smp_boot_secondary(3) → PSCI CPU_ON → CPU3 online
```

---

## 2. Why Split Discovery (setup_arch) from Bringup (kernel_init)?

At `setup_arch()` time:
- Buddy allocator not ready (can't allocate per-CPU data)
- Scheduler not initialized (no task structures for idle threads)
- IRQ subsystem not initialized (CPUs need an IRQ controller setup)
- GIC (Generic Interrupt Controller) not configured

Secondary CPUs need all of the above to run:
- Per-CPU idle task (`init_idle()`)
- Per-CPU IRQ stacks
- GIC CPU interface configured for this CPU

So the sequence is: `setup_arch()` discovers, `kernel_init()` bootstraps everything needed, then brings CPUs online.

---

## 3. Dependency Graph

```
[arm_dt_init_cpu_maps()]
  └── cpu_logical_map[], cpu_possible_mask set
        │
[psci_dt_init()]
  └── psci_ops.cpu_on available
        │
[smp_set_ops(mdesc->smp or psci_smp_ops)]
  └── smp_ops.smp_boot_secondary = psci_boot_secondary
        │
[smp_init_cpus()]
  └── smp_ops.smp_init_cpus() → marks cpu_present_mask
        │
[smp_build_mpidr_hash()]
  └── mpidr_hash[] built
        │
[... many kernel subsystems initialized ...]
        │
[smp_init() — from kernel_init]
  ├── cpu_up(1) → secondary CPU boots
  ├── cpu_up(2) → secondary CPU boots
  └── cpu_up(3) → secondary CPU boots
```

---

## 4. The Per-CPU Boot Protocol

When `smp_boot_secondary(cpu, idle)` → PSCI CPU_ON is called:

```
Boot CPU (CPU0):                    Secondary CPU (CPU1):
                                    [powered off]
PSCI CPU_ON(mpidr, entry) ──SMC──→ ATF at EL3
                                    ATF powers on CPU1
                                    ATF sets PC = secondary_entry
                                    ATF returns via ERET
                                    CPU1: secondary_entry runs
                                    CPU1: enables MMU
                                    CPU1: signals "I'm booting" via 
                                          atomic secondary_boot_lock
CPU0: waits on secondary_boot_lock
                                    CPU1: secondary_start_kernel()
                                          → cpu_startup_entry(CPUHP_ONLINE)
                                          → idle loop
CPU0: secondary_boot_lock set → CPU1 online!
```

---

## 5. System Design Q&A

**Q: What is the per_cpu mechanism and why must secondary CPUs be online before it works?**
> `per_cpu(variable, cpu)` accesses CPU-local storage — each CPU has its own copy of certain variables (scheduler runqueues, interrupt counters, etc.). The per-CPU data areas are allocated per-CPU during boot (`.percpu` section is replicated). The secondary CPU accesses its per-CPU data via a CPU-specific base offset stored in a dedicated register (`TPIDR_EL1` on ARM64, stored in `r10/sl` on ARM32). Until the secondary CPU boots and sets up this register, it cannot use `per_cpu()`. After `cpu_up()` completes, the secondary CPU's per-CPU register is configured and `per_cpu(var, cpu_id)` from any CPU correctly accesses that secondary's data.

**Q: What happens to the smp_operations after boot — is it kept in memory?**
> The `smp_operations` struct is marked `__initdata` for non-PSCI boards: `struct smp_operations omap4_smp_ops __initdata = {...}`. This means the data is in the `.init.data` section which is freed after boot by `free_initmem()`. After all secondary CPUs are online, the SMP bringup functions are no longer needed. For PSCI, the `psci_smp_ops` is also `__initdata`. The `smp_ops` global in `arch/arm/kernel/smp.c` gets a copy of the struct (by value, not pointer), so even after `.init.data` is freed, `smp_ops` in the always-live `.data` section retains valid function pointers for CPU hotplug.

**Q: Can smp_build_mpidr_hash() be called before smp_init_cpus()? Does order matter?**
> `smp_build_mpidr_hash()` needs `cpu_possible_mask` to be fully populated — it calls `for_each_possible_cpu(i)` to iterate all possible CPUs and read their `cpu_logical_map[i]`. If called before `smp_init_cpus()`, some CPUs might not be in the possible mask yet (for non-PSCI platforms that discover CPUs in `smp_init_cpus()`). For PSCI platforms, `arm_dt_init_cpu_maps()` already populated the possible mask, so order doesn't matter. But for correctness on all platforms, the order is `smp_set_ops()` → `smp_init_cpus()` (fills possible mask) → `smp_build_mpidr_hash()` (uses possible mask). This is the order in `setup_arch()`.
