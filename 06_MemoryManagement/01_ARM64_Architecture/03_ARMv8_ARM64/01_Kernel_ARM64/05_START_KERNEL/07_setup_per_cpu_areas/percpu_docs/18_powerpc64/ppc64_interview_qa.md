# PowerPC64 Per-CPU Interview Q&A

## Q1: Does PPC64 use generic or arch-specific `setup_per_cpu_areas()`?
**Answer:**
PPC64 has an architecture-specific wrapper in `arch/powerpc/kernel/setup_64.c`, but it still
calls generic percpu allocators (`pcpu_embed_first_chunk`, fallback `pcpu_page_first_chunk`).

## Q2: Where is current CPU per-CPU offset stored on PPC64?
**Answer:**
In PACA: `local_paca->data_offset` (`arch/powerpc/include/asm/percpu.h`).

## Q3: Which register identifies current PACA?
**Answer:**
`r13` is reserved for `local_paca` on PPC64 kernel.

## Q4: What is PPC64-specific in allocator setup?
**Answer:**
`atom_size` selection depends on MMU mode:
- Book3E: `SZ_1M`
- Radix: `PAGE_SIZE`
- Hash MMU: `PAGE_SIZE` for 4K linear map, else `SZ_1M`

## Q5: Why write both `__per_cpu_offset[cpu]` and `paca_ptrs[cpu]->data_offset`?
**Answer:**
`__per_cpu_offset[]` is generic infrastructure; `paca->data_offset` is PPC64 fast-path source for
`__my_cpu_offset`. They must match to keep generic and arch access consistent.

## Q6: What happens if `pcpu_embed_first_chunk()` fails?
**Answer:**
PPC64 warns and falls back to `pcpu_page_first_chunk()`. If that also fails, kernel panics.

## Q7: Is the runtime variable access formula different on PPC64?
**Answer:**
No. Formula is identical:
`&var_cpuN = &var_template + offset`
Only the offset source differs (PACA vs TPIDR registers).

## Q8: How does this compare with ARM64 VHE handling?
**Answer:**
ARM64 may switch between `tpidr_el1` and `tpidr_el2` via `ALTERNATIVE()`. PPC64 has no TPIDR
switching equivalent; it always uses PACA (`r13` -> `data_offset`).
