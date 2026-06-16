# PPC64 Master Call Flow: `setup_per_cpu_areas()`

## Source
- `arch/powerpc/kernel/setup_64.c:838`
- `mm/percpu.c` generic allocator internals

---

## End-to-End Tree

```text
start_kernel()
  -> setup_arch()
  -> setup_nr_cpu_ids()
  -> setup_per_cpu_areas()                    [arch/powerpc/kernel/setup_64.c]
       -> choose atom_size by MMU mode
          - Book3E: SZ_1M
          - radix: PAGE_SIZE
          - hash: PAGE_SIZE (4K linear psize) else SZ_1M
       -> if (pcpu_chosen_fc != PCPU_FC_PAGE)
            rc = pcpu_embed_first_chunk(0, dyn_size, atom_size,
                                        pcpu_cpu_distance,
                                        pcpu_cpu_to_node)
            if (rc) warn and fall back
       -> if (rc < 0)
            rc = pcpu_page_first_chunk(0, pcpu_cpu_to_node)
       -> if (rc < 0)
            panic(cannot initialize percpu)
       -> delta = pcpu_base_addr - __per_cpu_start
       -> for_each_possible_cpu(cpu)
            __per_cpu_offset[cpu] = delta + pcpu_unit_offsets[cpu]
            paca_ptrs[cpu]->data_offset = __per_cpu_offset[cpu]
```

---

## Key Outputs of PPC64 Setup

1. Generic offset table is initialized: `__per_cpu_offset[]`.
2. PPC64 PACA fast path is initialized: `paca->data_offset` for every possible CPU.
3. Runtime per-CPU access becomes valid through `__my_cpu_offset`.
