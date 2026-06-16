# PPC64 PACA Offset Wiring

## Source
- `arch/powerpc/kernel/setup_64.c:884-889`
- `arch/powerpc/include/asm/percpu.h`
- `arch/powerpc/include/asm/paca.h`

---

## Final Wiring Loop

```c
delta = (unsigned long)pcpu_base_addr - (unsigned long)__per_cpu_start;
for_each_possible_cpu(cpu) {
    __per_cpu_offset[cpu] = delta + pcpu_unit_offsets[cpu];
    paca_ptrs[cpu]->data_offset = __per_cpu_offset[cpu];
}
```

### Why both assignments are required
- `__per_cpu_offset[]`: canonical generic offset array.
- `paca->data_offset`: PPC64 architecture fast path used by `__my_cpu_offset`.

If they diverged, generic and arch macro expansion would resolve different addresses.

---

## Runtime Access Chain (PPC64)

```text
this_cpu_read(var)
  -> raw_cpu_read(var)
  -> arch_raw_cpu_ptr(&var)
  -> SHIFT_PERCPU_PTR(&var, __my_cpu_offset)
  -> __my_cpu_offset == local_paca->data_offset
```

PPC64 therefore avoids TPIDR-style system register reads and relies on PACA pointer (`r13`).
