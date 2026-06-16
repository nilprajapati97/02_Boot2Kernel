# Architecture Comparison: PPC64 vs ARM32/ARM64 Per-CPU Offset Model

| Topic | PPC64 | ARM32 | ARM64 |
|---|---|---|---|
| Current-CPU offset source | `local_paca->data_offset` | TPIDRPRW | `tpidr_el1` or `tpidr_el2` |
| Mechanism type | Memory load via PACA (`r13`) | CP15 system register | AArch64 system register |
| Arch `setup_per_cpu_areas` | Yes (`setup_64.c`) | No (generic) | No (generic) |
| Generic allocator usage | Yes | Yes | Yes |
| Offset table | `__per_cpu_offset[]` | `__per_cpu_offset[]` | `__per_cpu_offset[]` |
| Arch extra wiring | `paca_ptrs[cpu]->data_offset` | `set_my_cpu_offset()` writes TPIDRPRW | `set_my_cpu_offset()` writes TPIDR |

## Interview One-Liner

PPC64 keeps Linux per-CPU offset in PACA memory, while ARM keeps it in hardware thread-pointer registers; both still rely on the same generic `mm/percpu.c` allocation core.
