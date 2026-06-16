# Design Document: `smp_prepare_boot_cpu()` in Linux Boot (ARM64 Focus, ARM32 Contrast)

## 1. Document Control
- Title: Design and Boot-Phase Analysis of `smp_prepare_boot_cpu()`
- Scope: Linux kernel early boot path, with focus on ARM64 and contrast against ARM32
- Audience: Kernel engineers, platform bring-up teams, architecture learners
- Baseline: Mainline-style boot flow where `start_kernel()` calls `smp_prepare_boot_cpu()` after per-CPU area setup

---

## 2. Purpose
This document explains why `smp_prepare_boot_cpu()` exists, what architectural work it performs, and why ARM64 does significantly more work than ARM32 at this hook point.

The function call appears in the generic boot path:
- `start_kernel()`
- `setup_per_cpu_areas()`
- `smp_prepare_boot_cpu()`  <- architecture-specific hook
- continue early init

The hook is intentionally placed after runtime per-CPU memory is available and before broader interrupt/scheduler-dependent bring-up.

---

## 3. Problem Statement
During early boot, CPU0 (boot CPU) starts with temporary early state. Before SMP and later subsystems proceed, architecture code must:
- switch CPU0 to the runtime per-CPU base,
- capture boot CPU capabilities,
- apply CPU-specific early alternatives (if needed),
- prepare interrupt masking semantics where architecture requires it,
- initialize architecture-specific sanitization/tagging features.

If this transition is skipped or incorrectly ordered, the system can fail in subtle ways:
- wrong per-CPU accesses,
- incorrect static instruction selection,
- broken interrupt masking behavior,
- feature mismatch between boot and secondary CPUs,
- hard-to-diagnose early boot panics.

---

## 4. Context in the Boot Timeline

## 4.1 Simplified generic sequence
1. Early architecture setup (`setup_arch`)
2. `setup_nr_cpu_ids()`
3. `setup_per_cpu_areas()`
4. `smp_prepare_boot_cpu()`
5. parsing/init continues
6. later `smp_prepare_cpus()` and `smp_init()` bring up secondary CPUs

## 4.2 Why this exact placement matters
By this point:
- runtime per-CPU areas exist,
- IRQs are still tightly controlled,
- scheduler and secondary CPU startup are not fully active,
- architecture can safely perform CPU0 finalization without races from other CPUs.

---

## 5. ARM32 Design Behavior

## 5.1 Functional summary
ARM32 implementation is minimal and focused:
- Set CPU0 per-CPU offset to runtime area.

Equivalent behavior:
- `set_my_cpu_offset(per_cpu_offset(smp_processor_id()));`

## 5.2 Design rationale
ARM32 avoids heavy feature patching here. Historically, this hook in ARM32 is mainly a correctness step for per-CPU addressing continuity between early and runtime states.

## 5.3 Consequences
- Low complexity
- Smaller failure surface at this stage
- Fewer architecture side effects
- Most advanced adaptation deferred elsewhere

---

## 6. ARM64 Design Behavior

## 6.1 Functional summary
ARM64 uses this hook as an early architectural transition point:
1. Switch CPU0 to runtime per-CPU base
2. Store boot CPU capability info (`cpuinfo_store_boot_cpu()`)
3. Apply early alternatives (`apply_boot_alternatives()`)
4. Initialize GIC priority masking if active (`init_gic_priority_masking()`)
5. Initialize KASAN HW tags (`kasan_init_hw_tags()`)

## 6.2 Why ARM64 is richer here
ARM64 relies heavily on runtime feature discovery and alternative patching for efficiency and correctness. Some alternatives are required before later boot stages where interrupts/scheduling or broader concurrency appear.

## 6.3 Detailed step-by-step design

### Step A: Per-CPU base transition
- Action: `set_my_cpu_offset(per_cpu_offset(smp_processor_id()))`
- Goal: guarantee all per-CPU reads/writes for CPU0 target runtime per-CPU storage.
- Risk if omitted: corrupted per-CPU variables and incorrect CPU-local state.

### Step B: Boot CPU capability capture
- Action: `cpuinfo_store_boot_cpu()`
- Goal: record architectural features of the boot CPU as system baseline input.
- Risk if wrong: feature mismatches, invalid assumptions in alternatives/mitigations.

### Step C: Early alternatives patching
- Action: `apply_boot_alternatives()`
- Goal: patch instructions to best implementation for detected features as early as needed.
- Risk if delayed too far: execution of generic/unsafe paths in critical early code.

### Step D: Interrupt priority masking mode bridge
- Action: conditional `init_gic_priority_masking()`
- Goal: align interrupt masking with PMR-based behavior when platform config enables it.
- Risk if skipped: inconsistent interrupt masking semantics during early runtime.

### Step E: KASAN hardware tags bootstrap
- Action: `kasan_init_hw_tags()`
- Goal: ensure early memory safety tagging model is initialized on ARM64 HW-tag configurations.
- Risk if missing: incomplete sanitizer coverage or boot-time sanitizer faults.

---

## 7. ARM32 vs ARM64 Comparison

| Area | ARM32 | ARM64 |
|---|---|---|
| Per-CPU runtime offset setup | Yes | Yes |
| Boot CPU capability snapshot | Minimal/implicit | Explicit (`cpuinfo_store_boot_cpu`) |
| Early alternatives patching | Not a core action here | Explicit (`apply_boot_alternatives`) |
| Interrupt priority masking prep | Not central here | Conditional GIC PMR init |
| HW tag sanitizer early init | Not applicable in this form | Explicit KASAN HW tags init |
| Complexity at hook point | Low | Medium-high |

Key insight:
- ARM32 treats this hook mostly as per-CPU offset correction.
- ARM64 treats this hook as per-CPU correction plus early architecture feature convergence.

---

## 8. Interface Contract

## 8.1 Preconditions
- Called in early boot from generic `start_kernel()`.
- Runtime per-CPU areas already allocated.
- Boot CPU active; secondary CPU bring-up not complete.

## 8.2 Postconditions
- CPU0 uses runtime per-CPU base.
- ARM64-specific early architectural state is synchronized for boot CPU.
- System can safely proceed toward later init and SMP bring-up phases.

## 8.3 Non-goals
- Does not start secondary CPUs.
- Does not complete topology bring-up.
- Does not replace `smp_prepare_cpus()` / `smp_init()` responsibilities.

---

## 9. Failure Modes and Diagnostics

## 9.1 Typical failure classes
- Per-CPU corruption symptoms in early init
- Unexpected instruction behavior due to alternative mismatch
- IRQ masking anomalies on platforms using priority masking
- Early sanitizer faults tied to missing tag init

## 9.2 Debug strategy
1. Add early printk trace around `smp_prepare_boot_cpu()` entry/exit
2. Verify CPU0 per-CPU offset immediately after call
3. Confirm alternatives patching ran (ARM64)
4. Confirm GIC PMR path decision (ARM64)
5. Confirm KASAN HW tag init path (ARM64 builds with tagging)
6. Correlate with later SMP bring-up (`smp_prepare_cpus`, `smp_init`)

## 9.3 Suggested instrumentation points
- Before/after `set_my_cpu_offset`
- Before/after `apply_boot_alternatives`
- Branch decision for `system_uses_irq_prio_masking()`
- Before/after `kasan_init_hw_tags`

---

## 10. Ordering and Concurrency Notes
- At this hook, execution is still effectively boot-CPU serialized.
- It is intentionally before full interrupt/scheduler operation and before secondary CPU concurrency.
- Design assumption: this is the lowest-risk point for ARM64 to finalize CPU0 architectural behavior that must apply before parallel activity.

---

## 11. Security and Reliability Considerations
- Correct alternatives patching can be security-relevant (mitigation paths, safe instruction sequences).
- Wrong CPU feature baseline can produce undefined instruction usage on heterogeneous environments.
- Incorrect interrupt masking setup can produce subtle race or interrupt-latency pathologies.

---

## 12. Performance Considerations
- ARM32 path is near-trivial and cheap.
- ARM64 incurs modest early boot cost to avoid larger correctness/performance penalties later.
- Early patching avoids repeated runtime feature checks in hot paths.

---

## 13. Extensibility Guidance
When extending ARM64 `smp_prepare_boot_cpu()`:
- Keep strict ordering semantics.
- Add only operations that truly must happen this early.
- Avoid dependencies on late-initialized subsystems.
- Validate interactions with secondary CPU bring-up and CPU hotplug.

When extending ARM32 path:
- Keep minimal unless strong architectural reason exists.
- Preserve clear boundary between boot CPU preparation and later SMP phases.

---

## 14. Validation Checklist
- [ ] CPU0 per-CPU offset updated to runtime area
- [ ] No per-CPU access regressions in early boot logs
- [ ] ARM64 alternatives applied before dependent code executes
- [ ] IRQ priority masking configured correctly when enabled
- [ ] KASAN HW tags initialized successfully where configured
- [ ] Secondary CPU bring-up unaffected (`smp_prepare_cpus`, `smp_init`)

---

## 15. Short Summary
`smp_prepare_boot_cpu()` is the architecture-controlled handoff point where CPU0 transitions from early boot assumptions to runtime-correct SMP/per-CPU state.
- On ARM32, this is mostly a per-CPU offset fix-up.
- On ARM64, it is a broader early convergence step for per-CPU base, feature capture, alternatives patching, interrupt priority masking readiness, and HW-tag sanitizer initialization.

This difference reflects ARM64’s stronger dependence on early feature-driven patching and architectural runtime tuning.
