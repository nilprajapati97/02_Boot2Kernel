# Day 28 — SMP Bring-up via PSCI

> **Goal**: Bring secondary CPUs online using PSCI `CPU_ON`, give each its own per-CPU kernel stack and idle task, set up per-CPU runqueues, fix the GIC to enable per-CPU IRQs on every core, and run the scheduler across all CPUs.
>
> **Why today**: Until now Day 1's `_start` parked secondaries in a WFE loop. Real SMP unlocks throughput and exposes the locking bugs we want to find before declaring v1.0.

---

## 1. Background

### 1.1 PSCI (Power State Coordination Interface)
ARM-defined firmware ABI exposed through `hvc` (or `smc`) calls. QEMU `virt` advertises PSCI v0.2 in the FDT under `/psci`:
```
psci {
    method = "hvc";
    cpu_on = <0xC4000003>;
    cpu_off = <0x84000002>;
    ...
};
```

`CPU_ON(target_mpidr, entry_point, context_id)` boots a secondary at EL1 with `x0 = context_id`.

### 1.2 MPIDR addressing
Each CPU's `MPIDR_EL1` has `Aff0..Aff3`. On QEMU `virt`, secondaries are `Aff0=1,2,3,...` with `Aff1=0`. So `target_mpidr = cpu_id`.

### 1.3 Per-CPU data
We keep a `struct percpu` array indexed by `smp_processor_id()`:
```c
struct percpu {
    struct task *current;
    struct task *idle;
    struct runqueue rq;
    u64    kstack_phys;
    int    cpu_id;
};
extern struct percpu percpu_data[NR_CPUS];
```

`current()` becomes `percpu_data[smp_processor_id()].current`.

### 1.4 `smp_processor_id`
Read low affinity bits:
```c
static inline u32 smp_processor_id(void){ u64 m; asm("mrs %0, mpidr_el1":"=r"(m)); return m & 0xFF; }
```

We replace `sp_el0` "current" trick by storing a per-CPU pointer in `TPIDR_EL1`:
```c
asm volatile("msr tpidr_el1, %0" :: "r"(&percpu_data[id]));
```
Then `current()` is `((struct percpu*)read_tpidr_el1())->current`.

---

## 2. Design

### 2.1 Files
```
arch/arm64/kernel/smp.c
arch/arm64/kernel/psci.c
arch/arm64/boot/smp_head.S       (entry for secondaries)
kernel/percpu.c
```

### 2.2 Boot trampoline
A secondary enters at `secondary_start_el1` with MMU **disabled**. Steps:
1. Set up SP (per-CPU kstack passed via `context_id`).
2. Enable MMU (TTBR1 already has the shared kernel pgd).
3. Jump to `secondary_main(cpu_id)`.

### 2.3 secondary kernel stack
Allocated by the primary before invoking PSCI:
```c
for (cpu = 1; cpu < n; cpu++) {
    percpu_data[cpu].kstack_phys = alloc_pages(KSTACK_ORDER);
}
```

---

## 3. Implementation

### 3.1 PSCI wrapper
```c
static u32 psci_cpu_on_id = 0xC4000003;

static long psci_invoke(u64 fn, u64 a0, u64 a1, u64 a2)
{
    register u64 x0 asm("x0") = fn;
    register u64 x1 asm("x1") = a0;
    register u64 x2 asm("x2") = a1;
    register u64 x3 asm("x3") = a2;
    asm volatile("hvc #0" : "+r"(x0) : "r"(x1),"r"(x2),"r"(x3) : "memory");
    return x0;
}

int psci_cpu_on(u64 mpidr, u64 entry_pa, u64 ctx)
{
    return psci_invoke(psci_cpu_on_id, mpidr, entry_pa, ctx);
}
```

### 3.2 `smp_head.S` — secondary entry
```asm
.section .text.smp_head, "ax"
.global secondary_start_el1
secondary_start_el1:
    // x0 = context_id = &percpu_data[cpu]
    msr   tpidr_el1, x0

    // SP from percpu->kstack_phys + STACK_SIZE  (PA still in identity map)
    ldr   x1, [x0, #PERCPU_KSTACK_PHYS]
    mov   x2, #(KSTACK_SIZE)
    add   x1, x1, x2
    mov   sp, x1

    bl    el2_to_el1          // same as primary
    bl    mmu_enable_secondary
    ldr   x30, =1f
    ret
1:
    mov   x0, x19
    bl    secondary_main      // never returns
```

`mmu_enable_secondary`:
- Set `TTBR0_EL1` to a zero-page (no identity needed after MMU on, but careful with PA->VA SP).
- `TTBR1_EL1` = primary's kernel PGD PA.
- Same `MAIR`, `TCR`, `SCTLR` as primary.
- After MMU on, switch SP to virtual: `add sp, sp, KERNEL_VA_BASE`.

### 3.3 `secondary_main`
```c
void secondary_main(struct percpu *pc)
{
    pc->cpu_id = smp_processor_id();
    gic_init_percpu();          // SGI/PPI enable + priority on this redistributor / core if
    local_irq_enable();
    timer_init_percpu();        // arm the next tick on this CPU's PPI 30

    /* idle task already created by primary in pc->idle */
    pc->current = pc->idle;
    asm volatile("msr daifclr, #2");  // unmask IRQ
    schedule();                 // pick a runnable task; falls through to idle wfi
    for (;;) asm volatile("wfi");
}
```

### 3.4 GIC per-CPU init (GICv2)
```c
void gic_init_percpu(void)
{
    /* enable banked SGIs (0..15) and PPI 30 (timer) on this CPU */
    *GICD(0x100) = 0xFFFF0000 | (1 << 30);   /* GICD_ISENABLER0 has banked PPIs */
    *GICC(0x0)  = 1;                          /* CTLR: enable */
    *GICC(0x4)  = 0xFF;                       /* PMR: allow all */
}
```
SPIs (virtio interrupts etc.) are routed to CPU 0 via `GICD_ITARGETSR<n>`. For our load nothing else needs to change.

### 3.5 Primary boot — start secondaries
```c
void smp_start_secondaries(void)
{
    extern char secondary_start_el1[];
    u64 entry_pa = (u64)secondary_start_el1 - KERNEL_VA_BASE + KERNEL_LMA;
    int n = fdt_num_cpus();          /* from /cpus parsing */
    for (int cpu = 1; cpu < n; cpu++) {
        percpu_data[cpu].cpu_id = cpu;
        percpu_data[cpu].kstack_phys = alloc_pages(KSTACK_ORDER);
        percpu_data[cpu].idle = kthread_create(idle_fn, NULL);
        int r = psci_cpu_on(cpu, entry_pa, (u64)&percpu_data[cpu]);
        if (r) printk("PSCI CPU_ON cpu%d failed: %d\n", cpu, r);
    }
}
```

### 3.6 Per-CPU runqueue + scheduler tweak
```c
struct runqueue { struct task *head, *tail; spinlock_t lock; };

void sched_add(struct task *t) {
    struct runqueue *rq = &percpu_data[t->cpu].rq;
    spin_lock(&rq->lock);
    /* enqueue */
    spin_unlock(&rq->lock);
}

void schedule(void) {
    struct percpu *pc = (struct percpu*)read_tpidr_el1();
    struct runqueue *rq = &pc->rq;
    spin_lock(&rq->lock);
    struct task *next = rq_pick(rq);
    if (!next) next = pc->idle;
    struct task *prev = pc->current;
    pc->current = next;
    spin_unlock(&rq->lock);
    if (prev != next) cpu_switch_to(prev, next);
}
```

Initial task placement: round-robin `cpu = (next_pid % nr_cpus)`. No migration yet.

### 3.7 Inter-Processor Interrupts (IPI)
SGI 0 = "reschedule":
```c
void smp_send_reschedule(int cpu) {
    *GICD(0xF00) = (1<<24) | (1u << cpu) | 0; /* TargetList | SGI 0 */
}
void ipi_handler(int sgi) { /* nothing — schedule() will pick next */ }
```
Wire SGI in IRQ vector decode; ack normally.

---

## 4. Pitfalls

1. **TTBR0/TTBR1 mix-up on secondaries**: until MMU is on you must run from identity addresses, so the entry PA matters. Once MMU on, switch SP to VA.
2. **`current()` after enabling MMU**: do not call any function that uses `current()` before you've set `TPIDR_EL1`.
3. **Spinlocks must use `WFE/SEV`**: a spinning core should `wfe`; releaser does `dsb ish; sev`. Otherwise wastes power but still correct.
4. **GIC banked vs shared registers**: SGIs/PPIs (0..31) are *banked* per CPU — each core must write its own `GICD_ISENABLER0`.
5. **Timer per-CPU**: each CPU's generic timer compare register is private; tick init must happen on each core.
6. **Cache coherency**: ensure `dsb ish` between table builds and `cpu_on`; otherwise secondary reads stale PTEs.

---

## 5. Verification

```
[INFO] CPU0: nkernel boot
[INFO] PSCI: starting CPU 1
[INFO] CPU1: online
[INFO] PSCI: starting CPU 2 ... CPU3
$ /bin/spin &  /bin/spin &  /bin/spin &  /bin/spin
# 'top'-like dump shows 4 tasks running on 4 cores
```

GDB:
```
(gdb) info threads
  Id   Target Id    Frame
* 1    CPU 0        schedule
  2    CPU 1        idle_fn (wfi)
  3    CPU 2        ...
```

Stress: run `make -j4` (host build) of something simple — guarantees all 4 cores active.

---

## 6. Stretch

- Task migration / load balancing (every Nth tick on the rq picks the longest queue and steals).
- CPU hotplug `CPU_OFF` via PSCI.
- Cache-coherency tweak: enable `SCTLR_EL1.SA` for stack alignment trap to find bugs.
- Per-CPU statistics: ticks, idle time.

---

## 7. References

- ARM PSCI v1.1 spec (DEN0022D).
- ARM Generic Interrupt Controller v2 architecture spec (IHI 0048B).
- Linux `arch/arm64/kernel/smp.c`, `psci.c`.
