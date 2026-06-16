## `primary_entry` Control Flow

## End-to-End ARM64 Kernel Boot Call Flow

Starting from the bootloader handing control to the kernel:

---

```
[Bootloader]
    │  x0 = FDT physical address
    │
    ▼
[Kernel Image Header]  (.head.text / __HEAD)
    │  efi_signature_nop
    │  b primary_entry          ← branch to real entry
    ▼
SYM_CODE_START(primary_entry)          arch/arm64/kernel/head.S
    │
    ├── bl record_mmu_state            (local, __INIT)
    │       Reads SCTLR_EL1 or SCTLR_EL2
    │       x19 = 0      → MMU was OFF
    │       x19 = M bit  → MMU was ON
    │
    ├── bl preserve_boot_args          (local)
    │       x21 = x0 (FDT pointer saved)
    │       Stores x0–x3 → boot_args[]
    │       If MMU off: dmb + dcache_inval_poc on boot_args
    │       If MMU on:  str mmu_enabled_at_boot
    │
    ├── Set up early stack
    │       sp = early_init_stack
    │       x29 = 0  (null frame pointer)
    │
    ├── bl __pi_create_init_idmap      arch/arm64/mm/pi/map_range.c
    │       Builds identity-map (phys=virt) page tables
    │       into __pi_init_idmap_pg_dir
    │       Returns end of used region in x0
    │
    ├── [MMU was OFF] ─────────────────────────────────────────
    │       dmb sy
    │       blr dcache_inval_poc       ← invalidate page table cache lines
    │
    ├── [MMU was ON] ──────────────────────────────────────────
    │       blr dcache_clean_poc       ← clean __idmap_text to PoC
    │                                    (safe to run with MMU off)
    │
    ├── bl init_kernel_el              (local)
    │   │   Reads CurrentEL
    │   │
    │   ├── [EL1] → init_el1
    │   │       Sets SCTLR_EL1 = INIT_SCTLR_EL1_MMU_OFF
    │   │       Sets SPSR/ELR, eret back with BOOT_CPU_MODE_EL1
    │   │
    │   └── [EL2] → init_el2
    │           Sets up HCR_EL2, SCTLR_EL2
    │           Installs hyp stub vectors (__hyp_stub_vectors)
    │           Configures VHE or nVHE
    │           eret back with BOOT_CPU_MODE_EL2
    │
    │   w0 = cpu boot mode → saved to x20
    │
    ├── bl __cpu_setup                 arch/arm64/mm/proc.S
    │       Configures:
    │         MAIR_EL1  (memory attribute indirection)
    │         TCR_EL1   (translation control, VA bits)
    │         MDSCR_EL1 (debug)
    │         CPU errata workarounds
    │       Returns SCTLR_EL1 value (MMU-enable bits) in x0
    │
    └── b __primary_switch             (local, .idmap.text)
            │
            ├── adrp x1, reserved_pg_dir
            ├── adrp x2, __pi_init_idmap_pg_dir
            ├── bl __enable_mmu                (local)
            │       Validates granule size (ID_AA64MMFR0_EL1)
            │       Sets TTBR0_EL1 = idmap
            │       Sets TTBR1_EL1 = kernel page tables
            │       set_sctlr_el1 → MMU ON  ← MMU enabled here
            │
            ├── sp = early_init_stack  (reset stack after MMU on)
            ├── x29 = 0
            ├── x0 = x20 (boot status), x1 = x21 (FDT)
            ├── bl __pi_early_map_kernel       arch/arm64/mm/pi/map_kernel.c
            │       Maps kernel image into virtual address space
            │       Applies relocations (KASLR if enabled)
            │
            └── br x8 → __primary_switched    (virtual address, .text)
                    │
                    ├── init_cpu_task x4(init_task)
                    │       msr sp_el0, init_task
                    │       Sets up kernel stack pointer
                    │       Creates final unwind frame in pt_regs
                    │       Loads shadow call stack (SCS)
                    │       Sets per-CPU offset
                    │
                    ├── msr vbar_el1, vectors  ← install exception vectors
                    │
                    ├── str __fdt_pointer = x21
                    ├── str kimage_voffset  (virt - phys offset)
                    │
                    ├── bl set_cpu_boot_mode_flag
                    │       Stores EL1/EL2 boot mode → __boot_cpu_mode[]
                    │
                    ├── bl kasan_early_init    (if KASAN enabled)
                    │
                    ├── bl finalise_el2        arch/arm64/kernel/hyp-stub.S
                    │       Promotes to VHE if possible
                    │
                    └── bl start_kernel        init/main.c
                            │
                            ├── setup_arch()
                            ├── mm_init()
                            ├── sched_init()
                            ├── ... (subsystem init)
                            └── rest_init() → kernel_init (PID 1)
```

---

### Summary of Phases

| Phase | What Happens | MMU State |
|-------|-------------|-----------|
| `primary_entry` | Save boot args, build idmap tables, cache maintenance | **OFF** |
| `init_kernel_el` | Configure EL1/EL2, set up exception levels | **OFF** |
| `__cpu_setup` | Set TCR/MAIR, prepare CPU for MMU | **OFF** |
| `__enable_mmu` | Install page tables, flip MMU on | **OFF → ON** |
| `__pi_early_map_kernel` | Map kernel virtual space, apply KASLR relocs | **ON** |
| `__primary_switched` | Set up task/stack/vectors, per-CPU data | **ON** |
| `start_kernel` | Full C kernel initialization | **ON** |You've used 63% of your weekly rate limit. Your weekly rate limit will reset on April 27 at 5:30 AM. [Learn More](https://aka.ms/github-copilot-rate-limit-error)

---

### Key Registers Used as Variables

| Register | Scope | Value |
|----------|-------|-------|
| `x19` | `primary_entry` → `start_kernel` | `0` = MMU was off, nonzero = MMU was on |
| `x20` | `primary_entry` → `__primary_switch` | CPU boot mode (`BOOT_CPU_MODE_EL1` or `EL2`) |
| `x21` | `primary_entry` → `start_kernel` | FDT pointer from bootloader `x0` |

---

### The Two Cache Paths Explained

The fork at `cbnz x19, 0f` handles a critical difference:

- **MMU OFF** (`x19 == 0`): Page tables were written without cache — stale speculative cache lines may exist. Must **invalidate** (`dcache_inval_poc`) the page table region before use.
- **MMU ON** (`x19 != 0`): Idmap code was cached. Before turning MMU off to switch tables, must **clean** (`dcache_clean_poc`) the idmap text to Point-of-Coherency so it's visible and safe to execute with MMU off.You've used 62% of your weekly rate limit. Your weekly rate limit will reset on April 27 at 5:30 AM. [Learn More](https://aka.ms/github-copilot-rate-limit-error)


## End-to-End Call Sequence — `primary_entry`

```
Bootloader
└── primary_entry()                          arch/arm64/kernel/head.S
    ├── record_mmu_state()                   arch/arm64/kernel/head.S
    │
    ├── preserve_boot_args()                 arch/arm64/kernel/head.S
    │   └── dcache_inval_poc()               arch/arm64/mm/cache.S  [if MMU off]
    │
    ├── __pi_create_init_idmap()             arch/arm64/mm/pi/map_range.c
    │
    ├── dcache_inval_poc()                   arch/arm64/mm/cache.S  [if MMU off]
    │   OR
    │   dcache_clean_poc()                   arch/arm64/mm/cache.S  [if MMU on]
    │
    ├── init_kernel_el()                     arch/arm64/kernel/head.S
    │   ├── init_el1()                       arch/arm64/kernel/head.S  [if EL1]
    │   │   └── eret  →  returns to caller
    │   └── init_el2()                       arch/arm64/kernel/head.S  [if EL2]
    │       └── eret  →  returns to caller
    │
    ├── __cpu_setup()                        arch/arm64/mm/proc.S
    │
    └── __primary_switch()                   arch/arm64/kernel/head.S
        ├── __enable_mmu()                   arch/arm64/kernel/head.S
        │   └── [MMU turned ON here]
        │
        ├── __pi_early_map_kernel()          arch/arm64/mm/pi/map_kernel.c
        │   ├── map_segment()
        │   ├── kaslr_early_init()           [if CONFIG_RANDOMIZE_BASE]
        │   └── relocate_kernel()
        │
        └── __primary_switched()             arch/arm64/kernel/head.S
            ├── init_cpu_task()              [macro — sets sp_el0, stack, SCS]
            ├── set_cpu_boot_mode_flag()     arch/arm64/kernel/head.S
            ├── kasan_early_init()           arch/arm64/mm/kasan_init.c  [if KASAN]
            ├── finalise_el2()              arch/arm64/kernel/hyp-stub.S
            └── start_kernel()              init/main.c
                ├── setup_arch()            arch/arm64/kernel/setup.c
                │   ├── setup_machine_fdt()
                │   ├── paging_init()       arch/arm64/mm/mmu.c
                │   │   ├── map_kernel()
                │   │   └── map_mem()
                │   └── unflatten_device_tree()
                ├── mm_init()
                │   ├── mem_init()
                │   └── kmem_cache_init()
                ├── sched_init()
                ├── init_IRQ()
                ├── time_init()
                ├── softirq_init()
                ├── console_init()
                └── rest_init()
                    ├── kernel_thread(kernel_init)   → PID 1
                    │   └── kernel_init()
                    │       └── run_init_process()   → /sbin/init (userspace)
                    └── kernel_thread(kthreadd)      → PID 2

