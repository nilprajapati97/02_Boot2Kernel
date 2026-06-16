# PowerPC64 PACA and Per-CPU Access Path

## Source Reference
- `arch/powerpc/include/asm/paca.h` — `register struct paca_struct *local_paca asm("r13");`
- `arch/powerpc/include/asm/paca.h` — `u64 data_offset;` in `struct paca_struct`
- `arch/powerpc/include/asm/percpu.h` — `__my_cpu_offset local_paca->data_offset`

---

## PACA Basics

PACA = Processor Area. PPC64 keeps core-local kernel state in `struct paca_struct`.
A dedicated register points to the current CPU PACA:

```c
register struct paca_struct *local_paca asm("r13");
```

So every CPU has:
- Different `r13` value
- Different `local_paca`
- Different `local_paca->data_offset`

---

## Per-CPU Offset Source on PPC64

```c
#define __my_cpu_offset local_paca->data_offset
```

Generic percpu macros (`this_cpu_ptr`, `per_cpu_ptr`, `SHIFT_PERCPU_PTR`) then use this
same offset model as other architectures.

Address math remains:

```text
&var_for_current_cpu = &var_template + __my_cpu_offset
```

On PPC64, `__my_cpu_offset` resolves to a memory load from PACA instead of a system register read.

---

## Boot Sequence Dependency

Early setup includes:
- PACA creation and install (`setup_paca(...)`)
- Temporary `data_offset = 0` before percpu setup
- Later, `setup_per_cpu_areas()` computes and writes real offsets to every PACA

This ensures per-CPU accesses are valid once SMP and scheduler paths start using them.

---

## ARM vs PPC64 mental model

- ARM32/ARM64: offset lives in a dedicated hardware thread-ID register.
- PPC64: offset lives in memory (`paca->data_offset`) reached via dedicated register `r13`.
- Both feed the same generic per-CPU macro stack and delta formula.
