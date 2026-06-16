# ARM32 `secondary_start_kernel()` — Secondary CPU Per-CPU Init

## Source Reference
- `arch/arm/kernel/smp.c:410` — `secondary_start_kernel()` definition
- `arch/arm/include/asm/percpu.h:17` — `set_my_cpu_offset()` definition

---

## Context: Secondary CPU Bring-Up Flow

```
Boot CPU (CPU 0):                     Secondary CPU N:
                                      
start_kernel()                        
  setup_per_cpu_areas()               
  smp_prepare_boot_cpu()              [sleeping in WFI / bootrom]
  smp_init()
    cpu_up(N)
      __cpu_up()
        boot_secondary()              [sends SGI or writes pen release address]
          [platform-specific]         
                                      [wakes up from WFI]
                                      head.S: __secondary_switched()
                                        [MMU enabled, VA space active]
                                        [stack pointer set]
                                        secondary_start_kernel()   ← HERE
```

---

## `secondary_start_kernel()` — Full ARM32 Analysis

```c
/* arch/arm/kernel/smp.c:410 */
asmlinkage void secondary_start_kernel(void)
{
    struct mm_struct *mm = &init_mm;
    unsigned int cpu;

    /* Disable preemption for the entirety of CPU initialization */
    preempt_disable();

    /* Get this CPU's ID (reads MPIDR register) */
    cpu = smp_processor_id();

    pr_debug("CPU%u: Booted secondary processor\n", cpu);

    /*
     * Update system coherency:
     * On Cortex-A9 MPCore: join SMP domain
     * On other platforms: enable CCI (Cache Coherency Interconnect) port
     */

    /* ═══════════════════════════════════════════════════════════════ */
    /* CRITICAL: Set per-CPU offset for this CPU                      */
    /* Must happen BEFORE any this_cpu_*() access                     */
    /* ═══════════════════════════════════════════════════════════════ */
    set_my_cpu_offset(per_cpu_offset(cpu));
    /* Expands to:
     *   offset = __per_cpu_offset[cpu]
     *   asm volatile("mcr p15, 0, %0, c13, c0, 4" :: "r"(offset))
     *   TPIDRPRW on this core = __per_cpu_offset[cpu]
     *
     * After this line: this_cpu_*() and per_cpu() work correctly
     * for this CPU.
     */

    /*
     * Initialize this CPU:
     * - Reinitialize CPU vectors, interrupt controller setup
     * - Enable MMU features needed per-CPU (like ASID)
     */
    cpu_init();

    /*
     * Enable local interrupts.
     * Safe now because per-CPU interrupt data structures are initialized.
     */
    local_irq_enable();

    pr_debug("CPU%u: Calibrating delay loop...\n", cpu);
    calibrate_delay();

    /*
     * Notify the hotplug system that this CPU is starting.
     * Transitions CPU from CPUHP_BRINGUP_CPU to CPUHP_AP_ONLINE_IDLE.
     */
    notify_cpu_starting(cpu);

    /*
     * Signal boot CPU that this CPU is alive.
     * The boot CPU is blocked in __cpu_up() waiting for this.
     */
    complete(&cpu_running);

    preempt_enable();

    /*
     * Enter the idle loop.
     * CPU is now fully online and will pick up work from the scheduler.
     */
    cpu_startup_entry(CPUHP_AP_ONLINE_IDLE);
    /* Never returns */
}
```

---

## Where is `set_my_cpu_offset()` in the Sequence?

Key ordering requirements:

```
secondary_start_kernel():
  1. preempt_disable()          ← Must be first: no migration until we're set up
  2. cpu = smp_processor_id()   ← Get CPU ID (reads MPIDR, uses per-CPU)
  3. set_my_cpu_offset(...)     ← WRITE TPIDRPRW  ← this is what we focus on
  4. cpu_init()                 ← Uses this_cpu_*() internally
  5. local_irq_enable()         ← Can now handle interrupts
  ...
```

**Why must step 3 come before step 4?**

`cpu_init()` calls functions that use `this_cpu_*()` macros. Without `set_my_cpu_offset()`
having run, `mrc p15,0,r0,c13,c0,4` would return garbage (TPIDRPRW was never written
for this core).

**Why can step 2 (`smp_processor_id()`) work before step 3?**

`smp_processor_id()` on ARM32 reads from `MPIDR_EL1` (multiprocessor affinity register)
and performs a table lookup, NOT from `TPIDRPRW`. So it doesn't depend on per-CPU setup.

---

## ARM32 Assembly: The Actual MCR Instruction

When `set_my_cpu_offset(per_cpu_offset(cpu))` is called on secondary CPU, say CPU 2:

```asm
; In set_my_cpu_offset():
; Argument: r0 = __per_cpu_offset[2] = 0x0190E000

mcr   p15, 0, r0, c13, c0, 4
;                              Writes 0x0190E000 to TPIDRPRW on this core

; No subsequent instruction immediately needed:
; TPIDRPRW is readable by the very next MRC (no pipeline interlock needed
; on ARMv7 — TPIDRPRW writes are coherent with subsequent reads on the same core)
```

---

## What `__per_cpu_offset[cpu]` Holds at This Point

By the time `secondary_start_kernel()` runs on CPU N:
- `setup_per_cpu_areas()` has already run on the boot CPU (much earlier)
- `__per_cpu_offset[]` is fully populated and read-only
- CPU N just looks up `__per_cpu_offset[N]` and writes it to its TPIDRPRW

```
Global array in kernel data section (read-only after boot):
  __per_cpu_offset[0] = 0x01800000  ← written to TPIDRPRW by smp_prepare_boot_cpu
  __per_cpu_offset[1] = 0x01887000  ← written to TPIDRPRW by CPU1's secondary_start_kernel
  __per_cpu_offset[2] = 0x0190E000  ← written to TPIDRPRW by CPU2's secondary_start_kernel
  __per_cpu_offset[3] = 0x01995000  ← written to TPIDRPRW by CPU3's secondary_start_kernel
```

---

## CPU Hotplug: Re-Writing TPIDRPRW

On ARM systems that support CPU hotplug (CPU offline/online cycles), the TPIDRPRW
must be re-written after a CPU is powered back on from offline state:

```c
/* When CPU N comes back online after being hotplugged off: */
/* The CPU's TPIDRPRW is lost (hardware register reset) */
/* secondary_start_kernel() is called again → set_my_cpu_offset() writes it again */
```

This is transparent to the kernel because `secondary_start_kernel()` is the entry
point for both initial boot AND hotplug re-online.

---

## Interview Quick Facts

| Question | Answer |
|---|---|
| Where is secondary_start_kernel defined (ARM32)? | `arch/arm/kernel/smp.c:410` |
| First thing it does for per-CPU? | `set_my_cpu_offset(per_cpu_offset(smp_processor_id()))` |
| What instruction is executed? | `mcr p15, 0, Rn, c13, c0, 4` (write TPIDRPRW) |
| What value is written? | `__per_cpu_offset[cpu]` (computed earlier by setup_per_cpu_areas) |
| Why preempt_disable before set_my_cpu_offset? | Prevent migration before per-CPU is set up |
| Must TPIDRPRW be re-written on hotplug? | Yes — hardware register is reset on power-down |
| How does smp_processor_id work before set_my_cpu_offset? | Reads MPIDR register, not TPIDRPRW |
