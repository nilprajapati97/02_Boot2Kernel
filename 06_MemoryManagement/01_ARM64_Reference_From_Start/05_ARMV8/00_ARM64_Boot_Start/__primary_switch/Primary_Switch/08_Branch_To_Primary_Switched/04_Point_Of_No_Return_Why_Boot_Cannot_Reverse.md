# Point of No Return — Why Boot Cannot Reverse After `br x8`

**Context:** The architectural reasons why the pre-MMU boot world is permanently abandoned  
**Event:** `br x8` → `__primary_switched` → `start_kernel` → idle loop

---

## 0. The One-Way Nature of `br x8`

`br x8` (not `blr x8`) is used for the PA→VA transition. This was noted before:
there is no link register set, no return path. But the "point of no return" is
deeper than just the instruction choice — it's architectural.

---

## 1. The Identity Map Is Torn Down

The identity map (`idmap_pg_dir`) is used by the CPU for the instruction stream
during the transition window. After `__primary_switched` and well into
`start_kernel`, the identity map is replaced and freed:

```c
// arch/arm64/mm/mmu.c
void cpu_replace_ttbr1(pgd_t *pgdp, phys_addr_t ttbr1)
{
    // Replaces TTBR1 with the final init_mm page tables (post-linear-map build)
    // After this, swapper_pg_dir may be reused/freed
}

// arch/arm64/mm/init.c
void free_initmem(void)
{
    // Frees memory marked with __init: includes:
    //   idmap_pg_dir
    //   init sections of head.S code
    //   __primary_switch itself (it's in __init)
    //   early_init_stack area
}
```

After `free_initmem()`:
- `idmap_pg_dir` physical pages are returned to the allocator
- Any attempt to use the identity map would fault (page table freed)
- `__primary_switch` code itself is freed
- The pre-MMU world is unrecoverable

---

## 2. BSS Is Zeroed — Pre-MMU Stack Frames Are Gone

`__primary_switched` zeroes BSS before calling `start_kernel`:
```asm
bl  __pi_memset  // zeros [__bss_start, __bss_stop)
```

`early_init_stack` is in the `.bss` section. After zeroing:
- All stack frames from `primary_entry` through `__primary_switch` are gone
- Any return address saved on the early stack is destroyed
- Any function pointer saved on the early stack is destroyed
- No stack unwinder can reach pre-MMU frames (they are zeroed)

The pre-MMU call chain (`primary_entry → __cpu_setup → __primary_switch`) is
completely abandoned.

---

## 3. The init_task Becomes the Idle Loop

`start_kernel` → `rest_init()` → `cpu_startup_entry(CPUHP_ONLINE)`:

```c
// kernel/sched/idle.c
void cpu_startup_entry(enum cpuhp_state state)
{
    // This is PID 0, swapper — the idle task.
    // Its entire purpose from this point is:
    while (1) {
        if (cpu_is_offline(smp_processor_id()))
            arch_cpu_idle_dead();
        // ...
        arch_cpu_idle();   // ARM64: wfi or similar
        // Wait For Interrupt — low-power state until something needs to run
    }
}
```

The bootstrap task (PID 0, swapper/0) becomes an infinite idle loop. It can
never "return" to the boot code — there is no return code to return to, and the
task structure has been repurposed as the idle task.

---

## 4. The `sp_el0` / `current` Pointer Has Moved

Before `__primary_switched`, SP_EL0 was not properly set (or contained garbage).
Inside `__primary_switched`:
```asm
msr sp_el0, x4   // x4 = &init_thread_union = &init_task
```

From this point, `current` = `init_task` = swapper process. The kernel's task
management infrastructure is live. Any attempt to "go back" to pre-MMU code
would be in a context where `current` points to a valid task_struct — there is
no equivalent context in the pre-MMU world.

---

## 5. VBAR_EL1 Points to Real Kernel Vectors

Before `__primary_switched`, exception vectors were minimal stubs (panic on
any exception). Inside `__primary_switched`:
```asm
msr vbar_el1, x8   // x8 = VA of vectors table
```

The real exception vectors are now active. These vectors call kernel C code,
use `current`, use the scheduler, etc. They cannot be "backed out" to the
pre-MMU stubs safely.

---

## 6. A Timeline of Irreversibility

| Moment | What Becomes Irreversible |
|---|---|
| `br x8` executes | PC is now in VA space; identity map no longer needed |
| `add sp, x4, #THREAD_SIZE` | Old early_init_stack SP abandoned |
| `msr sp_el0, x4` | `current` = init_task; pre-MMU "context" has no equivalent |
| `msr vbar_el1, vectors` | Real exception vectors active; pre-MMU stubs gone logically |
| `__pi_memset` (BSS zero) | Pre-MMU stack frames zeroed; early_init_stack destroyed |
| `str_l w20, __boot_cpu_mode` | CPU boot mode recorded for rest of kernel life |
| `bl start_kernel` | Scheduler, memory allocator, drivers init — can't reverse |
| `cpu_startup_entry(CPUHP_ONLINE)` | Idle loop; PID 0 is now the idle task forever |
| `free_initmem()` | identity map, `__primary_switch`, head.S freed — PA lost |

---

## 7. What Happens to `__primary_switch` After Boot?

`__primary_switch` is marked `__init` (via `.head.text` section inclusion in
the `__init` range). After `free_initmem()`:
- The physical pages holding `__primary_switch` are freed
- Attempting to execute at the old `__primary_switch` VA would fault (unmapped)
- The `/proc/kallsyms` entry for `__primary_switch` remains (for debugging),
  but the code is gone

Similarly, `__enable_mmu`, `__pi_early_map_kernel`, and all of `head.S` are
freed. The entire boot path is a one-time-use code path.

---

## 8. Secondary CPUs Cannot Use `__primary_switch`

Secondary CPUs (CPU1, CPU2...) take a different path:
```asm
secondary_entry:
    ...
    bl  secondary_startup  // Not __primary_switch
```

`secondary_startup` assumes:
1. `kimage_voffset` already set (by primary CPU)
2. `swapper_pg_dir` already built (by primary CPU)
3. BSS already zeroed (by primary CPU)

Secondary CPUs skip the page table building and KASLR work. They just enable
the MMU (with the already-built `swapper_pg_dir`) and proceed to
`secondary_switched`.

This design confirms that `__primary_switch` is strictly once-per-boot,
primary-CPU-only code.

---

## 9. "Soft Reboot" (kexec) and the Exception

There is one "return to boot" scenario: `kexec`. When the kernel executes
`kexec`, it:
1. Disables interrupts
2. Tears down the current kernel
3. Copies the new kernel to a safe PA
4. Jumps to the new kernel's entry point (essentially re-running `primary_entry`)

But this is not "reversing" the boot — it's launching a SECOND kernel.
The first kernel's memory is intentionally overwritten. The new kernel runs
`__primary_switch` → `__primary_switched` → `start_kernel` as a fresh boot.

Even in `kexec`, `__primary_switch` is only executed once per kernel instance.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
The BR Xn instruction in ARMv8-A is an unconditional indirect branch: it sets PC = Xn. Unlike B (immediate) or BL (branch-with-link), BR does not use the PC-relative encoding and can reach any 64-bit address. The CPU's branch predictor can predict indirect branches using the indirect branch predictor (IBP) or indirect branch target buffer. After the MMU enable, the very first BR to a kernel VA address triggers a TLB miss (if the kernel VA is not yet in the TLB), which initiates a page-table walk using TTBR1_EL1. The walk succeeds if the kernel page tables have been correctly set up by __pi_early_map_kernel.

### Kernel Perspective (Linux ARM64)
In __primary_switch, the sequence:
  ldr x8, =primary_switched   // load VA of target function
  br  x8                       // jump to kernel VA
This is the point-of-no-return: the CPU jumps from the identity-mapped PA region to the kernel VA. After BR x8, x30 (link register) still holds the identity-mapped return address from the original caller, but it is never used again. The CPU is now fully in kernel VA space. Spectre-v2 mitigations require that indirect branches be protected: on patched kernels, BR x8 may be replaced by a retpoline or enhanced IBRS sequence.

### Memory Perspective (ARMv8 Memory Model)
The BR x8 instruction that transitions the CPU from PA to VA is a memory system event: it changes the instruction fetch address from an identity-mapped PA (low VA) to a high kernel VA. At this point, TTBR0_EL1 still points to the identity map (it will be cleared later when init_task's mm is set to NULL). TTBR1_EL1 covers the target VA. The TLB may not yet have an entry for the target VA; the first access causes a hardware page-table walk. If the page-table walk finds a valid entry (Normal, Execute permission, correct PA), the CPU populates the TLB and continues. This walk is the first use of the kernel page tables built by __pi_early_map_kernel.