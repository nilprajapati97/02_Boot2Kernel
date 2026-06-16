# PowerPC64 `setup_per_cpu_areas()` Deep Dive

## Source Reference
- `arch/powerpc/kernel/setup_64.c:838` — `void __init setup_per_cpu_areas(void)`
- `arch/powerpc/include/asm/percpu.h:12` — `#define __my_cpu_offset local_paca->data_offset`
- `arch/powerpc/include/asm/paca.h` — `struct paca_struct { ... u64 data_offset; ... }`

---

## Core Difference vs ARM32/ARM64

ARM32/ARM64 store current CPU percpu offset in a CPU system register (`TPIDRPRW`, `tpidr_el1/el2`).
PowerPC64 stores it in PACA:

```c
#define __my_cpu_offset local_paca->data_offset
```

So runtime per-CPU pointer calculation still uses the generic macros, but architecture reads
`__my_cpu_offset` from memory pointed by `r13` (`local_paca`) rather than an MSR/MRC instruction.

---

## Function Walkthrough

```c
void __init setup_per_cpu_areas(void)
{
    const size_t dyn_size = PERCPU_MODULE_RESERVE + PERCPU_DYNAMIC_RESERVE;
    size_t atom_size;
    unsigned long delta;
    unsigned int cpu;
    int rc = -EINVAL;

    if (IS_ENABLED(CONFIG_PPC_BOOK3E_64))
        atom_size = SZ_1M;
    else if (radix_enabled())
        atom_size = PAGE_SIZE;
    else if (IS_ENABLED(CONFIG_PPC_64S_HASH_MMU)) {
        if (mmu_linear_psize == MMU_PAGE_4K)
            atom_size = PAGE_SIZE;
        else
            atom_size = SZ_1M;
    }

    if (pcpu_chosen_fc != PCPU_FC_PAGE) {
        rc = pcpu_embed_first_chunk(0, dyn_size, atom_size,
                                    pcpu_cpu_distance, pcpu_cpu_to_node);
        if (rc)
            pr_warn(... fall back ...);
    }

    if (rc < 0)
        rc = pcpu_page_first_chunk(0, pcpu_cpu_to_node);
    if (rc < 0)
        panic("cannot initialize percpu area (err=%d)", rc);

    delta = (unsigned long)pcpu_base_addr - (unsigned long)__per_cpu_start;
    for_each_possible_cpu(cpu) {
        __per_cpu_offset[cpu] = delta + pcpu_unit_offsets[cpu];
        paca_ptrs[cpu]->data_offset = __per_cpu_offset[cpu];
    }
}
```

### What is PPC64-specific here?
- `atom_size` choice is tuned to PPC64 MMU mode (`Book3E`, radix, hash).
- `pcpu_embed_first_chunk()` and `pcpu_page_first_chunk()` are still generic allocators.
- Final per-CPU offset is copied into two places:
  - `__per_cpu_offset[cpu]` (generic array)
  - `paca_ptrs[cpu]->data_offset` (PPC64 fast path)

---

## Why `paca->data_offset` is written for all CPUs at boot

The boot CPU computes offsets for all possible CPUs once. Later, when a CPU becomes online,
its PACA already contains the right `data_offset` value so `__my_cpu_offset` works immediately
after PACA install and early SMP initialization.

This mirrors ARM behavior where each CPU eventually writes its own hardware offset register;
PPC64 just pre-populates per-CPU metadata in PACA structures.

---

## Interview Notes

- PPC64 does have an architecture-specific `setup_per_cpu_areas()` implementation in `setup_64.c`.
- Allocation still depends on generic percpu core (`pcpu_embed_first_chunk`, `pcpu_page_first_chunk`).
- Architecture-specific part is MMU-aware `atom_size` selection and PACA offset wiring.
- Runtime macro chain uses `local_paca->data_offset`, not a hardware TPIDR register.
