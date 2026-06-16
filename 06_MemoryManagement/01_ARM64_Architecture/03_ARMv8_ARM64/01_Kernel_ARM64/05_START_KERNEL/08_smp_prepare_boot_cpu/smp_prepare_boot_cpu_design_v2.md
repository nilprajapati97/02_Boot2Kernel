# SMP Prepare Boot CPU: Detailed Design Document

## 1. Objective
Define the design intent, execution timing, invariants, and architecture-specific behavior of `smp_prepare_boot_cpu()` with:
- Primary focus: ARM64
- Contrast baseline: ARM32

The goal of this hook is to transition CPU0 from early-boot assumptions to runtime-correct SMP/per-CPU state before broader kernel initialization progresses.

## 2. Scope
Included:
- Placement in `start_kernel()` flow
- Responsibilities at this hook point
- ARM64 internal sequence and rationale
- ARM32 behavior and why it is minimal
- Risks, diagnostics, and extension guidance

Excluded:
- Full secondary CPU bring-up internals (`smp_prepare_cpus()`, `smp_init()` deep internals)
- Generic scheduler design
- Platform-specific PSCI or firmware protocol details

## 3. Boot Lifecycle Placement
The generic boot path executes approximately:

1. `setup_arch()`
2. `setup_nr_cpu_ids()`
3. `setup_per_cpu_areas()`
4. `smp_prepare_boot_cpu()`
5. continue early core init
6. later in `kernel_init_freeable()` -> `smp_prepare_cpus()`
7. `smp_init()` brings up secondaries

Why this placement is correct:
- Runtime per-CPU memory is available.
- Boot is still serialized on CPU0.
- Interrupt/scheduler complexity is still controlled.
- Secondary CPU concurrency has not started.

## 4. Design Contract

### 4.1 Preconditions
- Executing on boot CPU (CPU0).
- Runtime per-CPU allocation already completed.
- No full SMP parallelism yet.

### 4.2 Postconditions
- CPU0 uses runtime per-CPU base.
- Architecture-required boot-CPU finalization is complete.
- Subsequent init code can assume correct CPU-local addressing and feature baseline.

### 4.3 Non-goals
- No secondary CPU startup here.
- No full topology/scheduler SMP activation.

## 5. ARM32 Design

### 5.1 Functional behavior
ARM32 implementation is intentionally small:
- Set CPU0 per-CPU offset to runtime offset.

Conceptually:
- `set_my_cpu_offset(per_cpu_offset(smp_processor_id()));`

### 5.2 Rationale
ARM32 uses this hook primarily as a correctness bridge from early per-CPU context to runtime per-CPU context. Most advanced arch behavior is handled in other stages.

### 5.3 Implications
- Simple and low risk.
- Minimal side effects at this point.
- Clear responsibility boundary.

## 6. ARM64 Design

### 6.1 Functional behavior
ARM64 performs a broader convergence step at this hook:
1. Switch CPU0 to runtime per-CPU base.
2. Store boot CPU capabilities.
3. Apply early alternatives patching.
4. Conditionally initialize GIC priority masking.
5. Initialize KASAN hardware tags.

### 6.2 Why ARM64 does more
ARM64 depends heavily on feature-driven alternatives and early architectural configuration. Deferring these can cause incorrect instruction paths or mismatched early behavior.

### 6.3 Step-by-step intent

#### Step A: runtime per-CPU base
Purpose:
- Ensure all CPU-local accesses for CPU0 use final per-CPU mapping.

Failure risk:
- Wrong CPU-local data access, unstable boot behavior.

#### Step B: boot CPU capability snapshot
Purpose:
- Record boot CPU feature state as input to system feature policy.

Failure risk:
- Wrong assumptions in feature-gated code.

#### Step C: early alternatives
Purpose:
- Patch instruction sequences to feature-optimal/safe variants early.

Failure risk:
- Suboptimal or invalid instruction paths before later init.

#### Step D: IRQ priority masking init
Purpose:
- Align interrupt masking semantics for PMR-based systems.

Failure risk:
- Interrupt masking inconsistencies and timing anomalies.

#### Step E: KASAN HW tags init
Purpose:
- Bring HW tag sanitizer machinery online at the required stage.

Failure risk:
- Early sanitizer inconsistencies or faults.

## 7. ARM32 vs ARM64 Summary Matrix

| Dimension | ARM32 | ARM64 |
|---|---|---|
| Runtime per-CPU base switch | Yes | Yes |
| Boot CPU feature capture | Limited at this hook | Explicit |
| Early alternatives patching | Not central here | Explicit and important |
| Early IRQ priority masking setup | Typically no | Conditional, explicit |
| HW tag KASAN setup | N/A in this form | Explicit |
| Complexity at hook | Low | Moderate |

Conclusion:
- ARM32: mostly per-CPU correctness handoff.
- ARM64: per-CPU handoff + early architecture convergence.

## 8. Invariants
After `smp_prepare_boot_cpu()`:
- `my_cpu_offset` is valid runtime offset for CPU0.
- CPU0-local variables are safe for normal early kernel use.
- On ARM64, early alternatives-sensitive code can execute with expected instruction variants.

## 9. Failure Modes

### 9.1 Per-CPU address corruption
Symptoms:
- Early crashes in unrelated code paths.
- Nonsensical per-CPU variable values.

Root cause candidates:
- Missed/incorrect runtime offset update.

### 9.2 Alternative mismatch
Symptoms:
- Illegal instruction faults or incorrect behavior on specific SoCs.

Root cause candidates:
- Missing or delayed alternative patching.

### 9.3 IRQ mask behavior anomalies (ARM64)
Symptoms:
- Spurious interrupt timing behavior during early boot.

Root cause candidates:
- GIC priority masking init skipped or misordered.

### 9.4 Sanitizer issues (ARM64)
Symptoms:
- KASAN tag-related early boot failures.

Root cause candidates:
- HW tags not initialized at required point.

## 10. Debug and Verification Plan

### 10.1 Lightweight tracing
Add temporary traces around each step in `smp_prepare_boot_cpu()`:
- Entry/exit trace
- Post `set_my_cpu_offset`
- Post feature capture
- Post alternatives
- PMR branch decision
- Post KASAN HW tags

### 10.2 Data checks
- Confirm CPU0 reports expected per-CPU offset.
- Confirm alternatives report/patched status if instrumentation exists.
- Confirm PMR path only when expected by configuration/platform.

### 10.3 Regression checks
- Boot on:
  - ARM64 with and without PMR path enabled
  - ARM64 with KASAN HW tags config variants
  - ARM32 SMP and UP configurations
- Ensure no regressions in secondary CPU bring-up.

## 11. Extension Rules
When adding logic to this hook:
- Keep operation strictly boot-CPU local.
- Do not add dependencies requiring full scheduler/IRQ environment unless proven safe.
- Preserve ordering: per-CPU base first.
- Keep idempotence assumptions clear (hook executes once during boot).

## 12. Recommended Mental Model
Treat `smp_prepare_boot_cpu()` as:
- The final boot-CPU architectural normalization point before broader kernel progression.

For ARM32:
- Normalization is mostly per-CPU pointer correction.

For ARM64:
- Normalization includes CPU feature baseline establishment and early execution-path shaping.

## 13. Quick Reference
- Called from generic early boot in `start_kernel()`.
- Comes after runtime per-CPU area creation.
- Precedes global bring-up stages and secondary CPU activation.

## 14. Final Takeaway
`smp_prepare_boot_cpu()` is small in API shape but high in architectural importance.

- On ARM32, it is mainly a precise correctness handoff for CPU0 per-CPU access.
- On ARM64, it is a strategic early convergence point for per-CPU state, feature policy, patching behavior, interrupt masking mode, and sanitizer readiness.

A correct implementation reduces downstream complexity and prevents subtle early-boot failures that are otherwise difficult to root-cause.
