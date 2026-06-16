# Master Call Flow Tree: `setup_per_cpu_areas()`

## Coverage: ARM32 and ARM64 (Both Use Generic Path)
## Source: Linux Kernel — mm/percpu.c, arch/arm/, arch/arm64/

---

## Complete Annotated Call Tree

```
start_kernel()                                    [init/main.c:854]
│
├─ setup_arch(&command_line)                      [arch/arm/kernel/setup.c or
│   Early arch setup; per-CPU not yet usable       arch/arm64/kernel/setup.c]
│
├─ setup_nr_cpu_ids()                             [kernel/smp.c]
│   Sets nr_cpu_ids from cpu_possible_mask
│
├─ setup_per_cpu_areas()  ◄══════════════════════ [mm/percpu.c:3383] ENTRY POINT
│   │
│   │   /* SMP generic path — ARM32 & ARM64 both use this */
│   │   /* Neither arch defines its own setup_per_cpu_areas() */
│   │
│   ├─ pcpu_embed_first_chunk(                    [mm/percpu.c:3075]
│   │      PERCPU_MODULE_RESERVE,   /* 8KB  */
│   │      PERCPU_DYNAMIC_RESERVE,  /* 20KB */
│   │      PAGE_SIZE,               /* atom_size */
│   │      NULL,                    /* cpu_distance_fn */
│   │      NULL)                    /* alloc_fn */
│   │   │
│   │   ├─ pcpu_build_alloc_info(               [mm/percpu.c:2864]
│   │   │      reserved_size,
│   │   │      dyn_size,
│   │   │      atom_size,
│   │   │      cpu_distance_fn)
│   │   │   │
│   │   │   ├─ Compute static_size               [mm/percpu.c:~2890]
│   │   │   │   = ALIGN(__per_cpu_end - __per_cpu_start, PCPU_MIN_ALLOC_SIZE)
│   │   │   │
│   │   │   ├─ Compute min_unit_size             [mm/percpu.c:~2900]
│   │   │   │   = max(static+reserved+dyn, PCPU_MIN_UNIT_SIZE=32KB)
│   │   │   │
│   │   │   ├─ Try best_upa (units per alloc)    [mm/percpu.c:~2920]
│   │   │   │   Choose upa for ≥75% fill ratio
│   │   │   │
│   │   │   ├─ for_each_possible_cpu()           [mm/percpu.c:~2950]
│   │   │   │   Group CPUs by cpu_distance_fn (LOCAL_DISTANCE)
│   │   │   │   Build pcpu_group_info per NUMA node
│   │   │   │
│   │   │   ├─ pcpu_alloc_info_alloc()           [mm/percpu.c:~2860]
│   │   │   │   Allocate struct pcpu_alloc_info from memblock
│   │   │   │
│   │   │   └─ Returns ai (pcpu_alloc_info*)
│   │   │
│   │   ├─ for each group i in ai->groups[]:      [mm/percpu.c:~3110]
│   │   │   │
│   │   │   └─ memblock_alloc_try_nid(           [mm/memblock.c]
│   │   │          ai->groups[i].alloc_size,
│   │   │          PAGE_SIZE,
│   │   │          __pa(MAX_DMA_ADDRESS),
│   │   │          MEMBLOCK_ALLOC_ACCESSIBLE,
│   │   │          cpu_to_node(group_cpu))
│   │   │       Allocates physically contiguous bootmem for this group
│   │   │       On NUMA: allocated from the group's local node
│   │   │
│   │   ├─ Find base = min(all group alloc addresses) [mm/percpu.c:~3130]
│   │   │   Compute group base_offsets relative to base
│   │   │
│   │   ├─ for each cpu in each group:            [mm/percpu.c:~3140]
│   │   │   │
│   │   │   └─ memcpy(unit_addr, __per_cpu_load,  [mm/percpu.c:~3145]
│   │   │              ai->static_size)
│   │   │       Copy .data..percpu template into this CPU's unit
│   │   │       Initializes static per-CPU variables to their initial values
│   │   │
│   │   ├─ Free tail padding pages                [mm/percpu.c:~3155]
│   │   │   memblock_free() any space past last real unit within alloc_size
│   │   │
│   │   ├─ pcpu_setup_first_chunk(ai, base)       [mm/percpu.c:2608]
│   │   │   │
│   │   │   ├─ Compute pcpu_unit_offsets[cpu]     [mm/percpu.c:~2650]
│   │   │   │   for each cpu: offset = group.base_offset + unit_index * unit_size
│   │   │   │
│   │   │   ├─ Set pcpu_base_addr = base          [mm/percpu.c:~2660]
│   │   │   │
│   │   │   ├─ Initialize pcpu_slot[] lists       [mm/percpu.c:~2700]
│   │   │   │   Array of list_heads indexed by free-size log2
│   │   │   │
│   │   │   ├─ pcpu_alloc_first_chunk(ai, base)   [mm/percpu.c:1345]
│   │   │   │   │
│   │   │   │   ├─ memblock_alloc() for           [mm/percpu.c:~1380]
│   │   │   │   │   struct pcpu_chunk
│   │   │   │   │   chunk->alloc_map
│   │   │   │   │   chunk->bound_map
│   │   │   │   │   chunk->md_blocks (pcpu_block_md array)
│   │   │   │   │
│   │   │   │   ├─ Set chunk->base_addr = base    [mm/percpu.c:~1420]
│   │   │   │   ├─ Set chunk->immutable = true    [mm/percpu.c:~1430]
│   │   │   │   └─ Initialize alloc_map bits      [mm/percpu.c:~1440]
│   │   │   │       Mark static+reserved as allocated (used)
│   │   │   │       Mark dynamic region as free
│   │   │   │
│   │   │   ├─ Set pcpu_reserved_chunk            [mm/percpu.c:~2750]
│   │   │   │   Points to region covering static+reserved
│   │   │   │
│   │   │   ├─ Set pcpu_first_chunk               [mm/percpu.c:~2760]
│   │   │   │   Points to region covering dynamic area
│   │   │   │
│   │   │   └─ pcpu_chunk_relocate(               [mm/percpu.c:555]
│   │   │          pcpu_first_chunk, -1)
│   │   │       Insert chunk into pcpu_slot[] at appropriate free-size slot
│   │   │
│   │   └─ pcpu_free_alloc_info(ai)               [mm/percpu.c:~3170]
│   │       memblock_free() the pcpu_alloc_info struct
│   │
│   ├─ delta = pcpu_base_addr - __per_cpu_start   [mm/percpu.c:~3400]
│   │
│   └─ for_each_possible_cpu(cpu):                [mm/percpu.c:~3402]
│       __per_cpu_offset[cpu] = delta + pcpu_unit_offsets[cpu]
│       ▲ TABLE IS NOW FULLY POPULATED ▲
│
├─ smp_prepare_boot_cpu()                         [arch/arm/kernel/smp.c:500]
│   │                                              [arch/arm64/kernel/smp.c:456]
│   └─ set_my_cpu_offset(per_cpu_offset(smp_processor_id()))
│       │   ARM32: mcr p15, 0, Rn, c13, c0, 4    [arch/arm/include/asm/percpu.h:17]
│       │   ARM64: msr tpidr_el1, Xn              [arch/arm64/include/asm/percpu.h:15]
│       │          (or tpidr_el2 if VHE active)
│       │
│       └─ ▲ CPU0 per-CPU access now works ▲
│
└─ [rest of start_kernel() ...]
    Later: smp_init() → cpu_up() → secondary_start_kernel() per secondary CPU
           Each secondary calls set_my_cpu_offset(__per_cpu_offset[N])
```

---

## Secondary CPU Bring-Up (ARM32)

```
secondary_start_kernel()                          [arch/arm/kernel/smp.c:410]
│
├─ cpu = smp_processor_id()
├─ set_my_cpu_offset(__per_cpu_offset[cpu])       [arch/arm/include/asm/percpu.h:17]
│   mcr p15, 0, Rn, c13, c0, 4  (write TPIDRPRW)
│   ▲ this CPU's per-CPU access now works ▲
│
├─ notify_cpu_starting(cpu)
├─ calibrate_delay()
└─ cpu_startup_entry(CPUHP_AP_ONLINE_IDLE)
```

---

## Secondary CPU Bring-Up (ARM64)

```
secondary_start_kernel()                          [arch/arm64/kernel/smp.c:203]
│
├─ cpu = smp_processor_id()
├─ set_my_cpu_offset(__per_cpu_offset[cpu])       [arch/arm64/include/asm/percpu.h:15]
│   ALTERNATIVE("msr tpidr_el1, Xn",
│               "msr tpidr_el2, Xn",
│               ARM64_HAS_VIRT_HOST_EXTN)
│   ▲ this CPU's per-CPU access now works ▲
│
├─ notify_cpu_starting(cpu)
├─ calibrate_delay()
└─ cpu_startup_entry(CPUHP_AP_ONLINE_IDLE)
```

---

## Runtime Per-CPU Access Flow

```
this_cpu_read(var)
│
├─ Expands to: raw_cpu_read(var)    [include/linux/percpu-defs.h:250]
│              (no preempt disable since we're reading current-CPU data)
│
├─ Expands to: arch_raw_cpu_ptr(&var)
│              (arch-specific implementation)
│
ARM32 path:
├─ __my_cpu_offset                  [arch/arm/include/asm/percpu.h:27]
│   asm("mrc p15, 0, %0, c13, c0, 4")  ← 1 instruction
│
└─ *((typeof(var) *)((unsigned long)&var + offset))
    ← compiler: add + load = 2 instructions
    = 3 total instructions

ARM64 path:
├─ __kern_my_cpu_offset()           [arch/arm64/include/asm/percpu.h:32]
│   ALTERNATIVE("mrs %0, tpidr_el1",
│               "mrs %0, tpidr_el2",
│               ARM64_HAS_VIRT_HOST_EXTN)  ← 1 instruction
│
└─ *((typeof(var) *)((unsigned long)&var + offset))
    ← compiler: add + load = 2 instructions
    = 3 total instructions
```

---

## Key Function Summary Table

| Function | File | Line | Purpose |
|---|---|---|---|
| `setup_per_cpu_areas()` | mm/percpu.c | 3383 | Top-level orchestrator |
| `pcpu_embed_first_chunk()` | mm/percpu.c | 3075 | Main allocator |
| `pcpu_build_alloc_info()` | mm/percpu.c | 2864 | Build NUMA groups |
| `memblock_alloc_try_nid()` | mm/memblock.c | — | Allocate bootmem per group |
| `pcpu_setup_first_chunk()` | mm/percpu.c | 2608 | Initialize chunk bookkeeping |
| `pcpu_alloc_first_chunk()` | mm/percpu.c | 1345 | Allocate chunk struct |
| `pcpu_chunk_relocate()` | mm/percpu.c | 555 | Place in free-size slot |
| `smp_prepare_boot_cpu()` (ARM32) | arch/arm/kernel/smp.c | 500 | Write TPIDRPRW for CPU0 |
| `smp_prepare_boot_cpu()` (ARM64) | arch/arm64/kernel/smp.c | 456 | Write tpidr_el1 for CPU0 |
| `set_my_cpu_offset()` (ARM32) | arch/arm/include/asm/percpu.h | 17 | MCR instruction |
| `set_my_cpu_offset()` (ARM64) | arch/arm64/include/asm/percpu.h | 15 | MSR instruction |
| `secondary_start_kernel()` (ARM32) | arch/arm/kernel/smp.c | 410 | Secondary CPU init |
| `secondary_start_kernel()` (ARM64) | arch/arm64/kernel/smp.c | 203 | Secondary CPU init |
